#include "cgalfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <CGAL/Advancing_front_surface_reconstruction.h>
#include <CGAL/Alpha_shape_3.h>
#include <CGAL/Alpha_shape_cell_base_3.h>
#include <CGAL/Alpha_shape_vertex_base_3.h>
#include <CGAL/Scale_space_reconstruction_3/Advancing_front_mesher.h>
#include <CGAL/Scale_space_reconstruction_3/Alpha_shape_mesher.h>
#include <CGAL/Scale_space_reconstruction_3/Weighted_PCA_smoother.h>
#include <CGAL/Scale_space_surface_reconstruction_3.h>
#include <CGAL/Delaunay_triangulation_3.h>
#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Kinetic_surface_reconstruction_3.h>
#include <CGAL/Point_set_3.h>
#include <CGAL/Surface_mesh.h>
#include <CGAL/compute_average_spacing.h>
#include <CGAL/mst_orient_normals.h>
#include <CGAL/optimal_bounding_box.h>
#include <CGAL/poisson_surface_reconstruction.h>
#include <CGAL/property_map.h>
#include <CGAL/Triangulation_data_structure_3.h>
#include <CGAL/Triangulation_vertex_base_with_info_3.h>
#include <CGAL/alpha_wrap_3.h>
#include <CGAL/boost/graph/iterator.h>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>
#include <QObject>
#include <QStringList>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <exception>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace {

constexpr QLatin1StringView kFilterAlphaWrap("reconstruct_surface_by_alpha_wrapping");
constexpr QLatin1StringView kFilterAlphaShape("reconstruct_surface_by_alpha_shape");
constexpr QLatin1StringView kFilterVoronoiFiltering("reconstruct_surface_by_voronoi_filtering");
constexpr QLatin1StringView kFilterScaleSpace("reconstruct_surface_by_scale_space");
constexpr QLatin1StringView kFilterAdvancingFront("reconstruct_surface_by_advancing_front");
constexpr QLatin1StringView kFilterOrientNormals("orient_point_cloud_normals");
constexpr QLatin1StringView kFilterPoisson("reconstruct_surface_by_poisson_cgal");
constexpr QLatin1StringView kFilterKinetic("reconstruct_surface_by_kinetic_partition");
constexpr QLatin1StringView kFilterMeshBoundingBox("create_mesh_bounding_box");
constexpr QLatin1StringView kFilterSceneBoundingBox("create_scene_bounding_box");
using Mask = vcg::tri::io::Mask;
using Kernel = CGAL::Exact_predicates_inexact_constructions_kernel;
using CgalPoint = Kernel::Point_3;
using CgalVector = Kernel::Vector_3;
using CgalMesh = CGAL::Surface_mesh<CgalPoint>;
using Triangle = std::array<std::size_t, 3>;

// An oriented point that remembers where it came from. mst_orient_normals *reorders* its
// input — it packs the successfully oriented points first — so the source vertex index
// has to travel with the point, or the normals cannot be written back.
using OrientedPoint = std::tuple<CgalPoint, CgalVector, std::size_t>;
using OrientedPointMap = CGAL::Nth_of_tuple_property_map<0, OrientedPoint>;
using OrientedNormalMap = CGAL::Nth_of_tuple_property_map<1, OrientedPoint>;

// The kinetic pipeline needs a range it can introspect rather than one described by
// property maps — see runKinetic.
using CgalPointSet = CGAL::Point_set_3<CgalPoint, CgalVector>;

// Alpha shapes need their own triangulation types: the cell and vertex bases carry the
// per-simplex alpha intervals that classify each simplex as the alpha value sweeps.
using AsVertexBase = CGAL::Alpha_shape_vertex_base_3<Kernel>;
using AsCellBase = CGAL::Alpha_shape_cell_base_3<Kernel>;
using AsTds = CGAL::Triangulation_data_structure_3<AsVertexBase, AsCellBase>;
using AsTriangulation = CGAL::Delaunay_triangulation_3<Kernel, AsTds>;
using AlphaShape = CGAL::Alpha_shape_3<AsTriangulation>;

// Scale space: smooth the point set, then mesh it with either of CGAL's two meshers.
using ScaleSpace = CGAL::Scale_space_surface_reconstruction_3<Kernel>;
using ScaleSpaceSmoother = CGAL::Scale_space_reconstruction_3::Weighted_PCA_smoother<Kernel>;
using ScaleSpaceAlphaMesher = CGAL::Scale_space_reconstruction_3::Alpha_shape_mesher<Kernel>;
using ScaleSpaceAdvancingFrontMesher =
    CGAL::Scale_space_reconstruction_3::Advancing_front_mesher<Kernel>;

// Plain Delaunay for the pole-finding pass, and an info-carrying one for the second pass
// where sample points and poles must be told apart.
using PlainDelaunay = CGAL::Delaunay_triangulation_3<Kernel>;
using CrustVertexBase = CGAL::Triangulation_vertex_base_with_info_3<bool, Kernel>;
using CrustTds = CGAL::Triangulation_data_structure_3<CrustVertexBase>;
using CrustDelaunay = CGAL::Delaunay_triangulation_3<Kernel, CrustTds>;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = message;
    return result;
}

MeshFilterRunResult success(const QStringList &info, int newMeshIndex)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    if (newMeshIndex >= 0)
        result.newMeshIndices.push_back(newMeshIndex);
    return result;
}

bool buildTriangleSoup(const VCGMesh &mesh,
                       std::vector<CgalPoint> &points,
                       std::vector<Triangle> &triangles,
                       int &skippedFaces,
                       QString &error)
{
    points.clear();
    triangles.clear();
    skippedFaces = 0;

    std::vector<std::size_t> vertexMap(mesh.vert.size(), std::size_t(-1));
    points.reserve(std::max(0, mesh.VN()));
    for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
        const VCGVertex &v = mesh.vert[i];
        if (v.IsD())
            continue;
        vertexMap[i] = points.size();
        points.emplace_back(v.cP().X(), v.cP().Y(), v.cP().Z());
    }

    const VCGVertex *vertexBase = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    triangles.reserve(std::max(0, mesh.FN()));
    for (const VCGFace &face : mesh.face) {
        if (face.IsD())
            continue;

        Triangle tri{};
        bool valid = true;
        for (int k = 0; k < 3; ++k) {
            const VCGVertex *v = face.cV(k);
            if (!v || !vertexBase) {
                valid = false;
                break;
            }
            const ptrdiff_t rawIndex = v - vertexBase;
            if (rawIndex < 0 || std::size_t(rawIndex) >= vertexMap.size()
                || vertexMap[std::size_t(rawIndex)] == std::size_t(-1)) {
                valid = false;
                break;
            }
            tri[std::size_t(k)] = vertexMap[std::size_t(rawIndex)];
        }

        if (!valid) {
            ++skippedFaces;
            continue;
        }
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[2] == tri[0]) {
            ++skippedFaces;
            continue;
        }
        triangles.push_back(tri);
    }

    if (points.empty()) {
        error = QObject::tr("Alpha Wrap requires at least one valid vertex.");
        return false;
    }
    // An empty triangle list is allowed: the caller then wraps the bare point set.
    return true;
}

