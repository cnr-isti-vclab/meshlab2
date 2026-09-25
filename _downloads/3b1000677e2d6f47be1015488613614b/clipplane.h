#pragma once

#include "renderingsettings.h"

#include <QMatrix4x4>
#include <QQuaternion>
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

// The offset at which the plane just touches the scene without cutting any of it, in the
// same diagonal fractions `GlobalRenderSettings::clipPlaneOffset` uses.
//
// Turning clipping on at this value means the very next notch of the wheel starts the cut
// at the surface nearest the camera. Guessing a safe value instead -- half a diagonal back,
// say -- leaves a stretch of offsets where the gesture visibly does nothing, because a
// box's diagonal is longer than its extent along any one direction.
float offsetClearOfScene(
    const GlobalRenderSettings &settings,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax,
    const QVector3D &viewDirection);

// Tips a world-space plane normal by a screen drag, as if the cursor had hold of the
// plane: dragging right swings the normal towards the camera's right, dragging down
// swings it towards the camera's floor. `dx`/`dy` are the drag in units of half the
// viewport's short side, `degreesPerUnit` how far a full such drag turns it.
//
// Returns the input unchanged when there is nothing to rotate or the basis is degenerate.
QVector3D rotateNormal(
    const QVector3D &normal,
    const QVector3D &cameraRight,
    const QVector3D &cameraUp,
    float dx,
    float dy,
    float degreesPerUnit = 90.0f);

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
