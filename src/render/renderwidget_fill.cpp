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

        // With radiance scaling on the shader reads the gradient buffer (bound where the normal
        // map goes, which plain does not use), and the gain travels in the slot of the normal
        // scale. If the buffer could not be made the option is simply off for this draw.
        QRhiTexture *gradientTexture = usesRadianceScaling(item.meshSettings)
            ? frame.services.radianceScalingGradientTexture() : nullptr;
        PerMeshRenderSettings settings = item.meshSettings;
        if (!gradientTexture)
            settings.fillPlain.radianceScaling = false;
        MainUbufMaterialOverrides overrides {
            gradientTexture ? radianceScalingGain(settings)
                            : settings.fillPbr.normalScale * batch.normalScale,
            1.0f,
            1.0f };
        // Per-mesh color override: when color source is PerMesh, use mesh.C()
        const Document *doc = frame.services.document();
        if (doc && item.meshIndex >= 0 && item.meshIndex < doc->meshCount()
            && settings.fillPlain.colorSource == FillColorSource::PerMesh) {
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
                settings,
                frame.pixelSize,
                true,
                frame.lightDir,
                overrides,
                ubufOffset);

        // When Texture source is selected, resolve the user-chosen texture by index;
        // fall back to the batch's baked base-colour texture if it cannot be found.
        QRhiTexture *albedo = batch.baseColorTexture;
        if (settings.fillPlain.colorSource == FillColorSource::Texture) {
            if (QRhiTexture *t = frame.services.resolveSelectedPbrTexture(
                    item.meshIndex,
                    settings.fillPlain.textureIndex,
                    item.fillView)) {
                albedo = t;
            }
        }
        frame.services.setShaderResourcesWithOffset(
            frame.cb,
            frame.services.shaderResourcesForFillTextures(
                albedo, gradientTexture, nullptr, nullptr,
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

RenderWidget::SceneFillFramePlan RenderWidget::buildSceneFillFramePlan(
    const RenderWidget::RenderFrameRequest &request)
{
    static const PlainFillRenderer plainFillRenderer;
    static const PbrFillRenderer pbrFillRenderer;
    auto rendererForMaterial = [&](FillMaterial material) -> const FillMaterialRenderer * {
        switch (material) {
        case FillMaterial::Plain:
            return &plainFillRenderer;
        case FillMaterial::Pbr:
            return &pbrFillRenderer;
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

// Radiance scaling is an option of the plain material, for a layer's surface and for its points,
// and its curvature comes from a buffer of the view-space normals (and log depth) of what is on
// screen: this pass writes it. The normals it stores are the ones each layer is shaded with, flat
// or smooth, and the points' own. It draws every layer that fills or shows points, not only those
// with the option on, because the others occlude them and a gradient buffer that did not know that
// would read a depth step where a layer in front ends as no step at all. Run once a frame, and only
// when some layer asks.
void RenderWidget::renderRadianceScalingPrepass(
    QRhiCommandBuffer *cb,
    const std::vector<RenderFramePlan> &plans)
{
    QSize targetPixelSize;
    for (const RenderFramePlan &plan : plans) {
        const auto asks = [](const auto &item) { return usesRadianceScaling(item.meshSettings); };
        if (std::any_of(plan.sceneFill.fillItems.begin(), plan.sceneFill.fillItems.end(), asks)
            || std::any_of(plan.pointItems.begin(), plan.pointItems.end(), asks)) {
            targetPixelSize = plan.targetPixelSize;
        }
    }
    if (targetPixelSize.isEmpty())
        return;

    // The gradient buffer spans the whole render target rather than one tile: the shaders read it
    // back by absolute framebuffer position, so each tile writes into its own corner of one
    // shared buffer.
    ensureRsGradResources(targetPixelSize);
    if (!m_rsGradRt || !m_rsGradPipeline || !m_rsGradPointsPipeline || !m_rsGradSrb)
        return;

    cb->beginPass(m_rsGradRt.get(), QColor(0, 0, 0, 0), { 1.0f, 0 }, nullptr);
    for (const RenderFramePlan &plan : plans) {
        cb->setViewport(plan.rhiViewport());
        cb->setGraphicsPipeline(m_rsGradPipeline.get());
        for (const SceneFillDrawItem &item : plan.sceneFill.fillItems) {
            for (int bi = 0; bi < item.fillView.batchCount; ++bi) {
                const auto &batch = item.fillView.batches[bi];
                if (!hasDrawableBatchGeometry(batch))
                    continue;
                const quint32 ubufOffset = allocateDynamicUbufOffset(m_mainUbufAllocator, "main");
                // Whether the layer is shaded flat is told to the pass through the slot of the
                // occlusion strength, which this pass has no other use for.
                const FillShading shading = item.meshSettings.fillMaterial == FillMaterial::Pbr
                    ? item.meshSettings.fillPbr.shading
                    : item.meshSettings.fillPlain.shading;
                uploadMainUbufForMesh(
                    cb,
                    item.meshIndex,
                    plan.sceneFill.proj,
                    plan.sceneFill.view,
                    item.meshSettings,
                    plan.sceneFill.pixelSize,
                    true,
                    plan.sceneFill.lightDir,
                    MainUbufMaterialOverrides {
                        1.0f, shading == FillShading::Flat ? 1.0f : 0.0f, 1.0f },
                    ubufOffset);
                setShaderResourcesWithOffset(cb, m_rsGradSrb.get(), ubufOffset);
                drawBatchGeometry(cb, batch);
            }
        }
        for (const SceneBufferDrawItem &item : plan.pointItems) {
            drawSceneBufferItem(
                cb, plan, item, m_rsGradPointsPipeline.get(), item.meshSettings, m_rsGradSrb.get());
        }
    }
    cb->endPass();
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
