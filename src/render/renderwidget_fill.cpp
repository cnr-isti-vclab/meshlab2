#include "renderwidget.h"
#include "document.h"
#include "renderwidget_internal.h"
#include <algorithm>
#include <vector>

using namespace RenderWidgetInternal;

class RenderWidget::FillRenderServices final
{
public:
    explicit FillRenderServices(RenderWidget &widget) : m_widget(widget) {}

    const Document *document() const { return m_widget.m_doc; }
    bool fillTextureNearestSampling() const { return m_widget.m_renderSettings.fillTextureNearestSampling; }

    void uploadMainUbufForMesh(
        QRhiCommandBuffer *cb,
        int meshIndex,
        const QMatrix4x4 &proj,
        const QMatrix4x4 &view,
        const PerMeshRenderSettings &meshSettings,
        const QSize &pixelSize,
        bool enableLighting,
        const QVector3D &lightDir,
        MainUbufMaterialOverrides materialOverrides,
        quint32 offset) const
    {
        m_widget.uploadMainUbufForMesh(
            cb,
            meshIndex,
            proj,
            view,
            meshSettings,
            pixelSize,
            enableLighting,
            lightDir,
            materialOverrides,
            offset);
    }

    QRhiTexture *resolveSelectedPbrTexture(
        int meshIndex,
        int textureIndex,
        const MeshGpuResourceCache::FillPassView &fillView) const
    {
        return m_widget.resolveSelectedPbrTexture(meshIndex, textureIndex, fillView);
    }

    QRhiShaderResourceBindings *shaderResourcesForFillTextures(
        QRhiTexture *baseColorTexture,
        QRhiTexture *normalTexture,
        QRhiTexture *occlusionTexture,
        QRhiTexture *roughnessTexture,
        bool nearest = false) const
    {
        return m_widget.shaderResourcesForFillTextures(
            baseColorTexture,
            normalTexture,
            occlusionTexture,
            roughnessTexture,
            nearest);
    }

    QRhiTexture *radianceScalingGradientTexture() const
    {
        return m_widget.m_rsGradTexture ? m_widget.m_rsGradTexture.get() : nullptr;
    }

    bool ensureRadianceScalingGradientResources(const QSize &pixelSize) const
    {
        m_widget.ensureRsGradResources(pixelSize);
        return m_widget.m_rsGradRt && m_widget.m_rsGradPipeline && m_widget.m_rsGradSrb;
    }

    QRhiTextureRenderTarget *radianceScalingGradientRenderTarget() const
    {
        return m_widget.m_rsGradRt.get();
    }

    QRhiGraphicsPipeline *radianceScalingGradientPipeline() const
    {
        return m_widget.m_rsGradPipeline.get();
    }

    QRhiShaderResourceBindings *radianceScalingGradientShaderResources() const
    {
        return m_widget.m_rsGradSrb.get();
    }

    quint32 allocateMainUbufOffset() const
    {
        return m_widget.allocateDynamicUbufOffset(m_widget.m_mainUbufAllocator, "main");
    }

    void setShaderResourcesWithOffset(
        QRhiCommandBuffer *cb,
        QRhiShaderResourceBindings *srb,
        quint32 offset) const
    {
        m_widget.setShaderResourcesWithOffset(cb, srb, offset);
    }

private:
    RenderWidget &m_widget;
};

class RenderWidget::FillMaterialRenderer
{
public:
    virtual ~FillMaterialRenderer() = default;

    // Takes every tile's plan, not one frame context: a prepass renders into a buffer
    // shared by the whole frame, and QRhi clears a render target whenever a pass opens on
    // it, so one pass per tile would wipe the tiles already drawn.
    virtual void renderPrepass(
        QRhiCommandBuffer *cb,
        const FillRenderServices &services,
        const std::vector<RenderFramePlan> &plans) const
    {
        Q_UNUSED(cb);
        Q_UNUSED(services);
        Q_UNUSED(plans);
    }

    virtual void drawBatch(
        const SceneFillDrawContext &ctx,
        const MeshGpuResourceCache::FillBatchView &batch) const = 0;
};

