#version 440

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inVertexColor;
layout(location = 2) in vec4 inEdgeColor;

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
} ub;

layout(location = 0) out vec4 vColor;

void main()
{
    gl_ClipDistance[0] = dot(vec4(inPos, 1.0), ub.clipPlane);
    gl_Position = ub.mvp * vec4(inPos, 1.0);
    float source = ub.lightingParams.x;
    vColor = source < 0.5
        ? ub.edgeColor
        : (source < 1.5 ? inVertexColor : inEdgeColor);
}
