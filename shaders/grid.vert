#version 440

layout(location = 0) in vec3 inPos;

layout(std140, binding = 0) uniform gridBuf {
    mat4 mvp;
    vec4 axisU;    // xyz = the world axis the grid's first coordinate runs along
    vec4 axisV;    // xyz = the second
    vec4 center;   // xyz = the middle of the quad, on the plane; w = the radius it fades out by
    vec4 levels;   // x = finest line spacing, y = its weight, z = the coarsest one's weight
    vec4 colorU;   // rgb = colour of the line along the first axis (where the second coordinate is 0)
    vec4 colorV;   // rgb = colour of the line along the second axis
    vec4 strength; // x, y, z = how strongly the fine, middle and coarse lines are drawn; w = axis width in pixels
    vec4 lineColor; // rgb = colour of the rules
} ub;

layout(location = 0) out vec3 vWorld;

void main()
{
    vWorld = inPos;
    gl_Position = ub.mvp * vec4(inPos, 1.0);
}
