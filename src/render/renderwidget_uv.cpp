#include "textureassociationutils.h"
#include "renderwidget.h"
#include "colormap.h"
#include "document.h"
#include "qualityrange.h"
#include "renderwidget_internal.h"
#include <wrap/io_trimesh/io_mask.h>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace RenderWidgetInternal;

namespace {
constexpr int kUvBackgroundUbufSize = 64;
constexpr float kUvMinZoom = 0.05f;
constexpr float kUvMaxZoom = 5000.0f;

std::vector<int> usedTextureGroups(const Document::MeshEntry &entry)
{
    std::vector<int> groups;
    for (const VCGFace &face : entry.mesh.face) {
        if (!face.IsD())
            groups.push_back(vcgFaceTextureGroup(entry.ioMask, face));
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    if (groups.empty())
        groups.push_back(0);
    return groups;
}
}

void RenderWidget::syncUvCacheWithDocument()
{
    if (!m_doc) {
        m_uvMeshGpu.clear();
        return;
    }

    std::unordered_map<std::uint64_t, bool> aliveMeshIds;
    aliveMeshIds.reserve(size_t(m_doc->meshCount()));
    for (int i = 0; i < m_doc->meshCount(); ++i)
        aliveMeshIds.emplace(m_doc->mesh(i).meshId, true);

    for (auto it = m_uvMeshGpu.begin(); it != m_uvMeshGpu.end();) {
        if (aliveMeshIds.find(it->first) == aliveMeshIds.end())
            it = m_uvMeshGpu.erase(it);
        else
            ++it;
    }
    for (auto it = m_uvTextureGroupByMesh.begin(); it != m_uvTextureGroupByMesh.end();) {
        if (aliveMeshIds.find(it->first) == aliveMeshIds.end())
            it = m_uvTextureGroupByMesh.erase(it);
        else
            ++it;
    }
}

int RenderWidget::activeUvTextureGroup() const
{
    const int meshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return 0;
    const auto it = m_uvTextureGroupByMesh.find(m_doc->mesh(meshIndex).meshId);
    return it == m_uvTextureGroupByMesh.end() ? 0 : std::max(0, it->second);
}

void RenderWidget::layoutUvTextureGroupUi()
{
    if (!m_uvTextureGroupList || !m_uvTextureGroupList->isVisible())
        return;
    constexpr int margin = 8;
    const int contentWidth = m_uvTextureGroupList->count() * 76 + 8;
    const int w = std::min(std::max(100, contentWidth), std::max(100, width() - 2 * margin));
    m_uvTextureGroupList->setGeometry(
        (width() - w) / 2, height() - m_uvTextureGroupList->height() - margin,
        w, m_uvTextureGroupList->height());
    m_uvTextureGroupList->raise();
}

void RenderWidget::syncUvTextureGroupUi()
{
    if (!m_uvTextureGroupList)
        return;
    const int meshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    if (m_viewMode != ViewMode::ParametrizationUV
        || !m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount()) {
        m_uvTextureGroupList->hide();
        return;
    }

    const auto &entry = m_doc->mesh(meshIndex);
    const bool rebuild =
        m_uvTextureGroupUiMeshId != entry.meshId
        || m_uvTextureGroupUiGeometryRevision != entry.geometryRevision
        || m_uvTextureGroupUiMaterialRevision != entry.materialRevision;
    const std::vector<int> groups = rebuild
        ? usedTextureGroups(entry)
        : std::vector<int>{};
    const int groupCount = rebuild ? int(groups.size()) : m_uvTextureGroupList->count();
    if (groupCount <= 1) {
        m_uvTextureGroupList->hide();
        if (rebuild)
            m_uvTextureGroupList->clear();
        m_uvTextureGroupByMesh[entry.meshId] =
            rebuild ? groups.front() : activeUvTextureGroup();
        m_uvTextureGroupUiMeshId = entry.meshId;
        m_uvTextureGroupUiGeometryRevision = entry.geometryRevision;
        m_uvTextureGroupUiMaterialRevision = entry.materialRevision;
        return;
    }

    int &activeGroup = m_uvTextureGroupByMesh[entry.meshId];
    if (rebuild && std::find(groups.begin(), groups.end(), activeGroup) == groups.end())
        activeGroup = groups.front();
    if (rebuild || m_uvTextureGroupList->count() != groupCount) {
        m_uvTextureGroupList->clear();
        for (const int group : groups) {
            int textureIndex = group;
            if (group < int(entry.materialSet.entries.size())) {
                const QString wanted = normalizeTexturePath(
                    entry.materialSet.entries[size_t(group)].baseColorTexture.filePath);
                if (!wanted.isEmpty()) {
                    for (int i = 0; i < Document::meshTextureAssociationCount(entry); ++i) {
                        if (normalizeTexturePath(Document::meshTextureSourcePath(entry, i)) == wanted) {
                            textureIndex = i;
                            break;
                        }
                    }
                }
            }

            // loadAssociatedTextureImage owns the whole resolution order -- embedded
            // asset first, then the backing file through readImageFile's fallbacks.
            QImage image;
            QString imageError;
            TextureAssociationUtils::loadAssociatedTextureImage(
                entry, textureIndex, image, imageError);
            QPixmap thumbnail(64, 64);
            thumbnail.fill(QColor(80, 80, 82));
            if (!image.isNull())
                thumbnail = QPixmap::fromImage(image).scaled(
                    64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);

            auto *item = new QListWidgetItem(
                QIcon(thumbnail), tr("%1").arg(group), m_uvTextureGroupList);
            item->setData(Qt::UserRole, group);
            const QString name = Document::meshTextureDisplayName(entry, textureIndex);
            item->setToolTip(
                image.isNull()
                    ? tr("Texture group %1 — %2").arg(group).arg(name)
                    : tr("Texture group %1 — %2 (%3×%4)")
                          .arg(group).arg(name).arg(image.width()).arg(image.height()));
            if (group == activeGroup)
                item->setSelected(true);
        }
        m_uvTextureGroupUiMeshId = entry.meshId;
        m_uvTextureGroupUiGeometryRevision = entry.geometryRevision;
        m_uvTextureGroupUiMaterialRevision = entry.materialRevision;
    }
    m_uvTextureGroupList->show();
    layoutUvTextureGroupUi();
}

bool RenderWidget::meshHasParametrization(int meshIndex) const
{
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return false;

    const auto &entry = m_doc->mesh(meshIndex);
    if (entry.mesh.FN() <= 0)
        return false;
    const int mask = entry.ioMask;
    const bool hasWedgeTex = (mask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0;
    const bool hasVertexTex = (mask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0;
    return hasWedgeTex || hasVertexTex;
}

bool RenderWidget::ensureUvMeshResources(int meshIndex, QRhiCommandBuffer *cb)
{
    if (!m_doc || !m_rhi || !cb || meshIndex < 0 || meshIndex >= m_doc->meshCount())
        return false;

    const auto &entry = m_doc->mesh(meshIndex);
    auto &gpu = m_uvMeshGpu[entry.meshId];
    const ColorMapRegistry &registry = ColorMapRegistry::instance();
    QString qualityColorMapName = m_renderSettings.qualityHistogramColorMapId.trimmed().toLower();
    if (qualityColorMapName.isEmpty() || !registry.hasMap(qualityColorMapName))
        qualityColorMapName = registry.fallbackMapId();
    const bool qualityColorMapInverted = m_renderSettings.qualityHistogramInvertColorMap;
    float qualityRangeMin = m_renderSettings.qualityHistogramMin;
    float qualityRangeMax = m_renderSettings.qualityHistogramMax;
    if (qualityRangeMin > qualityRangeMax)
        std::swap(qualityRangeMin, qualityRangeMax);
    const bool qualityFixedRange =
        m_renderSettings.qualityHistogramFixedRange
        && std::isfinite(qualityRangeMin)
        && std::isfinite(qualityRangeMax);
    const bool qualityCenterOnZero =
        !qualityFixedRange && m_renderSettings.qualityHistogramCenterOnZero;
    const float qualityPercentileCrop = qualityFixedRange
        ? 0.0f
        : std::clamp(m_renderSettings.qualityHistogramPercentileCrop, 0.0f, 0.5f);
    if (gpu.valid
        && gpu.geometryRevision == entry.geometryRevision
        && gpu.materialRevision == entry.materialRevision
        && gpu.selectionRevision == entry.selectionRevision
        && gpu.qualityColorMapId == qualityColorMapName
        && gpu.qualityColorMapInverted == qualityColorMapInverted
        && gpu.qualityFixedRange == qualityFixedRange
        && (!qualityFixedRange
            || (gpu.qualityRangeMin == qualityRangeMin
                && gpu.qualityRangeMax == qualityRangeMax))
        && (qualityFixedRange
            || (gpu.qualityCenterOnZero == qualityCenterOnZero
                && gpu.qualityPercentileCrop == qualityPercentileCrop))) {
        return true;
    }

    gpu = UvMeshGpu {};
    gpu.geometryRevision = entry.geometryRevision;
    gpu.materialRevision = entry.materialRevision;
    gpu.selectionRevision = entry.selectionRevision;
    gpu.qualityColorMapId = qualityColorMapName;
    gpu.qualityColorMapInverted = qualityColorMapInverted;
    gpu.qualityFixedRange = qualityFixedRange;
    gpu.qualityRangeMin = qualityRangeMin;
    gpu.qualityRangeMax = qualityRangeMax;
    gpu.qualityCenterOnZero = qualityCenterOnZero;
    gpu.qualityPercentileCrop = qualityPercentileCrop;
    if (!meshHasParametrization(meshIndex))
        return false;

    const VCGMesh &mesh = entry.mesh;
    const int mask = entry.ioMask;
    const bool hasVertexColors = (mask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0;
    const bool hasFaceColors = (mask & vcg::tri::io::Mask::IOM_FACECOLOR) != 0;
    const bool hasVertexQuality = (mask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0;
    const bool hasFaceQuality = (mask & vcg::tri::io::Mask::IOM_FACEQUALITY) != 0;
    const std::vector<int> textureGroups = usedTextureGroups(entry);
    const int groupCount = textureGroups.back() + 1;
    auto faceGroup = [mask, groupCount](const VCGFace &f) {
        return std::clamp(vcgFaceTextureGroup(mask, f), 0, groupCount - 1);
    };

    auto uvForCorner = [&](const VCGMesh::FaceType &f, int corner, QVector2D &outUv) -> bool {
        float u = 0.0f;
        float v = 0.0f;
        if (!vcgFaceCornerUV(mask, f, corner, u, v))
            return false;
        outUv = QVector2D(u, v);
        return true;
    };

    std::vector<std::vector<float>> wireData;
    std::vector<std::vector<float>> boundaryEdgeData;
    std::vector<std::vector<float>> textureSeamData;
    wireData.resize(size_t(groupCount));
    boundaryEdgeData.resize(size_t(groupCount));
    textureSeamData.resize(size_t(groupCount));
    std::array<std::vector<std::vector<float>>, 5> fillData;
    for (auto &v : fillData)
        v.resize(size_t(groupCount));
    std::array<std::vector<std::vector<float>>, 3> pointsData;
    for (auto &v : pointsData)
        v.resize(size_t(groupCount));

    std::vector<size_t> groupFaceCounts(size_t(groupCount), 0);
    for (int fi = 0; fi < mesh.FN(); ++fi) {
        const auto &f = mesh.face[fi];
        if (!f.IsD())
            ++groupFaceCounts[size_t(faceGroup(f))];
    }
    for (size_t group = 0; group < groupFaceCounts.size(); ++group) {
        const size_t faces = groupFaceCounts[group];
        wireData[group].reserve(faces * 18);
        boundaryEdgeData[group].reserve(faces * 6);
        textureSeamData[group].reserve(faces * 6);
        for (auto &variant : fillData)
            variant[group].reserve(faces * 3 * kFillVertexStrideFloats);
        for (auto &variant : pointsData)
            variant[group].reserve(faces * 3 * kPointsVertexStrideFloats);
    }

    QVector2D minUv(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    QVector2D maxUv(-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max());
    bool hasUv = false;

    auto includeUvBounds = [&](const QVector2D &uv) {
        minUv.setX(std::min(minUv.x(), uv.x()));
        minUv.setY(std::min(minUv.y(), uv.y()));
        maxUv.setX(std::max(maxUv.x(), uv.x()));
        maxUv.setY(std::max(maxUv.y(), uv.y()));
        hasUv = true;
    };

    auto appendFillVertex = [&](std::vector<float> &dst,
                                const QVector2D &uv,
                                const QVector3D &meshColor,
                                float useMeshColorFlag,
                                const QVector3D &texInfo) {
        dst.push_back(uv.x());
        dst.push_back(uv.y());
        dst.push_back(0.0f);
        dst.push_back(0.0f);
        dst.push_back(0.0f);
        dst.push_back(1.0f);
        dst.push_back(meshColor.x());
        dst.push_back(meshColor.y());
        dst.push_back(meshColor.z());
        dst.push_back(useMeshColorFlag);
        dst.push_back(texInfo.x());
        dst.push_back(texInfo.y());
        dst.push_back(texInfo.z());
    };

    auto appendPointVertex = [&](std::vector<float> &dst,
                                 const QVector2D &uv,
                                 const QVector3D &meshColor,
                                 float useMeshColorFlag) {
        dst.push_back(uv.x());
        dst.push_back(uv.y());
        dst.push_back(0.0f);
        dst.push_back(meshColor.x());
        dst.push_back(meshColor.y());
        dst.push_back(meshColor.z());
        dst.push_back(useMeshColorFlag);
        dst.push_back(0.0f);
        dst.push_back(0.0f);
        dst.push_back(1.0f);
        dst.push_back(0.0f);
    };

    const ColorMapDefinition *qualityMapDef = registry.definition(qualityColorMapName);
    auto qualityColorMap = [&registry, qualityMapDef, qualityColorMapInverted](float t) {
        if (qualityColorMapInverted)
            t = 1.0f - t;
        return registry.sampleRgb(qualityMapDef, t);
    };

    auto collectVertexQuality = [&]() {
        std::vector<float> values;
        values.reserve(size_t(mesh.VN()));
        for (int vi = 0; vi < mesh.VN(); ++vi) {
            const auto &v = mesh.vert[vi];
            if (!v.IsD())
                values.push_back(static_cast<float>(v.cQ()));
        }
        return values;
    };
    auto collectFaceQuality = [&]() {
        std::vector<float> values;
        values.reserve(size_t(mesh.FN()));
        for (int fi = 0; fi < mesh.FN(); ++fi) {
            const auto &f = mesh.face[fi];
            if (!f.IsD())
                values.push_back(static_cast<float>(f.cQ()));
        }
        return values;
    };

    RenderQualityRange vertexQualityRange;
    if (hasVertexQuality) {
        if (qualityFixedRange) {
            vertexQualityRange = fixedRenderQualityRange(qualityRangeMin, qualityRangeMax);
        } else {
            vertexQualityRange = sampledRenderQualityRange(
                collectVertexQuality(),
                qualityCenterOnZero,
                qualityPercentileCrop);
        }
    }

    RenderQualityRange faceQualityRange;
    if (hasFaceQuality) {
        if (qualityFixedRange) {
            faceQualityRange = fixedRenderQualityRange(qualityRangeMin, qualityRangeMax);
        } else {
            faceQualityRange = sampledRenderQualityRange(
                collectFaceQuality(),
                qualityCenterOnZero,
                qualityPercentileCrop);
        }
    }

    for (int fi = 0; fi < mesh.FN(); ++fi) {
        const auto &f = mesh.face[fi];
        if (f.IsD())
            continue;

        QVector2D uv[3];
        bool validFaceUv = true;
        for (int c = 0; c < 3; ++c) {
            if (!uvForCorner(f, c, uv[c])) {
                validFaceUv = false;
                break;
            }
        }
        if (!validFaceUv)
            continue;
        const size_t group = size_t(faceGroup(f));

        const auto fc = f.cC();
        const QVector3D faceColor(
            float(fc[0]) / 255.0f,
            float(fc[1]) / 255.0f,
            float(fc[2]) / 255.0f);
        const float useFaceColorFlag = hasFaceColors ? 1.0f : 0.0f;
        const QVector3D faceQualityColor = qualityColorMap(
            normalizedRenderQuality(static_cast<float>(f.cQ()), faceQualityRange));
        const float useFaceQualityFlag = hasFaceQuality ? 1.0f : 0.0f;

        for (int c = 0; c < 3; ++c) {
            includeUvBounds(uv[c]);
            const int n = (c + 1) % 3;
            wireData[group].push_back(uv[c].x());
            wireData[group].push_back(uv[c].y());
            wireData[group].push_back(0.0f);
            wireData[group].push_back(uv[n].x());
            wireData[group].push_back(uv[n].y());
            wireData[group].push_back(0.0f);

            const auto *vertex = f.cV(c);
            QVector3D vertexColor(1.0f, 1.0f, 1.0f);
            float useVertexColorFlag = 0.0f;
            if (vertex && hasVertexColors) {
                const auto vc = vertex->cC();
                vertexColor = QVector3D(
                    float(vc[0]) / 255.0f,
                    float(vc[1]) / 255.0f,
                    float(vc[2]) / 255.0f);
                useVertexColorFlag = 1.0f;
            }
            QVector3D vertexQualityColor(1.0f, 1.0f, 1.0f);
            float useVertexQualityFlag = 0.0f;
            if (vertex && hasVertexQuality) {
                vertexQualityColor = qualityColorMap(
                    normalizedRenderQuality(static_cast<float>(vertex->cQ()), vertexQualityRange));
                useVertexQualityFlag = 1.0f;
            }

            appendFillVertex(
                fillData[0][group], uv[c], QVector3D(1.0f, 1.0f, 1.0f), 0.0f, QVector3D(0.0f, 0.0f, 0.0f));
            appendFillVertex(
                fillData[1][group], uv[c], vertexColor, useVertexColorFlag, QVector3D(0.0f, 0.0f, 0.0f));
            appendFillVertex(
                fillData[2][group], uv[c], faceColor, useFaceColorFlag, QVector3D(0.0f, 0.0f, 0.0f));
            appendFillVertex(
                fillData[3][group],
                uv[c],
                vertexQualityColor,
                useVertexQualityFlag,
                QVector3D(0.0f, 0.0f, 0.0f));
            appendFillVertex(
                fillData[4][group],
                uv[c],
                faceQualityColor,
                useFaceQualityFlag,
                QVector3D(0.0f, 0.0f, 0.0f));

            appendPointVertex(pointsData[0][group], uv[c], QVector3D(1.0f, 1.0f, 1.0f), 0.0f);
            appendPointVertex(pointsData[1][group], uv[c], vertexColor, useVertexColorFlag);
            appendPointVertex(pointsData[2][group], uv[c], vertexQualityColor, useVertexQualityFlag);
        }
    }

    if (!hasUv)
        return false;

    struct UvEdgeSample {
        QVector2D uvA;
        QVector2D uvB;
        int group = 0;
    };
    std::unordered_map<std::uint64_t, std::vector<UvEdgeSample>> edgeSamples;
    edgeSamples.reserve(size_t(mesh.FN()) * 3);

    for (int fi = 0; fi < mesh.FN(); ++fi) {
        const auto &f = mesh.face[fi];
        if (f.IsD())
            continue;
        for (int i = 0; i < 3; ++i) {
            const int next = (i + 1) % 3;
            QVector2D uv0;
            QVector2D uv1;
            if (!uvForCorner(f, i, uv0) || !uvForCorner(f, next, uv1))
                continue;

            const auto *v0 = f.cV(i);
            const auto *v1 = f.cV(next);
            if (!v0 || !v1)
                continue;
            int a = vcg::tri::Index(mesh, v0);
            int b = vcg::tri::Index(mesh, v1);
            if (a < 0 || b < 0)
                continue;

            if (a > b) {
                std::swap(a, b);
                std::swap(uv0, uv1);
            }

            const std::uint64_t key =
                (std::uint64_t(std::uint32_t(a)) << 32) | std::uint64_t(std::uint32_t(b));
            edgeSamples[key].push_back(UvEdgeSample { uv0, uv1, faceGroup(f) });
        }
    }

    const float uvEps = 1e-6f;
    for (const auto &kv : edgeSamples) {
        const auto &samples = kv.second;
        if (samples.empty())
            continue;

        if (samples.size() == 1) {
            const UvEdgeSample &s = samples.front();
            auto &dst = boundaryEdgeData[size_t(s.group)];
            dst.insert(dst.end(), { s.uvA.x(), s.uvA.y(), 0.0f, s.uvB.x(), s.uvB.y(), 0.0f });
            continue;
        }

        const UvEdgeSample &ref = samples.front();
        bool isSeam = false;
        for (size_t si = 1; si < samples.size(); ++si) {
            const UvEdgeSample &s = samples[si];
            if ((s.uvA - ref.uvA).lengthSquared() > uvEps * uvEps
                || (s.uvB - ref.uvB).lengthSquared() > uvEps * uvEps) {
                isSeam = true;
                break;
            }
        }
        if (!isSeam)
            continue;

        for (const UvEdgeSample &s : samples) {
            auto &dst = textureSeamData[size_t(s.group)];
            dst.insert(dst.end(), { s.uvA.x(), s.uvA.y(), 0.0f, s.uvB.x(), s.uvB.y(), 0.0f });
        }
    }

    // Selection overlay geometry in UV space: triangles for selected faces,
    // points for selected vertices (position-only, drawn with the scene's
    // selection pipelines).
    std::vector<std::vector<float>> selectedFaceData;
    std::vector<std::vector<float>> selectedVertexData;
    selectedFaceData.resize(size_t(groupCount));
    selectedVertexData.resize(size_t(groupCount));
    for (int fi = 0; fi < mesh.FN(); ++fi) {
        const auto &f = mesh.face[fi];
        if (f.IsD())
            continue;
        QVector2D cuv[3];
        if (!uvForCorner(f, 0, cuv[0]) || !uvForCorner(f, 1, cuv[1]) || !uvForCorner(f, 2, cuv[2]))
            continue;
        const size_t group = size_t(faceGroup(f));
        if (f.IsS()) {
            for (const QVector2D &uv : cuv) {
                selectedFaceData[group].insert(
                    selectedFaceData[group].end(), { uv.x(), uv.y(), 0.0f });
            }
        }
        for (int c = 0; c < 3; ++c) {
            const auto *v = f.cV(c);
            if (v && v->IsS()) {
                selectedVertexData[group].insert(
                    selectedVertexData[group].end(), { cuv[c].x(), cuv[c].y(), 0.0f });
            }
        }
    }

    QRhiResourceUpdateBatch *updates = m_rhi->nextResourceUpdateBatch();
    bool anyUpload = false;
    auto uploadGroups = [&](const std::vector<std::vector<float>> &groups,
                            std::unique_ptr<QRhiBuffer> &dst,
                            int &dstCount,
                            std::vector<UvMeshGpu::DrawRange> &ranges,
                            int strideFloats) {
        size_t total = 0;
        for (const auto &group : groups)
            total += group.size();
        ranges.assign(groups.size(), {});
        size_t offsetFloats = 0;
        for (size_t group = 0; group < groups.size(); ++group) {
            ranges[group].byteOffset = quint32(offsetFloats * sizeof(float));
            ranges[group].vertexCount = int(groups[group].size() / size_t(strideFloats));
            offsetFloats += groups[group].size();
        }
        dst.reset();
        dstCount = 0;
        if (total == 0)
            return;
        dst.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                quint32(total * sizeof(float))));
        if (!dst || !dst->create()) {
            dst.reset();
            return;
        }
        for (size_t group = 0; group < groups.size(); ++group) {
            const auto &src = groups[group];
            if (!src.empty()) {
                updates->uploadStaticBuffer(
                    dst.get(),
                    ranges[group].byteOffset,
                    quint32(src.size() * sizeof(float)),
                    src.data());
            }
        }
        dstCount = int(total / size_t(strideFloats));
        anyUpload = true;
    };

    uploadGroups(wireData, gpu.wireVbuf, gpu.wireVertexCount, gpu.wireGroups, 3);
    uploadGroups(
        boundaryEdgeData,
        gpu.boundaryEdgesVbuf,
        gpu.boundaryEdgesVertexCount,
        gpu.boundaryEdgeGroups,
        3);
    uploadGroups(
        textureSeamData,
        gpu.textureSeamsVbuf,
        gpu.textureSeamsVertexCount,
        gpu.textureSeamGroups,
        3);
    for (int i = 0; i < 5; ++i) {
        uploadGroups(
            fillData[size_t(i)],
            gpu.fillVariants[size_t(i)].vbuf,
            gpu.fillVariants[size_t(i)].vertexCount,
            gpu.fillVariants[size_t(i)].groups,
            kFillVertexStrideFloats);
    }
    for (int i = 0; i < 3; ++i) {
        uploadGroups(
            pointsData[size_t(i)],
            gpu.pointsVariants[size_t(i)].vbuf,
            gpu.pointsVariants[size_t(i)].vertexCount,
            gpu.pointsVariants[size_t(i)].groups,
            kPointsVertexStrideFloats);
    }
    uploadGroups(
        selectedFaceData,
        gpu.selectedFacesVbuf,
        gpu.selectedFacesVertexCount,
        gpu.selectedFaceGroups,
        3);
    uploadGroups(
        selectedVertexData,
        gpu.selectedVerticesVbuf,
        gpu.selectedVerticesVertexCount,
        gpu.selectedVertexGroups,
        3);

    if (anyUpload)
        cb->resourceUpdate(updates);

    gpu.minUv = minUv;
    gpu.maxUv = maxUv;
    gpu.valid = (gpu.wireVertexCount > 0)
        || (gpu.fillVariants[0].vertexCount > 0)
        || (gpu.pointsVariants[0].vertexCount > 0);
    return gpu.valid;
}

void RenderWidget::fitUvViewToCurrentMesh(const QSize &pixelSize)
{
    const int meshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    const bool fitWholeTexture = m_renderSettings.uvShowFullTexture;
    if (!m_doc || meshIndex < 0 || meshIndex >= m_doc->meshCount()) {
        m_uvPan = QVector2D(0.5f, 0.5f);
        m_uvZoom = 1.0f;
        m_uvFitRequested = false;
        return;
    }

    const auto &entry = m_doc->mesh(meshIndex);
    const auto it = m_uvMeshGpu.find(entry.meshId);
    if (!fitWholeTexture && (it == m_uvMeshGpu.end() || !it->second.valid)) {
        m_uvPan = QVector2D(0.5f, 0.5f);
        m_uvZoom = 1.0f;
        m_uvFitRequested = false;
        return;
    }

    const QVector2D minUv = fitWholeTexture ? QVector2D(0.0f, 0.0f) : it->second.minUv;
    const QVector2D maxUv = fitWholeTexture ? QVector2D(1.0f, 1.0f) : it->second.maxUv;
    const QVector2D center = (minUv + maxUv) * 0.5f;
    const float halfW = qMax(1e-6f, (maxUv.x() - minUv.x()) * 0.5f);
    const float halfH = qMax(1e-6f, (maxUv.y() - minUv.y()) * 0.5f);

    const float aspect =
        (pixelSize.height() > 0) ? (float(pixelSize.width()) / float(pixelSize.height())) : 1.0f;
    const float xLim = (aspect >= 1.0f) ? aspect : 1.0f;
    const float yLim = (aspect >= 1.0f) ? 1.0f : (1.0f / qMax(1e-6f, aspect));
    const float padding = 0.92f;
    const float zoomX = (xLim * padding) / halfW;
    const float zoomY = (yLim * padding) / halfH;

    m_uvPan = center;
    m_uvZoom = std::clamp(std::min(zoomX, zoomY), kUvMinZoom, kUvMaxZoom);
    m_uvFitRequested = false;
}

void RenderWidget::updateUvScaleOverlay(
    const QMatrix4x4 &mvp,
    const QSize &pixelSize,
    bool showUvReference)
{
    auto hideAllLabels = [this]() {
        for (QLabel *label : m_uvScaleXTickLabels) {
            if (label)
                label->hide();
        }
        for (QLabel *label : m_uvScaleYTickLabels) {
            if (label)
                label->hide();
        }
    };
    if (m_viewMode != ViewMode::ParametrizationUV || !showUvReference || pixelSize.isEmpty()) {
        hideAllLabels();
        return;
    }

    const auto uvToWidgetPoint = [this, &mvp, &pixelSize](float u, float v, QPointF &out) -> bool {
        const QVector4D clip = mvp * QVector4D(u, v, 0.0f, 1.0f);
        if (clip.w() <= 1e-8f)
            return false;
        const QVector3D ndc = clip.toVector3DAffine();
        const float px = (ndc.x() * 0.5f + 0.5f) * float(pixelSize.width());
        const float py = (1.0f - (ndc.y() * 0.5f + 0.5f)) * float(pixelSize.height());
        const float dpr = qMax(1.0, devicePixelRatioF());
        out.setX(px / dpr);
        out.setY(py / dpr);
        return std::isfinite(out.x()) && std::isfinite(out.y());
    };

    QPointF p00;
    QPointF p10;
    QPointF p01;
    if (!uvToWidgetPoint(0.0f, 0.0f, p00)
        || !uvToWidgetPoint(1.0f, 0.0f, p10)
        || !uvToWidgetPoint(0.0f, 1.0f, p01)) {
        hideAllLabels();
        return;
    }

    const QVector2D xAxis = QVector2D(p10 - p00);
    const QVector2D yAxis = QVector2D(p01 - p00);
    if (xAxis.length() < 12.0f || yAxis.length() < 12.0f) {
        hideAllLabels();
        return;
    }

    QFont labelFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (labelFont.pointSizeF() <= 0.0f)
        labelFont.setPointSize(9);
    labelFont.setPointSizeF(std::max(8.0, labelFont.pointSizeF() - 1.0));
    const QFontMetrics fm(labelFont);
    const int labelH = qMax(10, fm.height());
    const int xLabelW = qMax(24, fm.horizontalAdvance(QStringLiteral("1.0")) + 4);
    const int yLabelW = qMax(24, fm.horizontalAdvance(QStringLiteral("1.0")) + 4);
    const int xOffset = 8;
    const int yOffset = 6;

    for (int i = 0; i <= 10; ++i) {
        const float t = float(i) / 10.0f;
        const QPointF xPt = p00 + (p10 - p00) * t;
        const QPointF yPt = p00 + (p01 - p00) * t;

        QLabel *xLabel = m_uvScaleXTickLabels[size_t(i)];
        if (xLabel) {
            xLabel->setFont(labelFont);
            xLabel->setFixedSize(xLabelW, labelH);
            int x = int(std::lround(xPt.x())) - xLabelW / 2;
            int y = int(std::lround(xPt.y())) + xOffset;
            x = std::clamp(x, 0, qMax(0, width() - xLabel->width()));
            y = std::clamp(y, 0, qMax(0, height() - xLabel->height()));
            xLabel->move(x, y);
            xLabel->show();
            xLabel->raise();
        }

        QLabel *yLabel = m_uvScaleYTickLabels[size_t(i)];
        if (yLabel) {
            yLabel->setFont(labelFont);
            yLabel->setFixedSize(yLabelW, labelH);
            int x = int(std::lround(yPt.x())) - yLabelW - yOffset;
            int y = int(std::lround(yPt.y())) - yLabel->height() / 2;
            x = std::clamp(x, 0, qMax(0, width() - yLabel->width()));
            y = std::clamp(y, 0, qMax(0, height() - yLabel->height()));
            yLabel->move(x, y);
            yLabel->show();
            yLabel->raise();
        }
    }
}

void RenderWidget::renderParametrization(QRhiCommandBuffer *cb)
{
    if (!m_rhi || !m_ubuf || !cb || !renderTarget())
        return;

    syncPerMeshRenderModesWithDocument();
    syncUvCacheWithDocument();
    syncUvTextureGroupUi();
    m_frameTimer.start();

    if (m_bboxMinCornerOverlayLabel)
        m_bboxMinCornerOverlayLabel->hide();
    if (m_bboxMaxCornerOverlayLabel)
        m_bboxMaxCornerOverlayLabel->hide();
    if (m_bboxDimXOverlayLabel)
        m_bboxDimXOverlayLabel->hide();
    if (m_bboxDimYOverlayLabel)
        m_bboxDimYOverlayLabel->hide();
    if (m_bboxDimZOverlayLabel)
        m_bboxDimZOverlayLabel->hide();

    const QSize sz = renderTarget()->pixelSize();
    const int meshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    const PerMeshRenderSettings meshSettings =
        (meshIndex >= 0 && meshIndex < m_doc->meshCount())
        ? renderModeForMesh(meshIndex)
        : PerMeshRenderSettings{};
    const bool hasMeshTextures =
        (m_doc && meshIndex >= 0 && meshIndex < m_doc->meshCount())
        ? (Document::meshTextureAssociationCount(m_doc->mesh(meshIndex)) > 0)
        : false;
    const bool canDraw =
        (meshIndex >= 0)
        && meshVisible(meshIndex)
        && ensureUvMeshResources(meshIndex, cb);
    if (m_uvFitRequested)
        fitUvViewToCurrentMesh(sz);

    if (!m_uvBackgroundUbuf) {
        m_uvBackgroundUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUvBackgroundUbufSize));
        if (!m_uvBackgroundUbuf->create())
            m_uvBackgroundUbuf.reset();
    }
    if (!m_uvBackgroundSrb && m_uvBackgroundUbuf) {
        m_uvBackgroundSrb.reset(m_rhi->newShaderResourceBindings());
        m_uvBackgroundSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, m_uvBackgroundUbuf.get())
        });
        if (!m_uvBackgroundSrb->create())
            m_uvBackgroundSrb.reset();
    }
    if (!m_uvBackgroundPipeline && m_uvBackgroundSrb) {
        m_uvBackgroundPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/uv_background.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/uv_background.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            m_uvBackgroundPipeline.reset();
        } else {
            m_uvBackgroundPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs },
            });
            m_uvBackgroundPipeline->setDepthTest(false);
            m_uvBackgroundPipeline->setDepthWrite(false);
            m_uvBackgroundPipeline->setCullMode(QRhiGraphicsPipeline::None);
            m_uvBackgroundPipeline->setShaderResourceBindings(m_uvBackgroundSrb.get());
            m_uvBackgroundPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_uvBackgroundPipeline->create()) {
                qWarning("Failed to create UV background pipeline");
                m_uvBackgroundPipeline.reset();
            }
        }
    }
    if (!m_uvTextureFillPipeline && m_srb) {
        m_uvTextureFillPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/uv_fill_texture.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/fill_smooth.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            m_uvTextureFillPipeline.reset();
        } else {
            m_uvTextureFillPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs },
            });
            m_uvTextureFillPipeline->setDepthTest(false);
            m_uvTextureFillPipeline->setDepthWrite(false);
            m_uvTextureFillPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kFillVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({
                { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
                { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },
                { 0, 2, QRhiVertexInputAttribute::Float4, 6 * sizeof(float) },
                { 0, 3, QRhiVertexInputAttribute::Float3, 10 * sizeof(float) },
            });
            m_uvTextureFillPipeline->setVertexInputLayout(layout);
            m_uvTextureFillPipeline->setShaderResourceBindings(m_srb.get());
            m_uvTextureFillPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_uvTextureFillPipeline->create()) {
                qWarning("Failed to create UV textured fill pipeline");
                m_uvTextureFillPipeline.reset();
            }
        }
    }
    if (!m_uvTextureQuadVbuf) {
        auto appendUvQuadVertex = [](std::array<float, 6 * kFillVertexStrideFloats> &dst,
                                     int vertexIndex,
                                     float u,
                                     float v) {
            const int o = vertexIndex * kFillVertexStrideFloats;
            dst[size_t(o + 0)] = 0.0f;
            dst[size_t(o + 1)] = 0.0f;
            dst[size_t(o + 2)] = 0.0f;
            dst[size_t(o + 3)] = 0.0f;
            dst[size_t(o + 4)] = 0.0f;
            dst[size_t(o + 5)] = 1.0f;
            dst[size_t(o + 6)] = 1.0f;
            dst[size_t(o + 7)] = 1.0f;
            dst[size_t(o + 8)] = 1.0f;
            dst[size_t(o + 9)] = 0.0f;
            dst[size_t(o + 10)] = u;
            dst[size_t(o + 11)] = v;
            dst[size_t(o + 12)] = 1.0f;
        };
        std::array<float, 6 * kFillVertexStrideFloats> quad = {};
        appendUvQuadVertex(quad, 0, 0.0f, 0.0f);
        appendUvQuadVertex(quad, 1, 1.0f, 0.0f);
        appendUvQuadVertex(quad, 2, 1.0f, 1.0f);
        appendUvQuadVertex(quad, 3, 0.0f, 0.0f);
        appendUvQuadVertex(quad, 4, 1.0f, 1.0f);
        appendUvQuadVertex(quad, 5, 0.0f, 1.0f);

        m_uvTextureQuadVbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                quint32(quad.size() * sizeof(float))));
        if (m_uvTextureQuadVbuf && m_uvTextureQuadVbuf->create()) {
            QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
            u->uploadStaticBuffer(m_uvTextureQuadVbuf.get(), quad.data());
            cb->resourceUpdate(u);
            m_uvTextureQuadVertexCount = 6;
        } else {
            m_uvTextureQuadVbuf.reset();
            m_uvTextureQuadVertexCount = 0;
        }
    }
    if (!m_uvAxesVbuf) {
        const std::array<float, 12> axes = {
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, // U axis
            0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f  // V axis
        };
        m_uvAxesVbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                quint32(axes.size() * sizeof(float))));
        if (m_uvAxesVbuf && m_uvAxesVbuf->create()) {
            QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
            u->uploadStaticBuffer(m_uvAxesVbuf.get(), axes.data());
            cb->resourceUpdate(u);
            m_uvAxesVertexCount = 4;
        } else {
            m_uvAxesVbuf.reset();
            m_uvAxesVertexCount = 0;
        }
    }
    if (!m_uvUnitBoxVbuf) {
        const std::array<float, 24> unitBox = {
            0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
            1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f
        };
        m_uvUnitBoxVbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                quint32(unitBox.size() * sizeof(float))));
        if (m_uvUnitBoxVbuf && m_uvUnitBoxVbuf->create()) {
            QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
            u->uploadStaticBuffer(m_uvUnitBoxVbuf.get(), unitBox.data());
            cb->resourceUpdate(u);
            m_uvUnitBoxVertexCount = 8;
        } else {
            m_uvUnitBoxVbuf.reset();
            m_uvUnitBoxVertexCount = 0;
        }
    }

    const float aspect = (sz.height() > 0) ? (float(sz.width()) / float(sz.height())) : 1.0f;
    const float xLim = (aspect >= 1.0f) ? aspect : 1.0f;
    const float yLim = (aspect >= 1.0f) ? 1.0f : (1.0f / qMax(1e-6f, aspect));

    QMatrix4x4 proj;
    proj.ortho(-xLim, xLim, -yLim, yLim, -1.0f, 1.0f);
    QMatrix4x4 model;
    model.scale(m_uvZoom, m_uvZoom, 1.0f);
    model.translate(-m_uvPan.x(), -m_uvPan.y(), 0.0f);
    const QMatrix4x4 mvp = proj * model;

    QMatrix4x4 modelView;
    modelView.setToIdentity();
    QMatrix3x3 normalMat;
    normalMat.fill(0.0f);
    normalMat(0, 0) = 1.0f;
    normalMat(1, 1) = 1.0f;
    normalMat(2, 2) = 1.0f;

    Document::FillPassGpuView textureFillView {};
    const int activeGroup = activeUvTextureGroup();
    auto displayedTexture = [this](const auto &batch) -> QRhiTexture * {
        switch (std::clamp(m_renderSettings.uvTextureChannel, 0, 3)) {
        case 1: return batch.normalTexture;
        case 2: return batch.occlusionTexture;
        case 3: return batch.roughnessTexture;
        default: return batch.baseColorTexture;
        }
    };
    const bool useTextureDrivenFill =
        hasMeshTextures
        && (meshSettings.fillPlain.colorSource == FillColorSource::Texture);
    const bool needTextureGeometry = hasMeshTextures
        && (m_renderSettings.uvShowFullTexture
            || (meshSettings.showFill && useTextureDrivenFill));
    if (canDraw && needTextureGeometry) {
        m_doc->ensureMeshGpuResources(
            m_rhi,
            cb,
            meshIndex,
            Document::FillGpuVariant::Texture,
            Document::PointGpuVariant::Constant,
            true,   // fill
            false,  // wire
            false,  // edges
            false,  // points
            false,  // bbox
            false,  // decorator normals
            false); // decorator boundaries
        textureFillView = m_doc->fillPassGpuView(m_rhi, meshIndex, Document::FillGpuVariant::Texture);
    }

    // Always push the UV-space transform at frame start so pan/zoom is applied
    // even when the first drawn pass reuses the same style key.
    uploadMainUbuf(cb, mvp, modelView, normalMat, meshSettings, sz, false);

    auto updateStyleUbuf = [&](const PerMeshRenderSettings &styleSettings,
                               float normalScale = 1.0f,
                               float occlusionStrength = 1.0f,
                               float roughnessFactor = 1.0f) {
        uploadMainUbuf(
            cb,
            mvp,
            modelView,
            normalMat,
            styleSettings,
            sz,
            false,
            QVector3D(0.0f, 0.0f, 1.0f),
            MainUbufMaterialOverrides { normalScale, occlusionStrength, roughnessFactor });
    };

    if (m_uvBackgroundUbuf) {
        float bgData[kUvBackgroundUbufSize / sizeof(float)] = {};
        bgData[0] = m_uvPan.x();
        bgData[1] = m_uvPan.y();
        bgData[2] = qMax(1e-6f, m_uvZoom);
        bgData[3] = qMax(1e-6f, aspect);
        bgData[4] = 0.30f; bgData[5] = 0.30f; bgData[6] = 0.32f; bgData[7] = 1.0f;
        bgData[8] = 0.36f; bgData[9] = 0.36f; bgData[10] = 0.38f; bgData[11] = 1.0f;
        bgData[12] = 12.0f; bgData[13] = 6.0f; bgData[14] = 0.0f; bgData[15] = 0.0f;
        QRhiResourceUpdateBatch *uBg = m_rhi->nextResourceUpdateBatch();
        uBg->updateDynamicBuffer(m_uvBackgroundUbuf.get(), 0, kUvBackgroundUbufSize, bgData);
        cb->resourceUpdate(uBg);
    }

    cb->beginPass(renderTarget(), QColor(40, 40, 40), { 1.0f, 0 }, nullptr);
    cb->setViewport({ 0, 0, float(sz.width()), float(sz.height()) });

    if (m_uvBackgroundPipeline && m_uvBackgroundSrb) {
        cb->setGraphicsPipeline(m_uvBackgroundPipeline.get());
        cb->setShaderResources(m_uvBackgroundSrb.get());
        cb->draw(3);
    }

    if (m_renderSettings.uvShowFullTexture
        && m_uvTextureFillPipeline
        && m_uvTextureQuadVbuf
        && m_uvTextureQuadVertexCount > 0
        && textureFillView.valid) {
        QRhiTexture *fullTexture = nullptr;
        for (int bi = 0; bi < textureFillView.batchCount; ++bi) {
            const auto &b = textureFillView.batches[bi];
            if (b.textureGroupIndex == activeGroup && displayedTexture(b)) {
                fullTexture = displayedTexture(b);
                break;
            }
        }
        if (fullTexture) {
            PerMeshRenderSettings textureBgSettings = meshSettings;
            textureBgSettings.fillLighting = false;
            updateStyleUbuf(textureBgSettings);
            cb->setGraphicsPipeline(m_uvTextureFillPipeline.get());
            setShaderResourcesWithOffset(
                cb,
                shaderResourcesForFillTextures(
                    fullTexture,
                    nullptr,
                    nullptr,
                    nullptr,
                    m_renderSettings.uvTextureNearestSampling),
                0);
            const QRhiCommandBuffer::VertexInput binding(m_uvTextureQuadVbuf.get(), 0);
            cb->setVertexInput(0, 1, &binding);
            cb->draw(m_uvTextureQuadVertexCount);
        }
    }

    int uvLineSlot = 0;
    auto ensureUvLineSlot = [&](int slot) -> bool {
        if (slot < 0 || slot >= int(m_uvLineUbufs.size()))
            return false;
        if (!m_uvLineUbufs[size_t(slot)]) {
            m_uvLineUbufs[size_t(slot)].reset(
                m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kDecoratorUbufSize));
            if (!m_uvLineUbufs[size_t(slot)] || !m_uvLineUbufs[size_t(slot)]->create())
                m_uvLineUbufs[size_t(slot)].reset();
        }
        if (!m_uvLineSrbs[size_t(slot)] && m_uvLineUbufs[size_t(slot)]) {
            m_uvLineSrbs[size_t(slot)].reset(m_rhi->newShaderResourceBindings());
            m_uvLineSrbs[size_t(slot)]->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0,
                    QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                    m_uvLineUbufs[size_t(slot)].get())
            });
            if (!m_uvLineSrbs[size_t(slot)]->create())
                m_uvLineSrbs[size_t(slot)].reset();
        }
        return m_uvLineUbufs[size_t(slot)] && m_uvLineSrbs[size_t(slot)];
    };

    auto uvLinePipelineForWidth = [&](float width) -> QRhiGraphicsPipeline * {
        if (!m_rhi || !renderTarget())
            return nullptr;
        const int key = int(std::lround(qMax(0.5f, width) * 10.0f));
        auto it = m_uvLinePipelinesByKey.find(key);
        if (it != m_uvLinePipelinesByKey.end())
            return it->second.get();
        if (!ensureUvLineSlot(0))
            return nullptr;

        auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_decorator.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_decorator.frag.qsb"));
        if (!vs.isValid() || !fs.isValid())
            return nullptr;

        pipeline->setShaderStages({
            { QRhiShaderStage::Vertex, vs },
            { QRhiShaderStage::Fragment, fs }
        });
        pipeline->setTopology(QRhiGraphicsPipeline::Lines);
        pipeline->setDepthTest(false);
        pipeline->setDepthWrite(false);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setLineWidth(qMax(0.5f, width));
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable = true;
        blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
        blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.opColor = QRhiGraphicsPipeline::Add;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        blend.opAlpha = QRhiGraphicsPipeline::Add;
        pipeline->setTargetBlends({ blend });
        QRhiVertexInputLayout layout;
        layout.setBindings({ { 3 * sizeof(float) } });
        layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
        pipeline->setVertexInputLayout(layout);
        pipeline->setShaderResourceBindings(m_uvLineSrbs[0].get());
        pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        if (!pipeline->create())
            return nullptr;

        auto inserted = m_uvLinePipelinesByKey.emplace(key, std::move(pipeline));
        return inserted.first->second.get();
    };

    auto drawUvLineSetStable = [&](const QColor &color,
                                   float width,
                                   QRhiBuffer *vbuf,
                                   int vertexCount,
                                   quint32 firstByteOffset = 0u) {
        if (!vbuf || vertexCount <= 0)
            return;
        if (uvLineSlot >= int(m_uvLineUbufs.size()))
            return;
        if (!ensureUvLineSlot(uvLineSlot))
            return;
        QRhiGraphicsPipeline *pipeline = uvLinePipelineForWidth(width);
        if (!pipeline)
            return;

        float decoData[kDecoratorUbufSize / sizeof(float)] = {};
        memcpy(decoData, mvp.constData(), 16 * sizeof(float));
        decoData[16] = color.redF();
        decoData[17] = color.greenF();
        decoData[18] = color.blueF();
        decoData[19] = color.alphaF();
        QRhiResourceUpdateBatch *uLine = m_rhi->nextResourceUpdateBatch();
        uLine->updateDynamicBuffer(
            m_uvLineUbufs[size_t(uvLineSlot)].get(), 0, kDecoratorUbufSize, decoData);
        cb->resourceUpdate(uLine);

        cb->setGraphicsPipeline(pipeline);
        cb->setShaderResources(m_uvLineSrbs[size_t(uvLineSlot)].get());
        const QRhiCommandBuffer::VertexInput binding(vbuf, firstByteOffset);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(vertexCount);
        ++uvLineSlot;
    };

    if (canDraw) {
        const auto &entry = m_doc->mesh(meshIndex);
        auto cacheIt = m_uvMeshGpu.find(entry.meshId);
        if (cacheIt != m_uvMeshGpu.end() && cacheIt->second.valid) {
            UvMeshGpu &uvGpu = cacheIt->second;
            const MeshRenderMode meshMode = renderModeForMesh(meshIndex);
            auto groupRange = [activeGroup](const std::vector<UvMeshGpu::DrawRange> &ranges) {
                return activeGroup >= 0 && activeGroup < int(ranges.size())
                    ? ranges[size_t(activeGroup)]
                    : UvMeshGpu::DrawRange{};
            };

            if (meshSettings.showFill) {
                if (useTextureDrivenFill
                    && m_uvTextureFillPipeline) {
                    updateStyleUbuf(meshSettings);
                    const Document::FillPassGpuView fillView = textureFillView;
                    if (fillView.valid) {
                        cb->setGraphicsPipeline(m_uvTextureFillPipeline.get());
                        updateStyleUbuf(meshSettings);
                        for (int bi = 0; bi < fillView.batchCount; ++bi) {
                            const auto &batch = fillView.batches[bi];
                            if (batch.textureGroupIndex != activeGroup
                                || !hasDrawableBatchGeometry(batch))
                                continue;
                            setShaderResourcesWithOffset(
                                cb,
                                shaderResourcesForFillTextures(
                                    displayedTexture(batch),
                                    nullptr,
                                    nullptr,
                                    nullptr,
                                    m_renderSettings.uvTextureNearestSampling),
                                0);
                            drawBatchGeometry(cb, batch);
                        }
                    }
                } else {
                    PerMeshRenderSettings fillSettings = meshSettings;
                    fillSettings.fillMaterial = FillMaterial::Plain;
                    fillSettings.fillLighting = false;
                    fillSettings.fillBackfaceCulling = false;
                    QRhiGraphicsPipeline *fillPipeline = fillPipelineForSettings(fillSettings);
                    int fillVariantIdx = 0;
                    if (meshSettings.fillPlain.colorSource == FillColorSource::PerVertex)
                        fillVariantIdx = 1;
                    else if (meshSettings.fillPlain.colorSource == FillColorSource::PerFace)
                        fillVariantIdx = 2;
                    else if (meshSettings.fillPlain.colorSource == FillColorSource::PerVertexQuality)
                        fillVariantIdx = 3;
                    else if (meshSettings.fillPlain.colorSource == FillColorSource::PerFaceQuality)
                        fillVariantIdx = 4;
                    const auto &fillVariant = uvGpu.fillVariants[size_t(fillVariantIdx)];
                    const UvMeshGpu::DrawRange range = groupRange(fillVariant.groups);
                    if (fillPipeline && fillVariant.vbuf && range.vertexCount > 0) {
                        updateStyleUbuf(fillSettings);
                        cb->setGraphicsPipeline(fillPipeline);
                        setShaderResourcesWithOffset(
                            cb,
                            shaderResourcesForFillTextures(
                                nullptr,
                                nullptr,
                                nullptr,
                                nullptr,
                                m_renderSettings.uvTextureNearestSampling),
                            0);
                        const QRhiCommandBuffer::VertexInput binding(
                            fillVariant.vbuf.get(), range.byteOffset);
                        cb->setVertexInput(0, 1, &binding);
                        cb->draw(range.vertexCount);
                    }
                }
            }

            const UvMeshGpu::DrawRange wireRange = groupRange(uvGpu.wireGroups);
            if (meshSettings.showWire)
                drawUvLineSetStable(
                    meshSettings.wireColor, meshSettings.wireSize, uvGpu.wireVbuf.get(),
                    wireRange.vertexCount, wireRange.byteOffset);
            if (meshSettings.showEdges)
                drawUvLineSetStable(
                    meshSettings.edgeColor, meshSettings.edgeSize, uvGpu.wireVbuf.get(),
                    wireRange.vertexCount, wireRange.byteOffset);
            // The toolbar button gates this pass in the UV view too.
            if (meshMode.decoratorBoundary && meshMode.decoratorBoundaryEdges)
            {
                const UvMeshGpu::DrawRange range = groupRange(uvGpu.boundaryEdgeGroups);
                drawUvLineSetStable(
                    meshMode.decoratorBoundaryEdgeColor,
                    qMax(0.5f, meshSettings.decoratorBoundaryWidth),
                    uvGpu.boundaryEdgesVbuf.get(),
                    range.vertexCount,
                    range.byteOffset);
            }
            if (meshMode.decoratorBoundary && meshMode.decoratorTextureSeams)
            {
                const UvMeshGpu::DrawRange range = groupRange(uvGpu.textureSeamGroups);
                drawUvLineSetStable(
                    meshMode.decoratorTextureSeamColor,
                    qMax(0.5f, meshSettings.decoratorBoundaryWidth),
                    uvGpu.textureSeamsVbuf.get(),
                    range.vertexCount,
                    range.byteOffset);
            }

            if (meshSettings.showPoints && m_pointsPipeline) {
                int pointVariantIdx = 0;
                if (meshSettings.pointColorSource == PointColorSource::PerVertex)
                    pointVariantIdx = 1;
                else if (meshSettings.pointColorSource == PointColorSource::PerVertexQuality)
                    pointVariantIdx = 2;
                const auto &pointVariant = uvGpu.pointsVariants[size_t(pointVariantIdx)];
                const UvMeshGpu::DrawRange range = groupRange(pointVariant.groups);
                if (pointVariant.vbuf && range.vertexCount > 0) {
                    PerMeshRenderSettings pointSettings = meshSettings;
                    pointSettings.pointLighting = false;
                    updateStyleUbuf(pointSettings);
                    cb->setGraphicsPipeline(m_pointsPipeline.get());
                    setShaderResourcesWithOffset(cb, m_srb.get(), 0);
                    const QRhiCommandBuffer::VertexInput binding(
                        pointVariant.vbuf.get(), range.byteOffset);
                    cb->setVertexInput(0, 1, &binding);
                    cb->draw(range.vertexCount);
                }
            }

            // Selection overlay, reusing the 3D scene's selection pipelines so the
            // look matches (translucent red faces + red points), projected with the
            // UV ortho MVP.
            const UvMeshGpu::DrawRange selectedFaceRange =
                groupRange(uvGpu.selectedFaceGroups);
            const UvMeshGpu::DrawRange selectedVertexRange =
                groupRange(uvGpu.selectedVertexGroups);
            const bool hasSelFaces =
                uvGpu.selectedFacesVbuf && selectedFaceRange.vertexCount > 0;
            const bool hasSelVerts =
                uvGpu.selectedVerticesVbuf && selectedVertexRange.vertexCount > 0;
            if (m_selectionUbuf && m_selectionSrb && (hasSelFaces || hasSelVerts)) {
                const quint32 selOffset =
                    allocateDynamicUbufOffset(m_selectionUbufAllocator, "selection");
                float selData[kDecoratorUbufSize / sizeof(float)] = {};
                memcpy(selData, mvp.constData(), 64);
                selData[16] = 1.0f; // red, 50% alpha — same as the scene overlay
                selData[17] = 0.0f;
                selData[18] = 0.0f;
                selData[19] = 0.5f;
                QRhiResourceUpdateBatch *uSel = m_rhi->nextResourceUpdateBatch();
                uSel->updateDynamicBuffer(m_selectionUbuf.get(), selOffset, kDecoratorUbufSize, selData);
                cb->resourceUpdate(uSel);
                if (hasSelFaces && m_selectionFacesPipeline) {
                    cb->setGraphicsPipeline(m_selectionFacesPipeline.get());
                    setShaderResourcesWithOffset(cb, m_selectionSrb.get(), selOffset);
                    const QRhiCommandBuffer::VertexInput fv(
                        uvGpu.selectedFacesVbuf.get(), selectedFaceRange.byteOffset);
                    cb->setVertexInput(0, 1, &fv);
                    cb->draw(selectedFaceRange.vertexCount);
                }
                if (hasSelVerts && m_selectionVerticesPipeline) {
                    cb->setGraphicsPipeline(m_selectionVerticesPipeline.get());
                    setShaderResourcesWithOffset(cb, m_selectionSrb.get(), selOffset);
                    const QRhiCommandBuffer::VertexInput vv(
                        uvGpu.selectedVerticesVbuf.get(), selectedVertexRange.byteOffset);
                    cb->setVertexInput(0, 1, &vv);
                    cb->draw(selectedVertexRange.vertexCount);
                }
            }
        }
    }

    const bool drawUnitSquare = m_renderSettings.uvShowReferenceFrame;
    if (drawUnitSquare && m_uvUnitBoxVbuf && m_uvUnitBoxVertexCount > 0) {
        const QColor squareColor(220, 220, 225, 220);
        const float squareWidth = 1.0f;
        drawUvLineSetStable(squareColor, squareWidth, m_uvUnitBoxVbuf.get(), m_uvUnitBoxVertexCount);
    }

    if (m_renderSettings.uvShowReferenceFrame && m_uvAxesVbuf && m_uvAxesVertexCount >= 4) {
        const float axisWidth = qMax(1.2f, meshSettings.edgeSize);
        drawUvLineSetStable(QColor(230, 82, 82), axisWidth, m_uvAxesVbuf.get(), 2, 0);
        drawUvLineSetStable(
            QColor(80, 200, 120),
            axisWidth,
            m_uvAxesVbuf.get(),
            2,
            quint32(2 * 3 * sizeof(float)));
    }

    updateUvScaleOverlay(mvp, sz, m_renderSettings.uvShowReferenceFrame);

    cb->endPass();

    const float cpuMs = m_frameTimer.nsecsElapsed() / 1e6f;
    const bool gpuTimingSupported = m_rhi->isFeatureSupported(QRhi::Timestamps);
    float gpuMs = 0.0f;
    bool gpuSampleValid = false;
    if (gpuTimingSupported) {
        const double gpuSeconds = cb->lastCompletedGpuTime();
        if (gpuSeconds > 0.0) {
            gpuMs = static_cast<float>(gpuSeconds * 1000.0);
            gpuSampleValid = true;
        }
    }
    emit frameRendered(cpuMs, gpuMs, gpuTimingSupported, gpuSampleValid);
}
