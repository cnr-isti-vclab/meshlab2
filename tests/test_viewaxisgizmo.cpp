#include <QtTest/QtTest>

#include <QColor>
#include <QImage>
#include <QQuaternion>
#include <QVector3D>

#include <QSignalSpy>
#include <QTest>

#include "viewaxisgizmo.h"

// The orientation gizmo is painted with QPainter, so depth is the painter's algorithm and
// nothing else enforces it. It used to draw in two passes -- every stem, then every disc --
// which put all six discs on top of all three stems regardless of the sort, so a handle
// behind an axis still covered it. Nothing catches that but looking at the pixels.
class ViewAxisGizmoTests : public QObject
{
    Q_OBJECT

private slots:
    void aHandleBehindAnAxisDoesNotCoverIt();
    void aHandleInFrontOfAnAxisStillCoversIt();
    void clickingTheAxisYouAlreadyFaceReversesIt();
    void clickingAnAxisYouAreNotFacingSnapsToIt();

private:
    // Finds a rotation where the `far` axis's disc overlaps the `near` axis's stem, with the
    // far handle genuinely behind. Returns the widget point to sample, or a null point.
    struct Crossing {
        QQuaternion rotation;
        QPointF sample;
        int nearAxis = -1;
        int farAxis = -1;
        bool found = false;
    };
    static Crossing findCrossing(ViewAxisGizmo &gizmo, bool wantFarBehind);
};

ViewAxisGizmoTests::Crossing
ViewAxisGizmoTests::findCrossing(ViewAxisGizmo &gizmo, bool wantFarBehind)
{
    const qreal radius = ViewAxisGizmo::handleRadius();
    const QPointF mid(gizmo.width() / 2.0, gizmo.height() / 2.0);

    // Sweep orientations rather than hand-picking one: the geometry that makes a disc sit on
    // another axis's stem is fiddly, and a search states the condition instead of a magic
    // quaternion whose meaning is lost the moment the handle radius changes.
    for (int yaw = 0; yaw < 360; yaw += 3) {
        for (int pitch = -80; pitch <= 80; pitch += 5) {
            const QQuaternion rotation =
                QQuaternion::fromEulerAngles(float(pitch), float(yaw), 0.0f);
            gizmo.setOrientation(rotation);
            const auto handles = gizmo.projectedHandles();

            for (const auto &stem : handles) {
                if (stem.negative)
                    continue;
                for (const auto &disc : handles) {
                    if (disc.negative || disc.axis == stem.axis)
                        continue;
                    // "Behind" or "in front" relative to the stem's handle.
                    const bool discIsBehind = disc.depth < stem.depth;
                    if (discIsBehind != wantFarBehind)
                        continue;

                    // Walk the stem and look for a point well inside the disc but clear of
                    // the stem's own handle, so the sampled pixel is unambiguous.
                    for (double t = 0.35; t <= 0.85; t += 0.02) {
                        const QPointF on = mid + t * (stem.center - mid);
                        const QPointF toDisc = on - disc.center;
                        const QPointF toStemEnd = on - stem.center;
                        const double dDisc = std::hypot(toDisc.x(), toDisc.y());
                        const double dStem = std::hypot(toStemEnd.x(), toStemEnd.y());
                        if (dDisc < radius - 3.0 && dStem > radius + 3.0) {
                            Crossing c;
                            c.rotation = rotation;
                            c.sample = on;
                            c.nearAxis = stem.axis;
                            c.farAxis = disc.axis;
                            c.found = true;
                            return c;
                        }
                    }
                }
            }
        }
    }
    return {};
}

void ViewAxisGizmoTests::aHandleBehindAnAxisDoesNotCoverIt()
{
    ViewAxisGizmo gizmo;
    gizmo.resize(80, 80);

    const Crossing crossing = findCrossing(gizmo, /*wantFarBehind=*/true);
    QVERIFY2(crossing.found, "no orientation put a handle behind another axis's stem");
    gizmo.setOrientation(crossing.rotation);

    QImage shot(gizmo.size(), QImage::Format_ARGB32);
    shot.fill(Qt::transparent);
    gizmo.render(&shot);

    const QColor sampled = shot.pixelColor(crossing.sample.toPoint());
    // The disc is behind, so the stem must be what shows through: the sampled pixel has to
    // be the stem's colour, not the disc's. Comparing which of the two it is nearer to keeps
    // this robust against antialiasing at the stem's edge.
    const QColor axisColors[3] = { QColor(0xF7, 0x5C, 0x68), QColor(0x9E, 0xD1, 0x3B),
                                   QColor(0x4B, 0x9B, 0xE8) };
    const auto distance = [&sampled](const QColor &c) {
        return std::hypot(std::hypot(sampled.red() - c.red(), sampled.green() - c.green()),
                          sampled.blue() - c.blue());
    };
    const double toStem = distance(axisColors[crossing.nearAxis]);
    const double toDisc = distance(axisColors[crossing.farAxis]);
    QVERIFY2(toStem < toDisc,
             qPrintable(QStringLiteral("a handle behind axis %1 covered it: sampled %2, "
                                       "stem %3, disc %4")
                            .arg(crossing.nearAxis)
                            .arg(sampled.name(), axisColors[crossing.nearAxis].name(),
                                 axisColors[crossing.farAxis].name())));
}

