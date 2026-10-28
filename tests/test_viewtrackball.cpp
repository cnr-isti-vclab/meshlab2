#include <QtTest/QtTest>

#include "viewtrackball.h"

#include <QJsonObject>
#include <QMouseEvent>
#include <QWheelEvent>

#include <cmath>
#include <limits>

// The orbit camera under both projections. What these pin is the one quantity the two share,
// H = 2 * distance * tan(fovY / 2), the world height visible at the orbit centre: if it holds,
// switching projection changes nothing about what is framed, and every piece of navigation
// arithmetic written in terms of it is right for both.
class ViewTrackballTests : public QObject
{
    Q_OBJECT

private slots:
    void anOrthographicViewFramesTheSameHeightAtTheCentreAsPerspective();
    void aPointOneHalfHeightAboveTheCentreIsAtTheTopEdgeAtAnyDistance();
    void perspectiveRaysShareTheEye();
    void orthographicRaysAreParallelAndStartOnTheNearPlane();
    void bothRaysThroughTheCentrePixelReachTheOrbitCentre();
    void theOrthographicVolumeStartsBehindTheEyeWhenTheCentreIsClose();
    void aCloserNearPlaneIsHonouredOnlyByOrthographic();
    void theOrthographicVolumeJustContainsTheSceneBox();
    void aSmallBoxStillLeavesRoomForTheGizmo();
    void perspectiveIgnoresTheSceneBox();
    void theFieldOfViewWheelPassesThroughTheNarrowestFieldIntoOrthographic();
    void turningTheFieldOfViewWheelBackLeavesOrthographic();
    void theFieldOfViewWheelKeepsTheVisibleHeightThroughTheSwitch();
    void theOrdinaryWheelStillZoomsInOrthographic();
    void theProjectionSurvivesStateRoundTripAndJson();
    void aBadProjectionNamesTheKey();
    void shiftDragRollsTheViewAboutItsOwnAxis();
    void rollingNearTheCentreDoesNothing();
    void framingFromTheEyeFillsTheViewWithoutMovingIt();
    void framingFromTheEyeRefusesWhatItCannotDo();
    void framingFromTheEyeCanChooseTheAspect();
    void theCameraStateDocumentRoundTripsWithItsViewport();
};

namespace {

ViewTrackball::State sampleState(Projection projection, float distance = 7.0f)
{
    ViewTrackball::State s;
    s.center = QVector3D(1.0f, 2.0f, 3.0f);
    s.rotation = QQuaternion::fromEulerAngles(20.0f, 35.0f, -10.0f);
    s.distance = distance;
    s.radius = 2.0f;
    s.fovYDeg = 50.0f;
    s.projection = projection;
    return s;
}

ViewTrackball makeTrackball(Projection projection, float distance = 7.0f)
{
    ViewTrackball tb;
    tb.setState(sampleState(projection, distance));
    tb.setSceneRadius(3.0f);
    return tb;
}

// Normalized device coordinates of a world point.
QVector3D ndc(const ViewTrackball &tb, float aspect, const QVector3D &world)
{
    const QVector4D clip = tb.projectionMatrix(aspect) * tb.viewMatrix() * QVector4D(world, 1.0f);
    return QVector3D(clip.x(), clip.y(), clip.z()) / clip.w();
}

float visibleHeight(const ViewTrackball &tb)
{
    return 2.0f * tb.state().distance * std::tan(0.5f * qDegreesToRadians(tb.fovYDegrees()));
}

constexpr float kAspect = 16.0f / 9.0f;
const QSize kViewport(1600, 900);

bool near(float a, float b, float tolerance = 1e-4f)
{
    return std::abs(a - b) <= tolerance;
}

} // namespace

