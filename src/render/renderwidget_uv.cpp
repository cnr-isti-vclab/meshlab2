#include "textureassociationutils.h"
#include "renderwidget.h"
#include "document.h"
#include "renderwidget_internal.h"
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QLabel>
#include <QListWidget>
#include <QPixmap>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

using namespace RenderWidgetInternal;

namespace {
constexpr int kUvBackgroundUbufSize = 64;
constexpr float kUvMinZoom = 0.05f;
constexpr float kUvMaxZoom = 5000.0f;

std::vector<int> usedTextureGroups(const Document::MeshEntry &entry)
{
    std::vector<int> groups;
    for (const VCGFace &face : entry.mesh.face) {
        // A face with no texture has no place in any texture's UV square.
        const int group = face.IsD() ? -1 : vcgFaceTextureGroup(entry.ioMask, face);
        if (group >= 0)
            groups.push_back(group);
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    if (groups.empty())
        groups.push_back(0);
    return groups;
}

// The cache id of a layer's texture group in UV space, apart from the layer's own: the high
// bit marks it, the group sits above the 40 bits left to the mesh id. Every view builds the
// same layout for the same id and revisions, so they share its buffers.
constexpr std::uint64_t kUvLayoutMeshIdMask = (std::uint64_t(1) << 40) - 1;
std::uint64_t uvLayoutCacheId(std::uint64_t meshId, int group)
{
    return (std::uint64_t(1) << 63) | (std::uint64_t(std::uint32_t(group)) << 40)
        | (meshId & kUvLayoutMeshIdMask);
}
}

void RenderWidget::syncUvCacheWithDocument()
{
    if (!m_doc)
        return;

    std::unordered_map<std::uint64_t, bool> aliveMeshIds;
    aliveMeshIds.reserve(size_t(m_doc->meshCount()));
    for (int i = 0; i < m_doc->meshCount(); ++i)
        aliveMeshIds.emplace(m_doc->mesh(i).meshId, true);

    // The cache purges a removed layer's own buffers, not those of its UV layouts.
    for (auto it = m_uvLayoutCacheIds.begin(); it != m_uvLayoutCacheIds.end();) {
        if (aliveMeshIds.find(*it & kUvLayoutMeshIdMask) == aliveMeshIds.end()) {
            m_doc->gpuCache().purgeMesh(*it);
            if (m_uvLayout && m_uvLayout->cacheId == *it)
                m_uvLayout.reset();
            it = m_uvLayoutCacheIds.erase(it);
        } else {
            ++it;
        }
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

bool RenderWidget::ensureUvLayout(int meshIndex)
{
    if (!meshHasParametrization(meshIndex)) {
        m_uvLayout.reset();
        return false;
    }
    const Document::MeshEntry &entry = m_doc->mesh(meshIndex);
    const VCGMesh &src = entry.mesh;
    const int group = activeUvTextureGroup();
    const std::uint64_t cacheId = uvLayoutCacheId(entry.meshId, group);
    if (m_uvLayout && m_uvLayout->cacheId == cacheId
        && m_uvLayout->geometryRevision == entry.geometryRevision
        && m_uvLayout->materialRevision == entry.materialRevision) {
        // A selection change only moves flags: copy them across.
        if (m_uvLayout->selectionRevision != entry.selectionRevision) {
            UvLayout &layout = *m_uvLayout;
            for (size_t vi = 0; vi < layout.sourceVertex.size(); ++vi)
                layout.mesh.vert[vi].Flags() = src.vert[size_t(layout.sourceVertex[vi])].cFlags();
            for (size_t fi = 0; fi < layout.sourceFace.size(); ++fi)
                layout.mesh.face[fi].Flags() = src.face[size_t(layout.sourceFace[fi])].cFlags();
            layout.selectionRevision = entry.selectionRevision;
        }
        return m_uvLayout->mesh.FN() > 0;
    }

    m_uvLayout = std::make_unique<UvLayout>();
    UvLayout &layout = *m_uvLayout;
    layout.cacheId = cacheId;
    layout.geometryRevision = entry.geometryRevision;
    layout.materialRevision = entry.materialRevision;
    layout.selectionRevision = entry.selectionRevision;
    layout.ioMask = (entry.ioMask & ~vcg::tri::io::Mask::IOM_VERTTEXCOORD)
        | vcg::tri::io::Mask::IOM_WEDGTEXCOORD;
    if (std::find(m_uvLayoutCacheIds.begin(), m_uvLayoutCacheIds.end(), cacheId)
        == m_uvLayoutCacheIds.end())
        m_uvLayoutCacheIds.push_back(cacheId);

    // One layout vertex per (vertex, u, v): a vertex on a seam becomes one per side.
    struct Corner {
        int vertex;
        float u;
        float v;
        bool operator==(const Corner &o) const { return vertex == o.vertex && u == o.u && v == o.v; }
    };
    struct CornerHash {
        size_t operator()(const Corner &c) const
        {
            return std::hash<int>()(c.vertex) ^ (std::hash<float>()(c.u) * 31u)
                ^ (std::hash<float>()(c.v) * 131u);
        }
    };
    std::unordered_map<Corner, int, CornerHash> vertexOf;
    std::vector<std::array<float, 2>> uvs;
    std::vector<std::array<int, 3>> faces;
    for (size_t fi = 0; fi < src.face.size(); ++fi) {
        const VCGFace &f = src.face[fi];
        // A face with no texture is drawn with group 0, as usedTextureGroups lists it.
        if (f.IsD() || std::max(0, vcgFaceTextureGroup(entry.ioMask, f)) != group)
            continue;
        Corner corners[3];
        bool valid = true;
        for (int c = 0; c < 3 && valid; ++c) {
            corners[c].vertex = int(vcg::tri::Index(src, f.cV(c)));
            valid = vcgFaceCornerUV(entry.ioMask, f, c, corners[c].u, corners[c].v);
        }
        if (!valid)
            continue;
        std::array<int, 3> ids;
        for (int c = 0; c < 3; ++c) {
            const auto [it, added] = vertexOf.try_emplace(corners[c], int(uvs.size()));
            if (added) {
                uvs.push_back({ corners[c].u, corners[c].v });
                layout.sourceVertex.push_back(corners[c].vertex);
            }
            ids[size_t(c)] = it->second;
        }
        faces.push_back(ids);
        layout.sourceFace.push_back(int(fi));
    }
    if (faces.empty())
        return false;

    VCGMesh &m = layout.mesh;
    vcg::tri::Allocator<VCGMesh>::AddVertices(m, uvs.size());
    vcg::tri::Allocator<VCGMesh>::AddFaces(m, faces.size());
    m.face.EnableWedgeTexCoord();
    layout.minUv = QVector2D(uvs[0][0], uvs[0][1]);
    layout.maxUv = layout.minUv;
    for (size_t vi = 0; vi < uvs.size(); ++vi) {
        const VCGVertex &sv = src.vert[size_t(layout.sourceVertex[vi])];
        VCGVertex &v = m.vert[vi];
        v.P() = VCGVertex::CoordType(uvs[vi][0], uvs[vi][1], 0);
        v.N() = VCGVertex::NormalType(0, 0, 1);
        v.C() = sv.cC();
        v.Q() = sv.cQ();
        v.Flags() = sv.cFlags();
        const QVector2D uv(uvs[vi][0], uvs[vi][1]);
        layout.minUv = QVector2D(std::min(layout.minUv.x(), uv.x()), std::min(layout.minUv.y(), uv.y()));
        layout.maxUv = QVector2D(std::max(layout.maxUv.x(), uv.x()), std::max(layout.maxUv.y(), uv.y()));
    }
    for (size_t fi = 0; fi < faces.size(); ++fi) {
        const VCGFace &sf = src.face[size_t(layout.sourceFace[fi])];
        VCGFace &f = m.face[fi];
        for (int c = 0; c < 3; ++c) {
            f.V(c) = &m.vert[size_t(faces[fi][size_t(c)])];
            f.WT(c).U() = uvs[size_t(faces[fi][size_t(c)])][0];
            f.WT(c).V() = uvs[size_t(faces[fi][size_t(c)])][1];
            f.WT(c).N() = short(group);
        }
        f.C() = sf.cC();
        f.Q() = sf.cQ();
        // Selection, the per-edge faux and selected-edge bits.
        f.Flags() = sf.cFlags();
    }
    return true;
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

    if (!fitWholeTexture && !m_uvLayout) {
        m_uvPan = QVector2D(0.5f, 0.5f);
        m_uvZoom = 1.0f;
        m_uvFitRequested = false;
        return;
    }

    const QVector2D minUv = fitWholeTexture ? QVector2D(0.0f, 0.0f) : m_uvLayout->minUv;
    const QVector2D maxUv = fitWholeTexture ? QVector2D(1.0f, 1.0f) : m_uvLayout->maxUv;
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
        && ensureUvLayout(meshIndex);
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

    MeshGpuResourceCache::FillPassView textureFillView {};
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
        MeshGpuResourceCache::Needs needs;
        needs.fill = true;
        ensureMeshGpu(cb, meshIndex, MeshGpuResourceCache::FillVariant::Texture,
                      MeshGpuResourceCache::PointVariant::Constant, needs, meshSettings.wireRespectFaux);
        textureFillView = m_doc->gpuCache().fillPassView(m_rhi, m_doc->mesh(meshIndex).meshId, MeshGpuResourceCache::FillVariant::Texture);
    }

    // Everything else comes from the layer's texture group laid out in UV space, built by the
    // shared cache like any layer, before the pass begins.
    using FillVariant = MeshGpuResourceCache::FillVariant;
    using PointVariant = MeshGpuResourceCache::PointVariant;
    FillVariant uvFillVariant = FillVariant::Constant;
    switch (meshSettings.fillPlain.colorSource) {
    case FillColorSource::PerVertex: uvFillVariant = FillVariant::PerVertex; break;
    case FillColorSource::PerFace: uvFillVariant = FillVariant::PerFace; break;
    case FillColorSource::PerVertexQuality: uvFillVariant = FillVariant::PerVertexQuality; break;
    case FillColorSource::PerFaceQuality: uvFillVariant = FillVariant::PerFaceQuality; break;
    default: break;
    }
    const PointVariant uvPointVariant =
        meshSettings.pointColorSource == PointColorSource::PerVertex ? PointVariant::PerVertex
        : meshSettings.pointColorSource == PointColorSource::PerVertexQuality ? PointVariant::PerVertexQuality
                                                                              : PointVariant::Constant;
    const bool drawPlainFill =
        meshSettings.showFill && !(useTextureDrivenFill && m_uvTextureFillPipeline);
    const bool drawBoundaries = meshSettings.decoratorBoundary
        && (meshSettings.decoratorBoundaryEdges || meshSettings.decoratorTextureSeams);
    if (canDraw) {
        // The scalar colors sample the view's color map, as in 3D.
        QRhiResourceUpdateBatch *lutUpdate = nullptr;
        updateQualityColorMapLut(lutUpdate);
        if (lutUpdate)
            cb->resourceUpdate(lutUpdate);
        MeshGpuResourceCache::Needs needs;
        needs.fill = drawPlainFill;
        needs.wire = meshSettings.showWire || meshSettings.showEdges;
        needs.points = meshSettings.showPoints;
        needs.selection = true;
        needs.decoratorBoundaries = drawBoundaries;
        ensureMeshGpu(cb, meshIndex, uvFillVariant, uvPointVariant, needs,
                      meshSettings.wireRespectFaux, true);
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
            MainUbufMaterialOverrides { normalScale, occlusionStrength, roughnessFactor },
            0,
            0.0f,
            QVector4D(),
            false,
            qualityMapping(meshIndex, styleSettings));
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
        memcpy(decoData, forBackend(mvp).constData(), 16 * sizeof(float));
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
        const MeshGpuResourceCache &cache = m_doc->gpuCache();
        const std::uint64_t layoutId = m_uvLayout->cacheId;

        if (meshSettings.showFill && !drawPlainFill) {
            const MeshGpuResourceCache::FillPassView fillView = textureFillView;
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
        } else if (drawPlainFill) {
            PerMeshRenderSettings fillSettings = meshSettings;
            fillSettings.fillMaterial = FillMaterial::Plain;
            fillSettings.fillLighting = false;
            fillSettings.fillBackfaceCulling = false;
            QRhiGraphicsPipeline *fillPipeline = fillPipelineForSettings(fillSettings);
            const MeshGpuResourceCache::FillPassView fillView =
                cache.fillPassView(m_rhi, layoutId, uvFillVariant);
            if (fillPipeline && fillView.valid) {
                updateStyleUbuf(fillSettings);
                cb->setGraphicsPipeline(fillPipeline);
                setShaderResourcesWithOffset(
                    cb,
                    shaderResourcesForFillTextures(
                        nullptr, nullptr, nullptr, nullptr, m_renderSettings.uvTextureNearestSampling),
                    0);
                for (int bi = 0; bi < fillView.batchCount; ++bi) {
                    if (hasDrawableBatchGeometry(fillView.batches[bi]))
                        drawBatchGeometry(cb, fillView.batches[bi]);
                }
            }
        }

        // The wire overlay of the 3D view; Edges draws the same wire in the edge style.
        const MeshGpuResourceCache::WirePassView wireView = cache.wirePassView(m_rhi, layoutId);
        auto drawWire = [&](const QColor &color, float size) {
            PerMeshRenderSettings wireSettings = meshSettings;
            wireSettings.wireColor = color;
            wireSettings.wireSize = size;
            wireSettings.wireLighting = false;
            wireSettings.wireBackfaceCulling = false;
            QRhiGraphicsPipeline *wirePipeline = wirePipelineForSettings(wireSettings);
            if (!wirePipeline || !wireView.valid)
                return;
            updateStyleUbuf(wireSettings);
            cb->setGraphicsPipeline(wirePipeline);
            setShaderResourcesWithOffset(cb, m_srb.get(), 0);
            const QRhiCommandBuffer::VertexInput binding(wireView.vertexBuffer, 0);
            cb->setVertexInput(0, 1, &binding);
            cb->draw(quint32(wireView.vertexCount));
        };
        if (meshSettings.showWire)
            drawWire(meshSettings.wireColor, meshSettings.wireSize);
        if (meshSettings.showEdges)
            drawWire(meshSettings.edgeColor, meshSettings.edgeSize);

        // The toolbar button gates this pass in the UV view too.
        if (drawBoundaries) {
            const MeshGpuResourceCache::DecoratorPassView decorators =
                cache.decoratorPassView(m_rhi, layoutId);
            const float width = qMax(0.5f, meshSettings.decoratorBoundaryWidth);
            if (meshSettings.decoratorBoundaryEdges)
                drawUvLineSetStable(meshSettings.decoratorBoundaryEdgeColor, width,
                                    decorators.boundaryEdgesBuffer, decorators.boundaryEdgesVertexCount);
            if (meshSettings.decoratorTextureSeams)
                drawUvLineSetStable(meshSettings.decoratorTextureSeamColor, width,
                                    decorators.textureSeamsBuffer, decorators.textureSeamsVertexCount);
        }

        if (meshSettings.showPoints && m_pointsPipeline) {
            const MeshGpuResourceCache::PointsPassView pointsView =
                cache.pointsPassView(m_rhi, layoutId, uvPointVariant);
            if (pointsView.valid && pointsView.vertexCount > 0) {
                PerMeshRenderSettings pointSettings = meshSettings;
                pointSettings.pointLighting = false;
                // No gradient buffer is drawn for the texture view.
                pointSettings.fillPlain.radianceScaling = false;
                updateStyleUbuf(pointSettings);
                cb->setGraphicsPipeline(m_pointsPipeline.get());
                setShaderResourcesWithOffset(cb, m_srb.get(), 0);
                const QRhiCommandBuffer::VertexInput binding(pointsView.vertexBuffer, 0);
                cb->setVertexInput(0, 1, &binding);
                cb->draw(quint32(pointsView.vertexCount));
            }
        }

        // Selection overlay, reusing the 3D scene's selection pipelines so the look matches
        // (translucent red faces + red points), projected with the UV ortho MVP.
        const MeshGpuResourceCache::SelectionPassView selection = cache.selectionPassView(m_rhi, layoutId);
        const bool hasSelFaces = selection.selectedFacesBuffer && selection.selectedFacesVertexCount > 0;
        const bool hasSelVerts = selection.selectedVerticesBuffer && selection.selectedVerticesVertexCount > 0;
        if (m_selectionUbuf && m_selectionSrb && (hasSelFaces || hasSelVerts)) {
            const quint32 selOffset =
                allocateDynamicUbufOffset(m_selectionUbufAllocator, "selection");
            float selData[kDecoratorUbufSize / sizeof(float)] = {};
            memcpy(selData, forBackend(mvp).constData(), 64);
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
                const QRhiCommandBuffer::VertexInput fv(selection.selectedFacesBuffer, 0);
                cb->setVertexInput(0, 1, &fv);
                cb->draw(quint32(selection.selectedFacesVertexCount));
            }
            if (hasSelVerts && m_selectionVerticesPipeline) {
                cb->setGraphicsPipeline(m_selectionVerticesPipeline.get());
                setShaderResourcesWithOffset(cb, m_selectionSrb.get(), selOffset);
                const QRhiCommandBuffer::VertexInput vv(selection.selectedVerticesBuffer, 0);
                cb->setVertexInput(0, 1, &vv);
                cb->draw(quint32(selection.selectedVerticesVertexCount));
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
