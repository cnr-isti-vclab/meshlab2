#include "renderwidget.h"
#include "document.h"
#include "meshgpuresourcecache.h"
#include "linerenderer.h"
#include "renderwidget_internal.h"
#include <QElapsedTimer>
#include <QImage>
#include <QVector4D>
#include <algorithm>
#include <cmath>

using namespace RenderWidgetInternal;

namespace {
constexpr int kSceneBackgroundUbufSize = 32;
}

bool RenderWidget::computeWorldSceneBBox(QVector3D &minCorner, QVector3D &maxCorner) const
{
    bool hasVisibleMesh = false;
    for (int i = 0; i < m_doc->meshCount(); ++i) {
        if (!meshVisible(i))
            continue;
        const auto &entry = m_doc->mesh(i);
        if (entry.mesh.bbox.IsNull())
            continue;

        const vcg::Box3f &box = entry.mesh.bbox;
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
            const QVector4D transformed = entry.transform * QVector4D(corner, 1.0f);
            const QVector3D worldCorner = (std::abs(transformed.w()) > 1e-8f)
                ? transformed.toVector3DAffine()
                : transformed.toVector3D();
            if (!hasVisibleMesh) {
                minCorner = worldCorner;
                maxCorner = worldCorner;
                hasVisibleMesh = true;
                continue;
            }
            minCorner.setX(std::min(minCorner.x(), worldCorner.x()));
            minCorner.setY(std::min(minCorner.y(), worldCorner.y()));
            minCorner.setZ(std::min(minCorner.z(), worldCorner.z()));
            maxCorner.setX(std::max(maxCorner.x(), worldCorner.x()));
            maxCorner.setY(std::max(maxCorner.y(), worldCorner.y()));
            maxCorner.setZ(std::max(maxCorner.z(), worldCorner.z()));
        }
    }
    return hasVisibleMesh;
}

bool RenderWidget::meshFitsInCurrentFrame(int index) const
{
    if (index < 0 || index >= m_doc->meshCount())
        return true;
    const auto &entry = m_doc->mesh(index);
    if (entry.mesh.bbox.IsNull())
        return true; // nothing to see, so nothing to reframe for

    // The trackball frames a sphere of `radius` about `center`; treat the new mesh as
    // visible when its whole bounding box sits inside that sphere. Conservative on
    // purpose -- a mesh hanging half out of view is the case this is meant to catch --
    // and it costs eight dot products rather than a frustum build.
    const ViewTrackball::State state = m_trackball.state();
    if (!(state.radius > 0.0f))
        return false;

    const vcg::Box3f &box = entry.mesh.bbox;
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
        const QVector4D transformed = entry.transform * QVector4D(corner, 1.0f);
        const QVector3D world = (std::abs(transformed.w()) > 1e-8f)
            ? transformed.toVector3DAffine()
            : transformed.toVector3D();
        if ((world - state.center).length() > state.radius)
            return false;
    }
    return true;
}

void RenderWidget::updateCameraFrameIfNeeded()
{
    if (!m_reframeCameraRequested)
        return;
    if (m_doc->meshCount() == 0)
        return;

    QVector3D sceneMin;
    QVector3D sceneMax;
    if (!computeWorldSceneBBox(sceneMin, sceneMax))
        return;

    const QVector3D center = 0.5f * (sceneMin + sceneMax);
    const float radius = qMax(1e-4f, 0.5f * (sceneMax - sceneMin).length());
    if (m_resetTrackballRequested) {
        m_trackball.resetToFrame(center, radius, radius * 3.0f);
        m_lightRotation = QQuaternion(); // reset headlight to default (0,0,1)
    } else
        m_trackball.setFrame(center, radius, radius * 3.0f);
    m_reframeCameraRequested = false;
    m_resetTrackballRequested = false;
    m_centerAnimActive = false;
}