void ViewTrackballTests::anOrthographicViewFramesTheSameHeightAtTheCentreAsPerspective()
{
    const ViewTrackball persp = makeTrackball(Projection::Perspective);
    const ViewTrackball ortho = makeTrackball(Projection::Orthographic);
    const QVector3D right = persp.cameraRight();
    const QVector3D up = persp.cameraUp();
    const QVector3D centre = persp.center();

    // Points on the plane through the orbit centre, square to the view.
    for (const QVector2D &offset : { QVector2D(0, 0), QVector2D(0.7f, 0), QVector2D(-1.3f, 0.4f),
                                     QVector2D(0.2f, -1.1f) }) {
        const QVector3D p = centre + right * offset.x() + up * offset.y();
        const QVector3D a = ndc(persp, kAspect, p);
        const QVector3D b = ndc(ortho, kAspect, p);
        QVERIFY2(near(a.x(), b.x()) && near(a.y(), b.y()),
                 qPrintable(QStringLiteral("(%1, %2) vs (%3, %4)")
                                .arg(a.x()).arg(a.y()).arg(b.x()).arg(b.y())));
    }
}

void ViewTrackballTests::aPointOneHalfHeightAboveTheCentreIsAtTheTopEdgeAtAnyDistance()
{
    for (const float distance : { 0.8f, 7.0f, 400.0f }) {
        const ViewTrackball tb = makeTrackball(Projection::Orthographic, distance);
        const QVector3D top = tb.center() + tb.cameraUp() * (0.5f * visibleHeight(tb));
        QVERIFY2(near(ndc(tb, kAspect, top).y(), 1.0f, 1e-3f), qPrintable(QString::number(distance)));
        // And its width is H * aspect.
        const QVector3D side = tb.center() + tb.cameraRight() * (0.5f * visibleHeight(tb) * kAspect);
        QVERIFY2(near(ndc(tb, kAspect, side).x(), 1.0f, 1e-3f), qPrintable(QString::number(distance)));
    }
}

void ViewTrackballTests::perspectiveRaysShareTheEye()
{
    const ViewTrackball tb = makeTrackball(Projection::Perspective);
    for (const QPointF &pixel : { QPointF(0, 0), QPointF(800, 450), QPointF(1599, 899), QPointF(300, 700) }) {
        ViewRay ray;
        QVERIFY(tb.viewRay(pixel, kViewport, ray));
        QVERIFY((ray.origin - tb.cameraEyePosition()).length() < 1e-3f);
        QVERIFY(near(ray.direction.length(), 1.0f));
    }
}

void ViewTrackballTests::orthographicRaysAreParallelAndStartOnTheNearPlane()
{
    const ViewTrackball tb = makeTrackball(Projection::Orthographic);
    const QVector3D viewDir = tb.cameraViewDirection();
    ViewRay centre;
    QVERIFY(tb.viewRay(QPointF(800, 450), kViewport, centre));

    for (const QPointF &pixel : { QPointF(0, 0), QPointF(1599, 899), QPointF(300, 700) }) {
        ViewRay ray;
        QVERIFY(tb.viewRay(pixel, kViewport, ray));
        QVERIFY((ray.direction - viewDir).length() < 1e-4f);
        // The origin is on the near plane: that far from the eye along the view direction.
        const float alongAxis = QVector3D::dotProduct(ray.origin - tb.cameraEyePosition(), viewDir);
        QVERIFY2(near(alongAxis, tb.nearClipPlaneDistance(), 1e-3f),
                 qPrintable(QStringLiteral("%1 vs %2").arg(alongAxis).arg(tb.nearClipPlaneDistance())));
        // And the pixel decides where it starts: offset from the centre ray's origin in the
        // image plane by that pixel's share of the view volume's width.
        const QVector3D shift = ray.origin - centre.origin;
        QVERIFY(std::abs(QVector3D::dotProduct(shift, viewDir)) < 1e-3f);
        const float expectedX = (float(pixel.x()) / kViewport.width() * 2.0f - 1.0f)
            * 0.5f * visibleHeight(tb) * kAspect;
        QVERIFY(near(QVector3D::dotProduct(shift, tb.cameraRight()), expectedX, 1e-3f));
    }
}

