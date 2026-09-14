#pragma once

#include <vector>

namespace LineRenderer {

constexpr int kLineStrideFloats = 6;    // p0.xyz + p1.xyz
constexpr int kFatLineStrideFloats = 8; // p0.xyz + p1.xyz + along + side

void appendFatLineSegmentVertices(std::vector<float> &dst,
                                  float p0x,
                                  float p0y,
                                  float p0z,
                                  float p1x,
                                  float p1y,
                                  float p1z);

std::vector<float> buildFatLineVertices(const std::vector<float> &lineSegments);

// Bounding-box line geometry, in one buffer holding both the styles it can be drawn in:
// the twelve edges of the box first, then the twenty-four corner brackets. Keeping them in
// one buffer means switching style is a change of draw range rather than a rebuild, and the
// two can never disagree about the box they describe.
constexpr int kBoundingBoxEdgeVertexCount = 24;     // 12 segments
constexpr int kBoundingBoxBracketVertexCount = 48;  // 24 segments, three per corner
constexpr int kBoundingBoxVertexCount =
    kBoundingBoxEdgeVertexCount + kBoundingBoxBracketVertexCount;

// Share of each side taken by a bracket arm. Small enough that the box reads as open
// rather than merely dashed, large enough to locate the corner at a glance.
constexpr float kBoundingBoxBracketFraction = 0.14f;

// `bracketFraction` is the share of a side's length taken by the bracket arms growing along
// it. Each axis uses its own side, so the arms stay in proportion on an elongated box, and
// the value is clamped to a half so arms from opposite corners meet at worst in the middle
// rather than crossing. A side of zero length yields arms of zero length, which draw
// nothing -- the honest result for a flat box.
std::vector<float> buildBoundingBoxVertices(
    const float minCorner[3],
    const float maxCorner[3],
    float bracketFraction);

} // namespace LineRenderer

