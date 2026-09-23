#version 440

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inMeshColor;
layout(location = 2) in vec4 inNormalAndFlag;

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

out gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    // Redeclaring the block means listing every built-in the shader writes, and this one
    // writes a clip distance now.
    float gl_ClipDistance[1];
};
layout(location = 0) out vec4 vMeshColor;
layout(location = 1) out vec4 vNormalAndFlag;
layout(location = 2) out vec3 vViewPos;

void main()
{
    gl_ClipDistance[0] = dot(vec4(inPos, 1.0), ub.clipPlane);
    vec4 viewPos = ub.modelView * vec4(inPos, 1.0);
    gl_Position = ub.mvp * vec4(inPos, 1.0);
    gl_PointSize = ub.pointParams.x;
    vMeshColor = inMeshColor;
    // Keep raw transformed normal; normalize safely in fragment stage.
    vNormalAndFlag = vec4(ub.normalMatrix * inNormalAndFlag.xyz, inNormalAndFlag.w);
    vViewPos = viewPos.xyz;
}
