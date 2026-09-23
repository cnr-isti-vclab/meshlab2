#include "renderwidget.h"
#include "linerenderer.h"
#include "clipplane.h"
#include "viewfrustumgizmo.h"
#include "document.h"
#include <algorithm>
#include <utility>

using namespace RenderWidgetInternal;

namespace {

bool requestsSelectionPass(const PerMeshRenderSettings &settings)
{
    return settings.showSelection
        && (settings.showSelectionVertices || settings.showSelectionFaces
            || settings.showSelectionEdges);
}

bool requestsDecoratorNormalPass(const PerMeshRenderSettings &settings)
{
    // Same contract as the boundary pass: the button gates, the checkboxes choose.
    return settings.decoratorNormals
        && (settings.decoratorVertexNormals
            || settings.decoratorFaceNormals
            || settings.decoratorCurvatureDir);
}

bool requestsDecoratorBoundaryPass(const PerMeshRenderSettings &settings)
{
    // The toolbar button gates the whole pass; the four checkboxes only say what it
    // draws. Nothing here writes back to them, so the panel keeps its state while off.
    return settings.decoratorBoundary
        && (settings.decoratorBoundaryEdges
            || settings.decoratorTextureSeams
            || settings.decoratorNonManifoldEdges
            || settings.decoratorNonManifoldVertices);
}

} // namespace

RenderWidget::RenderFramePassRequests RenderWidget::collectRenderFramePassRequests(
    int onlyMeshIndex) const
{
    RenderFramePassRequests requests;
    if (!m_doc)
        return requests;

    // A grid tile belongs to one mesh layer. Rasters are not mesh layers and have no tile of
    // their own, so a tile skips them -- drawing them would mean drawing them into every
    // tile at once.
    if (onlyMeshIndex < 0) {
        requests.rasterBackplates.reserve(m_doc->rasterCount());
        requests.rasterProjected.reserve(m_doc->rasterCount());
        if (m_viewMode == ViewMode::RasterImage) {
            const int currentRasterIndex = m_doc->currentRasterIndex();
            if (currentRasterIndex >= 0 && currentRasterIndex < m_doc->rasterCount()) {
                Document::RasterEntry &entry = m_doc->raster(currentRasterIndex);
                RasterPlane *plane = entry.currentPlane();
                if (plane) {
                    Document::ensureRasterPlaneImage(*plane);
                    if (!plane->image.isNull())
                        requests.rasterBackplates.push_back(currentRasterIndex);
                }
                if (!entry.shot.isValid())
                    return requests;
            } else {
                return requests;
            }
        } else {
            for (int ri = 0; ri < m_doc->rasterCount(); ++ri) {
                Document::RasterEntry &entry = m_doc->raster(ri);
                RasterPlane *plane = entry.currentPlane();
                if (!entry.visible || !plane)
                    continue;
                Document::ensureRasterPlaneImage(*plane);
                if (plane->image.isNull())
                    continue;
                if (entry.shot.isValid())
                    requests.rasterProjected.push_back(ri);
            }
        }
    }

    requests.meshes.reserve(m_doc->meshCount());
    for (int mi = 0; mi < m_doc->meshCount(); ++mi) {
        // A grid tile carries exactly one layer; the overlay tile passes -1 and takes all.
        if (onlyMeshIndex >= 0 && mi != onlyMeshIndex)
            continue;
        const bool visible = meshVisible(mi);
        const bool highlighted =
            m_renderSettings.highlightCurrentMesh && mi == m_doc->currentMeshIndex();
        if (!visible && !highlighted)
            continue;
        const PerMeshRenderSettings meshSettings = renderModeForMesh(mi);
        RenderMeshPassRequests meshRequests;
        meshRequests.meshIndex = mi;
        meshRequests.meshSettings = meshSettings;
        meshRequests.fill = visible && meshSettings.showFill;
        meshRequests.wire = visible && meshSettings.showWire;
        meshRequests.edges = visible && meshSettings.showEdges;
        meshRequests.boundingBox = visible && meshSettings.showBoundingBox;
        meshRequests.points = visible && meshSettings.showPoints;
        meshRequests.selection = visible && requestsSelectionPass(meshSettings);
        meshRequests.decoratorNormals = visible && requestsDecoratorNormalPass(meshSettings);
        meshRequests.decoratorBoundaries =
            visible && requestsDecoratorBoundaryPass(meshSettings);

        requests.fill = requests.fill || meshRequests.fill;
        requests.wire = requests.wire || meshRequests.wire;
        requests.edges = requests.edges || meshRequests.edges;
        requests.boundingBox = requests.boundingBox || meshRequests.boundingBox;
        requests.points = requests.points || meshRequests.points;
        requests.selection = requests.selection || meshRequests.selection;
        requests.decoratorNormals = requests.decoratorNormals || meshRequests.decoratorNormals;
        requests.decoratorBoundaries =
            requests.decoratorBoundaries || meshRequests.decoratorBoundaries;
        requests.meshes.push_back(std::move(meshRequests));
    }
    return requests;
}

