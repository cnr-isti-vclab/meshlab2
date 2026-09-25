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
#include <vcg/space/fitting3.h>
#include <vcg/space/planar_polygon_tessellation.h>
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace {

QString buildSectionCap(
    const VCGMesh &section,
    const vcg::Point3f &normal,
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

    std::vector<int> triangles;
    if (!vcg::TessellatePlanarContours3(contours, triangles))
        return QObject::tr("The planar section contours could not be triangulated.");

    size_t vertexCount = 0;
    for (const Contour &contour : contours)
        vertexCount += contour.size();

    cap.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(cap, vertexCount);
    size_t vertexOffset = 0;
    for (const Contour &contour : contours) {
        for (size_t i = 0; i < contour.size(); ++i)
            cap.vert[vertexOffset + i].P() = contour[i];
        vertexOffset += contour.size();
    }

    vcg::tri::Allocator<VCGMesh>::AddFaces(cap, triangles.size() / 3);
    for (size_t i = 0; i < triangles.size(); i += 3) {
        int a = triangles[i];
        int b = triangles[i + 1];
        int c = triangles[i + 2];
        if (((cap.vert[size_t(b)].cP() - cap.vert[size_t(a)].cP())
                ^ (cap.vert[size_t(c)].cP() - cap.vert[size_t(a)].cP())) * normal < 0)
            std::swap(b, c);
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
constexpr QLatin1StringView kIdVAttrSeam("split_vertices_by_attribute_seam");
constexpr QLatin1StringView kIdLS3Loop("subdivide_by_ls3_loop");

struct TransformOptions {
    bool freeze = true;
};

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

vcg::Point3f transformVectorLinear(const vcg::Matrix44f &m, const vcg::Point3f &v)
{
    return {
        m[0][0] * v.X() + m[0][1] * v.Y() + m[0][2] * v.Z(),
        m[1][0] * v.X() + m[1][1] * v.Y() + m[1][2] * v.Z(),
        m[2][0] * v.X() + m[2][1] * v.Y() + m[2][2] * v.Z()
    };
}

void applyTransformToMesh(VCGMesh &mesh, const vcg::Matrix44f &tr)
{
    for (VCGVertex &v : mesh.vert) {
        v.P() = tr * v.cP();
        vcg::Point3f nn = transformVectorLinear(tr, v.cN());
        const float n2 = nn.SquaredNorm();
        if (n2 > 1e-20f)
            v.N() = nn / std::sqrt(n2);
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (mesh.FN() > 0)
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
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
    applyTransformToMesh(referenceMesh, qtToVcg(referenceToCurrentLocal));
    return true;
}

void applyTransform(
    Document &doc,
    const vcg::Matrix44f &tr,
    const TransformOptions &opt,
    QVector<int> &touched)
{
    touched.clear();
    const int i = doc.currentMeshIndex();
    if (i < 0 || i >= doc.meshCount())
        return;

    Document::MeshEntry &entry = doc.mesh(i);
    // Compose the new filter matrix on top of the existing per-mesh transform.
    const QMatrix4x4 combined = vcgToQt(tr) * entry.transform;
    if (opt.freeze) {
        // Bake the combined transform into vertex positions, then reset the matrix.
        applyTransformToMesh(entry.mesh, qtToVcg(combined));
        QMatrix4x4 identity;
        identity.setToIdentity();
        doc.setMeshTransform(i, identity);
    } else {
        // Store the composed matrix for rendering without touching vertex data.
        doc.setMeshTransform(i, combined);
    }
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

            const bool selected = params.getBool(QStringLiteral("Selected"));
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
            return success(true,
                { QObject::tr("Clustering decimation: %1 → %2 vertices, %3 → %4 faces.")
                    .arg(srcVN).arg(output.VN()).arg(srcFN).arg(output.FN()) },
                { newIndex });
        }

        if (filterId == QString::fromLatin1(kIdInvertFaces)) {
            const bool forceFlip = params.getBool(QStringLiteral("forceFlip"));
            const bool onlySel = params.getBool(QStringLiteral("onlySelected"));
            if (forceFlip)
                vcg::tri::Clean<VCGMesh>::FlipMesh(mesh, onlySel);
            else
                vcg::tri::Clean<VCGMesh>::FlipNormalOutside(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            markGeometry(ci, QObject::tr("Inverted face orientation on '%1'").arg(entry.name));
            return success(true);
        }

        if (filterId == QString::fromLatin1(kIdQuadric)) {
            int targetFaceNum = params.getInt(QStringLiteral("TargetFaceNum"));
            const float targetPerc = float(params.getDouble(QStringLiteral("TargetPerc")));
            if (targetPerc > 0.0f)
                targetFaceNum = int(std::round(mesh.FN() * targetPerc));
            targetFaceNum = std::clamp(targetFaceNum, 1, std::max(1, mesh.FN()));

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
            const bool selected = params.getBool(QStringLiteral("Selected"));

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

            int targetFaceNum = params.getInt(QStringLiteral("TargetFaceNum"));
            const float targetPerc = float(params.getDouble(QStringLiteral("TargetPerc")));
            if (targetPerc > 0.0f)
                targetFaceNum = int(std::round(mesh.FN() * targetPerc));
            targetFaceNum = std::clamp(targetFaceNum, 1, std::max(1, mesh.FN()));

            vcg::tri::TriEdgeCollapseQuadricTexParameter pp;
            pp.QualityThr = float(params.getDouble(QStringLiteral("QualityThr")));
            pp.ExtraTCoordWeight = float(params.getDouble(QStringLiteral("Extratcoordw")));
            pp.OptimalPlacement = params.getBool(QStringLiteral("OptimalPlacement"));
            pp.PreserveBoundary = params.getBool(QStringLiteral("PreserveBoundary"));
            pp.BoundaryWeight = pp.BoundaryWeight * float(params.getDouble(QStringLiteral("BoundaryWeight")));
            pp.QualityQuadric = params.getBool(QStringLiteral("PlanarQuadric"));
            pp.NormalCheck = params.getBool(QStringLiteral("PreserveNormal"));
            const bool selected = params.getBool(QStringLiteral("Selected"));

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
            remeshParams.selectedOnly = params.getBool(QStringLiteral("SelectedOnly"));
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

        auto makeTransformOptions = [&]() {
            TransformOptions o;
            o.freeze = params.getBool(QStringLiteral("Freeze"));
            return o;
        };

        auto applyTransformAndMark = [&](const vcg::Matrix44f &tr, const TransformOptions &opt, const QString &opName) {
            QVector<int> touched;
            applyTransform(doc, tr, opt, touched);
            for (int idx : touched) {
                if (opt.freeze)
                    markGeometry(idx, QObject::tr("%1 on '%2'").arg(opName, doc.mesh(idx).name));
            }
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
            return applyTransformAndMark(tr, makeTransformOptions(), QObject::tr("Flip/Swap axes"));
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
            return applyTransformAndMark(tr, makeTransformOptions(), QObject::tr("Rotate"));
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
            return applyTransformAndMark(tr, makeTransformOptions(), QObject::tr("Rotate to fit"));
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
            applyTransform(doc, tr, makeTransformOptions(), touched);
            const TransformOptions opt = makeTransformOptions();
            for (int idx : touched) {
                if (opt.freeze)
                    markGeometry(idx, QObject::tr("Normalized the reference frame of '%1'").arg(doc.mesh(idx).name));
            }
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
            return applyTransformAndMark(tr, makeTransformOptions(), QObject::tr("Translate/Center"));
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
            return applyTransformAndMark(tr, makeTransformOptions(), QObject::tr("Scale"));
        }

        if (filterId == QString::fromLatin1(kIdReset)) {
            // Reset the per-mesh transform to the identity matrix.
            QMatrix4x4 identity;
            identity.setToIdentity();
            doc.setMeshTransform(ci, identity);
            return success(true, { QObject::tr("Transform reset on current layer.") });
        }

        if (filterId == QString::fromLatin1(kIdFreeze)) {
            // Bake the current per-mesh transform into vertex positions and reset to identity.
            QMatrix4x4 identity;
            identity.setToIdentity();
            applyTransformToMesh(mesh, qtToVcg(entry.transform));
            doc.setMeshTransform(ci, identity);
            markGeometry(ci, QObject::tr("Freeze transform on '%1'").arg(entry.name));
            return success(true, { QObject::tr("Transform frozen to vertices on current layer.") });
        }

        if (filterId == QString::fromLatin1(kIdInvertTr)) {
            // Invert the current per-mesh transform matrix.
            const bool freeze = params.getBool(QStringLiteral("Freeze"));
            QMatrix4x4 identity;
            identity.setToIdentity();
            bool invertible = false;
            const QMatrix4x4 inv = entry.transform.inverted(&invertible);
            if (!invertible)
                return fail(QObject::tr("Current transform matrix is not invertible."));
            if (freeze) {
                applyTransformToMesh(mesh, qtToVcg(inv));
                doc.setMeshTransform(ci, identity);
                markGeometry(ci, QObject::tr("Invert and freeze transform on '%1'").arg(entry.name));
            } else {
                doc.setMeshTransform(ci, inv);
            }
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
            return applyTransformAndMark(tr, makeTransformOptions(), QObject::tr("Set transform from parameters"));
        }

        if (filterId == QString::fromLatin1(kIdSetMatrix)) {
            vcg::Matrix44f tr;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    tr[r][c] = float(params.getDouble(QStringLiteral("m%1%2").arg(r).arg(c)));
            return applyTransformAndMark(tr, makeTransformOptions(), QObject::tr("Set transform matrix"));
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
            return success(true, { QObject::tr("Created edge mesh from selected edges.") }, { idx });
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
                clipped = vcg::tri::ClipMeshWithPlane(mesh, plane, closeCut, snap);
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
                const QString capError = buildSectionCap(section, axis, cap);
                if (!capError.isEmpty())
                    return fail(capError);
                vcg::tri::UpdateBounding<VCGMesh>::Box(cap);
                vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(cap);
            }

            QVector<int> created;
            const int secIdx = doc.addMesh(section, {}, Mask::IOM_EDGEINDEX);
            if (secIdx >= 0)
                created.push_back(secIdx);

            if (createSectionSurface) {
                const int capIdx = doc.addMesh(cap, {}, Mask::IOM_FACENORMAL | Mask::IOM_VERTNORMAL);
                if (capIdx >= 0)
                    created.push_back(capIdx);
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
