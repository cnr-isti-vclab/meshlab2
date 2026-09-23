#version 440

layout(location = 0) in vec3 inP0;
layout(location = 1) in vec3 inP1;
layout(location = 2) in float inAlong;
layout(location = 3) in float inSide;
layout(location = 4) in vec4 inVertexColor;
layout(location = 5) in vec4 inEdgeColor;

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
    // Mesh-LOCAL clipping plane: a vertex survives when dot(vec4(pos,1), clipPlane) >= 0.
    // All zero disables clipping. Written at kUbufClipPlaneOffset.
    vec4 clipPlane;
} ub;

layout(location = 0) out float vSide;
layout(location = 1) out vec4 vColor;

void main()
{
    gl_ClipDistance[0] = dot(vec4(mix(inP0, inP1, clamp(inAlong, 0.0, 1.0)), 1.0), ub.clipPlane);
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

    float halfWidthPx = max(0.5, ub.wireParams.y * 0.5);
    vec2 ndcPerPixel = vec2(2.0 * ub.wireParams.z, 2.0 * ub.wireParams.w);
    vec2 ndcOffset = normal * inSide * halfWidthPx * ndcPerPixel;
    baseClip.xy += ndcOffset * baseClip.w;

    gl_Position = baseClip;
    vSide = inSide;
    float source = ub.lightingParams.x;
    vColor = source < 0.5
        ? ub.edgeColor
        : (source < 1.5 ? inVertexColor : inEdgeColor);
}
