#include "renderwidget.h"
#include "linerenderer.h"
#include "document.h"
#include "preferences.h"
#include "renderoverlaypanel.h"
#include "renderwidget_internal.h"
#include <wrap/io_trimesh/io_mask.h>
#include <QHash>
#include <algorithm>
#include <cmath>

using namespace RenderWidgetInternal;

namespace {
bool hasSignificantVertexQuality(const VCGMesh &mesh)
{
    if (mesh.vert.empty())
        return false;
    float minVal = mesh.vert[0].Q();
    float maxVal = minVal;
    for (const auto &v : mesh.vert) {
        if (v.Q() < minVal)
            minVal = v.Q();
        if (v.Q() > maxVal)
            maxVal = v.Q();
    }
    return std::abs(maxVal - minVal) > 1e-5f;
}

bool hasSignificantFaceQuality(const VCGMesh &mesh)
{
    if (mesh.face.empty())
        return false;
    float minVal = mesh.face[0].Q();
    float maxVal = minVal;
    for (const auto &f : mesh.face) {
        if (f.Q() < minVal)
            minVal = f.Q();
        if (f.Q() > maxVal)
            maxVal = f.Q();
    }
    return std::abs(maxVal - minVal) > 1e-5f;
}

int findTextureIndexByPath(const Document::MeshEntry &entry, const QString &path)
{
    const QString wanted = normalizeTexturePath(path);
    if (wanted.isEmpty())
        return -1;
    const int textureCount = Document::meshTextureAssociationCount(entry);
    for (int i = 0; i < textureCount; ++i) {
        if (normalizeTexturePath(Document::meshTextureSourcePath(entry, i)) == wanted)
            return i;
    }
    return -1;
}

enum class MaterialTextureChannel {
    BaseColor,
    Normal,
    Occlusion,
    Roughness
};

int defaultTextureIndexForChannel(
    const Document::MeshEntry &entry,
    MaterialTextureChannel channel)
{
    for (const MeshIOMaterialSlot &slot : entry.materialSet.entries) {
        const MeshIOMaterialTextureRef *ref = nullptr;
        switch (channel) {
        case MaterialTextureChannel::BaseColor: ref = &slot.baseColorTexture; break;
        case MaterialTextureChannel::Normal: ref = &slot.normalTexture; break;
        case MaterialTextureChannel::Occlusion: ref = &slot.occlusionTexture; break;
        case MaterialTextureChannel::Roughness: ref = &slot.roughnessTexture; break;
        }
        if (!ref || !ref->isValid())
            continue;
        const int textureIndex = findTextureIndexByPath(entry, ref->filePath);
        if (textureIndex >= 0)
            return textureIndex;
        if (ref->assetIndex >= 0)
            return ref->assetIndex;
    }
    if (channel == MaterialTextureChannel::BaseColor && Document::meshTextureAssociationCount(entry) > 0)
        return 0;
    return -1;
}

QStringList pbrTextureSelectorEntries(const Document::MeshEntry &entry)
{
    QStringList labels;
    const int textureCount = Document::meshTextureAssociationCount(entry);
    labels.reserve(textureCount);
    for (int i = 0; i < textureCount; ++i) {
        const QString name = Document::meshTextureDisplayName(entry, i);
        labels.push_back(QObject::tr("%1: %2").arg(i).arg(name));
    }
    return labels;
}
}