void ViewTrackballTests::bothRaysThroughTheCentrePixelReachTheOrbitCentre()
{
    for (const Projection projection : { Projection::Perspective, Projection::Orthographic }) {
        const ViewTrackball tb = makeTrackball(projection);
        ViewRay ray;
        QVERIFY(tb.viewRay(QPointF(800, 450), kViewport, ray));
        // The distance along the ray to the point nearest the centre is the centre's.
        const float t = QVector3D::dotProduct(tb.center() - ray.origin, ray.direction);
        const QVector3D closest = ray.origin + ray.direction * t;
        QVERIFY((closest - tb.center()).length() < 1e-3f);
    }
}

void ViewTrackballTests::theOrthographicVolumeStartsBehindTheEyeWhenTheCentreIsClose()
{
    // distance 1 against a scene reach of 4 * max(radius, sceneRadius) = 12.
    const ViewTrackball tb = makeTrackball(Projection::Orthographic, 1.0f);
    QVERIFY(tb.nearClipPlaneDistance() < 0.0f);
    QVERIFY(tb.farClipPlaneDistance() > 1.0f);

    // A point 5 units behind the eye, on the view axis: still inside the depth range.
    const QVector3D behindTheEye = tb.cameraEyePosition() - tb.cameraViewDirection() * 5.0f;
    const QVector3D n = ndc(tb, kAspect, behindTheEye);
    QVERIFY2(n.z() > -1.0f && n.z() < 1.0f, qPrintable(QString::number(n.z())));
    // And it is on the axis, so it lands at the middle of the view.
    QVERIFY(near(n.x(), 0.0f, 1e-3f) && near(n.y(), 0.0f, 1e-3f));

    // Perspective never has a negative near plane.
    QVERIFY(makeTrackball(Projection::Perspective, 1.0f).nearClipPlaneDistance() > 0.0f);
}

void ViewTrackballTests::aCloserNearPlaneIsHonouredOnlyByOrthographic()
{
    // The distance of an orthographic matrix's near plane, read back from its depth row.
    const auto nearOf = [](const QMatrix4x4 &m) { return (m(2, 3) + 1.0f) / m(2, 2); };

    const ViewTrackball ortho = makeTrackball(Projection::Orthographic);
    QVERIFY(near(nearOf(ortho.projectionMatrix(kAspect)), ortho.nearClipPlaneDistance(), 1e-3f));
    // Ask for the volume to start 40 units behind the eye, further than the scene reach does.
    QVERIFY(ortho.nearClipPlaneDistance() > -40.0f);
    QVERIFY(near(nearOf(ortho.projectionMatrix(kAspect, 0.0f, -40.0f)), -40.0f, 1e-3f));
    // Asking for less than it already holds changes nothing.
    QCOMPARE(ortho.projectionMatrix(kAspect, 0.0f, 100.0f), ortho.projectionMatrix(kAspect));

    // Perspective cannot see behind the eye and ignores the request.
    const ViewTrackball persp = makeTrackball(Projection::Perspective);
    QCOMPARE(persp.projectionMatrix(kAspect, 0.0f, -40.0f), persp.projectionMatrix(kAspect));
}

// Distances in front of the eye of the nearest and farthest corner of a world box.
void boxDepths(const ViewTrackball &tb, const QVector3D &lo, const QVector3D &hi, float &nearest,
               float &farthest)
{
    const QMatrix4x4 view = tb.viewMatrix();
    nearest = std::numeric_limits<float>::infinity();
    farthest = -nearest;
    for (int c = 0; c < 8; ++c) {
        const QVector3D p((c & 1) ? hi.x() : lo.x(), (c & 2) ? hi.y() : lo.y(),
                          (c & 4) ? hi.z() : lo.z());
        const float d = -(view * p).z();
        nearest = std::min(nearest, d);
        farthest = std::max(farthest, d);
    }
}

