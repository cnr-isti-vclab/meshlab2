#pragma once

#include "camerashot.h"
#include "floatbuffer.h"
#include "vcgmesh.h"

#include <QMatrix4x4>
#include <QVector2D>
#include <QVector3D>

#include <algorithm>
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
    std::vector<bool>      frontFace; // true if vertex normal faces camera

    void clear() { proj.clear(); depth.clear(); frontFace.clear(); }
};

inline QVector3D transformPoint(const QMatrix4x4 &m, const vcg::Point3f &p)
{
    return m.map(QVector3D(p[0], p[1], p[2]));
}

inline void rasterizeTriangleDepth(
    std::vector<float> &zbuf, int w, int h,
    const QVector2D &p0, const QVector2D &p1, const QVector2D &p2,
    float d0, float d1, float d2)
{
    if (d0 <= 0.0f && d1 <= 0.0f && d2 <= 0.0f) return;
    const float area = (p1.x() - p0.x()) * (p2.y() - p0.y()) -
                       (p1.y() - p0.y()) * (p2.x() - p0.x());
    if (std::abs(area) < 1e-8f) return;
    const float invArea = 1.0f / area;

    const int ixMin = std::max(0,     int(std::floor(std::min({p0.x(), p1.x(), p2.x()}))));
    const int iyMin = std::max(0,     int(std::floor(std::min({p0.y(), p1.y(), p2.y()}))));
    const int ixMax = std::min(w - 1, int(std::ceil (std::max({p0.x(), p1.x(), p2.x()}))));
    const int iyMax = std::min(h - 1, int(std::ceil (std::max({p0.y(), p1.y(), p2.y()}))));
    if (ixMin > ixMax || iyMin > iyMax) return;

    for (int iy = iyMin; iy <= iyMax; ++iy) {
        for (int ix = ixMin; ix <= ixMax; ++ix) {
            const float px = float(ix) + 0.5f;
            const float py = float(iy) + 0.5f;
            const float f12 = (p2.x() - p1.x()) * (py - p1.y()) -
                              (p2.y() - p1.y()) * (px - p1.x());
            const float f20 = (p0.x() - p2.x()) * (py - p2.y()) -
                              (p0.y() - p2.y()) * (px - p2.x());
            const float f01 = area - f12 - f20;

            const float bw0 = f12 * invArea;
            const float bw1 = f20 * invArea;
            const float bw2 = 1.0f - bw0 - bw1;
            if (bw0 < -1e-5f || bw1 < -1e-5f || bw2 < -1e-5f) continue;
            const float d = bw0 * d0 + bw1 * d1 + bw2 * d2;
            if (d <= 0.0f) continue;
            float &z = zbuf[size_t(iy * w + ix)];
            if (d < z) z = d;
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
    std::vector<float> zbuf(size_t(w * h), kMax);

    // Pre-compute projection + depth for all vertices in one pass
    // (MeshLab meshes are always compact — no deleted vertices)
    std::vector<QVector2D> proj;
    std::vector<float> depths;
    std::vector<bool> frontFace;
    proj.resize(size_t(vn));
    depths.resize(size_t(vn));
    frontFace.resize(size_t(vn));
    const QVector3D viewPt = shot.viewPoint();
    const QVector3D viewDir = shot.referenceAxis(2); // camera forward = into scene
    for (int vi = 0; vi < vn; ++vi) {
        const auto &v = mesh.vert[size_t(vi)];
        const QVector3D wv = transformPoint(transform, v.cP());
        proj[size_t(vi)] = shot.project(wv);
        depths[size_t(vi)] = shot.depth(wv);
        // face-normal check: dot(viewPt-vertex, vertexNormal) > 0 means front-facing
        const QVector3D n(v.cN()[0], v.cN()[1], v.cN()[2]);
        frontFace[size_t(vi)] = QVector3D::dotProduct((viewPt - wv).normalized(), n.normalized()) > 0.0f;
    }

    // Rasterize faces using pre-computed per-vertex data
    const int fn = mesh.FN();
    for (int fi = 0; fi < fn; ++fi) {
        const auto &f = mesh.face[size_t(fi)];
        const int i0 = vcg::tri::Index(mesh, f.V(0));
        const int i1 = vcg::tri::Index(mesh, f.V(1));
        const int i2 = vcg::tri::Index(mesh, f.V(2));
        const float d0 = depths[size_t(i0)];
        const float d1 = depths[size_t(i1)];
        const float d2 = depths[size_t(i2)];
        if (d0 <= 0.0f && d1 <= 0.0f && d2 <= 0.0f) continue;
        rasterizeTriangleDepth(zbuf, w, h,
            proj[size_t(i0)], proj[size_t(i1)], proj[size_t(i2)], d0, d1, d2);
    }

    // Write directly to FloatBuffer (single pass)
    auto buf = std::make_unique<FloatBuffer>();
    buf->init(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float v = zbuf[size_t(y * w + x)];
            buf->setval(x, y, (v == kMax) ? 0.0f : v);
        }

    // Return vertex cache if requested
    if (outCache) {
        outCache->proj      = std::move(proj);
        outCache->depth     = std::move(depths);
        outCache->frontFace = std::move(frontFace);
    }
    return buf;
}
