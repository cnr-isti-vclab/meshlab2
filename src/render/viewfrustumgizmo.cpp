#include "viewfrustumgizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

struct Corners
{
    QVector3D bl, br, tl, tr;
};

} // namespace

ViewFrustumGizmo buildViewFrustumGizmo(
    const QMatrix4x4 &view, const QMatrix4x4 &proj, float nearDist, float farDist)
{
    ViewFrustumGizmo gizmo;

    // An orthographic projection has a 1 where a perspective one has its -1 and 0, and its
    // proj(1,1) is 1/halfHeight, not a cotangent: the volume is a box rather than a pyramid.
    const bool orthographic = std::abs(proj(3, 3)) > 0.5f;
    // Perspective cannot start at or behind the eye. An orthographic volume can, and often
    // does, because it is fitted to the scene rather than to the eye.
    if (!std::isfinite(nearDist) || !std::isfinite(farDist) || !(farDist > nearDist)
        || (!orthographic && !(nearDist > 0.0f))) {
        return gizmo;
    }
    // proj(1,1) is cot(fovY/2) -- or 1/halfHeight -- and proj(0,0) is that over the aspect,
    // so both must be positive and finite for the volume to have a shape at all.
    const float cotHalfFovY = proj(1, 1);
    const float cotHalfFovX = proj(0, 0);
    if (!std::isfinite(cotHalfFovY) || !std::isfinite(cotHalfFovX)
        || cotHalfFovY <= 0.0f || cotHalfFovX <= 0.0f) {
        return gizmo;
    }

    const float tanHalfFovY = 1.0f / cotHalfFovY;
    const float aspect = cotHalfFovY / cotHalfFovX;

    // The camera basis, read out of the inverse view matrix: the translation column is the
    // eye, and the rotation columns are its axes in world space. A camera looks down its
    // own -Z, so forward is the negated third column.
    const QMatrix4x4 invView = view.inverted();
    const QVector3D apex(invView(0, 3), invView(1, 3), invView(2, 3));
    const QVector3D right(invView(0, 0), invView(1, 0), invView(2, 0));
    const QVector3D up(invView(0, 1), invView(1, 1), invView(2, 1));
    const QVector3D forward = -QVector3D(invView(0, 2), invView(1, 2), invView(2, 2));

    const auto cornersAt = [&](float distance) {
        // The rectangle grows with distance in a pyramid and stays the same in a box.
        const float halfH = orthographic ? tanHalfFovY : tanHalfFovY * distance;
        const float halfW = halfH * aspect;
        const QVector3D centre = apex + forward * distance;
        Corners c;
        c.bl = centre - right * halfW - up * halfH;
        c.br = centre + right * halfW - up * halfH;
        c.tl = centre - right * halfW + up * halfH;
        c.tr = centre + right * halfW + up * halfH;
        return c;
    };

    bool first = true;
    const auto grow = [&](const QVector3D &p) {
        if (first) {
            gizmo.boundsMin = p;
            gizmo.boundsMax = p;
            first = false;
            return;
        }
        gizmo.boundsMin.setX(std::min(gizmo.boundsMin.x(), p.x()));
        gizmo.boundsMin.setY(std::min(gizmo.boundsMin.y(), p.y()));
        gizmo.boundsMin.setZ(std::min(gizmo.boundsMin.z(), p.z()));
        gizmo.boundsMax.setX(std::max(gizmo.boundsMax.x(), p.x()));
        gizmo.boundsMax.setY(std::max(gizmo.boundsMax.y(), p.y()));
        gizmo.boundsMax.setZ(std::max(gizmo.boundsMax.z(), p.z()));
    };
    const auto segment = [&](const QVector3D &a, const QVector3D &b) {
        gizmo.vertices.insert(gizmo.vertices.end(),
                              { a.x(), a.y(), a.z(), b.x(), b.y(), b.z() });
        ++gizmo.segmentCount;
        grow(a);
        grow(b);
    };
    const auto rectangle = [&](const Corners &c) {
        segment(c.bl, c.br);
        segment(c.br, c.tr);
        segment(c.tr, c.tl);
        segment(c.tl, c.bl);
    };

    const Corners nearCorners = cornersAt(nearDist);
    const Corners farCorners = cornersAt(farDist);

    if (orthographic) {
        // A box: the two rectangles and the four parallel edges between them. There is no
        // apex, and no point of the glyph that marks the eye; the box is the whole camera.
        segment(nearCorners.bl, farCorners.bl);
        segment(nearCorners.br, farCorners.br);
        segment(nearCorners.tl, farCorners.tl);
        segment(nearCorners.tr, farCorners.tr);
    } else {
        // The body runs the whole depth of the frustum, apex to far plane, so the two
        // rectangles read as what they are: positions along an edge that is really there.
        segment(apex, farCorners.bl);
        segment(apex, farCorners.br);
        segment(apex, farCorners.tl);
        segment(apex, farCorners.tr);
    }
    rectangle(nearCorners);
    rectangle(farCorners);

    gizmo.valid = gizmo.segmentCount > 0;
    return gizmo;
}

float farthestDistanceInView(
    const QMatrix4x4 &view, const QVector3D &boundsMin, const QVector3D &boundsMax)
{
    // The eye looks down -Z in view space, so a point's distance in front of it is -z.
    // Eight corners rather than every point inside: the far plane only has to contain the
    // box, not hug it.
    float farthest = 0.0f;
    for (int corner = 0; corner < 8; ++corner) {
        const QVector3D p(
            (corner & 1) ? boundsMax.x() : boundsMin.x(),
            (corner & 2) ? boundsMax.y() : boundsMin.y(),
            (corner & 4) ? boundsMax.z() : boundsMin.z());
        farthest = std::max(farthest, -(view * p).z());
    }
    return farthest;
}

float nearestDistanceInView(
    const QMatrix4x4 &view, const QVector3D &boundsMin, const QVector3D &boundsMax)
{
    float nearest = std::numeric_limits<float>::infinity();
    for (int corner = 0; corner < 8; ++corner) {
        const QVector3D p(
            (corner & 1) ? boundsMax.x() : boundsMin.x(),
            (corner & 2) ? boundsMax.y() : boundsMin.y(),
            (corner & 4) ? boundsMax.z() : boundsMin.z());
        nearest = std::min(nearest, -(view * p).z());
    }
    return nearest;
}