void ViewAxisGizmoTests::aHandleInFrontOfAnAxisStillCoversIt()
{
    // The other direction, so the fix cannot be "never let a disc cover a stem".
    ViewAxisGizmo gizmo;
    gizmo.resize(80, 80);

    const Crossing crossing = findCrossing(gizmo, /*wantFarBehind=*/false);
    QVERIFY2(crossing.found, "no orientation put a handle in front of another axis's stem");
    gizmo.setOrientation(crossing.rotation);

    QImage shot(gizmo.size(), QImage::Format_ARGB32);
    shot.fill(Qt::transparent);
    gizmo.render(&shot);

    const QColor sampled = shot.pixelColor(crossing.sample.toPoint());
    const QColor axisColors[3] = { QColor(0xF7, 0x5C, 0x68), QColor(0x9E, 0xD1, 0x3B),
                                   QColor(0x4B, 0x9B, 0xE8) };
    const auto distance = [&sampled](const QColor &c) {
        return std::hypot(std::hypot(sampled.red() - c.red(), sampled.green() - c.green()),
                          sampled.blue() - c.blue());
    };
    QVERIFY2(distance(axisColors[crossing.farAxis]) < distance(axisColors[crossing.nearAxis]),
             "a handle in front of an axis should cover it");
}


namespace {

// Clicks the handle whose centre is at `at`, press and release on the same point so the
// gizmo sees a click rather than the start of an orbit.
void clickAt(ViewAxisGizmo &gizmo, const QPointF &at)
{
    QTest::mousePress(&gizmo, Qt::LeftButton, Qt::NoModifier, at.toPoint());
    QTest::mouseRelease(&gizmo, Qt::LeftButton, Qt::NoModifier, at.toPoint());
}

} // namespace

void ViewAxisGizmoTests::clickingTheAxisYouAlreadyFaceReversesIt()
{
    ViewAxisGizmo gizmo;
    gizmo.resize(80, 80);
    gizmo.show();
    QVERIFY(QTest::qWaitForWindowExposed(&gizmo));

    // Looking straight down +X: the camera sits on +X, so m_rotation takes the world +X to
    // camera +z, which is what the gizmo reads as "you are already facing this one".
    gizmo.setOrientation(QQuaternion::fromEulerAngles(0.0f, -90.0f, 0.0f));
    const auto handles = gizmo.projectedHandles();
    const auto &plusX = handles[0];
    QVERIFY2(plusX.depth > 0.99f,
             qPrintable(QStringLiteral("+X should be facing the viewer, depth %1")
                            .arg(double(plusX.depth))));

    QSignalSpy picked(&gizmo, &ViewAxisGizmo::axisPicked);
    clickAt(gizmo, plusX.center);

    QCOMPARE(picked.count(), 1);
    const QVector3D direction = picked.first().first().value<QVector3D>();
    // Reversed, not repeated: clicking the letter you are already looking down used to emit
    // the same direction and so did nothing at all.
    QVERIFY2(direction.x() < -0.9f,
             qPrintable(QStringLiteral("expected -X, got (%1, %2, %3)")
                            .arg(double(direction.x()))
                            .arg(double(direction.y()))
                            .arg(double(direction.z()))));
}

void ViewAxisGizmoTests::clickingAnAxisYouAreNotFacingSnapsToIt()
{
    // The other half: from anywhere else the same handle must still snap TO the axis, or
    // the flip would make the gizmo unusable for its actual job.
    ViewAxisGizmo gizmo;
    gizmo.resize(80, 80);
    gizmo.show();
    QVERIFY(QTest::qWaitForWindowExposed(&gizmo));

    gizmo.setOrientation(QQuaternion()); // looking down -Z, so +X is off to the side
    const auto handles = gizmo.projectedHandles();
    const auto &plusX = handles[0];
    QVERIFY(std::abs(plusX.depth) < 0.5f);

    QSignalSpy picked(&gizmo, &ViewAxisGizmo::axisPicked);
    clickAt(gizmo, plusX.center);

    QCOMPARE(picked.count(), 1);
    const QVector3D direction = picked.first().first().value<QVector3D>();
    QVERIFY2(direction.x() > 0.9f, "should snap to +X, not away from it");
}

QTEST_MAIN(ViewAxisGizmoTests)
#include "test_viewaxisgizmo.moc"