class RenderWidget::PlainFillRenderer final : public FillMaterialRenderer
{
public:
    void drawBatch(
        const SceneFillDrawContext &ctx,
        const MeshGpuResourceCache::FillBatchView &batch) const override
    {
        const SceneFillDrawItem &item = ctx.item;
        const SceneFillFrameContext &frame = ctx.frame;
        const quint32 ubufOffset = frame.services.allocateMainUbufOffset();

        MainUbufMaterialOverrides overrides {
            item.meshSettings.fillPbr.normalScale * batch.normalScale,
            1.0f,
            1.0f };
        // Per-mesh color override: when color source is PerMesh, use mesh.C()
        const Document *doc = frame.services.document();
        if (doc && item.meshIndex >= 0 && item.meshIndex < doc->meshCount()
            && item.meshSettings.fillPlain.colorSource == FillColorSource::PerMesh) {
            const vcg::Color4b &mc = doc->mesh(item.meshIndex).mesh.C();
            if (mc[0] != 0 || mc[1] != 0 || mc[2] != 0) {
                overrides.fillColorOverride = QColor(mc[0], mc[1], mc[2], mc[3]);
            }
        }
        frame.services.uploadMainUbufForMesh(
                frame.cb,
                item.meshIndex,
                frame.proj,
                frame.view,
                item.meshSettings,
                frame.pixelSize,
                true,
                frame.lightDir,
                overrides,
                ubufOffset);

        // When Texture source is selected, resolve the user-chosen texture by index;
        // fall back to the batch's baked base-colour texture if it cannot be found.
        QRhiTexture *albedo = batch.baseColorTexture;
        if (item.meshSettings.fillPlain.colorSource == FillColorSource::Texture) {
            if (QRhiTexture *t = frame.services.resolveSelectedPbrTexture(
                    item.meshIndex,
                    item.meshSettings.fillPlain.textureIndex,
                    item.fillView)) {
                albedo = t;
            }
        }
        frame.services.setShaderResourcesWithOffset(
            frame.cb,
            frame.services.shaderResourcesForFillTextures(
                albedo, nullptr, nullptr, nullptr,
                frame.services.fillTextureNearestSampling()),
            ubufOffset);
        drawBatchGeometry(frame.cb, batch);
    }
};

class RenderWidget::PbrFillRenderer final : public FillMaterialRenderer
{
public:
    void drawBatch(
        const SceneFillDrawContext &ctx,
        const MeshGpuResourceCache::FillBatchView &batch) const override
    {
        const SceneFillDrawItem &item = ctx.item;
        const SceneFillFrameContext &frame = ctx.frame;
        const auto &pbr = item.meshSettings.fillPbr;
        QRhiTexture *albedo =
            (pbr.albedoSource == FillPbrTextureSource::Texture)
            ? frame.services.resolveSelectedPbrTexture(item.meshIndex, pbr.albedoIndex, item.fillView) : nullptr;
        QRhiTexture *normal =
            (pbr.normalSource == FillPbrTextureSource::Texture)
            ? frame.services.resolveSelectedPbrTexture(item.meshIndex, pbr.normalIndex, item.fillView) : nullptr;
        QRhiTexture *occlusion =
            (pbr.occlusionSource == FillPbrTextureSource::Texture)
            ? frame.services.resolveSelectedPbrTexture(item.meshIndex, pbr.occlusionIndex, item.fillView) : nullptr;
        QRhiTexture *roughness =
            (pbr.roughnessSource == FillPbrTextureSource::Texture)
            ? frame.services.resolveSelectedPbrTexture(item.meshIndex, pbr.roughnessIndex, item.fillView) : nullptr;
        QRhiTexture *resolvedNormal = normal ? normal : batch.normalTexture;
        PerMeshRenderSettings pbrSettings = item.meshSettings;
        if (pbrSettings.fillPbr.normalSource == FillPbrTextureSource::Texture && !resolvedNormal)
            pbrSettings.fillPbr.normalSource = FillPbrTextureSource::None;

        const quint32 ubufOffset = frame.services.allocateMainUbufOffset();
        frame.services.uploadMainUbufForMesh(
            frame.cb,
            item.meshIndex,
            frame.proj,
            frame.view,
            pbrSettings,
            frame.pixelSize,
            true,
            frame.lightDir,
            MainUbufMaterialOverrides {
                pbr.normalScale * batch.normalScale,
                pbr.occlusionStrength * batch.occlusionStrength,
                pbr.roughnessFactor * batch.roughnessFactor },
            ubufOffset);
        frame.services.setShaderResourcesWithOffset(
            frame.cb,
            frame.services.shaderResourcesForFillTextures(
                albedo    ? albedo    : batch.baseColorTexture,
                resolvedNormal,
                occlusion ? occlusion : batch.occlusionTexture,
                roughness ? roughness : batch.roughnessTexture,
                frame.services.fillTextureNearestSampling()),
            ubufOffset);
        drawBatchGeometry(frame.cb, batch);
    }
};

