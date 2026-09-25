#include "clipplane.h"

#include <algorithm>
#include <limits>
#include <cmath>

namespace ClipPlane {

QVector4D world(
    const GlobalRenderSettings &settings,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax,
    const QVector3D &viewDirection)
{
    if (!settings.clipPlaneEnabled)
        return QVector4D();

    QVector3D normal;
    switch (settings.clipPlaneAxis) {
    case ClipPlaneAxis::X:      normal = QVector3D(1.0f, 0.0f, 0.0f); break;
    case ClipPlaneAxis::Y:      normal = QVector3D(0.0f, 1.0f, 0.0f); break;
    case ClipPlaneAxis::Z:      normal = QVector3D(0.0f, 0.0f, 1.0f); break;
    case ClipPlaneAxis::View:   normal = viewDirection; break;
    case ClipPlaneAxis::Custom: normal = settings.clipPlaneCustomAxis; break;
    }
    const float length = normal.length();
    if (!std::isfinite(length) || length < 1e-6f)
        return QVector4D();
    normal /= length;

    const QVector3D extent = sceneMax - sceneMin;
    const float diagonal = extent.length();
    if (!std::isfinite(diagonal) || diagonal <= 0.0f)
        return QVector4D();

    QVector3D reference;
    switch (settings.clipPlaneRelativeTo) {
    case ClipPlaneReference::Origin: reference = QVector3D(); break;
    case ClipPlaneReference::Center: reference = 0.5f * (sceneMin + sceneMax); break;
    case ClipPlaneReference::Min:    reference = sceneMin; break;
    case ClipPlaneReference::Max:    reference = sceneMax; break;
    }

    // The offset is measured along the UNFLIPPED normal, and the flip is applied to the
    // finished plane. Folding the flip into the normal first would slide the cut to the
    // mirrored position as well as swapping sides, so asking for the other half of an
    // object would move the cut off it instead of showing the rest of it.
    const QVector3D onPlane = reference + normal * (settings.clipPlaneOffset * diagonal);
    const QVector4D plane(normal, -QVector3D::dotProduct(normal, onPlane));
    return settings.clipPlaneFlipped ? -plane : plane;
}

QVector4D toLocal(const QVector4D &worldPlane, const QMatrix4x4 &meshTransform)
{
    if (worldPlane.toVector3D().isNull())
        return QVector4D();
    return meshTransform.transposed() * worldPlane;
}

float offsetClearOfScene(
    const GlobalRenderSettings &settings,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax,
    const QVector3D &viewDirection)
{
    // Asked of the settings as they will be once enabled, so the caller can set the axis
    // it wants first and get the offset that matches.
    GlobalRenderSettings probe = settings;
    probe.clipPlaneEnabled = true;
    probe.clipPlaneOffset = 0.0f;
    const QVector4D plane = world(probe, sceneMin, sceneMax, viewDirection);
    if (plane.toVector3D().isNull())
        return 0.0f;

    const float diagonal = (sceneMax - sceneMin).length();
    if (!std::isfinite(diagonal) || diagonal <= 0.0f)
        return 0.0f;

    // How far the plane would have to move, along its own normal, for the deepest corner
    // of the box to be on the surviving side.
    float deepest = std::numeric_limits<float>::max();
    for (int corner = 0; corner < 8; ++corner) {
        const QVector3D p(
            (corner & 1) ? sceneMax.x() : sceneMin.x(),
            (corner & 2) ? sceneMax.y() : sceneMin.y(),
            (corner & 4) ? sceneMax.z() : sceneMin.z());
        deepest = std::min(deepest, QVector3D::dotProduct(plane.toVector3D(), p) + plane.w());
    }
    // A flip reverses which way the offset pushes the surviving side, so the sign follows.
    const float offset = deepest / diagonal;
    return settings.clipPlaneFlipped ? -offset : offset;
}

QVector3D rotateNormal(
    const QVector3D &normal,
    const QVector3D &cameraRight,
    const QVector3D &cameraUp,
    float dx,
    float dy,
    float degreesPerUnit)
{
    if (normal.isNull())
        return normal;

    // The axis that swings the normal the way the cursor went. Rotating about -up moves a
    // forward-facing normal towards +right, and about -right moves it towards -up, so the
    // plane follows the drag rather than running away from it.
    const QVector3D axis = -(cameraUp * dx + cameraRight * dy);
    const float magnitude = axis.length();
    if (!std::isfinite(magnitude) || magnitude < 1e-9f)
        return normal;

    const QQuaternion rotation =
        QQuaternion::fromAxisAndAngle(axis / magnitude, magnitude * degreesPerUnit);
    const QVector3D rotated = rotation.rotatedVector(normal);
    return rotated.isNull() ? normal : rotated.normalized() * normal.length();
}

namespace {

// The square on the plane that the gizmo draws and the cap fills.
struct PlaneFrame
{
    QVector3D centre;
    QVector3D u;
    QVector3D v;
    float half = 0.0f;
    float diagonal = 0.0f;
    bool valid = false;
};

PlaneFrame planeFrame(const QVector4D &worldPlane, const QVector3D &sceneMin, const QVector3D &sceneMax)
{
    PlaneFrame frame;
    const QVector3D normal = worldPlane.toVector3D();
    if (normal.isNull())
        return frame;

    const QVector3D extent = sceneMax - sceneMin;
    const float diagonal = extent.length();
    if (!std::isfinite(diagonal) || diagonal <= 0.0f)
        return frame;

    // The plane's own centre: the scene centre dropped onto it, so the square stays over
    // the object as the plane slides rather than drifting off with the offset.
    const QVector3D sceneCentre = 0.5f * (sceneMin + sceneMax);
    frame.centre =
        sceneCentre - normal * (QVector3D::dotProduct(normal, sceneCentre) + worldPlane.w());

    // Any two directions spanning the plane will do; picking the world axis least parallel
    // to the normal keeps the basis well conditioned whichever way the plane faces.
    const QVector3D absN(std::abs(normal.x()), std::abs(normal.y()), std::abs(normal.z()));
    QVector3D seed(0.0f, 0.0f, 1.0f);
    if (absN.x() <= absN.y() && absN.x() <= absN.z())
        seed = QVector3D(1.0f, 0.0f, 0.0f);
    else if (absN.y() <= absN.z())
        seed = QVector3D(0.0f, 1.0f, 0.0f);
    frame.u = QVector3D::crossProduct(normal, seed).normalized();
    frame.v = QVector3D::crossProduct(normal, frame.u).normalized();

    // Wider than the scene, so the grid reads as an unbounded plane rather than a card
    // floating inside the object -- and, for the cap, so it covers every point where the
    // plane can be inside anything: the scene's box meets the plane within half a diagonal
    // of this centre.
    frame.half = 0.6f * diagonal;
    frame.diagonal = diagonal;
    frame.valid = true;
    return frame;
}

} // namespace

std::vector<float> planeGizmo(
    const QVector4D &worldPlane,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax,
    int gridLines)
{
    std::vector<float> vertices;
    const PlaneFrame frame = planeFrame(worldPlane, sceneMin, sceneMax);
    if (!frame.valid)
        return vertices;
    const QVector3D normal = worldPlane.toVector3D();
    const QVector3D centre = frame.centre;
    const QVector3D u = frame.u;
    const QVector3D v = frame.v;
    const float half = frame.half;
    const float diagonal = frame.diagonal;

    const auto segment = [&vertices](const QVector3D &a, const QVector3D &b) {
        vertices.insert(vertices.end(), { a.x(), a.y(), a.z(), b.x(), b.y(), b.z() });
    };

    const QVector3D uu = u * half;
    const QVector3D vv = v * half;
    segment(centre - uu - vv, centre + uu - vv);
    segment(centre + uu - vv, centre + uu + vv);
    segment(centre + uu + vv, centre - uu + vv);
    segment(centre - uu + vv, centre - uu - vv);

    for (int i = 1; i <= std::max(0, gridLines); ++i) {
        const float t = float(i) / float(gridLines + 1) * 2.0f - 1.0f;
        segment(centre + u * (t * half) - vv, centre + u * (t * half) + vv);
        segment(centre + v * (t * half) - uu, centre + v * (t * half) + uu);
    }

    // The stem points at what is kept, which is the one thing a bare rectangle cannot say.
    segment(centre, centre + normal * (0.15f * diagonal));
    return vertices;
}

std::vector<float> capQuad(
    const QVector4D &worldPlane,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax)
{
    std::vector<float> vertices;
    const PlaneFrame frame = planeFrame(worldPlane, sceneMin, sceneMax);
    if (!frame.valid)
        return vertices;
    const QVector3D uu = frame.u * frame.half;
    const QVector3D vv = frame.v * frame.half;
    const QVector3D corners[4] = {
        frame.centre - uu - vv, frame.centre + uu - vv,
        frame.centre + uu + vv, frame.centre - uu + vv
    };
    for (int k : { 0, 1, 2, 0, 2, 3 })
        vertices.insert(vertices.end(), { corners[k].x(), corners[k].y(), corners[k].z() });
    return vertices;
}

} // namespace ClipPlane
