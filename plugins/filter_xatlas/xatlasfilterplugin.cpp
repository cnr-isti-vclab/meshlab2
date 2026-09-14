#include "xatlasfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "textureassociationutils.h"
#include "upstream/xatlas.h"
#include <wrap/callback.h>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <QScopeGuard>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr QLatin1StringView kFilterXAtlas("parametrize_by_atlas_xatlas");
using Mask = vcg::tri::io::Mask;
namespace Tex = TextureAssociationUtils;

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

struct ProgressBridge
{
    vcg::CallBackPos *callback = vcg::DummyCallBackPos;
};

const char *progressLabel(xatlas::ProgressCategory category)
{
    switch (category) {
    case xatlas::ProgressCategory::AddMesh:
        return "xatlas: copying mesh";
    case xatlas::ProgressCategory::ComputeCharts:
        return "xatlas: computing charts";
    case xatlas::ProgressCategory::PackCharts:
        return "xatlas: packing charts";
    case xatlas::ProgressCategory::BuildOutputMeshes:
        return "xatlas: building output";
    }
    return "xatlas";
}

bool xatlasProgressCallback(xatlas::ProgressCategory category, int progress, void *userData)
{
    ProgressBridge *bridge = static_cast<ProgressBridge *>(userData);
    if (!bridge || !bridge->callback)
        return true;
    return (*bridge->callback)(progress, progressLabel(category));
}

float finiteNonNegativeDouble(const FilterParams &params, const QString &id, double fallback = 0.0)
{
    const double value = params.getDouble(id, fallback);
    if (!std::isfinite(value) || value < 0.0)
        return -1.0f;
    if (value > double(std::numeric_limits<float>::max()))
        return -1.0f;
    return float(value);
}

} // namespace

QString XAtlasFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.xatlas");
}

QString XAtlasFilterPlugin::name() const
{
    return QObject::tr("xatlas Parametrization Filters");
}