class RenderWidget::RadianceScalingFillRenderer final : public FillMaterialRenderer
{
public:
    void renderPrepass(
        QRhiCommandBuffer *cb,
        const FillRenderServices &services,
        const std::vector<RenderFramePlan> &plans) const override
    {
        const auto itemUsesThisRenderer = [this](const SceneFillDrawItem &item) {
            return item.materialRenderer == this;
        };
        const auto planHasItems = [&](const RenderFramePlan &plan) {
            const auto &items = plan.sceneFill.fillItems;
            return std::any_of(items.begin(), items.end(), itemUsesThisRenderer);
        };

        QSize targetPixelSize;
        for (const RenderFramePlan &plan : plans) {
            if (planHasItems(plan))
                targetPixelSize = plan.targetPixelSize;
        }
        if (targetPixelSize.isEmpty())
            return;

        // The gradient buffer spans the whole render target rather than one tile: the fill
        // shader reads it back by absolute framebuffer position, so each tile writes into
        // its own corner of one shared buffer.
        if (!services.ensureRadianceScalingGradientResources(targetPixelSize))
            return;

        cb->beginPass(
            services.radianceScalingGradientRenderTarget(),
            QColor(0, 0, 0, 0),
            { 1.0f, 0 },
            nullptr);
        cb->setGraphicsPipeline(services.radianceScalingGradientPipeline());
        for (const RenderFramePlan &plan : plans) {
            if (!planHasItems(plan))
                continue;
            cb->setViewport(plan.rhiViewport());
            for (const SceneFillDrawItem &item : plan.sceneFill.fillItems) {
                if (!itemUsesThisRenderer(item))
                    continue;
                for (int bi = 0; bi < item.fillView.batchCount; ++bi) {
                    const auto &batch = item.fillView.batches[bi];
                    if (!hasDrawableBatchGeometry(batch))
                        continue;
                    const quint32 ubufOffset = services.allocateMainUbufOffset();
                    services.uploadMainUbufForMesh(
                        cb,
                        item.meshIndex,
                        plan.sceneFill.proj,
                        plan.sceneFill.view,
                        item.meshSettings,
                        plan.sceneFill.pixelSize,
                        true,
                        plan.sceneFill.lightDir,
                        MainUbufMaterialOverrides {
                            item.meshSettings.fillRs.enhancement,
                            1.0f,
                            1.0f },
                        ubufOffset);
                    services.setShaderResourcesWithOffset(
                        cb,
                        services.radianceScalingGradientShaderResources(),
                        ubufOffset);
                    drawBatchGeometry(cb, batch);
                }
            }
        }
        cb->endPass();
    }

    void drawBatch(
        const SceneFillDrawContext &ctx,
        const MeshGpuResourceCache::FillBatchView &batch) const override
    {
        const SceneFillDrawItem &item = ctx.item;
        const SceneFillFrameContext &frame = ctx.frame;
        const quint32 ubufOffset = frame.services.allocateMainUbufOffset();
        frame.services.uploadMainUbufForMesh(
            frame.cb,
            item.meshIndex,
            frame.proj,
            frame.view,
            item.meshSettings,
            frame.pixelSize,
            true,
            frame.lightDir,
            MainUbufMaterialOverrides {
                item.meshSettings.fillRs.enhancement,
                1.0f,
                1.0f },
            ubufOffset);
        QRhiTexture *gradTex = frame.services.radianceScalingGradientTexture();
        frame.services.setShaderResourcesWithOffset(
            frame.cb,
            frame.services.shaderResourcesForFillTextures(
                batch.baseColorTexture, gradTex, nullptr, nullptr,
                frame.services.fillTextureNearestSampling()),
            ubufOffset);
        drawBatchGeometry(frame.cb, batch);
    }
};

