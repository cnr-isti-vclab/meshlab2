#include <QtTest/QtTest>

#include "viewfrustumgizmo.h"

#include <cmath>

// The wireframe one 3D view draws to show where another view's camera is. Its whole job is
// to be believed: if the near rectangle is not on the near plane, someone reads the picture
// and concludes their object is not being clipped when it is. An earlier version drew the
// body out to 12% of the scene diagonal and put no rectangle at the real distances at all,
// which is what these tests exist to stop coming back.
class ViewFrustumGizmoTests : public QObject
{
    Q_OBJECT

private slots:
    void theBodyRunsFromTheEyeToTheFarPlane();
    void theRectanglesSitAtTheDistancesTheViewActuallyClipsAt();
    void theRectanglesAreSizedByTheFieldOfViewInTheProjection();
    void theRectangleAspectFollowsTheProjection();
    void theGizmoFollowsTheCameraPose();
    void boundsEncloseEveryPointDrawn();
    void aDegenerateClipRangeDrawsNothing();
    void aProjectionThatIsNotAFinitePerspectiveDrawsNothing();

    void theFarDistanceCoversTheWholeGizmo();
    void theFarDistanceIsZeroForABoxBehindTheEye();
};

namespace {

constexpr float kFovY = 60.0f;
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kNear = 0.25f;
constexpr float kFar = 40.0f;

// tan(30 degrees): half the vertical extent per unit of distance at a 60-degree fovY.
const float kTanHalfFovY = std::tan(float(M_PI) / 6.0f);

QMatrix4x4 perspective(float fovY = kFovY, float aspect = kAspect)
{
    QMatrix4x4 p;
    p.perspective(fovY, aspect, kNear, kFar);
    return p;
}

// Segment layout, in the order buildViewFrustumGizmo emits it.
constexpr int kBodySegments = 4;   // 0..3   eye to the four far corners
constexpr int kNearRectFirst = 4;  // 4..7
constexpr int kFarRectFirst = 8;   // 8..11
constexpr int kSegmentCount = 12;

QVector3D vertexAt(const ViewFrustumGizmo &g, int segment, int endpoint)
{
    const int base = segment * 6 + endpoint * 3;
    return QVector3D(g.vertices[base], g.vertices[base + 1], g.vertices[base + 2]);
}

std::vector<QVector3D> everyPoint(const ViewFrustumGizmo &g)
{
    std::vector<QVector3D> points;
    for (int s = 0; s < g.segmentCount; ++s) {
        points.push_back(vertexAt(g, s, 0));
        points.push_back(vertexAt(g, s, 1));
    }
    return points;
}

// The four corners a rectangle's segments visit, in no particular order.
std::vector<QVector3D> rectangleCorners(const ViewFrustumGizmo &g, int firstSegment)
{
    std::vector<QVector3D> corners;
    for (int s = firstSegment; s < firstSegment + 4; ++s)
        corners.push_back(vertexAt(g, s, 0));
    return corners;
}

bool closeTo(const QVector3D &a, const QVector3D &b, float eps = 1.0e-4f)
{
    return (a - b).length() <= eps;
}

bool containsPoint(const std::vector<QVector3D> &points, const QVector3D &p, float eps = 1.0e-4f)
{
    for (const QVector3D &candidate : points) {
        if (closeTo(candidate, p, eps))
            return true;
    }
    return false;
}

} // namespace

void ViewFrustumGizmoTests::theBodyRunsFromTheEyeToTheFarPlane()
{
    const ViewFrustumGizmo g = buildViewFrustumGizmo(QMatrix4x4(), perspective(), kNear, kFar);

    QVERIFY(g.valid);
    QCOMPARE(g.segmentCount, kSegmentCount);
    QCOMPARE(int(g.vertices.size()), kSegmentCount * 6);

    // An identity view puts the eye at the origin looking down -Z.
    const QVector3D eye(0.0f, 0.0f, 0.0f);
    for (int s = 0; s < kBodySegments; ++s) {
        QVERIFY2(closeTo(vertexAt(g, s, 0), eye), "each body edge starts at the eye");
        // and ends on the far plane, not at some invented reference depth.
        QCOMPARE(vertexAt(g, s, 1).z(), -kFar);
    }
}

