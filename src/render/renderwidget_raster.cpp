#include "renderwidget.h"
#include "document.h"
#include "renderwidget_internal.h"
#include <QImage>
#include <QSet>
#include <algorithm>
#include <QVector2D>
#include <QVector4D>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

using namespace RenderWidgetInternal;

namespace {
bool isFinite(const QVector3D &v)
{
    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
}

void appendProjectedRasterVertex(
    std::vector<float> &vertices,
    const QVector3D &position)
{
    vertices.push_back(position.x());
    vertices.push_back(position.y());
    vertices.push_back(position.z());
}

void appendProjectedRasterLine(
    std::vector<float> &vertices,
    const QVector3D &a,
    const QVector3D &b)
{
    appendProjectedRasterVertex(vertices, a);
    appendProjectedRasterVertex(vertices, b);
}

} // namespace

QSize RenderWidget::currentRasterImageSize() const
{
    if (!m_doc)
        return {};

    const int rasterIndex = m_doc->currentRasterIndex();
    if (rasterIndex < 0 || rasterIndex >= m_doc->rasterCount())
        return {};

    Document::RasterEntry &entry = m_doc->raster(rasterIndex);
    if (RasterPlane *plane = entry.currentPlane()) {
        Document::ensureRasterPlaneImage(*plane);
        if (!plane->image.isNull())
            return plane->image.size();
    }
    return entry.shot.viewportPx();
}

QVector2D RenderWidget::rasterScreenToImage(
    const QPointF &screenPos,
    const QSize &pixelSize) const
{
    const QSize imageSize = currentRasterImageSize();
    const QVector4D rect = rasterViewRectNdc(imageSize, pixelSize, m_rasterZoom, m_rasterPan);
    const float halfW = qMax(1e-6f, rect.z());
    const float halfH = qMax(1e-6f, rect.w());
    const float ndcX = 2.0f * (float(screenPos.x()) / float(qMax(1, pixelSize.width()))) - 1.0f;
    const float ndcY = 1.0f - 2.0f * (float(screenPos.y()) / float(qMax(1, pixelSize.height())));
    return QVector2D(
        0.5f + (ndcX - rect.x()) / (2.0f * halfW),
        0.5f - (ndcY - rect.y()) / (2.0f * halfH));
}

void RenderWidget::resetRasterView()
{
    m_rasterPanning = false;
    m_rasterLastMousePos = {};
    m_rasterZoom = 1.0f;
    m_rasterPan = QVector2D(0.5f, 0.5f);
}

void RenderWidget::syncRasterCacheWithDocument()
{
    if (!m_doc) {
        m_rastersGpu.clear();
        return;
    }

    QSet<std::uint64_t> aliveIds;
    for (int i = 0; i < m_doc->rasterCount(); ++i)
        aliveIds.insert(m_doc->raster(i).rasterId);

    for (auto it = m_rastersGpu.begin(); it != m_rastersGpu.end();) {
        if (!aliveIds.contains(it->first))
            it = m_rastersGpu.erase(it);
        else
            ++it;
    }
}

