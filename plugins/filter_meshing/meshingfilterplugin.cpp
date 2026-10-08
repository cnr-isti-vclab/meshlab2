#include "meshingfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include <QVector3D>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/container/simple_temporary_data.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/attribute_seam.h>
#include <vcg/complex/algorithms/bitquad_creation.h>
#include <vcg/complex/algorithms/bitquad_support.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/clip.h>
#include <vcg/complex/algorithms/clustering.h>
#include <vcg/complex/algorithms/crease_cut.h>
#include <vcg/complex/algorithms/curve_on_manifold.h>
#include <vcg/complex/algorithms/cut_tree.h>
#include <vcg/complex/algorithms/handle_tunnel_loops.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/hole.h>
#include <vcg/complex/algorithms/intersection.h>
#include <vcg/complex/algorithms/isotropic_remeshing.h>
#include <vcg/complex/algorithms/local_optimization.h>
#include <vcg/complex/algorithms/local_optimization/tri_edge_collapse_quadric.h>
#include <vcg/complex/algorithms/local_optimization/tri_edge_collapse_quadric_tex.h>
#include <vcg/complex/algorithms/pointcloud_normal.h>
#include <vcg/complex/algorithms/polygon_support.h>
#include <vcg/complex/algorithms/refine_catmullclark.h>
#include <vcg/complex/algorithms/refine_doosabin.h>
#include <vcg/complex/algorithms/refine_loop.h>
#include <vcg/complex/algorithms/reeb_graph.h>
#include <vcg/complex/algorithms/inertia.h>
#include <vcg/complex/algorithms/stat.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/curvature.h>
#include <vcg/complex/algorithms/update/curvature_fitting.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/position.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/math/base.h>
#include <vcg/math/disjoint_set.h>
#include <vcg/complex/algorithms/create/marching_cubes.h>
#include <vcg/complex/algorithms/create/mc_trivial_walker.h>
#include <vcg/complex/algorithms/closest.h>
#include <vcg/space/fitting3.h>
#include <vcg/space/hrbf.h>
#include <vcg/space/index/grid_static_ptr.h>
#include <vcg/space/planar_polygon_tessellation.h>
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {

// The faces of each FF-connected component of m (FF must be up to date).
std::vector<std::vector<size_t>> faceComponents(VCGMesh &m)
{
    std::vector<int> comp(m.face.size(), -1);
    std::vector<std::vector<size_t>> out;
    for (size_t seed = 0; seed < m.face.size(); ++seed) {
        if (m.face[seed].IsD() || comp[seed] >= 0) continue;
        out.emplace_back(1, seed);
        comp[seed] = int(out.size()) - 1;
        for (size_t k = 0; k < out.back().size(); ++k)
            for (int z = 0; z < 3; ++z) {
                const size_t g = vcg::tri::Index(m, m.face[out.back()[k]].FFp(z));
                if (comp[g] < 0) { comp[g] = comp[seed]; out.back().push_back(g); }
            }
    }
    return out;
}

// Create Surface from Polyline Loop (HRBF): the patch spanning the closed polyline \a poly
// (its coordinates mapped into the surface frame by \a toSurface), as described in the
// filter's documentation. Returns an error, or an empty string with the patch in \a patch.
QString buildLoopSurface(const VCGMesh &poly, const QMatrix4x4 &toSurface, VCGMesh &surface, bool across,
                         int sampleCount, int resolution, vcg::CallBackPos *cb, VCGMesh &patch, QStringList &info)
{
    // 1. The loop, in order.
    std::vector<std::vector<int>> nb(poly.vert.size());
    int edges = 0;
    for (const VCGEdge &e : poly.edge) {
        if (e.IsD()) continue;
        const int a = int(vcg::tri::Index(poly, e.cV(0))), b = int(vcg::tri::Index(poly, e.cV(1)));
        nb[size_t(a)].push_back(b);
        nb[size_t(b)].push_back(a);
        ++edges;
    }
    if (edges < 3)
        return QObject::tr("The polyline must be a closed loop of at least three edges.");
    int start = -1;
    for (size_t v = 0; v < nb.size(); ++v) {
        if (nb[v].empty()) continue;
        if (nb[v].size() != 2)
            return QObject::tr("The polyline must be a single closed loop, but a vertex of it has %1 edges.").arg(nb[v].size());
        if (start < 0) start = int(v);
    }
    std::vector<vcg::Point3d> loop;
    for (int prev = -1, cur = start; ; ) {
        const QVector3D q = toSurface.map(QVector3D(poly.vert[size_t(cur)].cP()[0], poly.vert[size_t(cur)].cP()[1], poly.vert[size_t(cur)].cP()[2]));
        loop.push_back(vcg::Point3d(q.x(), q.y(), q.z()));
        const int next = nb[size_t(cur)][0] != prev ? nb[size_t(cur)][0] : nb[size_t(cur)][1];
        prev = cur;
        cur = next;
        if (cur == start || int(loop.size()) > edges) break;
    }
    if (int(loop.size()) != edges)
        return QObject::tr("The polyline must be a single closed loop, but it has several pieces.");

    // 2. Samples at equal arc length.
    const size_t n = loop.size();
    std::vector<double> arc(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) arc[i + 1] = arc[i] + vcg::Distance(loop[i], loop[(i + 1) % n]);
    const double length = arc[n];
    if (!(length > 0))
        return QObject::tr("The loop has zero length.");
    const int count = std::max(8, sampleCount);
    const double spacing = length / count;
    std::vector<vcg::Point3d> samples;
    for (int k = 0, seg = 0; k < count; ++k) {
        const double s = k * spacing;
        while (arc[size_t(seg) + 1] < s) ++seg;
        const double t = (s - arc[size_t(seg)]) / std::max(arc[size_t(seg) + 1] - arc[size_t(seg)], 1e-300);
        samples.push_back(loop[size_t(seg)] + (loop[(size_t(seg) + 1) % n] - loop[size_t(seg)]) * t);
    }

    // 3. Their gradients: the surface normal there, or across the loop, in the surface and
    //    perpendicular to it. Their orientation only has to be consistent along the loop.
    vcg::GridStaticPtr<VCGFace, float> grid;
    grid.Set(surface.face.begin(), surface.face.end());
    std::vector<vcg::Point3d> normals;
    for (int k = 0; k < count; ++k) {
        const vcg::Point3f p = vcg::Point3f::Construct(samples[size_t(k)]);
        float dist = surface.bbox.Diag();
        vcg::Point3f closest, bary;
        VCGFace *f = vcg::tri::GetClosestFaceBase(surface, grid, p, surface.bbox.Diag(), dist, closest);
        if (!f)
            return QObject::tr("The loop is not on the surface layer: no face near one of its points.");
        vcg::Point3d nrm(0, 0, 0);
        if (vcg::InterpolationParameters(*f, f->cN(), closest, bary))
            for (int i = 0; i < 3; ++i) nrm += vcg::Point3d::Construct(f->cV(i)->cN()) * double(bary[i]);
        if (!(nrm.Norm() > 0)) nrm = vcg::Point3d::Construct(f->cN());
        nrm.Normalize();
        const vcg::Point3d tangent = samples[size_t((k + 1) % count)] - samples[size_t((k + count - 1) % count)];
        vcg::Point3d g = across ? (nrm ^ tangent) : nrm;
        if (!(g.Norm() > 0))
            return QObject::tr("The surface normal is undefined, or along the loop, at one of its points.");
        normals.push_back(g.Normalize());
    }
    if (cb) (*cb)(10, "Fitting the HRBF");

    // 4. The implicit function.
    vcg::HRBF<double> hrbf;
    try {
        hrbf.Fit(samples, normals);
    } catch (const std::exception &) {
        return QObject::tr("The interpolation system is singular: try fewer samples.");
    }

    // 5. Its values on a grid around the loop, enlarged so that a surface bulging out of the
    //    loop's plane fits; cells about the sample spacing, within reason.
    vcg::Box3d box;
    for (const vcg::Point3d &q : samples) box.Add(q);
    const double reach = 0.5 * box.Diag();   // how far from the loop the surface may extend
    box.Offset(reach);
    const double longest = std::max({ box.DimX(), box.DimY(), box.DimZ() });
    double cell = resolution > 0 ? longest / resolution : spacing;
    cell = std::clamp(cell, longest / (resolution > 0 ? 400 : 128), longest / 24);
    // Shifted off the loop's symmetries by odd fractions of a cell: a zero set passing through
    // grid points, as a plane of symmetry of the loop would, makes marching cubes degenerate.
    box.min -= vcg::Point3d(0.137, 0.291, 0.413) * cell;
    const vcg::Point3i dims(int(std::ceil(box.DimX() / cell)) + 2, int(std::ceil(box.DimY() / cell)) + 2,
                            int(std::ceil(box.DimZ() / cell)) + 2);
    using Volume = vcg::SimpleVolume<vcg::SimpleVoxel<float>>;
    Volume volume;
    volume.Init(dims, vcg::Box3f(vcg::Point3f::Construct(box.min),
                                 vcg::Point3f::Construct(box.min + vcg::Point3d(dims[0], dims[1], dims[2]) * cell)));
    {
        const int threads = std::max(1, int(std::thread::hardware_concurrency()));
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&, t] {
                for (int z = t; z < dims[2]; z += threads)
                    for (int y = 0; y < dims[1]; ++y)
                        for (int x = 0; x < dims[0]; ++x)
                            volume.Val(x, y, z) = float(hrbf.Value(box.min + vcg::Point3d(x, y, z) * cell));
            });
        for (std::thread &th : pool) th.join();
    }
    if (cb) (*cb)(50, "Extracting the isosurface");

    // 6. The zero set, cleaned for the curve embedding: no duplicates, no zero-area faces,
    //    edge-manifold, and only the piece the loop lies on.
    VCGMesh iso;
    {
        using Walker = vcg::tri::TrivialWalker<VCGMesh, Volume>;
        using MarchingCubes = vcg::tri::MarchingCubes<VCGMesh, Walker>;
        Walker walker;
        MarchingCubes mc(iso, walker);
        walker.BuildMesh<MarchingCubes>(iso, volume, mc, 0.0f);
    }
    iso.face.EnableFFAdjacency();
    iso.face.EnableMark();
    iso.vert.EnableMark();
    iso.vert.EnableVFAdjacency();
    iso.face.EnableVFAdjacency();
    vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(iso);
    vcg::tri::Clean<VCGMesh>::RemoveZeroAreaFace(iso);
    vcg::tri::Clean<VCGMesh>::RemoveDegenerateFace(iso);
    vcg::tri::Clean<VCGMesh>::RemoveDuplicateFace(iso);
    vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(iso);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(iso);
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(iso);
    vcg::tri::Clean<VCGMesh>::RemoveNonManifoldFace(iso);
    vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(iso);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(iso);
    // Only what lies within reach of the loop, as the grid would with no corners: the rest of
    // the sheet the grid clipped is never part of the result, and remeshing it is wasted.
    for (VCGFace &f : iso.face) {
        if (f.IsD()) continue;
        bool near = false;
        for (int i = 0; i < 3 && !near; ++i)
            for (const vcg::Point3d &q : samples)
                if (vcg::SquaredDistance(vcg::Point3d::Construct(f.cP(i)), q) < reach * reach) { near = true; break; }
        if (!near) vcg::tri::Allocator<VCGMesh>::DeleteFace(iso, f);
    }
    vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(iso);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(iso);
    if (iso.FN() == 0)
        return QObject::tr("The implicit surface is empty around the loop.");
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(iso);
    vcg::tri::UpdateBounding<VCGMesh>::Box(iso);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(iso);
    {
        vcg::GridStaticPtr<VCGFace, float> isoGrid;
        isoGrid.Set(iso.face.begin(), iso.face.end());
        float dist = iso.bbox.Diag();
        vcg::Point3f closest;
        VCGFace *f = vcg::tri::GetClosestFaceBase(iso, isoGrid, vcg::Point3f::Construct(samples[0]), iso.bbox.Diag(), dist, closest);
        if (!f)
            return QObject::tr("The implicit surface does not pass near the loop.");
        const size_t keep = vcg::tri::Index(iso, f);
        for (const std::vector<size_t> &c : faceComponents(iso))
            if (std::find(c.begin(), c.end(), keep) == c.end())
                for (size_t fi : c) vcg::tri::Allocator<VCGMesh>::DeleteFace(iso, iso.face[fi]);
        vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(iso);
        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(iso);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(iso);
    }
    // Where the region around the loop clipped the implicit surface: a private flag, which
    // cutting copies.
    const int gridSide = VCGVertex::NewBitFlag();
    vcg::tri::UpdateFlags<VCGMesh>::VertexBorderFromFaceAdj(iso);
    for (VCGVertex &v : iso.vert) { if (v.IsB()) v.SetUserBit(gridSide); else v.ClearUserBit(gridSide); }
    struct BitGuard { int bit; ~BitGuard() { VCGVertex::DeleteBitFlag(bit); } } bitGuard{ gridSide };
    if (cb) (*cb)(65, "Embedding the loop");

    // 7. The loop embedded in it, and cut along.
    VCGMesh curve;
    vcg::tri::Allocator<VCGMesh>::AddVertices(curve, n);
    vcg::tri::Allocator<VCGMesh>::AddEdges(curve, n);
    for (size_t i = 0; i < n; ++i) {
        curve.vert[i].P() = vcg::Point3f::Construct(loop[i]);
        curve.edge[i].V(0) = &curve.vert[i];
        curve.edge[i].V(1) = &curve.vert[(i + 1) % n];
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(curve);
    curve.vert.EnableVEAdjacency();
    {
        using CoM = vcg::tri::CoM<VCGMesh>;
        CoM com(iso);
        com.Init();
        com.SetControlPoints(curve);
        com.SmoothProject(curve, 1, 0, 1);   // onto the implicit surface, no smoothing
        com.RefineCurveByBaseMesh(curve);
        vcg::tri::UpdateFlags<VCGMesh>::FaceClearFaceEdgeS(iso);
        vcg::tri::CoMEmbed<VCGMesh>::SplitMeshWithPolyline(com, curve);
    }
    if (vcg::tri::UpdateSelection<VCGMesh>::FaceEdgeCount(iso) == 0)
        return QObject::tr("The loop could not be embedded in the implicit surface.");
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(iso);
    vcg::tri::CutMeshAlongSelectedFaceEdges(iso);
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(iso);

    // 8. The inside: the piece the cut separated that does not reach where the implicit
    //    surface was clipped; if several do not, the smallest.
    const std::vector<std::vector<size_t>> pieces = faceComponents(iso);
    int best = -1;
    double bestArea = std::numeric_limits<double>::max();
    for (size_t c = 0; c < pieces.size(); ++c) {
        bool open = false;
        double area = 0;
        for (size_t fi : pieces[c]) {
            VCGFace &f = iso.face[fi];
            for (int i = 0; i < 3; ++i) open |= f.V(i)->IsUserBit(gridSide);
            area += vcg::DoubleArea(f) / 2;
        }
        if (!open && area < bestArea) { bestArea = area; best = int(c); }
    }
    if (pieces.size() < 2 || best < 0)
        return QObject::tr("The loop does not cut a piece off the implicit surface: the surface through it "
                           "reaches past the region around the loop. Try the other gradient, or more samples.");
    vcg::tri::UpdateSelection<VCGMesh>::FaceClear(iso);
    for (size_t fi : pieces[size_t(best)]) iso.face[fi].SetS();
    patch.Clear();
    vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(patch, iso, true);
    vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(patch);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(patch);
    // Remeshed into well-shaped triangles about a cell long, projected back onto itself, with
    // its boundary -- the loop -- kept: marching cubes triangles are anything but.
    {
        patch.face.EnableFFAdjacency();
        patch.face.EnableMark();
        patch.vert.EnableMark();
        patch.vert.EnableVFAdjacency();
        patch.face.EnableVFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(patch);
        VCGMesh reference;
        reference.face.EnableMark();
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(reference, patch);
        vcg::tri::IsotropicRemeshing<VCGMesh>::Params rp;
        rp.SetTargetLen(float(cell));
        rp.SetFeatureAngleDeg(181);
        rp.iter = 3;
        rp.adapt = false;
        rp.splitFlag = rp.collapseFlag = rp.swapFlag = rp.smoothFlag = rp.projectFlag = true;
        vcg::tri::IsotropicRemeshing<VCGMesh>::Do(patch, reference, rp);
        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(patch);
        patch.face.DisableFFAdjacency();
        patch.face.DisableMark();
        patch.vert.DisableMark();
        patch.vert.DisableVFAdjacency();
        patch.face.DisableVFAdjacency();
    }
    vcg::tri::UpdateSelection<VCGMesh>::FaceClear(patch);
    vcg::tri::UpdateBounding<VCGMesh>::Box(patch);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(patch);
    info << QObject::tr("%1 samples, %2 x %3 x %4 grid.").arg(count).arg(dims[0]).arg(dims[1]).arg(dims[2])
         << QObject::tr("Surface: %1 faces, area %2.").arg(patch.FN()).arg(bestArea, 0, 'g', 4);
    return {};
}

