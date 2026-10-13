#include "viewgrid.h"

#include "preferences.h"

#include <algorithm>
#include <cmath>

ViewGridPlane chooseViewGridPlane(
    ReferenceGrid setting, const QVector3D &viewDirection, bool orthographic)
{
    ViewGridPlane plane;
    if (setting == ReferenceGrid::None)
        return plane;

    plane.valid = true;
    if (orthographic) {
        const QVector3D d = viewDirection.normalized();
        const float along[3] = { std::abs(d.x()), std::abs(d.y()), std::abs(d.z()) };
        for (int axis = 0; axis < 3; ++axis) {
            if (along[axis] >= kViewGridAlignedCosine) {
                // The plane across the line of sight: the other two axes, in their usual order.
                plane.normalAxis = axis;
                plane.uAxis = axis == 0 ? 1 : 0;
                plane.vAxis = axis == 2 ? 1 : 2;
                return plane;
            }
        }
    }
    if (setting == ReferenceGrid::XZ) {
        plane.uAxis = 0;
        plane.vAxis = 2;
        plane.normalAxis = 1;
    } else {
        plane.uAxis = 0;
        plane.vAxis = 1;
        plane.normalAxis = 2;
    }
    return plane;
}

ViewGridLevels viewGridLevels(float unitsPerPixel)
{
    // Ten pixels is one fine spacing (E = 0); with e its floor, 10^e is between one and ten
    // pixels, and the fraction f says how far through the decade the zoom is.
    const double u = std::max(double(unitsPerPixel), 1e-12);
    const double e10 = std::log10(10.0 * u);
    const double e = std::floor(e10);
    const double f = e10 - e;
    ViewGridLevels levels;
    levels.fine = float(std::pow(10.0, e));
    levels.fineWeight = float(1.0 - f);
    levels.coarseWeight = float(f);
    return levels;
}

namespace {

QVector3D rgb(const QColor &c)
{
    return QVector3D(float(c.redF()), float(c.greenF()), float(c.blueF()));
}

}

ViewGridStyle viewGridStyle()
{
    const Preferences &preferences = Preferences::instance();
    ViewGridStyle style;
    style.fineOpacity = float(preferences.doubleValue(QStringLiteral("view.grid.fineOpacity")));
    style.midOpacity = float(preferences.doubleValue(QStringLiteral("view.grid.midOpacity")));
    style.coarseOpacity = float(preferences.doubleValue(QStringLiteral("view.grid.coarseOpacity")));
    style.axisWidthPixels = float(preferences.doubleValue(QStringLiteral("view.grid.axisWidth")));
    style.extent = float(preferences.doubleValue(QStringLiteral("view.grid.extent")));
    style.lineColor = rgb(preferences.colorValue(QStringLiteral("view.grid.lineColor")));
    style.axisColor[0] = rgb(preferences.colorValue(QStringLiteral("view.grid.axisXColor")));
    style.axisColor[1] = rgb(preferences.colorValue(QStringLiteral("view.grid.axisYColor")));
    style.axisColor[2] = rgb(preferences.colorValue(QStringLiteral("view.grid.axisZColor")));
    return style;
}

ReferenceGrid defaultReferenceGrid()
{
    const QString name = Preferences::instance().stringValue(QStringLiteral("view.referenceGrid"));
    if (name == QLatin1String("xy"))
        return ReferenceGrid::XY;
    if (name == QLatin1String("xz"))
        return ReferenceGrid::XZ;
    return ReferenceGrid::None;
}
