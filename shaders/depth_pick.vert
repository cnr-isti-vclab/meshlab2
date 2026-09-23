#version 440

layout(location = 0) in vec3 position;

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

layout(location = 0) flat out float vPickId;

out gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    // Redeclaring the block means listing every built-in the shader writes, and this one
    // writes a clip distance now.
    float gl_ClipDistance[1];
};

void main()
{
    gl_ClipDistance[0] = dot(vec4(position, 1.0), ub.clipPlane);
    gl_Position = ub.mvp * vec4(position, 1.0);
    gl_PointSize = max(1.0, ub.pointParams.x);
    // pointParams.y carries (meshIndex+1)/255 during id-picks (0 otherwise).
    vPickId = ub.pointParams.y;
}
