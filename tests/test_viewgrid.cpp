#include <QtTest/QtTest>

#include "preferences.h"
#include "viewgrid.h"

#include <cmath>

// The arithmetic behind the reference grid: which plane, and how finely ruled. How it looks is
// the fragment shader's, and is checked by tests/render_probes/reference_grid.
class ViewGridTests : public QObject
{
    Q_OBJECT

private slots:
    void noGridMeansNoPlane();
    void aPerspectiveViewUsesTheChosenPlane();
    void anOrthographicViewDownAnAxisUsesThePlaneAcrossIt();
    void anOrthographicViewOffTheAxesUsesTheChosenPlane();
    void theFineRulesAreOneToTenPixelsApart();
    void theDecadesSlideIntoEachOtherWithoutAJump();
    void theAxesHaveTheirUsualColours();
    void theStyleComesFromThePreferencesAndTheirDefaultsAreTheShadersOwn();
};

namespace {

bool is(const ViewGridPlane &plane, int u, int v, int normal)
{
    return plane.valid && plane.uAxis == u && plane.vAxis == v && plane.normalAxis == normal;
}

}

void ViewGridTests::noGridMeansNoPlane()
{
    QVERIFY(!chooseViewGridPlane(ReferenceGrid::None, QVector3D(0, 0, -1), false).valid);
    // Not even where an orthographic view would otherwise want one: the setting is the switch.
    QVERIFY(!chooseViewGridPlane(ReferenceGrid::None, QVector3D(0, 0, -1), true).valid);
}

void ViewGridTests::aPerspectiveViewUsesTheChosenPlane()
{
    // Looking straight down an axis makes no difference in perspective.
    for (const QVector3D &direction : { QVector3D(0, 0, -1), QVector3D(0, -1, 0), QVector3D(1, 1, -1) }) {
        QVERIFY(is(chooseViewGridPlane(ReferenceGrid::XY, direction, false), 0, 1, 2));
        QVERIFY(is(chooseViewGridPlane(ReferenceGrid::XZ, direction, false), 0, 2, 1));
    }
}

void ViewGridTests::anOrthographicViewDownAnAxisUsesThePlaneAcrossIt()
{
    // Whatever the setting says: the chosen plane would be edge-on for some of these.
    for (const ReferenceGrid setting : { ReferenceGrid::XY, ReferenceGrid::XZ }) {
        QVERIFY(is(chooseViewGridPlane(setting, QVector3D(0, 0, -1), true), 0, 1, 2));   // front/back
        QVERIFY(is(chooseViewGridPlane(setting, QVector3D(0, 0, 1), true), 0, 1, 2));
        QVERIFY(is(chooseViewGridPlane(setting, QVector3D(0, -1, 0), true), 0, 2, 1));   // top/bottom
        QVERIFY(is(chooseViewGridPlane(setting, QVector3D(0, 1, 0), true), 0, 2, 1));
        QVERIFY(is(chooseViewGridPlane(setting, QVector3D(1, 0, 0), true), 1, 2, 0));    // sides
        QVERIFY(is(chooseViewGridPlane(setting, QVector3D(-1, 0, 0), true), 1, 2, 0));
    }
    // A degree off the axis still counts as down it.
    QVERIFY(is(chooseViewGridPlane(ReferenceGrid::XZ, QVector3D(0.01f, 0, -1), true), 0, 1, 2));
}

void ViewGridTests::anOrthographicViewOffTheAxesUsesTheChosenPlane()
{
    const QVector3D oblique(1, -1, -1);
    QVERIFY(is(chooseViewGridPlane(ReferenceGrid::XY, oblique, true), 0, 1, 2));
    QVERIFY(is(chooseViewGridPlane(ReferenceGrid::XZ, oblique, true), 0, 2, 1));
    // Five degrees off an axis is too far.
    const QVector3D tilted(std::sin(float(M_PI) * 5.0f / 180.0f), 0, -std::cos(float(M_PI) * 5.0f / 180.0f));
    QVERIFY(is(chooseViewGridPlane(ReferenceGrid::XZ, tilted, true), 0, 2, 1));
}