void RenderWidget::planRasterBackplatePasses(
    const RenderWidget::RenderFramePassRequests &requests,
    RenderWidget::RenderFramePlan &plan)
{
    if (!m_doc || !requests.hasRasterBackplates())
        return;

    for (int rasterIndex : requests.rasterBackplates) {
        if (rasterIndex < 0 || rasterIndex >= m_doc->rasterCount())
            continue;
        const Document::RasterEntry &entry = m_doc->raster(rasterIndex);
        const auto it = m_rastersGpu.find(entry.rasterId);
        if (it == m_rastersGpu.end())
            continue;
        const RasterGpu &gpu = it->second;
        if (!gpu.texture || !gpu.backplateSrb || gpu.size.isEmpty())
            continue;
        plan.rasterBackplateItems.push_back(SceneRasterBackplateDrawItem {
            rasterIndex,
            gpu.size,
            gpu.backplateSrb.get(),
            true  // always preserve image aspect ratio
        });
    }
}

void RenderWidget::planRasterProjectedPasses(
    const RenderWidget::RenderFramePassRequests &requests,
    RenderWidget::RenderFramePlan &plan)
{
    if (!m_doc || !requests.hasRasterProjected())
        return;

    for (int rasterIndex : requests.rasterProjected) {
        if (rasterIndex < 0 || rasterIndex >= m_doc->rasterCount())
            continue;
        const Document::RasterEntry &entry = m_doc->raster(rasterIndex);
        const auto it = m_rastersGpu.find(entry.rasterId);
        if (it == m_rastersGpu.end())
            continue;
        const RasterGpu &gpu = it->second;
        if (!gpu.projectedSrb || !gpu.projectedVbuf || gpu.projectedVertexCount <= 0)
            continue;
        plan.rasterProjectedItems.push_back(SceneRasterProjectedDrawItem {
            rasterIndex,
            gpu.projectedSrb.get(),
            gpu.projectedVbuf.get(),
            gpu.projectedVertexCount,
            m_doc->currentRasterIndex() == rasterIndex
        });
    }
}

void RenderWidget::updateFrameClipPlane()
{
    m_frameClipPlane = QVector4D();
    if (!m_renderSettings.clipPlaneEnabled || !m_doc)
        return;
    // Scene3D only: the cut is a statement about the 3D scene, and neither the UV layout
    // nor a photograph has a place to stand in it.
    if (m_viewMode != ViewMode::Scene3D)
        return;

    QVector3D sceneMin;
    QVector3D sceneMax;
    if (!computeWorldSceneBBox(sceneMin, sceneMax))
        return;

    m_frameClipPlane = ClipPlane::world(
        m_renderSettings, sceneMin, sceneMax, m_trackball.cameraViewDirection());
}