void ViewFrustumGizmoTests::theRectanglesSitAtTheDistancesTheViewActuallyClipsAt()
{
    const ViewFrustumGizmo g = buildViewFrustumGizmo(QMatrix4x4(), perspective(), kNear, kFar);
    QVERIFY(g.valid);

    for (const QVector3D &corner : rectangleCorners(g, kNearRectFirst))
        QCOMPARE(corner.z(), -kNear);
    for (const QVector3D &corner : rectangleCorners(g, kFarRectFirst))
        QCOMPARE(corner.z(), -kFar);

    // Each rectangle is closed: four segments visiting four distinct corners, every corner
    // both the start of one segment and the end of another.
    for (int first : { kNearRectFirst, kFarRectFirst }) {
        const std::vector<QVector3D> corners = rectangleCorners(g, first);
        for (int s = first; s < first + 4; ++s)
            QVERIFY(containsPoint(corners, vertexAt(g, s, 1)));
        for (int a = 0; a < 4; ++a) {
            for (int b = a + 1; b < 4; ++b)
                QVERIFY2(!closeTo(corners[a], corners[b]), "rectangle corners are distinct");
        }
    }
}

void ViewFrustumGizmoTests::theRectanglesAreSizedByTheFieldOfViewInTheProjection()
{
    const ViewFrustumGizmo g = buildViewFrustumGizmo(QMatrix4x4(), perspective(), kNear, kFar);
    QVERIFY(g.valid);

    for (const auto &rect : { std::make_pair(kNearRectFirst, kNear),
                              std::make_pair(kFarRectFirst, kFar) }) {
        const float halfH = kTanHalfFovY * rect.second;
        const float halfW = halfH * kAspect;
        const std::vector<QVector3D> corners = rectangleCorners(g, rect.first);
        for (float sx : { -1.0f, 1.0f }) {
            for (float sy : { -1.0f, 1.0f }) {
                const QVector3D expected(sx * halfW, sy * halfH, -rect.second);
                QVERIFY2(containsPoint(corners, expected, 1.0e-3f),
                         qPrintable(QStringLiteral("missing corner %1 %2 %3")
                                        .arg(expected.x()).arg(expected.y()).arg(expected.z())));
            }
        }
    }
}

void ViewFrustumGizmoTests::theRectangleAspectFollowsTheProjection()
{
    // A portrait projection must give a taller-than-wide rectangle. Reading the aspect out
    // of proj rather than the viewport is what keeps the two from disagreeing.
    const ViewFrustumGizmo portrait =
        buildViewFrustumGizmo(QMatrix4x4(), perspective(kFovY, 0.5f), kNear, kFar);
    QVERIFY(portrait.valid);

    float halfW = 0.0f;
    float halfH = 0.0f;
    for (const QVector3D &corner : rectangleCorners(portrait, kFarRectFirst)) {
        halfW = std::max(halfW, std::abs(corner.x()));
        halfH = std::max(halfH, std::abs(corner.y()));
    }
    QVERIFY(halfW < halfH);
    QVERIFY(std::abs(halfW / halfH - 0.5f) < 1.0e-3f);
}

void ViewFrustumGizmoTests::theGizmoFollowsTheCameraPose()
{
    // A camera at (10, 0, 0) looking back at the origin: the eye lands there and the body
    // runs along -X, the direction that camera is actually pointing.
    QMatrix4x4 view;
    view.lookAt(QVector3D(10.0f, 0.0f, 0.0f), QVector3D(0.0f, 0.0f, 0.0f),
                QVector3D(0.0f, 1.0f, 0.0f));

    const ViewFrustumGizmo g = buildViewFrustumGizmo(view, perspective(), kNear, kFar);
    QVERIFY(g.valid);

    const QVector3D eye(10.0f, 0.0f, 0.0f);
    for (int s = 0; s < kBodySegments; ++s)
        QVERIFY(closeTo(vertexAt(g, s, 0), eye, 1.0e-3f));

    // The near rectangle's centre is kNear along -X from the eye.
    QVector3D centre;
    for (const QVector3D &corner : rectangleCorners(g, kNearRectFirst))
        centre += corner;
    centre /= 4.0f;
    QVERIFY(closeTo(centre, QVector3D(10.0f - kNear, 0.0f, 0.0f), 1.0e-3f));
}