RenderWidget::MeshRenderMode RenderWidget::defaultRenderModeForMesh(int meshIndex) const
{
    MeshRenderMode mode;
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return mode;

    const auto &entry = m_doc->mesh(meshIndex);
    const int faceCount = entry.mesh.FN();
    const int edgeCount = entry.mesh.EN();
    const int mask = entry.ioMask;
    const bool hasVertexColors = (mask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0;
    const bool hasFaceColors = (mask & vcg::tri::io::Mask::IOM_FACECOLOR) != 0;
    mode.edgeColorSource = (mask & vcg::tri::io::Mask::IOM_EDGECOLOR)
        ? EdgeColorSource::PerEdge : EdgeColorSource::Constant;
    const bool hasVertexQuality = (mask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0 && hasSignificantVertexQuality(entry.mesh);
    const bool hasFaceQuality = (mask & vcg::tri::io::Mask::IOM_FACEQUALITY) != 0 && hasSignificantFaceQuality(entry.mesh);
    const bool hasVertexNormals = (mask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0;
    const bool hasTextureCoords =
        (mask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0
        || (mask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0;
    const bool hasTextures = hasTextureCoords && Document::meshTextureAssociationCount(entry) > 0;
    const int defaultAlbedoTextureIndex =
        defaultTextureIndexForChannel(entry, MaterialTextureChannel::BaseColor);
    const int defaultNormalTextureIndex =
        defaultTextureIndexForChannel(entry, MaterialTextureChannel::Normal);
    const int defaultOcclusionTextureIndex =
        defaultTextureIndexForChannel(entry, MaterialTextureChannel::Occlusion);
    const int defaultRoughnessTextureIndex =
        defaultTextureIndexForChannel(entry, MaterialTextureChannel::Roughness);
    const bool hasAnyPbrMap =
        defaultNormalTextureIndex >= 0
        || defaultOcclusionTextureIndex >= 0
        || defaultRoughnessTextureIndex >= 0;

    // Both thresholds are user preferences; see resources/preferences.json. They are
    // read per mesh rather than cached so that changing one in the dialog takes effect
    // for the next mesh loaded, without needing a restart.
    const Preferences &preferences = Preferences::instance();
    const int wireframeMaxFaces =
        preferences.intValue(QStringLiteral("render.wireframeOnLoadMaxFaces"));
    const int flatShadingMaxFaces =
        preferences.intValue(QStringLiteral("render.flatShadingMaxFaces"));

    if (faceCount > 0) {
        mode.showFill = true;
        mode.showWire = faceCount < wireframeMaxFaces;
        mode.showEdges = false;
        mode.showPoints = false;
        mode.fillPlain.colorSource = FillColorSource::Constant;
        if (hasTextures)
            mode.fillPlain.colorSource = FillColorSource::Texture;
        else if (hasVertexColors)
            mode.fillPlain.colorSource = FillColorSource::PerVertex;
        else if (hasFaceColors)
            mode.fillPlain.colorSource = FillColorSource::PerFace;
        else if (hasVertexQuality)
            mode.fillPlain.colorSource = FillColorSource::PerVertexQuality;
        else if (hasFaceQuality)
            mode.fillPlain.colorSource = FillColorSource::PerFaceQuality;
        mode.pointColorSource = hasVertexColors
            ? PointColorSource::PerVertex
            : (hasVertexQuality ? PointColorSource::PerVertexQuality : PointColorSource::Constant);
        mode.fillLighting = true;
        mode.fillMaterial = (hasTextures && hasAnyPbrMap) ? FillMaterial::Pbr : FillMaterial::Plain;
        mode.fillPbr.albedoSource = hasTextures
            ? FillPbrTextureSource::Texture
            : FillPbrTextureSource::Constant;
        mode.fillPbr.albedoIndex = hasTextures ? -1 : defaultAlbedoTextureIndex;
        mode.fillPbr.normalSource = defaultNormalTextureIndex >= 0
            ? FillPbrTextureSource::Texture
            : FillPbrTextureSource::None;
        mode.fillPbr.normalIndex = defaultNormalTextureIndex >= 0 ? -1 : defaultNormalTextureIndex;
        mode.fillPbr.occlusionSource = defaultOcclusionTextureIndex >= 0
            ? FillPbrTextureSource::Texture
            : FillPbrTextureSource::None;
        mode.fillPbr.occlusionIndex = defaultOcclusionTextureIndex >= 0 ? -1 : defaultOcclusionTextureIndex;
        mode.fillPbr.roughnessSource = defaultRoughnessTextureIndex >= 0
            ? FillPbrTextureSource::Texture
            : FillPbrTextureSource::Constant;
        mode.fillPbr.roughnessIndex = defaultRoughnessTextureIndex >= 0 ? -1 : defaultRoughnessTextureIndex;
        if (faceCount < flatShadingMaxFaces) {
            mode.fillPlain.shading = FillShading::Flat;
            mode.fillPbr.shading   = FillShading::Flat;
        }
        mode.pointLighting = false;
        mode.wireLighting = false;
    } else if (edgeCount > 0) {
        mode.showFill = false;
        mode.showWire = false;
        mode.showEdges = true;
        mode.showPoints = hasVertexColors;
        mode.pointColorSource = hasVertexColors ? PointColorSource::PerVertex : PointColorSource::Constant;
        mode.edgeSize = 4.0f;
        if (hasVertexColors)
            mode.pointSize = 8.0f;
        mode.fillLighting = false;
        mode.pointLighting = false;
        mode.wireLighting = false;
    } else {
        mode.showFill = false;
        mode.showWire = false;
        mode.showEdges = false;
        mode.showPoints = true;
        mode.pointColorSource = hasVertexColors
            ? PointColorSource::PerVertex
            : (hasVertexQuality ? PointColorSource::PerVertexQuality : PointColorSource::Constant);
        mode.pointLighting = hasVertexNormals;
        mode.fillLighting = false;
        mode.wireLighting = false;
    }

    return mode;
}

void RenderWidget::syncPerMeshRenderModesWithDocument()
{
    if (!m_doc) {
        m_meshRenderModes.clear();
        return;
    }

    std::unordered_map<std::uint64_t, bool> aliveMeshIds;
    aliveMeshIds.reserve(size_t(m_doc->meshCount()));
    for (int i = 0; i < m_doc->meshCount(); ++i)
        aliveMeshIds.emplace(m_doc->mesh(i).meshId, true);

    for (auto it = m_meshRenderModes.begin(); it != m_meshRenderModes.end();) {
        if (aliveMeshIds.find(it->first) == aliveMeshIds.end())
            it = m_meshRenderModes.erase(it);
        else
            ++it;
    }
    for (auto it = m_meshRenderModeRevisions.begin(); it != m_meshRenderModeRevisions.end();) {
        if (aliveMeshIds.find(it->first) == aliveMeshIds.end())
            it = m_meshRenderModeRevisions.erase(it);
        else
            ++it;
    }

    for (int i = 0; i < m_doc->meshCount(); ++i) {
        const auto &entry = m_doc->mesh(i);
        const std::uint64_t meshId = m_doc->mesh(i).meshId;
        const std::pair<std::uint64_t, std::uint64_t> revisions{
            entry.geometryRevision,
            entry.materialRevision
        };
        auto it = m_meshRenderModes.find(meshId);
        if (it == m_meshRenderModes.end()) {
            m_meshRenderModes.emplace(meshId, defaultRenderModeForMesh(i));
            m_meshRenderModeRevisions[meshId] = revisions;
            continue;
        }

        if (!(entry.ioMask & vcg::tri::io::Mask::IOM_EDGECOLOR))
            it->second.edgeColorSource = EdgeColorSource::Constant;
        auto revIt = m_meshRenderModeRevisions.find(meshId);
        if (revIt == m_meshRenderModeRevisions.end()) {
            m_meshRenderModeRevisions[meshId] = revisions;
            continue;
        }

        if (revIt->second.second != entry.materialRevision) {
            MeshRenderMode &mode = it->second;
            if (mode.fillPlain.colorSource == FillColorSource::Texture)
                mode.fillPlain.textureIndex = -1;
            if (mode.fillPbr.albedoSource == FillPbrTextureSource::Texture)
                mode.fillPbr.albedoIndex = -1;
            if (mode.fillPbr.normalSource == FillPbrTextureSource::Texture)
                mode.fillPbr.normalIndex = -1;
            if (mode.fillPbr.occlusionSource == FillPbrTextureSource::Texture)
                mode.fillPbr.occlusionIndex = -1;
            if (mode.fillPbr.roughnessSource == FillPbrTextureSource::Texture)
                mode.fillPbr.roughnessIndex = -1;
        }
        revIt->second = revisions;
    }
}

RenderWidget::MeshRenderMode RenderWidget::renderModeForMesh(int meshIndex) const
{
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return MeshRenderMode {};
    const std::uint64_t meshId = m_doc->mesh(meshIndex).meshId;
    const auto it = m_meshRenderModes.find(meshId);
    if (it != m_meshRenderModes.end()) {
        MeshRenderMode mode = it->second;
        if (!(m_doc->mesh(meshIndex).ioMask & vcg::tri::io::Mask::IOM_EDGECOLOR))
            mode.edgeColorSource = EdgeColorSource::Constant;
        return mode;
    }
    return defaultRenderModeForMesh(meshIndex);
}

RenderWidget::MeshRenderMode *RenderWidget::mutableRenderModeForMesh(int meshIndex)
{
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return nullptr;
    const std::uint64_t meshId = m_doc->mesh(meshIndex).meshId;
    auto it = m_meshRenderModes.find(meshId);
    if (it == m_meshRenderModes.end()) {
        it = m_meshRenderModes
                 .emplace(meshId, defaultRenderModeForMesh(meshIndex))
                 .first;
    }
    return &it->second;
}

void RenderWidget::setCurrentMeshSettings(const PerMeshRenderSettings &next)
{
    const int meshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return;
    const std::uint64_t meshId = m_doc->mesh(meshIndex).meshId;
    m_meshRenderModes[meshId] = next;
    if (m_overlayPanel)
        m_overlayPanel->setMeshSettings(next);
}

void RenderWidget::showQualityVisualization(int meshIndex, bool faceQuality)
{
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return;

    const auto &entry = m_doc->mesh(meshIndex);
    const int mask = entry.ioMask;
    const bool hasVertexQuality = (mask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0;
    const bool hasFaceQuality = (mask & vcg::tri::io::Mask::IOM_FACEQUALITY) != 0;
    if ((faceQuality && !hasFaceQuality) || (!faceQuality && !hasVertexQuality))
        return;

    MeshRenderMode *mode = mutableRenderModeForMesh(meshIndex);
    if (!mode)
        return;

    if (faceQuality) {
        mode->showFill = true;
        mode->fillMaterial = FillMaterial::Plain;
        mode->fillPlain.colorSource = FillColorSource::PerFaceQuality;
    } else if (entry.mesh.FN() > 0) {
        mode->showFill = true;
        mode->fillMaterial = FillMaterial::Plain;
        mode->fillPlain.colorSource = FillColorSource::PerVertexQuality;
    } else {
        mode->showPoints = true;
        mode->pointColorSource = PointColorSource::PerVertexQuality;
    }

    if (meshIndex == m_doc->currentMeshIndex()) {
        refreshColorSourceAvailability();
        syncOverlaySettingsToCurrentMesh();
    }
    update();
}

void RenderWidget::showTextureVisualization(int meshIndex)
{
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return;
    const auto &entry = m_doc->mesh(meshIndex);
    const bool hasTexCoords = (entry.ioMask
        & (vcg::tri::io::Mask::IOM_WEDGTEXCOORD | vcg::tri::io::Mask::IOM_VERTTEXCOORD)) != 0;
    if (!hasTexCoords)
        return;

    MeshRenderMode *mode = mutableRenderModeForMesh(meshIndex);
    if (!mode)
        return;
    mode->showFill = true;
    mode->fillMaterial = FillMaterial::Plain;
    mode->fillPlain.colorSource = FillColorSource::Texture;

    if (meshIndex == m_doc->currentMeshIndex()) {
        refreshColorSourceAvailability();
        syncOverlaySettingsToCurrentMesh();
    }
    update();
}

void RenderWidget::syncOverlaySettingsToCurrentMesh()
{
    const int meshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    if (m_overlayPanel && meshIndex >= 0 && m_doc && meshIndex < m_doc->meshCount())
        m_overlayPanel->setMeshSettings(renderModeForMesh(meshIndex));
    else if (m_overlayPanel)
        m_overlayPanel->setMeshSettings(PerMeshRenderSettings{});
    updateBoundingBoxCornersOverlay();
}

void RenderWidget::refreshColorSourceAvailability()
{
    bool hasEdgeColors = false;
    bool hasVertexColors = false;
    bool hasFaceColors = false;
    bool hasVertexQuality = false;
    bool hasFaceQuality = false;
    bool hasTextures = false;
    bool hasVertexNormals = false;
    bool hasPerMeshColor = false;
    const int meshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    const bool hasCurrentMesh =
        m_doc && meshIndex >= 0 && meshIndex < m_doc->meshCount();
    if (hasCurrentMesh) {
        const auto &meshEntry = m_doc->mesh(meshIndex);
        const int mask = meshEntry.ioMask;
        const bool hasTextureCoords =
            (mask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0
            || (mask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0;
        hasEdgeColors = (mask & vcg::tri::io::Mask::IOM_EDGECOLOR) != 0;
        hasVertexColors = (mask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0;
        hasFaceColors = (mask & vcg::tri::io::Mask::IOM_FACECOLOR) != 0;
        hasVertexQuality = (mask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0;
        hasFaceQuality = (mask & vcg::tri::io::Mask::IOM_FACEQUALITY) != 0;
        hasTextures = hasTextureCoords && Document::meshTextureAssociationCount(meshEntry) > 0;
        hasVertexNormals = (mask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0;
        hasPerMeshColor = meshEntry.mesh.C()[0] != 0
                       || meshEntry.mesh.C()[1] != 0
                       || meshEntry.mesh.C()[2] != 0;
    }

    if (m_overlayPanel)
        m_overlayPanel->setEdgeColorSourceAvailability(hasEdgeColors);
    if (m_overlayPanel)
        m_overlayPanel->setPointColorSourceAvailability(hasVertexColors, hasVertexQuality);
    if (m_overlayPanel)
        m_overlayPanel->setPointLightingAvailability(hasVertexNormals);
    if (m_overlayPanel)
        m_overlayPanel->setFillColorSourceAvailability(
            hasVertexColors,
            hasFaceColors,
            hasVertexQuality,
            hasFaceQuality,
            hasTextures);
    if (m_overlayPanel)
        m_overlayPanel->setFillPbrMapAvailability(hasTextures, hasTextures, hasTextures);
    if (m_overlayPanel) {
        QStringList textureLabels;
        if (hasCurrentMesh)
            textureLabels = pbrTextureSelectorEntries(m_doc->mesh(meshIndex));
        m_overlayPanel->setFillPbrTextureNames(textureLabels);
    }

    // Correct per-mesh settings for the current mesh.
    PerMeshRenderSettings meshCorrected =
        hasCurrentMesh ? renderModeForMesh(meshIndex) : PerMeshRenderSettings{};
    if (meshCorrected.edgeColorSource == EdgeColorSource::PerEdge && !hasEdgeColors)
        meshCorrected.edgeColorSource = EdgeColorSource::Constant;
    if (meshCorrected.pointColorSource == PointColorSource::PerVertex && !hasVertexColors)
        meshCorrected.pointColorSource = PointColorSource::Constant;
    if (meshCorrected.pointColorSource == PointColorSource::PerVertexQuality && !hasVertexQuality)
        meshCorrected.pointColorSource = PointColorSource::Constant;
    if (meshCorrected.pointLighting && !hasVertexNormals)
        meshCorrected.pointLighting = false;
    if (meshCorrected.fillPlain.colorSource == FillColorSource::PerVertex && !hasVertexColors)
        meshCorrected.fillPlain.colorSource = FillColorSource::Constant;
    if (meshCorrected.fillPlain.colorSource == FillColorSource::PerFace && !hasFaceColors)
        meshCorrected.fillPlain.colorSource = FillColorSource::Constant;
    if (meshCorrected.fillPlain.colorSource == FillColorSource::PerVertexQuality && !hasVertexQuality)
        meshCorrected.fillPlain.colorSource = FillColorSource::Constant;
    if (meshCorrected.fillPlain.colorSource == FillColorSource::PerFaceQuality && !hasFaceQuality)
        meshCorrected.fillPlain.colorSource = FillColorSource::Constant;
    if (meshCorrected.fillPlain.colorSource == FillColorSource::Texture && !hasTextures)
        meshCorrected.fillPlain.colorSource = FillColorSource::Constant;
    if (meshCorrected.fillPlain.colorSource == FillColorSource::PerMesh && !hasPerMeshColor)
        meshCorrected.fillPlain.colorSource = FillColorSource::Constant;
    auto clampPbrSource = [hasTextures](FillPbrTextureSource &source, int &index) {
        if (!hasTextures && source == FillPbrTextureSource::Texture)
            source = FillPbrTextureSource::None;
        if (!hasTextures)
            index = -1;
    };
    clampPbrSource(meshCorrected.fillPbr.albedoSource, meshCorrected.fillPbr.albedoIndex);
    clampPbrSource(meshCorrected.fillPbr.normalSource, meshCorrected.fillPbr.normalIndex);
    clampPbrSource(meshCorrected.fillPbr.occlusionSource, meshCorrected.fillPbr.occlusionIndex);
    clampPbrSource(meshCorrected.fillPbr.roughnessSource, meshCorrected.fillPbr.roughnessIndex);
    if (meshCorrected.fillPbr.albedoSource == FillPbrTextureSource::Texture && !hasTextures) {
        meshCorrected.fillPbr.albedoSource = FillPbrTextureSource::Constant;
        meshCorrected.fillPbr.albedoIndex = -1;
    }
    const int textureCount =
        hasCurrentMesh
        ? Document::meshTextureAssociationCount(m_doc->mesh(meshIndex))
        : 0;
    auto clampTextureIndex = [textureCount](int &index) {
        if (index < -1 || index >= textureCount)
            index = -1;
    };
    clampTextureIndex(meshCorrected.fillPbr.albedoIndex);
    clampTextureIndex(meshCorrected.fillPbr.normalIndex);
    clampTextureIndex(meshCorrected.fillPbr.occlusionIndex);
    clampTextureIndex(meshCorrected.fillPbr.roughnessIndex);
    clampTextureIndex(meshCorrected.fillPlain.textureIndex);

    if (hasCurrentMesh && meshCorrected != renderModeForMesh(meshIndex)) {
        const std::uint64_t meshId = m_doc->mesh(meshIndex).meshId;
        m_meshRenderModes[meshId] = meshCorrected;
        if (m_overlayPanel)
            m_overlayPanel->setMeshSettings(meshCorrected);
    } else if (!hasCurrentMesh && m_overlayPanel) {
        m_overlayPanel->setMeshSettings(meshCorrected);
    }

}

int RenderWidget::fillGpuVariantIndexForSettings(const PerMeshRenderSettings &settings) const
{
    // PBR relies on UV/material textures for normal/occlusion/roughness maps regardless
    // of whether albedo comes from texture or a plain color.
    FillColorSource source = (settings.fillMaterial == FillMaterial::Pbr)
        ? FillColorSource::Texture
        : settings.fillPlain.colorSource;
    switch (source) {
    case FillColorSource::PerVertex: return static_cast<int>(Document::FillGpuVariant::PerVertex);
    case FillColorSource::PerFace: return static_cast<int>(Document::FillGpuVariant::PerFace);
    case FillColorSource::PerVertexQuality:
        return static_cast<int>(Document::FillGpuVariant::PerVertexQuality);
    case FillColorSource::PerFaceQuality:
        return static_cast<int>(Document::FillGpuVariant::PerFaceQuality);
    case FillColorSource::Texture: return static_cast<int>(Document::FillGpuVariant::Texture);
    case FillColorSource::PerMesh:
    case FillColorSource::Constant:
    default:
        return static_cast<int>(Document::FillGpuVariant::Constant);
    }
}

int RenderWidget::pointGpuVariantIndexForSettings(const PerMeshRenderSettings &settings) const
{
    switch (settings.pointColorSource) {
    case PointColorSource::PerVertex: return static_cast<int>(Document::PointGpuVariant::PerVertex);
    case PointColorSource::PerVertexQuality:
        return static_cast<int>(Document::PointGpuVariant::PerVertexQuality);
    case PointColorSource::Constant:
    default:
        return static_cast<int>(Document::PointGpuVariant::Constant);
    }
}

QRhiTexture *RenderWidget::resolveSelectedPbrTexture(
    int meshIndex,
    int textureIndex,
    const MeshGpuResourceCache::FillPassView &fillView) const
{
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount() || !fillView.valid)
        return nullptr;
    if (textureIndex < 0) {
        return nullptr;
    }

    const auto &meshEntry = m_doc->mesh(meshIndex);
    if (textureIndex >= Document::meshTextureAssociationCount(meshEntry))
        return nullptr;

    const QString wantedPath = normalizeTexturePath(Document::meshTextureSourcePath(meshEntry, textureIndex));

    for (int bi = 0; bi < fillView.batchCount; ++bi) {
        const auto &batch = fillView.batches[bi];
        if (wantedPath.isEmpty() && batch.textureGroupIndex == textureIndex && batch.baseColorTexture)
            return batch.baseColorTexture;
        const QString basePath = normalizeTexturePath(batch.baseColorTexturePath);
        const QString normalPath = normalizeTexturePath(batch.normalTexturePath);
        const QString occlusionPath = normalizeTexturePath(batch.occlusionTexturePath);
        const QString roughnessPath = normalizeTexturePath(batch.roughnessTexturePath);
        if (batch.baseColorTexture && !basePath.isEmpty() && basePath == wantedPath)
            return batch.baseColorTexture;
        if (batch.normalTexture && !normalPath.isEmpty() && normalPath == wantedPath)
            return batch.normalTexture;
        if (batch.occlusionTexture && !occlusionPath.isEmpty() && occlusionPath == wantedPath)
            return batch.occlusionTexture;
        if (batch.roughnessTexture && !roughnessPath.isEmpty() && roughnessPath == wantedPath)
            return batch.roughnessTexture;
    }
    return nullptr;
}

// Returns the vertex and fragment shader resource paths for a given material/shading combination.
// Each fill shader pair must share the same SRB layout (5 texture samplers + shared UBO).
static std::pair<QString, QString> fillShaderPaths(FillMaterial material, FillShading shading)
{
    switch (material) {
    case FillMaterial::RadianceScaling:
        // RS always uses smooth per-vertex normals; the vert shader is shared with plain/smooth.
        return { QStringLiteral(":/shaders/fill_smooth.vert.qsb"),
                 QStringLiteral(":/shaders/fill_radscale.frag.qsb") };
    default:
        break;
    }
    // Plain and PBR are differentiated purely by UBO data; they share the same shader pairs.
    if (shading == FillShading::Flat)
        return { QStringLiteral(":/shaders/fill_flat.vert.qsb"),
                 QStringLiteral(":/shaders/fill_flat.frag.qsb") };
    return { QStringLiteral(":/shaders/fill_smooth.vert.qsb"),
             QStringLiteral(":/shaders/fill_smooth.frag.qsb") };
}

QRhiGraphicsPipeline *RenderWidget::fillPipelineForSettings(const PerMeshRenderSettings &settings)
{
    if (!m_rhi || !m_srb || !renderTarget())
        return nullptr;

    // PBR has its own per-material shading setting; RS always needs smooth normals.
    FillShading effectiveShading;
    switch (settings.fillMaterial) {
    case FillMaterial::Pbr:             effectiveShading = settings.fillPbr.shading; break;
    case FillMaterial::RadianceScaling: effectiveShading = FillShading::Smooth;      break;
    default: /* Plain */                effectiveShading = settings.fillPlain.shading;   break;
    }

    // Key encodes: material * 4 + shading * 2 + backfaceCulling
    const int key = int(settings.fillMaterial) * 4
                  + int(effectiveShading) * 2
                  + (settings.fillBackfaceCulling ? 1 : 0);
    auto it = m_fillPipelinesByKey.find(key);
    if (it != m_fillPipelinesByKey.end())
        return it->second.get();

    auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(m_rhi->newGraphicsPipeline());
    const auto [vsPath, fsPath] = fillShaderPaths(settings.fillMaterial, effectiveShading);
    QShader vs = loadShader(vsPath);
    QShader fs = loadShader(fsPath);
    if (!vs.isValid() || !fs.isValid())
        return nullptr;

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs }
    });
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(true);
    pipeline->setCullMode(
        settings.fillBackfaceCulling
            ? QRhiGraphicsPipeline::Back
            : QRhiGraphicsPipeline::None);

    // Flat layout has no per-vertex normal attribute; RS always uses the smooth layout.
    const bool useFlatLayout = (effectiveShading == FillShading::Flat);
    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ { kFillVertexStrideFloats * sizeof(float) } });
    if (useFlatLayout) {
        inputLayout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float4, 6 * sizeof(float) },
            { 0, 2, QRhiVertexInputAttribute::Float3, 10 * sizeof(float) }
        });
    } else {
        inputLayout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },
            { 0, 2, QRhiVertexInputAttribute::Float4, 6 * sizeof(float) },
            { 0, 3, QRhiVertexInputAttribute::Float3, 10 * sizeof(float) }
        });
    }
    pipeline->setVertexInputLayout(inputLayout);
    pipeline->setShaderResourceBindings(m_srb.get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create())
        return nullptr;

    auto inserted = m_fillPipelinesByKey.emplace(key, std::move(pipeline));
    return inserted.first->second.get();
}

