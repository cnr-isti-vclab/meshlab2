#include "unsharpfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/closest.h>
#include <vcg/complex/algorithms/crease_cut.h>
#include <vcg/complex/algorithms/harmonic.h>
#include <vcg/complex/algorithms/smooth.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/color.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/space/index/grid_static_ptr.h>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace {
constexpr QLatin1StringView kFilterCreaseCut("cut_along_crease_edges");
constexpr QLatin1StringView kFilterLaplacian("smooth_vertices_by_laplacian_vcglib");
constexpr QLatin1StringView kFilterHcLaplacian("smooth_vertices_by_hc_laplacian");
constexpr QLatin1StringView kFilterSdLaplacian("smooth_vertices_by_scale_dependent_laplacian");
constexpr QLatin1StringView kFilterTwoStep("smooth_vertices_by_two_step_normal_fitting");
constexpr QLatin1StringView kFilterTaubin("smooth_vertices_by_taubin_vcglib");
constexpr QLatin1StringView kFilterDepth("smooth_vertices_along_one_direction");
constexpr QLatin1StringView kFilterDirectional("project_vertices_onto_line_of_sight");
constexpr QLatin1StringView kFilterVertexQualitySmooth("smooth_vertex_scalar");
constexpr QLatin1StringView kFilterFaceNormalSmooth("smooth_face_normals");
constexpr QLatin1StringView kFilterUnsharpNormal("sharpen_face_normals_by_unsharp_mask");
constexpr QLatin1StringView kFilterUnsharpGeometry("sharpen_vertices_by_unsharp_mask");
constexpr QLatin1StringView kFilterUnsharpQuality("sharpen_vertex_scalar_by_unsharp_mask");
constexpr QLatin1StringView kFilterUnsharpColor("sharpen_vertex_color_by_unsharp_mask");
constexpr QLatin1StringView kFilterRecomputeVertexNormal("compute_vertex_normals");
constexpr QLatin1StringView kFilterRecomputeFaceNormal("compute_face_normals");
constexpr QLatin1StringView kFilterRecomputePolygonFaceNormal("compute_polygon_face_normals");
constexpr QLatin1StringView kFilterNormalizeFaceNormal("normalize_face_normals");
constexpr QLatin1StringView kFilterNormalizeVertexNormal("normalize_vertex_normals");
constexpr QLatin1StringView kFilterLinearMorph("displace_vertices_toward_target_mesh");
constexpr QLatin1StringView kFilterScalarHarmonic("compute_harmonic_scalar_field");

using Mask = vcg::tri::io::Mask;
using Point = vcg::Point3f;
using Scalar = VCGMesh::ScalarType;

enum class WeightMode {
    SimpleAverage,
    ByArea,
    ByAngle,
    NelsonMax
};

struct CurrentMeshRef {
    int index = -1;
    Document::MeshEntry *entry = nullptr;
};

std::optional<CurrentMeshRef> currentMesh(Document &doc, QString &errorMessage)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount()) {
        errorMessage = QObject::tr("No current mesh selected.");
        return std::nullopt;
    }
    return CurrentMeshRef{ meshIndex, &doc.mesh(meshIndex) };
}

QString meshName(const Document::MeshEntry &entry, int meshIndex)
{
    const QString trimmed = entry.name.trimmed();
    if (!trimmed.isEmpty())
        return trimmed;
    return QObject::tr("Mesh %1").arg(meshIndex + 1);
}

Point toPoint(const QVector3D &v)
{
    return Point(v.x(), v.y(), v.z());
}

void updateBBoxAndNormals(VCGMesh &mesh)
{
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (mesh.FN() > 0) {
        vcg::tri::UpdateNormal<VCGMesh>::PerFace(mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalized(mesh);
    }
}

void prepareFaceFaceNormalsSmoothing(VCGMesh &mesh)
{
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
    vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
}

size_t selectedVertexCountFromFaces(VCGMesh &mesh)
{
    return vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);
}

QString firstPointAttributeName(const VCGMesh &mesh)
{
    if (mesh.vert_attr.empty())
        return {};
    return QString::fromStdString(mesh.vert_attr.begin()->_name);
}