QString buildSectionCap(
    const VCGMesh &section,
    const vcg::Point3f &normal,
    const vcg::PlanarRefinement &refinement,
    VCGMesh &cap)
{
    using Contour = std::vector<vcg::Point3f>;

    std::vector<std::vector<size_t>> incidentEdges(section.vert.size());
    for (size_t edgeIndex = 0; edgeIndex < section.edge.size(); ++edgeIndex) {
        const VCGEdge &edge = section.edge[edgeIndex];
        if (edge.IsD())
            continue;
        const int first = vcg::tri::Index(section, edge.cV(0));
        const int second = vcg::tri::Index(section, edge.cV(1));
        if (first < 0 || second < 0 || first == second)
            return QObject::tr("The planar section contains an invalid edge.");
        incidentEdges[size_t(first)].push_back(edgeIndex);
        incidentEdges[size_t(second)].push_back(edgeIndex);
    }

    for (const auto &incident : incidentEdges) {
        if (!incident.empty() && incident.size() != 2)
            return QObject::tr("A section surface requires closed, non-branching contours.");
    }

    std::vector<Contour> contours;
    std::vector<bool> visited(section.edge.size(), false);
    for (size_t firstEdge = 0; firstEdge < section.edge.size(); ++firstEdge) {
        if (section.edge[firstEdge].IsD() || visited[firstEdge])
            continue;

        const int startVertex = vcg::tri::Index(section, section.edge[firstEdge].cV(0));
        int currentVertex = startVertex;
        size_t currentEdge = firstEdge;
        Contour contour;
        for (size_t step = 0; step <= section.edge.size(); ++step) {
            if (visited[currentEdge])
                return QObject::tr("The planar section contains an invalid contour cycle.");
            visited[currentEdge] = true;
            contour.push_back(section.vert[size_t(currentVertex)].cP());

            const VCGEdge &edge = section.edge[currentEdge];
            const int first = vcg::tri::Index(section, edge.cV(0));
            const int second = vcg::tri::Index(section, edge.cV(1));
            const int nextVertex = first == currentVertex ? second : first;
            if (nextVertex == startVertex)
                break;

            const auto &nextIncident = incidentEdges[size_t(nextVertex)];
            currentEdge = nextIncident[0] == currentEdge
                ? nextIncident[1]
                : nextIncident[0];
            currentVertex = nextVertex;
        }
        if (contour.size() < 3)
            return QObject::tr("The planar section contains a degenerate contour.");
        contours.push_back(std::move(contour));
    }

    if (contours.empty())
        return QObject::tr("The planar section contains no closed contour to triangulate.");

    // Triangulated in the plane's own orthonormal frame, where the refinement's circumcenters
    // and angles are the true ones (an axis projection would distort them).
    const vcg::Point3d n = vcg::Point3d::Construct(normal).Normalize();
    vcg::Point3d u = std::abs(n[0]) < 0.9 ? vcg::Point3d(1, 0, 0) : vcg::Point3d(0, 1, 0);
    u = (u - n * (n * u)).Normalize();
    const vcg::Point3d v = n ^ u;
    std::vector<std::vector<vcg::Point2d>> planar;
    std::vector<vcg::Point3f> positions;
    for (const Contour &contour : contours) {
        planar.emplace_back();
        for (const vcg::Point3f &p : contour) {
            const vcg::Point3d q = vcg::Point3d::Construct(p);
            planar.back().push_back({ u * q, v * q });
            positions.push_back(p);
        }
    }
    const double offset = n * vcg::Point3d::Construct(contours[0][0]);
    std::vector<int> triangles;
    if (!vcg::TessellatePlanarContours2(planar, triangles, true))
        return QObject::tr("The planar section contours could not be triangulated.");
    std::vector<vcg::Point2d> points;
    for (const auto &c : planar) points.insert(points.end(), c.begin(), c.end());
    const size_t inputCount = points.size();
    std::vector<vcg::SteinerPoint> added;
    if (refinement.minAngle > 0)
        vcg::RefinePlanarTriangulation2(points, triangles, refinement, added);
    for (size_t k = 0; k < added.size(); ++k) {
        const vcg::SteinerPoint &h = added[k];
        if (h.c < 0)   // on a section edge: exactly on it
            positions.push_back(positions[size_t(h.a)] + (positions[size_t(h.b)] - positions[size_t(h.a)]) * float(h.t));
        else {
            const vcg::Point2d &q = points[inputCount + k];
            positions.push_back(vcg::Point3f::Construct(u * q[0] + v * q[1] + n * offset));
        }
    }

    cap.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(cap, positions.size());
    for (size_t i = 0; i < positions.size(); ++i)
        cap.vert[i].P() = positions[i];

    vcg::tri::Allocator<VCGMesh>::AddFaces(cap, triangles.size() / 3);
    for (size_t i = 0; i < triangles.size(); i += 3) {
        // Counterclockwise in the (u, v) frame, and u x v = n: every face already faces n.
        const int a = triangles[i], b = triangles[i + 1], c = triangles[i + 2];
        VCGFace &face = cap.face[i / 3];
        face.V(0) = &cap.vert[size_t(a)];
        face.V(1) = &cap.vert[size_t(b)];
        face.V(2) = &cap.vert[size_t(c)];
    }
    return {};
}

// Polygonal mesh used by Doo-Sabin/Catmull-Clark refinement.
class PEdge;
class PFace;
class PVertex;
struct PUsedTypes
    : public vcg::UsedTypes<
          vcg::Use<PVertex>::AsVertexType,
          vcg::Use<PEdge>::AsEdgeType,
          vcg::Use<PFace>::AsFaceType> {};

class PVertex
    : public vcg::Vertex<
          PUsedTypes,
          vcg::vertex::Coord3f,
          vcg::vertex::Normal3f,
          vcg::vertex::Qualityf,
          vcg::vertex::Color4b,
          vcg::vertex::BitFlags> {};

class PEdge
    : public vcg::Edge<
          PUsedTypes,
          vcg::edge::VertexRef,
          vcg::edge::BitFlags> {};

class PFace
    : public vcg::Face<
          PUsedTypes,
          vcg::face::PolyInfo,
          vcg::face::PFVAdj,
          vcg::face::PFFAdj,
          vcg::face::Color4b,
          vcg::face::BitFlags,
          vcg::face::Normal3f,
          vcg::face::WedgeTexCoord2f> {};

class PMesh : public vcg::tri::TriMesh<std::vector<PVertex>, std::vector<PEdge>, std::vector<PFace>> {};

using VertexPair = vcg::tri::BasicVertexPair<VCGVertex>;
using QuadricTemp = vcg::SimpleTempData<VCGMesh::VertContainer, vcg::math::Quadric<double>>;

class QHelper
{
public:
    static void Init() {}
    static vcg::math::Quadric<double> &Qd(VCGVertex &v) { return TD()[v]; }
    static vcg::math::Quadric<double> &Qd(VCGVertex *v) { return TD()[*v]; }
    static VCGVertex::ScalarType W(VCGVertex *) { return 1.0f; }
    static VCGVertex::ScalarType W(VCGVertex &) { return 1.0f; }
    static void Merge(VCGVertex &, const VCGVertex &) {}
    static QuadricTemp *&TDp()
    {
        static QuadricTemp *td = nullptr;
        return td;
    }
    static QuadricTemp &TD() { return *TDp(); }
};

class MyTriEdgeCollapse
    : public vcg::tri::TriEdgeCollapseQuadric<VCGMesh, VertexPair, MyTriEdgeCollapse, QHelper>
{
public:
    using Base = vcg::tri::TriEdgeCollapseQuadric<VCGMesh, VertexPair, MyTriEdgeCollapse, QHelper>;
    MyTriEdgeCollapse(const VertexPair &p, int i, vcg::BaseParameterClass *pp)
        : Base(p, i, pp)
    {
    }
};

class MyTriEdgeCollapseQTex
    : public vcg::tri::TriEdgeCollapseQuadricTex<
          VCGMesh,
          VertexPair,
          MyTriEdgeCollapseQTex,
          vcg::tri::QuadricTexHelper<VCGMesh>>
{
public:
    using Base = vcg::tri::TriEdgeCollapseQuadricTex<
        VCGMesh,
        VertexPair,
        MyTriEdgeCollapseQTex,
        vcg::tri::QuadricTexHelper<VCGMesh>>;
    MyTriEdgeCollapseQTex(const VertexPair &p, int i, vcg::BaseParameterClass *pp)
        : Base(p, i, pp)
    {
    }
};

constexpr QLatin1StringView kIdLoop("subdivide_by_loop");
constexpr QLatin1StringView kIdButterfly("subdivide_by_butterfly");
constexpr QLatin1StringView kIdClustering("simplify_by_vertex_clustering");
constexpr QLatin1StringView kIdQuadric("simplify_by_quadric_edge_collapse_vcglib");
constexpr QLatin1StringView kIdQuadricTex("simplify_by_quadric_edge_collapse_with_texture_vcglib");
constexpr QLatin1StringView kIdIsoRemesh("remesh_isotropically_vcglib");
constexpr QLatin1StringView kIdNormalExtrap("compute_point_cloud_normals");
constexpr QLatin1StringView kIdNormalSmoothPc("smooth_point_cloud_normals");
constexpr QLatin1StringView kIdCurvDir("compute_principal_curvature_directions_vcglib");
constexpr QLatin1StringView kIdSlicePlane("create_polyline_from_planar_section");
constexpr QLatin1StringView kIdTrimByPlane("trim_surface_by_plane");
constexpr QLatin1StringView kIdPerimeterPolyline("create_polyline_from_selection_perimeter");
constexpr QLatin1StringView kIdMidpoint("subdivide_by_midpoint");
constexpr QLatin1StringView kIdReorient("orient_faces_consistently_vcglib");
constexpr QLatin1StringView kIdFlipSwap("mirror_or_swap_axes");
constexpr QLatin1StringView kIdRotate("rotate");
constexpr QLatin1StringView kIdRotateFit("rotate_to_fitted_plane");
constexpr QLatin1StringView kIdScale("scale");
constexpr QLatin1StringView kIdCenter("translate");
constexpr QLatin1StringView kIdNormalizeFrame("normalize_reference_frame");
constexpr QLatin1StringView kIdInvertFaces("invert_face_orientation");
constexpr QLatin1StringView kIdFreeze("freeze_matrix");
constexpr QLatin1StringView kIdReset("set_matrix_to_identity");
constexpr QLatin1StringView kIdInvertTr("invert_matrix");
constexpr QLatin1StringView kIdSetParams("set_matrix_from_translation_rotation_scale");
constexpr QLatin1StringView kIdSetMatrix("set_matrix_from_values_or_layer");
constexpr QLatin1StringView kIdCloseHoles("close_holes");
constexpr QLatin1StringView kIdCylinderUnwrap("parametrize_by_cylindrical_projection");
constexpr QLatin1StringView kIdCatmull("subdivide_by_catmull_clark");
constexpr QLatin1StringView kIdDooSabin("subdivide_by_doo_sabin");
constexpr QLatin1StringView kIdHalfCatmull("convert_to_quads_by_4_8_subdivision");
constexpr QLatin1StringView kIdQuadDominant("convert_to_quad_dominant_mesh");
constexpr QLatin1StringView kIdMakePureTri("convert_to_pure_triangles");
constexpr QLatin1StringView kIdQuadPairing("convert_to_quads_by_triangle_pairing");
constexpr QLatin1StringView kIdFauxCrease("select_crease_edges_vcglib");
constexpr QLatin1StringView kIdFauxExtract("create_polyline_from_selected_edges");
constexpr QLatin1StringView kIdCutSelectedEdges("cut_along_selected_edges");
constexpr QLatin1StringView kIdEmbedPolyline("embed_polyline_in_surface");
constexpr QLatin1StringView kIdSmoothPolyline("smooth_polyline_on_surface");
constexpr QLatin1StringView kIdCutGraph("create_polyline_from_cut_graph");
constexpr QLatin1StringView kIdReebGraph("create_reeb_graph_from_vertex_scalar");
constexpr QLatin1StringView kIdHandleTunnel("create_handle_and_tunnel_loops");
constexpr QLatin1StringView kIdLoopSurface("create_surface_from_polyline_loop_hrbf");
constexpr QLatin1StringView kIdVAttrSeam("split_vertices_by_attribute_seam");
constexpr QLatin1StringView kIdLS3Loop("subdivide_by_ls3_loop");

