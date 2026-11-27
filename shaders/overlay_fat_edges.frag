#version 440

layout(location = 0) in float vSide;
layout(location = 1) in vec4 vColor;
layout(location = 2) in float vQualityT;
layout(location = 0) out vec4 fragColor;

// The scalar color map; a negative coordinate means vColor is the color.
layout(binding = 2) uniform sampler2D qualityLutTex;

vec4 edgeColorFor(vec4 color, float qualityT)
{
    return qualityT < 0.0 ? color : vec4(texture(qualityLutTex, vec2(qualityT, 0.5)).rgb, color.a);
}

void main()
{
    float aa = max(fwidth(vSide), 1e-4);
    float alpha = 1.0 - smoothstep(1.0 - aa, 1.0, abs(vSide));
    vec4 color = edgeColorFor(vColor, vQualityT);
    fragColor = vec4(color.rgb, color.a * alpha);
    if (fragColor.a <= 0.001)
        discard;
}