bool copyCgalMeshToVcg(const CgalMesh &source, VCGMesh &target, QString &error)
{
    target.Clear();
    if (source.number_of_vertices() == 0 || source.number_of_faces() == 0) {
        error = QObject::tr("CGAL Alpha Wrap produced an empty mesh.");
        return false;
    }

    std::vector<int> vertexMap(source.number_of_vertices(), -1);
    vcg::tri::Allocator<VCGMesh>::AddVertices(target, int(source.number_of_vertices()));
    int dstIndex = 0;
    for (CgalMesh::Vertex_index v : source.vertices()) {
        const CgalPoint &p = source.point(v);
        target.vert[std::size_t(dstIndex)].P() = vcg::Point3f(float(p.x()), float(p.y()), float(p.z()));
        if (std::size_t(v.idx()) >= vertexMap.size()) {
            error = QObject::tr("CGAL Alpha Wrap returned a mesh with unexpected vertex indexing.");
            return false;
        }
        vertexMap[std::size_t(v.idx())] = dstIndex;
        ++dstIndex;
    }

    for (CgalMesh::Face_index f : source.faces()) {
        std::array<int, 3> faceVertices{};
        int count = 0;
        for (CgalMesh::Vertex_index v : CGAL::vertices_around_face(source.halfedge(f), source)) {
            if (count >= 3) {
                error = QObject::tr("CGAL Alpha Wrap returned a non-triangular face.");
                return false;
            }
            const std::size_t raw = std::size_t(v.idx());
            if (raw >= vertexMap.size() || vertexMap[raw] < 0) {
                error = QObject::tr("CGAL Alpha Wrap returned an invalid face vertex reference.");
                return false;
            }
            faceVertices[std::size_t(count)] = vertexMap[raw];
            ++count;
        }
        if (count != 3) {
            error = QObject::tr("CGAL Alpha Wrap returned a degenerate face.");
            return false;
        }
        vcg::tri::Allocator<VCGMesh>::AddFace(
            target,
            faceVertices[0],
            faceVertices[1],
            faceVertices[2]);
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(target);
    vcg::tri::UpdateBounding<VCGMesh>::Box(target);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(target);
    return true;
}

// Alpha complex / alpha shape of the current layer's vertices.
//
// CGAL's alpha values are *squared* radii, so the user-facing radius is squared on the
// way in. The facet classification is what separates the two outputs: REGULAR facets are
// on the boundary of the alpha complex — the alpha shape proper — while SINGULAR facets
// are the lower-dimensional bits the complex keeps and the shape drops.
MeshFilterRunResult runAlphaShape(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    const VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() < 4)
        return fail(QObject::tr("An alpha shape needs at least 4 vertices."));

    const double alpha = params.getDouble(QStringLiteral("alpha"), 0.0);
    if (!std::isfinite(alpha) || alpha <= 0.0)
        return fail(QObject::tr("Alpha must be a finite value larger than zero."));
    const bool wantComplex = params.getEnum(QStringLiteral("output")) == QStringLiteral("complex");

    std::vector<CgalPoint> points;
    points.reserve(std::size_t(std::max(0, mesh.VN())));
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        points.emplace_back(v.cP().X(), v.cP().Y(), v.cP().Z());
    }
    if (points.size() < 4)
        return fail(QObject::tr("An alpha shape needs at least 4 live vertices."));

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Alpha Shape"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(10, "Building Delaunay triangulation...");

    // Everything that touches a Cell_handle must stay inside this scope: the handles point
    // into `shape`'s compact container, so consuming them after it dies is a use-after-free.
    VCGMesh output;
    try {
        AlphaShape shape(points.begin(), points.end(), Kernel::FT(alpha * alpha),
                         AlphaShape::GENERAL);
        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(60, "Classifying facets...");

        std::vector<AlphaShape::Facet> facets;
        shape.get_alpha_shape_facets(std::back_inserter(facets), AlphaShape::REGULAR);
        if (wantComplex)
            shape.get_alpha_shape_facets(std::back_inserter(facets), AlphaShape::SINGULAR);

        if (facets.empty()) {
            const QString message = QObject::tr(
                "The alpha shape is empty at alpha = %1. Try a larger value.")
                    .arg(QString::number(alpha, 'g', 6));
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(85, "Building output mesh...");

        std::unordered_map<const void *, int> vertexIndex;
        for (const AlphaShape::Facet &facet : facets) {
            const AlphaShape::Cell_handle cell = facet.first;
            const int opposite = facet.second;
            int corner[3];
            bool allFinite = true;
            for (int k = 0; k < 3 && allFinite; ++k) {
                auto handle = cell->vertex((opposite + k + 1) % 4);
                if (shape.is_infinite(handle)) {
                    allFinite = false;
                    break;
                }
                const void *key = &*handle;
                auto it = vertexIndex.find(key);
                if (it == vertexIndex.end()) {
                    const CgalPoint &p = handle->point();
                    auto vi = vcg::tri::Allocator<VCGMesh>::AddVertex(
                        output, vcg::Point3f(float(p.x()), float(p.y()), float(p.z())));
                    it = vertexIndex.emplace(key, int(vcg::tri::Index(output, *vi))).first;
                }
                corner[k] = it->second;
            }
            if (!allFinite)
                continue;
            if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2])
                continue;
            vcg::tri::Allocator<VCGMesh>::AddFace(output, corner[0], corner[1], corner[2]);
        }
    } catch (const std::exception &e) {
        const QString message =
            QObject::tr("CGAL alpha shape failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    if (output.FN() <= 0) {
        const QString message = QObject::tr("The alpha shape produced no faces.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    // MeshLab stores the facet circumradius in face quality; keep that, it is what makes
    // the result explorable with the scalar histogram.
    for (VCGFace &f : output.face) {
        const float a = (f.cV(1)->cP() - f.cV(0)->cP()).Norm();
        const float b = (f.cV(2)->cP() - f.cV(1)->cP()).Norm();
        const float c = (f.cV(0)->cP() - f.cV(2)->cP()).Norm();
        const float area = vcg::DoubleArea(f) * 0.5f;
        f.Q() = (area > std::numeric_limits<float>::epsilon()) ? (a * b * c) / (4.0f * area) : 0.0f;
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(output);
    vcg::tri::UpdateBounding<VCGMesh>::Box(output);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);

    const int ioMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL
        | Mask::IOM_FACEQUALITY;
    const int newIndex = doc.addMesh(
        output,
        {},
        ioMask);
    if (newIndex < 0) {
        const QString message = QObject::tr("Failed to add the alpha shape layer.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    doc.finishFilterProgress(true, QObject::tr("Generated alpha shape."));

    QStringList info;
    info << QObject::tr("Alpha: %1").arg(QString::number(alpha, 'g', 6))
         << QObject::tr("Input: %1 points.").arg(points.size())
         << QObject::tr("Output mesh: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    MeshFilterRunResult result = success(info, newIndex);
    // Which of the two this run produced is a parameter, not a property of the filter, so
    // the descriptor's tag cannot say it and the result overrides it.
    result.outputTags << (wantComplex ? QStringLiteral("alpha complex")
                                      : QStringLiteral("alpha shape"));
    return result;
}

// Voronoi filtering — the Amenta/Bern "crust". Two Delaunay passes:
//
//  1. Triangulate the samples. For each sample p, its *poles* are the two Voronoi
//     vertices of its cell farthest from it, one on each side: p+ is the farthest one,
//     p- the farthest lying in the opposite half-space. The poles approximate the medial
//     axis, so they sit far from the surface.
//  2. Triangulate samples *and* poles together. A Delaunay triangle whose three corners
//     are all samples cannot span the medial axis, so those triangles are the surface.
//
// The guarantee assumes a closed, well-sampled surface. Points on the convex hull have
// unbounded Voronoi cells and thus no finite outer pole, so boundaries stay ragged — a
// property of the algorithm, not of this implementation.
MeshFilterRunResult runVoronoiFiltering(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    const VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() < 4)
        return fail(QObject::tr("Voronoi filtering needs at least 4 vertices."));

    const double threshold = params.getDouble(QStringLiteral("threshold"), 10.0);
    if (!std::isfinite(threshold) || threshold <= 0.0)
        return fail(QObject::tr("Threshold must be a finite value larger than zero."));

    std::vector<CgalPoint> samples;
    samples.reserve(std::size_t(std::max(0, mesh.VN())));
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        samples.emplace_back(v.cP().X(), v.cP().Y(), v.cP().Z());
    }
    if (samples.size() < 4)
        return fail(QObject::tr("Voronoi filtering needs at least 4 live vertices."));

    // Voronoi vertices of an almost-degenerate cell shoot off to infinity; anything
    // farther than this from its sample is discarded rather than used as a pole.
    const double diagonal = double(mesh.bbox.Diag());
    const double maxPoleDistance = (diagonal > 0.0 ? diagonal : 1.0) * threshold;

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Voronoi Filtering"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(10, "Triangulating samples...");

    VCGMesh output;
    int poleCount = 0;
    try {
        PlainDelaunay delaunay(samples.begin(), samples.end());
        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(35, "Selecting poles...");

        std::vector<CgalPoint> poles;
        std::vector<PlainDelaunay::Cell_handle> cells;
        for (auto v = delaunay.finite_vertices_begin(); v != delaunay.finite_vertices_end(); ++v) {
            const CgalPoint &p = v->point();
            cells.clear();
            delaunay.finite_incident_cells(v, std::back_inserter(cells));

            // First pole: farthest Voronoi vertex of this cell.
            CgalPoint firstPole;
            double bestSquared = -1.0;
            for (const auto &cell : cells) {
                const CgalPoint dual = delaunay.dual(cell);
                const double squared = CGAL::squared_distance(p, dual);
                if (squared > maxPoleDistance * maxPoleDistance)
                    continue;
                if (squared > bestSquared) {
                    bestSquared = squared;
                    firstPole = dual;
                }
            }
            if (bestSquared <= 0.0)
                continue; // unbounded or degenerate cell: no usable pole
            poles.push_back(firstPole);

            // Second pole: farthest Voronoi vertex on the other side of the surface.
            const Kernel::Vector_3 outward = firstPole - p;
            CgalPoint secondPole;
            double bestOpposite = -1.0;
            for (const auto &cell : cells) {
                const CgalPoint dual = delaunay.dual(cell);
                if ((dual - p) * outward >= 0.0)
                    continue;
                const double squared = CGAL::squared_distance(p, dual);
                if (squared > maxPoleDistance * maxPoleDistance)
                    continue;
                if (squared > bestOpposite) {
                    bestOpposite = squared;
                    secondPole = dual;
                }
            }
            if (bestOpposite > 0.0)
                poles.push_back(secondPole);
        }
        poleCount = int(poles.size());
        if (poles.empty()) {
            const QString message = QObject::tr(
                "No poles could be selected. The point set may be too small, degenerate, or "
                "entirely on its convex hull.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(60, "Triangulating samples and poles...");

        // Second pass. Samples are flagged true, poles false.
        std::vector<std::pair<CgalPoint, bool>> tagged;
        tagged.reserve(samples.size() + poles.size());
        for (const CgalPoint &p : samples)
            tagged.emplace_back(p, true);
        for (const CgalPoint &p : poles)
            tagged.emplace_back(p, false);
        CrustDelaunay crust(tagged.begin(), tagged.end());

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(85, "Extracting the crust...");

        std::unordered_map<const void *, int> vertexIndex;
        for (auto facet = crust.finite_facets_begin(); facet != crust.finite_facets_end(); ++facet) {
            const CrustDelaunay::Cell_handle cell = facet->first;
            const int opposite = facet->second;
            int corner[3];
            bool allSamples = true;
            for (int k = 0; k < 3 && allSamples; ++k) {
                auto handle = cell->vertex((opposite + k + 1) % 4);
                if (crust.is_infinite(handle) || !handle->info()) {
                    allSamples = false;
                    break;
                }
                const void *key = &*handle;
                auto it = vertexIndex.find(key);
                if (it == vertexIndex.end()) {
                    const CgalPoint &p = handle->point();
                    auto vi = vcg::tri::Allocator<VCGMesh>::AddVertex(
                        output, vcg::Point3f(float(p.x()), float(p.y()), float(p.z())));
                    it = vertexIndex.emplace(key, int(vcg::tri::Index(output, *vi))).first;
                }
                corner[k] = it->second;
            }
            if (!allSamples)
                continue;
            if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2])
                continue;
            vcg::tri::Allocator<VCGMesh>::AddFace(output, corner[0], corner[1], corner[2]);
        }
    } catch (const std::exception &e) {
        const QString message =
            QObject::tr("CGAL Voronoi filtering failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    if (output.FN() <= 0) {
        const QString message = QObject::tr(
            "Voronoi filtering produced no faces. The sampling may be too sparse or too noisy.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(output);
    vcg::tri::UpdateBounding<VCGMesh>::Box(output);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);

    const int ioMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    const int newIndex = doc.addMesh(output, {}, ioMask);
    if (newIndex < 0) {
        const QString message = QObject::tr("Failed to add the Voronoi filtering layer.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    doc.finishFilterProgress(true, QObject::tr("Generated crust surface."));

    QStringList info;
    info << QObject::tr("Input: %1 samples, %2 poles.").arg(samples.size()).arg(poleCount)
         << QObject::tr("Output mesh: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    return success(info, newIndex);
}

// Collect the current layer's live vertex positions. Every reconstruction here works
// from points alone, so faces and normals are ignored.
std::vector<CgalPoint> collectPoints(const VCGMesh &mesh)
{
    std::vector<CgalPoint> points;
    points.reserve(std::size_t(std::max(0, mesh.VN())));
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        points.emplace_back(v.cP().X(), v.cP().Y(), v.cP().Z());
    }
    return points;
}

// Build a VCG mesh from a point array plus index triples into it. Both scale space and
// advancing front report their result this way, so unlike the alpha shape there are no
// triangulation handles to outlive.
void buildIndexedMesh(const std::vector<CgalPoint> &points,
                      const std::vector<std::array<std::size_t, 3>> &facets,
                      VCGMesh &output)
{
    std::vector<int> remap(points.size(), -1);
    for (const auto &f : facets) {
        int corner[3];
        bool ok = true;
        for (int k = 0; k < 3 && ok; ++k) {
            const std::size_t idx = f[std::size_t(k)];
            if (idx >= points.size()) {
                ok = false;
                break;
            }
            if (remap[idx] < 0) {
                const CgalPoint &p = points[idx];
                auto vi = vcg::tri::Allocator<VCGMesh>::AddVertex(
                    output, vcg::Point3f(float(p.x()), float(p.y()), float(p.z())));
                remap[idx] = int(vcg::tri::Index(output, *vi));
            }
            corner[k] = remap[idx];
        }
        if (!ok)
            continue;
        if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2])
            continue;
        vcg::tri::Allocator<VCGMesh>::AddFace(output, corner[0], corner[1], corner[2]);
    }
}

MeshFilterRunResult finishReconstruction(
    Document &doc, VCGMesh &output, const QString &layerName,
    const QString &emptyMessage, QStringList info)
{
    if (output.FN() <= 0) {
        doc.finishFilterProgress(false, emptyMessage);
        return fail(emptyMessage);
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(output);
    vcg::tri::UpdateBounding<VCGMesh>::Box(output);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);

    const int ioMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    const int newIndex = doc.addMesh(output, {}, ioMask);
    if (newIndex < 0) {
        const QString message = QObject::tr("Failed to add the %1 layer.").arg(layerName);
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    doc.finishFilterProgress(true, QObject::tr("Generated %1.").arg(layerName));

    info << QObject::tr("Output mesh: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    return success(info, newIndex);
}

// Scale-space surface reconstruction.
//
// The point set is smoothed for a number of iterations, producing a coarser "scale" at
// which the surface is easier to extract; the mesher then triangulates the smoothed
// points. Note that the output vertices are the *smoothed* positions, so unlike the
// other interpolating reconstructions here they do not coincide with the input points.
MeshFilterRunResult runScaleSpace(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    const VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() < 4)
        return fail(QObject::tr("Scale-space reconstruction needs at least 4 vertices."));

    const int iterations = std::max(0, params.getInt(QStringLiteral("iterations"), 4));
    const int neighbors = std::max(3, params.getInt(QStringLiteral("neighbors"), 12));
    const int samples = std::max(neighbors, params.getInt(QStringLiteral("samples"), 300));
    const bool useAdvancingFront =
        params.getEnum(QStringLiteral("mesher")) == QStringLiteral("advancing_front");
    const double alpha = params.getDouble(QStringLiteral("alpha"), 0.0);
    const bool separateShells = params.getBool(QStringLiteral("separateShells"), false);
    const bool forceManifold = params.getBool(QStringLiteral("forceManifold"), true);
    if (!useAdvancingFront && (!std::isfinite(alpha) || alpha <= 0.0))
        return fail(QObject::tr("Alpha must be a finite value larger than zero."));

    std::vector<CgalPoint> points = collectPoints(mesh);
    if (points.size() < 4)
        return fail(QObject::tr("Scale-space reconstruction needs at least 4 live vertices."));

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Scale Space"));

    VCGMesh output;
    std::size_t pointCount = 0;
    try {
        ScaleSpace reconstruction;
        reconstruction.insert(points.begin(), points.end());

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(20, "Smoothing the point set...");
        if (iterations > 0) {
            reconstruction.increase_scale(
                std::size_t(iterations),
                ScaleSpaceSmoother(unsigned(neighbors), unsigned(samples)));
        }

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(60, "Meshing...");
        if (useAdvancingFront)
            reconstruction.reconstruct_surface(ScaleSpaceAdvancingFrontMesher());
        else
            reconstruction.reconstruct_surface(
                ScaleSpaceAlphaMesher(Kernel::FT(alpha * alpha), separateShells, forceManifold));

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(85, "Building output mesh...");

        // Read back the smoothed points: the facets index into them, not into the input.
        std::vector<CgalPoint> smoothed(
            reconstruction.points_begin(), reconstruction.points_end());
        pointCount = smoothed.size();
        std::vector<std::array<std::size_t, 3>> facets(
            reconstruction.facets_begin(), reconstruction.facets_end());
        buildIndexedMesh(smoothed, facets, output);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("CGAL scale-space reconstruction failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    QStringList info;
    info << QObject::tr("Smoothed %1 point(s) over %2 iteration(s).")
                .arg(pointCount).arg(iterations)
         << (useAdvancingFront ? QObject::tr("Mesher: advancing front.")
                               : QObject::tr("Mesher: alpha shape."));
    return finishReconstruction(
        doc, output, QObject::tr("Scale Space"),
        QObject::tr("Scale-space reconstruction produced no faces. Try more smoothing "
                    "iterations, or a larger alpha."),
        info);
}

// Advancing-front surface reconstruction (Da, Cohen-Steiner and Fabri). An interpolating
// method: it grows a triangulation outward from a seed facet, so every output vertex is
// an input point. CGAL has no ball-pivoting implementation; this is its counterpart to
// the vcglib Ball Pivoting filter, and the two are worth comparing.
MeshFilterRunResult runAdvancingFront(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    const VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() < 4)
        return fail(QObject::tr("Advancing front needs at least 4 vertices."));

    const double radiusRatioBound = params.getDouble(QStringLiteral("radiusRatioBound"), 5.0);
    const double betaDegrees = params.getDouble(QStringLiteral("beta"), 30.0);
    if (!std::isfinite(radiusRatioBound) || radiusRatioBound <= 0.0)
        return fail(QObject::tr("Radius ratio bound must be a finite value larger than zero."));
    if (!std::isfinite(betaDegrees) || betaDegrees < 0.0 || betaDegrees >= 90.0)
        return fail(QObject::tr("Beta must be in the range [0, 90) degrees."));

    std::vector<CgalPoint> points = collectPoints(mesh);
    if (points.size() < 4)
        return fail(QObject::tr("Advancing front needs at least 4 live vertices."));

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Advancing Front"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Running advancing front...");

    VCGMesh output;
    try {
        // CGAL takes beta in radians.
        const double beta = betaDegrees * M_PI / 180.0;
        std::vector<std::array<std::size_t, 3>> facets;
        CGAL::advancing_front_surface_reconstruction(
            points.begin(), points.end(), std::back_inserter(facets), radiusRatioBound, beta);

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(85, "Building output mesh...");
        buildIndexedMesh(points, facets, output);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("CGAL advancing front failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    QStringList info;
    info << QObject::tr("Input: %1 points.").arg(points.size());
    return finishReconstruction(
        doc, output, QObject::tr("Advancing Front"),
        QObject::tr("Advancing front produced no faces. The sampling may be too sparse "
                    "or too noisy; try raising the radius ratio bound."),
        info);
}

// Collect the current layer's live vertices as oriented points, each tagged with its
// index in the VCG mesh so results can be written back after CGAL reorders the range.
std::vector<OrientedPoint> collectOrientedPoints(const VCGMesh &mesh)
{
    std::vector<OrientedPoint> points;
    points.reserve(std::size_t(std::max(0, mesh.VN())));
    for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
        const VCGVertex &v = mesh.vert[i];
        if (v.IsD())
            continue;
        points.emplace_back(
            CgalPoint(v.cP().X(), v.cP().Y(), v.cP().Z()),
            CgalVector(v.cN().X(), v.cN().Y(), v.cN().Z()),
            i);
    }
    return points;
}

// Fixed because the oriented box is found by an evolutionary optimizer: an unseeded run
// gives a slightly different box every time, which would make the filter unreplayable from
// the action history for no benefit anyone wants from a bounding box.
constexpr unsigned int kOrientedBoxSeed = 0x5eedu;

// What a bounding-box filter measures: one layer in its own frame, or every visible layer
// in world space. Gathering the two subjects through the same struct is what lets the two
// filters share every line below -- they differ in which points go in and which matrix the
// resulting layer comes out with, and in nothing else.
struct BoxSubject
{
    QString name;
    QMatrix4x4 transform;    // matrix given to the new box layer
    // The layer the box was measured from, remembered here because addMesh() moves the
    // current index onto the new box and the rotation has to go to the source.
    int sourceIndex = -1;
    // Every layer that contributed a point, for naming the result. The scene box treats
    // them all alike, so it is named after the set rather than after one of them.
    QVector<int> contributors;
    vcg::Box3f aabb;         // bounds of the measured points, in the frame they were measured in
    int vertexCount = 0;
    int layerCount = 0;
    // Filled only when an oriented box was actually asked for. CGAL needs every point;
    // the axis-aligned path already has its whole answer in `aabb`, and on a large scene
    // this vector is the expensive part.
    std::vector<CgalPoint> points;
};

bool gatherBoxSubject(
    Document &doc, bool wholeScene, bool wantPoints, BoxSubject &subject, QString &error)
{
    subject.aabb.SetNull();

    const auto addLayer = [&](const Document::MeshEntry &entry, bool toWorld) {
        const VCGMesh &mesh = entry.mesh;
        int live = 0;
        for (const auto &v : mesh.vert) {
            if (v.IsD())
                continue;
            ++live;
            vcg::Point3f q = v.P();
            if (toWorld) {
                const QVector3D w = entry.transform.map(QVector3D(q[0], q[1], q[2]));
                q = vcg::Point3f(w.x(), w.y(), w.z());
            }
            subject.aabb.Add(q);
            if (wantPoints)
                subject.points.emplace_back(q[0], q[1], q[2]);
        }
        if (live > 0)
            ++subject.layerCount;
        subject.vertexCount += live;
    };

    if (wholeScene) {
        int reserve = 0;
        for (int i = 0; i < doc.meshCount(); ++i) {
            if (doc.mesh(i).visible)
                reserve += doc.mesh(i).mesh.VN();
        }
        if (wantPoints)
            subject.points.reserve(std::size_t(std::max(0, reserve)));
        for (int i = 0; i < doc.meshCount(); ++i) {
            const Document::MeshEntry &entry = doc.mesh(i);
            if (!entry.visible)
                continue;
            const int before = subject.layerCount;
            addLayer(entry, true);
            if (subject.layerCount > before)
                subject.contributors.push_back(i);
        }
        if (subject.layerCount == 0) {
            error = QObject::tr(
                "Create Scene Bounding Box needs at least one visible layer with vertices.");
            return false;
        }
        subject.name = QObject::tr("Scene");
        // World space, so the box keeps its place whatever the layers it was measured
        // from are moved to afterwards.
        subject.transform.setToIdentity();
        return true;
    }

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount()) {
        error = QObject::tr("No current mesh selected.");
        return false;
    }
    const Document::MeshEntry &entry = doc.mesh(meshIndex);
    if (entry.mesh.VN() <= 0) {
        error = QObject::tr("Create Mesh Bounding Box requires a layer with vertices.");
        return false;
    }
    if (wantPoints) {
        subject.points.reserve(std::size_t(entry.mesh.VN()));
        addLayer(entry, false);
    } else {
        // This filter declares "BBox" preparation, so the layer's own box is current and
        // the axis-aligned answer is already sitting there: no pass over the vertices, which
        // is what lets the description call that alignment instant. The scene has no such
        // shortcut -- a rotated layer's world extent is not its local box transformed.
        subject.aabb = entry.mesh.bbox;
        subject.vertexCount = entry.mesh.VN();
        subject.layerCount = 1;
    }
    subject.name = entry.name;
    subject.transform = entry.transform;
    subject.sourceIndex = meshIndex;
    subject.contributors.push_back(meshIndex);
    return true;
}

// The bounding box of a layer, or of the whole scene, as a mesh of its own.
//
// Both alignments end up describing the same eight corners in the same order -- x fastest,
// then y, then z -- and that is exactly the order vcg::tri::Box lays its vertices out in.
// So the box is built once by vcglib and the oriented case only moves the corners
// afterwards: the faces, their winding and their faux edges all come from the same code
// that builds Create Hexahedron, and the new layer looks like any other box primitive.
MeshFilterRunResult runBoundingBox(const FilterParams &params, Document &doc, bool wholeScene)
{
    const QString alignment = params.getEnum(QStringLiteral("alignment"));
    const bool oriented = (alignment == QLatin1String("oriented"));
    // Only the single-layer filter declares this option; the scene has no one layer to turn.
    const bool rotateLayer = !wholeScene && (alignment == QLatin1String("oriented_rotate_layer"));

    BoxSubject subject;
    QString error;
    if (!gatherBoxSubject(doc, wholeScene, oriented || rotateLayer, subject, error))
        return fail(error);
    if ((oriented || rotateLayer) && subject.points.size() < 3) {
        return fail(QObject::tr(
            "An oriented bounding box needs at least three vertices; this has %1.")
                        .arg(subject.points.size()));
    }

    QStringList info;

    // Turning the layer rather than the box. CGAL's transformation is the map into the frame
    // where the tightest box is axis-aligned; putting it in the layer's matrix leaves the
    // vertices untouched and moves the object instead, which is the same picture a baked
    // rotation would give and is undoable without rewriting any geometry.
    QMatrix4x4 rotatedTransform = subject.transform;
    vcg::Box3f rotatedBox;
    if (rotateLayer) {
        Kernel::Aff_transformation_3 toAxes;
        CGAL::oriented_bounding_box(
            subject.points, toAxes, CGAL::parameters::random_seed(kOrientedBoxSeed));

        QMatrix4x4 rotation;  // identity, then the 3x4 CGAL carries
        for (int row = 0; row < 3; ++row) {
            for (int col = 0; col < 4; ++col)
                rotation(row, col) = float(toAxes.m(row, col));
        }
        rotatedTransform = subject.transform * rotation;

        // The box is axis-aligned in that rotated frame, so it is simply the bounds of the
        // turned points -- no corner bookkeeping, and no dependence on CGAL's corner order.
        rotatedBox.SetNull();
        for (const CgalPoint &q : subject.points) {
            const CgalPoint turned = toAxes.transform(q);
            rotatedBox.Add(VCGMesh::CoordType(
                float(turned.x()), float(turned.y()), float(turned.z())));
        }
    }

    VCGMesh output;
    vcg::tri::Box<VCGMesh>(output, rotateLayer ? rotatedBox : subject.aabb);

    if (oriented) {
        std::array<CgalPoint, 8> obb;
        CGAL::oriented_bounding_box(
            subject.points, obb, CGAL::parameters::random_seed(kOrientedBoxSeed));

        // CGAL numbers its corners for make_hexahedron; vcglib numbers them by which side
        // of each axis they lie on. This is that relabelling, nothing more.
        constexpr std::array<int, 8> kVcgFromCgal = { 0, 1, 3, 2, 6, 4, 5, 7 };
        std::array<VCGMesh::CoordType, 8> corners;
        for (std::size_t i = 0; i < corners.size(); ++i) {
            corners[std::size_t(kVcgFromCgal[i])] = VCGMesh::CoordType(
                float(obb[i].x()), float(obb[i].y()), float(obb[i].z()));
        }

        // A reflection where a rotation was expected would leave every face wound inwards
        // and the box lit from the inside. Exchanging the two ends of each x edge restores
        // a right-handed frame without moving any of the eight points.
        const VCGMesh::CoordType ex = corners[1] - corners[0];
        const VCGMesh::CoordType ey = corners[2] - corners[0];
        const VCGMesh::CoordType ez = corners[4] - corners[0];
        if (((ex ^ ey) * ez) < 0.0f) {
            for (std::size_t i = 0; i < corners.size(); i += 2)
                std::swap(corners[i], corners[i + 1]);
        }

        std::size_t next = 0;
        for (auto &v : output.vert)
            v.P() = corners[next++];

        const float sides[3] = { ex.Norm(), ey.Norm(), ez.Norm() };
        info << QObject::tr("Oriented box: %1 x %2 x %3, volume %4.")
                    .arg(sides[0]).arg(sides[1]).arg(sides[2])
                    .arg(double(sides[0]) * double(sides[1]) * double(sides[2]));
    } else {
        const vcg::Box3f &measured = rotateLayer ? rotatedBox : subject.aabb;
        const vcg::Point3f diagonal = measured.max - measured.min;
        // After a rotation this is the tightest box, now sitting on the axes; without one
        // it is the plain axis-aligned box. Different meaning, same arithmetic.
        info << (rotateLayer
                     ? QObject::tr("Tightest box, put on the axes: %1 x %2 x %3, volume %4.")
                     : QObject::tr("Axis-aligned box: %1 x %2 x %3, volume %4."))
                    .arg(diagonal[0]).arg(diagonal[1]).arg(diagonal[2])
                    .arg(double(diagonal[0]) * double(diagonal[1]) * double(diagonal[2]));
    }

    if (wholeScene) {
        info << QObject::tr("Measured %1 visible layers, %2 vertices in world space.")
                    .arg(subject.layerCount)
                    .arg(subject.vertexCount);
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(output);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);

    const QString layerName = QObject::tr("%1 bounding box").arg(subject.name);
    const int ioMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    const int newIndex = doc.addMesh(output, {}, ioMask);
    if (newIndex < 0)
        return fail(QObject::tr("Failed to add the %1 layer.").arg(layerName));

    // The box always takes the matrix of the frame it was measured in: the layer's own for
    // the first two alignments, the layer's own again for the third (its coordinates are
    // already in the turned frame, so handing it the turned matrix as well would rotate it
    // a second time and slide it off the object it is meant to hug), and identity for the
    // scene, whose points were gathered in world space to begin with.
    doc.setMeshTransform(newIndex, subject.transform);
    if (rotateLayer) {
        doc.setMeshTransform(
            subject.sourceIndex,
            rotatedTransform,
            QObject::tr("Turned '%1' onto the axes of its tightest bounding box")
                .arg(subject.name));
        info << QObject::tr("Stored the rotation in the layer's transformation.");
    }

    MeshFilterRunResult result = success(info, newIndex);
    result.sourceMeshIndices = subject.contributors;
    return result;
}

// Orient an unoriented normal field with a minimum spanning tree of the Riemannian graph
// (Hoppe et al.). This is the missing step between "Compute Point Cloud Normals", which
// gives normals with arbitrary sign, and the reconstructions that need them oriented —
// Screened Poisson, SSD, CGAL Poisson and the kinetic pipeline all require it.
MeshFilterRunResult runOrientNormals(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    if (mesh.VN() < 2)
        return fail(QObject::tr("Normal orientation needs at least 2 vertices."));

    const int neighbors = std::max(1, params.getInt(QStringLiteral("neighbors"), 18));
    const bool removeUnoriented = params.getBool(QStringLiteral("removeUnoriented"), false);

    std::vector<OrientedPoint> points = collectOrientedPoints(mesh);
    if (points.size() < 2)
        return fail(QObject::tr("Normal orientation needs at least 2 live vertices."));

    doc.beginFilterProgress(QObject::tr("Orient Point Cloud Normals"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Building the Riemannian graph...");

    std::size_t orientedCount = 0;
    try {
        // Returns the first point it could not orient; everything before it is oriented.
        auto unorientedBegin = CGAL::mst_orient_normals(
            points, unsigned(neighbors),
            CGAL::parameters::point_map(OrientedPointMap())
                .normal_map(OrientedNormalMap()));
        orientedCount = std::size_t(std::distance(points.begin(), unorientedBegin));

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(80, "Writing normals back...");

        // The range was reordered, so the tagged index is the only way home.
        for (std::size_t i = 0; i < points.size(); ++i) {
            if (removeUnoriented && i >= orientedCount)
                continue;
            const CgalVector &n = std::get<1>(points[i]);
            const std::size_t vi = std::get<2>(points[i]);
            mesh.vert[vi].N() = vcg::Point3f(float(n.x()), float(n.y()), float(n.z()));
        }
        if (removeUnoriented && orientedCount < points.size()) {
            for (std::size_t i = orientedCount; i < points.size(); ++i)
                vcg::tri::Allocator<VCGMesh>::DeleteVertex(mesh, mesh.vert[std::get<2>(points[i])]);
        }
    } catch (const std::exception &e) {
        const QString message = QObject::tr("CGAL normal orientation failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    entry.ioMask |= Mask::IOM_VERTNORMAL;
    doc.markMeshGeometryChanged(
        meshIndex, QObject::tr("Oriented vertex normals of '%1'").arg(entry.name));
    doc.finishFilterProgress(true, QObject::tr("Oriented vertex normals."));

    const std::size_t failed = points.size() - orientedCount;
    QStringList info;
    info << QObject::tr("Oriented %1 of %2 normal(s).").arg(orientedCount).arg(points.size());
    if (failed > 0) {
        info << (removeUnoriented
                     ? QObject::tr("Removed %1 vertex(es) that could not be oriented.").arg(failed)
                     : QObject::tr("%1 normal(s) could not be oriented and were left as they "
                                   "were; they usually sit on disconnected components.")
                           .arg(failed));
    }
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    return result;
}

// Poisson surface reconstruction, CGAL's implementation. It differs from the Screened
// Poisson filter in how the implicit function is meshed: Delaunay refinement rather than
// marching cubes, so the output is manifold with well-shaped triangles at the cost of
// speed. Requires oriented normals — run "Orient Point Cloud Normals" first.
MeshFilterRunResult runPoisson(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    const VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() < 4)
        return fail(QObject::tr("Poisson reconstruction needs at least 4 vertices."));

    const double smAngle = params.getDouble(QStringLiteral("smAngle"), 20.0);
    const double smRadius = params.getDouble(QStringLiteral("smRadius"), 30.0);
    const double smDistance = params.getDouble(QStringLiteral("smDistance"), 0.375);
    const int spacingNeighbors = std::max(2, params.getInt(QStringLiteral("spacingNeighbors"), 6));
    const bool perturb = params.getBool(QStringLiteral("perturb"), false);

    std::vector<OrientedPoint> points = collectOrientedPoints(mesh);
    if (points.size() < 4)
        return fail(QObject::tr("Poisson reconstruction needs at least 4 live vertices."));

    // CGAL's surface mesher can fail to terminate on an exactly degenerate point set. An
    // analytic sphere is the case we hit: every point lies exactly on one sphere, so the
    // Delaunay triangulation is massively non-unique and the refinement never satisfies
    // its criteria -- it spins at 100% CPU allocating nothing, rather than running slowly.
    // A displacement of a millionth of the diagonal is enough to break it and is orders of
    // magnitude below any modelling tolerance. Deterministic, because a filter that moved
    // the input differently on every run would not be reproducible.
    if (perturb) {
        const double amount = 1e-6 * double(mesh.bbox.Diag());
        const auto dither = [](std::size_t index, unsigned axis) {
            std::uint32_t h = std::uint32_t(index) * 2654435761u + axis * 40503u;
            h ^= h >> 15;
            h *= 2246822519u;
            h ^= h >> 13;
            return double(h) / 2147483647.5 - 1.0;  // [-1, 1]
        };
        for (std::size_t i = 0; i < points.size(); ++i) {
            const CgalPoint &q = std::get<0>(points[i]);
            std::get<0>(points[i]) = CgalPoint(
                q.x() + amount * dither(i, 0),
                q.y() + amount * dither(i, 1),
                q.z() + amount * dither(i, 2));
        }
    }

    // A zero-length normal means the layer never had normals computed; Poisson would
    // return an unusable surface rather than fail, so say so plainly instead.
    std::size_t degenerateNormals = 0;
    for (const OrientedPoint &p : points) {
        if (std::get<1>(p).squared_length() < 1e-20)
            ++degenerateNormals;
    }
    if (degenerateNormals == points.size()) {
        return fail(QObject::tr(
            "Poisson reconstruction needs oriented normals, but this layer has none. Run "
            "'Compute Point Cloud Normals' and then 'Orient Point Cloud Normals' first."));
    }

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Poisson"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(10, "Estimating average spacing...");

    VCGMesh output;
    double spacing = 0.0;
    try {
        spacing = CGAL::compute_average_spacing<CGAL::Sequential_tag>(
            points, unsigned(spacingNeighbors),
            CGAL::parameters::point_map(OrientedPointMap()));
        if (!(spacing > 0.0))
            return fail(QObject::tr("Could not estimate the average point spacing."));

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(30, "Solving the Poisson equation...");

        CgalMesh reconstructed;
        const bool ok = CGAL::poisson_surface_reconstruction_delaunay(
            points.begin(), points.end(), OrientedPointMap(), OrientedNormalMap(),
            reconstructed, spacing, smAngle, smRadius, smDistance);
        if (!ok) {
            const QString message = QObject::tr(
                "CGAL Poisson reconstruction failed. The normals may be unoriented, or the "
                "sampling too sparse for the requested triangle size.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(85, "Building output mesh...");
        QString error;
        if (!copyCgalMeshToVcg(reconstructed, output, error)) {
            doc.finishFilterProgress(false, error);
            return fail(error);
        }
    } catch (const std::exception &e) {
        const QString message = QObject::tr("CGAL Poisson reconstruction failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    QStringList info;
    info << QObject::tr("Input: %1 oriented points.").arg(points.size())
         << QObject::tr("Average spacing: %1").arg(QString::number(spacing, 'g', 6));
    if (perturb)
        info << QObject::tr("Input perturbed by up to %1 to break exact degeneracies.")
                    .arg(QString::number(1e-6 * double(mesh.bbox.Diag()), 'g', 3));
    if (degenerateNormals > 0)
        info << QObject::tr("%1 point(s) had a zero-length normal.").arg(degenerateNormals);
    return finishReconstruction(
        doc, output, QObject::tr("Poisson"),
        QObject::tr("Poisson reconstruction produced no faces."), info);
}

// Kinetic surface reconstruction: detect planar shapes, regularize them, partition space
// kinetically into convex volumes, then label volumes inside/outside with a min-cut. The
// result is piecewise planar by construction, which suits buildings and other man-made
// shapes far better than a smooth reconstruction. Requires oriented normals.
MeshFilterRunResult runKinetic(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    const VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() < 4)
        return fail(QObject::tr("Kinetic reconstruction needs at least 4 vertices."));

    const int kNeighbors = std::max(3, params.getInt(QStringLiteral("kNeighbors"), 12));
    const double maxDistance = params.getDouble(QStringLiteral("maximumDistance"), 0.0);
    const double maxAngle = params.getDouble(QStringLiteral("maximumAngle"), 15.0);
    const int minRegionSize = std::max(3, params.getInt(QStringLiteral("minimumRegionSize"), 50));
    const int intersections = std::clamp(params.getInt(QStringLiteral("intersections"), 1), 1, 3);
    const double lambda = params.getDouble(QStringLiteral("lambda"), 0.5);
    if (!std::isfinite(maxDistance) || maxDistance <= 0.0)
        return fail(QObject::tr("Maximum distance must be a finite value larger than zero."));
    if (!std::isfinite(lambda) || lambda < 0.0 || lambda >= 1.0)
        return fail(QObject::tr("Lambda must be in the range [0, 1)."));

    std::vector<OrientedPoint> points = collectOrientedPoints(mesh);
    if (points.size() < 4)
        return fail(QObject::tr("Kinetic reconstruction needs at least 4 live vertices."));

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Kinetic Partition"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(10, "Detecting planar shapes...");

    // Unlike the other CGAL entry points here, the kinetic pipeline deduces its kernel
    // from the range type itself (Least_squares_plane_fit_region_for_point_set<Range>),
    // so a tuple range with property maps does not compile — it needs a Point_set_3.
    CgalPointSet pointSet;
    pointSet.add_normal_map();
    pointSet.reserve(points.size());
    for (const OrientedPoint &p : points)
        pointSet.insert(std::get<0>(p), std::get<1>(p));

    VCGMesh output;
    std::size_t polygonCount = 0;
    std::size_t planeCount = 0;
    try {
        using Kinetic = CGAL::Kinetic_surface_reconstruction_3<
            Kernel, CgalPointSet, CgalPointSet::Point_map, CgalPointSet::Vector_map>;

        Kinetic kinetic(
            pointSet,
            CGAL::parameters::point_map(pointSet.point_map())
                .normal_map(pointSet.normal_map()));

        // Run the three stages separately rather than via detection_and_partition(), so
        // that a run which finds no planes can say so instead of silently producing an
        // empty mesh — with these parameters that is by far the most common outcome.
        const auto detectionParams =
            CGAL::parameters::maximum_distance(maxDistance)
                .maximum_angle(maxAngle)
                .k_neighbors(std::size_t(kNeighbors))
                .minimum_region_size(std::size_t(minRegionSize));
        planeCount = kinetic.detect_planar_shapes(detectionParams);
        if (planeCount == 0) {
            const QString message = QObject::tr(
                "No planar regions were detected, so there is nothing to reconstruct. "
                "Increase Maximum Distance to the noise scale of the data, lower "
                "Minimum Region Size, or widen Maximum Angle.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(40, "Partitioning space kinetically...");
        kinetic.initialize_partition(detectionParams);
        kinetic.partition(std::size_t(intersections));

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(70, "Labelling volumes with a min-cut...");

        // An empty external_nodes map leaves every bounding-box side to the min-cut,
        // which is what we want: nothing here knows which side is "the ground".
        // An empty external_nodes map leaves every bounding-box side to the min-cut,
        // which is what we want: nothing here knows which side is "the ground".
        std::map<typename Kinetic::KSP::Face_support, bool> externalNodes;
        std::vector<CgalPoint> vertices;
        std::vector<std::vector<std::size_t>> polygons;
        kinetic.reconstruct(
            lambda, externalNodes, std::back_inserter(vertices), std::back_inserter(polygons));
        polygonCount = polygons.size();

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(90, "Building output mesh...");

        // The partition emits convex polygons, so fan-triangulate them. (CGAL also has a
        // reconstructed_model_trilist() that would do this and drop bounding-box faces,
        // but in this version it is private and does not compile.)
        std::vector<Triangle> facets;
        for (const std::vector<std::size_t> &poly : polygons) {
            for (std::size_t k = 2; k < poly.size(); ++k)
                facets.push_back({ poly[0], poly[k - 1], poly[k] });
        }
        buildIndexedMesh(vertices, facets, output);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("CGAL kinetic reconstruction failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    QStringList info;
    info << QObject::tr("Input: %1 oriented points.").arg(points.size())
         << QObject::tr("Detected %1 planar region(s).").arg(planeCount)
         << QObject::tr("Reconstructed %1 planar polygon(s).").arg(polygonCount);
    return finishReconstruction(
        doc, output, QObject::tr("Kinetic"),
        QObject::tr("Kinetic reconstruction produced no faces. Try a larger maximum "
                    "distance, or a smaller minimum region size, so that planes are found."),
        info);
}

} // namespace

QString CgalFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.cgal");
}

QString CgalFilterPlugin::name() const
{
    return QObject::tr("CGAL Filters");
}

MeshFilterRunResult CgalFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kFilterAlphaShape))
        return runAlphaShape(params, doc);
    if (filterId == QString::fromLatin1(kFilterVoronoiFiltering))
        return runVoronoiFiltering(params, doc);
    if (filterId == QString::fromLatin1(kFilterScaleSpace))
        return runScaleSpace(params, doc);
    if (filterId == QString::fromLatin1(kFilterAdvancingFront))
        return runAdvancingFront(params, doc);
    if (filterId == QString::fromLatin1(kFilterOrientNormals))
        return runOrientNormals(params, doc);
    if (filterId == QString::fromLatin1(kFilterMeshBoundingBox))
        return runBoundingBox(params, doc, false);
    if (filterId == QString::fromLatin1(kFilterSceneBoundingBox))
        return runBoundingBox(params, doc, true);
    if (filterId == QString::fromLatin1(kFilterPoisson))
        return runPoisson(params, doc);
    if (filterId == QString::fromLatin1(kFilterKinetic))
        return runKinetic(params, doc);
    if (filterId != QString::fromLatin1(kFilterAlphaWrap))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    const Document::MeshEntry &entry = doc.mesh(meshIndex);
    const VCGMesh &mesh = entry.mesh;
    // Faces are optional: without them the point set itself is wrapped.
    if (mesh.VN() <= 0)
        return fail(QObject::tr("Alpha Wrap requires a mesh or point cloud with vertices."));

    const double alpha = params.getDouble(QStringLiteral("Alpha"), 0.0);
    const double offset = params.getDouble(QStringLiteral("Offset"), 0.0);
    if (!std::isfinite(alpha) || alpha <= 0.0)
        return fail(QObject::tr("Alpha must be a finite value larger than zero."));
    if (!std::isfinite(offset) || offset <= 0.0)
        return fail(QObject::tr("Offset must be a finite value larger than zero."));

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Alpha Wrapping"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(5, "Preparing alpha wrap input...");

    std::vector<CgalPoint> points;
    std::vector<Triangle> triangles;
    int skippedFaces = 0;
    QString error;
    if (!buildTriangleSoup(mesh, points, triangles, skippedFaces, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }
    const bool pointSetInput = triangles.empty();

    CgalMesh wrapResult;
    try {
        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(15, "Running CGAL Alpha Wrap...");
        // CGAL wraps a bare point set as happily as a triangle soup; no normals needed,
        // since the strictly positive offset is what defines the envelope.
        if (pointSetInput)
            CGAL::alpha_wrap_3(points, alpha, offset, wrapResult);
        else
            CGAL::alpha_wrap_3(points, triangles, alpha, offset, wrapResult);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("CGAL Alpha Wrap failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    } catch (...) {
        const QString message = QObject::tr("CGAL Alpha Wrap failed with an unknown error.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(90, "Converting CGAL output mesh...");

    VCGMesh output;
    if (!copyCgalMeshToVcg(wrapResult, output, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    const int ioMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    const int newIndex = doc.addMesh(output, {}, ioMask);
    if (newIndex < 0) {
        const QString message = QObject::tr("Failed to add Alpha Wrap result to the document.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    doc.finishFilterProgress(true, QObject::tr("Generated Alpha Wrap mesh."));

    QStringList info;
    info << QObject::tr("Alpha: %1").arg(QString::number(alpha, 'g', 6))
         << QObject::tr("Offset: %1").arg(QString::number(offset, 'g', 6))
         << (pointSetInput
                 ? QObject::tr("Input point set: %1 points.").arg(points.size())
                 : QObject::tr("Input triangle soup: %1 vertices, %2 faces.")
                       .arg(points.size()).arg(triangles.size()))
         << QObject::tr("Output mesh: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    if (skippedFaces > 0)
        info << QObject::tr("Skipped %1 invalid or degenerate input face(s).").arg(skippedFaces);

    return success(info, newIndex);
}

void registerCgalFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<CgalFilterPlugin>());
}