void ViewGridTests::theFineRulesAreOneToTenPixelsApart()
{
    // From a view a pixel wide in micrometres to one a pixel wide in kilometres.
    for (double exponent = -6.0; exponent <= 3.0; exponent += 0.137) {
        const float unitsPerPixel = float(std::pow(10.0, exponent));
        const ViewGridLevels levels = viewGridLevels(unitsPerPixel);
        const double pixels = double(levels.fine) / double(unitsPerPixel);
        QVERIFY2(pixels > 0.99 && pixels <= 10.01, qPrintable(QString::number(pixels)));
        // The fine and the coarse level share the weight: as one fades out the other fades in.
        QVERIFY(std::abs(levels.fineWeight + levels.coarseWeight - 1.0f) < 1e-5f);
        QVERIFY(levels.fineWeight >= 0.0f && levels.coarseWeight >= 0.0f);
    }
}

void ViewGridTests::theDecadesSlideIntoEachOtherWithoutAJump()
{
    // Just either side of a decade the spacings, and the weights that show them, line up: what
    // was the middle decade (always full) is the finest one after, and the coarsest, fully
    // faded in, is the middle one.
    const float below = viewGridLevels(0.0999999f).fine;
    const float above = viewGridLevels(0.1000001f).fine;
    QVERIFY(std::abs(below * 10.0f - above) < 1e-3f * above);
    const ViewGridLevels b = viewGridLevels(0.0999999f);
    const ViewGridLevels a = viewGridLevels(0.1000001f);
    QVERIFY(b.coarseWeight > 0.999f);   // the coarsest, about to become the middle one
    QVERIFY(a.fineWeight > 0.999f);     // the finest, which was the middle one
    // Zooming out makes the same lines denser, never sparser.
    QVERIFY(viewGridLevels(1.0f).fine > viewGridLevels(0.001f).fine);
}

void ViewGridTests::theAxesHaveTheirUsualColours()
{
    // The style's own members, which are the grid out of the box; what a user has stored in their
    // settings is theirs and not this test's business.
    const ViewGridStyle style;
    const QVector3D x = style.axisColor[0], y = style.axisColor[1], z = style.axisColor[2];
    QVERIFY(x.x() > x.y() + 0.3f && x.x() > x.z() + 0.3f);   // red
    QVERIFY(y.y() > y.x() + 0.2f && y.y() > y.z() + 0.3f);   // green
    QVERIFY(z.z() > z.x() + 0.3f && z.z() > z.y() + 0.2f);   // blue
}

// The declared defaults of view.grid.* are kept equal to ViewGridStyle's own members, which are what
// the grid looks like out of the box, and the colour ones are "#rrggbb" strings that name the same
// colours. The declarations are compared, not the stored values: those are the user's.
void ViewGridTests::theStyleComesFromThePreferencesAndTheirDefaultsAreTheShadersOwn()
{
    const Preferences &preferences = Preferences::instance();
    const auto declared = [&](const char *id) {
        const MeshFilterParameterDescriptor *d = preferences.descriptor(QString::fromLatin1(id));
        return d ? d->defaultValue : QVariant();
    };
    const ViewGridStyle style;
    QVERIFY(std::abs(declared("view.grid.fineOpacity").toFloat() - style.fineOpacity) < 1e-5f);
    QVERIFY(std::abs(declared("view.grid.midOpacity").toFloat() - style.midOpacity) < 1e-5f);
    QVERIFY(std::abs(declared("view.grid.coarseOpacity").toFloat() - style.coarseOpacity) < 1e-5f);
    QVERIFY(std::abs(declared("view.grid.axisWidth").toFloat() - style.axisWidthPixels) < 1e-5f);
    QVERIFY(std::abs(declared("view.grid.extent").toFloat() - style.extent) < 1e-5f);
    const auto close = [](const QColor &c, const QVector3D &v) {
        return std::abs(float(c.redF()) - v.x()) < 0.01f && std::abs(float(c.greenF()) - v.y()) < 0.01f
            && std::abs(float(c.blueF()) - v.z()) < 0.01f;
    };
    QVERIFY(close(QColor(declared("view.grid.lineColor").toString()), style.lineColor));
    QVERIFY(close(QColor(declared("view.grid.axisXColor").toString()), style.axisColor[0]));
    QVERIFY(close(QColor(declared("view.grid.axisYColor").toString()), style.axisColor[1]));
    QVERIFY(close(QColor(declared("view.grid.axisZColor").toString()), style.axisColor[2]));
    QCOMPARE(declared("view.referenceGrid").toString(), QStringLiteral("none"));
}

QTEST_APPLESS_MAIN(ViewGridTests)
#include "test_viewgrid.moc"
