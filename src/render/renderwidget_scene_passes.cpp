#include "renderwidget.h"
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
    }
}
