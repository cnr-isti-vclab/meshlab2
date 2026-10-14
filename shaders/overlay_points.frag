#version 440
#extension GL_GOOGLE_include_directive : enable

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 modelView;
    mat3 normalMatrix;
    vec4 bboxColor;
    vec4 pointColor;
    vec4 pointParams;
    vec4 wireColor;
    vec4 wireParams;
    vec4 fillColor;
    vec4 lightingParams;
    vec4 edgeColor;
    vec4 materialFlags;
    vec4 materialParams;
    vec4 lightDir;
} ub;

layout(location = 0) in vec4 vMeshColor;
layout(location = 1) in vec4 vNormalAndFlag;
layout(location = 2) in vec3 vViewPos;
layout(binding = 2) uniform sampler2D qualityLutTex;
// Where the fill shaders keep their normal map; points have none, so with radiance scaling on it
// holds the gradient buffer (points_radscale_buf.frag).
layout(binding = 3) uniform sampler2D normalTex;
layout(location = 0) out vec4 fragColor;

// Unit vector from a point towards the eye. Rays leave a single eye in a perspective view,
// and run parallel in an orthographic one, where it is the same everywhere: (0, 0, 1) in
// view space. The main uniform block's lightDir.w says which.
vec3 towardsEye(vec3 viewPos)
{
    float len = length(viewPos);
    return (ub.lightDir.w > 0.5 || len < 1e-6) ? vec3(0.0, 0.0, 1.0) : -viewPos / len;
}

#include "radiance_scaling.glsl"

void main()
{
    // Discard corners to produce round dots
    vec2 coord = gl_PointCoord - vec2(0.5);
    if (dot(coord, coord) > 0.25)
        discard;

    vec3 color = ub.pointColor.rgb;
    if (vMeshColor.a < -0.5)
        color = texture(qualityLutTex, vec2(clamp(vMeshColor.r, 0.0, 1.0), 0.5)).rgb;
    else
        color = mix(ub.pointColor.rgb, vMeshColor.rgb, clamp(vMeshColor.a, 0.0, 1.0));
    // The lighting scale, 1 where nothing darkens the point; radiance scaling then bends it
    // by the curvature the point's neighbourhood has in the gradient buffer. Both need a normal.
    bool hasNormal = vNormalAndFlag.a > 0.5;
    float shade = 1.0;
    if (ub.lightingParams.y > 0.5 && hasNormal) {
        vec3 N = vNormalAndFlag.xyz;
        float nLen = length(N);
        if (nLen > 1e-6)
            N /= nLen;
        else
            N = vec3(0.0, 0.0, 1.0);
        vec3 lightDir = towardsEye(vViewPos);
        float diff = clamp(abs(dot(N, lightDir)), 0.0, 1.0);
        // Keep a strong ambient term so sparse/rough normals do not appear "culled".
        float ambient = 0.75;
        shade = ambient + (1.0 - ambient) * diff;
    }
    if (ub.materialParams.w > 1.5 && hasNormal)
        shade *= rsWarp(shade, rsCurvature());
    color *= shade;
    fragColor = vec4(color, ub.pointColor.a);
}
