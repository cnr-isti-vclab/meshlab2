#version 440

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 meshColor;
layout(location = 2) in vec3 texInfo;

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

layout(location = 0) out vec3 vViewPos;
layout(location = 1) out vec4 v_meshColor;
layout(location = 2) out vec3 v_texInfo;
// The clipping plane's own distance, so the fragment stage can draw the band
// where this surface runs into it.
layout(location = 3) out float v_clipDistance;

void main()
{
    gl_ClipDistance[0] = dot(vec4(inPos, 1.0), ub.clipPlane);
    v_clipDistance = gl_ClipDistance[0];
    vec4 vp = ub.modelView * vec4(inPos, 1.0);
    vViewPos = vp.xyz;
    v_meshColor = meshColor;
    v_texInfo = texInfo;
    gl_Position = ub.mvp * vec4(inPos, 1.0);
}