RenderWidget::SceneFillFramePlan RenderWidget::buildSceneFillFramePlan(
    const RenderWidget::RenderFrameRequest &request)
{
    static const PlainFillRenderer plainFillRenderer;
    static const PbrFillRenderer pbrFillRenderer;
    static const RadianceScalingFillRenderer radianceScalingFillRenderer;
    auto rendererForMaterial = [&](FillMaterial material) -> const FillMaterialRenderer * {
        switch (material) {
        case FillMaterial::Plain:
            return &plainFillRenderer;
        case FillMaterial::Pbr:
            return &pbrFillRenderer;
        case FillMaterial::RadianceScaling:
            return &radianceScalingFillRenderer;
        }
        return nullptr;
    };

    SceneFillFramePlan plan;
    plan.pixelSize = request.pixelSize;
    plan.proj = request.proj;
    plan.view = request.view;
    plan.lightDir = request.lightDir;
    plan.fillItems.reserve(request.passes.meshes.size());
    for (const RenderMeshPassRequests &meshRequest : request.passes.meshes) {
        if (!meshRequest.fill)
            continue;
        const int mi = meshRequest.meshIndex;
        const PerMeshRenderSettings &meshSettings = meshRequest.meshSettings;
        QRhiGraphicsPipeline *fillPipeline = fillPipelineForSettings(meshSettings);
        if (!fillPipeline)
            continue;
        const auto fillVariant = static_cast<Document::FillGpuVariant>(
            fillGpuVariantIndexForSettings(meshSettings));
        const Document::FillPassGpuView fillView =
            m_doc->fillPassGpuView(m_rhi, mi, fillVariant);
        if (!fillView.valid)
            continue;

        const FillMaterialRenderer *materialRenderer =
            rendererForMaterial(meshSettings.fillMaterial);
        if (!materialRenderer)
            continue;

        plan.fillItems.push_back(SceneFillDrawItem {
            mi,
            fillPipeline,
            materialRenderer,
            meshSettings,
            fillView
        });
    }
    return plan;
}

void RenderWidget::renderSceneFillPrepasses(
    QRhiCommandBuffer *cb,
    const std::vector<RenderFramePlan> &plans)
{
    const FillRenderServices services(*this);

    std::vector<const FillMaterialRenderer *> prepassRenderers;
    for (const RenderFramePlan &plan : plans) {
        for (const SceneFillDrawItem &item : plan.sceneFill.fillItems) {
            if (!item.materialRenderer)
                continue;
            if (std::find(prepassRenderers.begin(), prepassRenderers.end(), item.materialRenderer)
                != prepassRenderers.end()) {
                continue;
            }
            prepassRenderers.push_back(item.materialRenderer);
            item.materialRenderer->renderPrepass(cb, services, plans);
        }
    }
}

void RenderWidget::renderSceneFillPass(
    QRhiCommandBuffer *cb,
    const RenderFramePlan &plan)
{
    const SceneFillFramePlan &fillPlan = plan.sceneFill;
    const FillRenderServices services(*this);
    const SceneFillFrameContext frameCtx {
        services,
        cb,
        fillPlan.pixelSize,
        fillPlan.proj,
        fillPlan.view,
        fillPlan.lightDir
    };
    if (fillPlan.fillItems.empty())
        return;

    cb->setViewport(plan.rhiViewport());
    for (const SceneFillDrawItem &item : fillPlan.fillItems) {
        cb->setGraphicsPipeline(item.pipeline);
        const SceneFillDrawContext fillCtx {
            frameCtx,
            item
        };
        for (int bi = 0; bi < item.fillView.batchCount; ++bi) {
            const auto &batch = item.fillView.batches[bi];
            if (!hasDrawableBatchGeometry(batch))
                continue;
            item.materialRenderer->drawBatch(fillCtx, batch);
        }
    }
}