MeshFilterRunResult XAtlasFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId != QString::fromLatin1(kFilterXAtlas))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    if (mesh.VN() <= 0 || mesh.FN() <= 0)
        return fail(QObject::tr("Current mesh must contain vertices and faces."));

    const int triangleCount = mesh.FN();
    const int vertexCount = mesh.VN();

    const float maxChartArea =
        finiteNonNegativeDouble(params, QStringLiteral("maxChartArea"), 0.0);
    const float maxBoundaryLength =
        finiteNonNegativeDouble(params, QStringLiteral("maxBoundaryLength"), 0.0);
    const float normalDeviationWeight =
        finiteNonNegativeDouble(params, QStringLiteral("normalDeviationWeight"), 2.0);
    const float roundnessWeight =
        finiteNonNegativeDouble(params, QStringLiteral("roundnessWeight"), 0.01);
    const float straightnessWeight =
        finiteNonNegativeDouble(params, QStringLiteral("straightnessWeight"), 6.0);
    const float normalSeamWeight =
        finiteNonNegativeDouble(params, QStringLiteral("normalSeamWeight"), 4.0);
    const float maxCost =
        finiteNonNegativeDouble(params, QStringLiteral("maxCost"), 2.0);
    const float texelsPerUnit =
        finiteNonNegativeDouble(params, QStringLiteral("texelsPerUnit"), 0.0);

    if (maxChartArea < 0.0f || maxBoundaryLength < 0.0f || normalDeviationWeight < 0.0f
        || roundnessWeight < 0.0f || straightnessWeight < 0.0f || normalSeamWeight < 0.0f
        || maxCost < 0.0f || texelsPerUnit < 0.0f) {
        return fail(QObject::tr("One or more xatlas numeric parameters are invalid."));
    }

    const int maxIterations = params.getInt(QStringLiteral("maxIterations"), 1);
    const int maxChartSize = params.getInt(QStringLiteral("maxChartSize"), 0);
    const int padding = params.getInt(QStringLiteral("padding"), 0);
    const int resolution = params.getInt(QStringLiteral("resolution"), 0);
    if (maxIterations < 1)
        return fail(QObject::tr("Max iterations must be at least 1."));
    if (maxChartSize < 0 || padding < 0 || resolution < 0)
        return fail(QObject::tr("Max chart size, padding, and resolution must be non-negative."));

    std::vector<float> positions;
    positions.reserve(size_t(vertexCount) * 3u);
    std::vector<float> normals;
    normals.reserve(size_t(vertexCount) * 3u);
    for (const VCGVertex &vertex : mesh.vert) {
        positions.push_back(vertex.cP().X());
        positions.push_back(vertex.cP().Y());
        positions.push_back(vertex.cP().Z());
        normals.push_back(vertex.cN().X());
        normals.push_back(vertex.cN().Y());
        normals.push_back(vertex.cN().Z());
    }

    std::vector<uint32_t> indices;
    indices.reserve(size_t(triangleCount) * 3u);
    for (const VCGFace &face : mesh.face) {
        indices.push_back(uint32_t(vcg::tri::Index(mesh, face.cV(0))));
        indices.push_back(uint32_t(vcg::tri::Index(mesh, face.cV(1))));
        indices.push_back(uint32_t(vcg::tri::Index(mesh, face.cV(2))));
    }

    xatlas::MeshDecl meshDecl;
    meshDecl.vertexCount = uint32_t(vertexCount);
    meshDecl.vertexPositionData = positions.data();
    meshDecl.vertexPositionStride = uint32_t(sizeof(float) * 3u);
    meshDecl.vertexNormalData = normals.data();
    meshDecl.vertexNormalStride = uint32_t(sizeof(float) * 3u);
    meshDecl.indexData = indices.data();
    meshDecl.indexCount = uint32_t(indices.size());
    meshDecl.indexFormat = xatlas::IndexFormat::UInt32;
    meshDecl.faceCount = uint32_t(triangleCount);

    xatlas::ChartOptions chartOptions;
    chartOptions.maxChartArea = maxChartArea;
    chartOptions.maxBoundaryLength = maxBoundaryLength;
    chartOptions.normalDeviationWeight = normalDeviationWeight;
    chartOptions.roundnessWeight = roundnessWeight;
    chartOptions.straightnessWeight = straightnessWeight;
    chartOptions.normalSeamWeight = normalSeamWeight;
    chartOptions.maxCost = maxCost;
    chartOptions.maxIterations = uint32_t(maxIterations);
    chartOptions.fixWinding = params.getBool(QStringLiteral("fixWinding"), false);

    xatlas::PackOptions packOptions;
    packOptions.maxChartSize = uint32_t(maxChartSize);
    packOptions.padding = uint32_t(padding);
    packOptions.texelsPerUnit = texelsPerUnit;
    packOptions.resolution = uint32_t(resolution);
    packOptions.bilinear = params.getBool(QStringLiteral("bilinear"), true);
    packOptions.blockAlign = params.getBool(QStringLiteral("blockAlign"), false);
    packOptions.bruteForce = params.getBool(QStringLiteral("bruteForce"), false);
    packOptions.rotateChartsToAxis = params.getBool(QStringLiteral("rotateChartsToAxis"), true);
    packOptions.rotateCharts = params.getBool(QStringLiteral("rotateCharts"), true);
    packOptions.createImage = false;

    xatlas::Atlas *atlas = xatlas::Create();
    if (!atlas)
        return fail(QObject::tr("Failed to create xatlas atlas."));

    doc.beginFilterProgress(QObject::tr("Parametrize by Atlas (xatlas)"));
    const auto destroyAtlas = qScopeGuard([&]() { xatlas::Destroy(atlas); });
    ProgressBridge progressBridge;
    progressBridge.callback = doc.progressCallback() ? doc.progressCallback() : vcg::DummyCallBackPos;
    xatlas::SetProgressCallback(atlas, xatlasProgressCallback, &progressBridge);

    const xatlas::AddMeshError addResult = xatlas::AddMesh(atlas, meshDecl, 1);
    if (addResult != xatlas::AddMeshError::Success) {
        const QString message = QObject::tr("xatlas rejected the mesh: %1")
            .arg(QString::fromLatin1(xatlas::StringForEnum(addResult)));
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    xatlas::Generate(atlas, chartOptions, packOptions);

    if (atlas->meshCount != 1 || !atlas->meshes) {
        const QString message = QObject::tr("xatlas did not produce an output mesh.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    if (atlas->atlasCount > 1) {
        const QString message = QObject::tr(
            "xatlas generated %1 atlases. This first MeshLab integration currently supports only single-atlas output. "
            "Try lowering texels-per-unit, leaving resolution at 0, or reducing padding.")
            .arg(atlas->atlasCount);
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    if (atlas->width == 0 || atlas->height == 0) {
        const QString message = QObject::tr("xatlas produced an invalid atlas resolution.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    const xatlas::Mesh &outputMesh = atlas->meshes[0];
    if (outputMesh.indexCount != uint32_t(triangleCount * 3) || !outputMesh.indexArray || !outputMesh.vertexArray) {
        const QString message = QObject::tr("xatlas output index data does not match the current triangular mesh.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }

    mesh.face.EnableWedgeTexCoord();
    const float invWidth = 1.0f / float(atlas->width);
    const float invHeight = 1.0f / float(atlas->height);
    for (int faceIndex = 0; faceIndex < triangleCount; ++faceIndex) {
        VCGFace &face = mesh.face[size_t(faceIndex)];
        for (int corner = 0; corner < 3; ++corner) {
            const uint32_t outVertexIndex = outputMesh.indexArray[size_t(faceIndex * 3 + corner)];
            if (outVertexIndex >= outputMesh.vertexCount) {
                const QString message = QObject::tr("xatlas returned an out-of-range output vertex index.");
                doc.finishFilterProgress(false, message);
                return fail(message);
            }
            const xatlas::Vertex &outVertex = outputMesh.vertexArray[outVertexIndex];
            face.WT(corner) = VCGFace::TexCoordType(outVertex.uv[0] * invWidth, outVertex.uv[1] * invHeight);
            face.WT(corner).N() = 0;
        }
    }

    QString dummyTextureInfo;
    if (params.getBool(QStringLiteral("use_dummy_texture"))) {
        const int imageSize = params.getInt(QStringLiteral("dummy_img_size"));
        const int checkSize = params.getInt(QStringLiteral("dummy_check_size"));
        const QString dummyType = params.getEnum(QStringLiteral("dummy_type"));
        if (imageSize <= 0) {
            const QString message = QObject::tr("Dummy size has an incorrect value.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }
        if (checkSize <= 0) {
            const QString message = QObject::tr("Check size has an incorrect value.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }
        const bool checkerboard = (dummyType != QStringLiteral("grid"));
        const QString displayName = checkerboard
            ? QStringLiteral("xatlas Dummy Checkerboard")
            : QStringLiteral("xatlas Dummy Grid");
        Tex::replaceTextureAssociations(
            entry,
            { Tex::makeTextureAssetFromImage(
                Tex::makeDummyTexture(imageSize, checkSize, checkerboard),
                displayName) });
        dummyTextureInfo = QObject::tr("Added dummy texture: %1 (%2x%2).")
            .arg(displayName)
            .arg(imageSize);
    }

    entry.ioMask |= Mask::IOM_WEDGTEXCOORD;
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    // Per-wedge UVs live inside the VCGMesh and the undo system interns geometry
    // by geometryRevision, so enabling the wedge-texcoord component and writing
    // UVs is a GEOMETRY change: it must bump geometryRevision or the undo snapshot
    // reuses the stale pre-parametrization copy (UV-less) while ioMask claims UVs,
    // desyncing them and crashing every per-wedge reader after an undo/redo.
    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Computed xatlas UV parametrization for '%1'").arg(entry.name));

    QStringList info;
    info << QObject::tr("Computed xatlas UV parametrization for '%1'.").arg(entry.name)
         << QObject::tr("Atlas resolution: %1 x %2").arg(atlas->width).arg(atlas->height)
         << QObject::tr("Charts: %1").arg(atlas->chartCount)
         << QObject::tr("Output vertices after seam splitting: %1").arg(outputMesh.vertexCount)
         << QObject::tr("Texels per unit: %1").arg(QString::number(atlas->texelsPerUnit, 'f', 3));
    if (!dummyTextureInfo.isEmpty())
        info << dummyTextureInfo;
    if (atlas->atlasCount > 0 && atlas->utilization)
        info << QObject::tr("Utilization: %1%").arg(QString::number(atlas->utilization[0] * 100.0f, 'f', 2));

    doc.finishFilterProgress(true, QObject::tr("Generated xatlas UVs."));
    return success(info);
}

void registerXAtlasFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<XAtlasFilterPlugin>());
}
