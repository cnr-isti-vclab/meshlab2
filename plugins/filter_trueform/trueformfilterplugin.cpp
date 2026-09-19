#include "plugins/filter_trueform/trueformfilterplugin.h"

#include "document.h"
#include "filterparam.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <QMatrix3x3>
#include <QMatrix4x4>
#include <QObject>
#include <QList>
#include <QRegularExpression>
#include <QStringList>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <exception>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/append.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

// Qt's keyword macros collide with ordinary identifiers inside TrueForm and oneTBB:
// `emit` hits tbb::profiling::event::emit(), and `slots` hits local variables named
// slots. Both expand to nothing, so the declarations become syntactically invalid far
// from the real cause. Hide all three keyword macros across the include.
#pragma push_macro("emit")
#pragma push_macro("slots")
#pragma push_macro("signals")
#undef emit
#undef slots
#undef signals
#include <trueform/trueform.hpp>
#pragma pop_macro("signals")
#pragma pop_macro("slots")
#pragma pop_macro("emit")

namespace {

constexpr QLatin1StringView kFilterAlignObb("align_by_bounding_box_trueform");
constexpr QLatin1StringView kFilterAlignIcp("align_by_icp_trueform");
constexpr QLatin1StringView kFilterAlignCorresponding("align_to_corresponding_points_trueform");
constexpr QLatin1StringView kFilterUnion("mesh_union_trueform");
constexpr QLatin1StringView kFilterIntersection("mesh_intersection_trueform");
constexpr QLatin1StringView kFilterDifference("mesh_difference_trueform");
constexpr QLatin1StringView kFilterSymmetricDifference("mesh_symmetric_difference_trueform");
constexpr QLatin1StringView kFilterCsg("mesh_csg_expression_trueform");
constexpr QLatin1StringView kFilterSolidDomains("split_into_solid_domains_trueform");
constexpr QLatin1StringView kFilterSplitComponents(
    "split_into_connected_components_trueform");
constexpr QLatin1StringView kFilterOuterShell("extract_outer_shell_trueform");
constexpr QLatin1StringView kFilterSelfIntersectionCurves("create_polyline_from_self_intersections_trueform");
constexpr QLatin1StringView kFilterIntersectionCurves("create_polyline_from_mesh_intersection_trueform");
constexpr QLatin1StringView kFilterIsocurves("create_polyline_from_scalar_isocontour_trueform");
constexpr QLatin1StringView kFilterTube("create_tube_from_polyline_trueform");
constexpr QLatin1StringView kFilterSignedDistance("compute_signed_distance_to_mesh_trueform");
constexpr QLatin1StringView kFilterSelectInside("select_vertices_inside_mesh_trueform");
constexpr QLatin1StringView kFilterChamfer("measure_chamfer_distance_trueform");
constexpr QLatin1StringView kFilterLaplacian("smooth_vertices_by_laplacian_trueform");
constexpr QLatin1StringView kFilterTaubin("smooth_vertices_by_taubin_trueform");
constexpr QLatin1StringView kFilterCurvature("compute_curvature_trueform");
constexpr QLatin1StringView kFilterNormals("compute_normals_trueform");
constexpr QLatin1StringView kFilterIsotropic("remesh_isotropically_trueform");
constexpr QLatin1StringView kFilterSimplify("simplify_by_error_bound_trueform");
constexpr QLatin1StringView kFilterDecimate("simplify_by_decimation_trueform");
constexpr QLatin1StringView kFilterOrientCoherent("orient_faces_consistently_trueform");
constexpr QLatin1StringView kFilterOrientOutward("orient_faces_outward_trueform");
constexpr QLatin1StringView kFilterSelectCrease("select_crease_edges_trueform");
constexpr QLatin1StringView kFilterSelectNonManifold("select_non_manifold_edges_trueform");
constexpr QLatin1StringView kFilterRepairSelfIntersections("repair_self_intersections_trueform");
constexpr QLatin1StringView kFilterCutIsocontour("cut_along_scalar_isocontour_trueform");
constexpr QLatin1StringView kFilterClean("remove_duplicate_vertices_trueform");
constexpr QLatin1StringView kFilterImprove("remesh_by_edge_flipping_trueform");

using Mask = vcg::tri::io::Mask;
using TfPoints = tf::points_buffer<float, 3>;
using TfTree = tf::aabb_tree<int, float, 3>;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = message;
    return result;
}

MeshFilterRunResult success(const QStringList &info)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    return result;
}

// tfMeshFromLayer() skips deleted vertices, so the nth TrueForm point is the nth *live*
// VCG vertex. Results have to be written back through that mapping, not by raw index.
std::vector<std::size_t> liveVertexIndices(const VCGMesh &mesh)
{
    std::vector<std::size_t> live;
    live.reserve(std::size_t(std::max(0, mesh.VN())));
    for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
        if (!mesh.vert[i].IsD())
            live.push_back(i);
    }
    return live;
}

// Collect a layer's live vertices in world space. Alignment is only meaningful across
// layers, so the layer matrix has to be applied before anything is compared.
TfPoints worldPoints(const Document::MeshEntry &entry)
{
    const VCGMesh &mesh = entry.mesh;
    const QMatrix4x4 &m = entry.transform;
    const std::vector<std::size_t> live = liveVertexIndices(mesh);

    TfPoints points;
    points.allocate(live.size());
    tf::parallel_for_each(tf::enumerate(tf::make_range(live)), [&](auto pair) {
        auto &&[at, vi] = pair;
        const vcg::Point3f &p = mesh.vert[vi].cP();
        const QVector3D w = m.map(QVector3D(p.X(), p.Y(), p.Z()));
        points[std::size_t(at)] = tf::make_point(w.x(), w.y(), w.z());
    }, tf::checked);
    return points;
}

// The reference layer's own vertex normals, rotated into world space. Point-to-plane
// ICP needs a normal per reference point; MeshLab already maintains these, and the user
// controls them through the Compute/Orient normal filters, so they are a better source
// than recomputing from the faces here.
tf::unit_vectors_buffer<float, 3> worldNormals(const Document::MeshEntry &entry)
{
    tf::unit_vectors_buffer<float, 3> normals;
    const QMatrix3x3 n = entry.transform.normalMatrix();
    for (const VCGVertex &v : entry.mesh.vert) {
        if (v.IsD())
            continue;
        const vcg::Point3f &s = v.cN();
        QVector3D t(
            n(0, 0) * s.X() + n(0, 1) * s.Y() + n(0, 2) * s.Z(),
            n(1, 0) * s.X() + n(1, 1) * s.Y() + n(1, 2) * s.Z(),
            n(2, 0) * s.X() + n(2, 1) * s.Y() + n(2, 2) * s.Z());
        if (t.lengthSquared() < 1e-20f)
            t = QVector3D(0.0f, 0.0f, 1.0f); // degenerate normal: pick something unit
        t.normalize();
        normals.emplace_back(t.x(), t.y(), t.z());
    }
    return normals;
}

// TrueForm stores an affine transform as Dims x (Dims+1): the last column is the
// translation. Widen it to the 4x4 MeshLab keeps on a layer.
template <typename Transformation>
QMatrix4x4 toQMatrix(const Transformation &t)
{
    QMatrix4x4 m; // identity
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c)
            m(r, c) = float(t(std::size_t(r), std::size_t(c)));
    }
    return m;
}

struct AlignmentInputs
{
    int sourceIndex = -1;
    int referenceIndex = -1;
};

// Resolve and validate the two layers every alignment filter works on.
bool resolveInputs(const FilterParams &params, Document &doc, AlignmentInputs &out, QString &error)
{
    out.sourceIndex = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    out.referenceIndex = params.getMesh(QStringLiteral("referenceMesh"), -1);

    const auto valid = [&doc](int i) { return i >= 0 && i < doc.meshCount(); };
    if (!valid(out.sourceIndex) || !valid(out.referenceIndex)) {
        error = QObject::tr("Both a source and a reference layer must be selected.");
        return false;
    }
    if (out.sourceIndex == out.referenceIndex) {
        error = QObject::tr("The source and reference layers must be different.");
        return false;
    }
    if (doc.mesh(out.sourceIndex).mesh.VN() < 3 || doc.mesh(out.referenceIndex).mesh.VN() < 3) {
        error = QObject::tr("Both layers need at least 3 vertices.");
        return false;
    }
    return true;
}

// Compose the newly found world-space alignment onto the source layer's existing matrix
// and store it. Alignment moves the layer, it never touches vertex coordinates.
MeshFilterRunResult applyAlignment(
    Document &doc, int sourceIndex, const QMatrix4x4 &worldDelta, QStringList info)
{
    const QMatrix4x4 updated = worldDelta * doc.mesh(sourceIndex).transform;
    doc.setMeshTransform(sourceIndex, updated);
    doc.finishFilterProgress(true, QObject::tr("Aligned layer."));
    return success(info);
}

