/****************************************************************************
* Radiance Scaling fill shader — pass 2 of 2                               *
*                                                                           *
* Faithful port of 02_rs.fs (Vergne & Dumas, INRIA 2010).                  *
* Reads the (gx, gy, logZ, 1.0) gradient texture written by pass 1         *
* (fill_radscale_buf.frag), samples the exact 3×3 neighbourhood, and       *
* computes weight() and hessian() as in the original implementation.       *
*                                                                           *
* The gradient texture is bound at slot 3 (the normalTex SRB position,     *
* which is unused in RS mode).                                              *
*                                                                           *
* UBO materialFlags (RS):  x=invert, y=displayMode (0/1/2)                 *
* UBO materialParams (RS): x=enhancement (0.0–1.0, default 0.5)            *
* UBO pointParams:         z=1/targetWidth,   w=1/targetHeight              *
****************************************************************************/
#version 440

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 modelView;
    mat3 normalMatrix;
    vec4 bboxColor;
    vec4 pointColor;
    vec4 pointParams;  // z = 1/targetWidth (sw), w = 1/targetHeight (sh)
    vec4 wireColor;
    vec4 wireParams;
    vec4 fillColor;
    vec4 lightingParams;
    vec4 edgeColor;
    vec4 materialFlags;  // x=invert, y=displayMode
    vec4 materialParams; // x=enhancement
    vec4 lightDir;        // view-space light direction (w unused)
} ub;

layout(location = 0) in vec3 v_normal;
layout(location = 1) in vec4 v_meshColor;
layout(location = 2) in vec3 v_texInfo;
layout(location = 3) in vec3 v_viewPos;    // unused in pass 2; kept for layout compat

layout(binding = 1) uniform sampler2D albedoTex;
layout(binding = 2) uniform sampler2D qualityLutTex;
layout(binding = 3) uniform sampler2D gradTex;      // (gx, gy, logZ, 1.0) from pass 1
layout(binding = 4) uniform sampler2D occlusionTex; // SRB compat, unused
layout(binding = 5) uniform sampler2D roughnessTex; // SRB compat, unused

layout(location = 0) out vec4 fragColor;

// ── Exact ports of 02_rs.fs helpers (Vergne & Dumas, INRIA 2010) ──────────

// silhouetteWeight(): 1.0 = smooth interior, 0.0 = depth discontinuity/edge.
// s = abs(depth_neighbour - depth_centre) in log-space.
float rsSilhouetteWeight(float s) {
    const float ts = 0.07;
    const float t2 = 0.9 + ts;   // 0.97
    const float t1 = t2 - 0.01;  // 0.96
    return smoothstep(t1, t2, max(1.0 - s, 0.9));
}

// tanh() from 02_rs.fs — scaled curvature signal in [-1, 1].
float rsTanh(float c, float en) {
    const float tanhmax = 3.11622;
    return clamp(tanh((c * en * 15.0) / tanhmax), -1.0, 1.0);
}

// warp() from 02_rs.fs — rational radiance scaling function.
float rsWarp(float s, float beta) {
    const float alpha = 0.1;
    float eb  = exp(-beta);
    float num = alpha * eb + s * (1.0 - alpha - alpha * eb);
    float den = alpha + s * (eb - alpha - alpha * eb);
    return num / max(den, 1e-6);
}

// coloredDescriptor() from 02_rs.fs.
vec3 rsColoredDescriptor(float c, float w) {
    const vec3 convMax = vec3(0.1, 0.1, 0.8);
    const vec3 convMin = vec3(0.2, 0.2, 0.6);
    const vec3 concMax = vec3(0.8, 0.1, 0.1);
    const vec3 concMin = vec3(0.6, 0.2, 0.2);
    const vec3 plane   = vec3(0.7, 0.7, 0.2);
    const float t = 0.02;
    vec3 rgb;
    float a;
    if (c < -t) {
        a = (-c - t) / (1.0 - t);  rgb = mix(concMin, concMax, a);
    } else if (c > t) {
        a = (c - t) / (1.0 - t);   rgb = mix(convMin, convMax, a);
    } else if (c < 0.0) {
        a = -c / t;                 rgb = mix(plane, concMin, a);
    } else {
        a = c / t;                  rgb = mix(plane, convMin, a);
    }
    if (w < 0.95)
        rgb = vec3(0.2);
    return rgb;
}

// greyDescriptor() from 02_rs.fs.
vec3 rsGreyDescriptor(float c, float w) {
    return vec3(clamp((c * 0.5 + 0.5) - (1.0 - w), 0.0, 1.0));
}

