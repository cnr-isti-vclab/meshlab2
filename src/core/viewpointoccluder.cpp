#include "viewpointoccluder.h"

#include <vcg/complex/algorithms/closest.h>
#include <vcg/space/index/grid_static_ptr.h>
#include <vcg/space/ray3.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>

#if MESHLAB2_EMBREE_ENABLED
#include <wrap/embree/EmbreeAdaptor.h>
#endif

namespace {

// Robust ray/triangle intersection for the vcglib grid backend. vcglib's stock
// RayTriangleIntersectionFunctor runs Moller-Trumbore in the mesh scalar type
// (float): a ray from the eye to a HIDDEN face crosses the occluder near its
// silhouette, i.e. at a grazing angle, where the float determinant and
// barycentric coordinates lose almost all precision — the front hit is then
// miscomputed and rejected, the ray leaks through, and the hidden face is
// wrongly reported visible. Doing the arithmetic in double fixes the precision
// loss; the degenerate cutoff is relative to the triangle size and a small
// barycentric tolerance lets a ray grazing a shared edge be caught by both
// adjacent triangles. Two-sided. (Embree, when built, avoids all of this.)
struct RobustRayFaceIsectFunctor
{
    template <class FaceType, class ScalarType>
    bool operator()(const FaceType &f, const vcg::Ray3<ScalarType> &ray, ScalarType &t) const
    {
        using P3 = vcg::Point3d;
        const P3 v0 = P3::Construct(f.cP(0));
        const P3 e1 = P3::Construct(f.cP(1)) - v0;
        const P3 e2 = P3::Construct(f.cP(2)) - v0;
        const P3 dir = P3::Construct(ray.Direction());
        const P3 org = P3::Construct(ray.Origin());
        const P3 pvec = dir ^ e2;
        const double det = e1 * pvec;
        if (std::fabs(det) <= e1.Norm() * e2.Norm() * 1e-15)
            return false;
        const double invDet = 1.0 / det;
        const double eps = 1e-7;
        const P3 tvec = org - v0;
        const double u = (tvec * pvec) * invDet;
        if (u < -eps || u > 1.0 + eps)
            return false;
        const P3 qvec = tvec ^ e1;
        const double v = (dir * qvec) * invDet;
        if (v < -eps || u + v > 1.0 + eps)
            return false;
        const double tt = (e2 * qvec) * invDet;
        if (tt < 0.0)
            return false;
        t = ScalarType(tt);
        return true;
    }
};

// Size-1 cache keyed by (mesh, geometryRevision): a rubber-band drag re-runs the
// selection filter repeatedly on the same mesh, and rebuilding the acceleration
// structure each time dominates the cost. Guarded because queries fan out to
// worker threads, but getOrBuild itself is only entered single-threaded.
std::mutex g_cacheMutex;
std::shared_ptr<ViewpointOccluder> g_cached;
VCGMesh *g_cachedMesh = nullptr;
std::uint64_t g_cachedRev = ~std::uint64_t(0);

} // namespace

struct ViewpointOccluder::Impl
{
#if MESHLAB2_EMBREE_ENABLED
    std::unique_ptr<vcg::EmbreeAdaptor<VCGMesh>> embree;
#endif
    vcg::GridStaticPtr<VCGFace, VCGMesh::ScalarType> grid;
    float occlEps = 0.0f;
};

ViewpointOccluder::ViewpointOccluder(VCGMesh &mesh)
    : d(std::make_unique<Impl>())
{
    d->occlEps = mesh.bbox.IsNull() ? 0.0f : float(mesh.bbox.Diag()) * 1e-4f;
#if MESHLAB2_EMBREE_ENABLED
    // Builds the embree scene once (copies vertex/index buffers) and keeps it.
    // preprocess=false: a visibility query is read-only, so leave the mesh's
    // normals/bbox/flags untouched.
    d->embree = std::make_unique<vcg::EmbreeAdaptor<VCGMesh>>(mesh, /*preprocess=*/false);
#else
    d->grid.Set(mesh.face.begin(), mesh.face.end());
#endif
}

ViewpointOccluder::~ViewpointOccluder() = default;

std::shared_ptr<ViewpointOccluder> ViewpointOccluder::getOrBuild(VCGMesh &mesh,
                                                                 std::uint64_t geometryRevision)
{
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    if (g_cached && g_cachedMesh == &mesh && g_cachedRev == geometryRevision)
        return g_cached;
    g_cached = std::shared_ptr<ViewpointOccluder>(new ViewpointOccluder(mesh));
    g_cachedMesh = &mesh;
    g_cachedRev = geometryRevision;
    return g_cached;
}

bool ViewpointOccluder::usesEmbree() const
{
#if MESHLAB2_EMBREE_ENABLED
    return d->embree != nullptr;
#else
    return false;
#endif
}

bool ViewpointOccluder::isOccluded(const vcg::Point3f &sample, const vcg::Point3f &eye) const
{
#if MESHLAB2_EMBREE_ENABLED
    if (d->embree)
        return !d->embree->segmentUnoccluded(eye, sample);
#endif
    vcg::Point3f dir = sample - eye;
    const float dist = dir.Norm();
    if (dist <= 1e-9f)
        return false;
    vcg::Ray3f ray(eye, dir);
    ray.Normalize();
    RobustRayFaceIsectFunctor rayFunctor;
    vcg::tri::EmptyTMark<VCGMesh> marker;
    float t = 0.0f;
    const VCGFace *hit =
        d->grid.DoRay(rayFunctor, marker, ray, std::numeric_limits<float>::max(), t);
    if (!hit)
        return false;
    const float tol = std::max(dist * 1e-3f, d->occlEps);
    return t < dist - tol; // a surface meaningfully closer than the sample
}