WeightMode weightModeFromParam(const FilterParams &params)
{
    const QString mode = params.getEnum(QStringLiteral("weightMode"));
    if (mode == QStringLiteral("by_area"))
        return WeightMode::ByArea;
    if (mode == QStringLiteral("by_angle"))
        return WeightMode::ByAngle;
    if (mode == QStringLiteral("nelson_max"))
        return WeightMode::NelsonMax;
    return WeightMode::SimpleAverage;
}

MeshFilterRunResult successResult(const QStringList &info = {})
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    return result;
}

MeshFilterRunResult failResult(const QString &message)
{
    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = message;
    return result;
}

} // namespace

QString UnsharpFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.unsharp");
}

QString UnsharpFilterPlugin::name() const
{
    return QObject::tr("Smoothing Filters");
}

std::vector<MeshFilterDescriptor> UnsharpFilterPlugin::filters(const Document &doc) const
{
    auto descriptors = MeshFilterPlugin::filters(doc);
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return descriptors;

    const Document::MeshEntry &entry = doc.mesh(meshIndex);
    const vcg::Box3f &bbox = entry.mesh.bbox;
    const QVector3D bboxMin(bbox.min.X(), bbox.min.Y(), bbox.min.Z());
    const QVector3D bboxMax(bbox.max.X(), bbox.max.Y(), bbox.max.Z());
    const QString firstAttr = firstPointAttributeName(entry.mesh);

    for (MeshFilterDescriptor &descriptor : descriptors) {
        if (descriptor.id == QString::fromLatin1(kFilterDirectional)) {
            for (MeshFilterParameterDescriptor &param : descriptor.parameters) {
                if (param.id == QStringLiteral("attr_name") && !firstAttr.isEmpty())
                    param.defaultValue = firstAttr;
            }
        }
        if (descriptor.id == QString::fromLatin1(kFilterScalarHarmonic)) {
            for (MeshFilterParameterDescriptor &param : descriptor.parameters) {
                if (param.id == QStringLiteral("point1"))
                    param.defaultValue = QVariant::fromValue(bboxMin);
                else if (param.id == QStringLiteral("point2"))
                    param.defaultValue = QVariant::fromValue(bboxMax);
            }
        }
    }

    return descriptors;
}

