#version 440

layout(std140, binding = 0) uniform decoBuf {
    mat4 mvp;
    vec4 color;
    // Mesh-LOCAL clipping plane; see kUbufClipPlaneOffset. All zero disables clipping.
    vec4 clipPlane;
} ub;

layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = ub.color;
}
