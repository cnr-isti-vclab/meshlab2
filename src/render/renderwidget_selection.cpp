#include "renderwidget.h"
#include "document.h"
#include "interactivetool.h"
#include "renderwidget_internal.h"
#include <QPointer>

using namespace RenderWidgetInternal;

void RenderWidget::prepareDirtyBuffers(QRhiCommandBuffer *cb)
{
    if (!m_rhi)
        return;

    updateCameraFrameIfNeeded();
    syncPerMeshRenderModesWithDocument();
    const int currentMeshIndex = m_doc ? m_doc->currentMeshIndex() : -1;
    const bool drawCurrentMeshHighlight =
        m_doc
        && m_renderSettings.highlightCurrentMesh
        && currentMeshIndex >= 0
        && currentMeshIndex < m_doc->meshCount();
    const RenderFramePassRequests requests = collectRenderFramePassRequests(-1);
    prepareDirtyBuffers(cb, requests, currentMeshIndex, drawCurrentMeshHighlight);
}

void RenderWidget::prepareDirtyBuffers(
    QRhiCommandBuffer *cb,
    const RenderFramePassRequests &requests,
    int currentMeshIndex,
    bool drawCurrentMeshHighlight)
{
    if (!m_rhi || !cb)
        return;

    if (m_fallbackTextureUploadPending && m_fallbackTexture) {
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        QImage white(1, 1, QImage::Format_RGBA8888);
        white.fill(Qt::white);
        QRhiTextureUploadEntry entry(0, 0, QRhiTextureSubresourceUploadDescription(white));
        u->uploadTexture(m_fallbackTexture.get(), QRhiTextureUploadDescription({ entry }));
        cb->resourceUpdate(u);
        m_fallbackTextureUploadPending = false;
    }
    if (m_fallbackNormalTextureUploadPending && m_fallbackNormalTexture) {
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        QImage flatNormal(1, 1, QImage::Format_RGBA8888);
        flatNormal.fill(QColor(128, 128, 255, 255));
        QRhiTextureUploadEntry entry(0, 0, QRhiTextureSubresourceUploadDescription(flatNormal));
        u->uploadTexture(m_fallbackNormalTexture.get(), QRhiTextureUploadDescription({ entry }));
        cb->resourceUpdate(u);
        m_fallbackNormalTextureUploadPending = false;
    }
    if (m_fallbackOcclusionTextureUploadPending && m_fallbackOcclusionTexture) {
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        QImage white(1, 1, QImage::Format_RGBA8888);
        white.fill(Qt::white);
        QRhiTextureUploadEntry entry(0, 0, QRhiTextureSubresourceUploadDescription(white));
        u->uploadTexture(m_fallbackOcclusionTexture.get(), QRhiTextureUploadDescription({ entry }));
        cb->resourceUpdate(u);
        m_fallbackOcclusionTextureUploadPending = false;
    }
    if (m_fallbackRoughnessTextureUploadPending && m_fallbackRoughnessTexture) {
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        QImage white(1, 1, QImage::Format_RGBA8888);
        white.fill(Qt::white);
        QRhiTextureUploadEntry entry(0, 0, QRhiTextureSubresourceUploadDescription(white));
        u->uploadTexture(m_fallbackRoughnessTexture.get(), QRhiTextureUploadDescription({ entry }));
        cb->resourceUpdate(u);
        m_fallbackRoughnessTextureUploadPending = false;
    }

    if (requests.hasRasters())
        ensureRasterResources(cb, requests);

    if (!requests.hasVisibleMeshes())
        return;
    if (!requests.hasMeshResourceRequests() && !drawCurrentMeshHighlight)
        return;

    for (const RenderMeshPassRequests &meshRequest : requests.meshes) {
        const int mi = meshRequest.meshIndex;
        const PerMeshRenderSettings &meshSettings = meshRequest.meshSettings;
        const bool needHighlightForMesh = drawCurrentMeshHighlight && mi == currentMeshIndex;
        const bool needResourcesForMesh =
            meshRequest.hasMeshResourceRequests()
            || needHighlightForMesh;
        if (!needResourcesForMesh)
            continue;
        const auto pointVariant = static_cast<Document::PointGpuVariant>(
            pointGpuVariantIndexForSettings(meshSettings));
        const auto fillVariant = static_cast<Document::FillGpuVariant>(
            fillGpuVariantIndexForSettings(meshSettings));
        m_doc->ensureMeshGpuResources(
            m_rhi,
            cb,
            mi,
            fillVariant,
            pointVariant,
            meshRequest.fill || needHighlightForMesh,
            meshRequest.wire,
            meshRequest.edges || needHighlightForMesh,
            meshRequest.points || needHighlightForMesh,
            meshRequest.boundingBox,
            meshRequest.decoratorNormals,
            meshRequest.decoratorBoundaries,
            m_renderSettings.qualityHistogramFixedRange,
            m_renderSettings.qualityHistogramMin,
            m_renderSettings.qualityHistogramMax,
            meshRequest.selection,
            meshSettings.wireRespectFaux,
            m_renderSettings.qualityHistogramCenterOnZero,
            m_renderSettings.qualityHistogramPercentileCrop);
    }
}