QVector4D RenderWidget::localClipPlaneFor(int meshIndex) const
{
    if (m_frameClipPlane.isNull() || !m_doc
        || meshIndex < 0 || meshIndex >= m_doc->meshCount()) {
        return QVector4D();
    }
    return ClipPlane::toLocal(m_frameClipPlane, m_doc->mesh(meshIndex).transform);
}

void RenderWidget::planClipPlanePass(RenderWidget::RenderFramePlan &plan)
{
    m_clipPlaneGizmoVertices.clear();
    if (m_frameClipPlane.isNull() || !m_renderSettings.clipPlaneShowPlane || !m_rhi || !m_doc)
        return;

    QVector3D sceneMin;
    QVector3D sceneMax;
    if (!computeWorldSceneBBox(sceneMin, sceneMax))
        return;

    m_clipPlaneGizmoVertices = ClipPlane::planeGizmo(m_frameClipPlane, sceneMin, sceneMax);
    if (m_clipPlaneGizmoVertices.empty())
        return;

    const quint32 vbufSize = quint32(m_clipPlaneGizmoVertices.size() * sizeof(float));
    if (!m_clipPlaneGizmoVbuf || m_clipPlaneGizmoVbuf->size() < vbufSize) {
        m_clipPlaneGizmoVbuf.reset(m_rhi->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, vbufSize));
        m_clipPlaneGizmoVbuf->create();
    }
    if (!m_clipPlaneGizmoUbuf) {
        m_clipPlaneGizmoUbuf.reset(m_rhi->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kRasterProjectedUbufSize));
        m_clipPlaneGizmoUbuf->create();
    }
    if (!m_clipPlaneGizmoSrb) {
        QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
        srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage
                    | QRhiShaderResourceBinding::FragmentStage,
                m_clipPlaneGizmoUbuf.get()),
        });
        srb->create();
        m_clipPlaneGizmoSrb.reset(srb);
    }

    plan.rasterProjectedItems.push_back(SceneRasterProjectedDrawItem {
        kClipPlaneGizmoRasterIndex,
        m_clipPlaneGizmoSrb.get(),
        m_clipPlaneGizmoVbuf.get(),
        int(m_clipPlaneGizmoVertices.size() / 3),
        false
    });
}

void RenderWidget::rebuildViewFrustumGizmos()
{
    m_viewFrustumVertices.clear();
    m_viewFrustumCount = 0;
    m_viewFrustumBoundsValid = false;

    if (!m_peerViewCameraProvider || !m_renderSettings.showViewCameras
        || !m_doc || !m_rhi)
        return;

    auto shots = m_peerViewCameraProvider();
    if (shots.empty()) return;

    for (const PeerViewCamera &pvc : shots) {
        if (pvc.viewportSize.width() <= 0 || pvc.viewportSize.height() <= 0)
            continue;

        const ViewFrustumGizmo gizmo =
            buildViewFrustumGizmo(pvc.view, pvc.proj, pvc.nearDist, pvc.farDist);
        if (!gizmo.valid)
            continue;

        m_viewFrustumVertices.insert(m_viewFrustumVertices.end(),
                                     gizmo.vertices.begin(), gizmo.vertices.end());
        m_viewFrustumCount += gizmo.segmentCount;

        // Remembered so the frame can widen its own depth range to hold the gizmo.
        // Without this the gizmo is rasterised inside a clip volume sized for the meshes
        // and the far end of a peer frustum, which reaches well past them, is cut off.
        if (!m_viewFrustumBoundsValid) {
            m_viewFrustumBoundsMin = gizmo.boundsMin;
            m_viewFrustumBoundsMax = gizmo.boundsMax;
            m_viewFrustumBoundsValid = true;
        } else {
            for (int axis = 0; axis < 3; ++axis) {
                m_viewFrustumBoundsMin[axis] =
                    std::min(m_viewFrustumBoundsMin[axis], gizmo.boundsMin[axis]);
                m_viewFrustumBoundsMax[axis] =
                    std::max(m_viewFrustumBoundsMax[axis], gizmo.boundsMax[axis]);
            }
        }
    }
}

