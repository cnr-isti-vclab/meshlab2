#pragma once

#include "camerashot.h"
#include "floatbuffer.h"
#include "parallelfor.h"
#include "vcgmesh.h"

#include <QMatrix4x4>
#include <QVector2D>
#include <QVector3D>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

// Pre-computed per-vertex data shared between depth buffer building and
// visibility queries. Eliminates redundant transformPoint/project/depth calls
// (each vertex is projected ~6 times in rasterization + multiple times during
// visibility checks — with this cache it's done exactly once per raster).
struct DepthBufferVertexCache {
    std::vector<QVector2D> proj;      // screen-space pixel coords per vertex
    std::vector<float>     depth;     // view-space depth per vertex
    std::vector<char>      frontFace; // true if vertex normal faces camera

    void clear() { proj.clear(); depth.clear(); frontFace.clear(); }
};

inline QVector3D transformPoint(const QMatrix4x4 &m, const vcg::Point3f &p)
{
    return m.map(QVector3D(p[0], p[1], p[2]));
}

// The pixels a triangle covers: covered(index, depth, b0, b1, b2) is called for each pixel whose
// centre lies inside the triangle and in front of the camera, with the pixel's index, the
// depth there and its barycentric weights. The depth test is the caller's, so that it can
// keep its own buffer: a plain one, an atomic one shared by threads, or one per thread.
template <class Covered>
inline void rasterizeTriangle(
    int w, int h,
    const QVector2D &p0, const QVector2D &p1, const QVector2D &p2,
    float d0, float d1, float d2, Covered &&covered)
{
    // A triangle with a vertex at or behind the camera plane is skipped, not only one wholly
    // behind it: projecting such a vertex flips or explodes it, so the triangle's box covers
    // the whole image and its depths are wrong anyway. Drawing it right would need clipping
    // at a near plane; it only happens when a camera sits inside the model, and there it
    // turned a depth buffer from 0.2 s into a minute.
    if (d0 <= 0.0f || d1 <= 0.0f || d2 <= 0.0f) return;
    const float area = (p1.x() - p0.x()) * (p2.y() - p0.y()) -
                       (p1.y() - p0.y()) * (p2.x() - p0.x());
    if (std::abs(area) < 1e-8f) return;
    const float invArea = 1.0f / area;

    // The pixels whose centres, at k + 0.5, fall in the triangle's box, give or take 1e-3 px.
    // Rounding the box itself outward would add a row and a column on each side, most of what
    // is tested for a small triangle: 77 to 86% of the pixels tested were outside it.
    const float e = 1e-3f;
    const int ixMin = std::max(0,     int(std::ceil (std::min({p0.x(), p1.x(), p2.x()}) - 0.5f - e)));
    const int iyMin = std::max(0,     int(std::ceil (std::min({p0.y(), p1.y(), p2.y()}) - 0.5f - e)));
    const int ixMax = std::min(w - 1, int(std::floor(std::max({p0.x(), p1.x(), p2.x()}) - 0.5f + e)));
    const int iyMax = std::min(h - 1, int(std::floor(std::max({p0.y(), p1.y(), p2.y()}) - 0.5f + e)));
    if (ixMin > ixMax || iyMin > iyMax) return;

    for (int iy = iyMin; iy <= iyMax; ++iy) {
        for (int ix = ixMin; ix <= ixMax; ++ix) {
            const float px = float(ix) + 0.5f;
            const float py = float(iy) + 0.5f;
            const float f12 = (p2.x() - p1.x()) * (py - p1.y()) -
                              (p2.y() - p1.y()) * (px - p1.x());
            const float f20 = (p0.x() - p2.x()) * (py - p2.y()) -
                              (p0.y() - p2.y()) * (px - p2.x());

            const float bw0 = f12 * invArea;
            const float bw1 = f20 * invArea;
            const float bw2 = 1.0f - bw0 - bw1;
            if (bw0 < -1e-5f || bw1 < -1e-5f || bw2 < -1e-5f) continue;
            const float d = bw0 * d0 + bw1 * d1 + bw2 * d2;
            if (d <= 0.0f) continue;
            covered(size_t(iy) * size_t(w) + size_t(ix), d, bw0, bw1, bw2);
        }
    }
}

inline std::unique_ptr<FloatBuffer> buildDepthBuffer(
    const CameraShot &shot, const VCGMesh &mesh, const QMatrix4x4 &transform,
    DepthBufferVertexCache *outCache = nullptr)
{
    const int w = shot.viewportPx().width();
    const int h = shot.viewportPx().height();
    const int vn = mesh.VN();
    const float kMax = std::numeric_limits<float>::max();

    // Pre-compute projection + depth for all vertices in one pass
    // (MeshLab meshes are always compact — no deleted vertices)
    const size_t vertices = size_t(vn);
    std::vector<QVector2D> proj(vertices);
    std::vector<float> depths(vertices);
    std::vector<char> frontFace(vertices);
    parallelFor(vertices, [&](size_t first, size_t last) {
        for (size_t vi = first; vi < last; ++vi) {
            const auto &v = mesh.vert[vi];
            const QVector3D wv = transformPoint(transform, v.cP());
            proj[vi] = shot.project(wv);
            depths[vi] = shot.depth(wv);
            // face-normal check: dot(direction to the camera, vertexNormal) > 0 means front-facing
            const QVector3D n(v.cN()[0], v.cN()[1], v.cN()[2]);
            frontFace[vi] = QVector3D::dotProduct(shot.directionToCamera(wv), n.normalized()) > 0.0f;
        }
    });

    // Each pixel keeps the nearest depth of the faces that cover it, which does not depend on
    // the order they are drawn in: the faces are drawn in parallel into one buffer, lowered by
    // an atomic minimum, and the result is the serial one exactly.
    std::vector<std::atomic<float>> zbuf(size_t(w) * size_t(h));
    parallelFor(zbuf.size(), [&](size_t first, size_t last) {
        for (size_t i = first; i < last; ++i)
            zbuf[i].store(kMax, std::memory_order_relaxed);
    });
    parallelFor(size_t(mesh.FN()), [&](size_t first, size_t last) {
        for (size_t fi = first; fi < last; ++fi) {
            const auto &f = mesh.face[fi];
            const size_t i0 = size_t(vcg::tri::Index(mesh, f.cV(0)));
            const size_t i1 = size_t(vcg::tri::Index(mesh, f.cV(1)));
            const size_t i2 = size_t(vcg::tri::Index(mesh, f.cV(2)));
            rasterizeTriangle(w, h, proj[i0], proj[i1], proj[i2], depths[i0], depths[i1], depths[i2],
                              [&](size_t i, float d, float, float, float) {
                float z = zbuf[i].load(std::memory_order_relaxed);
                while (d < z && !zbuf[i].compare_exchange_weak(z, d, std::memory_order_relaxed)) {}
            });
        }
    }, 4096);

    auto buf = std::make_unique<FloatBuffer>();
    buf->init(w, h);
    parallelFor(zbuf.size(), [&](size_t first, size_t last) {
        for (size_t i = first; i < last; ++i) {
            const float v = zbuf[i].load(std::memory_order_relaxed);
            buf->data[i] = (v == kMax) ? 0.0f : v;
        }
    });

    // Return vertex cache if requested
    if (outCache) {
        outCache->proj      = std::move(proj);
        outCache->depth     = std::move(depths);
        outCache->frontFace = std::move(frontFace);
    }
    return buf;
}