MeshFilterRunResult UnsharpFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    QString errorMessage;
    auto current = currentMesh(doc, errorMessage);
    if (!current)
        return failResult(errorMessage);

    const int meshIndex = current->index;
    Document::MeshEntry &entry = *current->entry;
    VCGMesh &mesh = entry.mesh;
    vcg::CallBackPos *cb = doc.progressCallback();

    auto markGeometry = [&](const QString &action) {
        doc.markMeshGeometryChanged(meshIndex, action);
    };
    auto markMaterial = [&](const QString &action) {
        doc.markMeshMaterialChanged(meshIndex, action);
    };

    if (filterId == QString::fromLatin1(kFilterCreaseCut)) {
        if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh, false) > 0 ||
            vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(mesh, false) > 0) {
            return failResult(QObject::tr("Mesh has some non-manifold faces or vertices; this filter requires manifoldness."));
        }

        const float angleDeg = float(params.getDouble(QStringLiteral("angleDeg")));
        vcg::tri::CreaseCut(mesh, vcg::math::ToRad(angleDeg));
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Cut mesh '%1' along crease edges.").arg(meshName(entry, meshIndex)));
        return successResult({ QObject::tr("Cut mesh along crease edges using a %1 degree threshold.").arg(angleDeg, 0, 'f', 3) });
    }

    if (filterId == QString::fromLatin1(kFilterFaceNormalSmooth)) {
        VCGMeshFFAdjScope _ffAdj(mesh);
        prepareFaceFaceNormalsSmoothing(mesh);
        vcg::tri::Smooth<VCGMesh>::FaceNormalLaplacianFF(mesh);
        entry.ioMask |= Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Smoothed face normals of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterVertexQualitySmooth)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        vcg::tri::Smooth<VCGMesh>::VertexQualityLaplacian(mesh);
        entry.ioMask |= Mask::IOM_VERTQUALITY;
        markGeometry(QObject::tr("Smoothed vertex quality of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterLaplacian)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        const int steps = params.getInt(QStringLiteral("stepSmoothNum"));
        const bool selected = params.getBool(QStringLiteral("Selected"));
        const size_t selectedCount = selected ? selectedVertexCountFromFaces(mesh) : 0;

        const bool boundarySmooth = params.getBool(QStringLiteral("Boundary"));
        const bool cotangentWeight = params.getBool(QStringLiteral("cotangentWeight"));
        if (!boundarySmooth)
            vcg::tri::UpdateFlags<VCGMesh>::FaceClearB(mesh);

        vcg::tri::Smooth<VCGMesh>::VertexCoordLaplacian(mesh, steps, selected, cotangentWeight, cb);
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied Laplacian smoothing to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult({ QObject::tr("Smoothed %1 vertices.").arg(selected ? int(selectedCount) : mesh.vn) });
    }

    if (filterId == QString::fromLatin1(kFilterDepth)) {
        const int steps = params.getInt(QStringLiteral("stepSmoothNum"));
        const bool selected = params.getBool(QStringLiteral("Selected"));
        const size_t selectedCount = selected ? selectedVertexCountFromFaces(mesh) : 0;
        const Scalar delta = Scalar(params.getDouble(QStringLiteral("delta")));
        const Point viewpoint = toPoint(params.getPoint3f(QStringLiteral("viewPoint")));
        vcg::tri::Smooth<VCGMesh>::VertexCoordViewDepth(mesh, viewpoint, delta, steps, selected, true);
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied depth smoothing to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult({ QObject::tr("Depth-smoothed %1 vertices.").arg(selected ? int(selectedCount) : mesh.vn) });
    }

    if (filterId == QString::fromLatin1(kFilterDirectional)) {
        const std::string attribName = params.getString(QStringLiteral("attr_name")).trimmed().toStdString();
        if (attribName.empty()) {
            return failResult(QObject::tr("A per-vertex custom point attribute name is required."));
        }
        if (!vcg::tri::HasPerVertexAttribute(mesh, attribName)) {
            return failResult(
                QObject::tr("Current mesh has no per-vertex custom point attribute called '%1'.")
                    .arg(QString::fromStdString(attribName)));
        }
        auto handle = vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Point>(mesh, attribName);
        if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle(mesh, handle)) {
            return failResult(
                QObject::tr("Current mesh has a custom attribute called '%1', but it is not a point attribute.")
                    .arg(QString::fromStdString(attribName)));
        }

        const Point viewpoint = toPoint(params.getPoint3f(QStringLiteral("viewPoint")));
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            Point dir = handle[vi] - viewpoint;
            dir.Normalize();
            const Scalar projection = dir * (vi->cP() - handle[vi]);
            vi->P() = handle[vi] + dir * projection;
        }
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied directional geometry preservation to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterSdLaplacian)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        const int steps = params.getInt(QStringLiteral("stepSmoothNum"));
        const size_t selectedCount = selectedVertexCountFromFaces(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceClearB(mesh);
        const Scalar delta = Scalar(params.getDouble(QStringLiteral("delta")));
        vcg::tri::Smooth<VCGMesh>::VertexCoordScaleDependentLaplacian_Fujiwara(mesh, steps, delta);
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied scale-dependent Laplacian smoothing to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult({ QObject::tr("Smoothed %1 vertices.").arg(selectedCount > 0 ? int(selectedCount) : mesh.vn) });
    }

    if (filterId == QString::fromLatin1(kFilterHcLaplacian)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        const size_t selectedCount = selectedVertexCountFromFaces(mesh);
        vcg::tri::Smooth<VCGMesh>::VertexCoordLaplacianHC(mesh, 1, selectedCount > 0);
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied HC Laplacian smoothing to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterTwoStep)) {
        vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
        vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);
        const int steps = params.getInt(QStringLiteral("stepSmoothNum"));
        Scalar sigma = std::cos(vcg::math::ToRad(float(params.getDouble(QStringLiteral("normalThr")))));
        if (sigma < 0)
            sigma = 0;
        const int normalSteps = params.getInt(QStringLiteral("stepNormalNum"));
        const int fitSteps = params.getInt(QStringLiteral("stepFitNum"));
        const bool selected = params.getBool(QStringLiteral("Selected"));
        for (int i = 0; i < steps; ++i) {
            vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
            vcg::tri::Smooth<VCGMesh>::VertexCoordPasoDoble(mesh, normalSteps, sigma, fitSteps, selected);
        }
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied two-step smoothing to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterTaubin)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        const int steps = params.getInt(QStringLiteral("stepSmoothNum"));
        const Scalar lambda = Scalar(params.getDouble(QStringLiteral("lambda")));
        const Scalar mu = Scalar(params.getDouble(QStringLiteral("mu")));
        const size_t selectedCount = selectedVertexCountFromFaces(mesh);
        vcg::tri::Smooth<VCGMesh>::VertexCoordTaubin(mesh, steps, float(lambda), float(mu), selectedCount > 0, cb);
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied Taubin smoothing to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult({ QObject::tr("Smoothed %1 vertices.").arg(selectedCount > 0 ? int(selectedCount) : mesh.vn) });
    }

    if (filterId == QString::fromLatin1(kFilterRecomputeFaceNormal)) {
        vcg::tri::UpdateNormal<VCGMesh>::PerFace(mesh);
        entry.ioMask |= Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Recomputed face normals of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterRecomputePolygonFaceNormal)) {
        vcg::tri::UpdateNormal<VCGMesh>::PerBitPolygonFaceNormalized(mesh);
        entry.ioMask |= Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Recomputed polygon face normals of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterRecomputeVertexNormal)) {
        switch (weightModeFromParam(params)) {
        case WeightMode::SimpleAverage:
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerFace(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
            break;
        case WeightMode::ByArea:
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerFaceByArea(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
            break;
        case WeightMode::ByAngle:
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexAngleWeighted(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
            break;
        case WeightMode::NelsonMax:
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNelsonMaxWeighted(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
            break;
        }
        entry.ioMask |= Mask::IOM_VERTNORMAL;
        markGeometry(QObject::tr("Recomputed vertex normals of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterNormalizeFaceNormal)) {
        vcg::tri::UpdateNormal<VCGMesh>::NormalizePerFace(mesh);
        entry.ioMask |= Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Normalized face normals of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterNormalizeVertexNormal)) {
        vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL;
        markGeometry(QObject::tr("Normalized vertex normals of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterUnsharpNormal)) {
        VCGMeshFFAdjScope _ffAdj(mesh);
        prepareFaceFaceNormalsSmoothing(mesh);
        const Scalar alpha = Scalar(params.getDouble(QStringLiteral("weight")));
        const Scalar alphaOrig = Scalar(params.getDouble(QStringLiteral("weightOrig")));
        const int smoothIter = params.getInt(QStringLiteral("iterations"));
        if (params.getBool(QStringLiteral("recalc")))
            vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
        std::vector<Point> normalOrig(size_t(mesh.fn));
        for (int i = 0; i < mesh.fn; ++i)
            normalOrig[size_t(i)] = mesh.face[size_t(i)].cN();
        for (int i = 0; i < smoothIter; ++i)
            vcg::tri::Smooth<VCGMesh>::FaceNormalLaplacianFF(mesh);
        for (int i = 0; i < mesh.fn; ++i) {
            mesh.face[size_t(i)].N() =
                normalOrig[size_t(i)] * alphaOrig + (normalOrig[size_t(i)] - mesh.face[size_t(i)].N()) * alpha;
        }
        entry.ioMask |= Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied unsharp masking to face normals of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterUnsharpGeometry)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        const Scalar alpha = Scalar(params.getDouble(QStringLiteral("weight")));
        const Scalar alphaOrig = Scalar(params.getDouble(QStringLiteral("weightOrig")));
        const int smoothIter = params.getInt(QStringLiteral("iterations"));
        std::vector<Point> geomOrig(size_t(mesh.vn));
        for (int i = 0; i < mesh.vn; ++i)
            geomOrig[size_t(i)] = mesh.vert[size_t(i)].cP();
        vcg::tri::Smooth<VCGMesh>::VertexCoordLaplacian(mesh, smoothIter);
        for (int i = 0; i < mesh.vn; ++i) {
            mesh.vert[size_t(i)].P() =
                geomOrig[size_t(i)] * alphaOrig + (geomOrig[size_t(i)] - mesh.vert[size_t(i)].P()) * alpha;
        }
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied unsharp masking to geometry of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterUnsharpColor)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        const Scalar alpha = Scalar(params.getDouble(QStringLiteral("weight")));
        const Scalar alphaOrig = Scalar(params.getDouble(QStringLiteral("weightOrig")));
        const int smoothIter = params.getInt(QStringLiteral("iterations"));
        std::vector<vcg::Color4f> colorOrig(size_t(mesh.vn));
        for (int i = 0; i < mesh.vn; ++i)
            colorOrig[size_t(i)].Import(mesh.vert[size_t(i)].C());
        vcg::tri::Smooth<VCGMesh>::VertexColorLaplacian(mesh, smoothIter);
        for (int i = 0; i < mesh.vn; ++i) {
            vcg::Color4f colorDelta = colorOrig[size_t(i)] - vcg::Color4f::Construct(mesh.vert[size_t(i)].C());
            vcg::Color4f newColor = colorOrig[size_t(i)] * alphaOrig + colorDelta * alpha;
            vcg::Clamp(newColor);
            mesh.vert[size_t(i)].C().Import(newColor);
        }
        entry.ioMask |= Mask::IOM_VERTCOLOR;
        markMaterial(QObject::tr("Applied unsharp masking to vertex colors of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterUnsharpQuality)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromNone(mesh);
        const Scalar alpha = Scalar(params.getDouble(QStringLiteral("weight")));
        const Scalar alphaOrig = Scalar(params.getDouble(QStringLiteral("weightOrig")));
        const int smoothIter = params.getInt(QStringLiteral("iterations"));
        std::vector<float> qualityOrig(size_t(mesh.vn));
        for (int i = 0; i < mesh.vn; ++i)
            qualityOrig[size_t(i)] = mesh.vert[size_t(i)].Q();
        vcg::tri::Smooth<VCGMesh>::VertexQualityLaplacian(mesh, smoothIter);
        for (int i = 0; i < mesh.vn; ++i) {
            const float qualityDelta = qualityOrig[size_t(i)] - mesh.vert[size_t(i)].Q();
            mesh.vert[size_t(i)].Q() = qualityOrig[size_t(i)] * alphaOrig + qualityDelta * alpha;
        }
        entry.ioMask |= Mask::IOM_VERTQUALITY;
        markGeometry(QObject::tr("Applied unsharp masking to vertex quality of '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterLinearMorph)) {
        const int targetMeshIndex = params.getMesh(QStringLiteral("TargetMesh"), doc.currentMeshIndex());
        if (targetMeshIndex == meshIndex)
            return failResult(QObject::tr("Target mesh must be different from the current mesh."));

        VCGMesh targetMesh;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(targetMesh, doc.mesh(targetMeshIndex).mesh);
        if (mesh.vn != targetMesh.vn) {
            return failResult(QObject::tr("Number of vertices is not the same, so you can't morph between these two meshes."));
        }
        const Scalar percentage = Scalar(params.getDouble(QStringLiteral("PercentMorph")) / 100.0);
        for (int i = 0; i < mesh.vn; ++i) {
            Point &srcP = mesh.vert[size_t(i)].P();
            const Point trgP = targetMesh.vert[size_t(i)].cP();
            srcP = srcP + (trgP - srcP) * percentage;
        }
        updateBBoxAndNormals(mesh);
        entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        markGeometry(QObject::tr("Applied linear morphing to '%1'.").arg(meshName(entry, meshIndex)));
        return successResult();
    }

    if (filterId == QString::fromLatin1(kFilterScalarHarmonic)) {
        doc.beginFilterProgress(QObject::tr("Computing harmonic field"));
        if (vcg::tri::Clean<VCGMesh>::CountConnectedComponents(mesh) > 1) {
            doc.finishFilterProgress(false, QObject::tr("Mesh must have a single connected component."));
            return failResult(QObject::tr("A mesh composed by a single connected component is required by the filter to properly work."));
        }
        if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0) {
            doc.finishFilterProgress(false, QObject::tr("Mesh has non-manifold edges."));
            return failResult(QObject::tr("Mesh has some non-manifold faces; this filter requires manifoldness."));
        }
        if (vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(mesh) > 0) {
            doc.finishFilterProgress(false, QObject::tr("Mesh has non-manifold vertices."));
            return failResult(QObject::tr("Mesh has some non-manifold vertices; this filter requires manifoldness."));
        }

        if (cb)
            (*cb)(1, "Computing harmonic field...");

        vcg::GridStaticPtr<VCGVertex, Scalar> vertexGrid;
        vertexGrid.Set(mesh.vert.begin(), mesh.vert.end());
        vcg::vertex::PointDistanceFunctor<Scalar> pointDistance;
        vcg::tri::Tmark<VCGMesh, VCGVertex> marker;
        marker.SetMesh(&mesh);
        marker.UnMarkAll();
        Point closestPoint;
        Scalar minDist = 0;
        VCGVertex *v0 = vcg::GridClosest(
            vertexGrid,
            pointDistance,
            marker,
            toPoint(params.getPoint3f(QStringLiteral("point1"))),
            mesh.bbox.Diag(),
            minDist,
            closestPoint);
        VCGVertex *v1 = vcg::GridClosest(
            vertexGrid,
            pointDistance,
            marker,
            toPoint(params.getPoint3f(QStringLiteral("point2"))),
            mesh.bbox.Diag(),
            minDist,
            closestPoint);
        if (!v0 || !v1 || v0 == v1) {
            doc.finishFilterProgress(false, QObject::tr("Could not resolve the constrained vertices."));
            return failResult(QObject::tr("Error occurred for selected points."));
        }

        using FieldScalar = float;
        typename vcg::tri::Harmonic<VCGMesh, FieldScalar>::ConstraintVec constraints;
        constraints.push_back(typename vcg::tri::Harmonic<VCGMesh, FieldScalar>::Constraint(v0, FieldScalar(params.getDouble(QStringLiteral("value1")))));
        constraints.push_back(typename vcg::tri::Harmonic<VCGMesh, FieldScalar>::Constraint(v1, FieldScalar(params.getDouble(QStringLiteral("value2")))));

        auto handle = vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<FieldScalar>(mesh, "harmonic");
        const bool ok = vcg::tri::Harmonic<VCGMesh, FieldScalar>::ComputeScalarField(mesh, constraints, handle);
        if (!ok) {
            doc.finishFilterProgress(false, QObject::tr("The harmonic solver reported an error."));
            return failResult(QObject::tr("An error occurred."));
        }

        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            vi->Q() = handle[vi];
        }
        entry.ioMask |= Mask::IOM_VERTQUALITY;
        if (params.getBool(QStringLiteral("colorize"))) {
            vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(mesh);
            entry.ioMask |= Mask::IOM_VERTCOLOR;
            markMaterial(QObject::tr("Computed harmonic field colors for '%1'.").arg(meshName(entry, meshIndex)));
        }
        markGeometry(QObject::tr("Computed scalar harmonic field for '%1'.").arg(meshName(entry, meshIndex)));
        doc.finishFilterProgress(true, QObject::tr("Done."));
        MeshFilterRunResult result = successResult();
        result.visualizationHints.push_back({
            meshIndex,
            MeshFilterVisualizationAttribute::VertexQuality
        });
        return result;
    }

    return failResult(QObject::tr("Unknown filter id '%1'.").arg(filterId));
}

void registerUnsharpFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<UnsharpFilterPlugin>());
}