void RenderWidget::executePendingDepthPick(
    QRhiCommandBuffer *cb,
    const QSize &pixelSize,
    const ViewTile &tile)
{
    if (!m_depthPickPending || m_depthPickInFlight || !m_rhi || !cb || pixelSize.isEmpty())
        return;

    ensureDepthPickResources(pixelSize);
    if (!m_depthPickRt || !m_depthPickTexture || !m_depthPickSrb)
        return;
    if (!m_depthPickFillPipeline && !m_depthPickPointsPipeline)
        return;

    const float sx = float(pixelSize.width()) / float(qMax(1, width()));
    const float sy = float(pixelSize.height()) / float(qMax(1, height()));
    const int px = std::clamp(
        int(std::floor((float(m_depthPickPos.x()) + 0.5f) * sx)),
        0,
        pixelSize.width() - 1);
    const int pyScreen = std::clamp(
        int(std::floor((float(m_depthPickPos.y()) + 0.5f) * sy)),
        0,
        pixelSize.height() - 1);
    const int pyRead = m_rhi->isYUpInFramebuffer()
        ? (pixelSize.height() - 1 - pyScreen)
        : pyScreen;
    const bool yUpInNdc = m_rhi->isYUpInNDC();
    const bool clipDepthZeroToOne = m_rhi->isClipDepthZeroToOne();

    const auto fillVariant = Document::FillGpuVariant::Constant;

    // The pick target covers the whole view, but only the clicked tile is drawn into it, at
    // the tile's own place and through the tile's own projection. The readback coordinate is
    // therefore unchanged: it is still the pixel under the cursor.
    const float aspect = tile.rect.width() / float(qMax(1, tile.rect.height()));
    QMatrix4x4 proj = m_trackball.projectionMatrix(aspect);
    const QMatrix4x4 view = m_trackball.viewMatrix();
    const QMatrix4x4 vp = proj * view;

    for (int mi = 0; mi < m_doc->meshCount(); ++mi) {
        if (!meshVisible(mi) || !tileShowsMesh(tile, mi))
            continue;
        const PerMeshRenderSettings meshSettings = renderModeForMesh(mi);
        const auto pointVariant = static_cast<Document::PointGpuVariant>(
            pointGpuVariantIndexForSettings(meshSettings));
        m_doc->ensureMeshGpuResources(
            m_rhi,
            cb,
            mi,
            fillVariant,
            pointVariant,
            true,   // fill
            false,  // wire
            false,  // edges
            true,   // points
            false,  // bbox
            false,  // decorator normals
            false,  // decorator boundaries
            m_renderSettings.qualityHistogramFixedRange,
            m_renderSettings.qualityHistogramMin,
            m_renderSettings.qualityHistogramMax);
    }

    cb->beginPass(m_depthPickRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
    cb->setViewport(rhiViewportFor(tile.rect, pixelSize));

    for (int mi = 0; mi < m_doc->meshCount(); ++mi) {
        if (!meshVisible(mi) || !tileShowsMesh(tile, mi))
            continue;

        const PerMeshRenderSettings meshSettings = renderModeForMesh(mi);
        const auto pointVariant = static_cast<Document::PointGpuVariant>(
            pointGpuVariantIndexForSettings(meshSettings));

        const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
        // Encode the mesh id into alpha so the readback yields both depth and
        // which mesh was hit. id 0 is background, so meshes map to mi+1.
        const float pickId = float(mi + 1) / 255.0f;
        uploadMainUbufForMesh(
            cb, mi, proj, view, meshSettings, tile.rect.size(), true,
            QVector3D(0.0f, 0.0f, 1.0f), MainUbufMaterialOverrides{}, ubufOffset, pickId);

        if (m_depthPickFillPipeline) {
            const Document::FillPassGpuView fillView =
                m_doc->fillPassGpuView(m_rhi, mi, fillVariant);
            cb->setGraphicsPipeline(m_depthPickFillPipeline.get());
            setShaderResourcesWithOffset(cb, m_depthPickSrb.get(), ubufOffset);
            for (int bi = 0; bi < fillView.batchCount; ++bi) {
                const auto &batch = fillView.batches[bi];
                if (!hasDrawableBatchGeometry(batch))
                    continue;
                drawBatchGeometry(cb, batch);
            }
        }

        if (m_depthPickPointsPipeline) {
            const Document::PointsPassGpuView pointsView =
                m_doc->pointsPassGpuView(m_rhi, mi, pointVariant);
            if (pointsView.valid && pointsView.vertexBuffer && pointsView.vertexCount > 0) {
                cb->setGraphicsPipeline(m_depthPickPointsPipeline.get());
                setShaderResourcesWithOffset(cb, m_depthPickSrb.get(), ubufOffset);
                const QRhiCommandBuffer::VertexInput pv(pointsView.vertexBuffer, 0);
                cb->setVertexInput(0, 1, &pv);
                cb->draw(pointsView.vertexCount);
            }
        }
    }

    cb->endPass();

    QMatrix4x4 invMvp;
    bool invOk = false;
    invMvp = vp.inverted(&invOk);
    if (!invOk)
        return;

    QPointer<RenderWidget> self(this);
    const int pickSequence = m_depthPickSequence; /* capture at submit time */
    const PickPurpose purpose = m_depthPickPurpose;
    // The readback pixel is addressed in the whole pick target, but the geometry was
    // projected through the tile's viewport, so it is the tile that normalized device
    // coordinates span. Unprojecting against the target instead puts the recovered point
    // somewhere else entirely once a tile is smaller than the view.
    const QRect pickViewport = tile.rect;
    m_depthPickReadbackResult = std::make_unique<QRhiReadbackResult>();
    m_depthPickReadbackResult->completed =
        [self, pickSequence, purpose, invMvp, px, pyScreen, pickViewport, yUpInNdc, clipDepthZeroToOne]() {
        if (!self)
            return;
        const QByteArray data =
            self->m_depthPickReadbackResult ? self->m_depthPickReadbackResult->data : QByteArray();
        const QRhiTexture::Format format = self->m_depthPickReadbackResult
            ? self->m_depthPickReadbackResult->format
            : QRhiTexture::UnknownFormat;
        QMetaObject::invokeMethod(
            self,
            [self, pickSequence, purpose, data, format, invMvp, px, pyScreen, pickViewport, yUpInNdc, clipDepthZeroToOne]() {
            if (!self)
                return;
            /* Stale-callback guard: a newer double-click incremented the sequence. */
            if (pickSequence != self->m_depthPickSequence) {
                self->m_depthPickInFlight = false;
                self->m_depthPickReadbackResult.reset();
                if (self->m_depthPickPending)
                    self->update();
                return;
            }
            self->m_depthPickInFlight = false;
            self->m_depthPickReadbackResult.reset();

            if (self->m_depthPickPending)
                self->update();

            if (data.size() < 4)
                return;

            const uchar *pxData = reinterpret_cast<const uchar *>(data.constData());
            // Alpha encodes the picked mesh id (meshIndex+1); 0 means background.
            const int idByte = int(pxData[3]);
            const bool hit = (idByte != 0);
            const int meshIndex = hit ? (idByte - 1) : -1;

            QVector3D worldPos;
            bool haveWorldPos = false;
            if (hit) {
                const bool bgraOrder = (format == QRhiTexture::BGRA8);
                const float depth01 = decodePackedDepthRgb8(pxData, bgraOrder);
                if (depth01 > 0.0f && depth01 < 1.0f) {
                    const float tileX = float(px - pickViewport.x()) + 0.5f;
                    const float tileY = float(pyScreen - pickViewport.y()) + 0.5f;
                    const float ndcX =
                        (2.0f * tileX / float(qMax(1, pickViewport.width()))) - 1.0f;
                    const float y01 = tileY / float(qMax(1, pickViewport.height()));
                    const float ndcY = yUpInNdc ? (1.0f - 2.0f * y01) : (2.0f * y01 - 1.0f);
                    const float ndcZ = clipDepthZeroToOne ? depth01 : (depth01 * 2.0f - 1.0f);
                    QVector4D world = invMvp * QVector4D(ndcX, ndcY, ndcZ, 1.0f);
                    if (std::abs(world.w()) >= 1e-8f) {
                        world /= world.w();
                        worldPos = world.toVector3D();
                        haveWorldPos = true;
                    }
                }
            }

            if (purpose == PickPurpose::Recenter) {
                // Recenter needs a valid surface point; ignore background clicks.
                if (haveWorldPos) {
                    self->startCenterAnimation(worldPos);
                    emit self->trackballCenterPicked(worldPos);
                }
            } else if (self->m_activeTool) {
                SurfacePick result;
                result.hit = hit && haveWorldPos;
                result.meshIndex = result.hit ? meshIndex : -1;
                result.worldPos = worldPos;
                self->m_activeTool->onSurfacePicked(result);
                self->updateToolBadge();
                // A pick may change GPU-rendered tool geometry (for example a
                // measurement rubber band), not only its QPainter overlay.
                self->update();
            }
        },
            Qt::QueuedConnection);
    };

    QRhiReadbackDescription rb(m_depthPickTexture.get());
    rb.setRect(QRect(px, pyRead, 1, 1));

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->readBackTexture(rb, m_depthPickReadbackResult.get());
    cb->resourceUpdate(u);

    m_depthPickPending = false;
    m_depthPickInFlight = true;
}

void RenderWidget::renderCurrentMeshMask(
    QRhiCommandBuffer *cb,
    const QSize &pixelSize,
    const ViewTile &tile)
{
    if (!m_renderSettings.highlightCurrentMesh)
        return;

    m_currentMaskFromPoints = false;

    const int currentMeshIndex = m_doc->currentMeshIndex();
    if (currentMeshIndex < 0)
        return;

    ensureCurrentMeshMaskResources(pixelSize);
    if (!m_currentMaskRt || !m_currentMaskBaseRt || !m_currentMaskWorkRt)
        return;

    // The mask buffers span the whole view and the morphology and outline passes below read
    // them full-screen; only the geometry goes in at the tile, so the outline lands on the
    // current layer wherever its tile is.
    const float aspect = tile.rect.width() / float(qMax(1, tile.rect.height()));
    QMatrix4x4 proj = m_trackball.projectionMatrix(aspect);
    const QMatrix4x4 view = m_trackball.viewMatrix();
    const QRhiViewport tileViewport = rhiViewportFor(tile.rect, pixelSize);

    const PerMeshRenderSettings currentMeshSettings = renderModeForMesh(currentMeshIndex);
    const MeshRenderMode currentMeshMode = renderModeForMesh(currentMeshIndex);
    const auto pointVariant = static_cast<Document::PointGpuVariant>(
        pointGpuVariantIndexForSettings(currentMeshSettings));
    const auto fillVariant = static_cast<Document::FillGpuVariant>(
        fillGpuVariantIndexForSettings(currentMeshSettings));

    const Document::FillPassGpuView currentFillView =
        m_doc->fillPassGpuView(m_rhi, currentMeshIndex, fillVariant);
    const Document::EdgePassGpuView currentEdgeView =
        m_doc->edgePassGpuView(m_rhi, currentMeshIndex);
    const Document::EdgeFatPassGpuView currentEdgeFatView =
        m_doc->edgeFatPassGpuView(m_rhi, currentMeshIndex);
    const Document::PointsPassGpuView currentPointsView =
        m_doc->pointsPassGpuView(m_rhi, currentMeshIndex, pointVariant);

    bool currentHasFill = false;
    if (currentMeshMode.showFill) {
        for (int bi = 0; bi < currentFillView.batchCount; ++bi) {
            const auto &batch = currentFillView.batches[bi];
            if (hasDrawableBatchGeometry(batch)) {
                currentHasFill = true;
                break;
            }
        }
    }
    const bool currentHasEdges =
        currentMeshMode.showEdges
        && ((currentEdgeFatView.valid
             && currentEdgeFatView.vertexBuffer
             && currentEdgeFatView.vertexCount > 0)
            || (currentEdgeView.valid
                && currentEdgeView.vertexBuffer
                && currentEdgeView.vertexCount > 0));
    const bool currentHasPoints =
        currentMeshMode.showPoints
        && currentPointsView.valid
        && currentPointsView.vertexBuffer
        && currentPointsView.vertexCount > 0;

    if (!currentHasFill && !currentHasEdges && currentHasPoints) {
        // Keep the point-cloud outline path unchanged.
        cb->beginPass(m_currentMaskRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
        cb->setViewport(tileViewport);
        if (m_currentMaskPointsPipeline) {
            const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
            uploadMainUbufForMesh(
                cb,
                currentMeshIndex,
                proj,
                view,
                currentMeshSettings,
                tile.rect.size(),
                true,
                QVector3D(0.0f, 0.0f, 1.0f),
                MainUbufMaterialOverrides{},
                ubufOffset);
            cb->setGraphicsPipeline(m_currentMaskPointsPipeline.get());
            setShaderResourcesWithOffset(cb, m_srb.get(), ubufOffset);
            const QRhiCommandBuffer::VertexInput pv(currentPointsView.vertexBuffer, 0);
            cb->setVertexInput(0, 1, &pv);
            cb->draw(currentPointsView.vertexCount);
            m_currentMaskFromPoints = true;
        }
        cb->endPass();
        return;
    }

    auto drawFillDepth = [&](int meshIndex,
                             const PerMeshRenderSettings &meshSettings,
                             const Document::FillPassGpuView &fillView) {
        if (!m_currentMaskFillDepthOnlyPipeline)
            return;
        const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
        uploadMainUbufForMesh(
            cb,
            meshIndex,
            proj,
            view,
            meshSettings,
            tile.rect.size(),
            true,
            QVector3D(0.0f, 0.0f, 1.0f),
            MainUbufMaterialOverrides{},
            ubufOffset,
            1.0f);
        cb->setGraphicsPipeline(m_currentMaskFillDepthOnlyPipeline.get());
        setShaderResourcesWithOffset(cb, m_srb.get(), ubufOffset);
        for (int bi = 0; bi < fillView.batchCount; ++bi) {
            const auto &batch = fillView.batches[bi];
            if (!hasDrawableBatchGeometry(batch))
                continue;
            drawBatchGeometry(cb, batch);
        }
    };

    auto drawEdgeDepth = [&](int meshIndex,
                             const PerMeshRenderSettings &meshSettings,
                             const Document::EdgePassGpuView &edgeView,
                             const Document::EdgeFatPassGpuView &fatEdgeView) {
        const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
        uploadMainUbufForMesh(
            cb,
            meshIndex,
            proj,
            view,
            meshSettings,
            tile.rect.size(),
            true,
            QVector3D(0.0f, 0.0f, 1.0f),
            MainUbufMaterialOverrides{},
            ubufOffset,
            1.0f);
        if (m_currentMaskFatEdgesDepthOnlyPipeline
            && fatEdgeView.valid
            && fatEdgeView.vertexBuffer
            && fatEdgeView.vertexCount > 0) {
            const float wireParams[4] = {
                meshSettings.wireSize,
                qMax(1.0f, meshSettings.edgeSize),
                1.0f / float(qMax(1, pixelSize.width())),
                1.0f / float(qMax(1, pixelSize.height()))
            };
            QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
            u->updateDynamicBuffer(
                m_ubuf.get(),
                ubufOffset + kUbufWireParamsOffset * int(sizeof(float)),
                int(sizeof(wireParams)),
                wireParams);
            cb->resourceUpdate(u);

            cb->setGraphicsPipeline(m_currentMaskFatEdgesDepthOnlyPipeline.get());
            setShaderResourcesWithOffset(cb, m_srb.get(), ubufOffset);
            const QRhiCommandBuffer::VertexInput ev(fatEdgeView.vertexBuffer, 0);
            cb->setVertexInput(0, 1, &ev);
            cb->draw(fatEdgeView.vertexCount);
            return;
        }

        if (!m_currentMaskEdgesDepthOnlyPipeline)
            return;
        if (!edgeView.valid || !edgeView.vertexBuffer || edgeView.vertexCount <= 0)
            return;
        cb->setGraphicsPipeline(m_currentMaskEdgesDepthOnlyPipeline.get());
        setShaderResourcesWithOffset(cb, m_srb.get(), ubufOffset);
        const QRhiCommandBuffer::VertexInput ev(edgeView.vertexBuffer, 0);
        cb->setVertexInput(0, 1, &ev);
        cb->draw(edgeView.vertexCount);
    };

    auto drawPointsDepth = [&](int meshIndex,
                               const PerMeshRenderSettings &meshSettings,
                               const Document::PointsPassGpuView &pointsView) {
        if (!m_currentMaskPointsDepthOnlyPipeline)
            return;
        if (!pointsView.valid || !pointsView.vertexBuffer || pointsView.vertexCount <= 0)
            return;
        const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
        uploadMainUbufForMesh(
            cb,
            meshIndex,
            proj,
            view,
            meshSettings,
            tile.rect.size(),
            true,
            QVector3D(0.0f, 0.0f, 1.0f),
            MainUbufMaterialOverrides{},
            ubufOffset,
            1.0f);
        cb->setGraphicsPipeline(m_currentMaskPointsDepthOnlyPipeline.get());
        setShaderResourcesWithOffset(cb, m_srb.get(), ubufOffset);
        const QRhiCommandBuffer::VertexInput pv(pointsView.vertexBuffer, 0);
        cb->setVertexInput(0, 1, &pv);
        cb->draw(pointsView.vertexCount);
    };

    // 1) Render current mesh into buffer A (encoded depth in color, depth test/write enabled).
    cb->beginPass(m_currentMaskBaseRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
    cb->setViewport(tileViewport);
    if (currentHasFill) {
        drawFillDepth(currentMeshIndex, currentMeshSettings, currentFillView);
    } else if (currentHasEdges) {
        drawEdgeDepth(currentMeshIndex, currentMeshSettings, currentEdgeView, currentEdgeFatView);
    } else if (currentHasPoints) {
        drawPointsDepth(currentMeshIndex, currentMeshSettings, currentPointsView);
    }
    cb->endPass();

    // 2) Compute outline boundary O from A by removing pixels surrounded by 8 neighbors.
    cb->beginPass(m_currentMaskWorkRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
    cb->setViewport({ 0, 0, float(pixelSize.width()), float(pixelSize.height()) });
    if (m_outlineExtractPipeline && m_outlineExtractSrb && m_outlineExtractUbuf) {
        const float extractData[4] = {
            1.0f / float(qMax(1, pixelSize.width())),
            1.0f / float(qMax(1, pixelSize.height())),
            m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f,
            0.0f
        };
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        u->updateDynamicBuffer(m_outlineExtractUbuf.get(), 0, kOutlineExtractUbufSize, extractData);
        cb->resourceUpdate(u);

        cb->setGraphicsPipeline(m_outlineExtractPipeline.get());
        cb->setShaderResources(m_outlineExtractSrb.get());
        cb->draw(3);
    }
    cb->endPass();

    // 3) Render whole scene depth into a second depth-encoded buffer. In the grid the tile
    // holds only the current layer, so nothing occludes it and this draws nothing.
    cb->beginPass(m_currentMaskRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
    cb->setViewport(tileViewport);
    for (int mi = 0; mi < m_doc->meshCount(); ++mi) {
        if (!meshVisible(mi) || !tileShowsMesh(tile, mi))
            continue;
        if (mi == currentMeshIndex)
            continue;
        const MeshRenderMode mode = renderModeForMesh(mi);
        const PerMeshRenderSettings meshSettings = renderModeForMesh(mi);

        const Document::FillPassGpuView fillView =
            m_doc->fillPassGpuView(
                m_rhi,
                mi,
                static_cast<Document::FillGpuVariant>(
                    fillGpuVariantIndexForSettings(meshSettings)));
        const Document::EdgePassGpuView edgeView = m_doc->edgePassGpuView(m_rhi, mi);
        const Document::EdgeFatPassGpuView edgeFatView = m_doc->edgeFatPassGpuView(m_rhi, mi);
        const Document::PointsPassGpuView pointsView =
            m_doc->pointsPassGpuView(
                m_rhi,
                mi,
                static_cast<Document::PointGpuVariant>(
                    pointGpuVariantIndexForSettings(meshSettings)));

        if (mode.showFill)
            drawFillDepth(mi, meshSettings, fillView);
        if (mode.showEdges)
            drawEdgeDepth(mi, meshSettings, edgeView, edgeFatView);
        if (mode.showPoints)
            drawPointsDepth(mi, meshSettings, pointsView);
    }
    cb->endPass();
}

void RenderWidget::processCurrentMeshMask(QRhiCommandBuffer *cb, const QSize &pixelSize)
{
    if (!m_renderSettings.highlightCurrentMesh)
        return;
    if (!m_currentMaskRt || !m_currentMaskBaseRt || !m_currentMaskWorkRt)
        return;
    if (!m_maskMorphCopyUbuf
        || !m_maskMorphDilateUbuf
        || !m_maskMorphErodeUbuf
        || !m_maskMorphMaskToBaseSrb
        || !m_maskMorphMaskToWorkSrb
        || !m_maskMorphWorkToMaskSrb)
        return;
    if (!m_maskMorphToBasePipeline
        || !m_maskMorphToWorkPipeline
        || !m_maskMorphWorkToMaskPipeline)
        return;

    const float invW = 1.0f / float(qMax(1, pixelSize.width()));
    const float invH = 1.0f / float(qMax(1, pixelSize.height()));

    auto updateMorphParams = [&](QRhiBuffer *ubuf, float mode, float radiusPx) {
        // mode: 0 = dilate, 1 = erode
        const float params[4] = { invW, invH, radiusPx, mode };
        QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
        u->updateDynamicBuffer(ubuf, 0, kMaskMorphUbufSize, params);
        cb->resourceUpdate(u);
    };

    if (!m_currentMaskFromPoints)
        return;

    // Snapshot base point mask for debugging/inspection before any morphology.
    updateMorphParams(m_maskMorphCopyUbuf.get(), 0.0f, 0.0f);
    cb->beginPass(m_currentMaskBaseRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
    cb->setGraphicsPipeline(m_maskMorphToBasePipeline.get());
    cb->setViewport({ 0, 0, float(pixelSize.width()), float(pixelSize.height()) });
    cb->setShaderResources(m_maskMorphMaskToBaseSrb.get());
    cb->draw(3);
    cb->endPass();

    // Point-cloud outline pipeline:
    // 1) dilate(base, dilateRadius) -> work
    // 2) erode(work, erodeRadius) -> mask
    // 3) edge filter on final mask texture
    // This keeps mask rasterization cheap (1px points) and pushes expansion to screen space.
    const float dilateRadiusPx = qMax(0.0f, m_renderSettings.currentMeshDilateRadius);
    const float erosionRadiusPx = qBound(
        0.0f,
        m_renderSettings.currentMeshErodeRadius,
        dilateRadiusPx);
    if (dilateRadiusPx <= 0.0f && erosionRadiusPx <= 0.0f)
        return;

    updateMorphParams(m_maskMorphDilateUbuf.get(), 0.0f, dilateRadiusPx);
    cb->beginPass(m_currentMaskWorkRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
    cb->setGraphicsPipeline(m_maskMorphToWorkPipeline.get());
    cb->setViewport({ 0, 0, float(pixelSize.width()), float(pixelSize.height()) });
    cb->setShaderResources(m_maskMorphMaskToWorkSrb.get());
    cb->draw(3);
    cb->endPass();

    updateMorphParams(m_maskMorphErodeUbuf.get(), 1.0f, erosionRadiusPx);
    cb->beginPass(m_currentMaskRt.get(), Qt::transparent, { 1.0f, 0 }, nullptr);
    cb->setGraphicsPipeline(m_maskMorphWorkToMaskPipeline.get());
    cb->setViewport({ 0, 0, float(pixelSize.width()), float(pixelSize.height()) });
    cb->setShaderResources(m_maskMorphWorkToMaskSrb.get());
    cb->draw(3);
    cb->endPass();
}

void RenderWidget::drawCurrentMeshDebugView(QRhiCommandBuffer *cb, const QSize &pixelSize)
{
    if (!m_maskDebugPipeline || !m_maskDebugUbuf)
        return;

    QRhiShaderResourceBindings *srb = nullptr;
    float debugMode = 0.0f;
    switch (m_renderSettings.currentMeshDebugView) {
    case CurrentMeshDebugView::Outline:
        return;
    case CurrentMeshDebugView::FullMask:
        srb = m_maskDebugBaseSrb.get();
        break;
    case CurrentMeshDebugView::VisibleMask:
        srb = m_maskDebugMaskSrb.get();
        break;
    case CurrentMeshDebugView::OccludedMask:
        srb = m_maskDebugBaseSrb.get();
        debugMode = 1.0f;
        break;
    case CurrentMeshDebugView::DilatedMask:
        srb = m_currentMaskFromPoints ? m_maskDebugWorkSrb.get() : m_maskDebugBaseSrb.get();
        break;
    case CurrentMeshDebugView::ErodedMask:
        srb = m_maskDebugMaskSrb.get();
        break;
    }
    if (!srb)
        return;

    const float debugData[4] = {
        1.0f / float(qMax(1, pixelSize.width())),
        1.0f / float(qMax(1, pixelSize.height())),
        m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f,
        debugMode
    };
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_maskDebugUbuf.get(), 0, kMaskDebugUbufSize, debugData);
    cb->resourceUpdate(u);

    cb->setGraphicsPipeline(m_maskDebugPipeline.get());
    cb->setViewport({ 0, 0, float(pixelSize.width()), float(pixelSize.height()) });
    cb->setShaderResources(srb);
    cb->draw(3);
}

void RenderWidget::drawCurrentMeshOutline(QRhiCommandBuffer *cb, const QSize &pixelSize)
{
    if (!m_renderSettings.highlightCurrentMesh)
        return;

    if (m_renderSettings.currentMeshDebugView != CurrentMeshDebugView::Outline) {
        drawCurrentMeshDebugView(cb, pixelSize);
        return;
    }

    if (!m_outlinePipeline || !m_outlineSrb || !m_outlineUbuf)
        return;
    if (!m_currentMaskTexture || !m_currentMaskBaseTexture || !m_currentMaskWorkTexture)
        return;

    // For point-cloud highlighting, outline width is controlled by dilate/erode radius difference.
    // Keep edge extraction local and stable.
    const float widthPx = m_currentMaskFromPoints
        ? 1.0f
        : qMax(1.0f, m_renderSettings.currentMeshOutlineWidth);
    float outlineData[12] = {};
    outlineData[0] = m_renderSettings.currentMeshOutlineColor.redF();
    outlineData[1] = m_renderSettings.currentMeshOutlineColor.greenF();
    outlineData[2] = m_renderSettings.currentMeshOutlineColor.blueF();
    outlineData[3] = m_renderSettings.currentMeshOutlineColor.alphaF();
    outlineData[4] = widthPx;
    outlineData[5] = 1.0f / float(qMax(1, pixelSize.width()));
    outlineData[6] = 1.0f / float(qMax(1, pixelSize.height()));
    // Offscreen texture sampling needs a vertical flip on Y-up framebuffers.
    outlineData[7] = m_rhi->isYUpInFramebuffer() ? 1.0f : 0.0f;
    if (m_currentMaskFromPoints) {
        // Keep point-cloud outline behavior unchanged: no occlusion tinting.
        outlineData[8] = m_renderSettings.currentMeshOutlineColor.redF();
        outlineData[9] = m_renderSettings.currentMeshOutlineColor.greenF();
        outlineData[10] = m_renderSettings.currentMeshOutlineColor.blueF();
        outlineData[11] = 0.0f;
    } else {
        // Occluded outline blends 50% with the current framebuffer color.
        outlineData[8] = m_renderSettings.currentMeshOutlineColor.redF();
        outlineData[9] = m_renderSettings.currentMeshOutlineColor.greenF();
        outlineData[10] = m_renderSettings.currentMeshOutlineColor.blueF();
        outlineData[11] = m_renderSettings.currentMeshOutlineColor.alphaF() * 0.5f;
    }

    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_outlineUbuf.get(), 0, kOutlineUbufSize, outlineData);
    cb->resourceUpdate(u);

    cb->setGraphicsPipeline(m_outlinePipeline.get());
    cb->setViewport({ 0, 0, float(pixelSize.width()), float(pixelSize.height()) });
    cb->setShaderResources(m_outlineSrb.get());
    cb->draw(3);
}