float RenderWidget::viewFrustumFarDistance(const QMatrix4x4 &view) const
{
    if (!m_viewFrustumBoundsValid)
        return 0.0f;
    return farthestDistanceInView(view, m_viewFrustumBoundsMin, m_viewFrustumBoundsMax);
}

void RenderWidget::planViewFrustumPasses(
    RenderWidget::RenderFramePlan &plan)
{
    if (m_viewFrustumCount == 0 || !m_rhi) return;

    // Ensure GPU buffers
    const quint32 vbufSize = quint32(m_viewFrustumVertices.size() * sizeof(float));
    if (!m_viewFrustumVbuf || m_viewFrustumVbuf->size() < vbufSize) {
        m_viewFrustumVbuf.reset(m_rhi->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, vbufSize));
        m_viewFrustumVbuf->create();
    }

    // Ensure uniform buffer
    constexpr quint32 ubufSize = kRasterProjectedUbufSize; // mat4 mvp + vec4 color
    if (!m_viewFrustumUbuf) {
        m_viewFrustumUbuf.reset(m_rhi->newBuffer(
            QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, ubufSize));
        m_viewFrustumUbuf->create();
    }

    // Ensure SRB
    if (!m_viewFrustumSrb) {
        QRhiShaderResourceBindings *srb = m_rhi->newShaderResourceBindings();
        srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage
                    | QRhiShaderResourceBinding::FragmentStage,
                m_viewFrustumUbuf.get()),
        });
        srb->create();
        m_viewFrustumSrb.reset(srb);
    }

    // Add a single draw item for all view camera frustums
    plan.rasterProjectedItems.push_back(SceneRasterProjectedDrawItem {
        -1,  // negative raster index = view frustums
        m_viewFrustumSrb.get(),
        m_viewFrustumVbuf.get(),
        int(m_viewFrustumCount * 2), // 2 vertices per line
        false
    });
}

