#include <QtTest/QtTest>

#include "clipplane.h"

#include <cmath>

// The clipping plane's arithmetic. Whether the shaders honour it is checked through the
// real render path (see the headless probe in the design notes); what is pinned here is
// the part that decides WHERE the plane is, which is the part a user reads off the screen
// and believes.
class ClipPlaneTests : public QObject
{
    Q_OBJECT

private slots:
    void aDisabledPlaneKeepsEverything();
    void aDegenerateNormalKeepsEverything();
    void anEmptySceneKeepsEverything();
    void theAxisPresetsFaceTheRightWay();
    void theViewAxisFollowsTheCamera();
    void thePlanePassesThroughItsReferencePoint();
    void theOffsetIsAFractionOfTheSceneDiagonal();
    void flippingKeepsTheOtherSide();
    void flippingDoesNotMoveThePlane();
    void aLayerTransformMovesThePlaneWithIt();
    void aLocalPlaneOfNothingIsStillNothing();

    void theGizmoLiesOnThePlane();
    void theGizmoStemPointsAtWhatIsKept();
    void theGizmoIsEmptyWithoutAPlane();
};

namespace {

const QVector3D kUnitMin(-1.0f, -1.0f, -1.0f);
const QVector3D kUnitMax(1.0f, 1.0f, 1.0f);
// The diagonal of that box: |(2,2,2)|.
const float kUnitDiagonal = 2.0f * std::sqrt(3.0f);
const QVector3D kLookingDownZ(0.0f, 0.0f, -1.0f);

GlobalRenderSettings enabled(ClipPlaneAxis axis = ClipPlaneAxis::Z)
{
    GlobalRenderSettings s;
    s.clipPlaneEnabled = true;
    s.clipPlaneAxis = axis;
    s.clipPlaneRelativeTo = ClipPlaneReference::Center;
    return s;
}

// What the shader computes: a point survives when this is >= 0.
float distance(const QVector4D &plane, const QVector3D &p)
{
    return QVector3D::dotProduct(plane.toVector3D(), p) + plane.w();
}

bool closeTo(float a, float b, float eps = 1.0e-4f)
{
    return std::abs(a - b) <= eps;
}

} // namespace

void ClipPlaneTests::aDisabledPlaneKeepsEverything()
{
    GlobalRenderSettings s = enabled();
    s.clipPlaneEnabled = false;
    QCOMPARE(ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ), QVector4D());
}

void ClipPlaneTests::aDegenerateNormalKeepsEverything()
{
    GlobalRenderSettings s = enabled(ClipPlaneAxis::Custom);
    s.clipPlaneCustomAxis = QVector3D(0.0f, 0.0f, 0.0f);
    QCOMPARE(ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ), QVector4D());

    // `View` with no camera direction is the same situation.
    QCOMPARE(ClipPlane::world(enabled(ClipPlaneAxis::View), kUnitMin, kUnitMax, QVector3D()),
             QVector4D());
}

void ClipPlaneTests::anEmptySceneKeepsEverything()
{
    // The offset is measured in diagonals, so a scene with no extent has nothing to
    // measure against and cutting it would put the plane in an arbitrary place.
    const QVector3D point(3.0f, 3.0f, 3.0f);
    QCOMPARE(ClipPlane::world(enabled(), point, point, kLookingDownZ), QVector4D());
}

void ClipPlaneTests::theAxisPresetsFaceTheRightWay()
{
    const struct { ClipPlaneAxis axis; QVector3D normal; } cases[] = {
        { ClipPlaneAxis::X, QVector3D(1.0f, 0.0f, 0.0f) },
        { ClipPlaneAxis::Y, QVector3D(0.0f, 1.0f, 0.0f) },
        { ClipPlaneAxis::Z, QVector3D(0.0f, 0.0f, 1.0f) },
    };
    for (const auto &c : cases) {
        const QVector4D plane = ClipPlane::world(enabled(c.axis), kUnitMin, kUnitMax, kLookingDownZ);
        QVERIFY((plane.toVector3D() - c.normal).length() < 1.0e-5f);
    }

    GlobalRenderSettings custom = enabled(ClipPlaneAxis::Custom);
    custom.clipPlaneCustomAxis = QVector3D(0.0f, 5.0f, 0.0f);   // not unit length
    const QVector4D plane = ClipPlane::world(custom, kUnitMin, kUnitMax, kLookingDownZ);
    QVERIFY2(closeTo(plane.toVector3D().length(), 1.0f), "the normal is normalized");
    QVERIFY((plane.toVector3D() - QVector3D(0.0f, 1.0f, 0.0f)).length() < 1.0e-5f);
}

