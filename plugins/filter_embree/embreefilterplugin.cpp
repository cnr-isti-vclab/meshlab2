#include "embreefilterplugin.h"

#include "document.h"
#include "preferences.h"
#include "meshfilterpluginmanager.h"
#include <QMatrix4x4>
#include <QVector3D>
#include <wrap/embree/EmbreeAdaptor.h>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace {
constexpr QLatin1StringView kFilterObscurance("compute_obscurance");
constexpr QLatin1StringView kFilterFaceAmbientOcclusion("compute_face_ambient_occlusion");
constexpr QLatin1StringView kFilterPointCloudAmbientOcclusion(
    "compute_point_cloud_ambient_occlusion");
constexpr QLatin1StringView kFilterShapeDiameter("compute_shape_diameter_function");
constexpr QLatin1StringView kFilterSelectVisibleFaces("select_visible_faces");
constexpr QLatin1StringView kFilterAnalyzeNormals("orient_face_normals_by_ray_casting");

struct CurrentMeshRef
{
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
    return CurrentMeshRef { meshIndex, &doc.mesh(meshIndex) };
}

MeshFilterRunResult qualityResult(
    int meshIndex,
    MeshFilterVisualizationAttribute attribute,
    const QStringList &messages)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = messages;
    result.visualizationHints.push_back({ meshIndex, attribute });
    return result;
}

std::vector<vcg::Point3f> ambientDirections(
    int rays,
    float directionalBias,
    const vcg::Point3f &coneDirection,
    float coneAngleDegrees)
{
    const int coneRays = std::clamp(int(std::lround(rays * directionalBias)), 0, rays);
    std::vector<vcg::Point3f> directions;
    vcg::GenNormal<float>::Fibonacci(rays - coneRays, directions);
    if (coneRays > 0) {
        std::vector<vcg::Point3f> coneDirections;
        vcg::GenNormal<float>::UniformCone(
            coneRays, coneDirections, vcg::math::ToRad(coneAngleDegrees), coneDirection);
        directions.insert(directions.end(), coneDirections.begin(), coneDirections.end());
    }
    return directions;
}

bool hasValidVertexNormals(const VCGMesh &mesh)
{
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        const vcg::Point3f &n = v.cN();
        if (!std::isfinite(n.X()) || !std::isfinite(n.Y()) || !std::isfinite(n.Z())
            || n.SquaredNorm() <= 1e-20f)
            return false;
    }
    return mesh.VN() > 0;
}

vcg::Matrix44f qtToVcg(const QMatrix4x4 &m)
{
    vcg::Matrix44f result;
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            result[row][column] = m(row, column);
    return result;
}
}

QString EmbreeFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.embree");
}

QString EmbreeFilterPlugin::name() const
{
    return QObject::tr("Embree Filters");
}

