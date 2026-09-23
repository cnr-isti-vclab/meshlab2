#pragma once

#include "renderingsettings.h"

#include <QMatrix4x4>
#include <QVector3D>
#include <QVector4D>

#include <vector>

// The clipping plane, as the shaders want it.
//
// A plane is a vec4: xyz is the normal, w the offset, and a point p survives when
// dot(vec4(p, 1), plane) >= 0. An all-zero vec4 therefore keeps everything, which is what
// every pass writes when clipping is off -- no branch in the shader, no second pipeline.
//
// Extracted from RenderWidget so the arithmetic can be tested: RenderWidget lives in the
// application target and links nothing, as with viewfrustumgizmo.h.
namespace ClipPlane {

// Resolves the settings into a world-space plane.
//
// `sceneMin`/`sceneMax` are the visible scene's world bounding box; the offset is a
// fraction of its diagonal, so the same saved setting cuts a bunny and a building in the
// same place proportionally. `viewDirection` is where the camera looks, used only by
// `ClipPlaneAxis::View`.
//
// Returns a zero vec4 -- keep everything -- when clipping is off, when the chosen normal
// is degenerate, or when the scene has no extent to measure the offset against.
QVector4D world(
    const GlobalRenderSettings &settings,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax,
    const QVector3D &viewDirection);

// The same plane in a layer's own coordinates, which is what a vertex shader can test:
// it sees local positions, since `mvp` already carries the layer transform. A plane is a
// row vector, so it transforms by the transpose of the local-to-world matrix rather than
// by its inverse.
QVector4D toLocal(const QVector4D &worldPlane, const QMatrix4x4 &meshTransform);

// A wireframe for the plane itself: a bordered grid lying on it, sized to span the scene,
// plus a short stem along the normal marking the side that survives. Without it the cut
// has no visible cause -- the old near-plane trick's worst failing.
//
// Returns a line list, six floats per segment, in world space. Empty when there is no
// plane or no scene to size it against. `gridLines` is the number of interior lines each
// way; 0 gives the border and the stem alone.
std::vector<float> planeGizmo(
    const QVector4D &worldPlane,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax,
    int gridLines = 8);

} // namespace ClipPlane
