#version 440
#extension GL_GOOGLE_include_directive : enable

layout(location = 0) in vec3 inPos;

layout(std140, binding = 0) uniform decoBuf {
    mat4 mvp;
    vec4 color;
    // Mesh-LOCAL clipping plane; see kUbufClipPlaneOffset. All zero disables clipping.
    vec4 clipPlane;
    // xy = 1 / the viewport size in pixels, for points drawn as quads.
    vec4 viewport;
} ub;

#ifdef POINT_QUADS
#include "point_quad.glsl"
#endif

// The size of the points this shader draws (decorator markers, selected vertices), in pixels.
const float kPointPixels = 6.0;

void main()
{
    gl_ClipDistance[0] = dot(vec4(inPos, 1.0), ub.clipPlane);
    gl_Position = ub.mvp * vec4(inPos, 1.0);
#ifdef POINT_QUADS
    gl_Position = pointQuadPosition(gl_Position, kPointPixels, ub.viewport.xy);
#else
    gl_PointSize = kPointPixels;
#endif
}
