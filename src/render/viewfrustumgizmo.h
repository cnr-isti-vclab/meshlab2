#pragma once

#include <QMatrix4x4>
#include <QVector3D>

#include <vector>

// The wireframe that shows one 3D view where another view's camera is.
//
// It is the frustum itself and nothing else: the apex at the eye, the four edges running
// out to the far plane, and a rectangle on each of the near and far planes at the exact
// distances that view clips at. An earlier version drew the body out to an invented
// "reference depth" of 12% of the scene diagonal, which read as the near plane and was
// not one -- with the default clip ratio the real near plane sits at about 1/300 of the
// distance to the target, so the two were nowhere near each other.
//
// Extracted from RenderWidget so it can be tested: RenderWidget lives in the application
// target and links nothing, in the same way viewgridlayout.h was extracted.
struct ViewFrustumGizmo
{
    // Line list: six floats per segment, two xyz endpoints.
    std::vector<float> vertices;
    int segmentCount = 0;
    // World-space bounds of every point above. The host view needs these to choose a
    // depth range that contains the gizmo, or it clips the very thing it is drawing.
    QVector3D boundsMin;
    QVector3D boundsMax;
    bool valid = false;
};

// `view` and `proj` are the other view's matrices; `nearDist` and `farDist` are the
// distances it really clips at, which the caller reads from that view rather than
// recovering from `proj`, since a projection built for an infinite far plane carries no
// usable far distance. The field of view and aspect do come from `proj`.
//
// Returns an invalid gizmo when the projection is not a finite perspective one or the
// distances are not a sane near < far pair.
ViewFrustumGizmo buildViewFrustumGizmo(
    const QMatrix4x4 &view, const QMatrix4x4 &proj, float nearDist, float farDist);

// How far in front of `view`'s eye the farthest corner of the world-space box
// [boundsMin, boundsMax] lies. The view drawing a gizmo needs its far plane at least this
// far out or it clips the gizmo away; a peer frustum routinely reaches past the far plane
// a view would choose for its own scene. Never negative: a box entirely behind the eye
// asks for nothing.
float farthestDistanceInView(
    const QMatrix4x4 &view, const QVector3D &boundsMin, const QVector3D &boundsMax);
