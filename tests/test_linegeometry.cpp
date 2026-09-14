#include <QtTest/QtTest>

#include "linerenderer.h"

#include <array>
#include <cmath>

// Bounding-box line geometry. Twenty-four bracket segments with a sign per axis and per
// corner is exactly the kind of thing that renders as a plausible picture while being
// wrong -- arms growing outward, or three arms at one corner and none at its opposite --
// so the shape is pinned here rather than eyeballed.
class LineGeometryTests : public QObject
{
    Q_OBJECT

private slots:
    void theBufferHoldsBothStylesBackToBack();
    void theFirstRangeIsTheTwelveEdgesOfTheBox();
    void everyCornerGrowsOneArmAlongEachAxis();
    void armsGrowInwardAndNeverLeaveTheBox();
    void armLengthIsTheRequestedShareOfEachSide();
    void anOversizedFractionCannotCrossTheBox();
    void aFlatBoxProducesNoInfinitiesOrInversions();
};

namespace {

using Vec = std::array<float, 3>;

const float kMin[3] = { -2.0f, 1.0f, -0.5f };
const float kMax[3] = { 6.0f, 3.0f, 9.5f };

std::vector<Vec> verticesOf(const std::vector<float> &flat)
{
    std::vector<Vec> out;
    for (std::size_t i = 0; i + 2 < flat.size(); i += 3)
        out.push_back(Vec { flat[i], flat[i + 1], flat[i + 2] });
    return out;
}

bool isCorner(const Vec &v, const float mn[3], const float mx[3])
{
    for (int axis = 0; axis < 3; ++axis) {
        if (!qFuzzyCompare(1.0f + v[axis], 1.0f + mn[axis])
            && !qFuzzyCompare(1.0f + v[axis], 1.0f + mx[axis])) {
            return false;
        }
    }
    return true;
}

// Index of the corner, one bit per axis, max side set.
int cornerIndex(const Vec &v, const float mx[3])
{
    int index = 0;
    for (int axis = 0; axis < 3; ++axis) {
        if (qFuzzyCompare(1.0f + v[axis], 1.0f + mx[axis]))
            index |= (1 << axis);
    }
    return index;
}

} // namespace

void LineGeometryTests::theBufferHoldsBothStylesBackToBack()
{
    const std::vector<float> flat =
        LineRenderer::buildBoundingBoxVertices(kMin, kMax, 0.14f);
    QCOMPARE(int(flat.size()), LineRenderer::kBoundingBoxVertexCount * 3);
    QCOMPARE(LineRenderer::kBoundingBoxEdgeVertexCount
                 + LineRenderer::kBoundingBoxBracketVertexCount,
             LineRenderer::kBoundingBoxVertexCount);
}

void LineGeometryTests::theFirstRangeIsTheTwelveEdgesOfTheBox()
{
    const auto v = verticesOf(LineRenderer::buildBoundingBoxVertices(kMin, kMax, 0.14f));

    std::array<int, 8> touches {};
    for (int i = 0; i < LineRenderer::kBoundingBoxEdgeVertexCount; i += 2) {
        QVERIFY(isCorner(v[std::size_t(i)], kMin, kMax));
        QVERIFY(isCorner(v[std::size_t(i + 1)], kMin, kMax));
        const int a = cornerIndex(v[std::size_t(i)], kMax);
        const int b = cornerIndex(v[std::size_t(i + 1)], kMax);
        // An edge of a box joins two corners differing on exactly one axis; a diagonal
        // across a face or through the body differs on two or three.
        const int differing = a ^ b;
        QVERIFY2(differing && (differing & (differing - 1)) == 0,
                 "box edge must join corners differing on one axis");
        ++touches[std::size_t(a)];
        ++touches[std::size_t(b)];
    }
    // Every corner of a box has degree three.
    for (int n : touches)
        QCOMPARE(n, 3);
}

