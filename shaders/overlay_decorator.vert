#version 440

layout(location = 0) in vec3 inPos;

layout(std140, binding = 0) uniform decoBuf {
    mat4 mvp;
    vec4 color;
    // Mesh-LOCAL clipping plane; see kUbufClipPlaneOffset. All zero disables clipping.
    vec4 clipPlane;
} ub;

void main()
{
    gl_ClipDistance[0] = dot(vec4(inPos, 1.0), ub.clipPlane);
    gl_Position = ub.mvp * vec4(inPos, 1.0);
    gl_PointSize = 6.0;
}
