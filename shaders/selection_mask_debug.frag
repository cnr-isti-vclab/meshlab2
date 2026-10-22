#version 440

layout(std140, binding = 0) uniform debugBuf {
    vec4 params; // x: invWidth, y: invHeight, z: unused, w: mode
} ub;

layout(binding = 1) uniform sampler2D srcTex;
layout(binding = 2) uniform sampler2D auxTex;

layout(location = 0) out vec4 fragColor;

float decodePackedDepth24(vec3 enc)
{
    return clamp(dot(enc, vec3(1.0, 1.0 / 255.0, 1.0 / 65025.0)), 0.0, 1.0);
}

void main()
{
    // The masks were rendered by this same backend into targets laid out as this one is, so
    // the pixel under gl_FragCoord is at gl_FragCoord / size in them on every backend: OpenGL
    // counts both from the bottom row, Metal, Vulkan and D3D both from the top. No flip.
    vec2 uv = gl_FragCoord.xy * vec2(ub.params.x, ub.params.y);
    vec2 uvClamped = clamp(uv, vec2(0.0), vec2(1.0));
    vec4 src = texture(srcTex, uvClamped);
    vec4 aux = texture(auxTex, uvClamped);

    if (ub.params.w < 0.5) {
        float v = src.a > 0.001 ? decodePackedDepth24(src.rgb) : 0.0;
        fragColor = vec4(v, v, v, 1.0);
        return;
    }

    // mode 1: red where src depth is behind aux depth.
    float occ = 0.0;
    if (src.a > 0.001 && aux.a > 0.001) {
        const float eps = 1.0 / 65536.0;
        float srcDepth = decodePackedDepth24(src.rgb);
        float auxDepth = decodePackedDepth24(aux.rgb);
        occ = (srcDepth > auxDepth + eps) ? 1.0 : 0.0;
    }
    fragColor = vec4(occ, 0.0, 0.0, 1.0);
}