// Coarse alignment from the two point sets' oriented bounding boxes. Fast and needs no
// initial guess, but an OBB is only defined up to 180-degree flips about its axes; when
// a tree is supplied TrueForm tries the candidates and keeps the best.
MeshFilterRunResult runAlignObb(const FilterParams &params, Document &doc)
{
    AlignmentInputs in;
    QString error;
    if (!resolveInputs(params, doc, in, error))
        return fail(error);

    doc.beginFilterProgress(QObject::tr("Align by Bounding Box (TrueForm)"));
    QMatrix4x4 delta;
    try {
        const TfPoints source = worldPoints(doc.mesh(in.sourceIndex));
        const TfPoints reference = worldPoints(doc.mesh(in.referenceIndex));
        TfTree tree(tf::make_points(reference), tf::config_tree(4, 4));
        const auto t = tf::fit_obb_alignment(
            tf::make_points(source), tf::make_points(reference) | tf::tag(tree));
        delta = toQMatrix(t);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm bounding-box alignment failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    return applyAlignment(
        doc, in.sourceIndex, delta,
        { QObject::tr("Aligned '%1' to '%2' by oriented bounding box.")
              .arg(doc.mesh(in.sourceIndex).name, doc.mesh(in.referenceIndex).name) });
}

// Iterative closest point. Point-to-plane uses the reference's vertex normals and
// converges in far fewer iterations on smooth surfaces; point-to-point is the safer
// choice when those normals are unreliable.
MeshFilterRunResult runAlignIcp(const FilterParams &params, Document &doc)
{
    AlignmentInputs in;
    QString error;
    if (!resolveInputs(params, doc, in, error))
        return fail(error);

    const bool pointToPlane =
        params.getEnum(QStringLiteral("metric")) == QStringLiteral("point_to_plane");
    const bool coarseFirst = params.getBool(QStringLiteral("coarseInit"), true);
    tf::icp_config cfg;
    cfg.max_iterations = std::size_t(std::max(1, params.getInt(QStringLiteral("maxIterations"), 50)));
    cfg.n_samples = std::size_t(std::max(0, params.getInt(QStringLiteral("samples"), 1000)));
    cfg.min_relative_improvement =
        float(params.getDouble(QStringLiteral("minImprovement"), 0.001));
    cfg.outlier_proportion =
        float(std::clamp(params.getDouble(QStringLiteral("outlierProportion"), 0.0), 0.0, 0.9));

    doc.beginFilterProgress(QObject::tr("Align by ICP (TrueForm)"));
    QMatrix4x4 delta;
    double residual = 0.0;
    try {
        const TfPoints source = worldPoints(doc.mesh(in.sourceIndex));
        const TfPoints reference = worldPoints(doc.mesh(in.referenceIndex));
        auto sourcePts = tf::make_points(source);
        auto referencePts = tf::make_points(reference);
        TfTree tree(referencePts, tf::config_tree(4, 4));
        auto referenceWithTree = referencePts | tf::tag(tree);

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(20, coarseFirst ? "Coarse bounding-box alignment..." : "Running ICP...");

        // Start from the OBB fit when asked: ICP only refines locally, so a bad start
        // converges to a wrong local minimum rather than failing visibly.
        auto initial = coarseFirst ? tf::fit_obb_alignment(sourcePts, referenceWithTree)
                                   : tf::make_identity_transformation<float, 3>();

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(45, "Running ICP...");

        // fit_icp_alignment returns the delta relative to the initial guess.
        const auto referenceNormals = worldNormals(doc.mesh(in.referenceIndex));
        const auto refined = pointToPlane
            ? tf::fit_icp_alignment(
                  sourcePts | tf::tag(initial),
                  referenceWithTree | tf::tag_normals(referenceNormals.unit_vectors()),
                  cfg)
            : tf::fit_icp_alignment(sourcePts | tf::tag(initial), referenceWithTree, cfg);
        const auto total = tf::transformed(initial, refined);
        delta = toQMatrix(total);

        residual = double(tf::chamfer_error(sourcePts | tf::tag(total), referenceWithTree));
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm ICP failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    return applyAlignment(
        doc, in.sourceIndex, delta,
        { QObject::tr("Aligned '%1' to '%2'.")
              .arg(doc.mesh(in.sourceIndex).name, doc.mesh(in.referenceIndex).name),
          pointToPlane ? QObject::tr("Metric: point-to-plane.")
                       : QObject::tr("Metric: point-to-point."),
          QObject::tr("Chamfer residual: %1").arg(QString::number(residual, 'g', 6)) });
}

// Procrustes alignment of two layers whose vertices already correspond one to one, in
// order. Unlike ICP this can also solve for uniform scale, because the correspondences
// are given rather than guessed.
MeshFilterRunResult runAlignCorresponding(const FilterParams &params, Document &doc)
{
    AlignmentInputs in;
    QString error;
    if (!resolveInputs(params, doc, in, error))
        return fail(error);

    const bool allowScale = params.getBool(QStringLiteral("allowScale"), false);

    const TfPoints source = worldPoints(doc.mesh(in.sourceIndex));
    const TfPoints reference = worldPoints(doc.mesh(in.referenceIndex));
    if (source.size() != reference.size()) {
        return fail(QObject::tr(
            "This filter needs one-to-one correspondences: both layers must have the same "
            "number of live vertices, in matching order ('%1' has %2, '%3' has %4). Use "
            "an 'Align by ICP' filter when the correspondence is unknown.")
                        .arg(doc.mesh(in.sourceIndex).name)
                        .arg(source.size())
                        .arg(doc.mesh(in.referenceIndex).name)
                        .arg(reference.size()));
    }

    doc.beginFilterProgress(QObject::tr("Align to Corresponding Points (TrueForm)"));
    QMatrix4x4 delta;
    double fittedScale = 1.0;
    try {
        auto sourcePts = tf::make_points(source);
        auto referencePts = tf::make_points(reference);
        if (allowScale) {
            delta = toQMatrix(tf::fit_similarity_alignment(sourcePts, referencePts));
            // The fit stores s*R, so the uniform scale is the norm of any column.
            fittedScale =
                double(QVector3D(delta(0, 0), delta(1, 0), delta(2, 0)).length());
        } else {
            delta = toQMatrix(tf::fit_rigid_alignment(sourcePts, referencePts));
        }
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm alignment failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    return applyAlignment(
        doc, in.sourceIndex, delta,
        { QObject::tr("Aligned '%1' to '%2' over %3 corresponding point(s).")
              .arg(doc.mesh(in.sourceIndex).name, doc.mesh(in.referenceIndex).name)
              .arg(source.size()),
          allowScale
              ? QObject::tr("Similarity fit: rotation, translation and uniform scale %1.")
                    .arg(QString::number(fittedScale, 'g', 6))
              : QObject::tr("Rigid fit: rotation and translation only.") });
}

// ---------------------------------------------------------------------------
// Mesh conversion shared by the boolean/CSG filters
// ---------------------------------------------------------------------------

using TfMesh = tf::polygons_buffer<int, float, 3, 3>;
using TfForms = std::vector<decltype(std::declval<const TfMesh &>().polygons())>;

// A layer's triangles in world space. Booleans across layers are only meaningful once
// each layer's matrix has been applied.
TfMesh tfMeshFromLayer(const Document::MeshEntry &entry)
{
    TfMesh out;
    const VCGMesh &mesh = entry.mesh;
    const QMatrix4x4 &m = entry.transform;

    const std::vector<std::size_t> live = liveVertexIndices(mesh);
    std::vector<int> remap(mesh.vert.size(), -1);
    auto &points = out.points_buffer();
    points.allocate(live.size());
    tf::parallel_for_each(tf::enumerate(tf::make_range(live)), [&](auto pair) {
        auto &&[at, vi] = pair;
        remap[vi] = int(at);
        const vcg::Point3f &p = mesh.vert[vi].cP();
        const QVector3D w = m.map(QVector3D(p.X(), p.Y(), p.Z()));
        points[std::size_t(at)] = tf::make_point(w.x(), w.y(), w.z());
    }, tf::checked);

    const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    if (!base)
        return out;

    tf::sequenced_generate(
        tf::make_sequence_range(mesh.face.size()), out.faces_buffer().data_buffer(),
        [&](std::size_t fi, tf::buffer<int> &corners) {
            const VCGFace &f = mesh.face[fi];
            if (f.IsD())
                return;
            int corner[3];
            for (int k = 0; k < 3; ++k) {
                const ptrdiff_t raw = f.cV(k) - base;
                if (raw < 0 || std::size_t(raw) >= remap.size()
                    || remap[std::size_t(raw)] < 0)
                    return;
                corner[k] = remap[std::size_t(raw)];
            }
            if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2])
                return;
            corners.push_back(corner[0]);
            corners.push_back(corner[1]);
            corners.push_back(corner[2]);
        },
        tf::checked);
    return out;
}

// The layers an N-operand read runs on, in operand order.
std::vector<TfMesh> tfMeshesFromLayers(const QList<int> &layers, Document &doc)
{
    std::vector<TfMesh> meshes;
    meshes.reserve(std::size_t(layers.size()));
    for (int layer : layers)
        meshes.push_back(tfMeshFromLayer(doc.mesh(layer)));
    return meshes;
}

// The views a graph is built over. They borrow the meshes, which the caller keeps alive
// for as long as the graph lives.
TfForms tfFormsOf(const std::vector<TfMesh> &meshes)
{
    TfForms forms;
    forms.reserve(meshes.size());
    for (const TfMesh &m : meshes)
        forms.emplace_back(m.polygons());
    return forms;
}

// Copy a TrueForm result buffer into a fresh document layer. The result is already in
// world space, so the new layer keeps an identity matrix, and a polygon face is fanned
// into triangles. Returns -1 when the buffer carries no face the document can hold; what
// that means is the calling filter's to say.
template <typename Buffer>
int addBufferLayer(Document &doc, const Buffer &buffer)
{
    VCGMesh output;
    const auto points = buffer.points();
    const std::size_t pointCount = std::size_t(points.size());
    if (pointCount > 0) {
        vcg::tri::Allocator<VCGMesh>::AddVertices(output, int(pointCount));
        tf::parallel_for_each(tf::enumerate(points), [&output](auto pair) {
            auto &&[vi, p] = pair;
            output.vert[std::size_t(vi)].P() =
                vcg::Point3f(float(p[0]), float(p[1]), float(p[2]));
        }, tf::checked);
        for (const auto &face : buffer.faces()) {
            const std::size_t n = std::size_t(face.size());
            for (std::size_t k = 2; k < n; ++k) {
                const int a = int(face[0]), b = int(face[k - 1]), c = int(face[k]);
                if (a < 0 || b < 0 || c < 0)
                    continue;
                if (std::size_t(a) >= pointCount || std::size_t(b) >= pointCount
                    || std::size_t(c) >= pointCount)
                    continue;
                if (a == b || b == c || a == c)
                    continue;
                vcg::tri::Allocator<VCGMesh>::AddFace(output, a, b, c);
            }
        }
    }

    if (output.FN() <= 0)
        return -1;

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(output);
    vcg::tri::UpdateBounding<VCGMesh>::Box(output);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);

    const int ioMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    return doc.addMesh(output, {}, ioMask);
}

