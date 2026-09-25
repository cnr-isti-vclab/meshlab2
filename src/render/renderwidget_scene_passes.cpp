#include "renderwidget.h"
#include "clipplane.h"
#include "document.h"
#include "renderwidget_internal.h"
#include <cstring>

using namespace RenderWidgetInternal;

void RenderWidget::renderSceneBufferItems(
    QRhiCommandBuffer *cb,
    const RenderFramePlan &plan,
    const std::vector<SceneBufferDrawItem> &items)
{
    if (items.empty())
        return;

    cb->setViewport(plan.rhiViewport());
    for (const SceneBufferDrawItem &item : items) {
        if (!item.pipeline || !item.vertexBuffer || item.vertexCount <= 0)
            continue;
        cb->setGraphicsPipeline(item.pipeline);
        const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
        uploadMainUbufForMesh(
            cb,
            item.meshIndex,
            plan.proj,
            plan.view,
            item.meshSettings,
            plan.pixelSize,
            true,
            plan.lightDir,
            MainUbufMaterialOverrides{},
            ubufOffset);
        setShaderResourcesWithOffset(cb, m_srb.get(), ubufOffset);
        const QRhiCommandBuffer::VertexInput binding(item.vertexBuffer, 0);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(quint32(item.vertexCount), 1, quint32(item.firstVertex));
    }
}

void RenderWidget::renderSceneDecoratorItems(
    QRhiCommandBuffer *cb,
    const RenderFramePlan &plan)
{
    if (plan.decoratorItems.empty())
        return;

    const QSize &sz = plan.pixelSize;
    const QMatrix4x4 frameVp = plan.proj * plan.view;
    auto uploadDecoratorColor = [&](const SceneDecoratorDrawItem &item) -> bool {
        if (item.slot < 0 || item.slot >= kDecoratorSlotCount)
            return false;
        if (item.meshIndex < 0 || item.meshIndex >= m_doc->meshCount())
            return false;
        QRhiBuffer *decoratorUbuf = m_decoratorUbufs[item.slot].get();
        QRhiShaderResourceBindings *decoratorSrb = m_decoratorSrbs[item.slot].get();
        if (!decoratorUbuf || !decoratorSrb)
            return false;
        const quint32 ubufOffset = allocateDynamicUbufOffset(
            m_decoratorUbufAllocators[size_t(item.slot)],
            "decorator");
        float decoratorData[kDecoratorUbufSize / sizeof(float)] = {};
        const QMatrix4x4 meshMvp = frameVp * m_doc->mesh(item.meshIndex).transform;
        memcpy(decoratorData, meshMvp.constData(), 64);
        decoratorData[16] = item.color.redF();
        decoratorData[17] = item.color.greenF();
        decoratorData[18] = item.color.blueF();
        decoratorData[19] = item.color.alphaF();
        const QVector4D localClip = localClipPlaneFor(item.meshIndex);
        decoratorData[20] = localClip.x();
        decoratorData[21] = localClip.y();
        decoratorData[22] = localClip.z();
        decoratorData[23] = localClip.w();
        QRhiResourceUpdateBatch *uDecor = m_rhi->nextResourceUpdateBatch();
        uDecor->updateDynamicBuffer(
            decoratorUbuf, ubufOffset, kDecoratorUbufSize, decoratorData);
        cb->resourceUpdate(uDecor);
        setShaderResourcesWithOffset(cb, decoratorSrb, ubufOffset);
        return true;
    };
    auto uploadDecoratorFat = [&](const SceneDecoratorDrawItem &item) -> bool {
        if (item.meshIndex < 0 || item.meshIndex >= m_doc->meshCount())
            return false;
        if (!m_decoratorFatUbuf || !m_decoratorFatSrb)
            return false;
        const quint32 ubufOffset = allocateDynamicUbufOffset(
            m_decoratorFatUbufAllocator,
            "decorator-fat");
        float fatData[kDecoratorFatUbufSize / sizeof(float)] = {};
        const QMatrix4x4 meshMvp = frameVp * m_doc->mesh(item.meshIndex).transform;
        memcpy(fatData, meshMvp.constData(), 64);
        fatData[16] = item.color.redF();
        fatData[17] = item.color.greenF();
        fatData[18] = item.color.blueF();
        fatData[19] = item.color.alphaF();
        fatData[20] = qMax(0.5f, item.width);
        fatData[21] = 1.0f / float(qMax(1, sz.width()));
        fatData[22] = 1.0f / float(qMax(1, sz.height()));
        const QVector4D localClip = localClipPlaneFor(item.meshIndex);
        fatData[24] = localClip.x();
        fatData[25] = localClip.y();
        fatData[26] = localClip.z();
        fatData[27] = localClip.w();
        QRhiResourceUpdateBatch *uFat = m_rhi->nextResourceUpdateBatch();
        uFat->updateDynamicBuffer(
            m_decoratorFatUbuf.get(), ubufOffset, kDecoratorFatUbufSize, fatData);
        cb->resourceUpdate(uFat);
        setShaderResourcesWithOffset(cb, m_decoratorFatSrb.get(), ubufOffset);
        return true;
    };

    cb->setViewport(plan.rhiViewport());
    for (const SceneDecoratorDrawItem &item : plan.decoratorItems) {
        if (!item.vertexBuffer || item.vertexCount <= 0)
            continue;

        switch (item.kind) {
        case SceneDecoratorDrawKind::Line:
            if (!m_decoratorPipeline)
                continue;
            cb->setGraphicsPipeline(m_decoratorPipeline.get());
            if (!uploadDecoratorColor(item))
                continue;
            break;
        case SceneDecoratorDrawKind::FatLine:
            if (!m_decoratorFatPipeline)
                continue;
            cb->setGraphicsPipeline(m_decoratorFatPipeline.get());
            if (!uploadDecoratorFat(item))
                continue;
            break;
        case SceneDecoratorDrawKind::Point:
            if (!m_decoratorPointPipeline)
                continue;
            cb->setGraphicsPipeline(m_decoratorPointPipeline.get());
            if (!uploadDecoratorColor(item))
                continue;
            break;
        }

        const QRhiCommandBuffer::VertexInput binding(item.vertexBuffer, 0);
        cb->setVertexInput(0, 1, &binding);
        cb->draw(item.vertexCount);
    }
}