void ViewTrackballTests::theOrthographicVolumeJustContainsTheSceneBox()
{
    ViewTrackball tb = makeTrackball(Projection::Orthographic);
    // A box well larger than the gizmo, off-centre, so its depth range is its own.
    const QVector3D lo(-9, -2, -8), hi(14, 9, 11);
    tb.setSceneBounds(lo, hi);

    float nearest, farthest;
    boxDepths(tb, lo, hi, nearest, farthest);
    const float nearPlane = tb.nearClipPlaneDistance();
    const float farPlane = tb.farClipPlaneDistance();
    // Contains it ...
    QVERIFY2(nearPlane < nearest && farPlane > farthest,
             qPrintable(QStringLiteral("[%1, %2] vs [%3, %4]").arg(nearPlane).arg(farPlane)
                            .arg(nearest).arg(farthest)));
    // ... and only just: the slack is the 5% pad and a thousandth of the view height.
    const float range = farthest - nearest;
    const float slack = 0.05f * range + 1.5e-3f * visibleHeight(tb);
    QVERIFY2(nearest - nearPlane < slack && farPlane - farthest < slack,
             qPrintable(QStringLiteral("slack %1 / %2 vs %3")
                            .arg(nearest - nearPlane).arg(farPlane - farthest).arg(slack)));

    // The matrix maps the corners strictly inside the depth range.
    for (int c = 0; c < 8; ++c) {
        const QVector3D p((c & 1) ? hi.x() : lo.x(), (c & 2) ? hi.y() : lo.y(),
                          (c & 4) ? hi.z() : lo.z());
        const float z = ndc(tb, kAspect, p).z();
        QVERIFY2(z > -1.0f && z < 1.0f, qPrintable(QString::number(z)));
    }
}

void ViewTrackballTests::aSmallBoxStillLeavesRoomForTheGizmo()
{
    // A box far smaller than the trackball gizmo about the orbit centre: the gizmo, not the
    // box, sets the depth range, or its far side is clipped away.
    ViewTrackball tb = makeTrackball(Projection::Orthographic);
    const QVector3D centre = tb.center();
    tb.setSceneBounds(centre - QVector3D(0.1f, 0.1f, 0.1f), centre + QVector3D(0.1f, 0.1f, 0.1f));
    const float gizmo = tb.gizmoWorldRadius();
    QVERIFY(gizmo > 1.0f);
    QVERIFY(tb.nearClipPlaneDistance() < tb.state().distance - gizmo);
    QVERIFY(tb.farClipPlaneDistance() > tb.state().distance + gizmo);
}

void ViewTrackballTests::perspectiveIgnoresTheSceneBox()
{
    ViewTrackball tb = makeTrackball(Projection::Perspective);
    const float nearBefore = tb.nearClipPlaneDistance();
    const float farBefore = tb.farClipPlaneDistance();
    tb.setSceneBounds(QVector3D(-1, -1, -1), QVector3D(1, 1, 1));
    QCOMPARE(tb.nearClipPlaneDistance(), nearBefore);
    QCOMPARE(tb.farClipPlaneDistance(), farBefore);
}

namespace {

// Shift + wheel: forward (positive) narrows the field of view.
bool shiftWheel(ViewTrackball &tb, int notches)
{
    const QPointF pos(100, 100);
    const QWheelEvent e(pos, pos, QPoint(), QPoint(0, 120 * notches), Qt::NoButton,
                        Qt::ShiftModifier, Qt::NoScrollPhase, false);
    return tb.wheel(&e);
}

constexpr float kNarrowest = 10.0f; // the narrowest perspective field of view

} // namespace

