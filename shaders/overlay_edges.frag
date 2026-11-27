#version 440

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vQualityT;
layout(location = 0) out vec4 fragColor;

// The scalar color map; a negative coordinate means vColor is the color.
layout(binding = 2) uniform sampler2D qualityLutTex;

vec4 edgeColorFor(vec4 color, float qualityT)
{
    return qualityT < 0.0 ? color : vec4(texture(qualityLutTex, vec2(qualityT, 0.5)).rgb, color.a);
}

void main()
{
    fragColor = edgeColorFor(vColor, vQualityT);
}
