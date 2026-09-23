#include "clipplane.h"

#include <algorithm>
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

std::vector<float> planeGizmo(
    const QVector4D &worldPlane,
    const QVector3D &sceneMin,
    const QVector3D &sceneMax,
    int gridLines)
{
    std::vector<float> vertices;
    const QVector3D normal = worldPlane.toVector3D();
    if (normal.isNull())
        return vertices;

    const QVector3D extent = sceneMax - sceneMin;
    const float diagonal = extent.length();
    if (!std::isfinite(diagonal) || diagonal <= 0.0f)
        return vertices;

    // The plane's own centre: the scene centre dropped onto it, so the grid stays over the
    // object as the plane slides rather than drifting off with the offset.
    const QVector3D sceneCentre = 0.5f * (sceneMin + sceneMax);
    const QVector3D centre =
        sceneCentre - normal * (QVector3D::dotProduct(normal, sceneCentre) + worldPlane.w());

    // Any two directions spanning the plane will do; picking the world axis least parallel
    // to the normal keeps the basis well conditioned whichever way the plane faces.
    const QVector3D absN(std::abs(normal.x()), std::abs(normal.y()), std::abs(normal.z()));
    QVector3D seed(0.0f, 0.0f, 1.0f);
    if (absN.x() <= absN.y() && absN.x() <= absN.z())
        seed = QVector3D(1.0f, 0.0f, 0.0f);
    else if (absN.y() <= absN.z())
        seed = QVector3D(0.0f, 1.0f, 0.0f);
    const QVector3D u = QVector3D::crossProduct(normal, seed).normalized();
    const QVector3D v = QVector3D::crossProduct(normal, u).normalized();

    // Wider than the scene, so the grid reads as an unbounded plane rather than a card
    // floating inside the object.
    const float half = 0.6f * diagonal;

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

} // namespace ClipPlane
