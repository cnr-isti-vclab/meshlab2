#version 440

layout(std140, binding = 0) uniform gridBuf {
    mat4 mvp;
    vec4 axisU;
    vec4 axisV;
    vec4 center;
    vec4 levels;
    vec4 colorU;
    vec4 colorV;
    vec4 strength;
    vec4 lineColor;
} ub;

layout(location = 0) in vec3 vWorld;
layout(location = 0) out vec4 fragColor;

// Coverage of the lines of a grid of cells `spacing` apart, one pixel wide whatever the view:
// the distance to the nearest line, in pixels, from the screen-space derivative of the cell
// coordinate. `density` is how many cells fall in a pixel, which says when the cells have
// become too small to tell apart and the level should have faded out.
float gridLines(vec2 p, float spacing, out float density)
{
    vec2 cell = p / spacing;
    vec2 d = max(fwidth(cell), vec2(1e-9));
    vec2 g = abs(fract(cell - 0.5) - 0.5) / d;
    density = max(d.x, d.y);
    return 1.0 - min(min(g.x, g.y), 1.0);
}

// A level's contribution: its lines, dimmed as its cells shrink towards the pixel size, so that
// a plane seen at a grazing angle goes quietly to nothing in the distance instead of to noise.
float gridLevel(vec2 p, float spacing, float weight)
{
    float density;
    float lines = gridLines(p, spacing, density);
    return lines * weight * (1.0 - smoothstep(0.2, 0.5, density));
}

// The line where `coordinate` is zero, `width` pixels across.
float axisLine(float coordinate, float width)
{
    float pixels = abs(coordinate) / max(fwidth(coordinate), 1e-9);
    return 1.0 - clamp(pixels - (0.5 * width - 0.5), 0.0, 1.0);
}

void main()
{
    vec2 p = vec2(dot(vWorld, ub.axisU.xyz), dot(vWorld, ub.axisV.xyz));

    float fine = gridLevel(p, ub.levels.x, ub.levels.y);
    float mid = gridLevel(p, ub.levels.x * 10.0, 1.0);
    float coarse = gridLevel(p, ub.levels.x * 100.0, ub.levels.z);
    // How much each shows is a preference; by default the coarser the line, the more it shows.
    float grid = max(max(fine * ub.strength.x, mid * ub.strength.y), coarse * ub.strength.z);

    float alongU = axisLine(p.y, ub.strength.w);   // the line the first axis makes
    float alongV = axisLine(p.x, ub.strength.w);
    float axis = max(alongU, alongV);

    // Out to the edge of the quad the grid fades, so that no edge is seen.
    float r = length(vWorld - ub.center.xyz);
    float fade = 1.0 - smoothstep(0.35 * ub.center.w, ub.center.w, r);

    vec3 gridColor = ub.lineColor.rgb;
    vec3 axisColor = alongU >= alongV ? ub.colorU.rgb : ub.colorV.rgb;
    float axisAlpha = axis * 0.9;
    float alpha = max(grid, axisAlpha);
    vec3 color = axisAlpha >= grid ? axisColor : gridColor;
    // The strengths are preferences and may exceed one; a line cannot be more opaque than opaque.
    fragColor = vec4(color, min(alpha * fade, 1.0));
}