// The single-result form: one layer, and the progress and reporting around it.
template <typename Buffer>
MeshFilterRunResult addResultLayer(
    Document &doc, const Buffer &buffer, const QString &layerName,
    const QString &emptyMessage, QStringList info)
{
    const int newIndex = addBufferLayer(doc, buffer);
    if (newIndex < 0) {
        doc.finishFilterProgress(false, emptyMessage);
        return fail(emptyMessage);
    }
    doc.finishFilterProgress(true, QObject::tr("Created %1.").arg(layerName));

    const VCGMesh &output = doc.mesh(newIndex).mesh;
    info << QObject::tr("Output: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    result.newMeshIndices.push_back(newIndex);
    return result;
}

// The many-result form every decomposition shares: one layer per buffer, named in turn,
// and the run reported once over the whole set. An empty buffer produces no layer, so the
// reported indices are exactly the layers that exist.
template <typename Buffers, typename Naming>
MeshFilterRunResult addResultLayers(
    Document &doc, const Buffers &buffers, Naming name_of,
    const QString &emptyMessage, QStringList info)
{
    QVector<int> newIndices;
    newIndices.reserve(int(buffers.size()));
    for (std::size_t k = 0; k < buffers.size(); ++k) {
        const int newIndex = addBufferLayer(doc, buffers[k]);
        if (newIndex >= 0)
            newIndices.push_back(newIndex);
    }

    if (newIndices.isEmpty()) {
        doc.finishFilterProgress(false, emptyMessage);
        return fail(emptyMessage);
    }
    doc.finishFilterProgress(
        true, QObject::tr("Created %1 layer(s).").arg(newIndices.size()));

    int vertices = 0, faces = 0;
    QStringList names;
    for (int index : newIndices) {
        const VCGMesh &output = doc.mesh(index).mesh;
        vertices += output.VN();
        faces += output.FN();
        names << doc.mesh(index).name;
    }
    info.prepend(QObject::tr("Created %1 layer(s): %2.")
                     .arg(newIndices.size()).arg(names.join(QStringLiteral(", "))));
    info << QObject::tr("Output: %1 vertices, %2 faces.").arg(vertices).arg(faces);
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    result.newMeshIndices = newIndices;
    return result;
}

// ---------------------------------------------------------------------------
// Booleans
// ---------------------------------------------------------------------------

// The two-layer preamble every boolean shares.
bool resolveBooleanPair(const FilterParams &params, Document &doc, int &a, int &b, QString &error)
{
    a = params.getMesh(QStringLiteral("firstMesh"), doc.currentMeshIndex());
    b = params.getMesh(QStringLiteral("secondMesh"), -1);
    const auto valid = [&doc](int i) { return i >= 0 && i < doc.meshCount(); };
    if (!valid(a) || !valid(b)) {
        error = QObject::tr("Two layers must be selected.");
        return false;
    }
    if (a == b) {
        error = QObject::tr("The two layers must be different.");
        return false;
    }
    if (doc.mesh(a).mesh.FN() <= 0 || doc.mesh(b).mesh.FN() <= 0) {
        error = QObject::tr("Both layers need faces.");
        return false;
    }
    return true;
}

MeshFilterRunResult runBoolean(const QString &filterId, const FilterParams &params, Document &doc)
{
    int aIndex = -1, bIndex = -1;
    QString error;
    if (!resolveBooleanPair(params, doc, aIndex, bIndex, error))
        return fail(error);

    tf::boolean_op op = tf::boolean_op::merge;
    QString label;
    if (filterId == QString::fromLatin1(kFilterUnion)) {
        op = tf::boolean_op::merge;
        label = QObject::tr("Union");
    } else if (filterId == QString::fromLatin1(kFilterIntersection)) {
        op = tf::boolean_op::intersection;
        label = QObject::tr("Intersection");
    } else {
        op = tf::boolean_op::left_difference;
        label = QObject::tr("Difference");
    }

    doc.beginFilterProgress(QObject::tr("Mesh %1 (TrueForm)").arg(label));
    try {
        const TfMesh a = tfMeshFromLayer(doc.mesh(aIndex));
        const TfMesh b = tfMeshFromLayer(doc.mesh(bIndex));
        auto [mesh, tagLabels, faceLabels] =
            tf::make_boolean(a.polygons(), b.polygons(), op);
        return addResultLayer(
            doc, mesh, label,
            QObject::tr("The %1 is empty.").arg(label),
            { QObject::tr("'%1' and '%2'.").arg(doc.mesh(aIndex).name, doc.mesh(bIndex).name) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm boolean failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// ---------------------------------------------------------------------------
// CSG over many layers
// ---------------------------------------------------------------------------

// A small recursive-descent parser over layer indices and the four CSG operators.
//
//   expression := term (('|' | '-') term)*
//   term       := factor ('&' factor)*
//   factor     := '~' factor | number | '(' expression ')'
//
// Union and difference share the lowest precedence and associate left, intersection
// binds tighter, and complement binds tightest — the usual reading of "A | B - C".
class CsgExpressionParser
{
public:
    CsgExpressionParser(const QString &text, int meshCount)
        : m_text(text), m_meshCount(meshCount) {}

    bool parse(std::optional<tf::csg::expr> &out, QString &error)
    {
        m_pos = 0;
        m_error.clear();
        m_operands.clear();
        if (!parseExpression(out) || !out.has_value()) {
            error = m_error.isEmpty() ? QObject::tr("Could not parse the expression.") : m_error;
            return false;
        }
        skipSpace();
        if (m_pos != m_text.size()) {
            error = QObject::tr("Unexpected '%1' at position %2.")
                        .arg(m_text.mid(m_pos, 1)).arg(m_pos);
            return false;
        }
        if (m_operands.isEmpty()) {
            error = QObject::tr("The expression refers to no layers.");
            return false;
        }
        return true;
    }

    // Layer indices used, in first-seen order; this is the operand order of the graph.
    QList<int> operands() const { return m_operands; }

private:
    void skipSpace()
    {
        while (m_pos < m_text.size() && m_text.at(m_pos).isSpace())
            ++m_pos;
    }

    bool peek(QChar c)
    {
        skipSpace();
        return m_pos < m_text.size() && m_text.at(m_pos) == c;
    }

    bool parseExpression(std::optional<tf::csg::expr> &out)
    {
        if (!parseTerm(out) || !out.has_value())
            return false;
        while (true) {
            const bool isUnion = peek(QLatin1Char('|'));
            if (!isUnion && !peek(QLatin1Char('-')))
                return true;
            ++m_pos;
            std::optional<tf::csg::expr> rhs;
            if (!parseTerm(rhs) || !rhs.has_value())
                return false;
            out = isUnion ? (*out | *rhs) : (*out - *rhs);
        }
    }

    bool parseTerm(std::optional<tf::csg::expr> &out)
    {
        if (!parseFactor(out) || !out.has_value())
            return false;
        while (peek(QLatin1Char('&'))) {
            ++m_pos;
            std::optional<tf::csg::expr> rhs;
            if (!parseFactor(rhs) || !rhs.has_value())
                return false;
            out = *out & *rhs;
        }
        return true;
    }

    bool parseFactor(std::optional<tf::csg::expr> &out)
    {
        skipSpace();
        if (m_pos >= m_text.size()) {
            m_error = QObject::tr("The expression ends unexpectedly.");
            return false;
        }
        if (peek(QLatin1Char('~'))) {
            ++m_pos;
            std::optional<tf::csg::expr> inner;
            if (!parseFactor(inner) || !inner.has_value())
                return false;
            out = ~(*inner);
            return true;
        }
        if (peek(QLatin1Char('('))) {
            ++m_pos;
            if (!parseExpression(out))
                return false;
            if (!peek(QLatin1Char(')'))) {
                m_error = QObject::tr("Missing ')'.");
                return false;
            }
            ++m_pos;
            return true;
        }
        skipSpace();
        const int start = m_pos;
        while (m_pos < m_text.size() && m_text.at(m_pos).isDigit())
            ++m_pos;
        if (m_pos == start) {
            m_error = QObject::tr("Expected a layer number at position %1, found '%2'.")
                          .arg(start).arg(m_text.mid(start, 1));
            return false;
        }
        const int layer = m_text.mid(start, m_pos - start).toInt();
        if (layer < 0 || layer >= m_meshCount) {
            m_error = QObject::tr("Layer %1 does not exist; the document has %2.")
                          .arg(layer).arg(m_meshCount);
            return false;
        }
        // The graph is built from the operands in first-seen order, so an expression
        // referring to a layer twice must reuse the same operand id.
        int operandId = int(m_operands.indexOf(layer));
        if (operandId < 0) {
            operandId = int(m_operands.size());
            m_operands.append(layer);
        }
        out.emplace(operandId);
        return true;
    }

    QString m_text;
    int m_meshCount = 0;
    int m_pos = 0;
    QString m_error;
    QList<int> m_operands;
};

// Layer numbers written the way the CSG expression writes its operands, separated by
// spaces or commas. Empty text is an empty list rather than an error: it is how a caller
// says "none of them".
bool parseLayerList(const QString &text, int meshCount, QList<int> &layers, QString &error)
{
    layers.clear();
    static const QRegularExpression separators(QStringLiteral("[\\s,]+"));
    const QStringList parts = text.trimmed().split(separators, Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        bool ok = false;
        const int layer = part.toInt(&ok);
        if (!ok) {
            error = QObject::tr("'%1' is not a layer number.").arg(part);
            return false;
        }
        if (layer < 0 || layer >= meshCount) {
            error = QObject::tr("Layer %1 does not exist; the document has %2.")
                        .arg(layer).arg(meshCount);
            return false;
        }
        if (!layers.contains(layer))
            layers.append(layer);
    }
    return true;
}

// The sheet declaration TrueForm's graph takes: operand ids, not layer numbers. A sheet
// cuts only inside the arrangement it belongs to, so a layer named here has to be one of
// the operands, in the order the graph will see them.
bool resolveSheetTags(
    const QString &text, const QList<int> &operands, Document &doc,
    std::vector<int> &tags, QString &error)
{
    QList<int> layers;
    if (!parseLayerList(text, doc.meshCount(), layers, error))
        return false;
    tags.clear();
    tags.reserve(std::size_t(layers.size()));
    for (int layer : layers) {
        const int tag = int(operands.indexOf(layer));
        if (tag < 0) {
            error = QObject::tr("Layer %1 ('%2') is listed as a sheet but is not one of "
                                "the operands.")
                        .arg(layer).arg(doc.mesh(layer).name);
            return false;
        }
        tags.push_back(tag);
    }
    return true;
}

MeshFilterRunResult runCsgExpression(const FilterParams &params, Document &doc)
{
    const QString text = params.getString(QStringLiteral("expression")).trimmed();
    if (text.isEmpty())
        return fail(QObject::tr("The expression is empty."));

    CsgExpressionParser parser(text, doc.meshCount());
    std::optional<tf::csg::expr> expression;
    QString error;
    if (!parser.parse(expression, error))
        return fail(error);

    const QList<int> operands = parser.operands();
    for (int layer : operands) {
        if (doc.mesh(layer).mesh.FN() <= 0) {
            return fail(QObject::tr("Layer %1 ('%2') has no faces.")
                            .arg(layer).arg(doc.mesh(layer).name));
        }
    }

    std::vector<int> sheets;
    if (!resolveSheetTags(params.getString(QStringLiteral("sheets")), operands, doc,
                          sheets, error))
        return fail(error);

    doc.beginFilterProgress(QObject::tr("Mesh CSG Expression (TrueForm)"));
    try {
        const std::vector<TfMesh> meshes = tfMeshesFromLayers(operands, doc);
        const TfForms forms = tfFormsOf(meshes);

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(30, "Building the arrangement...");
        auto graph = tf::make_csg_graph(tf::make_range(forms.data(), forms.size()),
                                        tf::make_range(sheets.data(), sheets.size()));

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(70, "Evaluating the expression...");
        auto result = tf::make_csg_mesh(graph, *expression);

        QStringList used;
        for (int layer : operands)
            used << QObject::tr("%1 = '%2'").arg(layer).arg(doc.mesh(layer).name);
        QStringList info{ QObject::tr("Expression: %1").arg(text),
                          used.join(QStringLiteral(", ")) };
        if (!sheets.empty()) {
            QStringList named;
            for (int tag : sheets)
                named << QObject::tr("'%1'").arg(doc.mesh(operands.at(tag)).name);
            info << QObject::tr("Cutting with %1 as %2.")
                        .arg(named.join(QStringLiteral(", ")),
                             sheets.size() == 1 ? QObject::tr("a sheet")
                                                : QObject::tr("sheets"));
        }
        return addResultLayer(
            doc, result, QObject::tr("CSG"),
            QObject::tr("The expression evaluates to an empty solid."), info);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm CSG failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Symmetric difference is not one of TrueForm's boolean_op values, so it goes through
// the CSG evaluator as (A - B) | (B - A).
MeshFilterRunResult runSymmetricDifference(const FilterParams &params, Document &doc)
{
    int aIndex = -1, bIndex = -1;
    QString error;
    if (!resolveBooleanPair(params, doc, aIndex, bIndex, error))
        return fail(error);

    doc.beginFilterProgress(QObject::tr("Mesh Symmetric Difference (TrueForm)"));
    try {
        const TfMesh a = tfMeshFromLayer(doc.mesh(aIndex));
        const TfMesh b = tfMeshFromLayer(doc.mesh(bIndex));
        auto graph = tf::make_csg_graph(a.polygons(), b.polygons());

        const tf::csg::expr lhs(0);
        const tf::csg::expr rhs(1);
        auto result = tf::make_csg_mesh(graph, (lhs - rhs) | (rhs - lhs));
        return addResultLayer(
            doc, result, QObject::tr("Symmetric Difference"),
            QObject::tr("The symmetric difference is empty; the two layers may coincide."),
            { QObject::tr("'%1' and '%2'.").arg(doc.mesh(aIndex).name, doc.mesh(bIndex).name) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm symmetric difference failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// The outermost boundary of a single layer, discarding internal shells and resolving
// self-intersections along the way. Useful before 3D printing, and as a repair step on
// geometry assembled from overlapping parts.
MeshFilterRunResult runOuterShell(const FilterParams &params, Document &doc)
{
    const int index = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    doc.beginFilterProgress(QObject::tr("Extract Outer Shell (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        auto result = tf::make_outer_shell(source.polygons());
        return addResultLayer(
            doc, result, QObject::tr("Outer Shell"),
            QObject::tr("No outer shell was found. The layer may be open rather than solid."),
            { QObject::tr("From '%1'.").arg(doc.mesh(index).name) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm outer shell failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Every region of space the operands enclose, each as its own watertight layer. A boolean
// picks one of those regions and merges the rest away; this keeps them all and keeps them
// apart, which is the read nothing else in the ecosystem offers.
MeshFilterRunResult runSolidDomains(const FilterParams &params, Document &doc)
{
    QList<int> layers;
    QString error;
    if (!parseLayerList(params.getString(QStringLiteral("layers")), doc.meshCount(),
                        layers, error))
        return fail(error);
    if (layers.isEmpty())
        return fail(QObject::tr("No layers were listed."));
    for (int layer : layers) {
        if (doc.mesh(layer).mesh.FN() <= 0) {
            return fail(QObject::tr("Layer %1 ('%2') has no faces.")
                            .arg(layer).arg(doc.mesh(layer).name));
        }
    }

    std::vector<int> sheets;
    if (!resolveSheetTags(params.getString(QStringLiteral("sheets")), layers, doc,
                          sheets, error))
        return fail(error);

    doc.beginFilterProgress(QObject::tr("Split into Solid Domains (TrueForm)"));
    try {
        const std::vector<TfMesh> meshes = tfMeshesFromLayers(layers, doc);
        const TfForms forms = tfFormsOf(meshes);

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(30, "Building the arrangement...");
        auto graph = tf::make_csg_graph(tf::make_range(forms.data(), forms.size()),
                                        tf::make_range(sheets.data(), sheets.size()));

        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(70, "Extracting the domains...");
        auto domains = tf::make_csg_domains(graph);
        const auto &cells = domains.first;
        const auto &ids = domains.second;

        QStringList used;
        for (int layer : layers)
            used << QObject::tr("'%1'").arg(doc.mesh(layer).name);
        return addResultLayers(
            doc, cells,
            [&ids](std::size_t k) { return QObject::tr("Domain %1").arg(ids[k]); },
            QObject::tr("The layers enclose nothing to decompose."),
            { QObject::tr("From %1.").arg(used.join(QStringLiteral(", "))) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm domain decomposition failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// ---------------------------------------------------------------------------
// Connected components
// ---------------------------------------------------------------------------

// Per-face component labels under the asked connectivity. TrueForm has a producer for
// each standard; the vertex-connected one labels VERTICES rather than faces, and a face's
// corners are all linked to each other through the face's own edges, so the component of
// any one corner is the component of the face.
tf::buffer<int> faceComponentLabels(const TfMesh &source, const QString &connectivity)
{
    if (connectivity == QStringLiteral("edge")) {
        auto cc = tf::make_edge_connected_component_labels(source.polygons());
        return std::move(cc.labels);
    }
    if (connectivity == QStringLiteral("vertex")) {
        const auto cc = tf::make_vertex_connected_component_labels(source.polygons());
        tf::buffer<int> labels;
        labels.allocate(source.polygons().size());
        tf::parallel_for_each(tf::enumerate(source.polygons().faces()),
            [&labels, &cc](auto pair) {
                auto &&[fi, face] = pair;
                labels[std::size_t(fi)] = cc.labels[std::size_t(face[0])];
            }, tf::checked);
        return labels;
    }
    auto cc = tf::make_manifold_edge_connected_component_labels(source.polygons());
    return std::move(cc.labels);
}

// One layer per surface that stands on its own. TrueForm labels the faces in parallel and
// splits them in one pass, which is where the speed on a large mesh comes from, and it has
// a producer for each of the three connectivity standards the parameter offers.
MeshFilterRunResult runSplitComponents(const FilterParams &params, Document &doc)
{
    const int index = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const QString connectivity = params.getEnum(QStringLiteral("connectivity"));

    doc.beginFilterProgress(QObject::tr("Split into Connected Components (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        const tf::buffer<int> faceLabels = faceComponentLabels(source, connectivity);
        auto split = tf::split_into_components(source.polygons(), faceLabels);
        const auto &components = split.first;
        const auto &componentLabels = split.second;

        return addResultLayers(
            doc, components,
            [&componentLabels](std::size_t k) {
                return QObject::tr("Component %1").arg(componentLabels[k]);
            },
            QObject::tr("The layer has no connected component to split out."),
            { QObject::tr("From '%1'.").arg(doc.mesh(index).name) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm component splitting failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// ---------------------------------------------------------------------------
// Curves
// ---------------------------------------------------------------------------

// Turn a TrueForm curves_buffer into a MeshLab polyline layer: an edge mesh, which is
// what the Create Polyline family already produces.
// Append one curves_buffer into an edge mesh, stamping `edgeQuality` on every segment
// it produces. Isocontours are the reason this takes a scalar: TrueForm's multi-value
// overload merges every level into one buffer with no way to tell which path came from
// which level, so the caller extracts one level at a time and labels it here.
template <typename Curves>
std::size_t appendCurvesToMesh(VCGMesh &output, const Curves &curves, float edgeQuality)
{
    const auto points = curves.points();
    const std::size_t pointCount = std::size_t(points.size());
    if (pointCount == 0)
        return 0;

    const std::size_t vertexBase = output.vert.size();
    vcg::tri::Allocator<VCGMesh>::AddVertices(output, int(pointCount));
    tf::parallel_for_each(tf::enumerate(points), [&output, vertexBase](auto pair) {
        auto &&[vi, p] = pair;
        output.vert[vertexBase + std::size_t(vi)].P() =
            vcg::Point3f(float(p[0]), float(p[1]), float(p[2]));
    }, tf::checked);

    std::size_t segmentCount = 0;
    for (const auto &path : curves.paths()) {
        const std::size_t n = std::size_t(path.size());
        for (std::size_t k = 1; k < n; ++k) {
            const int a = int(path[k - 1]);
            const int b = int(path[k]);
            if (a == b || a < 0 || b < 0)
                continue;
            if (std::size_t(a) >= pointCount || std::size_t(b) >= pointCount)
                continue;
            auto e = vcg::tri::Allocator<VCGMesh>::AddEdges(output, 1);
            e->V(0) = &output.vert[vertexBase + std::size_t(a)];
            e->V(1) = &output.vert[vertexBase + std::size_t(b)];
            e->Q() = edgeQuality;
            ++segmentCount;
        }
    }
    return segmentCount;
}

// Finish an edge mesh built by one or more appendCurvesToMesh calls.
inline MeshFilterRunResult finishPolylineLayer(
    Document &doc, VCGMesh &output, std::size_t segmentCount, std::size_t pathCount,
    const QString &layerName, const QString &emptyMessage, QStringList info,
    int extraMask = 0)
{
    if (segmentCount == 0) {
        doc.finishFilterProgress(false, emptyMessage);
        return fail(emptyMessage);
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(output);
    vcg::tri::UpdateBounding<VCGMesh>::Box(output);

    const int newIndex = doc.addMesh(output, {}, Mask::IOM_EDGEINDEX | extraMask);
    if (newIndex < 0) {
        const QString message = QObject::tr("Failed to add the %1 layer.").arg(layerName);
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    doc.finishFilterProgress(true, QObject::tr("Created %1.").arg(layerName));

    info.prepend(QObject::tr("Created polyline '%1'.").arg(doc.mesh(newIndex).name));
    info << QObject::tr("%1 path(s), %2 segment(s).")
                .arg(pathCount).arg(segmentCount);
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    result.newMeshIndices.push_back(newIndex);
    return result;
}

template <typename Curves>
MeshFilterRunResult addPolylineLayer(
    Document &doc, const Curves &curves, const QString &layerName,
    const QString &emptyMessage, QStringList info)
{
    VCGMesh output;
    const std::size_t segmentCount = appendCurvesToMesh(output, curves, 0.0f);
    return finishPolylineLayer(
        doc, output, segmentCount, std::size_t(curves.paths().size()),
        layerName, emptyMessage, std::move(info));
}

// Where a mesh passes through itself. Unlike Select Self-Intersecting Faces, which marks
// the faces involved, this extracts the intersection curve itself.
MeshFilterRunResult runSelfIntersectionCurves(const FilterParams &params, Document &doc)
{
    const int index = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    doc.beginFilterProgress(QObject::tr("Create Polyline from Self-Intersections (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        // v0.10.0 replaced embedded_self_intersection_curves with this, which returns the
        // curves themselves rather than a tuple ending in them. The default config is the
        // same primitives | resolve_contours the old entry point used.
        auto curves = tf::make_self_intersection_curves(source.polygons());
        return addPolylineLayer(
            doc, curves, QObject::tr("Self-Intersections"),
            QObject::tr("No self-intersections were found."),
            { QObject::tr("From '%1'.").arg(doc.mesh(index).name) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm self-intersection curves failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Where two layers cross. The curve is exact, so it is usable as a construction line
// rather than only as a diagnostic.
MeshFilterRunResult runIntersectionCurves(const FilterParams &params, Document &doc)
{
    int aIndex = -1, bIndex = -1;
    QString error;
    if (!resolveBooleanPair(params, doc, aIndex, bIndex, error))
        return fail(error);

    doc.beginFilterProgress(QObject::tr("Create Polyline from Mesh Intersection (TrueForm)"));
    try {
        const TfMesh a = tfMeshFromLayer(doc.mesh(aIndex));
        const TfMesh b = tfMeshFromLayer(doc.mesh(bIndex));
        auto curves = tf::make_intersection_curves(a.polygons(), b.polygons());
        return addPolylineLayer(
            doc, curves, QObject::tr("Intersection Curve"),
            QObject::tr("The two layers do not intersect."),
            { QObject::tr("'%1' against '%2'.")
                  .arg(doc.mesh(aIndex).name, doc.mesh(bIndex).name) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm intersection curves failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Contours of the per-vertex scalar field, as polylines lying on the surface. This is
// what makes every scalar MeshLab computes — geodesic distance, curvature, ambient
// occlusion, raster coverage — into something with extractable level sets.
MeshFilterRunResult runIsocurves(const FilterParams &params, Document &doc)
{
    const int index = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));
    const VCGMesh &mesh = doc.mesh(index).mesh;
    if (mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const int count = std::max(1, params.getInt(QStringLiteral("contourCount"), 10));
    const bool useRange = params.getBool(QStringLiteral("useCustomRange"), false);
    double minValue = params.getDouble(QStringLiteral("minValue"), 0.0);
    double maxValue = params.getDouble(QStringLiteral("maxValue"), 0.0);

    // Gather the scalars in the same order tfMeshFromLayer emits points.
    std::vector<float> scalars;
    scalars.reserve(std::size_t(std::max(0, mesh.VN())));
    float observedMin = std::numeric_limits<float>::max();
    float observedMax = std::numeric_limits<float>::lowest();
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        const float q = v.cQ();
        scalars.push_back(q);
        if (std::isfinite(q)) {
            observedMin = std::min(observedMin, q);
            observedMax = std::max(observedMax, q);
        }
    }
    if (scalars.empty())
        return fail(QObject::tr("The layer has no live vertices."));
    if (!(observedMax > observedMin)) {
        return fail(QObject::tr(
            "The scalar field is constant, so it has no contours. Compute a per-vertex "
            "scalar first — a geodesic distance or a curvature, for instance."));
    }
    if (!useRange) {
        minValue = double(observedMin);
        maxValue = double(observedMax);
    }
    if (!(maxValue > minValue))
        return fail(QObject::tr("The maximum must be larger than the minimum."));

    // Contours strictly inside the range: a contour exactly at the extreme is either
    // empty or the whole boundary, neither of which is useful.
    std::vector<float> cutValues;
    cutValues.reserve(std::size_t(count));
    for (int i = 0; i < count; ++i) {
        const double t = (double(i) + 1.0) / (double(count) + 1.0);
        cutValues.push_back(float(minValue + t * (maxValue - minValue)));
    }

    doc.beginFilterProgress(QObject::tr("Create Polyline from Scalar Isocontour (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        // One extraction per level rather than the multi-value call, because that one
        // merges every level into a single buffer and discards which is which. Taking
        // them one at a time is what lets each contour keep its own value, which is the
        // difference between a uniform hairball and something Colorize Edges by Scalar
        // can turn into readable isolines.
        VCGMesh output;
        std::size_t segmentCount = 0;
        std::size_t pathCount = 0;
        int levelsWithContours = 0;
        for (const float cutValue : cutValues) {
            auto curves = tf::make_isocontours(
                source.polygons(),
                tf::make_range(scalars.data(), scalars.size()),
                cutValue);
            const std::size_t added = appendCurvesToMesh(output, curves, cutValue);
            if (added > 0) {
                ++levelsWithContours;
                pathCount += std::size_t(curves.paths().size());
            }
            segmentCount += added;
        }
        return finishPolylineLayer(
            doc, output, segmentCount, pathCount, QObject::tr("Isocontours"),
            QObject::tr("No contours were produced at the requested values."),
            { QObject::tr("From '%1'.").arg(doc.mesh(index).name),
              QObject::tr("%1 contour level(s) between %2 and %3; %4 produced geometry.")
                  .arg(count)
                  .arg(QString::number(minValue, 'g', 6))
                  .arg(QString::number(maxValue, 'g', 6))
                  .arg(levelsWithContours),
              QObject::tr("Each edge carries its contour value as its scalar. Run "
                          "Colorize Edges by Scalar to see the levels apart.") },
            Mask::IOM_EDGEQUALITY);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm isocurves failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Sweep a circular profile along a polyline. The Create Polyline family produces edge
// meshes that render as hairlines; this turns one into geometry that can be shaded,
// exported or printed.
MeshFilterRunResult runTubeFromPolyline(const FilterParams &params, Document &doc)
{
    const int index = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));

    const Document::MeshEntry &entry = doc.mesh(index);
    const VCGMesh &mesh = entry.mesh;
    if (mesh.EN() <= 0) {
        return fail(QObject::tr(
            "'%1' has no edges. This filter sweeps a polyline layer, such as one made by "
            "the Create Polyline filters.").arg(entry.name));
    }

    const double radius = params.getDouble(QStringLiteral("radius"), 0.0);
    const int segments = std::clamp(params.getInt(QStringLiteral("segments"), 8), 3, 256);
    if (!std::isfinite(radius) || radius <= 0.0)
        return fail(QObject::tr("The radius must be a finite value larger than zero."));

    const std::size_t vertexCount = mesh.vert.size();
    tf::buffer<int> edgeIds;
    edgeIds.reserve(std::size_t(std::max(0, mesh.EN())) * 2);
    std::vector<int> valence(vertexCount, 0);
    const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    for (const auto &e : mesh.edge) {
        if (e.IsD() || !base)
            continue;
        const ptrdiff_t v0 = e.cV(0) - base;
        const ptrdiff_t v1 = e.cV(1) - base;
        if (v0 < 0 || v1 < 0 || std::size_t(v0) >= vertexCount || std::size_t(v1) >= vertexCount)
            continue;
        edgeIds.push_back(int(v0));
        edgeIds.push_back(int(v1));
        ++valence[std::size_t(v0)];
        ++valence[std::size_t(v1)];
    }
    if (edgeIds.size() == 0)
        return fail(QObject::tr("The layer has no usable edges."));

    int branching = 0;
    for (int n : valence) {
        if (n > 2)
            ++branching;
    }

    doc.beginFilterProgress(QObject::tr("Create Tube from Polyline (TrueForm)"));

    try {
        tf::curves_buffer<int, float, 3> polyline;
        polyline.paths_buffer() = tf::connect_edges_to_paths(tf::make_edges(edgeIds));

        const QMatrix4x4 &m = entry.transform;
        auto &points = polyline.points_buffer();
        points.allocate(vertexCount);
        tf::parallel_for_each(tf::make_sequence_range(vertexCount), [&](std::size_t i) {
            const vcg::Point3f &p = mesh.vert[i].cP();
            const QVector3D w = m.map(QVector3D(p.X(), p.Y(), p.Z()));
            points[i] = tf::make_point(w.x(), w.y(), w.z());
        }, tf::checked);

        auto tubes = tf::make_tube_mesh(polyline.curves(), float(radius), segments);

        QStringList info;
        info << QObject::tr("Swept %1 path(s) from '%2'.")
                    .arg(polyline.paths_buffer().size()).arg(entry.name);
        if (branching > 0) {
            info << QObject::tr(
                "%1 vertex junction(s) joined more than two edges; the polyline was split "
                "there rather than branched.").arg(branching);
        }
        return addResultLayer(
            doc, tubes, QObject::tr("Tube"),
            QObject::tr("The sweep produced no geometry."), info);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm tube generation failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// ---------------------------------------------------------------------------
// Distance and containment
// ---------------------------------------------------------------------------

// signed_distance() needs the polygons tagged with a spatial index, face membership and
// the manifold edge link: the tree finds candidates, and the other two let it decide
// which side of the surface a point falls on near an edge or a vertex, where the closest
// face alone is ambiguous. The tags reference these objects, so they must outlive use.
struct SignedDistanceContext
{
    explicit SignedDistanceContext(const TfMesh &mesh)
        : membership(mesh.polygons())
        , edgeLink(mesh.polygons().faces(), membership)
        , tree(mesh.polygons(), tf::config_tree(4, 4))
    {}

    tf::face_membership<int> membership;
    tf::manifold_edge_link<int, 3> edgeLink;
    tf::aabb_tree<int, float, 3> tree;
};

// Signed distance from every vertex of one layer to the surface of another. Negative
// inside, positive outside — which is what makes it a field rather than a proximity
// readout, and what lets the containment filter share the same machinery.
MeshFilterRunResult runSignedDistance(const FilterParams &params, Document &doc)
{
    const int targetIndex = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    const int referenceIndex = params.getMesh(QStringLiteral("referenceMesh"), -1);
    const auto valid = [&doc](int i) { return i >= 0 && i < doc.meshCount(); };
    if (!valid(targetIndex) || !valid(referenceIndex))
        return fail(QObject::tr("A layer and a reference layer must be selected."));
    if (targetIndex == referenceIndex)
        return fail(QObject::tr("The two layers must be different."));
    if (doc.mesh(referenceIndex).mesh.FN() <= 0)
        return fail(QObject::tr("The reference layer needs faces."));
    if (doc.mesh(targetIndex).mesh.VN() <= 0)
        return fail(QObject::tr("The layer needs vertices."));

    const bool absolute = params.getBool(QStringLiteral("unsigned"), false);

    doc.beginFilterProgress(QObject::tr("Compute Signed Distance to Mesh (TrueForm)"));
    double minDistance = std::numeric_limits<double>::max();
    double maxDistance = std::numeric_limits<double>::lowest();
    int insideCount = 0;
    try {
        const TfMesh reference = tfMeshFromLayer(doc.mesh(referenceIndex));
        SignedDistanceContext ctx(reference);
        auto tagged = reference.polygons() | tf::tag(ctx.tree) | tf::tag(ctx.membership)
            | tf::tag(ctx.edgeLink);

        Document::MeshEntry &entry = doc.mesh(targetIndex);
        const TfPoints queries = worldPoints(entry);
        tf::buffer<double> distances;
        distances.allocate(std::size_t(queries.size()));
        tf::parallel_transform(
            queries, distances,
            [&tagged](const auto &q) { return tf::signed_distance(tagged, q); },
            tf::checked);

        const std::vector<std::size_t> live = liveVertexIndices(entry.mesh);
        for (std::size_t i = 0; i < live.size(); ++i) {
            double d = distances[i];
            if (d < 0.0)
                ++insideCount;
            if (absolute)
                d = std::abs(d);
            entry.mesh.vert[live[i]].Q() = float(d);
            minDistance = std::min(minDistance, d);
            maxDistance = std::max(maxDistance, d);
        }
        entry.ioMask |= Mask::IOM_VERTQUALITY;
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm signed distance failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    doc.markMeshGeometryChanged(
        targetIndex,
        QObject::tr("Computed distance from '%1' to '%2'")
            .arg(doc.mesh(targetIndex).name, doc.mesh(referenceIndex).name));
    doc.finishFilterProgress(true, QObject::tr("Computed signed distance."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Range: %1 to %2")
               .arg(QString::number(minDistance, 'g', 6))
               .arg(QString::number(maxDistance, 'g', 6))
        << QObject::tr("%1 vertex(es) lie inside '%2'.")
               .arg(insideCount).arg(doc.mesh(referenceIndex).name);
    result.visualizationHints.push_back(
        { targetIndex, MeshFilterVisualizationAttribute::VertexQuality });
    return result;
}

// Select the vertices enclosed by another layer. Uses the sign of the distance rather
// than a ray-parity test, so a point exactly on the surface is decided consistently.
MeshFilterRunResult runSelectInsideMesh(const FilterParams &params, Document &doc)
{
    const int targetIndex = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    const int referenceIndex = params.getMesh(QStringLiteral("referenceMesh"), -1);
    const auto valid = [&doc](int i) { return i >= 0 && i < doc.meshCount(); };
    if (!valid(targetIndex) || !valid(referenceIndex))
        return fail(QObject::tr("A layer and an enclosing layer must be selected."));
    if (targetIndex == referenceIndex)
        return fail(QObject::tr("The two layers must be different."));
    if (doc.mesh(referenceIndex).mesh.FN() <= 0)
        return fail(QObject::tr("The enclosing layer needs faces."));

    const bool invert = params.getBool(QStringLiteral("selectOutside"), false);
    const QString mode = params.getEnum(QStringLiteral("mode"));

    doc.beginFilterProgress(QObject::tr("Select Vertices Inside Mesh (TrueForm)"));
    try {
        const TfMesh reference = tfMeshFromLayer(doc.mesh(referenceIndex));
        SignedDistanceContext ctx(reference);
        auto tagged = reference.polygons() | tf::tag(ctx.tree) | tf::tag(ctx.membership)
            | tf::tag(ctx.edgeLink);

        Document::MeshEntry &entry = doc.mesh(targetIndex);
        const TfPoints queries = worldPoints(entry);
        tf::buffer<double> distances;
        distances.allocate(std::size_t(queries.size()));
        tf::parallel_transform(
            queries, distances,
            [&tagged](const auto &q) { return tf::signed_distance(tagged, q); },
            tf::checked);

        const std::vector<std::size_t> live = liveVertexIndices(entry.mesh);
        const bool replace = (mode != QStringLiteral("add") && mode != QStringLiteral("subtract"));
        for (std::size_t i = 0; i < live.size(); ++i) {
            VCGVertex &v = entry.mesh.vert[live[i]];
            const double d = distances[i];
            const bool hit = invert ? (d > 0.0) : (d < 0.0);
            if (replace)
                hit ? v.SetS() : v.ClearS();
            else if (mode == QStringLiteral("add") && hit)
                v.SetS();
            else if (mode == QStringLiteral("subtract") && hit)
                v.ClearS();
        }
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm containment test failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    doc.markMeshSelectionChanged(
        targetIndex,
        QObject::tr("Selected vertices of '%1' inside '%2'")
            .arg(doc.mesh(targetIndex).name, doc.mesh(referenceIndex).name));
    doc.finishFilterProgress(true, QObject::tr("Updated selection."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    // The count comes from the framework, in the wording shared by every selection filter.
    return result;
}

// Mean distance from one layer's vertices to the nearest point of another's. Unlike the
// Hausdorff distance, which reports the single worst correspondence, this averages — so
// it is stable against a lone outlier and useful as a fit score.
MeshFilterRunResult runChamferDistance(const FilterParams &params, Document &doc)
{
    const int aIndex = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    const int bIndex = params.getMesh(QStringLiteral("referenceMesh"), -1);
    const auto valid = [&doc](int i) { return i >= 0 && i < doc.meshCount(); };
    if (!valid(aIndex) || !valid(bIndex))
        return fail(QObject::tr("Two layers must be selected."));
    if (aIndex == bIndex)
        return fail(QObject::tr("The two layers must be different."));
    if (doc.mesh(aIndex).mesh.VN() <= 0 || doc.mesh(bIndex).mesh.VN() <= 0)
        return fail(QObject::tr("Both layers need vertices."));

    const bool symmetric = params.getBool(QStringLiteral("symmetric"), true);
    const double outlier =
        std::clamp(params.getDouble(QStringLiteral("outlierProportion"), 0.0), 0.0, 0.9);

    doc.beginFilterProgress(QObject::tr("Measure Chamfer Distance (TrueForm)"));
    double forward = 0.0;
    double backward = 0.0;
    try {
        const TfPoints a = worldPoints(doc.mesh(aIndex));
        const TfPoints b = worldPoints(doc.mesh(bIndex));
        auto pa = tf::make_points(a);
        auto pb = tf::make_points(b);

        TfTree treeB(pb, tf::config_tree(4, 4));
        forward = double(tf::chamfer_error(pa, pb | tf::tag(treeB), float(outlier)));
        if (symmetric) {
            TfTree treeA(pa, tf::config_tree(4, 4));
            backward = double(tf::chamfer_error(pb, pa | tf::tag(treeA), float(outlier)));
        }
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm chamfer distance failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
    doc.finishFilterProgress(true, QObject::tr("Measured chamfer distance."));

    const QString aName = doc.mesh(aIndex).name;
    const QString bName = doc.mesh(bIndex).name;
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = false;
    result.infoMessages
        << QObject::tr("'%1' to '%2': %3").arg(aName, bName, QString::number(forward, 'g', 6));
    if (symmetric) {
        result.infoMessages
            << QObject::tr("'%1' to '%2': %3").arg(bName, aName, QString::number(backward, 'g', 6))
            << QObject::tr("Symmetric (max): %1")
                   .arg(QString::number(std::max(forward, backward), 'g', 6));
    }
    return result;
}

// ---------------------------------------------------------------------------
// Smoothing, curvature and normals
// ---------------------------------------------------------------------------

// Connectivity shared by the per-vertex operators. Built once per run; the tags below
// reference these, so they must outlive the call that uses them.
struct VertexConnectivity
{
    explicit VertexConnectivity(const TfMesh &mesh)
        : membership(mesh.polygons())
    {
        link.build(mesh.polygons(), membership);
    }

    tf::face_membership<int> membership;
    tf::vertex_link<int> link;
};

MeshFilterRunResult runSmooth(const QString &filterId, const FilterParams &params, Document &doc)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces: smoothing follows the vertex link."));

    const bool taubin = (filterId == QString::fromLatin1(kFilterTaubin));
    const int iterations = std::max(1, params.getInt(QStringLiteral("iterations"), 10));
    const double lambda = std::clamp(params.getDouble(QStringLiteral("lambda"), 0.5), 0.0, 1.0);
    const double kpb = params.getDouble(QStringLiteral("kpb"), 0.1);
    const bool selectedOnly = params.getBool(QStringLiteral("selectedOnly"), false);

    doc.beginFilterProgress(taubin ? QObject::tr("Smooth Vertices by Taubin (TrueForm)")
                                   : QObject::tr("Smooth Vertices by Laplacian (TrueForm)"));
    try {
        Document::MeshEntry &entry = doc.mesh(index);
        const TfMesh source = tfMeshFromLayer(entry);
        VertexConnectivity conn(source);
        auto tagged = source.points() | tf::tag(conn.link);

        auto smoothed = taubin
            ? tf::taubin_smoothed(tagged, std::size_t(iterations), float(lambda), float(kpb))
            : tf::laplacian_smoothed(tagged, std::size_t(iterations), float(lambda));

        // The source was taken in world space, so the result comes back there too and
        // has to be mapped through the inverse layer matrix before it is stored.
        bool invertible = false;
        const QMatrix4x4 inverse = entry.transform.inverted(&invertible);
        if (!invertible)
            return fail(QObject::tr("The layer matrix is not invertible."));

        const std::vector<std::size_t> live = liveVertexIndices(entry.mesh);
        const auto points = smoothed.points();
        if (std::size_t(points.size()) != live.size())
            return fail(QObject::tr("Smoothing returned an unexpected number of points."));

        int moved = 0;
        for (std::size_t i = 0; i < live.size(); ++i) {
            VCGVertex &v = entry.mesh.vert[live[i]];
            if (selectedOnly && !v.IsS())
                continue;
            const auto &p = points[i];
            const QVector3D local = inverse.map(QVector3D(float(p[0]), float(p[1]), float(p[2])));
            v.P() = vcg::Point3f(local.x(), local.y(), local.z());
            ++moved;
        }

        vcg::tri::UpdateBounding<VCGMesh>::Box(entry.mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry.mesh);
        doc.markMeshGeometryChanged(
            index, QObject::tr("Smoothed '%1'").arg(entry.name));
        doc.finishFilterProgress(true, QObject::tr("Smoothed the layer."));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages
            << QObject::tr("Moved %1 vertex(es) over %2 iteration(s).").arg(moved).arg(iterations);
        return result;
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm smoothing failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

MeshFilterRunResult runCurvature(const FilterParams &params, Document &doc)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const QString measure = params.getEnum(QStringLiteral("measure"));
    const int ring = std::max(1, params.getInt(QStringLiteral("ring"), 2));

    doc.beginFilterProgress(QObject::tr("Compute Curvature (TrueForm)"));
    try {
        Document::MeshEntry &entry = doc.mesh(index);
        const TfMesh source = tfMeshFromLayer(entry);
        const std::vector<std::size_t> live = liveVertexIndices(entry.mesh);

        std::vector<float> values;
        if (measure == QStringLiteral("shape_index")) {
            auto shape = tf::make_shape_index(source.polygons(), std::size_t(ring));
            values.assign(shape.begin(), shape.end());
        } else {
            auto [k0, k1] = tf::make_principal_curvatures(source.polygons(), std::size_t(ring));
            const std::size_t n = std::min(std::size_t(k0.size()), std::size_t(k1.size()));
            values.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                const double a = double(k0[i]);
                const double b = double(k1[i]);
                double value = 0.0;
                if (measure == QStringLiteral("gaussian")) value = a * b;
                else if (measure == QStringLiteral("min")) value = std::min(a, b);
                else if (measure == QStringLiteral("max")) value = std::max(a, b);
                else value = 0.5 * (a + b); // mean
                values.push_back(float(value));
            }
        }

        if (values.size() != live.size())
            return fail(QObject::tr("Curvature returned an unexpected number of values."));
        for (std::size_t i = 0; i < live.size(); ++i)
            entry.mesh.vert[live[i]].Q() = values[i];
        entry.ioMask |= Mask::IOM_VERTQUALITY;

        doc.markMeshGeometryChanged(
            index, QObject::tr("Computed curvature on '%1'").arg(entry.name));
        doc.finishFilterProgress(true, QObject::tr("Computed curvature."));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages << QObject::tr("Wrote %1 per-vertex value(s).").arg(values.size());
        result.visualizationHints.push_back(
            { index, MeshFilterVisualizationAttribute::VertexQuality });
        return result;
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm curvature failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

MeshFilterRunResult runNormals(const FilterParams &params, Document &doc)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const bool perFace = params.getEnum(QStringLiteral("target")) == QStringLiteral("face");

    doc.beginFilterProgress(QObject::tr("Compute Normals (TrueForm)"));
    try {
        Document::MeshEntry &entry = doc.mesh(index);
        const TfMesh source = tfMeshFromLayer(entry);

        // Normals are computed in world space, so they come back rotated by the layer
        // matrix and must be brought back into layer space before being stored.
        bool invertible = false;
        const QMatrix4x4 inverse = entry.transform.inverted(&invertible);
        if (!invertible)
            return fail(QObject::tr("The layer matrix is not invertible."));
        const QMatrix3x3 backToLayer = inverse.normalMatrix();

        const auto toLayer = [&backToLayer](float x, float y, float z) {
            QVector3D t(backToLayer(0, 0) * x + backToLayer(0, 1) * y + backToLayer(0, 2) * z,
                        backToLayer(1, 0) * x + backToLayer(1, 1) * y + backToLayer(1, 2) * z,
                        backToLayer(2, 0) * x + backToLayer(2, 1) * y + backToLayer(2, 2) * z);
            if (t.lengthSquared() > 1e-20f)
                t.normalize();
            return t;
        };

        std::size_t written = 0;
        if (perFace) {
            auto normals = tf::compute_normals(source.polygons());
            const auto vectors = normals.unit_vectors();
            std::size_t k = 0;
            for (VCGFace &f : entry.mesh.face) {
                if (f.IsD())
                    continue;
                if (k >= std::size_t(vectors.size()))
                    break;
                const auto &n = vectors[k++];
                const QVector3D t = toLayer(float(n[0]), float(n[1]), float(n[2]));
                f.N() = vcg::Point3f(t.x(), t.y(), t.z());
                ++written;
            }
            entry.ioMask |= Mask::IOM_FACENORMAL;
        } else {
            VertexConnectivity conn(source);
            auto normals = tf::compute_point_normals(source.polygons() | tf::tag(conn.membership));
            const auto vectors = normals.unit_vectors();
            const std::vector<std::size_t> live = liveVertexIndices(entry.mesh);
            if (std::size_t(vectors.size()) != live.size())
                return fail(QObject::tr("Normal computation returned an unexpected count."));
            for (std::size_t i = 0; i < live.size(); ++i) {
                const auto &n = vectors[i];
                const QVector3D t = toLayer(float(n[0]), float(n[1]), float(n[2]));
                entry.mesh.vert[live[i]].N() = vcg::Point3f(t.x(), t.y(), t.z());
                ++written;
            }
            entry.ioMask |= Mask::IOM_VERTNORMAL;
        }

        doc.markMeshGeometryChanged(
            index, QObject::tr("Computed normals on '%1'").arg(entry.name));
        doc.finishFilterProgress(true, QObject::tr("Computed normals."));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages << (perFace ? QObject::tr("Wrote %1 face normal(s).").arg(written)
                                        : QObject::tr("Wrote %1 vertex normal(s).").arg(written));
        return result;
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm normal computation failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// ---------------------------------------------------------------------------
// Remeshing
// ---------------------------------------------------------------------------

// Replace a layer's geometry with a TrueForm result, keeping the layer and its matrix.
// The result arrives in world space, so it is mapped back through the inverse matrix.
MeshFilterRunResult replaceLayerGeometry(
    Document &doc, int index, const TfMesh &result, const QString &context, QStringList info)
{
    Document::MeshEntry &entry = doc.mesh(index);
    bool invertible = false;
    const QMatrix4x4 inverse = entry.transform.inverted(&invertible);
    if (!invertible) {
        const QString message = QObject::tr("The layer matrix is not invertible.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    VCGMesh output;
    const auto points = result.points();
    const std::size_t pointCount = std::size_t(points.size());
    if (pointCount > 0) {
        vcg::tri::Allocator<VCGMesh>::AddVertices(output, int(pointCount));
        tf::parallel_for_each(tf::enumerate(points), [&output, &inverse](auto pair) {
            auto &&[vi, p] = pair;
            const QVector3D local =
                inverse.map(QVector3D(float(p[0]), float(p[1]), float(p[2])));
            output.vert[std::size_t(vi)].P() =
                vcg::Point3f(local.x(), local.y(), local.z());
        }, tf::checked);
        for (const auto &face : result.faces()) {
            const std::size_t n = std::size_t(face.size());
            for (std::size_t k = 2; k < n; ++k) {
                const int a = int(face[0]), b = int(face[k - 1]), c = int(face[k]);
                if (a < 0 || b < 0 || c < 0)
                    continue;
                if (std::size_t(a) >= pointCount || std::size_t(b) >= pointCount
                    || std::size_t(c) >= pointCount)
                    continue;
                if (a == b || b == c || a == c)
                    continue;
                vcg::tri::Allocator<VCGMesh>::AddFace(output, a, b, c);
            }
        }
    }

    if (output.FN() <= 0) {
        const QString message = QObject::tr("The operation produced no faces.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    const int beforeV = entry.mesh.VN();
    const int beforeF = entry.mesh.FN();

    entry.mesh.Clear();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopy(entry.mesh, output);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(entry.mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(entry.mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry.mesh);
    entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;

    doc.markMeshGeometryChanged(index, context);
    doc.finishFilterProgress(true, context);

    MeshFilterRunResult r;
    r.success = true;
    r.documentModified = true;
    info << QObject::tr("%1 vertices, %2 faces -> %3 vertices, %4 faces.")
                .arg(beforeV).arg(beforeF).arg(entry.mesh.VN()).arg(entry.mesh.FN());
    r.infoMessages = info;
    return r;
}

MeshFilterRunResult runRemesh(const QString &filterId, const FilterParams &params, Document &doc)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const bool preserveBoundary = params.getBool(QStringLiteral("preserveBoundary"), true);
    const double featureAngle = params.getDouble(QStringLiteral("featureAngle"), -1.0);
    // A negative feature angle disables feature detection, which is TrueForm's own
    // convention; exposing it as "0 means off" would be a needless second convention.
    const auto featureRad = tf::rad<float>(
        featureAngle > 0.0 ? float(featureAngle * M_PI / 180.0) : -1.0f);

    doc.beginFilterProgress(QObject::tr("Remeshing (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));

        if (filterId == QString::fromLatin1(kFilterIsotropic)) {
            const double target = params.getDouble(QStringLiteral("targetLength"), 0.0);
            if (!std::isfinite(target) || target <= 0.0)
                return fail(QObject::tr("The target edge length must be larger than zero."));
            tf::isotropic_remesh_config<float> config{ float(target) };
            config.iterations = std::max(1, params.getInt(QStringLiteral("iterations"), 3));
            config.relaxation_iters =
                std::max(0, params.getInt(QStringLiteral("relaxationIterations"), 3));
            config.preserve_boundary = preserveBoundary;
            config.feature_angle = featureRad;
            auto [mesh, he] = tf::isotropic_remeshed(source.polygons(), config);
            (void) he;
            return replaceLayerGeometry(
                doc, index, mesh,
                QObject::tr("Remeshed '%1' isotropically").arg(doc.mesh(index).name),
                { QObject::tr("Target edge length: %1").arg(QString::number(target, 'g', 6)) });
        }

        if (filterId == QString::fromLatin1(kFilterSimplify)) {
            tf::simplify_config<float> config;
            // The parameter is an absolute distance; TrueForm wants it as a fraction of
            // the bounding-box diagonal (simplify_config::error_rel).
            const double diagonal = double(doc.mesh(index).mesh.bbox.Diag());
            const double bound = params.getDouble(QStringLiteral("errorBound"), 0.0);
            config.error_rel = float(std::max(
                1e-9, diagonal > 0.0 ? bound / diagonal : 0.002));
            config.iterations = std::max(1, params.getInt(QStringLiteral("iterations"), 1));
            config.preserve_boundary = preserveBoundary;
            config.feature_angle = featureRad;
            auto [mesh, he] = tf::simplified(source.polygons(), config);
            (void) he;
            return replaceLayerGeometry(
                doc, index, mesh,
                QObject::tr("Simplified '%1'").arg(doc.mesh(index).name),
                { QObject::tr("Error bound: %1 (%2 of the bounding-box diagonal)")
                      .arg(QString::number(bound, 'g', 6),
                           QString::number(100.0 * config.error_rel, 'g', 4) + QStringLiteral("%")) });
        }

        // Decimation to a target proportion of the original face count.
        const double proportion =
            std::clamp(params.getDouble(QStringLiteral("targetProportion"), 0.5), 0.001, 0.999);
        tf::decimate_config<float> config;
        config.preserve_boundary = preserveBoundary;
        config.feature_angle = featureRad;
        auto [mesh, he] = tf::decimated(source.polygons(), float(proportion), config);
        (void) he;
        return replaceLayerGeometry(
            doc, index, mesh,
            QObject::tr("Decimated '%1'").arg(doc.mesh(index).name),
            { QObject::tr("Target proportion: %1").arg(proportion) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm remeshing failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// ---------------------------------------------------------------------------
// Orientation and edge selection
// ---------------------------------------------------------------------------

MeshFilterRunResult runOrient(const QString &filterId, const FilterParams &params, Document &doc)
{
    (void) params;
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const bool outward = (filterId == QString::fromLatin1(kFilterOrientOutward));

    doc.beginFilterProgress(outward ? QObject::tr("Orient Faces Outward (TrueForm)")
                                    : QObject::tr("Orient Faces Consistently (TrueForm)"));
    int flipped = 0;
    bool orientable = false;
    try {
        Document::MeshEntry &entry = doc.mesh(index);
        TfMesh source = tfMeshFromLayer(entry);

        {
            auto polys = source.polygons();
            orientable = outward ? tf::ensure_positive_orientation(polys)
                                 : tf::orient_faces_consistently(polys);
        }
        // Only the winding changed, so the faces are rewritten in place; positions and
        // the vertex numbering are untouched.
        const std::vector<std::size_t> live = liveVertexIndices(entry.mesh);
        std::size_t faceIndex = 0;
        const auto faces = source.faces();
        for (VCGFace &f : entry.mesh.face) {
            if (f.IsD())
                continue;
            if (faceIndex >= std::size_t(faces.size()))
                break;
            const auto &face = faces[faceIndex++];
            bool usable = true;
            std::size_t corner[3];
            for (int k = 0; k < 3; ++k) {
                const std::size_t id = std::size_t(face[std::size_t(k)]);
                if (id >= live.size()) {
                    usable = false;
                    break;
                }
                corner[k] = live[id];
            }
            if (!usable)
                continue;
            const bool changed = (f.cV(0) != &entry.mesh.vert[corner[0]])
                || (f.cV(1) != &entry.mesh.vert[corner[1]])
                || (f.cV(2) != &entry.mesh.vert[corner[2]]);
            if (!changed)
                continue;
            for (int k = 0; k < 3; ++k)
                f.V(k) = &entry.mesh.vert[corner[std::size_t(k)]];
            ++flipped;
        }

        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry.mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm orientation failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    doc.markMeshGeometryChanged(
        index, QObject::tr("Oriented faces of '%1'").arg(doc.mesh(index).name));
    doc.finishFilterProgress(true, QObject::tr("Oriented faces."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages << QObject::tr("Reoriented %1 face(s).").arg(flipped);
    if (!orientable) {
        result.infoMessages << QObject::tr(
            "The mesh is non-orientable; components without a consistent winding "
            "were left unchanged.");
    }
    return result;
}

// Mark the edges reported by TrueForm on the VCG mesh's per-face edge selection, which is
// where MeshLab keeps edge selections. The reported pairs are point indices, so they are
// mapped back through the live-vertex table and matched against each face's three edges.
int selectFaceEdges(
    VCGMesh &mesh,
    const std::vector<std::size_t> &live,
    const std::vector<std::pair<int, int>> &edges,
    bool clearFirst)
{
    using EdgeKey = std::array<std::size_t, 2>;
    const auto canonical = [](std::size_t a, std::size_t b) {
        return a < b ? EdgeKey{a, b} : EdgeKey{b, a};
    };

    tf::hash_set<EdgeKey, tf::array_hash<std::size_t, 2>> wanted;
    wanted.reserve(edges.size());
    for (const auto &e : edges) {
        if (e.first < 0 || e.second < 0)
            continue;
        if (std::size_t(e.first) >= live.size() || std::size_t(e.second) >= live.size())
            continue;
        wanted.insert(canonical(live[std::size_t(e.first)], live[std::size_t(e.second)]));
    }

    const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    if (!base)
        return 0;

    int marked = 0;
    tf::blocked_reduce(
        tf::make_sequence_range(mesh.face.size()), marked, int(0),
        [&](auto &&block, int &local) {
            for (std::size_t fi : block) {
                VCGFace &f = mesh.face[fi];
                if (f.IsD())
                    continue;
                for (int k = 0; k < 3; ++k) {
                    if (clearFirst)
                        f.ClearFaceEdgeS(k);
                    const auto key = canonical(std::size_t(f.cV(k) - base),
                                               std::size_t(f.cV((k + 1) % 3) - base));
                    if (wanted.count(key)) {
                        f.SetFaceEdgeS(k);
                        ++local;
                    }
                }
            }
        },
        [](int local, int &total) { total += local; }, tf::checked);
    return marked;
}

MeshFilterRunResult runSelectEdges(const QString &filterId, const FilterParams &params, Document &doc)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const bool crease = (filterId == QString::fromLatin1(kFilterSelectCrease));
    const double angle = params.getDouble(QStringLiteral("angle"), 60.0);
    const bool clearFirst = params.getBool(QStringLiteral("replaceSelection"), true);

    doc.beginFilterProgress(crease ? QObject::tr("Select Crease Edges (TrueForm)")
                                   : QObject::tr("Select Non-Manifold Edges (TrueForm)"));
    try {
        Document::MeshEntry &entry = doc.mesh(index);
        const TfMesh source = tfMeshFromLayer(entry);

        std::vector<std::pair<int, int>> pairs;
        if (crease) {
            auto sharp = tf::make_sharp_edges(source.polygons(), tf::deg<float>(float(angle)));
            for (const auto &e : sharp)
                pairs.emplace_back(int(e[0]), int(e[1]));
        } else {
            auto nm = tf::make_non_manifold_edges(source.polygons());
            for (const auto &e : nm)
                pairs.emplace_back(int(e[0]), int(e[1]));
        }

        selectFaceEdges(entry.mesh, liveVertexIndices(entry.mesh), pairs, clearFirst);
        entry.ioMask |= Mask::IOM_FACEFLAGS;
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm edge selection failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    doc.markMeshSelectionChanged(
        index, QObject::tr("Selected edges on '%1'").arg(doc.mesh(index).name));
    doc.finishFilterProgress(true, QObject::tr("Updated edge selection."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    // The count comes from the framework, which reads the face-edge bits back off the
    // layer -- this filter declares "FES", so they are reported rather than the whole-face
    // selection it does not touch.
    return result;
}

// ---------------------------------------------------------------------------
// Arrangement repair, isobands and cleaning
// ---------------------------------------------------------------------------

// Resolve a mesh against itself. Every self-crossing becomes a real edge and every
// crossed face is split along it, so the result has a well-defined inside and outside
// where the original had neither.
MeshFilterRunResult runRepairSelfIntersections(const FilterParams &params, Document &doc)
{
    const int index = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    doc.beginFilterProgress(QObject::tr("Repair Self-Intersections (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        auto [mesh, faceLabels] = tf::make_polygon_arrangements(source.polygons());

        return addResultLayer(
            doc, mesh, QObject::tr("Resolved"),
            QObject::tr("The arrangement is empty."),
            { QObject::tr("Resolved self-intersections of '%1'.").arg(doc.mesh(index).name) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm arrangement failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Split the surface along contours of the vertex scalar, so each band between successive
// contour values becomes its own set of faces.
MeshFilterRunResult runCutAlongIsocontour(const FilterParams &params, Document &doc)
{
    const int index = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No layer selected."));
    const VCGMesh &mesh = doc.mesh(index).mesh;
    if (mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const int count = std::max(1, params.getInt(QStringLiteral("contourCount"), 5));

    std::vector<float> scalars;
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        const float q = v.cQ();
        scalars.push_back(q);
        if (std::isfinite(q)) {
            lo = std::min(lo, q);
            hi = std::max(hi, q);
        }
    }
    if (scalars.empty())
        return fail(QObject::tr("The layer has no live vertices."));
    if (!(hi > lo)) {
        return fail(QObject::tr(
            "The scalar field is constant, so there is nothing to cut along. Compute a "
            "per-vertex scalar first."));
    }

    std::vector<float> cutValues;
    for (int i = 0; i < count; ++i) {
        const double t = (double(i) + 1.0) / (double(count) + 1.0);
        cutValues.push_back(float(double(lo) + t * (double(hi) - double(lo))));
    }
    doc.beginFilterProgress(QObject::tr("Cut Along Scalar Isocontour (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        auto [mesh, labels, faceLabels] = tf::embedded_isocurves(
            source.polygons(),
            tf::make_range(scalars.data(), scalars.size()),
            tf::make_range(cutValues.data(), cutValues.size()));
        return addResultLayer(
            doc, mesh, QObject::tr("Isobands"),
            QObject::tr("Cutting produced no faces."),
            { QObject::tr("From '%1'.").arg(doc.mesh(index).name),
              QObject::tr("%1 contour(s), %2 band(s), between %3 and %4.")
                  .arg(count).arg(count + 1)
                  .arg(QString::number(double(lo), 'g', 6))
                  .arg(QString::number(double(hi), 'g', 6)) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm isobands failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Weld coincident vertices and drop the degeneracies that welding exposes.
MeshFilterRunResult runCleanMesh(const FilterParams &params, Document &doc)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    const double tolerance = params.getDouble(QStringLiteral("tolerance"), 0.0);

    doc.beginFilterProgress(QObject::tr("Remove Duplicate Vertices (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        const int beforeV = doc.mesh(index).mesh.VN();
        const int beforeF = doc.mesh(index).mesh.FN();

        auto cleaned = (tolerance > 0.0)
            ? tf::cleaned(source.polygons(), float(tolerance))
            : tf::cleaned(source.polygons());

        return replaceLayerGeometry(
            doc, index, cleaned,
            QObject::tr("Cleaned '%1'").arg(doc.mesh(index).name),
            { tolerance > 0.0
                  ? QObject::tr("Welded vertices closer than %1.")
                        .arg(QString::number(tolerance, 'g', 6))
                  : QObject::tr("Welded exactly coincident vertices."),
              QObject::tr("Was %1 vertices, %2 faces.").arg(beforeV).arg(beforeF) });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm cleaning failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

// Improve triangle quality without changing the vertex count: interleaved rounds of edge
// flips and tangential relaxation. Connectivity and positions both change, but no vertex
// is added or removed, so it refines an existing tessellation rather than rebuilding it.
MeshFilterRunResult runImproveTriangulation(const FilterParams &params, Document &doc)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    if (doc.mesh(index).mesh.FN() <= 0)
        return fail(QObject::tr("The layer needs faces."));

    tf::improve_config<float> config;
    config.iterations = std::max(1, params.getInt(QStringLiteral("iterations"), 3));
    config.relaxation_iters = std::max(0, params.getInt(QStringLiteral("relaxationIterations"), 3));
    config.lambda = float(std::clamp(params.getDouble(QStringLiteral("lambda"), 0.5), 0.0, 1.0));
    config.check_normals = params.getBool(QStringLiteral("checkNormals"), true);
    config.flip = (params.getEnum(QStringLiteral("objective")) == QStringLiteral("min_angle"))
        ? tf::flip_objective::min_angle
        : tf::flip_objective::valence;
    // Zero lets relaxation move vertices freely along the surface; a positive bound caps
    // how far the surface may drift from where it started.
    config.max_deviation = float(std::max(0.0, params.getDouble(QStringLiteral("maxDeviation"), 0.0)));

    doc.beginFilterProgress(QObject::tr("Remesh by Edge Flipping (TrueForm)"));
    try {
        const TfMesh source = tfMeshFromLayer(doc.mesh(index));
        // No convenience wrapper for this one: extract the half-edge structure, operate on
        // it, and rebuild, which is what isotropic_remeshed() does internally.
        auto [he, points] = tf::remesh::extract_he_points(source.polygons());
        tf::improve_triangulation(he, tf::make_points(points), config);
        auto improved = tf::remesh::make_mesh(he, std::move(points));

        return replaceLayerGeometry(
            doc, index, improved,
            QObject::tr("Improved the triangulation of '%1'").arg(doc.mesh(index).name),
            { config.flip == tf::flip_objective::min_angle
                  ? QObject::tr("Flips chosen to maximise the smallest angle.")
                  : QObject::tr("Flips chosen to even out vertex valence.") });
    } catch (const std::exception &e) {
        const QString message = QObject::tr("TrueForm triangulation improvement failed: %1")
                                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

} // namespace

QString TrueFormFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.trueform");
}

QString TrueFormFilterPlugin::name() const
{
    return QObject::tr("TrueForm Filters");
}

MeshFilterRunResult TrueFormFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kFilterAlignObb))
        return runAlignObb(params, doc);
    if (filterId == QString::fromLatin1(kFilterAlignIcp))
        return runAlignIcp(params, doc);
    if (filterId == QString::fromLatin1(kFilterAlignCorresponding))
        return runAlignCorresponding(params, doc);
    if (filterId == QString::fromLatin1(kFilterUnion)
        || filterId == QString::fromLatin1(kFilterIntersection)
        || filterId == QString::fromLatin1(kFilterDifference))
        return runBoolean(filterId, params, doc);
    if (filterId == QString::fromLatin1(kFilterSymmetricDifference))
        return runSymmetricDifference(params, doc);
    if (filterId == QString::fromLatin1(kFilterCsg))
        return runCsgExpression(params, doc);
    if (filterId == QString::fromLatin1(kFilterSolidDomains))
        return runSolidDomains(params, doc);
    if (filterId == QString::fromLatin1(kFilterSplitComponents))
        return runSplitComponents(params, doc);
    if (filterId == QString::fromLatin1(kFilterOuterShell))
        return runOuterShell(params, doc);
    if (filterId == QString::fromLatin1(kFilterSelfIntersectionCurves))
        return runSelfIntersectionCurves(params, doc);
    if (filterId == QString::fromLatin1(kFilterIntersectionCurves))
        return runIntersectionCurves(params, doc);
    if (filterId == QString::fromLatin1(kFilterIsocurves))
        return runIsocurves(params, doc);
    if (filterId == QString::fromLatin1(kFilterTube))
        return runTubeFromPolyline(params, doc);
    if (filterId == QString::fromLatin1(kFilterSignedDistance))
        return runSignedDistance(params, doc);
    if (filterId == QString::fromLatin1(kFilterSelectInside))
        return runSelectInsideMesh(params, doc);
    if (filterId == QString::fromLatin1(kFilterChamfer))
        return runChamferDistance(params, doc);
    if (filterId == QString::fromLatin1(kFilterLaplacian)
        || filterId == QString::fromLatin1(kFilterTaubin))
        return runSmooth(filterId, params, doc);
    if (filterId == QString::fromLatin1(kFilterCurvature))
        return runCurvature(params, doc);
    if (filterId == QString::fromLatin1(kFilterNormals))
        return runNormals(params, doc);
    if (filterId == QString::fromLatin1(kFilterIsotropic)
        || filterId == QString::fromLatin1(kFilterSimplify)
        || filterId == QString::fromLatin1(kFilterDecimate))
        return runRemesh(filterId, params, doc);
    if (filterId == QString::fromLatin1(kFilterOrientCoherent)
        || filterId == QString::fromLatin1(kFilterOrientOutward))
        return runOrient(filterId, params, doc);
    if (filterId == QString::fromLatin1(kFilterSelectCrease)
        || filterId == QString::fromLatin1(kFilterSelectNonManifold))
        return runSelectEdges(filterId, params, doc);
    if (filterId == QString::fromLatin1(kFilterRepairSelfIntersections))
        return runRepairSelfIntersections(params, doc);
    if (filterId == QString::fromLatin1(kFilterCutIsocontour))
        return runCutAlongIsocontour(params, doc);
    if (filterId == QString::fromLatin1(kFilterClean))
        return runCleanMesh(params, doc);
    if (filterId == QString::fromLatin1(kFilterImprove))
        return runImproveTriangulation(params, doc);
    return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
}

void registerTrueFormFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<TrueFormFilterPlugin>());
}