void ViewTrackballTests::theFieldOfViewWheelPassesThroughTheNarrowestFieldIntoOrthographic()
{
    ViewTrackball tb = makeTrackball(Projection::Perspective);
    // Wheel forward until the field of view runs out; it must end orthographic.
    int notches = 0;
    while (!tb.isOrthographic() && notches < 60) {
        QVERIFY(shiftWheel(tb, 1));
        ++notches;
    }
    QVERIFY2(tb.isOrthographic(), "never reached orthographic");
    // The field of view stopped at its floor on the way, rather than past it.
    QVERIFY(near(tb.fovYDegrees(), kNarrowest));
    QVERIFY(notches > 1);

    // Further the same way is a no-op: nothing is flatter than parallel.
    const ViewTrackball::State before = tb.state();
    QVERIFY(!shiftWheel(tb, 1));
    QCOMPARE(tb.state().distance, before.distance);
    QVERIFY(tb.isOrthographic());
}

void ViewTrackballTests::turningTheFieldOfViewWheelBackLeavesOrthographic()
{
    ViewTrackball tb = makeTrackball(Projection::Perspective);
    while (!tb.isOrthographic())
        QVERIFY(shiftWheel(tb, 1));

    QVERIFY(shiftWheel(tb, -1));
    QVERIFY(!tb.isOrthographic());
    QVERIFY(tb.fovYDegrees() > kNarrowest);

    // And forward once more is orthographic again: the wheel is a continuous control.
    while (!tb.isOrthographic())
        QVERIFY(shiftWheel(tb, 1));
    QVERIFY(tb.isOrthographic());
}

void ViewTrackballTests::theFieldOfViewWheelKeepsTheVisibleHeightThroughTheSwitch()
{
    ViewTrackball tb = makeTrackball(Projection::Perspective);
    const float height = visibleHeight(tb);
    // All the way in, and part of the way back out.
    for (int i = 0; i < 40; ++i)
        shiftWheel(tb, 1);
    QVERIFY(tb.isOrthographic());
    QVERIFY2(near(visibleHeight(tb), height, 1e-3f * height), qPrintable(QString::number(visibleHeight(tb))));
    for (int i = 0; i < 3; ++i)
        shiftWheel(tb, -1);
    QVERIFY(near(visibleHeight(tb), height, 1e-3f * height));
}

void ViewTrackballTests::theOrdinaryWheelStillZoomsInOrthographic()
{
    ViewTrackball ortho = makeTrackball(Projection::Orthographic);
    const float heightBefore = visibleHeight(ortho);
    const QPointF pos(100, 100);
    const QWheelEvent wheel(pos, pos, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier,
                            Qt::NoScrollPhase, false);
    QVERIFY(ortho.wheel(&wheel));
    // Zooming in shrinks the view volume: H follows distance.
    QVERIFY(visibleHeight(ortho) < heightBefore);
}

void ViewTrackballTests::theProjectionSurvivesStateRoundTripAndJson()
{
    ViewTrackball tb;
    tb.setState(sampleState(Projection::Orthographic));
    QCOMPARE(tb.state().projection, Projection::Orthographic);
    QVERIFY(tb.isOrthographic());

    ViewTrackball::State parsed;
    QString error;
    QVERIFY2(ViewTrackball::stateFromJson(
                 QJsonObject{ { QStringLiteral("projection"), QStringLiteral("orthographic") } },
                 parsed, &error), qPrintable(error));
    QCOMPARE(parsed.projection, Projection::Orthographic);

    // An absent key leaves the default, perspective.
    ViewTrackball::State untouched;
    QVERIFY(ViewTrackball::stateFromJson(QJsonObject{}, untouched, &error));
    QCOMPARE(untouched.projection, Projection::Perspective);

    // The names the payloads carry.
    QCOMPARE(ViewTrackball::projectionJsonName(Projection::Perspective), QStringLiteral("perspective"));
    QCOMPARE(ViewTrackball::projectionJsonName(Projection::Orthographic), QStringLiteral("orthographic"));
}

void ViewTrackballTests::aBadProjectionNamesTheKey()
{
    ViewTrackball::State parsed;
    QString error;
    QVERIFY(!ViewTrackball::stateFromJson(
        QJsonObject{ { QStringLiteral("projection"), QStringLiteral("oblique") } }, parsed, &error));
    QVERIFY2(error.contains(QStringLiteral("projection")), qPrintable(error));
    QVERIFY(!ViewTrackball::stateFromJson(
        QJsonObject{ { QStringLiteral("projection"), 3 } }, parsed, &error));
}