QRhiGraphicsPipeline *RenderWidget::wirePipelineForSettings(const PerMeshRenderSettings &settings)
{
    if (!m_rhi || !m_srb || !renderTarget())
        return nullptr;
    const int key = settings.wireBackfaceCulling ? 1 : 0;
    auto it = m_wirePipelinesByKey.find(key);
    if (it != m_wirePipelinesByKey.end())
        return it->second.get();

    auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(m_rhi->newGraphicsPipeline());
    QShader vs = loadShader(QStringLiteral(":/shaders/fill_wire.vert.qsb"));
    QShader fs = loadShader(QStringLiteral(":/shaders/fill_wire_overlay.frag.qsb"));
    if (!vs.isValid() || !fs.isValid())
        return nullptr;

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs }
    });
    pipeline->setDepthTest(true);
    pipeline->setDepthWrite(false);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setCullMode(
        settings.wireBackfaceCulling
            ? QRhiGraphicsPipeline::Back
            : QRhiGraphicsPipeline::None);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opColor = QRhiGraphicsPipeline::Add;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opAlpha = QRhiGraphicsPipeline::Add;
    pipeline->setTargetBlends({ blend });

    QRhiVertexInputLayout wireLayout;
    wireLayout.setBindings({ { 6 * sizeof(float) } });
    wireLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) }
    });
    pipeline->setVertexInputLayout(wireLayout);
    pipeline->setShaderResourceBindings(m_srb.get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create())
        return nullptr;

    auto inserted = m_wirePipelinesByKey.emplace(key, std::move(pipeline));
    return inserted.first->second.get();
}

