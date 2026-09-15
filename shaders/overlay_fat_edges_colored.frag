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
} ub;

layout(location = 0) in float vSide;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 fragColor;

void main()
{
    float aa = max(fwidth(vSide), 1e-4);
    float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, abs(vSide));
    fragColor = vec4(vColor.rgb, vColor.a * alpha);
    if (fragColor.a <= 0.001)
        discard;
}