MeshFilterRunResult EmbreeFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    QString errorMessage;
    auto current = currentMesh(doc, errorMessage);
    if (!current)
        return { false, false, errorMessage };
    Document::MeshEntry &entry = *current->entry;

    using Mask = vcg::tri::io::Mask;
    const int rays = std::clamp(params.getInt(QStringLiteral("rays")), 1, 8192);
    vcg::CallBackPos *cb = doc.progressCallback();
    auto interruptedResult = [&]() -> MeshFilterRunResult {
        return { false, false, QObject::tr("Filter interrupted by user.") };
    };

    const bool faceAmbient = filterId == QString::fromLatin1(kFilterFaceAmbientOcclusion);
    const bool pointAmbient = filterId == QString::fromLatin1(kFilterPointCloudAmbientOcclusion);
    if (faceAmbient || pointAmbient) {
        const float bias = std::clamp(
            float(params.getDouble(QStringLiteral("directional_bias"))), 0.0f, 1.0f);
        const QVector3D dv = params.getPoint3f(QStringLiteral("cone_direction"));
        const vcg::Point3f coneDirection(float(dv.x()), float(dv.y()), float(dv.z()));
        if (bias > 0.0f && coneDirection.SquaredNorm() <= 1e-20f)
            return { false, false, QObject::tr("Lighting direction must be non-zero.") };
        const float coneAngle = std::clamp(
            float(params.getDouble(QStringLiteral("cone_half_angle"))), 0.0f, 180.0f);
        const std::vector<vcg::Point3f> directions =
            ambientDirections(rays, bias, coneDirection, coneAngle);

        if (faceAmbient) {
            if (entry.mesh.FN() <= 0)
                return { false, false, QObject::tr("Current mesh has no faces.") };

            vcg::EmbreeAdaptor<VCGMesh> adaptor(entry.mesh);
            adaptor.callbackChunkCount =
                Preferences::instance().intValue(QStringLiteral("advanced.rayCallbackChunks"));
            adaptor.computeAmbientOcclusion(entry.mesh, directions, cb);
            if (doc.isOperationCancelRequested())
                return interruptedResult();

            vcg::tri::UpdateQuality<VCGMesh>::VertexFromFace(entry.mesh);
            entry.ioMask |= Mask::IOM_FACEQUALITY | Mask::IOM_VERTQUALITY;
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Computed face ambient occlusion for '%1'").arg(entry.name));
            return qualityResult(
                current->index, MeshFilterVisualizationAttribute::FaceQuality,
                { QObject::tr("Computed face ambient occlusion using %1 rays").arg(rays) });
        }

        if (entry.mesh.FN() > 0) {
            return { false, false,
                QObject::tr("Point-cloud ambient occlusion requires a layer without faces.") };
        }
        const int occluderIndex = params.getMesh(QStringLiteral("occluder_mesh"));
        if (occluderIndex < 0 || occluderIndex >= doc.meshCount()
            || doc.mesh(occluderIndex).mesh.FN() <= 0) {
            return { false, false,
                QObject::tr("The point-cloud occluder must be a surface mesh with faces.") };
        }

        const QString normalSource = params.getEnum(QStringLiteral("normal_source"));
        if (normalSource == QStringLiteral("point_normals") && !hasValidVertexNormals(entry.mesh)) {
            return { false, false,
                QObject::tr("Point-normal mode requires a valid normal on every point.") };
        }

        bool invertible = false;
        const QMatrix4x4 worldToTarget = entry.transform.inverted(&invertible);
        if (!invertible) {
            return { false, false,
                QObject::tr("The current mesh transform is not invertible.") };
        }
        Document::MeshEntry &occluder = doc.mesh(occluderIndex);
        // Upload the occluder in target-local coordinates. This honors both layer
        // transforms without copying what may be a very large mesh.
        const QMatrix4x4 occluderToTargetQt = worldToTarget * occluder.transform;
        vcg::EmbreeAdaptor<VCGMesh> adaptor(occluder.mesh, qtToVcg(occluderToTargetQt));
        adaptor.callbackChunkCount =
            Preferences::instance().intValue(QStringLiteral("advanced.rayCallbackChunks"));

        if (normalSource == QStringLiteral("point_normals")) {
            adaptor.computeAmbientOcclusionPerVertex(entry.mesh, directions, cb);
        } else if (normalSource == QStringLiteral("closest_occluder_surface")) {
            auto closestSurfaceNormal = [&adaptor](const VCGVertex &vertex) {
                return adaptor.closestFaceNormal(vcg::Point3f::Construct(vertex.cP()));
            };
            adaptor.computeAmbientOcclusionPerVertex(
                entry.mesh, directions, closestSurfaceNormal, cb);
        } else if (normalSource == QStringLiteral("no_normal_spherical")) {
            adaptor.computeSphericalOcclusionPerVertex(entry.mesh, directions, cb);
        } else {
            return { false, false, QObject::tr("Unknown point-cloud normal source.") };
        }
        if (doc.isOperationCancelRequested())
            return interruptedResult();

        entry.ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(
            current->index,
            QObject::tr("Computed point-cloud ambient occlusion for '%1'").arg(entry.name));
        return qualityResult(
            current->index, MeshFilterVisualizationAttribute::VertexQuality,
            { QObject::tr("Computed point-cloud ambient occlusion using %1 rays against '%2'")
                  .arg(rays)
                  .arg(occluder.name) });
    }

    if (entry.mesh.FN() <= 0)
        return { false, false, QObject::tr("Current mesh has no faces.") };

    vcg::EmbreeAdaptor<VCGMesh> adaptor(entry.mesh);
    adaptor.callbackChunkCount =
        Preferences::instance().intValue(QStringLiteral("advanced.rayCallbackChunks"));

    if (filterId == QString::fromLatin1(kFilterObscurance)) {
        const float tau = float(params.getDouble(QStringLiteral("tau")));
        adaptor.computeObscurance(entry.mesh, rays, tau, cb);
        if (doc.isOperationCancelRequested())
            return interruptedResult();
        vcg::tri::UpdateQuality<VCGMesh>::VertexFromFace(entry.mesh);
        // Deliberately does NOT bake color: a Compute filter stores the scalar and
        // lets the render pass map scalar->color (via the visualization hint below).
        // Baking here silently destroyed any existing vertex/face color, and the
        // descriptor only ever declared outputModifies FQ/VQ.
        entry.ioMask |= Mask::IOM_FACEQUALITY | Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(
            current->index,
            QObject::tr("Computed obscurance for '%1'").arg(entry.name));

        return qualityResult(current->index, MeshFilterVisualizationAttribute::FaceQuality, {
            QObject::tr("Computed obscurance using %1 rays and tau=%2")
                .arg(rays)
                .arg(QString::number(tau, 'f', 4))
        });
    }

    if (filterId == QString::fromLatin1(kFilterShapeDiameter)) {
        const float coneAmplitude = float(params.getDouble(QStringLiteral("cone_amplitude")));
        adaptor.computeSDF(entry.mesh, rays, coneAmplitude, cb);
        if (doc.isOperationCancelRequested())
            return interruptedResult();
        vcg::tri::UpdateQuality<VCGMesh>::VertexFromFace(entry.mesh);

        entry.ioMask |=
            Mask::IOM_FACEQUALITY | Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(
            current->index,
            QObject::tr("Computed shape diameter function for '%1'").arg(entry.name));

        return qualityResult(current->index, MeshFilterVisualizationAttribute::FaceQuality, {
            QObject::tr("Computed SDF using %1 rays and cone amplitude=%2°")
                .arg(rays)
                .arg(QString::number(coneAmplitude, 'f', 2))
        });
    }

    if (filterId == QString::fromLatin1(kFilterSelectVisibleFaces)) {
        const QVector3D dv = params.getPoint3f(QStringLiteral("direction"));
        const vcg::Point3f dir { -float(dv.x()), -float(dv.y()), -float(dv.z()) };
        if (dir.SquaredNorm() <= 1e-20f)
            return { false, false, QObject::tr("Direction vector must be non-zero.") };

        // Always pass incremental=false: the framework manager handles incremental
        // selection by ORing back the pre-run selection when requested.
        adaptor.selectVisibleFaces(entry.mesh, dir, false, cb);
        if (doc.isOperationCancelRequested())
            return interruptedResult();

        int selectedFaces = 0;
        for (const VCGFace &f : entry.mesh.face) {
            if (f.IsS())
                ++selectedFaces;
        }

        entry.ioMask |= Mask::IOM_FACEFLAGS;
        doc.markMeshSelectionChanged(
            current->index,
            QObject::tr("Selected visible faces for '%1'").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Selected %1 visible faces").arg(selectedFaces)
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterAnalyzeNormals)) {
        const bool parity = params.getBool(QStringLiteral("parity_sampling"));
        adaptor.computeNormalAnalysis(entry.mesh, rays, parity, cb);
        if (doc.isOperationCancelRequested())
            return interruptedResult();

        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(entry.mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry.mesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(entry.mesh);

        entry.ioMask |= Mask::IOM_FACENORMAL | Mask::IOM_VERTNORMAL | Mask::IOM_FACEFLAGS;
        doc.markMeshGeometryChanged(
            current->index,
            QObject::tr("Reoriented face normals for '%1'").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Normal reorientation completed (%1 sampling)")
                .arg(parity ? QObject::tr("parity") : QObject::tr("visibility"))
        };
        return result;
    }

    return { false, false, QObject::tr("Unknown filter id: %1").arg(filterId) };
}

void registerEmbreeFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<EmbreeFilterPlugin>());
}
