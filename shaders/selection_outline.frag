#version 440

layout(std140, binding = 0) uniform outlineBuf {
    vec4 outlineColor;
    vec4 outlineParams; // x: widthPx, y: invWidth, z: invHeight, w: unused
    vec4 occludedOutlineColor;
} ub;

layout(binding = 1) uniform sampler2D outlineTex;
layout(binding = 2) uniform sampler2D currentTex;
layout(binding = 3) uniform sampler2D sceneTex;

layout(location = 0) out vec4 fragColor;

float decodePackedDepth24(vec3 enc)
{
    return clamp(dot(enc, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0)), 0.0, 1.0);
}

void main()
{
    vec2 invSize = vec2(ub.outlineParams.y, ub.outlineParams.z);
    // The masks were rendered by this same backend into targets laid out as this one is, so
    // the pixel under gl_FragCoord is at gl_FragCoord / size in them on every backend: OpenGL
    // counts both from the bottom row, Metal, Vulkan and D3D both from the top. No flip.
    vec2 uv = gl_FragCoord.xy * invSize;
    vec2 uvClamped = clamp(uv, vec2(0.0), vec2(1.0));

    vec2 d = invSize * max(1.0, ub.outlineParams.x);
    vec2 uv0 = clamp(uv + vec2( d.x, 0.0), vec2(0.0), vec2(1.0));
    vec2 uv1 = clamp(uv + vec2(-d.x, 0.0), vec2(0.0), vec2(1.0));
    vec2 uv2 = clamp(uv + vec2(0.0,  d.y), vec2(0.0), vec2(1.0));
    vec2 uv3 = clamp(uv + vec2(0.0, -d.y), vec2(0.0), vec2(1.0));
    vec2 uv4 = clamp(uv + vec2( d.x,  d.y), vec2(0.0), vec2(1.0));
    vec2 uv5 = clamp(uv + vec2(-d.x,  d.y), vec2(0.0), vec2(1.0));
    vec2 uv6 = clamp(uv + vec2( d.x, -d.y), vec2(0.0), vec2(1.0));
    vec2 uv7 = clamp(uv + vec2(-d.x, -d.y), vec2(0.0), vec2(1.0));

    // Point-cloud fallback keeps the old mask-based behavior.
    if (ub.occludedOutlineColor.a <= 0.0001) {
        float center = texture(currentTex, uvClamped).r;
        float n0 = texture(currentTex, uv0).r;
        float n1 = texture(currentTex, uv1).r;
        float n2 = texture(currentTex, uv2).r;
        float n3 = texture(currentTex, uv3).r;
        float n4 = texture(currentTex, uv4).r;
        float n5 = texture(currentTex, uv5).r;
        float n6 = texture(currentTex, uv6).r;
        float n7 = texture(currentTex, uv7).r;
        float dilated = max(max(max(n0, n1), max(n2, n3)), max(max(n4, n5), max(n6, n7)));
        float fullEdge = clamp(dilated - center, 0.0, 1.0);

        float vCenter = texture(sceneTex, uvClamped).r;
        float v0 = texture(sceneTex, uv0).r;
        float v1 = texture(sceneTex, uv1).r;
        float v2 = texture(sceneTex, uv2).r;
        float v3 = texture(sceneTex, uv3).r;
        float v4 = texture(sceneTex, uv4).r;
        float v5 = texture(sceneTex, uv5).r;
        float v6 = texture(sceneTex, uv6).r;
        float v7 = texture(sceneTex, uv7).r;
        float vDilated = max(
            max(max(vCenter, v0), max(v1, v2)),
            max(max(v3, v4), max(v5, max(v6, v7))));
        float visibleCoverage = vDilated;
        float vis = fullEdge * smoothstep(0.05, 0.2, visibleCoverage);
        if (vis <= 0.001)
            discard;
        fragColor = vec4(ub.outlineColor.rgb, ub.outlineColor.a * vis);
        return;
    }

    const int kMaxRadiusPx = 16;
    int radiusPx = int(floor(max(1.0, ub.outlineParams.x) + 0.5));
    radiusPx = clamp(radiusPx, 1, kMaxRadiusPx);
    int radius2 = radiusPx * radiusPx;

    float bestCov = 0.0;
    bool bestVisible = true;
    // Relax depth equality to avoid self-occlusion acne on the same surface.
    const float epsilon = 4.0 / 65536.0;
    float denom = float(radius2 + 1);

    for (int oy = -kMaxRadiusPx; oy <= kMaxRadiusPx; ++oy) {
        if (abs(oy) > radiusPx)
            continue;
        for (int ox = -kMaxRadiusPx; ox <= kMaxRadiusPx; ++ox) {
            if (abs(ox) > radiusPx)
                continue;
            int dist2 = ox * ox + oy * oy;
            if (dist2 > radius2)
                continue;

            vec2 duv = vec2(float(ox) * invSize.x, float(oy) * invSize.y);
            vec2 suv = clamp(uv + duv, vec2(0.0), vec2(1.0));
            float edge = texture(outlineTex, suv).r;
            if (edge <= 0.001)
                continue;

            float w = max(0.0, 1.0 - float(dist2) / denom) * edge;
            vec4 currentDepthPacked = texture(currentTex, suv);
            if (currentDepthPacked.a <= 0.001)
                continue;

            vec4 sceneDepthPacked = texture(sceneTex, suv);
            bool visible = true;
            if (sceneDepthPacked.a > 0.001) {
                float currentDepth = decodePackedDepth24(currentDepthPacked.rgb);
                float sceneDepth = decodePackedDepth24(sceneDepthPacked.rgb);
                visible = (currentDepth <= sceneDepth + epsilon);
            }

            if (w > bestCov) {
                bestCov = w;
                bestVisible = visible;
            }
        }
    }

    if (bestCov <= 0.001)
        discard;

    vec4 outColor = bestVisible ? ub.outlineColor : ub.occludedOutlineColor;
    if (outColor.a <= 0.001)
        discard;
    fragColor = vec4(outColor.rgb, outColor.a * bestCov);
}