QRhiGraphicsPipeline *RenderWidget::edgesPipelineForSettings(const PerMeshRenderSettings &settings)
{
    if (!m_rhi || !m_srb || !renderTarget())
        return nullptr;
    const bool colored = settings.edgeColorSource == EdgeColorSource::PerEdge;
    const int key = int(std::lround(qMax(1.0f, settings.edgeSize) * 10.0f)) * 4
        + int(colored) + 2 * int(settings.showPoints);
    auto it = m_edgesPipelinesByKey.find(key);
    if (it != m_edgesPipelinesByKey.end())
        return it->second.get();

    auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(m_rhi->newGraphicsPipeline());
    QShader vs = loadShader(colored ? QStringLiteral(":/shaders/overlay_edges_colored.vert.qsb")
                                    : QStringLiteral(":/shaders/overlay_edges.vert.qsb"));
    QShader fs = loadShader(colored ? QStringLiteral(":/shaders/overlay_edges_colored.frag.qsb")
                                    : QStringLiteral(":/shaders/overlay_edges.frag.qsb"));
    if (!vs.isValid() || !fs.isValid())
        return nullptr;

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs }
    });
    pipeline->setTopology(QRhiGraphicsPipeline::Lines);
    pipeline->setDepthTest(true);
    // Points are drawn afterwards: avoid biased edge depth hiding their centers.
    // Depth testing still lets foreground surfaces occlude the edges and points.
    pipeline->setDepthWrite(!settings.showPoints);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setDepthBias(-1);
    pipeline->setSlopeScaledDepthBias(-1.0f);
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    pipeline->setLineWidth(qMax(1.0f, settings.edgeSize));
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opColor = QRhiGraphicsPipeline::Add;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opAlpha = QRhiGraphicsPipeline::Add;
    pipeline->setTargetBlends({ blend });

    QRhiVertexInputLayout edgesLayout;
    const int stride = colored ? LineRenderer::kColoredLineVertexStrideFloats : 3;
    edgesLayout.setBindings({ { quint32(stride * sizeof(float)) } });
    if (colored) {
        edgesLayout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float4, 3 * sizeof(float) }
        });
    } else {
        edgesLayout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
    }
    pipeline->setVertexInputLayout(edgesLayout);
    pipeline->setShaderResourceBindings(m_srb.get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create())
        return nullptr;

    auto inserted = m_edgesPipelinesByKey.emplace(key, std::move(pipeline));
    return inserted.first->second.get();
}