int selectedFaceCount(const VCGMesh &mesh)
{
    int cnt = 0;
    for (const VCGFace &f : mesh.face) {
        if (f.IsS())
            ++cnt;
    }
    return cnt;
}

int selectedVertCount(const VCGMesh &mesh)
{
    int cnt = 0;
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsS())
            ++cnt;
    }
    return cnt;
}

static QMatrix4x4 vcgToQt(const vcg::Matrix44f &m)
{
    return QMatrix4x4(m[0][0], m[0][1], m[0][2], m[0][3],
                      m[1][0], m[1][1], m[1][2], m[1][3],
                      m[2][0], m[2][1], m[2][2], m[2][3],
                      m[3][0], m[3][1], m[3][2], m[3][3]);
}

static vcg::Matrix44f qtToVcg(const QMatrix4x4 &m)
{
    vcg::Matrix44f r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            r[row][col] = m(row, col);
    return r;
}

vcg::Box3f sceneBBox(const Document &doc, bool visibleOnly)
{
    vcg::Box3f bb;
    bb.SetNull();
    for (int i = 0; i < doc.meshCount(); ++i) {
        const Document::MeshEntry &entry = doc.mesh(i);
        if (visibleOnly && !entry.visible)
            continue;
        if (entry.mesh.bbox.IsNull())
            vcg::tri::UpdateBounding<VCGMesh>::Box(const_cast<VCGMesh &>(entry.mesh));
        const vcg::Matrix44f tr = qtToVcg(entry.transform);
        for (int c = 0; c < 8; ++c) {
            vcg::Point3f corner(
                (c & 1) ? entry.mesh.bbox.max.X() : entry.mesh.bbox.min.X(),
                (c & 2) ? entry.mesh.bbox.max.Y() : entry.mesh.bbox.min.Y(),
                (c & 4) ? entry.mesh.bbox.max.Z() : entry.mesh.bbox.min.Z());
            bb.Add(tr * corner);
        }
    }
    return bb;
}

bool buildReferenceSurfaceForIsotropicRemeshing(
    const Document &doc,
    int currentMeshIndex,
    int referenceMeshIndex,
    const VCGMesh &currentMesh,
    VCGMesh &referenceMesh,
    QString &errorMessage)
{
    referenceMesh.Clear();
    referenceMesh.face.EnableMark();

    if (referenceMeshIndex < 0 || referenceMeshIndex >= doc.meshCount()) {
        errorMessage = QObject::tr("Reference surface mesh index is invalid.");
        return false;
    }

    if (referenceMeshIndex == currentMeshIndex) {
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(referenceMesh, currentMesh);
        return true;
    }

    const Document::MeshEntry &currentEntry = doc.mesh(currentMeshIndex);
    const Document::MeshEntry &referenceEntry = doc.mesh(referenceMeshIndex);
    if (referenceEntry.mesh.FN() <= 0) {
        errorMessage = QObject::tr("Reference surface mesh '%1' has no faces.")
                           .arg(referenceEntry.name);
        return false;
    }

    bool invertible = false;
    const QMatrix4x4 currentToWorldInv = currentEntry.transform.inverted(&invertible);
    if (!invertible) {
        errorMessage = QObject::tr(
            "Cannot use another reference surface because the current mesh transform is not invertible.");
        return false;
    }

    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(referenceMesh, referenceEntry.mesh);
    const QMatrix4x4 referenceToCurrentLocal = currentToWorldInv * referenceEntry.transform;
    Document::transformMeshGeometry(referenceMesh, referenceToCurrentLocal);
    return true;
}

// Leave the filter's matrix on the current layer, composed on top of the layer's own or in
// its place. Writing it into the vertices is the framework's, when the call asks for Bake
// positions (MeshFilterDescriptor::transformResult).
void applyTransform(
    Document &doc,
    const vcg::Matrix44f &tr,
    bool compose,
    const QString &opName,
    QVector<int> &touched)
{
    touched.clear();
    const int i = doc.currentMeshIndex();
    if (i < 0 || i >= doc.meshCount())
        return;

    const Document::MeshEntry &entry = doc.mesh(i);
    const QMatrix4x4 matrix = compose ? vcgToQt(tr) * entry.transform : vcgToQt(tr);
    doc.setMeshTransform(i, matrix, QObject::tr("%1 on '%2'").arg(opName, entry.name));
    touched.push_back(i);
}

void quadricSimplification(
    VCGMesh &mesh,
    int targetFaceNum,
    bool selected,
    vcg::tri::TriEdgeCollapseQuadricParameter &pp,
    vcg::CallBackPos *cb)
{
    vcg::math::Quadric<double> qZero;
    qZero.SetZero();
    QuadricTemp td(mesh.vert, qZero);
    QHelper::TDp() = &td;

    if (selected) {
        vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);
        for (VCGVertex &v : mesh.vert) {
            if (!v.IsS())
                v.ClearW();
            else
                v.SetW();
        }
    }

    if (pp.PreserveBoundary && !selected) {
        pp.FastPreserveBoundary = true;
        pp.PreserveBoundary = false;
    }

    if (pp.NormalCheck)
        pp.NormalThrRad = float(M_PI / 4.0);

    vcg::LocalOptimization<VCGMesh> deciSession(mesh, &pp);
    if (cb)
        (*cb)(1, "Initializing simplification");
    deciSession.Init<MyTriEdgeCollapse>();

    if (selected)
        targetFaceNum = mesh.fn - (selectedFaceCount(mesh) - targetFaceNum);

    deciSession.SetTargetSimplices(targetFaceNum);
    deciSession.SetTimeBudget(0.1f);
    const int faceToDel = std::max(1, mesh.fn - targetFaceNum);
    while (deciSession.DoOptimization() && mesh.fn > targetFaceNum) {
        if (cb) {
            const int p = 100 - 100 * (mesh.fn - targetFaceNum) / faceToDel;
            if (!(*cb)(p, "Simplifying..."))
                break;
        }
    }

    deciSession.Finalize<MyTriEdgeCollapse>();

    if (selected) {
        for (VCGVertex &v : mesh.vert) {
            if (!v.IsD())
                v.SetW();
            if (v.IsS())
                v.ClearS();
        }
    }
    QHelper::TDp() = nullptr;
}

void quadricTexSimplification(
    VCGMesh &mesh,
    int targetFaceNum,
    bool selected,
    vcg::tri::TriEdgeCollapseQuadricTexParameter &pp,
    vcg::CallBackPos *cb)
{
    vcg::tri::UpdateNormal<VCGMesh>::PerFace(mesh);
    vcg::math::Quadric<double> qZero;
    qZero.SetZero();
    using QTH = vcg::tri::QuadricTexHelper<VCGMesh>;
    QTH::QuadricTemp td3(mesh.vert, qZero);
    QTH::TDp3() = &td3;

    std::vector<std::pair<vcg::TexCoord2<float>, vcg::Quadric5<double>>> qv;
    QTH::Quadric5Temp td(mesh.vert, qv);
    QTH::TDp() = &td;

    if (selected) {
        vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);
        for (VCGVertex &v : mesh.vert) {
            if (!v.IsS())
                v.ClearW();
            else
                v.SetW();
        }
    }

    vcg::LocalOptimization<VCGMesh> deciSession(mesh, &pp);
    if (cb)
        (*cb)(1, "Initializing simplification");
    deciSession.Init<MyTriEdgeCollapseQTex>();

    if (selected)
        targetFaceNum = mesh.fn - (selectedFaceCount(mesh) - targetFaceNum);

    deciSession.SetTargetSimplices(targetFaceNum);
    deciSession.SetTimeBudget(0.1f);
    const int faceToDel = std::max(1, mesh.fn - targetFaceNum);
    while (deciSession.DoOptimization() && mesh.fn > targetFaceNum) {
        if (cb) {
            const int p = 100 - 100 * (mesh.fn - targetFaceNum) / faceToDel;
            if (!(*cb)(p, "Simplifying textured mesh..."))
                break;
        }
    }

    deciSession.Finalize<MyTriEdgeCollapseQTex>();

    if (selected) {
        for (VCGVertex &v : mesh.vert) {
            if (!v.IsD())
                v.SetW();
            if (v.IsS())
                v.ClearS();
        }
    }

    QTH::TDp3() = nullptr;
    QTH::TDp() = nullptr;
}

MeshFilterRunResult fail(const QString &msg)
{
    return { false, false, msg };
}

MeshFilterRunResult success(bool modified = true, const QStringList &info = {}, const QVector<int> &newMeshes = {})
{
    MeshFilterRunResult r;
    r.success = true;
    r.documentModified = modified;
    r.infoMessages = info;
    r.newMeshIndices = newMeshes;
    return r;
}

MeshFilterRunResult qualitySuccess(
    int meshIndex,
    MeshFilterVisualizationAttribute attribute,
    const QStringList &info = {})
{
    MeshFilterRunResult r = success(true, info);
    r.visualizationHints.push_back({ meshIndex, attribute });
    return r;
}
}

QString MeshingFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.meshing");
}

QString MeshingFilterPlugin::name() const
{
    return QObject::tr("Meshing Filters");
}

