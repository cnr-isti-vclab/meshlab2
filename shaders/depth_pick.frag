#version 440

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

layout(location = 0) out vec4 fragColor;

vec3 packDepth24(float depth)
{
    vec3 enc = fract(depth * vec3(1.0, 255.0, 65025.0));
    enc -= enc.yzz * vec3(1.0 / 255.0, 1.0 / 255.0, 0.0);
    return enc;
}

void main()
{
    // RGB = packed depth (for world-point unprojection); A = picked mesh id
    // encoded as (meshIndex+1)/255, so 0 means background. The id is read here from the
    // uniform block rather than passed on by the vertex shader, so that this pairs with any
    // vertex shader declaring the main block -- the fat edges' one included, which writes other
    // outputs at the location an id would take (and OpenGL matches stage interfaces by name).
    fragColor = vec4(packDepth24(gl_FragCoord.z), ub.pointParams.y);
}