void ClipPlaneTests::theViewAxisFollowsTheCamera()
{
    const QVector3D direction = QVector3D(1.0f, -2.0f, 3.0f).normalized();
    const QVector4D plane =
        ClipPlane::world(enabled(ClipPlaneAxis::View), kUnitMin, kUnitMax, direction);
    QVERIFY((plane.toVector3D() - direction).length() < 1.0e-5f);

    // Facing the way the camera looks means the far side is kept -- which is what raising
    // the near plane used to do, and the behaviour this replaces.
    QVERIFY(distance(plane, QVector3D(0.0f, 0.0f, 0.0f) + direction) > 0.0f);
}

void ClipPlaneTests::thePlanePassesThroughItsReferencePoint()
{
    const QVector3D min(2.0f, 0.0f, 0.0f);
    const QVector3D max(6.0f, 4.0f, 4.0f);
    const struct { ClipPlaneReference reference; QVector3D point; } cases[] = {
        { ClipPlaneReference::Origin, QVector3D(0.0f, 0.0f, 0.0f) },
        { ClipPlaneReference::Center, QVector3D(4.0f, 2.0f, 2.0f) },
        { ClipPlaneReference::Min,    min },
        { ClipPlaneReference::Max,    max },
    };
    for (const auto &c : cases) {
        GlobalRenderSettings s = enabled(ClipPlaneAxis::X);
        s.clipPlaneRelativeTo = c.reference;
        const QVector4D plane = ClipPlane::world(s, min, max, kLookingDownZ);
        QVERIFY2(closeTo(distance(plane, c.point), 0.0f),
                 "at zero offset the plane passes through its reference point");
    }
}

void ClipPlaneTests::theOffsetIsAFractionOfTheSceneDiagonal()
{
    GlobalRenderSettings s = enabled(ClipPlaneAxis::X);
    s.clipPlaneOffset = 0.25f;
    const QVector4D plane = ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ);

    // The centre is now behind the plane by a quarter of the diagonal.
    QVERIFY(closeTo(distance(plane, QVector3D(0.0f, 0.0f, 0.0f)), -0.25f * kUnitDiagonal));
    // And the plane itself sits that far along its normal from the centre.
    QVERIFY(closeTo(distance(plane, QVector3D(0.25f * kUnitDiagonal, 0.0f, 0.0f)), 0.0f));

    // Twice the scene, same setting, twice the distance: the control means the same thing
    // whatever a model's units are.
    const QVector4D bigger = ClipPlane::world(s, 2.0f * kUnitMin, 2.0f * kUnitMax, kLookingDownZ);
    QVERIFY(closeTo(distance(bigger, QVector3D(0.0f, 0.0f, 0.0f)), -0.5f * kUnitDiagonal));
}

void ClipPlaneTests::flippingKeepsTheOtherSide()
{
    GlobalRenderSettings s = enabled(ClipPlaneAxis::X);
    const QVector4D plane = ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ);
    s.clipPlaneFlipped = true;
    const QVector4D flipped = ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ);

    const QVector3D probe(0.5f, 0.0f, 0.0f);
    QVERIFY(distance(plane, probe) > 0.0f);
    QVERIFY(distance(flipped, probe) < 0.0f);
}

void ClipPlaneTests::flippingDoesNotMoveThePlane()
{
    // At a non-zero offset a flip must swap which side survives without sliding the cut,
    // or the picture jumps when you press the button.
    GlobalRenderSettings s = enabled(ClipPlaneAxis::X);
    s.clipPlaneOffset = 0.3f;
    const QVector4D plane = ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ);
    s.clipPlaneFlipped = true;
    const QVector4D flipped = ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ);

    const QVector3D onPlane(0.3f * kUnitDiagonal, 0.7f, -0.2f);
    QVERIFY(closeTo(distance(plane, onPlane), 0.0f));
    QVERIFY2(closeTo(distance(flipped, onPlane), 0.0f),
             "the flipped plane is the same plane, facing the other way");
}

