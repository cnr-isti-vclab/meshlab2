#version 440

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 meshColor;
layout(location = 3) in vec3 texInfo;

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
};

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_meshColor;
layout(location = 2) out vec3 v_texInfo;
layout(location = 3) out vec3 v_viewPos;

void main()
{
    gl_ClipDistance[0] = dot(vec4(position, 1.0), clipPlane);
    vec4 viewPos = modelView * vec4(position, 1.0);
    v_normal = normalize(normalMatrix * normal);
    v_meshColor = meshColor;
    v_texInfo = texInfo;
    v_viewPos = viewPos.xyz;
    gl_Position = mvp * vec4(position, 1.0);
}
