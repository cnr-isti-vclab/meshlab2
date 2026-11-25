// Points as screen-aligned quads, for backends that cannot size a point primitive.
//
// Direct3D 11 and 12 rasterize every point as one pixel whatever gl_PointSize says (QRhi reports
// VertexShaderPointSize unsupported). The shaders that draw sized points are therefore also
// compiled with POINT_QUADS defined: the point buffer is then bound per instance and each point
// drawn as a 4-vertex triangle strip, its corner given by gl_VertexIndex. RenderWidget picks the
// variant (preference render.pointPrimitive); elsewhere native points are kept, being faster.

// The corner of this vertex, (0,0) (1,0) (0,1) (1,1) in strip order: also the coordinate across
// the dot that gl_PointCoord would give.
vec2 pointQuadCorner()
{
    return vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1));
}

// clipPos moved to this vertex's corner of a square sizePx pixels across, centred on the point.
// invViewport is 1 / the viewport size in pixels; NDC spans 2 across it.
vec4 pointQuadPosition(vec4 clipPos, float sizePx, vec2 invViewport)
{
    clipPos.xy += (pointQuadCorner() * 2.0 - 1.0) * sizePx * invViewport * clipPos.w;
    return clipPos;
}
