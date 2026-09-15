#version 440

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
