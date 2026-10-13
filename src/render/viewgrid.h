#pragma once

#include "renderingsettings.h"

#include <QVector3D>

// The reference grid of a 3D view: a plane through the world origin ruled in powers of ten, with
// the two world axes lying in it drawn in their colours (X red, Y green, Z blue). It tells where
// the origin is and how big things are. What is arithmetic here -- which plane, how fine, how
// large -- is separated from RenderWidget so that it can be tested; the drawing is
// shaders/grid.frag.

// A coordinate plane: the two world axes that span it (as indices, 0 = X, 1 = Y, 2 = Z) and the
// one that is its normal.
struct ViewGridPlane {
    bool valid = false;
    int uAxis = 0;
    int vAxis = 1;
    int normalAxis = 2;
};

// Which plane the grid is drawn on. The setting names one (XY or XZ) for ordinary views. An
// orthographic view looking straight down a coordinate axis -- front, side, top, which is what
// the numeric views and the axis gizmo give -- always gets the plane across its line of sight,
// whatever the setting says: the chosen one may be seen edge-on, and an orthographic view with
// no grid in it has nothing to measure with. Invalid for ReferenceGrid::None.
ViewGridPlane chooseViewGridPlane(
    ReferenceGrid setting, const QVector3D &viewDirection, bool orthographic);

// How far from an axis the view direction may be, as the cosine of the angle, and still count
// as looking down it.
constexpr float kViewGridAlignedCosine = 0.9998f;

// The line spacings for a view in which one pixel is `unitsPerPixel` world units wide (at the
// orbit centre). Three decades are drawn, each ten times the last: `fine`, which is between 1
// and 10 pixels apart, `mid`, between 10 and 100, and `coarse`, between 100 and 1000. As the
// view zooms they slide through: the finest fades out as it gets too dense to read and the
// coarsest fades in as it becomes sparse enough to, so a decade never pops in or out. `fineWeight`
// and `coarseWeight` are those fades, 0 to 1; the middle decade is always drawn in full.
struct ViewGridLevels {
    float fine = 1.0f;
    float fineWeight = 1.0f;
    float coarseWeight = 0.0f;
};
ViewGridLevels viewGridLevels(float unitsPerPixel);

// How the grid looks, from the preferences (view.grid.*): how strongly each decade of lines is
// drawn, how wide the axis lines are, how far the grid reaches relative to the default, and the
// colours. Read each frame, so a change in the dialog shows at once.
struct ViewGridStyle {
    float fineOpacity = 0.4f;
    float midOpacity = 0.8f;
    float coarseOpacity = 1.5f;
    float axisWidthPixels = 2.0f;
    float extent = 1.0f;
    QVector3D lineColor = QVector3D(0.62f, 0.62f, 0.62f);
    QVector3D axisColor[3] = { QVector3D(0.93f, 0.25f, 0.30f),   // X, red
                               QVector3D(0.50f, 0.80f, 0.25f),   // Y, green
                               QVector3D(0.27f, 0.50f, 0.95f) }; // Z, blue
};
ViewGridStyle viewGridStyle();

// The reference grid a new view starts with, from the preferences (view.referenceGrid).
ReferenceGrid defaultReferenceGrid();