void RenderWidget::ensureCurrentMeshMaskResources(const QSize &pixelSize)
{
    if (!m_rhi || pixelSize.isEmpty())
        return;

    if (m_currentMaskRt && m_currentMaskSize == pixelSize)
        return;

    m_currentMaskFillPipeline.reset();
    m_currentMaskFillDepthOnlyPipeline.reset();
    m_currentMaskEdgesPipeline.reset();
    m_currentMaskEdgesDepthPipeline.reset();
    m_currentMaskEdgesDepthOnlyPipeline.reset();
    m_currentMaskFatEdgesDepthOnlyPipeline.reset();
    m_currentMaskPointsPipeline.reset();
    m_currentMaskPointsDepthOnlyPipeline.reset();
    m_maskMorphMaskToBaseSrb.reset();
    m_maskMorphMaskToWorkSrb.reset();
    m_maskMorphWorkToMaskSrb.reset();
    m_maskMorphToBasePipeline.reset();
    m_maskMorphToWorkPipeline.reset();
    m_maskMorphWorkToMaskPipeline.reset();
    m_outlineExtractUbuf.reset();
    m_outlineExtractSrb.reset();
    m_outlineExtractPipeline.reset();
    m_maskDebugBaseSrb.reset();
    m_maskDebugWorkSrb.reset();
    m_maskDebugMaskSrb.reset();
    m_maskDebugPipeline.reset();
    m_outlineSrb.reset();
    m_outlinePipeline.reset();
    m_currentMaskBaseRt.reset();
    m_currentMaskBaseRp.reset();
    m_currentMaskBaseTexture.reset();
    m_currentMaskRt.reset();
    m_currentMaskRp.reset();
    m_currentMaskDepth.reset();
    m_currentMaskTexture.reset();
    m_currentMaskWorkRt.reset();
    m_currentMaskWorkRp.reset();
    m_currentMaskWorkTexture.reset();

    m_currentMaskTexture.reset(
        m_rhi->newTexture(
            QRhiTexture::RGBA8,
            pixelSize,
            1,
            QRhiTexture::RenderTarget));
    if (!m_currentMaskTexture || !m_currentMaskTexture->create()) {
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    m_currentMaskDepth.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!m_currentMaskDepth || !m_currentMaskDepth->create()) {
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(m_currentMaskTexture.get()));
    rtDesc.setDepthStencilBuffer(m_currentMaskDepth.get());
    m_currentMaskRt.reset(m_rhi->newTextureRenderTarget(rtDesc));
    if (!m_currentMaskRt) {
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    m_currentMaskRp.reset(m_currentMaskRt->newCompatibleRenderPassDescriptor());
    m_currentMaskRt->setRenderPassDescriptor(m_currentMaskRp.get());
    if (!m_currentMaskRt->create()) {
        m_currentMaskRp.reset();
        m_currentMaskRt.reset();
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    m_currentMaskBaseTexture.reset(
        m_rhi->newTexture(
            QRhiTexture::RGBA8,
            pixelSize,
            1,
            QRhiTexture::RenderTarget));
    if (!m_currentMaskBaseTexture || !m_currentMaskBaseTexture->create()) {
        m_currentMaskBaseTexture.reset();
        m_currentMaskRp.reset();
        m_currentMaskRt.reset();
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    QRhiTextureRenderTargetDescription baseRtDesc(QRhiColorAttachment(m_currentMaskBaseTexture.get()));
    baseRtDesc.setDepthStencilBuffer(m_currentMaskDepth.get());
    m_currentMaskBaseRt.reset(m_rhi->newTextureRenderTarget(baseRtDesc));
    if (!m_currentMaskBaseRt) {
        m_currentMaskBaseTexture.reset();
        m_currentMaskRp.reset();
        m_currentMaskRt.reset();
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    m_currentMaskBaseRp.reset(m_currentMaskBaseRt->newCompatibleRenderPassDescriptor());
    m_currentMaskBaseRt->setRenderPassDescriptor(m_currentMaskBaseRp.get());
    if (!m_currentMaskBaseRt->create()) {
        m_currentMaskBaseRp.reset();
        m_currentMaskBaseRt.reset();
        m_currentMaskBaseTexture.reset();
        m_currentMaskRp.reset();
        m_currentMaskRt.reset();
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    m_currentMaskWorkTexture.reset(
        m_rhi->newTexture(
            QRhiTexture::RGBA8,
            pixelSize,
            1,
            QRhiTexture::RenderTarget));
    if (!m_currentMaskWorkTexture || !m_currentMaskWorkTexture->create()) {
        m_currentMaskWorkTexture.reset();
        m_currentMaskBaseRp.reset();
        m_currentMaskBaseRt.reset();
        m_currentMaskBaseTexture.reset();
        m_currentMaskRp.reset();
        m_currentMaskRt.reset();
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    QRhiTextureRenderTargetDescription workRtDesc(QRhiColorAttachment(m_currentMaskWorkTexture.get()));
    m_currentMaskWorkRt.reset(m_rhi->newTextureRenderTarget(workRtDesc));
    if (!m_currentMaskWorkRt) {
        m_currentMaskWorkTexture.reset();
        m_currentMaskBaseRp.reset();
        m_currentMaskBaseRt.reset();
        m_currentMaskBaseTexture.reset();
        m_currentMaskRp.reset();
        m_currentMaskRt.reset();
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    m_currentMaskWorkRp.reset(m_currentMaskWorkRt->newCompatibleRenderPassDescriptor());
    m_currentMaskWorkRt->setRenderPassDescriptor(m_currentMaskWorkRp.get());
    if (!m_currentMaskWorkRt->create()) {
        m_currentMaskWorkRp.reset();
        m_currentMaskWorkRt.reset();
        m_currentMaskWorkTexture.reset();
        m_currentMaskBaseRp.reset();
        m_currentMaskBaseRt.reset();
        m_currentMaskBaseTexture.reset();
        m_currentMaskRp.reset();
        m_currentMaskRt.reset();
        m_currentMaskDepth.reset();
        m_currentMaskTexture.reset();
        m_currentMaskSize = QSize();
        return;
    }

    m_currentMaskSize = pixelSize;
}

void RenderWidget::ensureDepthPickResources(const QSize &pixelSize)
{
    if (!m_rhi || pixelSize.isEmpty())
        return;

    if (m_depthPickRt && m_depthPickSize == pixelSize)
        return;

    m_depthPickFillPipeline.reset();
    m_depthPickPointsPipeline.reset();
    m_depthPickSrb.reset();
    m_depthPickRp.reset();
    m_depthPickRt.reset();
    m_depthPickDepth.reset();
    m_depthPickTexture.reset();

    m_depthPickTexture.reset(
        m_rhi->newTexture(
            QRhiTexture::RGBA8,
            pixelSize,
            1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!m_depthPickTexture || !m_depthPickTexture->create()) {
        m_depthPickTexture.reset();
        m_depthPickSize = QSize();
        return;
    }

    m_depthPickDepth.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!m_depthPickDepth || !m_depthPickDepth->create()) {
        m_depthPickDepth.reset();
        m_depthPickTexture.reset();
        m_depthPickSize = QSize();
        return;
    }

    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(m_depthPickTexture.get()));
    rtDesc.setDepthStencilBuffer(m_depthPickDepth.get());
    m_depthPickRt.reset(m_rhi->newTextureRenderTarget(rtDesc));
    if (!m_depthPickRt) {
        m_depthPickDepth.reset();
        m_depthPickTexture.reset();
        m_depthPickSize = QSize();
        return;
    }

    m_depthPickRp.reset(m_depthPickRt->newCompatibleRenderPassDescriptor());
    m_depthPickRt->setRenderPassDescriptor(m_depthPickRp.get());
    if (!m_depthPickRt->create()) {
        m_depthPickRp.reset();
        m_depthPickRt.reset();
        m_depthPickDepth.reset();
        m_depthPickTexture.reset();
        m_depthPickSize = QSize();
        return;
    }

    if (!m_depthPickSrb && m_ubuf) {
        m_depthPickSrb.reset(m_rhi->newShaderResourceBindings());
        m_depthPickSrb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage,
                m_ubuf.get(),
                kUbufSize)
        });
        if (!m_depthPickSrb->create())
            m_depthPickSrb.reset();
    }

    if (m_depthPickSrb && !m_depthPickFillPipeline) {
        m_depthPickFillPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/depth_pick.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/depth_pick.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load depth-pick fill shaders");
            m_depthPickFillPipeline.reset();
        } else {
            m_depthPickFillPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_depthPickFillPipeline->setDepthTest(true);
            m_depthPickFillPipeline->setDepthWrite(true);
            m_depthPickFillPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kFillVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_depthPickFillPipeline->setVertexInputLayout(layout);
            m_depthPickFillPipeline->setShaderResourceBindings(m_depthPickSrb.get());
            m_depthPickFillPipeline->setRenderPassDescriptor(m_depthPickRp.get());
            if (!m_depthPickFillPipeline->create()) {
                qWarning("Failed to create depth-pick fill pipeline");
                m_depthPickFillPipeline.reset();
            }
        }
    }

    if (m_depthPickSrb && !m_depthPickPointsPipeline) {
        m_depthPickPointsPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/depth_pick.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/depth_pick.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load depth-pick points shaders");
            m_depthPickPointsPipeline.reset();
        } else {
            m_depthPickPointsPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_depthPickPointsPipeline->setTopology(QRhiGraphicsPipeline::Points);
            m_depthPickPointsPipeline->setDepthTest(true);
            m_depthPickPointsPipeline->setDepthWrite(true);
            m_depthPickPointsPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kPointsVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_depthPickPointsPipeline->setVertexInputLayout(layout);
            m_depthPickPointsPipeline->setShaderResourceBindings(m_depthPickSrb.get());
            m_depthPickPointsPipeline->setRenderPassDescriptor(m_depthPickRp.get());
            if (!m_depthPickPointsPipeline->create()) {
                qWarning("Failed to create depth-pick points pipeline");
                m_depthPickPointsPipeline.reset();
            }
        }
    }

    m_depthPickSize = pixelSize;
}

void RenderWidget::ensureRsGradResources(const QSize &pixelSize)
{
    if (!m_rhi || !m_ubuf || pixelSize.isEmpty())
        return;

    // Already up to date?
    if (m_rsGradRt && m_rsGradSize == pixelSize)
        return;

    // Tear down everything that depends on the render target size.
    m_rsGradPipeline.reset();
    m_rsGradSrb.reset();
    m_rsGradRp.reset();
    m_rsGradRt.reset();
    m_rsGradDepth.reset();
    m_rsGradTexture.reset();
    m_rsGradSize = QSize();

    // RGBA32F gradient texture: stores (gx, gy, logZ, 1.0) per fragment.
    m_rsGradTexture.reset(
        m_rhi->newTexture(
            QRhiTexture::RGBA32F,
            pixelSize,
            1,
            QRhiTexture::RenderTarget));
    if (!m_rsGradTexture || !m_rsGradTexture->create()) {
        m_rsGradTexture.reset();
        return;
    }

    m_rsGradDepth.reset(m_rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, pixelSize, 1));
    if (!m_rsGradDepth || !m_rsGradDepth->create()) {
        m_rsGradDepth.reset();
        m_rsGradTexture.reset();
        return;
    }

    QRhiTextureRenderTargetDescription rtDesc(QRhiColorAttachment(m_rsGradTexture.get()));
    rtDesc.setDepthStencilBuffer(m_rsGradDepth.get());
    m_rsGradRt.reset(m_rhi->newTextureRenderTarget(rtDesc));
    if (!m_rsGradRt) {
        m_rsGradDepth.reset();
        m_rsGradTexture.reset();
        return;
    }

    m_rsGradRp.reset(m_rsGradRt->newCompatibleRenderPassDescriptor());
    m_rsGradRt->setRenderPassDescriptor(m_rsGradRp.get());
    if (!m_rsGradRt->create()) {
        m_rsGradRp.reset();
        m_rsGradRt.reset();
        m_rsGradDepth.reset();
        m_rsGradTexture.reset();
        return;
    }

    // Minimal SRB: only the shared UBO at binding 0.
    m_rsGradSrb.reset(m_rhi->newShaderResourceBindings());
    m_rsGradSrb->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
            m_ubuf.get())
    });
    if (!m_rsGradSrb->create()) {
        m_rsGradSrb.reset();
        m_rsGradRp.reset();
        m_rsGradRt.reset();
        m_rsGradDepth.reset();
        m_rsGradTexture.reset();
        return;
    }

    // Gradient buffer pipeline: fill_smooth.vert + fill_radscale_buf.frag.
    // Uses smooth vertex layout (position + normal), no backface culling
    // (depth test handles front-surface selection), depth test + write enabled.
    m_rsGradPipeline.reset(m_rhi->newGraphicsPipeline());
    QShader vs = loadShader(QStringLiteral(":/shaders/fill_smooth.vert.qsb"));
    QShader fs = loadShader(QStringLiteral(":/shaders/fill_radscale_buf.frag.qsb"));
    if (!vs.isValid() || !fs.isValid()) {
        qWarning("Failed to load RS gradient buffer shaders");
        m_rsGradPipeline.reset();
        m_rsGradSrb.reset();
        m_rsGradRp.reset();
        m_rsGradRt.reset();
        m_rsGradDepth.reset();
        m_rsGradTexture.reset();
        return;
    }
    m_rsGradPipeline->setShaderStages({
        { QRhiShaderStage::Vertex, vs },
        { QRhiShaderStage::Fragment, fs }
    });
    m_rsGradPipeline->setDepthTest(true);
    m_rsGradPipeline->setDepthWrite(true);
    m_rsGradPipeline->setCullMode(QRhiGraphicsPipeline::None);

    QRhiVertexInputLayout inputLayout;
    inputLayout.setBindings({ { kFillVertexStrideFloats * sizeof(float) } });
    inputLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float3, 0 },                   // position
        { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },   // normal
        { 0, 2, QRhiVertexInputAttribute::Float4, 6 * sizeof(float) },   // meshColor
        { 0, 3, QRhiVertexInputAttribute::Float3, 10 * sizeof(float) }   // texInfo
    });
    m_rsGradPipeline->setVertexInputLayout(inputLayout);
    m_rsGradPipeline->setShaderResourceBindings(m_rsGradSrb.get());
    m_rsGradPipeline->setRenderPassDescriptor(m_rsGradRp.get());
    if (!m_rsGradPipeline->create()) {
        qWarning("Failed to create RS gradient pipeline");
        m_rsGradPipeline.reset();
        m_rsGradSrb.reset();
        m_rsGradRp.reset();
        m_rsGradRt.reset();
        m_rsGradDepth.reset();
        m_rsGradTexture.reset();
        return;
    }

    m_rsGradSize = pixelSize;
}