void RenderWidget::planSimpleBufferPasses(
    const RenderWidget::RenderFramePassRequests &requests,
    RenderWidget::RenderFramePlan &plan)
{
    auto appendBufferDrawItem =
        [](std::vector<SceneBufferDrawItem> &items,
           int meshIndex,
           QRhiGraphicsPipeline *pipeline,
           const PerMeshRenderSettings &meshSettings,
           QRhiBuffer *vertexBuffer,
           int vertexCount,
           int firstVertex = 0) {
            if (!pipeline || !vertexBuffer || vertexCount <= 0)
                return;
            items.push_back(SceneBufferDrawItem {
                meshIndex,
                pipeline,
                meshSettings,
                vertexBuffer,
                vertexCount,
                firstVertex
            });
        };

    const bool buildSimpleBufferItems =
        requests.hasSimpleBufferRequests()
        && (requests.wire || requests.edges || (requests.boundingBox && m_bboxPipeline)
            || (requests.points && m_pointsPipeline));
    if (!buildSimpleBufferItems)
        return;

    for (const RenderMeshPassRequests &meshRequest : requests.meshes) {
        const int mi = meshRequest.meshIndex;
        const PerMeshRenderSettings &meshSettings = meshRequest.meshSettings;

        if (meshRequest.wire) {
            const MeshGpuResourceCache::WirePassView wireView =
                m_doc->wirePassGpuView(m_rhi, mi);
            if (wireView.valid) {
                appendBufferDrawItem(
                    plan.wireItems,
                    mi,
                    wirePipelineForSettings(meshSettings),
                    meshSettings,
                    wireView.vertexBuffer,
                    wireView.vertexCount);
            }
        }

        if (meshRequest.edges) {
            bool edgeItemAppended = false;
            const MeshGpuResourceCache::EdgeFatPassView fatView =
                m_doc->edgeFatPassGpuView(m_rhi, mi);
            if (fatView.valid) {
                const size_t before = plan.edgeItems.size();
                appendBufferDrawItem(
                    plan.edgeItems,
                    mi,
                    fatEdgesPipelineForSettings(meshSettings),
                    meshSettings,
                    fatView.vertexBuffer,
                    fatView.vertexCount);
                edgeItemAppended = (plan.edgeItems.size() != before);
            }

            if (!edgeItemAppended) {
                const MeshGpuResourceCache::EdgePassView lineView =
                    m_doc->edgePassGpuView(m_rhi, mi);
                if (lineView.valid) {
                    appendBufferDrawItem(
                        plan.edgeItems,
                        mi,
                        edgesPipelineForSettings(meshSettings),
                        meshSettings,
                        lineView.vertexBuffer,
                        lineView.vertexCount);
                }
            }
        }

        if (meshRequest.boundingBox && m_bboxPipeline) {
            const MeshGpuResourceCache::BBoxPassView bboxView =
                m_doc->bboxPassGpuView(m_rhi, mi);
            if (bboxView.valid) {
                // The buffer carries the box edges then the corner brackets; the style
                // chooses which half to draw.
                const bool brackets =
                    meshSettings.boundingBoxStyle == BoundingBoxStyle::CornerBrackets;
                appendBufferDrawItem(
                    plan.boundingBoxItems,
                    mi,
                    m_bboxPipeline.get(),
                    meshSettings,
                    bboxView.vertexBuffer,
                    brackets ? LineRenderer::kBoundingBoxBracketVertexCount
                             : LineRenderer::kBoundingBoxEdgeVertexCount,
                    brackets ? LineRenderer::kBoundingBoxEdgeVertexCount : 0);
            }
        }

        if (meshRequest.points && m_pointsPipeline) {
            const auto pointVariant = static_cast<Document::PointGpuVariant>(
                pointGpuVariantIndexForSettings(meshSettings));
            const MeshGpuResourceCache::PointsPassView pointsView =
                m_doc->pointsPassGpuView(m_rhi, mi, pointVariant);
            if (pointsView.valid) {
                appendBufferDrawItem(
                    plan.pointItems,
                    mi,
                    m_pointsPipeline.get(),
                    meshSettings,
                    pointsView.vertexBuffer,
                    pointsView.vertexCount);
            }
        }
    }
}

