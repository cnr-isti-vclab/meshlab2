#version 440

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    vec4 color;
    vec4 params; // width(px), 1/viewport width, 1/viewport height, dashed flag
    // Mesh-LOCAL clipping plane; see kUbufClipPlaneOffset. All zero disables clipping.
    vec4 clipPlane;
} ub;

layout(location = 0) in float vSide;
layout(location = 1) in float vAlongPx;
layout(location = 0) out vec4 fragColor;

void main()
{
    // The hidden-line pass enables a 5 px on / 5 px off screen-space pattern.
    if (ub.params.w > 0.5 && mod(vAlongPx, 10.0) > 5.0)
        discard;
    float aa = max(fwidth(vSide), 1e-4);
    float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, abs(vSide));
    fragColor = vec4(ub.color.rgb, ub.color.a * alpha);
    if (fragColor.a <= 0.001)
        discard;
}