void RenderWidget::renderSceneSelectionItems(
    QRhiCommandBuffer *cb,
    const RenderFramePlan &plan)
{
    if (plan.selectionItems.empty()
        || !m_selectionUbuf
        || !m_selectionSrb
        || (!m_selectionFacesPipeline && !m_selectionVerticesPipeline)) {
        return;
    }

    const QSize &sz = plan.pixelSize;
    cb->setViewport(plan.rhiViewport());
    const QMatrix4x4 frameVp = plan.proj * plan.view;

    for (const SceneSelectionDrawItem &item : plan.selectionItems) {
        if (item.meshIndex < 0 || item.meshIndex >= m_doc->meshCount())
            continue;
        const MeshGpuResourceCache::SelectionPassView &selectionView = item.selectionView;
        if (!selectionView.valid)
            continue;

        const quint32 ubufOffset = allocateDynamicUbufOffset(
            m_selectionUbufAllocator,
            "selection");
        float selectionData[kDecoratorUbufSize / sizeof(float)] = {};
        const QMatrix4x4 meshMvp = frameVp * m_doc->mesh(item.meshIndex).transform;
        memcpy(selectionData, meshMvp.constData(), 64);
        selectionData[16] = 1.0f;
        selectionData[17] = 0.0f;
        selectionData[18] = 0.0f;
        selectionData[19] = 0.5f;
        const QVector4D localClip = localClipPlaneFor(item.meshIndex);
        selectionData[20] = localClip.x();
        selectionData[21] = localClip.y();
        selectionData[22] = localClip.z();
        selectionData[23] = localClip.w();
        QRhiResourceUpdateBatch *uSel = m_rhi->nextResourceUpdateBatch();
        uSel->updateDynamicBuffer(
            m_selectionUbuf.get(), ubufOffset, kDecoratorUbufSize, selectionData);
        cb->resourceUpdate(uSel);

        if (item.drawFaces
            && m_selectionFacesPipeline
            && selectionView.selectedFacesBuffer
            && selectionView.selectedFacesVertexCount > 0) {
            cb->setGraphicsPipeline(m_selectionFacesPipeline.get());
            setShaderResourcesWithOffset(cb, m_selectionSrb.get(), ubufOffset);
            const QRhiCommandBuffer::VertexInput fv(
                selectionView.selectedFacesBuffer, 0);
            cb->setVertexInput(0, 1, &fv);
            cb->draw(selectionView.selectedFacesVertexCount);
        }

        if (item.drawVertices
            && m_selectionVerticesPipeline
            && selectionView.selectedVerticesBuffer
            && selectionView.selectedVerticesVertexCount > 0) {
            cb->setGraphicsPipeline(m_selectionVerticesPipeline.get());
            setShaderResourcesWithOffset(cb, m_selectionSrb.get(), ubufOffset);
            const QRhiCommandBuffer::VertexInput vv(
                selectionView.selectedVerticesBuffer, 0);
            cb->setVertexInput(0, 1, &vv);
            cb->draw(selectionView.selectedVerticesVertexCount);
        }

        if (item.drawEdges
            && m_selectionEdgesPipeline
            && selectionView.selectedEdgesBuffer
            && selectionView.selectedEdgesVertexCount > 0) {
            cb->setGraphicsPipeline(m_selectionEdgesPipeline.get());
            setShaderResourcesWithOffset(cb, m_selectionSrb.get(), ubufOffset);
            const QRhiCommandBuffer::VertexInput ev(
                selectionView.selectedEdgesBuffer, 0);
            cb->setVertexInput(0, 1, &ev);
            cb->draw(selectionView.selectedEdgesVertexCount);
        }
    }
}