void RenderWidget::planDecoratorPasses(
    const RenderWidget::RenderFramePassRequests &requests,
    RenderWidget::RenderFramePlan &plan)
{
    if (!requests.decorators())
        return;

    auto appendDecoratorDrawItem =
        [](std::vector<SceneDecoratorDrawItem> &items,
           int meshIndex,
           int slot,
           SceneDecoratorDrawKind kind,
           const QColor &color,
           float width,
           QRhiBuffer *vertexBuffer,
           int vertexCount) {
            if (!vertexBuffer || vertexCount <= 0)
                return;
            items.push_back(SceneDecoratorDrawItem {
                meshIndex,
                slot,
                kind,
                color,
                width,
                vertexBuffer,
                vertexCount
            });
        };

    auto canDrawLineDecoratorSlot = [&](int slot) {
        return m_decoratorPipeline
            && slot >= 0
            && slot < kDecoratorSlotCount
            && m_decoratorUbufs[slot]
            && m_decoratorSrbs[slot];
    };

    auto appendLineDecoratorItems =
        [&](int slot, auto shouldDraw, auto colorGetter, auto bufferGetter, auto countGetter) {
            if (!canDrawLineDecoratorSlot(slot))
                return;
            for (const RenderMeshPassRequests &meshRequest : requests.meshes) {
                const int mi = meshRequest.meshIndex;
                const PerMeshRenderSettings &meshSettings = meshRequest.meshSettings;
                if (!shouldDraw(meshSettings))
                    continue;

                const MeshGpuResourceCache::DecoratorPassView decoratorView =
                    m_doc->decoratorPassGpuView(m_rhi, mi);
                if (!decoratorView.valid)
                    continue;

                appendDecoratorDrawItem(
                    plan.decoratorItems,
                    mi,
                    slot,
                    SceneDecoratorDrawKind::Line,
                    colorGetter(meshSettings),
                    1.0f,
                    bufferGetter(decoratorView),
                    countGetter(decoratorView));
            }
        };

    auto appendFatOrLineDecoratorItems =
        [&](int slot,
            auto shouldDraw,
            auto colorGetter,
            auto fatBufferGetter,
            auto fatCountGetter,
            auto lineBufferGetter,
            auto lineCountGetter) {
            const bool canDrawFatDecorator =
                m_decoratorFatPipeline && m_decoratorFatUbuf && m_decoratorFatSrb;
            for (const RenderMeshPassRequests &meshRequest : requests.meshes) {
                const int mi = meshRequest.meshIndex;
                const PerMeshRenderSettings &meshSettings = meshRequest.meshSettings;
                if (!shouldDraw(meshSettings))
                    continue;

                const MeshGpuResourceCache::DecoratorPassView decoratorView =
                    m_doc->decoratorPassGpuView(m_rhi, mi);
                if (!decoratorView.valid)
                    continue;

                const QColor color = colorGetter(meshSettings);
                const float width = qMax(0.5f, meshSettings.decoratorBoundaryWidth);
                if (canDrawFatDecorator) {
                    const size_t before = plan.decoratorItems.size();
                    appendDecoratorDrawItem(
                        plan.decoratorItems,
                        mi,
                        slot,
                        SceneDecoratorDrawKind::FatLine,
                        color,
                        width,
                        fatBufferGetter(decoratorView),
                        fatCountGetter(decoratorView));
                    if (plan.decoratorItems.size() != before)
                        continue;
                }

                if (!canDrawLineDecoratorSlot(slot))
                    continue;
                appendDecoratorDrawItem(
                    plan.decoratorItems,
                    mi,
                    slot,
                    SceneDecoratorDrawKind::Line,
                    color,
                    width,
                    lineBufferGetter(decoratorView),
                    lineCountGetter(decoratorView));
            }
        };

    appendLineDecoratorItems(
        kDecoratorSlotVertexNormals,
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorVertexNormals;
        },
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorVertexNormalColor;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.vertexNormalsBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.vertexNormalsVertexCount;
        });
    appendLineDecoratorItems(
        kDecoratorSlotFaceNormals,
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorFaceNormals;
        },
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorFaceNormalColor;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.faceNormalsBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.faceNormalsVertexCount;
        });
    appendLineDecoratorItems(
        kDecoratorSlotCurvaturePD1,
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorCurvatureDir;
        },
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorCurvatureDirPD1Color;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.curvatureDirPD1Buffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.curvatureDirPD1VertexCount;
        });
    appendLineDecoratorItems(
        kDecoratorSlotCurvaturePD2,
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorCurvatureDir;
        },
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorCurvatureDirPD2Color;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.curvatureDirPD2Buffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.curvatureDirPD2VertexCount;
        });
    appendFatOrLineDecoratorItems(
        kDecoratorSlotBoundaryEdges,
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorBoundaryEdges;
        },
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorBoundaryEdgeColor;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.boundaryEdgesFatBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.boundaryEdgesFatVertexCount;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.boundaryEdgesBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.boundaryEdgesVertexCount;
        });
    appendFatOrLineDecoratorItems(
        kDecoratorSlotTextureSeams,
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorTextureSeams;
        },
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorTextureSeamColor;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.textureSeamsFatBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.textureSeamsFatVertexCount;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.textureSeamsBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.textureSeamsVertexCount;
        });
    appendFatOrLineDecoratorItems(
        kDecoratorSlotNonManifoldEdges,
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorNonManifoldEdges;
        },
        [](const PerMeshRenderSettings &settings) {
            return settings.decoratorNonManifoldEdgeColor;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.nonManifoldEdgesFatBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.nonManifoldEdgesFatVertexCount;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.nonManifoldEdgesBuffer;
        },
        [](const MeshGpuResourceCache::DecoratorPassView &view) {
            return view.nonManifoldEdgesVertexCount;
        });

    const int nonManifoldVertexSlot = kDecoratorSlotNonManifoldVertices;
    if (canDrawLineDecoratorSlot(nonManifoldVertexSlot) && m_decoratorPointPipeline) {
        for (const RenderMeshPassRequests &meshRequest : requests.meshes) {
            const int mi = meshRequest.meshIndex;
            const PerMeshRenderSettings &meshSettings = meshRequest.meshSettings;
            if (!meshSettings.decoratorNonManifoldVertices)
                continue;

            const MeshGpuResourceCache::DecoratorPassView decoratorView =
                m_doc->decoratorPassGpuView(m_rhi, mi);
            if (!decoratorView.valid)
                continue;

            appendDecoratorDrawItem(
                plan.decoratorItems,
                mi,
                nonManifoldVertexSlot,
                SceneDecoratorDrawKind::Point,
                meshSettings.decoratorNonManifoldVertexColor,
                1.0f,
                decoratorView.nonManifoldVerticesBuffer,
                decoratorView.nonManifoldVerticesVertexCount);
        }
    }
}

