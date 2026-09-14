#include "linerenderer.h"

#include <algorithm>

#include <array>

namespace LineRenderer {

namespace {
constexpr std::array<std::array<float, 2>, 6> kFatTriTemplate = {{
    {{ 0.0f, -1.0f }},
    {{ 0.0f, 1.0f }},
    {{ 1.0f, -1.0f }},
    {{ 1.0f, -1.0f }},
    {{ 0.0f, 1.0f }},
    {{ 1.0f, 1.0f }},
}};
} // namespace

void appendFatLineSegmentVertices(std::vector<float> &dst,
                                  float p0x,
                                  float p0y,
                                  float p0z,
                                  float p1x,
                                  float p1y,
                                  float p1z)
{
    for (const auto &tpl : kFatTriTemplate) {
        dst.push_back(p0x);
        dst.push_back(p0y);
        dst.push_back(p0z);
        dst.push_back(p1x);
        dst.push_back(p1y);
        dst.push_back(p1z);
        dst.push_back(tpl[0]); // along (0=start, 1=end)
        dst.push_back(tpl[1]); // side (-1/+1)
    }
}

std::vector<float> buildFatLineVertices(const std::vector<float> &lineSegments)
{
    const size_t segmentCount = lineSegments.size() / kLineStrideFloats;
    if (segmentCount == 0)
        return {};

    std::vector<float> fatData;
    fatData.reserve(segmentCount * 6 * kFatLineStrideFloats);

    for (size_t si = 0; si < segmentCount; ++si) {
        const float p0x = lineSegments[si * kLineStrideFloats + 0];
        const float p0y = lineSegments[si * kLineStrideFloats + 1];
        const float p0z = lineSegments[si * kLineStrideFloats + 2];
        const float p1x = lineSegments[si * kLineStrideFloats + 3];
        const float p1y = lineSegments[si * kLineStrideFloats + 4];
        const float p1z = lineSegments[si * kLineStrideFloats + 5];
        appendFatLineSegmentVertices(fatData, p0x, p0y, p0z, p1x, p1y, p1z);
    }

    return fatData;
}

std::vector<float> buildBoundingBoxVertices(
    const float minCorner[3],
    const float maxCorner[3],
    float bracketFraction)
{
    std::vector<float> vertices;
    vertices.reserve(std::size_t(kBoundingBoxVertexCount) * 3);

    const auto push = [&vertices](const float p[3]) {
        vertices.push_back(p[0]);
        vertices.push_back(p[1]);
        vertices.push_back(p[2]);
    };
    // Bit b of the corner index selects the max side on axis b, so corner ^ (1 << b) is the
    // neighbour across axis b and the twelve edges are the pairs differing in one bit.
    const auto cornerAt = [&](int corner, float out[3]) {
        for (int axis = 0; axis < 3; ++axis)
            out[axis] = (corner & (1 << axis)) ? maxCorner[axis] : minCorner[axis];
    };

    for (int corner = 0; corner < 8; ++corner) {
        for (int axis = 0; axis < 3; ++axis) {
            const int neighbour = corner ^ (1 << axis);
            if (neighbour < corner)
                continue; // emit each edge once, from its lower-indexed end
            float a[3];
            float b[3];
            cornerAt(corner, a);
            cornerAt(neighbour, b);
            push(a);
            push(b);
        }
    }

    const float fraction = std::clamp(bracketFraction, 0.0f, 0.5f);
    float arm[3];
    for (int axis = 0; axis < 3; ++axis)
        arm[axis] = (maxCorner[axis] - minCorner[axis]) * fraction;

    for (int corner = 0; corner < 8; ++corner) {
        float base[3];
        cornerAt(corner, base);
        for (int axis = 0; axis < 3; ++axis) {
            float tip[3] = { base[0], base[1], base[2] };
            // Inward: away from whichever face of this axis the corner sits on.
            tip[axis] += (corner & (1 << axis)) ? -arm[axis] : arm[axis];
            push(base);
            push(tip);
        }
    }

    return vertices;
}

} // namespace LineRenderer