void LineGeometryTests::everyCornerGrowsOneArmAlongEachAxis()
{
    const auto v = verticesOf(LineRenderer::buildBoundingBoxVertices(kMin, kMax, 0.14f));

    std::array<std::array<int, 3>, 8> armsPerCornerAxis {};
    for (int i = LineRenderer::kBoundingBoxEdgeVertexCount;
         i < LineRenderer::kBoundingBoxVertexCount;
         i += 2) {
        const Vec &base = v[std::size_t(i)];
        const Vec &tip = v[std::size_t(i + 1)];
        QVERIFY2(isCorner(base, kMin, kMax), "an arm must start at a corner");

        int movingAxis = -1;
        int movedCount = 0;
        for (int axis = 0; axis < 3; ++axis) {
            if (!qFuzzyCompare(1.0f + base[axis], 1.0f + tip[axis])) {
                movingAxis = axis;
                ++movedCount;
            }
        }
        QCOMPARE(movedCount, 1); // axis-aligned
        ++armsPerCornerAxis[std::size_t(cornerIndex(base, kMax))][std::size_t(movingAxis)];
    }

    for (const auto &perAxis : armsPerCornerAxis) {
        for (int n : perAxis)
            QCOMPARE(n, 1);
    }
}

// An arm pointing the wrong way leaves the box entirely and reads as a stray tick floating
// beside the corner -- the single most likely way to get this geometry wrong.
void LineGeometryTests::armsGrowInwardAndNeverLeaveTheBox()
{
    const auto v = verticesOf(LineRenderer::buildBoundingBoxVertices(kMin, kMax, 0.2f));

    for (int i = LineRenderer::kBoundingBoxEdgeVertexCount;
         i < LineRenderer::kBoundingBoxVertexCount;
         i += 2) {
        const Vec &tip = v[std::size_t(i + 1)];
        for (int axis = 0; axis < 3; ++axis) {
            QVERIFY2(tip[axis] >= kMin[axis] - 1e-4f && tip[axis] <= kMax[axis] + 1e-4f,
                     "arm tip left the box");
        }
    }
}

void LineGeometryTests::armLengthIsTheRequestedShareOfEachSide()
{
    constexpr float kFraction = 0.25f;
    const auto v = verticesOf(LineRenderer::buildBoundingBoxVertices(kMin, kMax, kFraction));

    for (int i = LineRenderer::kBoundingBoxEdgeVertexCount;
         i < LineRenderer::kBoundingBoxVertexCount;
         i += 2) {
        const Vec &base = v[std::size_t(i)];
        const Vec &tip = v[std::size_t(i + 1)];
        for (int axis = 0; axis < 3; ++axis) {
            const float moved = std::abs(tip[axis] - base[axis]);
            if (moved <= 1e-6f)
                continue;
            // Each axis is scaled by its own side, so the brackets stay in proportion on a
            // box that is far longer in one direction than another.
            const float expected = (kMax[axis] - kMin[axis]) * kFraction;
            QVERIFY2(std::abs(moved - expected) < 1e-4f,
                     qPrintable(QStringLiteral("axis %1: %2 vs %3")
                                    .arg(axis).arg(double(moved)).arg(double(expected))));
        }
    }
}

void LineGeometryTests::anOversizedFractionCannotCrossTheBox()
{
    // At a half the arms from opposite corners meet in the middle; beyond it they would
    // overlap and the "open" box would read as a solid one.
    const auto v = verticesOf(LineRenderer::buildBoundingBoxVertices(kMin, kMax, 4.0f));

    for (int i = LineRenderer::kBoundingBoxEdgeVertexCount;
         i < LineRenderer::kBoundingBoxVertexCount;
         i += 2) {
        const Vec &base = v[std::size_t(i)];
        const Vec &tip = v[std::size_t(i + 1)];
        for (int axis = 0; axis < 3; ++axis) {
            const float moved = std::abs(tip[axis] - base[axis]);
            const float half = 0.5f * (kMax[axis] - kMin[axis]);
            QVERIFY2(moved <= half + 1e-4f, "arm grew past the middle of its side");
        }
    }
}

void LineGeometryTests::aFlatBoxProducesNoInfinitiesOrInversions()
{
    // A planar mesh has no extent on one axis. Its arms there are zero-length, which draws
    // nothing; what must not happen is a NaN or an arm pointing out of the plane.
    const float flatMin[3] = { 0.0f, 5.0f, 0.0f };
    const float flatMax[3] = { 4.0f, 5.0f, 4.0f };
    const auto v = verticesOf(LineRenderer::buildBoundingBoxVertices(flatMin, flatMax, 0.14f));

    QCOMPARE(int(v.size()), LineRenderer::kBoundingBoxVertexCount);
    for (const Vec &p : v) {
        for (int axis = 0; axis < 3; ++axis)
            QVERIFY(std::isfinite(p[axis]));
        QVERIFY(qFuzzyCompare(1.0f + p[1], 1.0f + 5.0f));
    }
}

QTEST_APPLESS_MAIN(LineGeometryTests)
#include "test_linegeometry.moc"