void RenderWidget::renderSceneSolidCut(
    QRhiCommandBuffer *cb,
    const RenderFramePlan &plan)
{
    if (!m_renderSettings.clipPlaneSolidCut || m_frameClipPlane.isNull())
        return;
    if (!m_clipCountPipeline || !m_clipCapPipeline || !m_clipCapUbuf || !m_clipCapSrb || !m_srb)
        return;
    const SceneFillFramePlan &fillPlan = plan.sceneFill;
    if (fillPlan.fillItems.empty())
        return;

    QVector3D sceneMin;
    QVector3D sceneMax;
    if (!computeWorldSceneBBox(sceneMin, sceneMax))
        return;
    const std::vector<float> quad = ClipPlane::capQuad(m_frameClipPlane, sceneMin, sceneMax);
    if (quad.empty())
        return;

    cb->setViewport(plan.rhiViewport());

    // Pass 1: count. Only the layers whose fill is showing take part -- a wire-only or
    // point layer has no surface to be solid.
    for (const SceneFillDrawItem &item : fillPlan.fillItems) {
        const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
        uploadMainUbufForMesh(
            cb,
            item.meshIndex,
            fillPlan.proj,
            fillPlan.view,
            item.meshSettings,
            fillPlan.pixelSize,
            false,
            fillPlan.lightDir,
            MainUbufMaterialOverrides{},
            ubufOffset,
            0.0f);
        cb->setGraphicsPipeline(m_clipCountPipeline.get());
        setShaderResourcesWithOffset(cb, m_srb.get(), ubufOffset);
        for (int bi = 0; bi < item.fillView.batchCount; ++bi) {
            const auto &batch = item.fillView.batches[bi];
            if (!hasDrawableBatchGeometry(batch))
                continue;
            drawBatchGeometry(cb, batch);
        }
    }

    // Pass 2: the cap. Planar, lit by a directional light, so its diffuse term is one value
    // for the whole face -- computed here with the fill shaders' own model (ambient plus
    // Lambert, same ambient share) so the cut sits in the same light as the surface around
    // it. It faces the side that was cut away, which is where a camera looking at it is.
    const QVector3D planeNormal = m_frameClipPlane.toVector3D().normalized();
    const QVector3D capNormalView =
        (fillPlan.view * QVector4D(-planeNormal, 0.0f)).toVector3D().normalized();
    const float diffuse =
        std::max(0.0f, QVector3D::dotProduct(capNormalView, fillPlan.lightDir.normalized()));
    constexpr float kAmbient = 0.18f;   // the fill shaders' kAmbient
    const float shade = kAmbient + (1.0f - kAmbient) * diffuse;
    const QColor base = m_renderSettings.clipPlaneSolidCutColor;

    float ubuf[kRasterProjectedUbufSize / sizeof(float)] = {};
    const QMatrix4x4 mvp = fillPlan.proj * fillPlan.view;
    memcpy(ubuf, mvp.constData(), 64);
    ubuf[16] = float(base.redF()) * shade;
    ubuf[17] = float(base.greenF()) * shade;
    ubuf[18] = float(base.blueF()) * shade;
    ubuf[19] = 1.0f;

    const quint32 vbufSize = quint32(quad.size() * sizeof(float));
    if (!m_clipCapVbuf || m_clipCapVbuf->size() < vbufSize) {
        m_clipCapVbuf.reset(m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, vbufSize));
        if (!m_clipCapVbuf->create()) {
            m_clipCapVbuf.reset();
            return;
        }
    }
    QRhiResourceUpdateBatch *u = m_rhi->nextResourceUpdateBatch();
    u->updateDynamicBuffer(m_clipCapVbuf.get(), 0, vbufSize, quad.data());
    u->updateDynamicBuffer(m_clipCapUbuf.get(), 0, kRasterProjectedUbufSize, ubuf);
    cb->resourceUpdate(u);

    cb->setGraphicsPipeline(m_clipCapPipeline.get());
    cb->setStencilRef(kSolidCutStencilBias);
    cb->setShaderResources(m_clipCapSrb.get());
    const QRhiCommandBuffer::VertexInput binding(m_clipCapVbuf.get(), 0);
    cb->setVertexInput(0, 1, &binding);
    cb->draw(quint32(quad.size() / 3));
}