void RenderWidget::ensureRenderResources()
{
    if (m_rhi != rhi()) {
        if (m_rhi)
            m_doc->releaseRhiGpuResources(m_rhi);
        m_rhi = rhi();
        m_fillPipelinesByKey.clear();
        m_wirePipelinesByKey.clear();
        m_edgesPipelinesByKey.clear();
        m_fatEdgesPipelinesByKey.clear();
        m_bboxPipeline.reset();
        m_pointsPipeline.reset();
        m_decoratorPipeline.reset();
        m_selectionUbuf.reset();
        m_selectionSrb.reset();
        m_selectionFacesPipeline.reset();
        m_selectionVerticesPipeline.reset();
        m_decoratorFatUbuf.reset();
        m_decoratorFatSrb.reset();
        m_decoratorFatPipeline.reset();
        m_decoratorPointPipeline.reset();
        for (auto &ubuf : m_toolLineUbufs)
            ubuf.reset();
        m_toolLineVbuf.reset();
        for (auto &srb : m_toolLineSrbs)
            srb.reset();
        m_toolLineHiddenPipeline.reset();
        m_toolLineVisiblePipeline.reset();
        m_toolLineVertexCount = 0;
        for (auto &srb : m_decoratorSrbs)
            srb.reset();
        for (auto &ubuf : m_decoratorUbufs)
            ubuf.reset();
        m_depthPickTexture.reset();
        m_depthPickDepth.reset();
        m_depthPickRt.reset();
        m_depthPickRp.reset();
        m_depthPickSize = QSize();
        m_depthPickSrb.reset();
        m_depthPickFillPipeline.reset();
        m_depthPickPointsPipeline.reset();
        m_depthPickPending = false;
        m_depthPickInFlight = false;
        m_depthPickReadbackResult.reset();
        m_currentMaskBaseTexture.reset();
        m_currentMaskBaseRt.reset();
        m_currentMaskBaseRp.reset();
        m_currentMaskTexture.reset();
        m_currentMaskDepth.reset();
        m_currentMaskRt.reset();
        m_currentMaskRp.reset();
        m_currentMaskWorkTexture.reset();
        m_currentMaskWorkRt.reset();
        m_currentMaskWorkRp.reset();
        m_currentMaskSize = QSize();
        m_currentMaskFillPipeline.reset();
        m_currentMaskFillDepthOnlyPipeline.reset();
        m_currentMaskEdgesPipeline.reset();
        m_currentMaskEdgesDepthPipeline.reset();
        m_currentMaskEdgesDepthOnlyPipeline.reset();
        m_currentMaskFatEdgesDepthOnlyPipeline.reset();
        m_currentMaskPointsPipeline.reset();
        m_currentMaskPointsDepthOnlyPipeline.reset();
        m_maskMorphCopyUbuf.reset();
        m_maskMorphDilateUbuf.reset();
        m_maskMorphErodeUbuf.reset();
        m_maskMorphSampler.reset();
        m_maskMorphMaskToBaseSrb.reset();
        m_maskMorphMaskToWorkSrb.reset();
        m_maskMorphWorkToMaskSrb.reset();
        m_maskMorphToBasePipeline.reset();
        m_maskMorphToWorkPipeline.reset();
        m_maskMorphWorkToMaskPipeline.reset();
        m_outlineExtractUbuf.reset();
        m_outlineExtractSrb.reset();
        m_outlineExtractPipeline.reset();
        m_maskDebugUbuf.reset();
        m_maskDebugBaseSrb.reset();
        m_maskDebugWorkSrb.reset();
        m_maskDebugMaskSrb.reset();
        m_maskDebugPipeline.reset();
        m_outlineUbuf.reset();
        m_outlineSampler.reset();
        m_outlineSrb.reset();
        m_outlinePipeline.reset();
        m_trackballGizmoUbuf.reset();
        m_trackballGizmoVbuf.reset();
        m_trackballGizmoSrb.reset();
        m_trackballGizmoPipeline.reset();
        m_trackballGizmoVertexCount = 0;
        m_lightGizmoUbuf.reset();
        m_lightGizmoVbuf.reset();
        m_lightGizmoSrb.reset();
        m_lightGizmoPipeline.reset();
        m_lightGizmoVertexCount = 0;
        m_rsGradPipeline.reset();
        m_rsGradSrb.reset();
        m_rsGradRp.reset();
        m_rsGradRt.reset();
        m_rsGradDepth.reset();
        m_rsGradTexture.reset();
        m_rsGradSize = QSize();
        m_srb.reset();
        m_ubuf.reset();
        m_sceneBackgroundUbuf.reset();
        m_sceneBackgroundSrb.reset();
        m_sceneBackgroundPipeline.reset();
        m_rasterBackplateUbuf.reset();
        m_rasterBackplateFallbackSrb.reset();
        m_rasterBackplatePipeline.reset();
        m_rasterProjectedUbuf.reset();
        m_rasterProjectedFallbackSrb.reset();
        m_rasterProjectedPipeline.reset();
        m_rastersGpu.clear();
        m_textureSampler.reset();
        m_textureSamplerNearest.reset();
        m_rasterSampler.reset();
        m_qualityColorMapSampler.reset();
        m_fallbackTexture.reset();
        m_fallbackTextureUploadPending = false;
        m_fallbackNormalTexture.reset();
        m_fallbackNormalTextureUploadPending = false;
        m_fallbackOcclusionTexture.reset();
        m_fallbackOcclusionTextureUploadPending = false;
        m_fallbackRoughnessTexture.reset();
        m_fallbackRoughnessTextureUploadPending = false;
        m_qualityColorMapTexture.reset();
        m_qualityColorMapTextureUploadPending = false;
        m_qualityColorMapTextureMapId.clear();
        m_qualityColorMapTextureInverted = false;
        m_qualityColorMapTextureIsolinesEnabled = false;
        m_qualityColorMapTextureIsolineCount = 0;
        m_textureSrbs.clear();
        m_uvBackgroundUbuf.reset();
        m_uvBackgroundSrb.reset();
        m_uvBackgroundPipeline.reset();
        m_uvTextureFillPipeline.reset();
        m_uvTextureQuadVbuf.reset();
        m_uvTextureQuadVertexCount = 0;
        m_uvAxesVbuf.reset();
        m_uvAxesVertexCount = 0;
        m_uvUnitBoxVbuf.reset();
        m_uvUnitBoxVertexCount = 0;
        for (auto &ubuf : m_uvLineUbufs)
            ubuf.reset();
        for (auto &srb : m_uvLineSrbs)
            srb.reset();
        m_uvLinePipelinesByKey.clear();
        m_uvMeshGpu.clear();
    }

    if (!m_rhi || !renderTarget())
        return;

    const int meshCount = m_doc ? m_doc->meshCount() : 0;
    const int rasterCount = m_doc ? m_doc->rasterCount() : 0;
    const int desiredMainUbufCapacity = qMax(64, meshCount * 32);
    const int desiredOverlayUbufCapacity = qMax(32, meshCount * 8);
    const int desiredRasterProjectedUbufCapacity = qMax(8, rasterCount * 4);
    const quint32 mainUbufStride = quint32(m_rhi->ubufAligned(kUbufSize));
    const quint32 decoratorUbufStride = quint32(m_rhi->ubufAligned(kDecoratorUbufSize));
    const quint32 decoratorFatUbufStride = quint32(m_rhi->ubufAligned(kDecoratorFatUbufSize));
    const quint32 rasterProjectedUbufStride = quint32(m_rhi->ubufAligned(kRasterProjectedUbufSize));

    const bool rebuildMainUbuf =
        !m_ubuf
        || m_mainUbufAllocator.stride != mainUbufStride
        || m_mainUbufAllocator.capacity < desiredMainUbufCapacity;
    if (rebuildMainUbuf) {
        m_srb.reset();
        m_depthPickSrb.reset();
        m_textureSrbs.clear();
        m_ubuf.reset(m_rhi->newBuffer(
            QRhiBuffer::Dynamic,
            QRhiBuffer::UniformBuffer,
            mainUbufStride * quint32(desiredMainUbufCapacity)));
        if (!m_ubuf || !m_ubuf->create()) {
            m_ubuf.reset();
            return;
        }
        m_mainUbufAllocator.stride = mainUbufStride;
        m_mainUbufAllocator.capacity = desiredMainUbufCapacity;
    }

    if (!m_textureSampler) {
        m_textureSampler.reset(
            m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                              QRhiSampler::Repeat, QRhiSampler::Repeat));
        if (!m_textureSampler || !m_textureSampler->create()) {
            m_textureSampler.reset();
            return;
        }
    }

    if (!m_textureSamplerNearest) {
        m_textureSamplerNearest.reset(
            m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                              QRhiSampler::Repeat, QRhiSampler::Repeat));
        if (!m_textureSamplerNearest || !m_textureSamplerNearest->create()) {
            m_textureSamplerNearest.reset();
            return;
        }
    }

    if (!m_rasterSampler) {
        m_rasterSampler.reset(
            m_rhi->newSampler(QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!m_rasterSampler || !m_rasterSampler->create()) {
            m_rasterSampler.reset();
            return;
        }
    }

    if (!m_qualityColorMapSampler) {
        // ClampToEdge, unlike the Repeat used for tiling albedo UVs. The LUT is a
        // 1024x1 ramp whose texel centres sit at (i+0.5)/1024, so a scalar clamped to
        // the top of its range samples at u=1.0 -> texel coordinate 1023.5, which with
        // Repeat blends the last texel with the *wrapped* first one: the maximum of a
        // gray ramp came out mid-gray instead of white, and the minimum likewise.
        m_qualityColorMapSampler.reset(
            m_rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                              QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!m_qualityColorMapSampler || !m_qualityColorMapSampler->create()) {
            m_qualityColorMapSampler.reset();
            return;
        }
    }

    if (!m_fallbackTexture) {
        m_fallbackTexture.reset(
            m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1));
        if (m_fallbackTexture && m_fallbackTexture->create()) {
            m_fallbackTextureUploadPending = true;
        } else {
            m_fallbackTexture.reset();
            m_fallbackTextureUploadPending = false;
        }
    }
    if (!m_fallbackNormalTexture) {
        m_fallbackNormalTexture.reset(
            m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1));
        if (m_fallbackNormalTexture && m_fallbackNormalTexture->create()) {
            m_fallbackNormalTextureUploadPending = true;
        } else {
            m_fallbackNormalTexture.reset();
            m_fallbackNormalTextureUploadPending = false;
        }
    }
    if (!m_fallbackOcclusionTexture) {
        m_fallbackOcclusionTexture.reset(
            m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1));
        if (m_fallbackOcclusionTexture && m_fallbackOcclusionTexture->create()) {
            m_fallbackOcclusionTextureUploadPending = true;
        } else {
            m_fallbackOcclusionTexture.reset();
            m_fallbackOcclusionTextureUploadPending = false;
        }
    }
    if (!m_fallbackRoughnessTexture) {
        m_fallbackRoughnessTexture.reset(
            m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1), 1));
        if (m_fallbackRoughnessTexture && m_fallbackRoughnessTexture->create()) {
            m_fallbackRoughnessTextureUploadPending = true;
        } else {
            m_fallbackRoughnessTexture.reset();
            m_fallbackRoughnessTextureUploadPending = false;
        }
    }

    if (!m_qualityColorMapTexture) {
        m_qualityColorMapTexture.reset(
            m_rhi->newTexture(QRhiTexture::RGBA8, QSize(1024, 1), 1));
        if (m_qualityColorMapTexture && m_qualityColorMapTexture->create()) {
            m_qualityColorMapTextureUploadPending = true;
            m_qualityColorMapTextureMapId.clear();
            m_qualityColorMapTextureInverted = false;
            m_qualityColorMapTextureIsolinesEnabled = false;
            m_qualityColorMapTextureIsolineCount = 0;
        } else {
            m_qualityColorMapTexture.reset();
            m_qualityColorMapTextureUploadPending = false;
            m_qualityColorMapTextureMapId.clear();
            m_qualityColorMapTextureInverted = false;
            m_qualityColorMapTextureIsolinesEnabled = false;
            m_qualityColorMapTextureIsolineCount = 0;
        }
    }

    ensureCurrentMeshMaskResources(renderTarget()->pixelSize());

    if (!m_srb) {
        if (!m_ubuf
            || !m_textureSampler
            || !m_qualityColorMapSampler
            || !m_fallbackTexture
            || !m_fallbackNormalTexture
            || !m_fallbackOcclusionTexture
            || !m_fallbackRoughnessTexture
            || !m_qualityColorMapTexture) {
            return;
        }
        m_srb.reset(m_rhi->newShaderResourceBindings());
        m_srb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                m_ubuf.get(),
                kUbufSize),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_fallbackTexture.get(),
                m_textureSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2,
                QRhiShaderResourceBinding::FragmentStage,
                m_qualityColorMapTexture.get(),
                m_qualityColorMapSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                3,
                QRhiShaderResourceBinding::FragmentStage,
                m_fallbackNormalTexture.get(),
                m_textureSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                4,
                QRhiShaderResourceBinding::FragmentStage,
                m_fallbackOcclusionTexture.get(),
                m_textureSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                5,
                QRhiShaderResourceBinding::FragmentStage,
                m_fallbackRoughnessTexture.get(),
                m_textureSampler.get())
        });
        if (!m_srb->create()) {
            m_srb.reset();
            return;
        }
    }

    if (!m_sceneBackgroundUbuf) {
        m_sceneBackgroundUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kSceneBackgroundUbufSize));
        if (!m_sceneBackgroundUbuf || !m_sceneBackgroundUbuf->create())
            m_sceneBackgroundUbuf.reset();
    }

    if (!m_sceneBackgroundSrb && m_sceneBackgroundUbuf) {
        m_sceneBackgroundSrb.reset(m_rhi->newShaderResourceBindings());
        m_sceneBackgroundSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_sceneBackgroundUbuf.get())
        });
        if (!m_sceneBackgroundSrb->create())
            m_sceneBackgroundSrb.reset();
    }

    if (!m_sceneBackgroundPipeline && m_sceneBackgroundSrb) {
        m_sceneBackgroundPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/scene_background.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/scene_background.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load scene background shaders");
            m_sceneBackgroundPipeline.reset();
        } else {
            m_sceneBackgroundPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_sceneBackgroundPipeline->setDepthTest(false);
            m_sceneBackgroundPipeline->setDepthWrite(false);
            m_sceneBackgroundPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            m_sceneBackgroundPipeline->setVertexInputLayout(layout);
            m_sceneBackgroundPipeline->setShaderResourceBindings(m_sceneBackgroundSrb.get());
            m_sceneBackgroundPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_sceneBackgroundPipeline->create()) {
                qWarning("Failed to create scene background pipeline");
                m_sceneBackgroundPipeline.reset();
            }
        }
    }

    if (!m_rasterBackplateUbuf) {
        m_rasterBackplateUbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                kRasterBackplateUbufSize));
        if (!m_rasterBackplateUbuf || !m_rasterBackplateUbuf->create())
            m_rasterBackplateUbuf.reset();
    }

    if (!m_rasterBackplateFallbackSrb
        && m_rasterBackplateUbuf
        && m_rasterSampler
        && m_fallbackTexture) {
        m_rasterBackplateFallbackSrb.reset(m_rhi->newShaderResourceBindings());
        m_rasterBackplateFallbackSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                m_rasterBackplateUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_fallbackTexture.get(),
                m_rasterSampler.get())
        });
        if (!m_rasterBackplateFallbackSrb->create())
            m_rasterBackplateFallbackSrb.reset();
    }

    if (!m_rasterBackplatePipeline && m_rasterBackplateFallbackSrb) {
        m_rasterBackplatePipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/raster_backplate.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/raster_backplate.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load raster backplate shaders");
            m_rasterBackplatePipeline.reset();
        } else {
            m_rasterBackplatePipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_rasterBackplatePipeline->setDepthTest(false);
            m_rasterBackplatePipeline->setDepthWrite(false);
            m_rasterBackplatePipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_rasterBackplatePipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            m_rasterBackplatePipeline->setVertexInputLayout(layout);
            m_rasterBackplatePipeline->setShaderResourceBindings(
                m_rasterBackplateFallbackSrb.get());
            m_rasterBackplatePipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_rasterBackplatePipeline->create()) {
                qWarning("Failed to create raster backplate pipeline");
                m_rasterBackplatePipeline.reset();
            }
        }
    }

    const bool rebuildRasterProjectedUbuf =
        !m_rasterProjectedUbuf
        || m_rasterProjectedUbufAllocator.stride != rasterProjectedUbufStride
        || m_rasterProjectedUbufAllocator.capacity < desiredRasterProjectedUbufCapacity;
    if (rebuildRasterProjectedUbuf) {
        m_rasterProjectedFallbackSrb.reset();
        for (auto &[id, gpu] : m_rastersGpu)
            gpu.projectedSrb.reset();
        m_rasterProjectedUbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                rasterProjectedUbufStride * quint32(desiredRasterProjectedUbufCapacity)));
        if (!m_rasterProjectedUbuf || !m_rasterProjectedUbuf->create())
            m_rasterProjectedUbuf.reset();
        m_rasterProjectedUbufAllocator.stride = rasterProjectedUbufStride;
        m_rasterProjectedUbufAllocator.capacity = desiredRasterProjectedUbufCapacity;
    }

    if (!m_rasterProjectedFallbackSrb && m_rasterProjectedUbuf) {
        m_rasterProjectedFallbackSrb.reset(m_rhi->newShaderResourceBindings());
        m_rasterProjectedFallbackSrb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                m_rasterProjectedUbuf.get(),
                kRasterProjectedUbufSize)
        });
        if (!m_rasterProjectedFallbackSrb->create())
            m_rasterProjectedFallbackSrb.reset();
    }

    if (!m_rasterProjectedPipeline && m_rasterProjectedFallbackSrb) {
        m_rasterProjectedPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/raster_projected.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/raster_projected.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load raster projected shaders");
            m_rasterProjectedPipeline.reset();
        } else {
            m_rasterProjectedPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_rasterProjectedPipeline->setTopology(QRhiGraphicsPipeline::Lines);
            m_rasterProjectedPipeline->setDepthTest(true);
            m_rasterProjectedPipeline->setDepthWrite(false);
            m_rasterProjectedPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_rasterProjectedPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_rasterProjectedPipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kRasterProjectedVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({
                { 0, 0, QRhiVertexInputAttribute::Float3, 0 }
            });
            m_rasterProjectedPipeline->setVertexInputLayout(layout);
            m_rasterProjectedPipeline->setShaderResourceBindings(
                m_rasterProjectedFallbackSrb.get());
            m_rasterProjectedPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_rasterProjectedPipeline->create()) {
                qWarning("Failed to create raster projected pipeline");
                m_rasterProjectedPipeline.reset();
            }
        }
    }

    for (int slot = 0; slot < kDecoratorSlotCount; ++slot) {
        const bool rebuildDecoratorUbuf =
            !m_decoratorUbufs[slot]
            || m_decoratorUbufAllocators[slot].stride != decoratorUbufStride
            || m_decoratorUbufAllocators[slot].capacity < desiredOverlayUbufCapacity;
        if (rebuildDecoratorUbuf) {
            m_decoratorSrbs[slot].reset();
            m_decoratorUbufs[slot].reset(
                m_rhi->newBuffer(
                    QRhiBuffer::Dynamic,
                    QRhiBuffer::UniformBuffer,
                    decoratorUbufStride * quint32(desiredOverlayUbufCapacity)));
            if (!m_decoratorUbufs[slot] || !m_decoratorUbufs[slot]->create())
                m_decoratorUbufs[slot].reset();
            m_decoratorUbufAllocators[slot].stride = decoratorUbufStride;
            m_decoratorUbufAllocators[slot].capacity = desiredOverlayUbufCapacity;
        }
        if (!m_decoratorSrbs[slot] && m_decoratorUbufs[slot]) {
            m_decoratorSrbs[slot].reset(m_rhi->newShaderResourceBindings());
            m_decoratorSrbs[slot]->setBindings({
                QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                    0,
                    QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                    m_decoratorUbufs[slot].get(),
                    kDecoratorUbufSize)
            });
            if (!m_decoratorSrbs[slot]->create())
                m_decoratorSrbs[slot].reset();
        }
    }

    if (!m_outlineUbuf) {
        m_outlineUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kOutlineUbufSize));
        if (!m_outlineUbuf || !m_outlineUbuf->create())
            m_outlineUbuf.reset();
    }
    if (!m_outlineSampler) {
        m_outlineSampler.reset(
            m_rhi->newSampler(
                QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!m_outlineSampler || !m_outlineSampler->create())
            m_outlineSampler.reset();
    }
    if (!m_maskMorphCopyUbuf) {
        m_maskMorphCopyUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMaskMorphUbufSize));
        if (!m_maskMorphCopyUbuf || !m_maskMorphCopyUbuf->create())
            m_maskMorphCopyUbuf.reset();
    }
    if (!m_maskMorphDilateUbuf) {
        m_maskMorphDilateUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMaskMorphUbufSize));
        if (!m_maskMorphDilateUbuf || !m_maskMorphDilateUbuf->create())
            m_maskMorphDilateUbuf.reset();
    }
    if (!m_maskMorphErodeUbuf) {
        m_maskMorphErodeUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMaskMorphUbufSize));
        if (!m_maskMorphErodeUbuf || !m_maskMorphErodeUbuf->create())
            m_maskMorphErodeUbuf.reset();
    }
    if (!m_maskMorphSampler) {
        m_maskMorphSampler.reset(
            m_rhi->newSampler(
                QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
                QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!m_maskMorphSampler || !m_maskMorphSampler->create())
            m_maskMorphSampler.reset();
    }
    if (!m_maskDebugUbuf) {
        m_maskDebugUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMaskDebugUbufSize));
        if (!m_maskDebugUbuf || !m_maskDebugUbuf->create())
            m_maskDebugUbuf.reset();
    }
    if (!m_maskMorphMaskToBaseSrb
        && m_maskMorphCopyUbuf
        && m_maskMorphSampler
        && m_currentMaskTexture
        && m_currentMaskBaseTexture) {
        m_maskMorphMaskToBaseSrb.reset(m_rhi->newShaderResourceBindings());
        m_maskMorphMaskToBaseSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_maskMorphCopyUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskTexture.get(),
                m_maskMorphSampler.get())
        });
        if (!m_maskMorphMaskToBaseSrb->create())
            m_maskMorphMaskToBaseSrb.reset();
    }
    if (!m_maskMorphMaskToWorkSrb
        && m_maskMorphDilateUbuf
        && m_maskMorphSampler
        && m_currentMaskBaseTexture
        && m_currentMaskWorkTexture) {
        m_maskMorphMaskToWorkSrb.reset(m_rhi->newShaderResourceBindings());
        m_maskMorphMaskToWorkSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_maskMorphDilateUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskBaseTexture.get(),
                m_maskMorphSampler.get())
        });
        if (!m_maskMorphMaskToWorkSrb->create())
            m_maskMorphMaskToWorkSrb.reset();
    }
    if (!m_maskMorphWorkToMaskSrb
        && m_maskMorphErodeUbuf
        && m_maskMorphSampler
        && m_currentMaskTexture
        && m_currentMaskWorkTexture) {
        m_maskMorphWorkToMaskSrb.reset(m_rhi->newShaderResourceBindings());
        m_maskMorphWorkToMaskSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_maskMorphErodeUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskWorkTexture.get(),
                m_maskMorphSampler.get())
        });
        if (!m_maskMorphWorkToMaskSrb->create())
            m_maskMorphWorkToMaskSrb.reset();
    }
    if (!m_outlineExtractUbuf) {
        m_outlineExtractUbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                kOutlineExtractUbufSize));
        if (!m_outlineExtractUbuf || !m_outlineExtractUbuf->create())
            m_outlineExtractUbuf.reset();
    }
    if (!m_outlineExtractSrb
        && m_outlineExtractUbuf
        && m_maskMorphSampler
        && m_currentMaskBaseTexture) {
        m_outlineExtractSrb.reset(m_rhi->newShaderResourceBindings());
        m_outlineExtractSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_outlineExtractUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskBaseTexture.get(),
                m_maskMorphSampler.get())
        });
        if (!m_outlineExtractSrb->create())
            m_outlineExtractSrb.reset();
    }
    if (!m_outlineExtractPipeline && m_outlineExtractSrb && m_currentMaskWorkRp) {
        m_outlineExtractPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_outline.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_outline_extract.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load outline-extract shaders");
            m_outlineExtractPipeline.reset();
        } else {
            m_outlineExtractPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_outlineExtractPipeline->setDepthTest(false);
            m_outlineExtractPipeline->setDepthWrite(false);
            m_outlineExtractPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            m_outlineExtractPipeline->setVertexInputLayout(layout);
            m_outlineExtractPipeline->setShaderResourceBindings(m_outlineExtractSrb.get());
            m_outlineExtractPipeline->setRenderPassDescriptor(m_currentMaskWorkRp.get());
            if (!m_outlineExtractPipeline->create()) {
                qWarning("Failed to create outline-extract pipeline");
                m_outlineExtractPipeline.reset();
            }
        }
    }
    if (!m_maskDebugBaseSrb
        && m_maskDebugUbuf
        && m_maskMorphSampler
        && m_currentMaskBaseTexture
        && m_currentMaskTexture) {
        m_maskDebugBaseSrb.reset(m_rhi->newShaderResourceBindings());
        m_maskDebugBaseSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_maskDebugUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskBaseTexture.get(),
                m_maskMorphSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskTexture.get(),
                m_maskMorphSampler.get())
        });
        if (!m_maskDebugBaseSrb->create())
            m_maskDebugBaseSrb.reset();
    }
    if (!m_maskDebugWorkSrb
        && m_maskDebugUbuf
        && m_maskMorphSampler
        && m_currentMaskWorkTexture
        && m_currentMaskTexture) {
        m_maskDebugWorkSrb.reset(m_rhi->newShaderResourceBindings());
        m_maskDebugWorkSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_maskDebugUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskWorkTexture.get(),
                m_maskMorphSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskTexture.get(),
                m_maskMorphSampler.get())
        });
        if (!m_maskDebugWorkSrb->create())
            m_maskDebugWorkSrb.reset();
    }
    if (!m_maskDebugMaskSrb
        && m_maskDebugUbuf
        && m_maskMorphSampler
        && m_currentMaskTexture
        && m_currentMaskBaseTexture) {
        m_maskDebugMaskSrb.reset(m_rhi->newShaderResourceBindings());
        m_maskDebugMaskSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_maskDebugUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskTexture.get(),
                m_maskMorphSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskBaseTexture.get(),
                m_maskMorphSampler.get())
        });
        if (!m_maskDebugMaskSrb->create())
            m_maskDebugMaskSrb.reset();
    }
    if (!m_outlineSrb
        && m_outlineUbuf
        && m_outlineSampler
        && m_currentMaskWorkTexture
        && m_currentMaskBaseTexture
        && m_currentMaskTexture) {
        m_outlineSrb.reset(m_rhi->newShaderResourceBindings());
        m_outlineSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::FragmentStage,
                m_outlineUbuf.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskWorkTexture.get(),
                m_outlineSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                2,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskBaseTexture.get(),
                m_outlineSampler.get()),
            QRhiShaderResourceBinding::sampledTexture(
                3,
                QRhiShaderResourceBinding::FragmentStage,
                m_currentMaskTexture.get(),
                m_outlineSampler.get())
        });
        if (!m_outlineSrb->create())
            m_outlineSrb.reset();
    }

    if (!m_bboxPipeline) {
        m_bboxPipeline.reset(m_rhi->newGraphicsPipeline());

        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_bbox.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_bbox.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load bbox shaders");
            m_bboxPipeline.reset();
            return;
        }

        m_bboxPipeline->setShaderStages({
            { QRhiShaderStage::Vertex, vs },
            { QRhiShaderStage::Fragment, fs }
        });
        m_bboxPipeline->setTopology(QRhiGraphicsPipeline::Lines);
        m_bboxPipeline->setDepthTest(true);
        m_bboxPipeline->setDepthWrite(false);
        m_bboxPipeline->setCullMode(QRhiGraphicsPipeline::None);

        QRhiVertexInputLayout bboxLayout;
        bboxLayout.setBindings({ { 3 * sizeof(float) } });
        bboxLayout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
        m_bboxPipeline->setVertexInputLayout(bboxLayout);
        m_bboxPipeline->setShaderResourceBindings(m_srb.get());
        m_bboxPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

        if (!m_bboxPipeline->create()) {
            qWarning("Failed to create bbox pipeline");
            m_bboxPipeline.reset();
        }
    }

    if (!m_pointsPipeline) {
        m_pointsPipeline.reset(m_rhi->newGraphicsPipeline());

        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_points.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_points.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load points shaders");
            m_pointsPipeline.reset();
            return;
        }

        m_pointsPipeline->setShaderStages({
            { QRhiShaderStage::Vertex, vs },
            { QRhiShaderStage::Fragment, fs }
        });
        m_pointsPipeline->setTopology(QRhiGraphicsPipeline::Points);
        m_pointsPipeline->setDepthTest(true);
        m_pointsPipeline->setDepthWrite(true);
        m_pointsPipeline->setCullMode(QRhiGraphicsPipeline::None);

        QRhiVertexInputLayout ptsLayout;
        // Points vertex layout: position(3f) + meshColor(3f) + useMeshColorFlag(1f)
        // + normal(3f) + useNormalFlag(1f)
        ptsLayout.setBindings({ { kPointsVertexStrideFloats * sizeof(float) } });
        ptsLayout.setAttributes({
            { 0, 0, QRhiVertexInputAttribute::Float3, 0 },                 // position
            { 0, 1, QRhiVertexInputAttribute::Float4, 3 * sizeof(float) }, // mesh color + use flag
            { 0, 2, QRhiVertexInputAttribute::Float4, 7 * sizeof(float) }  // normal + use flag
        });
        m_pointsPipeline->setVertexInputLayout(ptsLayout);
        m_pointsPipeline->setShaderResourceBindings(m_srb.get());
        m_pointsPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());

        if (!m_pointsPipeline->create()) {
            qWarning("Failed to create points pipeline");
            m_pointsPipeline.reset();
        }
    }

    if (!m_decoratorPipeline && m_decoratorSrbs[kDecoratorSlotVertexNormals]) {
        m_decoratorPipeline.reset(m_rhi->newGraphicsPipeline());

        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_decorator.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_decorator.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load decorator shaders");
            m_decoratorPipeline.reset();
        } else {
            m_decoratorPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_decoratorPipeline->setTopology(QRhiGraphicsPipeline::Lines);
            m_decoratorPipeline->setDepthTest(true);
            m_decoratorPipeline->setDepthWrite(false);
            m_decoratorPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_decoratorPipeline->setDepthBias(-1);
            m_decoratorPipeline->setSlopeScaledDepthBias(-1.0f);
            m_decoratorPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 3 * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_decoratorPipeline->setVertexInputLayout(layout);
            m_decoratorPipeline->setShaderResourceBindings(
                m_decoratorSrbs[kDecoratorSlotVertexNormals].get());
            m_decoratorPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_decoratorPipeline->create()) {
                qWarning("Failed to create decorator pipeline");
                m_decoratorPipeline.reset();
            }
        }
    }

    if (!m_decoratorPointPipeline && m_decoratorSrbs[kDecoratorSlotNonManifoldVertices]) {
        m_decoratorPointPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_decorator.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_decorator.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load decorator point shaders");
            m_decoratorPointPipeline.reset();
        } else {
            m_decoratorPointPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_decoratorPointPipeline->setTopology(QRhiGraphicsPipeline::Points);
            m_decoratorPointPipeline->setDepthTest(true);
            m_decoratorPointPipeline->setDepthWrite(false);
            m_decoratorPointPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_decoratorPointPipeline->setDepthBias(-2);
            m_decoratorPointPipeline->setSlopeScaledDepthBias(-2.0f);
            m_decoratorPointPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 3 * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_decoratorPointPipeline->setVertexInputLayout(layout);
            m_decoratorPointPipeline->setShaderResourceBindings(
                m_decoratorSrbs[kDecoratorSlotNonManifoldVertices].get());
            m_decoratorPointPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_decoratorPointPipeline->create()) {
                qWarning("Failed to create decorator point pipeline");
                m_decoratorPointPipeline.reset();
            }
        }
    }

    const bool rebuildSelectionUbuf =
        !m_selectionUbuf
        || m_selectionUbufAllocator.stride != decoratorUbufStride
        || m_selectionUbufAllocator.capacity < desiredOverlayUbufCapacity;
    if (rebuildSelectionUbuf) {
        m_selectionSrb.reset();
        m_selectionUbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                decoratorUbufStride * quint32(desiredOverlayUbufCapacity)));
        if (!m_selectionUbuf || !m_selectionUbuf->create())
            m_selectionUbuf.reset();
        m_selectionUbufAllocator.stride = decoratorUbufStride;
        m_selectionUbufAllocator.capacity = desiredOverlayUbufCapacity;
    }
    if (!m_selectionSrb && m_selectionUbuf) {
        m_selectionSrb.reset(m_rhi->newShaderResourceBindings());
        m_selectionSrb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                m_selectionUbuf.get(),
                kDecoratorUbufSize)
        });
        if (!m_selectionSrb->create())
            m_selectionSrb.reset();
    }
    if (!m_selectionFacesPipeline && m_selectionSrb) {
        m_selectionFacesPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_decorator.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_decorator.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load selection face shaders");
            m_selectionFacesPipeline.reset();
        } else {
            m_selectionFacesPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_selectionFacesPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
            m_selectionFacesPipeline->setDepthTest(true);
            m_selectionFacesPipeline->setDepthWrite(false);
            m_selectionFacesPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_selectionFacesPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_selectionFacesPipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 3 * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_selectionFacesPipeline->setVertexInputLayout(layout);
            m_selectionFacesPipeline->setShaderResourceBindings(m_selectionSrb.get());
            m_selectionFacesPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_selectionFacesPipeline->create()) {
                qWarning("Failed to create selection face pipeline");
                m_selectionFacesPipeline.reset();
            }
        }
    }
    if (!m_selectionVerticesPipeline && m_selectionSrb) {
        m_selectionVerticesPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_decorator.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_decorator.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load selection vertex shaders");
            m_selectionVerticesPipeline.reset();
        } else {
            m_selectionVerticesPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_selectionVerticesPipeline->setTopology(QRhiGraphicsPipeline::Points);
            m_selectionVerticesPipeline->setDepthTest(true);
            m_selectionVerticesPipeline->setDepthWrite(false);
            m_selectionVerticesPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_selectionVerticesPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_selectionVerticesPipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 3 * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_selectionVerticesPipeline->setVertexInputLayout(layout);
            m_selectionVerticesPipeline->setShaderResourceBindings(m_selectionSrb.get());
            m_selectionVerticesPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_selectionVerticesPipeline->create()) {
                qWarning("Failed to create selection vertex pipeline");
                m_selectionVerticesPipeline.reset();
            }
        }
    }

    const bool rebuildDecoratorFatUbuf =
        !m_decoratorFatUbuf
        || m_decoratorFatUbufAllocator.stride != decoratorFatUbufStride
        || m_decoratorFatUbufAllocator.capacity < desiredOverlayUbufCapacity;
    if (rebuildDecoratorFatUbuf) {
        m_decoratorFatSrb.reset();
        m_decoratorFatUbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::UniformBuffer,
                decoratorFatUbufStride * quint32(desiredOverlayUbufCapacity)));
        if (!m_decoratorFatUbuf || !m_decoratorFatUbuf->create())
            m_decoratorFatUbuf.reset();
        m_decoratorFatUbufAllocator.stride = decoratorFatUbufStride;
        m_decoratorFatUbufAllocator.capacity = desiredOverlayUbufCapacity;
    }
    if (!m_decoratorFatSrb && m_decoratorFatUbuf) {
        m_decoratorFatSrb.reset(m_rhi->newShaderResourceBindings());
        m_decoratorFatSrb->setBindings({
            QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
                0,
                QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                m_decoratorFatUbuf.get(),
                kDecoratorFatUbufSize)
        });
        if (!m_decoratorFatSrb->create())
            m_decoratorFatSrb.reset();
    }
    if (!m_decoratorFatPipeline && m_decoratorFatSrb) {
        m_decoratorFatPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_fat_decorator.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_fat_decorator.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load fat decorator shaders");
            m_decoratorFatPipeline.reset();
        } else {
            m_decoratorFatPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_decoratorFatPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
            m_decoratorFatPipeline->setDepthTest(true);
            m_decoratorFatPipeline->setDepthWrite(false);
            m_decoratorFatPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_decoratorFatPipeline->setDepthBias(-1);
            m_decoratorFatPipeline->setSlopeScaledDepthBias(-1.0f);
            m_decoratorFatPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_decoratorFatPipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 8 * sizeof(float) } });
            layout.setAttributes({
                { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
                { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },
                { 0, 2, QRhiVertexInputAttribute::Float, 6 * sizeof(float) },
                { 0, 3, QRhiVertexInputAttribute::Float, 7 * sizeof(float) }
            });
            m_decoratorFatPipeline->setVertexInputLayout(layout);
            m_decoratorFatPipeline->setShaderResourceBindings(m_decoratorFatSrb.get());
            m_decoratorFatPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_decoratorFatPipeline->create()) {
                qWarning("Failed to create fat decorator pipeline");
                m_decoratorFatPipeline.reset();
            }
        }
    }

    for (int pass = 0; pass < 2; ++pass) {
        if (!m_toolLineUbufs[pass]) {
            m_toolLineUbufs[pass].reset(
                m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kToolLineUbufSize));
            if (!m_toolLineUbufs[pass] || !m_toolLineUbufs[pass]->create())
                m_toolLineUbufs[pass].reset();
        }
        if (!m_toolLineSrbs[pass] && m_toolLineUbufs[pass]) {
            m_toolLineSrbs[pass].reset(m_rhi->newShaderResourceBindings());
            m_toolLineSrbs[pass]->setBindings({
                QRhiShaderResourceBinding::uniformBuffer(
                    0,
                    QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
                    m_toolLineUbufs[pass].get())
            });
            if (!m_toolLineSrbs[pass]->create())
                m_toolLineSrbs[pass].reset();
        }
    }
    // Both passes share geometry and shaders.  Only the second pipeline tests
    // scene depth; neither writes it, so this visual cue cannot affect the scene.
    auto makeToolLinePipeline = [&](std::unique_ptr<QRhiGraphicsPipeline> &pipeline,
                                    QRhiShaderResourceBindings *srb,
                                    bool depthTest) {
        if (pipeline || !srb)
            return;
        pipeline.reset(m_rhi->newGraphicsPipeline());
        const QShader vs = loadShader(QStringLiteral(":/shaders/overlay_fat_decorator.vert.qsb"));
        const QShader fs = loadShader(QStringLiteral(":/shaders/overlay_fat_decorator.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load interactive-tool line shaders");
            pipeline.reset();
            return;
        }
        pipeline->setShaderStages({
            { QRhiShaderStage::Vertex, vs },
            { QRhiShaderStage::Fragment, fs }
        });
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setDepthTest(depthTest);
        pipeline->setDepthWrite(false);
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
        pipeline->setTargetBlends({blend});
        QRhiVertexInputLayout layout;
        layout.setBindings({{LineRenderer::kFatLineStrideFloats * sizeof(float)}});
        layout.setAttributes({
            {0, 0, QRhiVertexInputAttribute::Float3, 0},
            {0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float)},
            {0, 2, QRhiVertexInputAttribute::Float, 6 * sizeof(float)},
            {0, 3, QRhiVertexInputAttribute::Float, 7 * sizeof(float)}
        });
        pipeline->setVertexInputLayout(layout);
        pipeline->setShaderResourceBindings(srb);
        pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
        if (!pipeline->create()) {
            qWarning("Failed to create interactive-tool line pipeline");
            pipeline.reset();
        }
    };
    makeToolLinePipeline(m_toolLineHiddenPipeline, m_toolLineSrbs[0].get(), false);
    makeToolLinePipeline(m_toolLineVisiblePipeline, m_toolLineSrbs[1].get(), true);

    if (!m_currentMaskFillPipeline && m_currentMaskRp) {
        m_currentMaskFillPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_mask.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask shaders");
            m_currentMaskFillPipeline.reset();
        } else {
            m_currentMaskFillPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskFillPipeline->setDepthTest(true);
            m_currentMaskFillPipeline->setDepthWrite(true);
            m_currentMaskFillPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            // Slight negative bias avoids self-occlusion when comparing current mesh
            // against a scene depth prepass that already contains the same geometry.
            m_currentMaskFillPipeline->setDepthBias(-1);
            m_currentMaskFillPipeline->setSlopeScaledDepthBias(-1.0f);
            // Outline mask generation should include both front and back faces.
            m_currentMaskFillPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kFillVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_currentMaskFillPipeline->setVertexInputLayout(layout);
            m_currentMaskFillPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskFillPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskFillPipeline->create()) {
                qWarning("Failed to create current-mask fill pipeline");
                m_currentMaskFillPipeline.reset();
            }
        }
    }

    if (!m_currentMaskFillDepthOnlyPipeline && m_currentMaskRp) {
        m_currentMaskFillDepthOnlyPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/depth_pick.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/depth_pick.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask fill depth-only shaders");
            m_currentMaskFillDepthOnlyPipeline.reset();
        } else {
            m_currentMaskFillDepthOnlyPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskFillDepthOnlyPipeline->setDepthTest(true);
            m_currentMaskFillDepthOnlyPipeline->setDepthWrite(true);
            m_currentMaskFillDepthOnlyPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kFillVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_currentMaskFillDepthOnlyPipeline->setVertexInputLayout(layout);
            m_currentMaskFillDepthOnlyPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskFillDepthOnlyPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskFillDepthOnlyPipeline->create()) {
                qWarning("Failed to create current-mask fill depth-only pipeline");
                m_currentMaskFillDepthOnlyPipeline.reset();
            }
        }
    }

    if (!m_currentMaskEdgesPipeline && m_currentMaskRp) {
        m_currentMaskEdgesPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_mask.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask edge shaders");
            m_currentMaskEdgesPipeline.reset();
        } else {
            m_currentMaskEdgesPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskEdgesPipeline->setTopology(QRhiGraphicsPipeline::Lines);
            // Edge-mesh outline mask should capture all visible segments robustly.
            m_currentMaskEdgesPipeline->setDepthTest(false);
            m_currentMaskEdgesPipeline->setDepthWrite(false);
            m_currentMaskEdgesPipeline->setCullMode(QRhiGraphicsPipeline::None);
            m_currentMaskEdgesPipeline->setLineWidth(1.0f);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 3 * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_currentMaskEdgesPipeline->setVertexInputLayout(layout);
            m_currentMaskEdgesPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskEdgesPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskEdgesPipeline->create()) {
                qWarning("Failed to create current-mask edges pipeline");
                m_currentMaskEdgesPipeline.reset();
            }
        }
    }

    if (!m_currentMaskEdgesDepthPipeline && m_currentMaskRp) {
        m_currentMaskEdgesDepthPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_mask.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask depth-edges shaders");
            m_currentMaskEdgesDepthPipeline.reset();
        } else {
            m_currentMaskEdgesDepthPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskEdgesDepthPipeline->setTopology(QRhiGraphicsPipeline::Lines);
            m_currentMaskEdgesDepthPipeline->setDepthTest(true);
            m_currentMaskEdgesDepthPipeline->setDepthWrite(true);
            m_currentMaskEdgesDepthPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_currentMaskEdgesDepthPipeline->setDepthBias(-1);
            m_currentMaskEdgesDepthPipeline->setSlopeScaledDepthBias(-1.0f);
            m_currentMaskEdgesDepthPipeline->setCullMode(QRhiGraphicsPipeline::None);
            m_currentMaskEdgesDepthPipeline->setLineWidth(1.0f);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 3 * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_currentMaskEdgesDepthPipeline->setVertexInputLayout(layout);
            m_currentMaskEdgesDepthPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskEdgesDepthPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskEdgesDepthPipeline->create()) {
                qWarning("Failed to create current-mask depth-edges pipeline");
                m_currentMaskEdgesDepthPipeline.reset();
            }
        }
    }

    if (!m_currentMaskEdgesDepthOnlyPipeline && m_currentMaskRp) {
        m_currentMaskEdgesDepthOnlyPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/depth_pick.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/depth_pick.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask edge depth-only shaders");
            m_currentMaskEdgesDepthOnlyPipeline.reset();
        } else {
            m_currentMaskEdgesDepthOnlyPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskEdgesDepthOnlyPipeline->setTopology(QRhiGraphicsPipeline::Lines);
            m_currentMaskEdgesDepthOnlyPipeline->setDepthTest(true);
            m_currentMaskEdgesDepthOnlyPipeline->setDepthWrite(true);
            m_currentMaskEdgesDepthOnlyPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_currentMaskEdgesDepthOnlyPipeline->setCullMode(QRhiGraphicsPipeline::None);
            m_currentMaskEdgesDepthOnlyPipeline->setLineWidth(1.0f);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 3 * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_currentMaskEdgesDepthOnlyPipeline->setVertexInputLayout(layout);
            m_currentMaskEdgesDepthOnlyPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskEdgesDepthOnlyPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskEdgesDepthOnlyPipeline->create()) {
                qWarning("Failed to create current-mask edge depth-only pipeline");
                m_currentMaskEdgesDepthOnlyPipeline.reset();
            }
        }
    }

    if (!m_currentMaskFatEdgesDepthOnlyPipeline && m_currentMaskRp) {
        m_currentMaskFatEdgesDepthOnlyPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_fat_edges.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/depth_pick.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask fat-edge depth-only shaders");
            m_currentMaskFatEdgesDepthOnlyPipeline.reset();
        } else {
            m_currentMaskFatEdgesDepthOnlyPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskFatEdgesDepthOnlyPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
            m_currentMaskFatEdgesDepthOnlyPipeline->setDepthTest(true);
            m_currentMaskFatEdgesDepthOnlyPipeline->setDepthWrite(true);
            m_currentMaskFatEdgesDepthOnlyPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_currentMaskFatEdgesDepthOnlyPipeline->setDepthBias(-1);
            m_currentMaskFatEdgesDepthOnlyPipeline->setSlopeScaledDepthBias(-1.0f);
            m_currentMaskFatEdgesDepthOnlyPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 8 * sizeof(float) } });
            layout.setAttributes({
                { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
                { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },
                { 0, 2, QRhiVertexInputAttribute::Float, 6 * sizeof(float) },
                { 0, 3, QRhiVertexInputAttribute::Float, 7 * sizeof(float) }
            });
            m_currentMaskFatEdgesDepthOnlyPipeline->setVertexInputLayout(layout);
            m_currentMaskFatEdgesDepthOnlyPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskFatEdgesDepthOnlyPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskFatEdgesDepthOnlyPipeline->create()) {
                qWarning("Failed to create current-mask fat-edge depth-only pipeline");
                m_currentMaskFatEdgesDepthOnlyPipeline.reset();
            }
        }
    }

    if (!m_currentMaskPointsPipeline && m_currentMaskRp) {
        m_currentMaskPointsPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_mask.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask point shaders");
            m_currentMaskPointsPipeline.reset();
        } else {
            m_currentMaskPointsPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskPointsPipeline->setTopology(QRhiGraphicsPipeline::Points);
            // Point-cloud outline mask: draw all vertices as pure occupancy (no depth/shading).
            m_currentMaskPointsPipeline->setDepthTest(false);
            m_currentMaskPointsPipeline->setDepthWrite(false);
            m_currentMaskPointsPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kPointsVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_currentMaskPointsPipeline->setVertexInputLayout(layout);
            m_currentMaskPointsPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskPointsPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskPointsPipeline->create()) {
                qWarning("Failed to create current-mask points pipeline");
                m_currentMaskPointsPipeline.reset();
            }
        }
    }

    if (!m_currentMaskPointsDepthOnlyPipeline && m_currentMaskRp) {
        m_currentMaskPointsDepthOnlyPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/depth_pick.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/depth_pick.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load current-mask points depth-only shaders");
            m_currentMaskPointsDepthOnlyPipeline.reset();
        } else {
            m_currentMaskPointsDepthOnlyPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_currentMaskPointsDepthOnlyPipeline->setTopology(QRhiGraphicsPipeline::Points);
            m_currentMaskPointsDepthOnlyPipeline->setDepthTest(true);
            m_currentMaskPointsDepthOnlyPipeline->setDepthWrite(true);
            m_currentMaskPointsDepthOnlyPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            layout.setBindings({ { kPointsVertexStrideFloats * sizeof(float) } });
            layout.setAttributes({ { 0, 0, QRhiVertexInputAttribute::Float3, 0 } });
            m_currentMaskPointsDepthOnlyPipeline->setVertexInputLayout(layout);
            m_currentMaskPointsDepthOnlyPipeline->setShaderResourceBindings(m_srb.get());
            m_currentMaskPointsDepthOnlyPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_currentMaskPointsDepthOnlyPipeline->create()) {
                qWarning("Failed to create current-mask points depth-only pipeline");
                m_currentMaskPointsDepthOnlyPipeline.reset();
            }
        }
    }

    if (!m_maskMorphToBasePipeline && m_maskMorphMaskToBaseSrb && m_currentMaskBaseRp) {
        m_maskMorphToBasePipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_outline.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask_morph.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load mask-morph shaders (to base)");
            m_maskMorphToBasePipeline.reset();
        } else {
            m_maskMorphToBasePipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_maskMorphToBasePipeline->setDepthTest(false);
            m_maskMorphToBasePipeline->setDepthWrite(false);
            m_maskMorphToBasePipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            m_maskMorphToBasePipeline->setVertexInputLayout(layout);
            m_maskMorphToBasePipeline->setShaderResourceBindings(m_maskMorphMaskToBaseSrb.get());
            m_maskMorphToBasePipeline->setRenderPassDescriptor(m_currentMaskBaseRp.get());
            if (!m_maskMorphToBasePipeline->create()) {
                qWarning("Failed to create mask-morph pipeline (to base)");
                m_maskMorphToBasePipeline.reset();
            }
        }
    }

    if (!m_maskMorphToWorkPipeline && m_maskMorphMaskToWorkSrb && m_currentMaskWorkRp) {
        m_maskMorphToWorkPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_outline.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask_morph.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load mask-morph shaders (to work)");
            m_maskMorphToWorkPipeline.reset();
        } else {
            m_maskMorphToWorkPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_maskMorphToWorkPipeline->setDepthTest(false);
            m_maskMorphToWorkPipeline->setDepthWrite(false);
            m_maskMorphToWorkPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            m_maskMorphToWorkPipeline->setVertexInputLayout(layout);
            m_maskMorphToWorkPipeline->setShaderResourceBindings(m_maskMorphMaskToWorkSrb.get());
            m_maskMorphToWorkPipeline->setRenderPassDescriptor(m_currentMaskWorkRp.get());
            if (!m_maskMorphToWorkPipeline->create()) {
                qWarning("Failed to create mask-morph pipeline (to work)");
                m_maskMorphToWorkPipeline.reset();
            }
        }
    }

    if (!m_maskMorphWorkToMaskPipeline && m_maskMorphWorkToMaskSrb && m_currentMaskRp) {
        m_maskMorphWorkToMaskPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_outline.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask_morph.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load mask-morph shaders (to mask)");
            m_maskMorphWorkToMaskPipeline.reset();
        } else {
            m_maskMorphWorkToMaskPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_maskMorphWorkToMaskPipeline->setDepthTest(false);
            m_maskMorphWorkToMaskPipeline->setDepthWrite(false);
            m_maskMorphWorkToMaskPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            m_maskMorphWorkToMaskPipeline->setVertexInputLayout(layout);
            m_maskMorphWorkToMaskPipeline->setShaderResourceBindings(m_maskMorphWorkToMaskSrb.get());
            m_maskMorphWorkToMaskPipeline->setRenderPassDescriptor(m_currentMaskRp.get());
            if (!m_maskMorphWorkToMaskPipeline->create()) {
                qWarning("Failed to create mask-morph pipeline (to mask)");
                m_maskMorphWorkToMaskPipeline.reset();
            }
        }
    }

    if (!m_maskDebugPipeline && m_maskDebugBaseSrb) {
        m_maskDebugPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_outline.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_mask_debug.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load mask-debug shaders");
            m_maskDebugPipeline.reset();
        } else {
            m_maskDebugPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_maskDebugPipeline->setDepthTest(false);
            m_maskDebugPipeline->setDepthWrite(false);
            m_maskDebugPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiVertexInputLayout layout;
            m_maskDebugPipeline->setVertexInputLayout(layout);
            m_maskDebugPipeline->setShaderResourceBindings(m_maskDebugBaseSrb.get());
            m_maskDebugPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_maskDebugPipeline->create()) {
                qWarning("Failed to create mask-debug pipeline");
                m_maskDebugPipeline.reset();
            }
        }
    }

    if (!m_outlinePipeline && m_outlineSrb) {
        m_outlinePipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/selection_outline.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/selection_outline.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load outline shaders");
            m_outlinePipeline.reset();
        } else {
            m_outlinePipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_outlinePipeline->setDepthTest(false);
            m_outlinePipeline->setDepthWrite(false);
            m_outlinePipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_outlinePipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            m_outlinePipeline->setVertexInputLayout(layout);
            m_outlinePipeline->setShaderResourceBindings(m_outlineSrb.get());
            m_outlinePipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_outlinePipeline->create()) {
                qWarning("Failed to create outline pipeline");
                m_outlinePipeline.reset();
            }
        }
    }

    if (!m_trackballGizmoUbuf) {
        m_trackballGizmoUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kTrackballGizmoUbufSize));
        if (!m_trackballGizmoUbuf || !m_trackballGizmoUbuf->create())
            m_trackballGizmoUbuf.reset();
    }

    if (!m_trackballGizmoVbuf) {
        const auto &verts = trackballGizmoVertices();
        m_trackballGizmoVbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(verts.size() * sizeof(float))));
        if (!m_trackballGizmoVbuf || !m_trackballGizmoVbuf->create()) {
            m_trackballGizmoVbuf.reset();
            m_trackballGizmoVertexCount = 0;
        } else {
            m_trackballGizmoVertexCount = int(verts.size() / 6);
        }
    }

    if (!m_trackballGizmoSrb && m_trackballGizmoUbuf) {
        m_trackballGizmoSrb.reset(m_rhi->newShaderResourceBindings());
        m_trackballGizmoSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage,
                m_trackballGizmoUbuf.get())
        });
        if (!m_trackballGizmoSrb->create())
            m_trackballGizmoSrb.reset();
    }

    if (!m_trackballGizmoPipeline && m_trackballGizmoSrb) {
        m_trackballGizmoPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_trackball_gizmo.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_trackball_gizmo.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load trackball gizmo shaders");
            m_trackballGizmoPipeline.reset();
        } else {
            m_trackballGizmoPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_trackballGizmoPipeline->setTopology(QRhiGraphicsPipeline::Lines);
            m_trackballGizmoPipeline->setDepthTest(true);
            m_trackballGizmoPipeline->setDepthWrite(true);
            m_trackballGizmoPipeline->setDepthOp(QRhiGraphicsPipeline::LessOrEqual);
            m_trackballGizmoPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_trackballGizmoPipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 6 * sizeof(float) } });
            layout.setAttributes({
                { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
                { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) }
            });
            m_trackballGizmoPipeline->setVertexInputLayout(layout);
            m_trackballGizmoPipeline->setShaderResourceBindings(m_trackballGizmoSrb.get());
            m_trackballGizmoPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_trackballGizmoPipeline->create()) {
                qWarning("Failed to create trackball gizmo pipeline");
                m_trackballGizmoPipeline.reset();
            }
        }
    }

    // Light gizmo resources (always created; drawn only during light drag)
    if (!m_lightGizmoUbuf) {
        m_lightGizmoUbuf.reset(
            m_rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kLightGizmoUbufSize));
        if (!m_lightGizmoUbuf || !m_lightGizmoUbuf->create())
            m_lightGizmoUbuf.reset();
    }

    if (!m_lightGizmoVbuf) {
        const auto &verts = lightGizmoVertices();
        m_lightGizmoVbuf.reset(
            m_rhi->newBuffer(
                QRhiBuffer::Dynamic,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(verts.size() * sizeof(float))));
        if (!m_lightGizmoVbuf || !m_lightGizmoVbuf->create()) {
            m_lightGizmoVbuf.reset();
            m_lightGizmoVertexCount = 0;
        } else {
            m_lightGizmoVertexCount = int(verts.size() / 6);
        }
    }

    if (!m_lightGizmoSrb && m_lightGizmoUbuf) {
        m_lightGizmoSrb.reset(m_rhi->newShaderResourceBindings());
        m_lightGizmoSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0,
                QRhiShaderResourceBinding::VertexStage,
                m_lightGizmoUbuf.get())
        });
        if (!m_lightGizmoSrb->create())
            m_lightGizmoSrb.reset();
    }

    if (!m_lightGizmoPipeline && m_lightGizmoSrb) {
        m_lightGizmoPipeline.reset(m_rhi->newGraphicsPipeline());
        QShader vs = loadShader(QStringLiteral(":/shaders/overlay_light_gizmo.vert.qsb"));
        QShader fs = loadShader(QStringLiteral(":/shaders/overlay_light_gizmo.frag.qsb"));
        if (!vs.isValid() || !fs.isValid()) {
            qWarning("Failed to load light gizmo shaders");
            m_lightGizmoPipeline.reset();
        } else {
            m_lightGizmoPipeline->setShaderStages({
                { QRhiShaderStage::Vertex, vs },
                { QRhiShaderStage::Fragment, fs }
            });
            m_lightGizmoPipeline->setTopology(QRhiGraphicsPipeline::Lines);
            m_lightGizmoPipeline->setDepthTest(false);
            m_lightGizmoPipeline->setDepthWrite(false);
            m_lightGizmoPipeline->setCullMode(QRhiGraphicsPipeline::None);
            QRhiGraphicsPipeline::TargetBlend blend;
            blend.enable = true;
            blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
            blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opColor = QRhiGraphicsPipeline::Add;
            blend.srcAlpha = QRhiGraphicsPipeline::One;
            blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
            blend.opAlpha = QRhiGraphicsPipeline::Add;
            m_lightGizmoPipeline->setTargetBlends({ blend });
            QRhiVertexInputLayout layout;
            layout.setBindings({ { 6 * sizeof(float) } });
            layout.setAttributes({
                { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
                { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) }
            });
            m_lightGizmoPipeline->setVertexInputLayout(layout);
            m_lightGizmoPipeline->setShaderResourceBindings(m_lightGizmoSrb.get());
            m_lightGizmoPipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
            if (!m_lightGizmoPipeline->create()) {
                qWarning("Failed to create light gizmo pipeline");
                m_lightGizmoPipeline.reset();
            }
        }
    }
}
