#version 440

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inMeshColor;
layout(location = 2) in vec4 inNormalAndFlag;

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

out gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    // Redeclaring the block means listing every built-in the shader writes, and this one
    // writes a clip distance now.
    float gl_ClipDistance[1];
};
layout(location = 0) out vec4 vMeshColor;
layout(location = 1) out vec4 vNormalAndFlag;
layout(location = 2) out vec3 vViewPos;

// pointParams.x is a diameter in pixels when positive, and minus a diameter in the mesh's
// own units when negative. The latter is measured by projecting an offset of that length
// along the view's vertical, carried back into mesh space: the result is the dot's height
// on screen whether the camera is perspective or orthographic. Held between 1 pixel and
// kMaxObjectPointPixels, so a point near the eye does not fill the view.
// Kept identical in overlay_points.vert and depth_pick.vert.
const float kMaxObjectPointPixels = 64.0;
float pointSizePixels(vec3 pos, vec4 clipPos)
{
    float size = ub.pointParams.x;
    if (size >= 0.0)
        return size;
    vec3 up = vec3(ub.modelView[0][1], ub.modelView[1][1], ub.modelView[2][1]);
    float upLen = length(up);
    if (upLen < 1e-12 || clipPos.w <= 1e-9)
        return 1.0;
    vec4 offsetClip = ub.mvp * vec4(pos + (up / upLen) * (-size), 1.0);
    if (offsetClip.w <= 1e-9)
        return kMaxObjectPointPixels;
    float ndcHeight = abs(offsetClip.y / offsetClip.w - clipPos.y / clipPos.w);
    // wireParams.w is 1 / the viewport height in pixels, and NDC spans 2 across it.
    float pixels = ndcHeight * 0.5 / max(ub.wireParams.w, 1e-9);
    return clamp(pixels, 1.0, kMaxObjectPointPixels);
}

void main()
{
    gl_ClipDistance[0] = dot(vec4(inPos, 1.0), ub.clipPlane);
    vec4 viewPos = ub.modelView * vec4(inPos, 1.0);
    gl_Position = ub.mvp * vec4(inPos, 1.0);
    gl_PointSize = pointSizePixels(inPos, gl_Position);
    vMeshColor = inMeshColor;
    // Keep raw transformed normal; normalize safely in fragment stage.
    vNormalAndFlag = vec4(ub.normalMatrix * inNormalAndFlag.xyz, inNormalAndFlag.w);
    vViewPos = viewPos.xyz;
}
