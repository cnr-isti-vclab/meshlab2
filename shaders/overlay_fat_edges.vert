#version 440

layout(location = 0) in vec3 inP0;
layout(location = 1) in vec3 inP1;
layout(location = 2) in float inAlong;
layout(location = 3) in float inSide;
layout(location = 4) in vec4 inVertexColor;
layout(location = 5) in vec4 inEdgeColor;

layout(location = 6) in vec4 inQuality;

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
    // rgb = the colour of the band where the surface meets the clipping plane, a = its
    // width in pixels. Zero width leaves the band undrawn.
    vec4 clipRim;
    // A raw scalar q becomes the color map coordinate clamp(q * scale + offset): (scale,
    // offset) for per-vertex values in qualityMap.xy, for per-edge ones in edgeQualityMap.xy
    // (kUbufQualityMapOffset, kUbufEdgeQualityMapOffset).
    vec4 qualityMap;
    vec4 edgeQualityMap;
} ub;

layout(location = 0) out float vSide;
layout(location = 1) out vec4 vColor;
layout(location = 2) out float vQualityT;

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
    // lightingParams.x is the EdgeColorSource: 0 constant, 1 per-vertex color, 2 per-edge
    // color, 3 per-vertex scalar, 4 per-edge scalar. A scalar becomes a color map coordinate
    // here, interpolated along the edge, and is looked up per fragment; a value that is not
    // finite (inQuality.y or .w zero) is drawn in the constant color, as having no data.
    float source = ub.lightingParams.x;
    vColor = (source < 0.5 || source > 2.5)
        ? ub.edgeColor
        : (source < 1.5 ? inVertexColor : inEdgeColor);
    vQualityT = -1.0;
    if (source > 2.5) {
        bool perEdge = source > 3.5;
        vec2 q = perEdge ? inQuality.zw : inQuality.xy;
        vec2 map = perEdge ? ub.edgeQualityMap.xy : ub.qualityMap.xy;
        if (q.y > 0.5)
            vQualityT = clamp(q.x * map.x + map.y, 0.0, 1.0);
    }
}