QRhiGraphicsPipeline *RenderWidget::fatEdgesPipelineForSettings(const PerMeshRenderSettings &settings)
{
    if (!m_rhi || !m_srb || !renderTarget())
        return nullptr;
    const bool colored = settings.edgeColorSource == EdgeColorSource::PerEdge;
    const int key = int(std::lround(qMax(1.0f, settings.edgeSize) * 10.0f)) * 4
        + int(colored) + 2 * int(settings.showPoints);
    auto it = m_fatEdgesPipelinesByKey.find(key);
    if (it != m_fatEdgesPipelinesByKey.end())
        return it->second.get();

    auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(m_rhi->newGraphicsPipeline());
    QShader vs = loadShader(colored ? QStringLiteral(":/shaders/overlay_fat_edges_colored.vert.qsb")
                                    : QStringLiteral(":/shaders/overlay_fat_edges.vert.qsb"));
    QShader fs = loadShader(colored ? QStringLiteral(":/shaders/overlay_fat_edges_colored.frag.qsb")
                                    : QStringLiteral(":/shaders/overlay_fat_edges.frag.qsb"));
    if (!vs.isValid() || !fs.isValid())
        return nullptr;

    pipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs }
    });
    pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    pipeline->setDepthTest(true);
    // Points are drawn afterwards: avoid biased edge depth hiding their centers.
    // Depth testing still lets foreground surfaces occlude the edges and points.
    pipeline->setDepthWrite(!settings.showPoints);
    pipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
    pipeline->setDepthBias(-1);
    pipeline->setSlopeScaledDepthBias(-1.0f);
    pipeline->setCullMode(QRhiGraphicsPipeline::None);
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opColor = QRhiGraphicsPipeline::Add;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.opAlpha = QRhiGraphicsPipeline::Add;
    pipeline->setTargetBlends({ blend });

    QRhiVertexInputLayout edgesLayout;
    const int stride = colored ? LineRenderer::kColoredFatLineStrideFloats
                               : LineRenderer::kFatLineStrideFloats;
    edgesLayout.setBindings({ { quint32(stride * sizeof(float)) } });
    if (colored) {
        edgesLayout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },
            { 0, 2, QRhiVertexInputAttribute::Float, 6 * sizeof(float) },
            { 0, 3, QRhiVertexInputAttribute::Float, 7 * sizeof(float) },
            { 0, 4, QRhiVertexInputAttribute::Float4, 8 * sizeof(float) }
        });
    } else {
        edgesLayout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
            { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },
            { 0, 2, QRhiVertexInputAttribute::Float, 6 * sizeof(float) },
            { 0, 3, QRhiVertexInputAttribute::Float, 7 * sizeof(float) }
        });
    }
    pipeline->setVertexInputLayout(edgesLayout);
    pipeline->setShaderResourceBindings(m_srb.get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create())
        return nullptr;

    auto inserted = m_fatEdgesPipelinesByKey.emplace(key, std::move(pipeline));
    return inserted.first->second.get();
}