void RenderWidget::ensureRasterResources(
    QRhiCommandBuffer *cb,
    const RenderFramePassRequests &requests)
{
    if (!m_doc || !m_rhi || !cb || !requests.hasRasters()) {
        return;
    }

    syncRasterCacheWithDocument();

    auto rasterFrustumDepth = [&]() {
        bool hasBounds = false;
        QVector3D sceneMin;
        QVector3D sceneMax;
        for (int meshIndex = 0; meshIndex < m_doc->meshCount(); ++meshIndex) {
            if (!meshVisible(meshIndex))
                continue;
            const Document::MeshEntry &meshEntry = m_doc->mesh(meshIndex);
            if (meshEntry.mesh.bbox.IsNull())
                continue;

            const vcg::Box3f &box = meshEntry.mesh.bbox;
            const QVector3D corners[8] = {
                QVector3D(box.min[0], box.min[1], box.min[2]),
                QVector3D(box.max[0], box.min[1], box.min[2]),
                QVector3D(box.min[0], box.max[1], box.min[2]),
                QVector3D(box.max[0], box.max[1], box.min[2]),
                QVector3D(box.min[0], box.min[1], box.max[2]),
                QVector3D(box.max[0], box.min[1], box.max[2]),
                QVector3D(box.min[0], box.max[1], box.max[2]),
                QVector3D(box.max[0], box.max[1], box.max[2])
            };
            for (const QVector3D &corner : corners) {
                const QVector4D transformed = meshEntry.transform * QVector4D(corner, 1.0f);
                const QVector3D worldCorner = (std::abs(transformed.w()) > 1e-8f)
                    ? transformed.toVector3DAffine()
                    : transformed.toVector3D();
                if (!hasBounds) {
                    sceneMin = worldCorner;
                    sceneMax = worldCorner;
                    hasBounds = true;
                    continue;
                }
                sceneMin.setX(std::min(sceneMin.x(), worldCorner.x()));
                sceneMin.setY(std::min(sceneMin.y(), worldCorner.y()));
                sceneMin.setZ(std::min(sceneMin.z(), worldCorner.z()));
                sceneMax.setX(std::max(sceneMax.x(), worldCorner.x()));
                sceneMax.setY(std::max(sceneMax.y(), worldCorner.y()));
                sceneMax.setZ(std::max(sceneMax.z(), worldCorner.z()));
            }
        }

        if (hasBounds)
            return std::max(1e-3f, (sceneMax - sceneMin).length() * 0.12f);

        return std::max(1e-3f, m_trackball.radius() * 0.25f);
    };

    auto buildProjectedVertices = [&](const Document::RasterEntry &entry) {
        std::vector<float> vertices;
        if (!entry.shot.isValid())
            return vertices;

        const QSize viewport = entry.shot.viewportPx();
        if (viewport.width() <= 0 || viewport.height() <= 0)
            return vertices;

        const float depth = rasterFrustumDepth();
        if (!std::isfinite(depth) || depth <= 0.0f)
            return vertices;

        const QVector3D apex = entry.shot.viewPoint();
        const QVector3D bl = entry.shot.unproject(QVector2D(0.0f, 0.0f), depth);
        const QVector3D br = entry.shot.unproject(QVector2D(float(viewport.width()), 0.0f), depth);
        const QVector3D tl = entry.shot.unproject(QVector2D(0.0f, float(viewport.height())), depth);
        const QVector3D tr =
            entry.shot.unproject(QVector2D(float(viewport.width()), float(viewport.height())), depth);
        if (!isFinite(apex) || !isFinite(bl) || !isFinite(br) || !isFinite(tl) || !isFinite(tr))
            return vertices;

        vertices.reserve(kRasterProjectedFrustumVertexCount * kRasterProjectedVertexStrideFloats);
        appendProjectedRasterLine(vertices, apex, bl);
        appendProjectedRasterLine(vertices, apex, br);
        appendProjectedRasterLine(vertices, apex, tl);
        appendProjectedRasterLine(vertices, apex, tr);
        appendProjectedRasterLine(vertices, bl, br);
        appendProjectedRasterLine(vertices, br, tr);
        appendProjectedRasterLine(vertices, tr, tl);
        appendProjectedRasterLine(vertices, tl, bl);
        return vertices;
    };

    QRhiResourceUpdateBatch *updates = nullptr;
    auto ensureUpdates = [&]() {
        if (!updates)
            updates = m_rhi->nextResourceUpdateBatch();
        return updates;
    };

    auto ensureRaster = [&](int rasterIndex, bool projected) {
        if (rasterIndex < 0 || rasterIndex >= m_doc->rasterCount())
            return;

        Document::RasterEntry &entry = m_doc->raster(rasterIndex);
        RasterPlane *plane = entry.currentPlane();
        if (!plane)
            return;
        Document::ensureRasterPlaneImage(*plane);
        if (plane->image.isNull())
            return;

        RasterGpu &gpu = m_rastersGpu[entry.rasterId];

        if (projected) {
            if (!gpu.projectedSrb) {
                if (!m_rasterProjectedUbuf)
                    return;
                gpu.projectedSrb.reset(m_rhi->newShaderResourceBindings());
                gpu.projectedSrb->setBindings({
                    QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                        0,
                        QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                        m_rasterProjectedUbuf.get(),
                        kRasterProjectedUbufSize)
                });
                if (!gpu.projectedSrb->create())
                    gpu.projectedSrb.reset();
            }

            constexpr int vertexBytes =
                kRasterProjectedFrustumVertexCount
                * kRasterProjectedVertexStrideFloats
                * int(sizeof(float));
            if (!gpu.projectedVbuf) {
                gpu.projectedVbuf.reset(
                    m_rhi->newBuffer(
                        QRhiBuffer::Dynamic,
                        QRhiBuffer::VertexBuffer,
                        vertexBytes));
                if (!gpu.projectedVbuf || !gpu.projectedVbuf->create()) {
                    gpu.projectedVbuf.reset();
                    gpu.projectedVertexCount = 0;
                    return;
                }
            }

            const std::vector<float> vertices = buildProjectedVertices(entry);
            if (int(vertices.size())
                != kRasterProjectedFrustumVertexCount * kRasterProjectedVertexStrideFloats) {
                gpu.projectedVertexCount = 0;
                return;
            }
            ensureUpdates()->updateDynamicBuffer(
                gpu.projectedVbuf.get(),
                0,
                vertexBytes,
                vertices.data());
            gpu.projectedVertexCount = kRasterProjectedFrustumVertexCount;
            return;
        }

        if (!m_rasterSampler)
            return;

        QImage uploadImage = plane->image.convertToFormat(QImage::Format_RGBA8888);
        if (uploadImage.isNull() || uploadImage.width() <= 0 || uploadImage.height() <= 0)
            return;

        const bool needsTexture =
            !gpu.texture
            || gpu.imageRevision != entry.imageRevision
            || gpu.planeIndex != entry.currentPlaneIndex
            || gpu.size != uploadImage.size();

        if (needsTexture) {
            gpu.backplateSrb.reset();
            gpu.projectedSrb.reset();
            gpu.texture.reset(
                m_rhi->newTexture(QRhiTexture::RGBA8, uploadImage.size(), 1));
            if (!gpu.texture || !gpu.texture->create()) {
                gpu.texture.reset();
                gpu.imageRevision = 0;
                gpu.planeIndex = -1;
                gpu.size = QSize();
                return;
            }
            gpu.imageRevision = entry.imageRevision;
            gpu.planeIndex = entry.currentPlaneIndex;
            gpu.size = uploadImage.size();

            QRhiTextureUploadEntry textureEntry(
                0, 0, QRhiTextureSubresourceUploadDescription(uploadImage));
            ensureUpdates()->uploadTexture(
                gpu.texture.get(),
                QRhiTextureUploadDescription({ textureEntry }));
        }

        if (!projected && !gpu.backplateSrb && gpu.texture) {
            if (!m_rasterBackplateUbuf)
                return;
            gpu.backplateSrb.reset(m_rhi->newShaderResourceBindings());
            gpu.backplateSrb->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0,
                    QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                    m_rasterBackplateUbuf.get()),
                QRhiShaderResourceBinding::sampledTexture(
                    1,
                    QRhiShaderResourceBinding::FragmentStage,
                    gpu.texture.get(),
                    m_rasterSampler.get())
            });
            if (!gpu.backplateSrb->create())
                gpu.backplateSrb.reset();
        }

    };

    for (int rasterIndex : requests.rasterBackplates)
        ensureRaster(rasterIndex, false);
    for (int rasterIndex : requests.rasterProjected)
        ensureRaster(rasterIndex, true);

    if (updates)
        cb->resourceUpdate(updates);
}

