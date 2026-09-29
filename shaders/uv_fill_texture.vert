#version 440

layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec4 meshColor;
layout(location = 3) in vec3 texInfo;

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
    vec4 pbrMapUsage;
    vec4 pbrParams;
};

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec4 v_meshColor;
layout(location = 2) out vec3 v_texInfo;
layout(location = 3) out vec3 v_viewPos;
// fill_smooth.frag, which this shares with the 3D view, reads the clipping-plane distance
// for its rim. The UV view has no clipping plane: a constant distance clips nothing, and
// with no gradient across it fwidth() is zero and no rim is drawn. Leaving it unwritten
// makes Metal refuse the whole pipeline.
layout(location = 4) out float v_clipDistance;

void main()
{
    vec3 uvPos = vec3(texInfo.xy, 0.0);
    vec4 viewPos = modelView * vec4(uvPos, 1.0);
    v_normal = vec3(0.0, 0.0, 1.0);
    v_meshColor = vec4(1.0, 1.0, 1.0, 0.0);
    v_texInfo = vec3(texInfo.xy, 1.0);
    v_viewPos = viewPos.xyz;
    v_clipDistance = 1.0;
    gl_Position = mvp * vec4(uvPos, 1.0);
}