void main()
{
    // ── Base colour ───────────────────────────────────────────────────────────
    vec3 baseColor = ub.fillColor.rgb;
    if (v_meshColor.a < -0.5)
        baseColor = texture(qualityLutTex, vec2(clamp(v_meshColor.r, 0.0, 1.0), 0.5)).rgb;
    else
        baseColor = mix(ub.fillColor.rgb, v_meshColor.rgb, clamp(v_meshColor.a, 0.0, 1.0));
    if (v_texInfo.z > 0.5)
        baseColor = texture(albedoTex, vec2(v_texInfo.x, 1.0 - v_texInfo.y)).rgb;

    // RS parameters
    const bool  rsInvert      = ub.materialFlags.x > 0.5;
    const int   rsDisplayMode = int(ub.materialFlags.y + 0.5);
    const float enhancement   = max(ub.materialParams.x, 0.001);

    // ── Radiance Scaling ──────────────────────────────────────────────────────
    vec3 color = baseColor;
    if (ub.lightingParams.w > 0.5) {

        // Pixel-step sizes in the gradient texture (= sw, sh from 02_rs.fs). The gradient
        // covers the whole render target and is addressed by absolute framebuffer position,
        // so this is the target's inverse size, not the viewport's: the two differ when the
        // view draws each layer in its own tile.
        float sw = ub.pointParams.z;  // 1 / targetWidth
        float sh = ub.pointParams.w;  // 1 / targetHeight

        // Screen-space UV of this fragment.
        float xc = gl_FragCoord.x * sw;
        float yc = gl_FragCoord.y * sh;

        // 3×3 neighbourhood — exact port of loadValues() from 02_rs.fs.
        // Layout:  A B C
        //          D X E
        //          F G H
        vec4 X = texture(gradTex, vec2(xc,      yc     ));
        vec4 A = texture(gradTex, vec2(xc - sw, yc + sh));
        vec4 B = texture(gradTex, vec2(xc,      yc + sh));
        vec4 C = texture(gradTex, vec2(xc + sw, yc + sh));
        vec4 D = texture(gradTex, vec2(xc - sw, yc     ));
        vec4 E = texture(gradTex, vec2(xc + sw, yc     ));
        vec4 F = texture(gradTex, vec2(xc - sw, yc - sh));
        vec4 G = texture(gradTex, vec2(xc,      yc - sh));
        vec4 H = texture(gradTex, vec2(xc + sw, yc - sh));

        // weight() — exact port from 02_rs.fs.
        // .z channel of grad texture = logZ (depth discontinuity proxy).
        float w = (rsSilhouetteWeight(abs(A.z - X.z)) +
                   rsSilhouetteWeight(abs(B.z - X.z)) +
                   rsSilhouetteWeight(abs(C.z - X.z)) +
                   rsSilhouetteWeight(abs(D.z - X.z)) +
                   rsSilhouetteWeight(abs(E.z - X.z)) +
                   rsSilhouetteWeight(abs(F.z - X.z)) +
                   rsSilhouetteWeight(abs(G.z - X.z)) +
                   rsSilhouetteWeight(abs(H.z - X.z))) / 8.0;

        // hessian() — exact port from 02_rs.fs (2-pixel central differences).
        // Note: B is at yc+sh (one pixel DOWN in Y-down/Metal convention) and
        //       G is at yc-sh (one pixel UP). The original ran on OpenGL Y-up
        //       where B was above and G below, giving hyy = gy_above - gy_below.
        //       We swap the subtraction here to preserve that semantic.
        float hxx = E.x - D.x;   // gx(x+1) - gx(x-1)  — X is same in both conventions
        float hyy = G.y - B.y;   // gy(y-1) - gy(y+1) = gy_above - gy_below (corrected)

        // curvature() — exact port from 02_rs.fs.
        // The /2.0 is correct: hxx and hyy are 2-pixel central diffs.
        float c = rsTanh(-(hxx + hyy) / 2.0, enhancement) * max(w - 0.5, 0.0);
        if (rsInvert)
            c = -c;

        // Lighting + display mode.
        vec3 N = normalize(v_normal);
        if (rsDisplayMode == 1) {
            color = rsColoredDescriptor(c, w);
        } else if (rsDisplayMode == 2) {
            color = rsGreyDescriptor(c, w);
        } else {
            // Lambertian RS (display mode 0): baseColor * cosine * warp(cosine, c)
            float cosineTerm = max(dot(N, normalize(ub.lightDir.xyz)), 0.0);
            color = baseColor * cosineTerm * rsWarp(cosineTerm, c);
        }
    }

    fragColor = vec4(color, 1.0);
}