void ViewFrustumGizmoTests::boundsEncloseEveryPointDrawn()
{
    // The host view sizes its far plane from these bounds, so a point outside them is a
    // point that gets clipped away.
    QMatrix4x4 view;
    view.lookAt(QVector3D(3.0f, -4.0f, 5.0f), QVector3D(1.0f, 1.0f, 1.0f),
                QVector3D(0.0f, 1.0f, 0.0f));
    const ViewFrustumGizmo g = buildViewFrustumGizmo(view, perspective(), kNear, kFar);
    QVERIFY(g.valid);

    const std::vector<QVector3D> points = everyPoint(g);
    for (const QVector3D &p : points) {
        for (int axis = 0; axis < 3; ++axis) {
            QVERIFY(p[axis] >= g.boundsMin[axis] - 1.0e-4f);
            QVERIFY(p[axis] <= g.boundsMax[axis] + 1.0e-4f);
        }
    }

    // And they are tight: every bound is touched by some point.
    for (int axis = 0; axis < 3; ++axis) {
        float lo = std::numeric_limits<float>::max();
        float hi = std::numeric_limits<float>::lowest();
        for (const QVector3D &p : points) {
            lo = std::min(lo, p[axis]);
            hi = std::max(hi, p[axis]);
        }
        QVERIFY(std::abs(lo - g.boundsMin[axis]) < 1.0e-4f);
        QVERIFY(std::abs(hi - g.boundsMax[axis]) < 1.0e-4f);
    }
}

void ViewFrustumGizmoTests::aDegenerateClipRangeDrawsNothing()
{
    const QMatrix4x4 proj = perspective();
    for (const auto &range : { std::make_pair(0.0f, 10.0f),
                               std::make_pair(-1.0f, 10.0f),
                               std::make_pair(10.0f, 10.0f),
                               std::make_pair(10.0f, 1.0f) }) {
        const ViewFrustumGizmo g =
            buildViewFrustumGizmo(QMatrix4x4(), proj, range.first, range.second);
        QVERIFY2(!g.valid, qPrintable(QStringLiteral("near %1 far %2 should draw nothing")
                                          .arg(range.first).arg(range.second)));
        QCOMPARE(g.segmentCount, 0);
        QVERIFY(g.vertices.empty());
    }
}

void ViewFrustumGizmoTests::aProjectionThatIsNotAFinitePerspectiveDrawsNothing()
{
    QMatrix4x4 zero;
    zero.fill(0.0f);
    QVERIFY(!buildViewFrustumGizmo(QMatrix4x4(), zero, kNear, kFar).valid);

    QMatrix4x4 flipped = perspective();
    flipped(1, 1) = -flipped(1, 1);
    QVERIFY(!buildViewFrustumGizmo(QMatrix4x4(), flipped, kNear, kFar).valid);

    QMatrix4x4 notANumber = perspective();
    notANumber(0, 0) = std::numeric_limits<float>::quiet_NaN();
    QVERIFY(!buildViewFrustumGizmo(QMatrix4x4(), notANumber, kNear, kFar).valid);
}

void ViewFrustumGizmoTests::theFarDistanceCoversTheWholeGizmo()
{
    // The case from the bug report: a peer camera framing the scene from far away, seen by
    // a host view sitting close in. The host's own far plane -- a few scene radii -- ends
    // well short of the peer's far rectangle, so the gizmo was cut in half.
    QMatrix4x4 peerView;
    peerView.lookAt(QVector3D(0.0f, 0.0f, 120.0f), QVector3D(0.0f, 0.0f, 0.0f),
                    QVector3D(0.0f, 1.0f, 0.0f));
    const ViewFrustumGizmo g = buildViewFrustumGizmo(peerView, perspective(), 0.4f, 200.0f);
    QVERIFY(g.valid);

    QMatrix4x4 hostView;
    hostView.lookAt(QVector3D(5.0f, 0.0f, 0.0f), QVector3D(0.0f, 0.0f, 0.0f),
                    QVector3D(0.0f, 1.0f, 0.0f));
    const float farDist = farthestDistanceInView(hostView, g.boundsMin, g.boundsMax);

    // Every point the gizmo draws is inside that far plane.
    for (const QVector3D &p : everyPoint(g))
        QVERIFY2(-(hostView * p).z() <= farDist + 1.0e-3f, "a gizmo point beyond the far plane");

    // And it is a real widening, not a no-op: the peer's far rectangle is 80 units behind
    // the peer's own eye position, nowhere near the host's scene-sized far plane.
    QVERIFY(farDist > 80.0f);
}

void ViewFrustumGizmoTests::theFarDistanceIsZeroForABoxBehindTheEye()
{
    QMatrix4x4 hostView;
    hostView.lookAt(QVector3D(0.0f, 0.0f, 0.0f), QVector3D(0.0f, 0.0f, -1.0f),
                    QVector3D(0.0f, 1.0f, 0.0f));
    // A box entirely behind the viewer asks for no widening at all.
    QCOMPARE(farthestDistanceInView(hostView, QVector3D(-1.0f, -1.0f, 10.0f),
                                    QVector3D(1.0f, 1.0f, 20.0f)),
             0.0f);
}

QTEST_APPLESS_MAIN(ViewFrustumGizmoTests)
#include "test_viewfrustumgizmo.moc"