namespace {

QMouseEvent mouseEvent(QEvent::Type type, const QPointF &pos, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
{
    return QMouseEvent(
        type, pos, pos, Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : buttons, mods);
}

} // namespace

void ViewTrackballTests::shiftDragRollsTheViewAboutItsOwnAxis()
{
    ViewTrackball tb = makeTrackball(Projection::Perspective);
    const QVector3D look = tb.cameraViewDirection();
    const QVector3D oldRight = tb.cameraRight();
    const QVector3D oldUp = tb.cameraUp();
    const QVector3D centre = tb.state().center;
    const QSize size(400, 400);

    // From the right of the centre, round to above it: a quarter turn counter-clockwise on screen.
    const QPointF c(200.0, 200.0);
    const QMouseEvent press = mouseEvent(QEvent::MouseButtonPress, c + QPointF(100, 0), Qt::LeftButton, Qt::ShiftModifier);
    tb.mousePress(&press, size);
    // In steps, as a real drag would be, through 45 degrees.
    for (int i = 1; i <= 2; ++i) {
        const double a = qDegreesToRadians(45.0 * i);
        const QMouseEvent move = mouseEvent(
            QEvent::MouseMove, c + QPointF(100.0 * std::cos(a), -100.0 * std::sin(a)), Qt::LeftButton, Qt::ShiftModifier);
        QVERIFY(tb.mouseMove(&move, size));
    }
    const QMouseEvent release = mouseEvent(QEvent::MouseButtonRelease, c + QPointF(0, -100), Qt::LeftButton, Qt::ShiftModifier);
    tb.mouseRelease(&release);

    // The camera looks the same way from the same place; what was at the right of the screen is now above.
    QVERIFY((tb.cameraViewDirection() - look).length() < 1e-4f);
    QVERIFY((tb.state().center - centre).length() < 1e-6f);
    QVERIFY((tb.cameraUp() - oldRight).length() < 1e-4f);
    QVERIFY((tb.cameraRight() + oldUp).length() < 1e-4f);
}

void ViewTrackballTests::rollingNearTheCentreDoesNothing()
{
    ViewTrackball tb = makeTrackball(Projection::Perspective);
    const QVector3D up = tb.cameraUp();
    const QSize size(400, 400);
    const QPointF c(200.0, 200.0);
    const QMouseEvent press = mouseEvent(QEvent::MouseButtonPress, c + QPointF(20, 0), Qt::LeftButton, Qt::ShiftModifier);
    tb.mousePress(&press, size);
    const QMouseEvent move = mouseEvent(QEvent::MouseMove, c + QPointF(0, -20), Qt::LeftButton, Qt::ShiftModifier);
    QVERIFY(!tb.mouseMove(&move, size));
    QVERIFY((tb.cameraUp() - up).length() < 1e-6f);
}

// An irregular cloud off the view axis ends up centred and touching two opposite edges of the
// view, with the eye where it was, no roll about the new axis, and the trackball gizmo the size
// on screen it was, as through a zoom: one far away and narrow, and
// one close and wide, where aiming at the middle of the image-plane extent overshoots by more
// than twice the error and diverges.
void ViewTrackballTests::framingFromTheEyeFillsTheViewWithoutMovingIt()
{
    const std::vector<QVector3D> offsets = { QVector3D(0, 0, 0), QVector3D(2.0f, 0.3f, -1.0f),
                                             QVector3D(-1.5f, 1.2f, 0.4f), QVector3D(0.2f, -2.1f, 1.8f),
                                             QVector3D(1.1f, 1.7f, 2.3f), QVector3D(-0.8f, -0.9f, -2.2f) };
    for (const float depth : { 12.0f, 1.5f }) {
        ViewTrackball tb = makeTrackball(Projection::Perspective);
        const QVector3D eye = tb.cameraEyePosition();
        const QVector3D oldUp = tb.cameraUp();
        const QVector3D away = tb.cameraViewDirection() * depth + tb.cameraRight() * 2.0f - oldUp * 1.0f;
        std::vector<QVector3D> points;
        for (const QVector3D &offset : offsets)
            points.push_back(eye + away + offset);
        // The trackball gizmo, as a fraction of the view's height.
        const auto gizmoOnScreen = [&tb] { return tb.gizmoWorldRadius() / visibleHeight(tb); };
        const float gizmoBefore = gizmoOnScreen();

        float aspect = kAspect;
        QVERIFY(tb.frameFromEye(points, aspect) == ViewTrackball::EyeFraming::Framed);
        QVERIFY((tb.cameraEyePosition() - eye).length() < 1e-4f);
        QVERIFY2(near(gizmoOnScreen(), gizmoBefore),
                 qPrintable(QStringLiteral("%1 became %2").arg(gizmoBefore).arg(gizmoOnScreen())));
        QVERIFY(std::abs(QVector3D::dotProduct(tb.cameraRight(), oldUp)) < 1e-4f);
        float xMin = 1e9f, xMax = -1e9f, yMin = 1e9f, yMax = -1e9f;
        for (const QVector3D &p : points) {
            const QVector3D q = ndc(tb, kAspect, p);
            xMin = std::min(xMin, q.x());
            xMax = std::max(xMax, q.x());
            yMin = std::min(yMin, q.y());
            yMax = std::max(yMax, q.y());
        }
        const QString extent = QStringLiteral("depth %1, fov %2: x %3..%4, y %5..%6")
                                   .arg(depth).arg(tb.fovYDegrees()).arg(xMin).arg(xMax).arg(yMin).arg(yMax);
        QVERIFY2(near(xMin + xMax, 0.0f, 1e-3f) && near(yMin + yMax, 0.0f, 1e-3f), qPrintable(extent));
        QVERIFY2(near(std::max(xMax, yMax), 1.0f, 1e-3f) && std::min(xMax, yMax) <= 1.0f + 1e-3f,
                 qPrintable(extent));
        QVERIFY2(depth > 10.0f || tb.fovYDegrees() > 60.0f, qPrintable(extent));
    }
}

// Each case it cannot frame leaves the camera exactly as it was.
void ViewTrackballTests::framingFromTheEyeRefusesWhatItCannotDo()
{
    using EyeFraming = ViewTrackball::EyeFraming;
    const ViewTrackball reference = makeTrackball(Projection::Perspective);
    const QVector3D eye = reference.cameraEyePosition();
    const QVector3D look = reference.cameraViewDirection();
    const QVector3D side = reference.cameraRight();
    const QVector3D up = reference.cameraUp();
    const auto check = [](Projection projection, const std::vector<QVector3D> &points, EyeFraming expected,
                          const char *what) {
        ViewTrackball tb = makeTrackball(projection);
        float aspect = kAspect;
        QVERIFY2(tb.frameFromEye(points, aspect) == expected, what);
        const ViewTrackball::State s = tb.state();
        const ViewTrackball::State t = makeTrackball(projection).state();
        QVERIFY2(s.center == t.center && s.rotation == t.rotation && s.distance == t.distance
                     && s.fovYDeg == t.fovYDeg && s.radius == t.radius,
                 what);
    };
    check(Projection::Perspective, {}, EyeFraming::NoPoints, "nothing");
    check(Projection::Orthographic, { eye + look * 10 + side, eye + look * 10 - side }, EyeFraming::Orthographic,
          "orthographic");
    // From inside the scene no direction has all of it in front.
    check(Projection::Perspective, { eye + look * 5, eye - look * 5, eye + side * 5 }, EyeFraming::BehindEye,
          "inside");
    // Well framed by its far part, but one point is nearer than the near plane would be.
    check(Projection::Perspective,
          { eye + look * 0.001f, eye + look * 10 + side * 4 + up * 2, eye + look * 10 - side * 4 - up * 2 },
          EyeFraming::TooClose, "close");
    check(Projection::Perspective, { eye + look + side * 10, eye + look - side * 10 }, EyeFraming::TooWide, "wide");
    check(Projection::Perspective, { eye + look * 1000 + side, eye + look * 1000 - side }, EyeFraming::TooNarrow,
          "far");
}

// Asked to choose the aspect too, as a snapshot fitted to the scene does, the points touch all
// four sides of a view of that aspect.
void ViewTrackballTests::framingFromTheEyeCanChooseTheAspect()
{
    ViewTrackball tb = makeTrackball(Projection::Perspective);
    const QVector3D eye = tb.cameraEyePosition();
    const QVector3D look = tb.cameraViewDirection();
    const QVector3D side = tb.cameraRight();
    const QVector3D up = tb.cameraUp();
    // Three times as wide as it is tall, off the axis, and not square to the view.
    std::vector<QVector3D> points;
    for (const QVector3D &corner : { QVector3D(-3, -1, 0), QVector3D(3, -1, 1), QVector3D(-3, 1, -1), QVector3D(3, 1, 0) })
        points.push_back(eye + look * (9.0f + corner.z()) + side * (2.0f + corner.x()) + up * (1.0f + corner.y()));

    float aspect = 0.0f;
    QVERIFY(tb.frameFromEye(points, aspect, true) == ViewTrackball::EyeFraming::Framed);
    QVERIFY2(aspect > 2.0f && aspect < 4.0f, qPrintable(QString::number(aspect)));
    QVERIFY((tb.cameraEyePosition() - eye).length() < 1e-4f);
    float xMin = 1e9f, xMax = -1e9f, yMin = 1e9f, yMax = -1e9f;
    for (const QVector3D &p : points) {
        const QVector3D q = ndc(tb, aspect, p);
        xMin = std::min(xMin, q.x());
        xMax = std::max(xMax, q.x());
        yMin = std::min(yMin, q.y());
        yMax = std::max(yMax, q.y());
    }
    const QString extent = QStringLiteral("x %1..%2, y %3..%4").arg(xMin).arg(xMax).arg(yMin).arg(yMax);
    QVERIFY2(near(xMin, -1.0f, 1e-3f) && near(xMax, 1.0f, 1e-3f) && near(yMin, -1.0f, 1e-3f)
                 && near(yMax, 1.0f, 1e-3f),
             qPrintable(extent));
}

// What a filter or the view writes as a camera, read back: the trackball, and the size of the
// image it was for, which frames a width as well as a height.
void ViewTrackballTests::theCameraStateDocumentRoundTripsWithItsViewport()
{
    const ViewTrackball::State written = sampleState(Projection::Orthographic);
    ViewTrackball::State read;
    QSize viewport;
    QString error;
    QVERIFY2(ViewTrackball::stateFromCameraStateJson(ViewTrackball::cameraStateJson(written, QSize(640, 480)),
                                                     read, &error, &viewport),
             qPrintable(error));
    QCOMPARE(viewport, QSize(640, 480));
    QVERIFY((read.center - written.center).length() < 1e-5f);
    QVERIFY(qFuzzyCompare(read.rotation, written.rotation));
    QCOMPARE(read.distance, written.distance);
    QCOMPARE(read.fovYDeg, written.fovYDeg);
    QCOMPARE(read.projection, Projection::Orthographic);

    QVERIFY(ViewTrackball::stateFromCameraStateJson(ViewTrackball::cameraStateJson(written), read, &error, &viewport));
    QVERIFY(!viewport.isValid());
    QVERIFY(!ViewTrackball::stateFromCameraStateJson(QStringLiteral("{\"kind\": \"MeshLab.RenderState\"}"), read, &error));
}

QTEST_APPLESS_MAIN(ViewTrackballTests)
#include "test_viewtrackball.moc"