MeshFilterRunResult MeshingFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int ci = doc.currentMeshIndex();
    if (ci < 0 || ci >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    auto &entry = doc.mesh(ci);
    auto &mesh = entry.mesh;
    using Mask = vcg::tri::io::Mask;

    auto markGeometry = [&](int idx, const QString &msg) {
        doc.markMeshGeometryChanged(idx, msg);
    };

    try {
        if (filterId == QString::fromLatin1(kIdLoop)
            || filterId == QString::fromLatin1(kIdButterfly)
            || filterId == QString::fromLatin1(kIdMidpoint)
            || filterId == QString::fromLatin1(kIdLS3Loop)) {
            if (mesh.FN() <= 0)
                return fail(QObject::tr("Current mesh has no faces."));

            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0) {
                return fail(QObject::tr("Subdivision surfaces require manifoldness."));
            }

            const bool selected = params.getBool(QStringLiteral("selectedOnly"));
            const float threshold = float(params.getDouble(QStringLiteral("Threshold")));
            const int iterations = std::max(1, params.getInt(QStringLiteral("Iterations")));
            const QString w = params.getEnum(QStringLiteral("LoopWeight"));
            vcg::CallBackPos *cb = doc.progressCallback();

            for (int i = 0; i < iterations; ++i) {
                if (filterId == QString::fromLatin1(kIdLoop)) {
                    if (w == QStringLiteral("regularity")) {
                        vcg::tri::RefineOddEven<VCGMesh>(
                            mesh,
                            vcg::tri::OddPointLoopGeneric<VCGMesh, vcg::tri::Centroid<VCGMesh>, vcg::tri::RegularLoopWeight<float>>(mesh),
                            vcg::tri::EvenPointLoopGeneric<VCGMesh, vcg::tri::Centroid<VCGMesh>, vcg::tri::RegularLoopWeight<float>>(),
                            threshold,
                            selected,
                            cb);
                    } else if (w == QStringLiteral("continuity")) {
                        vcg::tri::RefineOddEven<VCGMesh>(
                            mesh,
                            vcg::tri::OddPointLoopGeneric<VCGMesh, vcg::tri::Centroid<VCGMesh>, vcg::tri::ContinuityLoopWeight<float>>(mesh),
                            vcg::tri::EvenPointLoopGeneric<VCGMesh, vcg::tri::Centroid<VCGMesh>, vcg::tri::ContinuityLoopWeight<float>>(),
                            threshold,
                            selected,
                            cb);
                    } else {
                        vcg::tri::RefineOddEven<VCGMesh>(
                            mesh,
                            vcg::tri::OddPointLoop<VCGMesh>(mesh),
                            vcg::tri::EvenPointLoop<VCGMesh>(),
                            threshold,
                            selected,
                            cb);
                    }
                } else if (filterId == QString::fromLatin1(kIdButterfly)) {
                    vcg::tri::Refine<VCGMesh, vcg::tri::MidPointButterfly<VCGMesh>>(
                        mesh,
                        vcg::tri::MidPointButterfly<VCGMesh>(mesh),
                        threshold,
                        selected,
                        cb);
                } else if (filterId == QString::fromLatin1(kIdMidpoint)) {
                    vcg::tri::Refine<VCGMesh, vcg::tri::MidPoint<VCGMesh>>(
                        mesh,
                        vcg::tri::MidPoint<VCGMesh>(&mesh),
                        threshold,
                        selected,
                        cb);
                } else {
                    if (w == QStringLiteral("regularity")) {
                        vcg::tri::RefineOddEven<VCGMesh>(
                            mesh,
                            vcg::tri::OddPointLoopGeneric<VCGMesh, vcg::tri::LS3Projection<VCGMesh, double>, vcg::tri::RegularLoopWeight<double>>(mesh),
                            vcg::tri::EvenPointLoopGeneric<VCGMesh, vcg::tri::LS3Projection<VCGMesh, double>, vcg::tri::RegularLoopWeight<double>>(),
                            threshold,
                            selected,
                            cb);
                    } else if (w == QStringLiteral("continuity")) {
                        vcg::tri::RefineOddEven<VCGMesh>(
                            mesh,
                            vcg::tri::OddPointLoopGeneric<VCGMesh, vcg::tri::LS3Projection<VCGMesh, double>, vcg::tri::ContinuityLoopWeight<double>>(mesh),
                            vcg::tri::EvenPointLoopGeneric<VCGMesh, vcg::tri::LS3Projection<VCGMesh, double>, vcg::tri::ContinuityLoopWeight<double>>(),
                            threshold,
                            selected,
                            cb);
                    } else {
                        vcg::tri::RefineOddEven<VCGMesh>(
                            mesh,
                            vcg::tri::OddPointLoopGeneric<VCGMesh, vcg::tri::LS3Projection<VCGMesh, double>>(mesh),
                            vcg::tri::EvenPointLoopGeneric<VCGMesh, vcg::tri::LS3Projection<VCGMesh, double>>(),
                            threshold,
                            selected,
                            cb);
                    }
                }
            }
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            markGeometry(ci, QObject::tr("Applied subdivision on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdReorient)) {
            if (mesh.FN() <= 0)
                return fail(QObject::tr("Current mesh has no faces."));
            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0)
                return fail(QObject::tr("Orientability requires manifoldness."));
            bool oriented = false;
            bool orientable = false;
            vcg::tri::Clean<VCGMesh>::OrientCoherentlyMesh(mesh, oriented, orientable);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            markGeometry(ci, QObject::tr("Reoriented faces on '%1'").arg(entry.name));
            return success(true, { QObject::tr("Oriented: %1, Orientable: %2").arg(oriented).arg(orientable) });
        }

        if (filterId == QString::fromLatin1(kIdClustering)) {
            const float threshold = float(params.getDouble(QStringLiteral("Threshold")));
            vcg::tri::Clustering<VCGMesh, vcg::tri::AverageColorCell<VCGMesh>> grid(mesh.bbox, 100000, threshold);
            VCGMesh output;
            const int srcVN = mesh.VN();
            const int srcFN = mesh.FN();
            if (srcFN == 0) {
                grid.AddPointSet(mesh);
                grid.ExtractPointSet(output);
            } else {
                grid.AddMesh(mesh);
                grid.ExtractMesh(output);
            }
            vcg::tri::UpdateBounding<VCGMesh>::Box(output);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);
            const int newIndex = doc.addMesh(output, {}, entry.ioMask);
            if (newIndex >= 0)
                doc.mesh(newIndex).transform = doc.mesh(ci).transform;  // in the frame of the layer it came from
            return success(true,
                { QObject::tr("Clustering decimation: %1 → %2 vertices, %3 → %4 faces.")
                    .arg(srcVN).arg(output.VN()).arg(srcFN).arg(output.FN()) },
                { newIndex });
        }

        if (filterId == QString::fromLatin1(kIdInvertFaces)) {
            const bool forceFlip = params.getBool(QStringLiteral("forceFlip"));
            const bool onlySel = params.getBool(QStringLiteral("selectedOnly"));
            // Automatic orientation is a decision about the whole surface, so it cannot be
            // confined to part of it without flipping faces outside the selection.
            if (onlySel && !forceFlip)
                return fail(QObject::tr("Automatic orientation decides for the whole mesh: "
                                        "turn on Force Flip to flip only the selected faces."));
            if (forceFlip)
                vcg::tri::Clean<VCGMesh>::FlipMesh(mesh, onlySel);
            else
                vcg::tri::Clean<VCGMesh>::FlipNormalOutside(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            markGeometry(ci, QObject::tr("Inverted face orientation on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdQuadric)) {
            // Both targets are read against what is being simplified: with selectedOnly the
            // selection, whose face count TargetFaceNum is (quadricSimplification turns it
            // into a whole-mesh target), so a percentage has to be of the selection too.
            const bool selected = params.getBool(QStringLiteral("selectedOnly"));
            const int simplifiedFaces = selected ? selectedFaceCount(mesh) : mesh.FN();
            int targetFaceNum = params.getInt(QStringLiteral("TargetFaceNum"));
            const float targetPerc = float(params.getDouble(QStringLiteral("TargetPerc")));
            if (targetPerc > 0.0f)
                targetFaceNum = int(std::round(simplifiedFaces * targetPerc));
            targetFaceNum = std::clamp(targetFaceNum, 1, std::max(1, simplifiedFaces));

            vcg::tri::TriEdgeCollapseQuadricParameter pp;
            pp.QualityThr = float(params.getDouble(QStringLiteral("QualityThr")));
            pp.PreserveBoundary = params.getBool(QStringLiteral("PreserveBoundary"));
            pp.BoundaryQuadricWeight = pp.BoundaryQuadricWeight * float(params.getDouble(QStringLiteral("BoundaryWeight")));
            pp.PreserveTopology = params.getBool(QStringLiteral("PreserveTopology"));
            pp.QualityWeight = params.getBool(QStringLiteral("QualityWeight"));
            pp.NormalCheck = params.getBool(QStringLiteral("PreserveNormal"));
            pp.OptimalPlacement = params.getBool(QStringLiteral("OptimalPlacement"));
            pp.QualityQuadric = params.getBool(QStringLiteral("PlanarQuadric"));
            pp.QualityQuadricWeight = float(params.getDouble(QStringLiteral("PlanarWeight")));

            quadricSimplification(mesh, targetFaceNum, selected, pp, doc.progressCallback());

            if (params.getBool(QStringLiteral("AutoClean"))) {
                vcg::tri::Clean<VCGMesh>::RemoveFaceOutOfRangeArea(mesh, 0);
                vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(mesh);
                vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
            }

            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
            markGeometry(ci, QObject::tr("Applied quadric simplification on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdQuadricTex)) {
            if (!vcg::tri::Clean<VCGMesh>::HasConsistentPerWedgeTexCoord(mesh))
                return fail(QObject::tr("Mesh has inconsistent per-wedge texture coordinates."));

            // Both targets are read against what is being simplified: with selectedOnly the
            // selection, whose face count TargetFaceNum is (quadricSimplification turns it
            // into a whole-mesh target), so a percentage has to be of the selection too.
            const bool selected = params.getBool(QStringLiteral("selectedOnly"));
            const int simplifiedFaces = selected ? selectedFaceCount(mesh) : mesh.FN();
            int targetFaceNum = params.getInt(QStringLiteral("TargetFaceNum"));
            const float targetPerc = float(params.getDouble(QStringLiteral("TargetPerc")));
            if (targetPerc > 0.0f)
                targetFaceNum = int(std::round(simplifiedFaces * targetPerc));
            targetFaceNum = std::clamp(targetFaceNum, 1, std::max(1, simplifiedFaces));

            vcg::tri::TriEdgeCollapseQuadricTexParameter pp;
            pp.QualityThr = float(params.getDouble(QStringLiteral("QualityThr")));
            pp.ExtraTCoordWeight = float(params.getDouble(QStringLiteral("Extratcoordw")));
            pp.OptimalPlacement = params.getBool(QStringLiteral("OptimalPlacement"));
            pp.PreserveBoundary = params.getBool(QStringLiteral("PreserveBoundary"));
            pp.BoundaryWeight = pp.BoundaryWeight * float(params.getDouble(QStringLiteral("BoundaryWeight")));
            pp.QualityQuadric = params.getBool(QStringLiteral("PlanarQuadric"));
            pp.NormalCheck = params.getBool(QStringLiteral("PreserveNormal"));

            quadricTexSimplification(mesh, targetFaceNum, selected, pp, doc.progressCallback());
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
            markGeometry(ci, QObject::tr("Applied textured quadric simplification on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdIsoRemesh)) {
            vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(mesh);
            vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
            vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
            vcg::tri::UpdateFlags<VCGMesh>::FaceClearF(mesh); // remove faux edges
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);

            VCGMeshFFAdjScope _ffAdj(mesh);
            VCGMeshVFAdjScope _vfAdj(mesh);
            VCGMeshMarkScope _mark(mesh);
            VCGMeshVertexMarkScope _vertMark(mesh);

            const int referenceMeshIndex = params.getMesh(QStringLiteral("ReferenceMesh"), ci);
            VCGMesh toProjectCopy;
            QString referenceError;
            if (!buildReferenceSurfaceForIsotropicRemeshing(
                    doc,
                    ci,
                    referenceMeshIndex,
                    mesh,
                    toProjectCopy,
                    referenceError)) {
                return fail(referenceError);
            }

            vcg::tri::IsotropicRemeshing<VCGMesh>::Params remeshParams;
            remeshParams.SetTargetLen(float(params.getDouble(QStringLiteral("TargetLen"))));
            remeshParams.SetFeatureAngleDeg(float(params.getDouble(QStringLiteral("FeatureDeg"))));
            remeshParams.maxSurfDist = float(params.getDouble(QStringLiteral("MaxSurfDist")));
            remeshParams.iter = std::max(1, params.getInt(QStringLiteral("Iterations")));
            remeshParams.adapt = params.getBool(QStringLiteral("Adaptive"));
            remeshParams.selectedOnly = params.getBool(QStringLiteral("selectedOnly"));
            remeshParams.splitFlag = params.getBool(QStringLiteral("SplitFlag"));
            remeshParams.collapseFlag = params.getBool(QStringLiteral("CollapseFlag"));
            remeshParams.swapFlag = params.getBool(QStringLiteral("SwapFlag"));
            remeshParams.smoothFlag = params.getBool(QStringLiteral("SmoothFlag"));
            remeshParams.projectFlag = params.getBool(QStringLiteral("ReprojectFlag"));
            remeshParams.surfDistCheck = params.getBool(QStringLiteral("CheckSurfDist"));

            vcg::tri::IsotropicRemeshing<VCGMesh>::Do(mesh, toProjectCopy, remeshParams, doc.progressCallback());
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            markGeometry(ci, QObject::tr("Applied isotropic remeshing on '%1'").arg(entry.name));
            QStringList info;
            if (referenceMeshIndex != ci) {
                info << QObject::tr("Used '%1' as reference surface for distance checks and reprojection.")
                            .arg(doc.mesh(referenceMeshIndex).name);
            }
            return success(true, info);
        }

        auto applyTransformAndReport = [&](const vcg::Matrix44f &tr, const QString &opName, bool compose = true) {
            QVector<int> touched;
            applyTransform(doc, tr, compose, opName, touched);
            return success(!touched.isEmpty(), { QObject::tr("Affected layers: %1").arg(touched.size()) });
        };

        if (filterId == QString::fromLatin1(kIdFlipSwap)) {
            vcg::Matrix44f tr;
            tr.SetIdentity();
            if (params.getBool(QStringLiteral("flipX"))) {
                vcg::Matrix44f m; m.SetIdentity(); m[0][0] = -1.0f; tr *= m;
            }
            if (params.getBool(QStringLiteral("flipY"))) {
                vcg::Matrix44f m; m.SetIdentity(); m[1][1] = -1.0f; tr *= m;
            }
            if (params.getBool(QStringLiteral("flipZ"))) {
                vcg::Matrix44f m; m.SetIdentity(); m[2][2] = -1.0f; tr *= m;
            }
            if (params.getBool(QStringLiteral("swapXY"))) {
                vcg::Matrix44f m; m.SetIdentity(); m[0][0] = 0; m[0][1] = 1; m[1][0] = 1; m[1][1] = 0; tr *= m;
            }
            if (params.getBool(QStringLiteral("swapXZ"))) {
                vcg::Matrix44f m; m.SetIdentity(); m[0][0] = 0; m[0][2] = 1; m[2][0] = 1; m[2][2] = 0; tr *= m;
            }
            if (params.getBool(QStringLiteral("swapYZ"))) {
                vcg::Matrix44f m; m.SetIdentity(); m[1][1] = 0; m[1][2] = 1; m[2][1] = 1; m[2][2] = 0; tr *= m;
            }
            return applyTransformAndReport(tr, QObject::tr("Flip/Swap axes"));
        }

        if (filterId == QString::fromLatin1(kIdRotate)) {
            const QString axisMode = params.getEnum(QStringLiteral("rotAxis"));
            vcg::Point3f axis(1, 0, 0);
            if (axisMode == QStringLiteral("y"))
                axis = { 0, 1, 0 };
            else if (axisMode == QStringLiteral("z"))
                axis = { 0, 0, 1 };
            else if (axisMode == QStringLiteral("custom")) {
                const QVector3D av = params.getPoint3f(QStringLiteral("customAxis"));
                axis = { float(av.x()), float(av.y()), float(av.z()) };
            }

            const float n2 = axis.SquaredNorm();
            if (n2 <= 1e-20f)
                return fail(QObject::tr("Custom rotation axis must be non-zero."));
            axis /= std::sqrt(n2);

            vcg::Point3f center(0, 0, 0);
            const QString centerMode = params.getEnum(QStringLiteral("rotCenter"));
            if (centerMode == QStringLiteral("bbox_center")) {
                // mesh.bbox is the *untransformed* local box, while applyTransform()
                // composes this matrix on the left of the layer transform and therefore
                // applies it in world space. Using the local centre directly pivots
                // around the wrong point on any layer that has been moved.
                center = qtToVcg(entry.transform) * mesh.bbox.Center();
            }
            else if (centerMode == QStringLiteral("custom")) {
                const QVector3D cv = params.getPoint3f(QStringLiteral("customCenter"));
                center = { float(cv.x()), float(cv.y()), float(cv.z()) };
            }

            float angleDeg = float(params.getDouble(QStringLiteral("angle")));
            if (params.getBool(QStringLiteral("snapFlag"))) {
                const float snap = float(params.getDouble(QStringLiteral("snapAngle")));
                if (std::fabs(snap) > 1e-12f)
                    angleDeg = std::floor(angleDeg / snap) * snap;
            }

            vcg::Matrix44f trRot;
            trRot.SetRotateDeg(angleDeg, axis);
            vcg::Matrix44f trT, trInvT;
            trT.SetTranslate(center);
            trInvT.SetTranslate(-center);
            vcg::Matrix44f tr = trT * trRot * trInvT;
            return applyTransformAndReport(tr, QObject::tr("Rotate"));
        }

        if (filterId == QString::fromLatin1(kIdRotateFit)) {
            if (selectedVertCount(mesh) == 0 && selectedFaceCount(mesh) == 0)
                return fail(QObject::tr("Cannot compute rotation: there is no selection."));

            if (selectedVertCount(mesh) == 0 && selectedFaceCount(mesh) > 0) {
                vcg::tri::UpdateSelection<VCGMesh>::VertexClear(mesh);
                vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceLoose(mesh);
            }

            vcg::Box3f selBox;
            selBox.SetNull();
            std::vector<vcg::Point3f> selectedPts;
            selectedPts.reserve(static_cast<size_t>(std::max(1, selectedVertCount(mesh))));
            for (VCGVertex &v : mesh.vert) {
                if (!v.IsS())
                    continue;
                selBox.Add(v.P());
                selectedPts.push_back(v.P());
            }
            if (selectedPts.empty())
                return fail(QObject::tr("Cannot compute rotation: empty selected vertices."));

            vcg::Plane3f plane;
            vcg::FitPlaneToPointSet(selectedPts, plane);

            vcg::Point3f targetPlane(0, 0, 1);
            const QString tplane = params.getEnum(QStringLiteral("targetPlane"));
            if (tplane == QStringLiteral("yz"))
                targetPlane = { 1, 0, 0 };
            else if (tplane == QStringLiteral("zx"))
                targetPlane = { 0, 1, 0 };

            vcg::Point3f rotAxis = targetPlane ^ plane.Direction();
            float angleRad = vcg::Angle(targetPlane, plane.Direction());

            const QString raxis = params.getEnum(QStringLiteral("rotAxis"));
            if (raxis != QStringLiteral("any")) {
                vcg::Point3f projDir;
                if (raxis == QStringLiteral("x")) {
                    rotAxis = -vcg::Point3f(1, 0, 0);
                    projDir = { 0, plane.Direction().Y(), plane.Direction().Z() };
                } else if (raxis == QStringLiteral("y")) {
                    rotAxis = -vcg::Point3f(0, 1, 0);
                    projDir = { plane.Direction().X(), 0, plane.Direction().Z() };
                } else {
                    rotAxis = -vcg::Point3f(0, 0, 1);
                    projDir = { plane.Direction().X(), plane.Direction().Y(), 0 };
                }
                angleRad = vcg::Angle(targetPlane, projDir);
                const float angleSign = (targetPlane ^ projDir) * rotAxis;
                if (angleSign < 0)
                    angleRad = -angleRad;
                else if (angleSign == 0)
                    angleRad = 0;
            }

            const float rn2 = rotAxis.SquaredNorm();
            if (rn2 <= 1e-20f)
                return fail(QObject::tr("Cannot compute fitting rotation axis."));
            rotAxis /= std::sqrt(rn2);

            vcg::Matrix44f rt;
            rt.SetRotateRad(-angleRad, rotAxis);
            vcg::Matrix44f tr = rt;
            if (params.getBool(QStringLiteral("ToOrigin"))) {
                vcg::Matrix44f t;
                t.SetTranslate(-selBox.Center());
                tr = rt * t;
            }
            return applyTransformAndReport(tr, QObject::tr("Rotate to fit"));
        }

        if (filterId == QString::fromLatin1(kIdNormalizeFrame)) {
            if (mesh.VN() <= 0)
                return fail(QObject::tr("Current mesh has no vertices."));
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);

            const QString positionMode = params.getEnum(QStringLiteral("position"));
            const QString rotationMode = params.getEnum(QStringLiteral("rotation"));
            const QString scaleMode    = params.getEnum(QStringLiteral("scale"));
            const double  minSeparation = params.getDouble(QStringLiteral("minAxisSeparation"));
            QStringList notes;

            // ---- the centre everything else pivots about -------------------------------
            vcg::Point3f centre = mesh.bbox.Center();
            if (positionMode == QLatin1StringView("vertex_average")) {
                vcg::Point3f sum(0, 0, 0);
                int n = 0;
                for (const VCGVertex &v : mesh.vert) {
                    if (v.IsD())
                        continue;
                    sum += v.cP();
                    ++n;
                }
                if (n == 0)
                    return fail(QObject::tr("Current mesh has no vertices."));
                centre = sum / float(n);
            }
            else if (positionMode == QLatin1StringView("shell_barycenter")) {
                if (mesh.FN() <= 0)
                    return fail(QObject::tr("The shell barycenter needs faces; use the vertex average for a point cloud."));
                centre = vcg::tri::Stat<VCGMesh>::ComputeShellBarycenter(mesh);
            }
            else if (positionMode == QLatin1StringView("mesh_barycenter")) {
                if (mesh.FN() <= 0 || !vcg::tri::Clean<VCGMesh>::IsWaterTight(mesh)) {
                    return fail(QObject::tr(
                        "The mesh barycenter is the centre of mass of the enclosed solid, so it needs a "
                        "watertight mesh. Use the shell barycenter instead."));
                }
                vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
                const vcg::tri::Inertia<VCGMesh> inertia(mesh);
                centre = inertia.CenterOfMass();
            }

            // ---- rotation -------------------------------------------------------------
            vcg::Matrix33f rot;
            rot.SetIdentity();
            if (rotationMode != QLatin1StringView("unchanged")) {
                const bool areaWeighted = rotationMode == QLatin1StringView("pca_area_weighted");
                if (areaWeighted && mesh.FN() <= 0)
                    return fail(QObject::tr("Area weighted principal axes need faces; use the vertex variant for a point cloud."));

                Eigen::Matrix3d cov = Eigen::Matrix3d::Zero();
                double weightSum = 0.0;
                auto outer = [](const vcg::Point3f &d) {
                    const Eigen::Vector3d e(double(d.X()), double(d.Y()), double(d.Z()));
                    return Eigen::Matrix3d(e * e.transpose());
                };
                if (areaWeighted) {
                    // Exact second moment of a triangle about `centre`, by the parallel axis
                    // theorem: (A/12)*sum_i (v_i-g)(v_i-g)^T + A*(g-centre)(g-centre)^T.
                    for (const VCGFace &f : mesh.face) {
                        if (f.IsD())
                            continue;
                        const double area = double(vcg::DoubleArea(f)) * 0.5;
                        if (area <= 0.0)
                            continue;
                        const vcg::Point3f g = vcg::Barycenter(f);
                        Eigen::Matrix3d local = Eigen::Matrix3d::Zero();
                        for (int k = 0; k < 3; ++k)
                            local += outer(f.cP(k) - g);
                        cov += local * (area / 12.0) + outer(g - centre) * area;
                        weightSum += area;
                    }
                } else {
                    for (const VCGVertex &v : mesh.vert) {
                        if (v.IsD())
                            continue;
                        cov += outer(v.cP() - centre);
                        weightSum += 1.0;
                    }
                }
                if (weightSum <= 0.0)
                    return fail(QObject::tr("Cannot derive principal axes from an empty mesh."));
                cov /= weightSum;

                Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(cov);
                if (eig.info() != Eigen::Success)
                    return fail(QObject::tr("Failed to compute the principal-axis eigen decomposition."));

                // SelfAdjointEigenSolver returns ascending eigenvalues; we want the widest
                // spread on X.
                int order[3] = { 2, 1, 0 };
                const Eigen::Vector3d lambda = eig.eigenvalues();
                vcg::Point3f axis[3];
                for (int k = 0; k < 3; ++k) {
                    const Eigen::Vector3d c = eig.eigenvectors().col(order[k]);
                    axis[k] = vcg::Point3f(float(c[0]), float(c[1]), float(c[2]));
                    axis[k].Normalize();
                }

                const double l0 = lambda[order[0]], l1 = lambda[order[1]], l2 = lambda[order[2]];
                bool skipRotation = l0 <= 0.0;
                if (!skipRotation && minSeparation > 0.0) {
                    const double sep0 = (l0 - l1) / l0;
                    const double sep1 = (l1 > 0.0) ? (l1 - l2) / l1 : 0.0;
                    skipRotation = std::min(sep0, sep1) < minSeparation;
                }

                if (skipRotation) {
                    notes << QObject::tr(
                        "Principal axes are too close to being equal, so the rotation was skipped. "
                        "Lower the minimum axis separation to rotate anyway.");
                } else {
                    // Principal axes are only defined up to sign. Fix the first two by the sign
                    // of the third moment along them -- a shape with any asymmetry then lands
                    // the same way whatever its input orientation -- and take the third as the
                    // cross product, which makes the frame right handed by construction.
                    for (int k = 0; k < 2; ++k) {
                        double m3 = 0.0;
                        if (areaWeighted) {
                            for (const VCGFace &f : mesh.face) {
                                if (f.IsD())
                                    continue;
                                const double area = double(vcg::DoubleArea(f)) * 0.5;
                                const double t = double((vcg::Barycenter(f) - centre) * axis[k]);
                                m3 += area * t * t * t;
                            }
                        } else {
                            for (const VCGVertex &v : mesh.vert) {
                                if (v.IsD())
                                    continue;
                                const double t = double((v.cP() - centre) * axis[k]);
                                m3 += t * t * t;
                            }
                        }
                        if (m3 < 0.0) {
                            axis[k] = -axis[k];
                        } else if (std::abs(m3) <= 1e-12) {
                            // Symmetric along this axis, so the third moment cannot choose.
                            // Fall back on something deterministic: make the dominant
                            // component positive.
                            int dom = 0;
                            for (int c = 1; c < 3; ++c)
                                if (std::abs(axis[k][c]) > std::abs(axis[k][dom]))
                                    dom = c;
                            if (axis[k][dom] < 0.0f)
                                axis[k] = -axis[k];
                        }
                    }
                    axis[2] = axis[0] ^ axis[1];
                    for (int r = 0; r < 3; ++r)
                        for (int c = 0; c < 3; ++c)
                            rot[r][c] = axis[r][c];
                }
            }

            // ---- scale, measured in the rotated frame ---------------------------------
            float scaleFactor = 1.0f;
            if (scaleMode != QLatin1StringView("unchanged")) {
                vcg::Point3f lo(std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max());
                vcg::Point3f hi = -lo;
                float maxRadius = 0.0f;
                for (const VCGVertex &v : mesh.vert) {
                    if (v.IsD())
                        continue;
                    const vcg::Point3f q = rot * (v.cP() - centre);
                    for (int c = 0; c < 3; ++c) {
                        lo[c] = std::min(lo[c], q[c]);
                        hi[c] = std::max(hi[c], q[c]);
                    }
                    maxRadius = std::max(maxRadius, q.Norm());
                }
                const vcg::Point3f extent = hi - lo;
                float reference = 0.0f;
                if (scaleMode == QLatin1StringView("unit_longest_side"))
                    reference = std::max({ extent.X(), extent.Y(), extent.Z() });
                else if (scaleMode == QLatin1StringView("unit_diagonal"))
                    reference = extent.Norm();
                else
                    reference = maxRadius;
                if (!(reference > 1e-12f))
                    return fail(QObject::tr("Cannot normalize the scale of a degenerate mesh."));
                scaleFactor = 1.0f / reference;
            }

            // ---- compose: M = T(target) * S * R * T(-centre) ---------------------------
            const bool keepPosition = positionMode == QLatin1StringView("unchanged");
            vcg::Matrix44f toOrigin;
            toOrigin.SetTranslate(-centre);
            vcg::Matrix44f rot4;
            rot4.SetIdentity();
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 3; ++c)
                    rot4[r][c] = rot[r][c];
            vcg::Matrix44f scale4;
            scale4.SetIdentity();
            scale4[0][0] = scale4[1][1] = scale4[2][2] = scaleFactor;
            vcg::Matrix44f back;
            back.SetTranslate(keepPosition ? centre : vcg::Point3f(0, 0, 0));

            const vcg::Matrix44f tr = back * scale4 * rot4 * toOrigin;

            QVector<int> touched;
            applyTransform(doc, tr, true, QObject::tr("Normalize reference frame"), touched);
            notes << QObject::tr("Affected layers: %1").arg(touched.size());
            return success(!touched.isEmpty(), notes);
        }

        if (filterId == QString::fromLatin1(kIdCenter)) {
            const QVector3D axv = params.getPoint3f(QStringLiteral("axis"));
            vcg::Point3f translation(float(axv.x()), float(axv.y()), float(axv.z()));

            const QString method = params.getEnum(QStringLiteral("traslMethod"));
            if (method == QStringLiteral("scene_bbox"))
                translation = -sceneBBox(doc, true).Center();
            else if (method == QStringLiteral("new_origin")) {
                const QVector3D nov = params.getPoint3f(QStringLiteral("newOrigin"));
                translation = -vcg::Point3f(float(nov.x()), float(nov.y()), float(nov.z()));
            }

            vcg::Matrix44f tr;
            tr.SetTranslate(translation);
            return applyTransformAndReport(tr, QObject::tr("Translate/Center"));
        }

        if (filterId == QString::fromLatin1(kIdScale)) {
            vcg::Box3f sb = mesh.bbox;

            float sx = float(params.getDouble(QStringLiteral("axisX")));
            float sy = float(params.getDouble(QStringLiteral("axisY")));
            float sz = float(params.getDouble(QStringLiteral("axisZ")));
            if (params.getBool(QStringLiteral("uniformFlag")))
                sy = sz = sx;
            vcg::Point3f c(0, 0, 0);
            const QString centerMode = params.getEnum(QStringLiteral("scaleCenter"));
            if (centerMode == QStringLiteral("bbox_center")) {
                // World space, for the same reason as the rotation centre above.
                c = qtToVcg(entry.transform) * sb.Center();
            }
            else if (centerMode == QStringLiteral("custom")) {
                const QVector3D cv = params.getPoint3f(QStringLiteral("customCenter"));
                c = { float(cv.x()), float(cv.y()), float(cv.z()) };
            }

            vcg::Matrix44f s;
            s.SetScale(sx, sy, sz);
            vcg::Matrix44f t, it;
            t.SetTranslate(c);
            it.SetTranslate(-c);
            vcg::Matrix44f tr = t * s * it;
            return applyTransformAndReport(tr, QObject::tr("Scale"));
        }

        if (filterId == QString::fromLatin1(kIdReset)) {
            // Reset the per-mesh transform to the identity matrix.
            QMatrix4x4 identity;
            identity.setToIdentity();
            doc.setMeshTransform(ci, identity);
            return success(true, { QObject::tr("Transform reset on current layer.") });
        }

        if (filterId == QString::fromLatin1(kIdFreeze)) {
            doc.freezeMeshTransform(ci, QObject::tr("Freeze transform on '%1'").arg(entry.name));
            return success(true, { QObject::tr("Transform frozen to vertices on current layer.") });
        }

        if (filterId == QString::fromLatin1(kIdInvertTr)) {
            // Invert the current per-mesh transform matrix.
            bool invertible = false;
            const QMatrix4x4 inv = entry.transform.inverted(&invertible);
            if (!invertible)
                return fail(QObject::tr("Current transform matrix is not invertible."));
            doc.setMeshTransform(ci, inv, QObject::tr("Invert transform on '%1'").arg(entry.name));
            return success(true, { QObject::tr("Transform inverted on current layer.") });
        }

        if (filterId == QString::fromLatin1(kIdSetParams)) {
            const float tx = float(params.getDouble(QStringLiteral("translationX")));
            const float ty = float(params.getDouble(QStringLiteral("translationY")));
            const float tz = float(params.getDouble(QStringLiteral("translationZ")));
            const float rx = float(params.getDouble(QStringLiteral("rotationX")));
            const float ry = float(params.getDouble(QStringLiteral("rotationY")));
            const float rz = float(params.getDouble(QStringLiteral("rotationZ")));
            const float sx = float(params.getDouble(QStringLiteral("scaleX")));
            const float sy = float(params.getDouble(QStringLiteral("scaleY")));
            const float sz = float(params.getDouble(QStringLiteral("scaleZ")));

            vcg::Matrix44f tr;
            tr.SetIdentity();
            vcg::Matrix44f tt;
            tt.SetTranslate(tx, ty, tz);
            tr = tr * tt;
            if (rx != 0.0f || ry != 0.0f || rz != 0.0f) {
                tt.FromEulerAngles(vcg::math::ToRad(rx), vcg::math::ToRad(ry), vcg::math::ToRad(rz));
                tr = tr * tt;
            }
            if (sx != 0.0f || sy != 0.0f || sz != 0.0f) {
                tt.SetScale(sx, sy, sz);
                tr = tr * tt;
            }
            return applyTransformAndReport(
                tr, QObject::tr("Set transform from parameters"), params.getBool(QStringLiteral("compose")));
        }

        if (filterId == QString::fromLatin1(kIdSetMatrix)) {
            vcg::Matrix44f tr;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    tr[r][c] = float(params.getDouble(QStringLiteral("m%1%2").arg(r).arg(c)));
            return applyTransformAndReport(
                tr, QObject::tr("Set transform matrix"), params.getBool(QStringLiteral("compose")));
        }

        if (filterId == QString::fromLatin1(kIdNormalExtrap)) {
            vcg::tri::PointCloudNormal<VCGMesh>::Param p;
            p.fittingAdjNum = std::max(1, params.getInt(QStringLiteral("K")));
            p.smoothingIterNum = std::max(0, params.getInt(QStringLiteral("smoothIter")));
            p.useViewPoint = params.getBool(QStringLiteral("flipFlag"));
            const QVector3D vpv = params.getPoint3f(QStringLiteral("viewPos"));
            p.viewPoint = { float(vpv.x()), float(vpv.y()), float(vpv.z()) };
            vcg::tri::PointCloudNormal<VCGMesh>::Compute(mesh, p, doc.progressCallback());
            entry.ioMask |= Mask::IOM_VERTNORMAL;
            doc.markMeshMaterialChanged(ci, QObject::tr("Computed point-set normals for '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdNormalSmoothPc)) {
            vcg::tri::Smooth<VCGMesh>::VertexNormalPointCloud(mesh, std::max(1, params.getInt(QStringLiteral("K"))), 1);
            entry.ioMask |= Mask::IOM_VERTNORMAL;
            doc.markMeshMaterialChanged(ci, QObject::tr("Smoothed point-set normals for '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdCurvDir)) {
            const float scale = float(params.getDouble(QStringLiteral("Scale")));
            VCGMeshFFAdjScope _ffAdj(mesh);
            VCGMeshVFAdjScope _vfAdj(mesh);
            vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
            vcg::tri::UpdateTopology<VCGMesh>::VertexFace(mesh);
            mesh.vert.EnableCurvatureDir();
            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0)
                return fail(QObject::tr("Cannot compute principal directions on non-manifold faces."));

            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);

            const QString method = params.getEnum(QStringLiteral("Method"));
            const RandomSeed seed = params.getRandomSeed();
            if (method == QStringLiteral("taubin")) {
                vcg::tri::UpdateCurvature<VCGMesh>::PrincipalDirections(mesh);
            } else if (method == QStringLiteral("pca")) {
                // PrincipalDirectionsPCA Monte-Carlo samples the surface to build its
                // neighbourhood grid; the other methods here are deterministic.
                vcg::tri::SurfaceSampling<VCGMesh, vcg::tri::TrivialSampler<VCGMesh>>
                    ::SamplingRandomGenerator().initialize(seed.value);
                vcg::tri::UpdateCurvature<VCGMesh>::PrincipalDirectionsPCA(mesh, scale, true, doc.progressCallback());
            } else if (method == QStringLiteral("normal_cycle")) {
                vcg::tri::UpdateCurvature<VCGMesh>::PrincipalDirectionsNormalCycle(mesh);
            } else if (method == QStringLiteral("sd_quadric")) {
                vcg::tri::UpdateCurvatureFitting<VCGMesh>::updateCurvatureLocal(mesh, scale, doc.progressCallback());
            } else {
                vcg::tri::UpdateCurvatureFitting<VCGMesh>::computeCurvature(mesh);
            }

            const QString cm = params.getEnum(QStringLiteral("CurvColorMethod"));
            if (cm == QStringLiteral("gaussian"))
                vcg::tri::UpdateQuality<VCGMesh>::VertexGaussianFromCurvatureDir(mesh);
            else if (cm == QStringLiteral("min"))
                vcg::tri::UpdateQuality<VCGMesh>::VertexMinCurvFromCurvatureDir(mesh);
            else if (cm == QStringLiteral("max"))
                vcg::tri::UpdateQuality<VCGMesh>::VertexMaxCurvFromCurvatureDir(mesh);
            else if (cm == QStringLiteral("shape"))
                vcg::tri::UpdateQuality<VCGMesh>::VertexShapeIndexFromCurvatureDir(mesh);
            else if (cm == QStringLiteral("curvedness"))
                vcg::tri::UpdateQuality<VCGMesh>::VertexCurvednessFromCurvatureDir(mesh);
            else if (cm == QStringLiteral("none"))
                vcg::tri::UpdateQuality<VCGMesh>::VertexConstant(mesh, 0);
            else
                vcg::tri::UpdateQuality<VCGMesh>::VertexMeanFromCurvatureDir(mesh);

            entry.ioMask |= Mask::IOM_VERTQUALITY;
            doc.markMeshGeometryChanged(ci, QObject::tr("Computed principal curvature directions for '%1'").arg(entry.name));
            return qualitySuccess(
                ci,
                MeshFilterVisualizationAttribute::VertexQuality,
                method == QStringLiteral("pca") ? QStringList{ seed.message() } : QStringList{});
        }

        if (filterId == QString::fromLatin1(kIdCloseHoles)) {
            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0)
                return fail(QObject::tr("Hole closing requires edge-manifold mesh."));

            const size_t originalSize = mesh.face.size();
            const int maxHoleSize = std::max(1, params.getInt(QStringLiteral("MaxHoleSize")));
            const bool selectedFlag = params.getBool(QStringLiteral("Selected"));
            const bool selfInter = params.getBool(QStringLiteral("SelfIntersection"));
            const bool newFaceSel = params.getBool(QStringLiteral("NewFaceSelected"));
            const bool refineHole = params.getBool(QStringLiteral("RefineHole"));
            const float refineLen = float(params.getDouble(QStringLiteral("RefineHoleEdgeLen")));

            int holeCnt = 0;
            if (selfInter)
                holeCnt = vcg::tri::Hole<VCGMesh>::EarCuttingIntersectionFill<vcg::tri::SelfIntersectionEar<VCGMesh>>(mesh, maxHoleSize, selectedFlag, doc.progressCallback());
            else
                holeCnt = vcg::tri::Hole<VCGMesh>::EarCuttingFill<vcg::tri::MinimumWeightEar<VCGMesh>>(mesh, maxHoleSize, selectedFlag, doc.progressCallback());

            if (newFaceSel) {
                vcg::tri::UpdateSelection<VCGMesh>::FaceClear(mesh);
                for (size_t i = originalSize; i < mesh.face.size(); ++i) {
                    if (!mesh.face[i].IsD())
                        mesh.face[i].SetS();
                }
            }

            if (refineHole) {
                VCGMeshFFAdjScope _refFFAdj(mesh);
                VCGMeshVFAdjScope _refVFAdj(mesh);
                VCGMeshMarkScope _refMark(mesh);
                VCGMeshVertexMarkScope _refVertMark(mesh);
                vcg::tri::IsotropicRemeshing<VCGMesh>::Params refParams;
                refParams.SetFeatureAngleDeg(181.0f);
                refParams.adapt = false;
                refParams.selectedOnly = true;
                refParams.splitFlag = true;
                refParams.collapseFlag = true;
                refParams.swapFlag = true;
                refParams.smoothFlag = true;
                refParams.projectFlag = false;
                refParams.surfDistCheck = false;
                for (int k = 0; k < 3; ++k) {
                    refParams.SetTargetLen(refineLen * 3.0f);
                    refParams.iter = 5;
                    vcg::tri::IsotropicRemeshing<VCGMesh>::Do(mesh, refParams);

                    refParams.SetTargetLen(refineLen / 3.0f);
                    refParams.iter = 3;
                    vcg::tri::IsotropicRemeshing<VCGMesh>::Do(mesh, refParams);

                    refParams.SetTargetLen(refineLen);
                    refParams.iter = 2;
                    vcg::tri::IsotropicRemeshing<VCGMesh>::Do(mesh, refParams);
                }
            }

            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            markGeometry(ci, QObject::tr("Closed holes on '%1'").arg(entry.name));
            return success(
                true,
                {
                    QObject::tr("Closed %1 holes and added %2 new faces.").arg(holeCnt).arg(mesh.FN() - int(originalSize))
                });
        }

        if (filterId == QString::fromLatin1(kIdCylinderUnwrap)) {
            const float startAngle = float(params.getDouble(QStringLiteral("startAngle")));
            const float endAngle = float(params.getDouble(QStringLiteral("endAngle")));
            const float radiusUser = float(params.getDouble(QStringLiteral("radius")));

            if (endAngle <= startAngle)
                return fail(QObject::tr("End angle must be greater than start angle."));

            const int numLoop = int(1 + (endAngle - startAngle) / 360.0f);
            if (numLoop <= 0)
                return fail(QObject::tr("Invalid unwrapping angular interval."));

            std::vector<std::vector<int>> vertRefLoop(static_cast<size_t>(numLoop));
            for (int i = 0; i < numLoop; ++i)
                vertRefLoop[static_cast<size_t>(i)].assign(mesh.vert.size(), -1);

            VCGMesh unrolled;
            unrolled.textures = mesh.textures;
            float avgR = 0.0f;
            int avgCount = 0;

            for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
                vcg::Point3f p = vi->P();
                p.Y() = 0;
                VCGMesh::ScalarType ro, theta, phi;
                p.ToPolarRad(ro, theta, phi);
                float thetaDeg = vcg::math::ToDeg(theta);
                int loopIndex = 0;
                while (thetaDeg < endAngle && loopIndex < numLoop) {
                    if (thetaDeg >= startAngle) {
                        auto nvi = vcg::tri::Allocator<VCGMesh>::AddVertices(unrolled, 1);
                        vertRefLoop[static_cast<size_t>(loopIndex)][static_cast<size_t>(vi - mesh.vert.begin())] = int(nvi - unrolled.vert.begin());
                        nvi->ImportData(*vi);
                        nvi->P().X() = -vcg::math::ToRad(thetaDeg);
                        nvi->P().Y() = vi->P().Y();
                        nvi->P().Z() = ro;
                        avgR += ro;
                        ++avgCount;
                    }
                    thetaDeg += 360.0f;
                    ++loopIndex;
                }
            }

            if (avgCount == 0)
                return fail(QObject::tr("Cylindrical unwrapping produced no vertices."));
            avgR = avgR / float(avgCount);
            if (radiusUser > 0.0f)
                avgR = radiusUser;
            for (VCGVertex &v : unrolled.vert)
                v.P().X() *= avgR;

            for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
                int loopIndex = 0;
                while (loopIndex < numLoop) {
                    const int endIt = std::min(2, numLoop - loopIndex);
                    for (int ii0 = 0; ii0 < endIt; ++ii0) {
                        for (int ii1 = 0; ii1 < endIt; ++ii1) {
                            for (int ii2 = 0; ii2 < endIt; ++ii2) {
                                const int i0 = vertRefLoop[static_cast<size_t>(loopIndex + ii0)][static_cast<size_t>(fi->V(0) - &mesh.vert[0])];
                                const int i1 = vertRefLoop[static_cast<size_t>(loopIndex + ii1)][static_cast<size_t>(fi->V(1) - &mesh.vert[0])];
                                const int i2 = vertRefLoop[static_cast<size_t>(loopIndex + ii2)][static_cast<size_t>(fi->V(2) - &mesh.vert[0])];
                                if (i0 < 0 || i1 < 0 || i2 < 0)
                                    continue;
                                if (vcg::Distance(unrolled.vert[static_cast<size_t>(i0)].P(), unrolled.vert[static_cast<size_t>(i1)].P()) >= avgR / 10.0f)
                                    continue;
                                if (vcg::Distance(unrolled.vert[static_cast<size_t>(i0)].P(), unrolled.vert[static_cast<size_t>(i2)].P()) >= avgR / 10.0f)
                                    continue;
                                auto nfi = vcg::tri::Allocator<VCGMesh>::AddFaces(unrolled, 1);
                                nfi->ImportData(*fi);
                                nfi->V(0) = &unrolled.vert[static_cast<size_t>(i0)];
                                nfi->V(1) = &unrolled.vert[static_cast<size_t>(i1)];
                                nfi->V(2) = &unrolled.vert[static_cast<size_t>(i2)];
                            }
                        }
                    }
                    ++loopIndex;
                }
            }

            vcg::tri::UpdateBounding<VCGMesh>::Box(unrolled);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(unrolled);
            const int ioMask = entry.ioMask;
            const int newIndex = doc.addMesh(unrolled, {}, ioMask);
            if (newIndex < 0)
                return fail(QObject::tr("Failed to add unrolled mesh layer."));
            return success(true, { QObject::tr("Created unrolled mesh layer.") }, { newIndex });
        }

        if (filterId == QString::fromLatin1(kIdHalfCatmull)) {
            if (!vcg::tri::BitQuadCreation<VCGMesh>::IsTriQuadOnly(mesh)) {
                return fail(QObject::tr("Filter requires triangular and/or quad faces only."));
            }
            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0)
                return fail(QObject::tr("4-8 subdivision requires a two-manifold mesh."));
            if (!vcg::tri::Clean<VCGMesh>::IsFaceFauxConsistent(mesh))
                return fail(QObject::tr("Mesh has inconsistent faux-edge tagging."));
            vcg::tri::BitQuadCreation<VCGMesh>::MakePureByRefine(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerBitQuadFaceNormalized(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            markGeometry(ci, QObject::tr("Applied 4-8 subdivision on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdCatmull)) {
            PMesh baseIn, refinedOut;
            const int it = std::max(1, params.getInt(QStringLiteral("Iterations")));
            vcg::tri::PolygonSupport<VCGMesh, PMesh>::ImportFromTriMesh(baseIn, mesh);
            vcg::tri::Clean<PMesh>::RemoveUnreferencedVertex(baseIn);
            vcg::tri::Allocator<PMesh>::CompactEveryVector(baseIn);
            vcg::tri::CatmullClark<PMesh>::Refine(baseIn, refinedOut, it);
            if (!vcg::tri::PolygonSupport<VCGMesh, PMesh>::ImportFromPolyMesh(mesh, refinedOut))
                return fail(QObject::tr("Catmull-Clark produced a polygon that cannot be triangulated."));
            vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerBitPolygonFaceNormalized(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            markGeometry(ci, QObject::tr("Applied Catmull-Clark subdivision on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdDooSabin)) {
            PMesh baseIn, refinedOut;
            if (!vcg::tri::Clean<VCGMesh>::IsFaceFauxConsistent(mesh))
                return fail(QObject::tr("Mesh has inconsistent faux-edge tagging."));
            vcg::tri::PolygonSupport<VCGMesh, PMesh>::ImportFromTriMesh(baseIn, mesh);
            vcg::tri::Clean<PMesh>::RemoveUnreferencedVertex(baseIn);
            vcg::tri::Allocator<PMesh>::CompactEveryVector(baseIn);
            vcg::tri::DooSabin<PMesh>::Refine(baseIn, refinedOut);
            if (!vcg::tri::PolygonSupport<VCGMesh, PMesh>::ImportFromPolyMesh(mesh, refinedOut))
                return fail(QObject::tr("Doo-Sabin produced a polygon that cannot be triangulated."));
            vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerBitPolygonFaceNormalized(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            markGeometry(ci, QObject::tr("Applied Doo-Sabin subdivision on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdQuadPairing)) {
            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0)
                return fail(QObject::tr("Filter requires manifoldness."));
            // Choose the pairings by quad quality first. Without this the mesh reaches
            // MakePureByFlip with no pairing at all, and that routine is purely
            // topological: it takes the first unpaired triangle in array order, finds a
            // partner by breadth-first edge distance and flips its way across. On a grid
            // of squares split by random diagonals -- an input whose original quads are
            // exactly recoverable -- that scores a mean quad quality of 0.54, where the
            // quality pass recovers every square at 1.00.
            vcg::tri::BitQuadCreation<VCGMesh>::MakeDominant(mesh, 2);
            vcg::tri::BitQuadCreation<VCGMesh>::MakeTriEvenBySplit(mesh);
            // Whatever the pairing pass could not match is resolved by flipping; it is a
            // no-op when the pairing already covered every triangle.
            const bool pure = vcg::tri::BitQuadCreation<VCGMesh>::MakePureByFlip(mesh, 100);
            vcg::tri::UpdateNormal<VCGMesh>::PerBitQuadFaceNormalized(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            markGeometry(ci, QObject::tr("Applied tri-to-quad pairing on '%1'").arg(entry.name));
            if (!pure) {
                return success(true, { QObject::tr(
                    "Some triangles could not be paired into quads; the result is quad "
                    "dominant rather than pure quad.") });
            }
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdQuadDominant)) {
            const QString lvl = params.getEnum(QStringLiteral("level"));
            int level = 0;
            if (lvl == QStringLiteral("mid"))
                level = 1;
            else if (lvl == QStringLiteral("shape"))
                level = 2;
            vcg::tri::BitQuadCreation<VCGMesh>::MakeDominant(mesh, level);
            vcg::tri::UpdateNormal<VCGMesh>::PerBitQuadFaceNormalized(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            markGeometry(ci, QObject::tr("Converted '%1' to quad-dominant").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdMakePureTri)) {
            vcg::tri::BitQuadCreation<VCGMesh>::MakeBitTriOnly(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            markGeometry(ci, QObject::tr("Converted '%1' to pure triangular mesh").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdFauxCrease)) {
            const float neg = float(params.getDouble(QStringLiteral("AngleDegNeg")));
            const float pos = float(params.getDouble(QStringLiteral("AngleDegPos")));
            vcg::tri::UpdateFlags<VCGMesh>::FaceEdgeSelSignedCrease(mesh, vcg::math::ToRad(neg), vcg::math::ToRad(pos));
            entry.ioMask |= Mask::IOM_FACEFLAGS;
            doc.markMeshSelectionChanged(ci, QObject::tr("Selected crease edges on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdFauxExtract)) {
            VCGMesh edgeMesh;
            vcg::tri::BuildFromFaceEdgeSel(mesh, edgeMesh);
            // With no edge selection this yields an empty mesh. Say so rather than
            // adding an empty layer and reporting success, which is what the sibling
            // perimeter filter already does for an empty face selection.
            if (edgeMesh.EN() == 0)
                return fail(QObject::tr("No selected edges to build a polyline from."));
            vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(edgeMesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(edgeMesh);
            const int idx = doc.addMesh(edgeMesh, {}, Mask::IOM_EDGEINDEX);
            if (idx < 0)
                return fail(QObject::tr("Failed to create edge extraction layer."));
            doc.mesh(idx).transform = doc.mesh(ci).transform;  // in the frame of the layer it came from
            return success(true, { QObject::tr("Created edge mesh from selected edges.") }, { idx });
        }

        if (filterId == QString::fromLatin1(kIdCutGraph)) {
            // CutTree finds mesh vertices again by position: two at the same place would be confused.
            std::vector<vcg::Point3f> pos;
            pos.reserve(size_t(mesh.VN()));
            for (const VCGVertex &v : mesh.vert) if (!v.IsD()) pos.push_back(v.cP());
            std::sort(pos.begin(), pos.end());
            if (std::adjacent_find(pos.begin(), pos.end()) != pos.end())
                return fail(QObject::tr("Mesh has duplicate vertices; remove them first (Remove Duplicate Vertices)."));
            const RandomSeed seed = params.getRandomSeed();
            vcg::math::MarsenneTwisterRNG rng(seed.value);
            VCGMesh tree;
            tree.vert.EnableVEAdjacency();
            vcg::tri::CutTree<VCGMesh> ct(mesh, seed.value);
            ct.Build(tree, int(rng.generate(unsigned(mesh.FN()))));
            if (tree.EN() == 0)
                return fail(QObject::tr("The mesh is already a topological disk: it needs no cut."));
            tree.vert.DisableVEAdjacency();
            const int idx = doc.addMesh(tree, {}, Mask::IOM_EDGEINDEX);
            if (idx < 0)
                return fail(QObject::tr("Failed to create the cut graph layer."));
            doc.mesh(idx).transform = doc.mesh(ci).transform;  // in the frame of the layer it came from
            MeshFilterRunResult r = success(true, {
                QObject::tr("Cut graph: %1 edges.").arg(tree.EN()), seed.message() }, { idx });
            r.outputValues["edges"] = tree.EN();
            return r;
        }

        if (filterId == QString::fromLatin1(kIdSmoothPolyline)) {
            const int si = params.getMesh(QStringLiteral("surface"));
            if (si < 0 || si >= doc.meshCount() || si == ci)
                return fail(QObject::tr("Choose a surface layer other than the polyline."));
            auto &surfEntry = doc.mesh(si);
            // CoM works in the surface's frame: bring the polyline there and back.
            bool invertible = true, invertible2 = true;
            const QMatrix4x4 toSurface = surfEntry.transform.inverted(&invertible) * entry.transform;
            const QMatrix4x4 back = toSurface.inverted(&invertible2);
            if (!invertible || !invertible2)
                return fail(QObject::tr("The layer matrices cannot be inverted."));
            auto mapAll = [&](const QMatrix4x4 &mx) {
                for (VCGVertex &v : mesh.vert) {
                    const QVector3D q = mx.map(QVector3D(v.P()[0], v.P()[1], v.P()[2]));
                    v.P() = vcg::Point3f(q.x(), q.y(), q.z());
                }
            };
            using CoM = vcg::tri::CoM<VCGMesh>;
            CoM com(surfEntry.mesh);
            com.par.cb = doc.progressCallback();
            com.Init();
            const int before = mesh.VN();
            mapAll(toSurface);
            const QString controlPoints = params.getEnum(QStringLiteral("controlPoints"));
            if (controlPoints == QStringLiteral("none"))
                vcg::tri::UpdateSelection<VCGMesh>::VertexClear(mesh);  // then "selected" fixes nothing
            com.SetControlPoints(mesh, controlPoints == QStringLiteral("ends_and_nodes") ? CoM::EndsAndNodes : CoM::Selected);
            com.SmoothProject(mesh, params.getInt(QStringLiteral("iterations")),
                              float(params.getDouble(QStringLiteral("smoothWeight"))),
                              float(params.getDouble(QStringLiteral("projectWeight"))));
            com.RefineCurveByBaseMesh(mesh);  // straight in every face: control points and edge crossings only
            // Without control points an open polyline shortens from its ends, and a loop that
            // can contract does, until nothing is left of it.
            float length = 0;
            for (const VCGEdge &e : mesh.edge) if (!e.IsD()) length += vcg::edge::Length(e);
            if (!(length > surfEntry.mesh.bbox.Diag() * 1e-6f))
                return fail(QObject::tr("The polyline contracted to a point: fix some of its vertices, or use fewer iterations."));
            mapAll(back);  // on failure the rollback restores the layer, frame included
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            markGeometry(ci, QObject::tr("Smoothed '%1' on '%2'").arg(entry.name, surfEntry.name));
            MeshFilterRunResult r = success(true, {
                QObject::tr("Smoothed the polyline on '%1': %2 vertices, now %3.").arg(surfEntry.name).arg(before).arg(mesh.VN()) });
            r.outputValues["vertices"] = mesh.VN();
            return r;
        }

        if (filterId == QString::fromLatin1(kIdEmbedPolyline)) {
            const int pi = params.getMesh(QStringLiteral("polyline"));
            if (pi < 0 || pi >= doc.meshCount() || pi == ci)
                return fail(QObject::tr("Choose a polyline layer other than the surface."));
            const auto &polyEntry = doc.mesh(pi);
            if (polyEntry.mesh.EN() == 0)
                return fail(QObject::tr("Layer '%1' has no edges to embed.").arg(polyEntry.name));

            // Work on a copy, in the surface's own frame: the polyline layer stays as it is.
            VCGMesh poly;
            vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(poly, polyEntry.mesh);
            bool invertible = true;
            const QMatrix4x4 toSurface = entry.transform.inverted(&invertible) * polyEntry.transform;
            if (!invertible)
                return fail(QObject::tr("The surface's matrix cannot be inverted."));
            for (VCGVertex &v : poly.vert) {
                const QVector3D q = toSurface.map(QVector3D(v.P()[0], v.P()[1], v.P()[2]));
                v.P() = vcg::Point3f(q.x(), q.y(), q.z());
            }
            poly.vert.EnableVEAdjacency();

            using CoM = vcg::tri::CoM<VCGMesh>;
            CoM com(mesh);
            com.par.cb = doc.progressCallback();
            com.Init();
            const QString mode = params.getEnum(QStringLiteral("controlPoints"));
            com.SetControlPoints(poly, mode == QStringLiteral("all_vertices") ? CoM::AllVertices
                                      : mode == QStringLiteral("selected") ? CoM::Selected : CoM::EndsAndNodes);
            com.SmoothProject(poly, 1, 0, 1);  // project only: no smoothing
            com.RefineCurveByBaseMesh(poly);
            const int vertsBefore = mesh.VN(), facesBefore = mesh.FN();
            vcg::tri::CoMEmbed<VCGMesh>::SplitMeshWithPolyline(com, poly);
            entry.ioMask |= Mask::IOM_FACEFLAGS;
            markGeometry(ci, QObject::tr("Embedded '%1' in '%2'").arg(polyEntry.name, entry.name));
            MeshFilterRunResult r = success(true, {
                QObject::tr("Embedded %1 polyline edges: %2 vertices and %3 faces added.")
                    .arg(poly.EN()).arg(mesh.VN() - vertsBefore).arg(mesh.FN() - facesBefore) });
            r.outputValues["curve_edges"] = poly.EN();
            return r;
        }

        if (filterId == QString::fromLatin1(kIdCutSelectedEdges)) {
            const size_t selected = vcg::tri::UpdateSelection<VCGMesh>::FaceEdgeCount(mesh);
            if (selected == 0)
                return fail(QObject::tr("No selected edges to cut along."));
            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh, false) > 0 ||
                vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(mesh, false) > 0)
                return fail(QObject::tr("Mesh has non-manifold edges or vertices; cutting requires a manifold mesh."));
            const int before = mesh.VN();
            vcg::tri::CutMeshAlongSelectedFaceEdges(mesh);
            const int added = mesh.VN() - before;
            markGeometry(ci, QObject::tr("Cut '%1' along %2 selected edges").arg(entry.name).arg(selected));
            MeshFilterRunResult r = success(true, {
                QObject::tr("Cut along %1 selected edges: %2 vertices duplicated.").arg(selected).arg(added) });
            r.outputValues["vertices_added"] = added;
            return r;
        }

        if (filterId == QString::fromLatin1(kIdReebGraph)) {
            VCGMesh graph;
            vcg::tri::ReebGraph<VCGMesh> reeb;
            reeb.Compute(mesh, graph);
            // Each node sits on a mesh vertex: give it the field value there, so the graph
            // can be colored by the function it was built from.
            for (size_t n = 0; n < graph.vert.size(); ++n)
                graph.vert[n].Q() = mesh.vert[size_t(reeb.nodeVert[n])].cQ();
            vcg::tri::UpdateBounding<VCGMesh>::Box(graph);

            // Independent cycles of a graph: arcs - nodes + connected components.
            vcg::DisjointSet<VCGVertex> components;
            for (VCGVertex &v : graph.vert)
                components.MakeSet(&v);
            for (VCGEdge &e : graph.edge)
                if (components.FindSet(e.V(0)) != components.FindSet(e.V(1)))
                    components.Union(e.V(0), e.V(1));
            int componentCount = 0;
            for (VCGVertex &v : graph.vert)
                componentCount += components.FindSet(&v) == &v ? 1 : 0;
            const int cycles = graph.EN() - graph.VN() + componentCount;

            const int idx = doc.addMesh(graph, {}, Mask::IOM_EDGEINDEX | Mask::IOM_VERTQUALITY);
            if (idx < 0)
                return fail(QObject::tr("Failed to create the Reeb graph layer."));
            doc.mesh(idx).transform = doc.mesh(ci).transform;  // in the frame of the layer it came from
            MeshFilterRunResult r = success(true, {
                QObject::tr("Reeb graph: %1 nodes, %2 arcs, %3 independent cycles (the genus, on a closed surface).")
                    .arg(graph.VN()).arg(graph.EN()).arg(cycles) }, { idx });
            r.outputValues["cycles"] = cycles;
            return r;
        }

        if (filterId == QString::fromLatin1(kIdLoopSurface)) {
            const int si = params.getMesh(QStringLiteral("surface"));
            if (si < 0 || si >= doc.meshCount() || si == ci)
                return fail(QObject::tr("Choose a surface layer other than the polyline."));
            auto &surfEntry = doc.mesh(si);
            bool invertible = true;
            const QMatrix4x4 toSurface = surfEntry.transform.inverted(&invertible) * entry.transform;
            if (!invertible)
                return fail(QObject::tr("The surface's matrix cannot be inverted."));
            VCGMesh patch;
            QStringList info;
            const QString error = buildLoopSurface(mesh, toSurface, surfEntry.mesh,
                params.getEnum(QStringLiteral("gradient")) == QStringLiteral("across"),
                params.getInt(QStringLiteral("samples")), params.getInt(QStringLiteral("resolution")),
                doc.progressCallback(), patch, info);
            if (!error.isEmpty())
                return fail(error);
            const int idx = doc.addMesh(patch, {}, Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL);
            if (idx < 0)
                return fail(QObject::tr("Failed to create the surface layer."));
            doc.mesh(idx).transform = doc.mesh(si).transform;   // built in the surface's frame
            MeshFilterRunResult r = success(true, info, { idx });
            r.outputValues["faces"] = doc.mesh(idx).mesh.FN();
            return r;
        }

        if (filterId == QString::fromLatin1(kIdHandleTunnel)) {
            using Loops = vcg::tri::HandleTunnelLoops<VCGMesh>;
            Loops::Param par;
            par.maxIter = params.getInt(QStringLiteral("tighteningRounds"));
            par.patience = params.getInt(QStringLiteral("patience"));
            par.localMinima = params.getBool(QStringLiteral("localMinima"));
            par.samples = params.getInt(QStringLiteral("samples"));
            par.persistence = params.getDouble(QStringLiteral("persistence"));
            par.maxLoops = params.getInt(QStringLiteral("maxLoops"));
            const RandomSeed seed = params.getRandomSeed();
            par.seed = seed.value;
            Loops ht;
            ht.Compute(mesh, par);
            // Say so rather than add two empty layers: a sphere has nothing to report.
            if (ht.genus == 0)
                return fail(QObject::tr("The surface has genus 0, so it has no handle or tunnel loops."));

            QStringList info{ par.localMinima
                ? QObject::tr("Genus %1; local minima from %2 starting points.").arg(ht.genus).arg(par.samples)
                : QObject::tr("Genus %1.").arg(ht.genus) };
            QVector<int> created;
            // Handles first, then tunnels: the order of the descriptor's outputTag.
            for (const auto &[family, name] : { std::pair{ &ht.handles, QObject::tr("handle") },
                                                std::pair{ &ht.tunnels, QObject::tr("tunnel") } }) {
                VCGMesh loops;
                Loops::LoopsToEdgeMesh(mesh, *family, loops);
                // A loop of k vertices is k consecutive edges: number them by loop, so the
                // loops can be told apart by coloring the edges by scalar.
                float shortest = std::numeric_limits<float>::max(), longest = 0.0f;
                size_t e = 0;
                for (size_t li = 0; li < family->size(); ++li) {
                    float length = 0.0f;
                    for (size_t k = 0; k < (*family)[li].size(); ++k, ++e) {
                        loops.edge[e].Q() = float(li + 1);
                        length += vcg::edge::Length(loops.edge[e]);
                    }
                    shortest = std::min(shortest, length);
                    longest = std::max(longest, length);
                }
                vcg::tri::UpdateBounding<VCGMesh>::Box(loops);
                const int idx = doc.addMesh(loops, {}, Mask::IOM_EDGEINDEX | Mask::IOM_EDGEQUALITY);
                if (idx < 0)
                    return fail(QObject::tr("Failed to create the %1 loop layer.").arg(name));
                doc.mesh(idx).transform = doc.mesh(ci).transform;  // in the frame of the layer it came from
                created << idx;
                info << QObject::tr("%1 %2 loop(s), length %3 to %4.")
                            .arg(family->size()).arg(name)
                            .arg(double(shortest), 0, 'g', 4).arg(double(longest), 0, 'g', 4);
            }
            info << seed.message();
            MeshFilterRunResult r = success(true, info, created);
            r.outputValues["genus"] = ht.genus;
            return r;
        }

        if (filterId == QString::fromLatin1(kIdVAttrSeam)) {
            unsigned int vmask = vcg::tri::AttributeSeam::POSITION_PER_VERTEX;
            unsigned int nmask = 0;
            const QString nmode = params.getEnum(QStringLiteral("NormalMode"));
            if (nmode == QStringLiteral("vertex"))
                nmask |= vcg::tri::AttributeSeam::NORMAL_PER_VERTEX;
            else if (nmode == QStringLiteral("face"))
                nmask |= vcg::tri::AttributeSeam::NORMAL_PER_FACE;

            unsigned int cmask = 0;
            const QString cmode = params.getEnum(QStringLiteral("ColorMode"));
            if (cmode == QStringLiteral("vertex"))
                cmask |= vcg::tri::AttributeSeam::COLOR_PER_VERTEX;
            else if (cmode == QStringLiteral("face"))
                cmask |= vcg::tri::AttributeSeam::COLOR_PER_FACE;

            unsigned int tmask = 0;
            const QString tmode = params.getEnum(QStringLiteral("TexcoordMode"));
            if (tmode == QStringLiteral("vertex")) {
                if (!vcg::tri::HasPerVertexTexCoord(mesh))
                    return fail(QObject::tr("Vertex texcoord source requires per-vertex texture coordinates."));
                tmask |= vcg::tri::AttributeSeam::TEXCOORD_PER_VERTEX;
                mesh.vert.EnableTexCoord();
            } else if (tmode == QStringLiteral("wedge")) {
                if (!vcg::tri::HasPerWedgeTexCoord(mesh))
                    return fail(QObject::tr("Wedge texcoord source requires per-wedge texture coordinates."));
                tmask |= vcg::tri::AttributeSeam::TEXCOORD_PER_WEDGE;
                mesh.vert.EnableTexCoord();
            }

            const unsigned int mask = vmask | nmask | cmask | tmask;
            if (mask == 0)
                return success(false, { QObject::tr("No attribute source selected; no changes applied.") });

            vcg::tri::AttributeSeam::ASExtract<VCGMesh, VCGMesh> vExtract(mask);
            vcg::tri::AttributeSeam::ASCompare<VCGMesh> vCompare(mask);
            const bool ok = vcg::tri::AttributeSeam::SplitVertex(mesh, vExtract, vCompare);
            if (!ok)
                return fail(QObject::tr("Failed to split vertices by attribute seam."));

            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_VERTCOLOR | Mask::IOM_VERTTEXCOORD;
            markGeometry(ci, QObject::tr("Split vertices by attribute seam on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdPerimeterPolyline)) {
            if (selectedFaceCount(mesh) == 0)
                return fail(QObject::tr("No selected faces to build perimeter polyline."));

            VCGMesh perimeter;
            perimeter.textures = mesh.textures;

            for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
                if (!fi->IsS())
                    continue;
                for (int ei = 0; ei < 3; ++ei) {
                    VCGFace *adjf = fi->FFp(ei);
                    if (adjf != &(*fi) && adjf && adjf->IsS())
                        continue;
                    auto eIt = vcg::tri::Allocator<VCGMesh>::AddEdges(perimeter, 1);
                    auto vIt = vcg::tri::Allocator<VCGMesh>::AddVertices(perimeter, 2);
                    vIt->P() = fi->V(ei)->P();
                    vIt->N() = fi->V(ei)->N();
                    eIt->V(0) = &(*vIt);
                    ++vIt;
                    vIt->P() = fi->V((ei + 1) % 3)->P();
                    vIt->N() = fi->V((ei + 1) % 3)->N();
                    eIt->V(1) = &(*vIt);
                }
            }

            vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(perimeter);
            vcg::tri::UpdateBounding<VCGMesh>::Box(perimeter);
            const int idx = doc.addMesh(perimeter, {}, Mask::IOM_EDGEINDEX);
            if (idx < 0)
                return fail(QObject::tr("Failed to create perimeter polyline layer."));
            doc.mesh(idx).transform = doc.mesh(ci).transform;  // in the frame of the layer it came from
            return success(true, { QObject::tr("Created perimeter polyline layer.") }, { idx });
        }

        if (filterId == QString::fromLatin1(kIdTrimByPlane)) {
            const QString filterName = QObject::tr("Trim Surface by Plane");
            if (mesh.VN() <= 0 || mesh.FN() <= 0)
                return fail(QObject::tr("%1 requires a mesh with faces.").arg(filterName));

            // One control, not two: the point3f direction editor already offers the six
            // axis presets and the view direction, so an enum beside it would be the same
            // choice asked twice. Create Polyline from Planar Section still has that older
            // pair; this filter does not copy it.
            const QVector3D nv = params.getPoint3f(QStringLiteral("planeNormal"));
            vcg::Point3f normal(float(nv.x()), float(nv.y()), float(nv.z()));
            const float length = normal.Norm();
            if (!std::isfinite(length) || length <= 1e-9f)
                return fail(QObject::tr("%1 needs a plane normal that is not zero.").arg(filterName));
            normal /= length;
            // Which side survives: the clip keeps what the normal points at.
            if (params.getBool(QStringLiteral("flip")))
                normal = -normal;

            if (mesh.bbox.IsNull())
                vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            const QString rel = params.getEnum(QStringLiteral("relativeTo"));
            vcg::Point3f reference(0, 0, 0);
            if (rel == QStringLiteral("center"))
                reference = mesh.bbox.Center();
            else if (rel == QStringLiteral("min"))
                reference = mesh.bbox.min;
            else if (rel == QStringLiteral("max"))
                reference = mesh.bbox.max;
            const float offset = float(params.getDouble(QStringLiteral("planeOffset")));

            vcg::Plane3f plane;
            plane.Init(reference + normal * offset, normal);

            const bool closeCut = params.getBool(QStringLiteral("closeCut"));
            // All of the geometry is vcglib's: the refine framework splits the faces at the
            // plane without duplicating vertices it meets head-on, and the cap tessellates
            // every loop of the outline together so concentric ones come out as a ring.
            // Face-face adjacency is optional storage on VCGMesh and both of those need it.
            const float snap = float(std::clamp(
                params.getDouble(QStringLiteral("snapTolerance")), 0.0, 0.45));
            bool clipped = false;
            {
                VCGMeshFFAdjScope _clipFFAdj(mesh);
                vcg::PlanarRefinement refinement;   // opt-in here: a minimum angle of 0 adds no points
                refinement.minAngle = params.getBool(QStringLiteral("refineCap"))
                    ? params.getDouble(QStringLiteral("capMinAngle")) : 0.0;
                refinement.splitBoundary = params.getBool(QStringLiteral("capRefineBoundary"));
                clipped = vcg::tri::ClipMeshWithPlane(mesh, plane, closeCut, snap, refinement);
            }
            if (!clipped)
                return fail(QObject::tr("%1 left the mesh unchanged: the plane does not cut it.").arg(filterName));
            if (mesh.FN() <= 0)
                return fail(QObject::tr("%1 would remove the whole mesh. It was left unchanged.").arg(filterName));

            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            const QString contextMessage =
                QObject::tr("Trimmed mesh '%1' (%2 vertices, %3 faces).")
                    .arg(entry.name).arg(mesh.VN()).arg(mesh.FN());
            markGeometry(ci, contextMessage);

            QStringList info{ contextMessage };
            if (closeCut) {
                // ClipMeshWithPlane leaves whatever part of the cut it cannot close open
                // rather than failing, and holes the mesh already had are not its business,
                // so what is counted is the boundary still lying on the plane -- by the test
                // CapPlanarBoundary itself uses.
                VCGMeshFFAdjScope _borderFFAdj(mesh);
                vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
                const float tolerance = vcg::tri::PlaneTolerance(mesh);
                const auto onPlane = [&](const vcg::Point3f &p) {
                    return std::abs(vcg::tri::PlaneDistance(plane, p)) <= tolerance;
                };
                int open = 0;
                for (const VCGFace &f : mesh.face) {
                    if (f.IsD())
                        continue;
                    for (int e = 0; e < 3; ++e)
                        if (vcg::face::IsBorder(f, e) && onPlane(f.cP0(e)) && onPlane(f.cP1(e)))
                            ++open;
                }
                info << (open == 0
                             ? QObject::tr("Closed the cut.")
                             : QObject::tr("Could not close %1 edge(s) of the cut; they were left open.").arg(open));
            }
            return success(true, info);
        }

        if (filterId == QString::fromLatin1(kIdSlicePlane)) {
            vcg::Point3f axis(1, 0, 0);
            const QString am = params.getEnum(QStringLiteral("planeAxis"));
            if (am == QStringLiteral("y"))
                axis = { 0, 1, 0 };
            else if (am == QStringLiteral("z"))
                axis = { 0, 0, 1 };
            else if (am == QStringLiteral("custom")) {
                const QVector3D av = params.getPoint3f(QStringLiteral("customAxis"));
                axis = { float(av.x()), float(av.y()), float(av.z()) };
            }
            const float axn = std::sqrt(axis.SquaredNorm());
            if (axn <= 1e-20f)
                return fail(QObject::tr("Custom slicing axis must be non-zero."));
            axis /= axn;

            const float offset = float(params.getDouble(QStringLiteral("planeOffset")));
            const QString rel = params.getEnum(QStringLiteral("relativeTo"));
            vcg::Point3f planeCenter;
            if (rel == QStringLiteral("center"))
                planeCenter = mesh.bbox.Center() + axis * offset * (mesh.bbox.Diag() / 2.0f);
            else if (rel == QStringLiteral("min"))
                planeCenter = mesh.bbox.min + axis * offset * (mesh.bbox.Diag() / 2.0f);
            else
                planeCenter = axis * offset;

            vcg::Plane3f slicingPlane;
            slicingPlane.Init(planeCenter, axis);

            VCGMesh section;
            vcg::IntersectionPlaneMesh<VCGMesh, VCGMesh, VCGMesh::ScalarType>(mesh, slicingPlane, section);
            vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(section);
            vcg::tri::UpdateBounding<VCGMesh>::Box(section);

            VCGMesh cap;
            const bool createSectionSurface = params.getBool(QStringLiteral("createSectionSurface"));
            if (createSectionSurface) {
                vcg::PlanarRefinement refinement;   // opt-in: a minimum angle of 0 adds no points
                refinement.minAngle = params.getBool(QStringLiteral("refineCap"))
                    ? params.getDouble(QStringLiteral("capMinAngle")) : 0.0;
                refinement.splitBoundary = params.getBool(QStringLiteral("capRefineBoundary"));
                const QString capError = buildSectionCap(section, axis, refinement, cap);
                if (!capError.isEmpty())
                    return fail(capError);
                vcg::tri::UpdateBounding<VCGMesh>::Box(cap);
                vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(cap);
            }

            QVector<int> created;
            const int secIdx = doc.addMesh(section, {}, Mask::IOM_EDGEINDEX);
            if (secIdx >= 0) {
                doc.mesh(secIdx).transform = doc.mesh(ci).transform;  // in the frame of the layer it came from
                created.push_back(secIdx);
            }

            if (createSectionSurface) {
                const int capIdx = doc.addMesh(cap, {}, Mask::IOM_FACENORMAL | Mask::IOM_VERTNORMAL);
                if (capIdx >= 0) {
                    doc.mesh(capIdx).transform = doc.mesh(ci).transform;
                    created.push_back(capIdx);
                }
            }

            if (params.getBool(QStringLiteral("splitSurfaceWithSection"))) {
                return fail(QObject::tr("splitSurfaceWithSection is not yet supported in MeshLab port."));
            }

            if (created.isEmpty())
                return fail(QObject::tr("Section computation produced no output."));
            return success(true, { QObject::tr("Created %1 section layer(s).").arg(created.size()) }, created);
        }

        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
    } catch (const vcg::MissingPreconditionException &e) {
        return fail(QString::fromLocal8Bit(e.what()));
    } catch (const std::exception &e) {
        return fail(QString::fromLocal8Bit(e.what()));
    } catch (...) {
        return fail(QObject::tr("Unexpected meshing filter error."));
    }
}

void registerMeshingFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<MeshingFilterPlugin>());
}