void RenderWidget::planSelectionPasses(
    const RenderWidget::RenderFramePassRequests &requests,
    RenderWidget::RenderFramePlan &plan)
{
    if (!requests.selection
        || !m_selectionUbuf
        || !m_selectionSrb
        || (!m_selectionFacesPipeline && !m_selectionVerticesPipeline
             && !m_selectionEdgesPipeline)) {
        return;
    }

    for (const RenderMeshPassRequests &meshRequest : requests.meshes) {
        if (!meshRequest.selection)
            continue;

        const int mi = meshRequest.meshIndex;
        const PerMeshRenderSettings &meshSettings = meshRequest.meshSettings;

        const MeshGpuResourceCache::SelectionPassView selectionView =
            m_doc->selectionPassGpuView(m_rhi, mi);
        if (!selectionView.valid)
            continue;

        const bool drawFaces =
            meshSettings.showSelectionFaces
            && m_selectionFacesPipeline
            && selectionView.selectedFacesBuffer
            && selectionView.selectedFacesVertexCount > 0;
        const bool drawVertices =
            meshSettings.showSelectionVertices
            && m_selectionVerticesPipeline
            && selectionView.selectedVerticesBuffer
            && selectionView.selectedVerticesVertexCount > 0;
        const bool drawEdges =
            meshSettings.showSelectionEdges
            && m_selectionEdgesPipeline
            && selectionView.selectedEdgesBuffer
            && selectionView.selectedEdgesVertexCount > 0;
        if (!drawFaces && !drawVertices && !drawEdges)
            continue;

        plan.selectionItems.push_back(SceneSelectionDrawItem {
            mi,
            drawFaces,
            drawVertices,
            drawEdges,
            selectionView
        });
    }
}

RenderWidget::RenderFramePlan RenderWidget::buildRenderFramePlan(
    const RenderWidget::RenderFrameRequest &request)
{
    RenderFramePlan plan;
    plan.viewMode = request.viewMode;
    plan.pixelSize = request.pixelSize;
    plan.viewportRect = request.viewportRect;
    plan.targetPixelSize = request.targetPixelSize;
    plan.proj = request.proj;
    plan.view = request.view;
    plan.lightDir = request.lightDir;
    plan.rasterOpacity = request.rasterOpacity;
    plan.rasterZoom = request.rasterZoom;
    plan.rasterPan = request.rasterPan;

    if (request.passes.fill)
        plan.sceneFill = buildSceneFillFramePlan(request);

    planRasterBackplatePasses(request.passes, plan);
    planRasterProjectedPasses(request.passes, plan);
    planViewFrustumPasses(plan);
    planClipPlanePass(plan);
    planSimpleBufferPasses(request.passes, plan);
    planDecoratorPasses(request.passes, plan);
    planSelectionPasses(request.passes, plan);

    return plan;
}
