// ── Radiance scaling (Vergne et al., I3D 2010), the option of the plain material ──────────────
// The curvature comes from a buffer of the view-space normals and log depth of what is on screen
// (fill_radscale_buf.frag), bound where the normal map goes, which plain does not use. Ports of
// 02_rs.fs. Shared by fill_smooth.frag, fill_flat.frag and overlay_points.frag, which declare the
// uniform block `ub` and the sampler `normalTex` (binding 3) before including it.

// silhouetteWeight(): 1.0 = smooth interior, 0.0 = depth discontinuity/edge.
float rsSilhouetteWeight(float s) {
    const float ts = 0.07;
    const float t2 = 0.9 + ts;
    const float t1 = t2 - 0.01;
    return smoothstep(t1, t2, max(1.0 - s, 0.9));
}

// Scaled curvature signal in [-1, 1].
float rsTanh(float c, float en) {
    const float tanhmax = 3.11622;
    return clamp(tanh((c * en * 15.0) / tanhmax), -1.0, 1.0);
}

// The scaling function of the paper (eq. 2) with its invariant point alpha = 0.1 and magnitude 1:
// maps the lighting value s in [0, 1] to its scaled value, unchanged where s = alpha, brighter on
// convexities and darker on concavities above it, the other way round below.
float rsWarp(float s, float beta) {
    const float alpha = 0.1;
    float eb  = exp(-beta);
    float num = alpha * eb + s * (1.0 - alpha - alpha * eb);
    float den = alpha + s * (eb - alpha - alpha * eb);
    return num / max(den, 1e-6);
}

// The curvature of the surface under this fragment, from the Hessian of the normals' gradient in
// a 3x3 neighbourhood of the buffer, in pixels (so a smaller object is crisper), scaled by the
// gain and faded to nothing at depth discontinuities.
float rsCurvature()
{
    // The buffer covers the whole render target and is addressed by absolute framebuffer position,
    // so the step is the target's inverse size, not the viewport's: they differ when the view
    // draws each layer in its own tile.
    float sw = ub.pointParams.z;
    float sh = ub.pointParams.w;
    float xc = gl_FragCoord.x * sw;
    float yc = gl_FragCoord.y * sh;

    // Layout:  A B C
    //          D X E
    //          F G H
    vec4 X = texture(normalTex, vec2(xc,      yc     ));
    vec4 A = texture(normalTex, vec2(xc - sw, yc + sh));
    vec4 B = texture(normalTex, vec2(xc,      yc + sh));
    vec4 C = texture(normalTex, vec2(xc + sw, yc + sh));
    vec4 D = texture(normalTex, vec2(xc - sw, yc     ));
    vec4 E = texture(normalTex, vec2(xc + sw, yc     ));
    vec4 F = texture(normalTex, vec2(xc - sw, yc - sh));
    vec4 G = texture(normalTex, vec2(xc,      yc - sh));
    vec4 H = texture(normalTex, vec2(xc + sw, yc - sh));

    float w = (rsSilhouetteWeight(abs(A.z - X.z)) + rsSilhouetteWeight(abs(B.z - X.z))
             + rsSilhouetteWeight(abs(C.z - X.z)) + rsSilhouetteWeight(abs(D.z - X.z))
             + rsSilhouetteWeight(abs(E.z - X.z)) + rsSilhouetteWeight(abs(F.z - X.z))
             + rsSilhouetteWeight(abs(G.z - X.z)) + rsSilhouetteWeight(abs(H.z - X.z))) / 8.0;

    // 2-pixel central differences. B is one pixel down in Y-down (Metal) and G one up, where the
    // original ran Y-up: hyy = gy_above - gy_below, so the subtraction is swapped.
    float hxx = E.x - D.x;
    float hyy = G.y - B.y;
    return rsTanh(-(hxx + hyy) / 2.0, ub.materialParams.x) * max(w - 0.5, 0.0);
}