void ClipPlaneTests::aLayerTransformMovesThePlaneWithIt()
{
    // One world plane, a layer translated 10 along X: a local point survives exactly when
    // the world point it maps to does. Getting this wrong cuts transformed layers in the
    // wrong place, which is the common case in an alignment session.
    const QVector4D world = ClipPlane::world(enabled(ClipPlaneAxis::X), kUnitMin, kUnitMax, kLookingDownZ);

    QMatrix4x4 transform;
    transform.translate(10.0f, 0.0f, 0.0f);
    transform.rotate(37.0f, 0.3f, 1.0f, 0.2f);
    transform.scale(2.0f);
    const QVector4D local = ClipPlane::toLocal(world, transform);

    const QVector3D probes[] = {
        QVector3D(0.0f, 0.0f, 0.0f), QVector3D(1.0f, -2.0f, 0.5f),
        QVector3D(-4.0f, 3.0f, 1.0f), QVector3D(0.2f, 0.2f, -7.0f)
    };
    for (const QVector3D &p : probes) {
        const float localDistance = distance(local, p);
        const float worldDistance = distance(world, transform.map(p));
        QVERIFY2(closeTo(localDistance, worldDistance, 1.0e-3f),
                 qPrintable(QStringLiteral("local %1 vs world %2")
                                .arg(localDistance).arg(worldDistance)));
    }
}

void ClipPlaneTests::aLocalPlaneOfNothingIsStillNothing()
{
    QMatrix4x4 transform;
    transform.translate(3.0f, 4.0f, 5.0f);
    // A zero plane is how every pass says "no clipping"; a transform must not turn it into
    // a real one, or a transformed layer would be cut when nothing else is.
    QCOMPARE(ClipPlane::toLocal(QVector4D(), transform), QVector4D());
}

void ClipPlaneTests::theGizmoLiesOnThePlane()
{
    GlobalRenderSettings s = enabled(ClipPlaneAxis::Custom);
    s.clipPlaneCustomAxis = QVector3D(1.0f, 2.0f, -0.5f);
    s.clipPlaneOffset = 0.2f;
    const QVector4D plane = ClipPlane::world(s, kUnitMin, kUnitMax, kLookingDownZ);

    const std::vector<float> lines = ClipPlane::planeGizmo(plane, kUnitMin, kUnitMax, 4);
    QVERIFY(!lines.empty());
    QCOMPARE(int(lines.size()) % 6, 0);
    // 4 border + 2*4 grid + 1 stem.
    QCOMPARE(int(lines.size()) / 6, 13);

    // Every point except the stem's far end lies on the plane.
    const int pointCount = int(lines.size()) / 3;
    for (int i = 0; i < pointCount - 1; ++i) {
        const QVector3D p(lines[i * 3], lines[i * 3 + 1], lines[i * 3 + 2]);
        QVERIFY2(closeTo(distance(plane, p), 0.0f, 1.0e-3f),
                 qPrintable(QStringLiteral("gizmo point %1 is off the plane by %2")
                                .arg(i).arg(distance(plane, p))));
    }

    // And it spans the scene rather than hiding inside it.
    float reach = 0.0f;
    for (int i = 0; i < pointCount; ++i) {
        const QVector3D p(lines[i * 3], lines[i * 3 + 1], lines[i * 3 + 2]);
        reach = std::max(reach, p.length());
    }
    QVERIFY(reach > 0.5f * kUnitDiagonal);
}

void ClipPlaneTests::theGizmoStemPointsAtWhatIsKept()
{
    const QVector4D plane = ClipPlane::world(enabled(ClipPlaneAxis::Z), kUnitMin, kUnitMax, kLookingDownZ);
    const std::vector<float> lines = ClipPlane::planeGizmo(plane, kUnitMin, kUnitMax, 0);
    QCOMPARE(int(lines.size()) / 6, 5);   // 4 border + the stem

    const int last = int(lines.size()) - 3;
    const QVector3D tip(lines[last], lines[last + 1], lines[last + 2]);
    QVERIFY2(distance(plane, tip) > 0.0f, "the stem leans towards the surviving side");
}

void ClipPlaneTests::theGizmoIsEmptyWithoutAPlane()
{
    QVERIFY(ClipPlane::planeGizmo(QVector4D(), kUnitMin, kUnitMax).empty());
    const QVector4D plane(0.0f, 0.0f, 1.0f, 0.0f);
    const QVector3D point(1.0f, 1.0f, 1.0f);
    QVERIFY2(ClipPlane::planeGizmo(plane, point, point).empty(), "no scene, nothing to size it to");
}

QTEST_APPLESS_MAIN(ClipPlaneTests)
#include "test_clipplane.moc"
