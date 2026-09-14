#include "renderwidget.h"
#include "linerenderer.h"
#include "document.h"
#include <utility>

using namespace RenderWidgetInternal;

namespace {

bool requestsSelectionPass(const PerMeshRenderSettings &settings)
{
    return settings.showSelection
        && (settings.showSelectionVertices || settings.showSelectionFaces);
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

void RenderWidget::planViewFrustumPasses(
    RenderWidget::RenderFramePlan &plan)
{
    if (!m_peerViewCameraProvider || !m_renderSettings.showViewCameras
        || !m_doc || !m_rhi)
        return;

    auto shots = m_peerViewCameraProvider();
    if (shots.empty()) return;

    // Build frustum vertices for each peer view camera
    m_viewFrustumVertices.clear();
    m_viewFrustumCount = 0;

    // Compute a reasonable frustum depth
    float frustumDepth = 0.0f;
    {
        bool hasBounds = false;
        QVector3D sceneMin, sceneMax;
        for (int mi = 0; mi < m_doc->meshCount(); ++mi) {
            if (!meshVisible(mi)) continue;
            const auto &m = m_doc->mesh(mi).mesh;
            if (m.bbox.IsNull()) continue;
            const auto &tf = m_doc->mesh(mi).transform;
            const QVector3D corners[8] = {
                tf.map(QVector3D(float(m.bbox.min[0]), float(m.bbox.min[1]), float(m.bbox.min[2]))),
                tf.map(QVector3D(float(m.bbox.max[0]), float(m.bbox.min[1]), float(m.bbox.min[2]))),
                tf.map(QVector3D(float(m.bbox.min[0]), float(m.bbox.max[1]), float(m.bbox.min[2]))),
                tf.map(QVector3D(float(m.bbox.max[0]), float(m.bbox.max[1]), float(m.bbox.min[2]))),
                tf.map(QVector3D(float(m.bbox.min[0]), float(m.bbox.min[1]), float(m.bbox.max[2]))),
                tf.map(QVector3D(float(m.bbox.max[0]), float(m.bbox.min[1]), float(m.bbox.max[2]))),
                tf.map(QVector3D(float(m.bbox.min[0]), float(m.bbox.max[1]), float(m.bbox.max[2]))),
                tf.map(QVector3D(float(m.bbox.max[0]), float(m.bbox.max[1]), float(m.bbox.max[2]))) };
            if (!hasBounds) { sceneMin = corners[0]; sceneMax = corners[0]; hasBounds = true; }
            for (int c = 0; c < 8; ++c) {
                sceneMin.setX(std::min(sceneMin.x(), corners[c].x()));
                sceneMin.setY(std::min(sceneMin.y(), corners[c].y()));
                sceneMin.setZ(std::min(sceneMin.z(), corners[c].z()));
                sceneMax.setX(std::max(sceneMax.x(), corners[c].x()));
                sceneMax.setY(std::max(sceneMax.y(), corners[c].y()));
                sceneMax.setZ(std::max(sceneMax.z(), corners[c].z()));
            }
        }
        if (hasBounds)
            frustumDepth = std::max(1e-3f, (sceneMax - sceneMin).length() * 0.12f);
    }
    if (frustumDepth <= 0.0f)
        frustumDepth = std::max(1e-3f, m_trackball.radius() * 0.25f);

    for (const PeerViewCamera &pvc : shots) {
        if (pvc.viewportSize.width() <= 0 || pvc.viewportSize.height() <= 0)
            continue;

        // Extract camera parameters from view/projection matrices
        QMatrix4x4 invView = pvc.view.inverted();
        QVector3D apex(invView(0, 3), invView(1, 3), invView(2, 3));
        // Camera looks down -Z in view space; column 2 of invView = camera Z axis in world
        QVector3D fwd(-invView(0, 2), -invView(1, 2), -invView(2, 2));
        QVector3D right(invView(0, 0), invView(1, 0), invView(2, 0));
        QVector3D up(invView(0, 1), invView(1, 1), invView(2, 1));

        // Derive FOV and aspect from the projection matrix: proj(1,1) = cot(fovY/2)
        float tanHalfFovY = 1.0f / pvc.proj(1, 1);
        float aspect = pvc.proj(1, 1) / pvc.proj(0, 0);

        auto append = [this](const QVector3D &a, const QVector3D &b) {
            m_viewFrustumVertices.insert(m_viewFrustumVertices.end(),
                {a.x(), a.y(), a.z(), b.x(), b.y(), b.z()});
            ++m_viewFrustumCount;
        };

        auto cornersAtDepth = [&](float d) {
            struct { QVector3D bl, br, tl, tr; } r;
            float halfH = tanHalfFovY * d;
            float halfW = halfH * aspect;
            QVector3D center = apex + fwd * d;
            r.bl = center - right * halfW - up * halfH;
            r.br = center + right * halfW - up * halfH;
            r.tl = center - right * halfW + up * halfH;
            r.tr = center + right * halfW + up * halfH;
            return r;
        };

        // Main frustum at reference depth
        {
            auto c = cornersAtDepth(frustumDepth);
            append(apex, c.bl); append(apex, c.br); append(apex, c.tl); append(apex, c.tr);
            append(c.bl, c.br); append(c.br, c.tr); append(c.tr, c.tl); append(c.tl, c.bl);
        }

        // Near plane rectangle
        if (pvc.nearDist > 0.0f) {
            auto c = cornersAtDepth(pvc.nearDist);
            append(c.bl, c.br); append(c.br, c.tr); append(c.tr, c.tl); append(c.tl, c.bl);
        }

        // Far plane rectangle
        float farD = pvc.farDist > 0.0f ? pvc.farDist
            : std::max(frustumDepth * 3.0f, m_trackball.radius() * 10.0f);
        {
            auto c = cornersAtDepth(farD);
            append(c.bl, c.br); append(c.br, c.tr); append(c.tr, c.tl); append(c.tl, c.bl);
        }
    }

    if (m_viewFrustumCount == 0) return;

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
        || (!m_selectionFacesPipeline && !m_selectionVerticesPipeline)) {
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
        if (!drawFaces && !drawVertices)
            continue;

        plan.selectionItems.push_back(SceneSelectionDrawItem {
            mi,
            drawFaces,
            drawVertices,
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
    planSimpleBufferPasses(request.passes, plan);
    planDecoratorPasses(request.passes, plan);
    planSelectionPasses(request.passes, plan);

    return plan;
}
