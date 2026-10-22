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

layout(location = 0) in vec3 vBarycentric;
layout(location = 1) in vec3 vViewPos;
layout(location = 0) out vec4 fragColor;

// Unit vector from a point towards the eye. Rays leave a single eye in a perspective view,
// and run parallel in an orthographic one, where it is the same everywhere: (0, 0, 1) in
// view space. The main uniform block's lightDir.w says which.
vec3 towardsEye(vec3 viewPos)
{
    float len = length(viewPos);
    return (ub.lightDir.w > 0.5 || len < 1e-6) ? vec3(0.0, 0.0, 1.0) : -viewPos / len;
}

void main()
{
    float edgeDistance = min(vBarycentric.x, min(vBarycentric.y, vBarycentric.z));
    float edgeWidth = max(fwidth(edgeDistance) * ub.wireParams.x, 0.001);
    float wire = 1.0 - smoothstep(0.0, edgeWidth, edgeDistance);

    if (wire < 0.05)
        discard;

    vec3 color = ub.wireColor.rgb;
    if (ub.lightingParams.z > 0.5) {
        vec3 N = normalize(cross(dFdx(vViewPos), dFdy(vViewPos)));
        vec3 lightDir = towardsEye(vViewPos);
        float diff = clamp(abs(dot(N, lightDir)), 0.0, 1.0);
        float ambient = 0.25;
        color *= ambient + (1.0 - ambient) * diff;
    }

    fragColor = vec4(color, wire * ub.wireColor.a);
}
