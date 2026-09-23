#version 440

layout(location = 0) in vec3 inP0;
layout(location = 1) in vec3 inP1;
layout(location = 2) in float inAlong;
layout(location = 3) in float inSide;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    vec4 params; // width(px), 1/viewport width, 1/viewport height, dashed flag
    // Mesh-LOCAL clipping plane; see kUbufClipPlaneOffset. All zero disables clipping.
    vec4 clipPlane;
} ub;

layout(location = 0) out float vSide;
layout(location = 1) out float vAlongPx;

void main()
{
    // The point on the segment this quad corner belongs to, matching baseClip below.
    gl_ClipDistance[0] =
        dot(vec4(mix(inP0, inP1, clamp(inAlong, 0.0, 1.0)), 1.0), ub.clipPlane);
    vec4 clip0 = ub.mvp * vec4(inP0, 1.0);
    vec4 clip1 = ub.mvp * vec4(inP1, 1.0);

    float w0 = max(abs(clip0.w), 1e-6);
    float w1 = max(abs(clip1.w), 1e-6);
    vec2 ndc0 = clip0.xy / w0;
    vec2 ndc1 = clip1.xy / w1;

    vec2 dir = ndc1 - ndc0;
    float len = length(dir);
    vec2 normal = (len > 1e-6) ? vec2(-dir.y, dir.x) / len : vec2(0.0, 1.0);

    vec4 baseClip = mix(clip0, clip1, clamp(inAlong, 0.0, 1.0));

    float halfWidthPx = max(0.25, ub.params.x * 0.5);
    vec2 ndcPerPixel = vec2(2.0 * ub.params.y, 2.0 * ub.params.z);
    vec2 ndcOffset = normal * inSide * halfWidthPx * ndcPerPixel;
    baseClip.xy += ndcOffset * baseClip.w;

    gl_Position = baseClip;
    vSide = inSide;
    vAlongPx = inAlong * length((ndc1 - ndc0) * 0.5 / ub.params.yz);
}