void RenderWidget::renderSceneRasterProjected(
    QRhiCommandBuffer *cb,
    const RenderFramePlan &plan)
{
    if (!cb || plan.rasterProjectedItems.empty() || !m_rasterProjectedPipeline)
        return;

    const bool hasRasterItems = std::any_of(
        plan.rasterProjectedItems.begin(), plan.rasterProjectedItems.end(),
        [](const SceneRasterProjectedDrawItem &item) { return item.rasterIndex >= 0; });
    if (hasRasterItems && !m_rasterProjectedUbuf)
        return;

    cb->setGraphicsPipeline(m_rasterProjectedPipeline.get());
    cb->setViewport(plan.rhiViewport());

    const QMatrix4x4 mvp = plan.proj * plan.view;
    for (const SceneRasterProjectedDrawItem &item : plan.rasterProjectedItems) {
        if (!item.srb || !item.vertexBuffer || item.vertexCount <= 0)
            continue;

        float ubuf[kRasterProjectedUbufSize / sizeof(float)] = {};
        memcpy(ubuf, mvp.constData(), 64);
        if (item.rasterIndex < 0) {
            ubuf[16] = 0.85f;
            ubuf[17] = 0.85f;
            ubuf[18] = 0.85f;
            ubuf[19] = 0.90f;
        } else if (item.isCurrent) {
            ubuf[16] = 1.0f;
            ubuf[17] = 0.82f;
            ubuf[18] = 0.25f;
            ubuf[19] = 0.95f;
        } else {
            ubuf[16] = 0.15f;
            ubuf[17] = 0.65f;
            ubuf[18] = 1.0f;
            ubuf[19] = 0.95f;
        }

        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        quint32 ubufOffset = 0;

        if (item.rasterIndex < 0) {
            // View camera frustum: use its own ubuf at offset 0
            if (!m_viewFrustumVertices.empty()) {
                u->updateDynamicBuffer(m_viewFrustumVbuf.get(), 0,
                    quint32(m_viewFrustumVertices.size() * sizeof(float)),
                    m_viewFrustumVertices.data());
            }
            u->updateDynamicBuffer(m_viewFrustumUbuf.get(), 0,
                kRasterProjectedUbufSize, ubuf);
        } else {
            ubufOffset = allocateDynamicUbufOffset(
                m_rasterProjectedUbufAllocator, "raster-projected");
            u->updateDynamicBuffer(
                m_rasterProjectedUbuf.get(),
                ubufOffset,
                kRasterProjectedUbufSize,
                ubuf);
        }
        cb->resourceUpdate(u);

        setShaderResourcesWithOffset(cb, item.srb, ubufOffset);
        const QRhiCommandBuffer::VertexInput binding(item.vertexBuffer, 0);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(item.vertexCount);
    }
}