QRhiShaderResourceBindings *RenderWidget::shaderResourcesForFillTextures(
    QRhiTexture *baseColorTexture,
    QRhiTexture *normalTexture,
    QRhiTexture *occlusionTexture,
    QRhiTexture *roughnessTexture,
    bool nearest)
{
    if (!m_rhi
        || !m_ubuf
        || !m_textureSampler
        || !m_textureSamplerNearest
        || !m_qualityColorMapSampler
        || !m_qualityColorMapTexture
        || !m_fallbackTexture
        || !m_fallbackNormalTexture
        || !m_fallbackOcclusionTexture
        || !m_fallbackRoughnessTexture) {
        return m_srb.get();
    }

    QRhiTexture *resolvedBase = baseColorTexture ? baseColorTexture : m_fallbackTexture.get();
    QRhiTexture *resolvedNormal = normalTexture ? normalTexture : m_fallbackNormalTexture.get();
    QRhiTexture *resolvedOcclusion =
        occlusionTexture ? occlusionTexture : m_fallbackOcclusionTexture.get();
    QRhiTexture *resolvedRoughness =
        roughnessTexture ? roughnessTexture : m_fallbackRoughnessTexture.get();

    FillTextureSetKey key;
    key.baseColorTexture = resolvedBase;
    key.normalTexture = resolvedNormal;
    key.occlusionTexture = resolvedOcclusion;
    key.roughnessTexture = resolvedRoughness;
    key.nearest = nearest;
    auto it = m_textureSrbs.find(key);
    if (it != m_textureSrbs.end())
        return it->second.get();

    QRhiSampler *sampler = nearest ? m_textureSamplerNearest.get() : m_textureSampler.get();

    auto textureSrb =
        std::unique_ptr<QRhiShaderResourceBindings>(m_rhi->newShaderResourceBindings());
    textureSrb->setBindings({
        QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
            0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_ubuf.get(),
            kUbufSize),
        QRhiShaderResourceBinding::sampledTexture(
            1,
            QRhiShaderResourceBinding::FragmentStage,
            resolvedBase,
            sampler),
        QRhiShaderResourceBinding::sampledTexture(
            2,
            QRhiShaderResourceBinding::FragmentStage,
            m_qualityColorMapTexture.get(),
            m_qualityColorMapSampler.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3,
            QRhiShaderResourceBinding::FragmentStage,
            resolvedNormal,
            sampler),
        QRhiShaderResourceBinding::sampledTexture(
            4,
            QRhiShaderResourceBinding::FragmentStage,
            resolvedOcclusion,
            sampler),
        QRhiShaderResourceBinding::sampledTexture(
            5,
            QRhiShaderResourceBinding::FragmentStage,
            resolvedRoughness,
            sampler)
    });
    if (!textureSrb->create())
        return m_srb.get();

    QRhiShaderResourceBindings *raw = textureSrb.get();
    m_textureSrbs.emplace(key, std::move(textureSrb));
    return raw;
}

QRhiShaderResourceBindings *RenderWidget::shaderResourcesForTexture(QRhiTexture *texture)
{
    return shaderResourcesForFillTextures(texture, nullptr, nullptr, nullptr);
}