void RenderWidget::renderSceneRasterBackplates(
    QRhiCommandBuffer *cb,
    const RenderFramePlan &plan)
{
    if (!cb || plan.rasterBackplateItems.empty()
        || !m_rasterBackplatePipeline || !m_rasterBackplateUbuf) {
        return;
    }

    cb->setGraphicsPipeline(m_rasterBackplatePipeline.get());
    cb->setViewport(plan.rhiViewport());

    for (const SceneRasterBackplateDrawItem &item : plan.rasterBackplateItems) {
        if (!item.srb || item.imageSize.isEmpty())
            continue;

        const QVector4D rect = item.fitToViewport
            ? rasterViewRectNdc(
                item.imageSize,
                plan.pixelSize,
                plan.rasterZoom,
                plan.rasterPan)
            : QVector4D(0.0f, 0.0f, 1.0f, 1.0f);
        float ubuf[kRasterBackplateUbufSize / sizeof(float)] = {};
        ubuf[0] = rect.x();
        ubuf[1] = rect.y();
        ubuf[2] = rect.z();
        ubuf[3] = rect.w();
        ubuf[4] = std::clamp(plan.rasterOpacity, 0.0f, 1.0f);

        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        u->updateDynamicBuffer(
            m_rasterBackplateUbuf.get(),
            0,
            kRasterBackplateUbufSize,
            ubuf);
        cb->resourceUpdate(u);

        cb->setShaderResources(item.srb);
        cb->draw(6);
    }
}
