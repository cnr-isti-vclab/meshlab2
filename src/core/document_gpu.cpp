#include "document_internal.h"

using namespace DocumentInternal;

void Document::ensureMeshGpuResources(QRhi *rhi,
                                      QRhiCommandBuffer *cb,
                                      int meshIndex,
                                      FillGpuVariant fillVariant,
                                      PointGpuVariant pointVariant,
                                      bool needFill,
                                      bool needWire,
                                      bool needEdges,
                                      bool needPoints,
                                      bool needBoundingBox,
                                      bool needDecoratorNormals,
                                      bool needDecoratorBoundaries,
                                      bool qualityFixedRange,
                                      float qualityRangeMin,
                                      float qualityRangeMax,
                                      bool needSelection,
                                      bool wireRespectFaux,
                                      bool qualityCenterOnZero,
                                      float qualityPercentileCrop,
                                      EdgeGpuVariant edgeVariant)
{
    if (!m_gpuCache || !rhi || !cb)
        return;
    if (meshIndex < 0 || meshIndex >= meshCount())
        return;

    const MeshEntry &meshEntry = mesh(meshIndex);
    MeshGpuResourceCache::MeshSource source;
    source.meshId = meshEntry.meshId;
    source.geometryRevision = meshEntry.geometryRevision;
    source.materialRevision = meshEntry.materialRevision;
    source.selectionRevision = meshEntry.selectionRevision;
    source.ioMask = meshEntry.ioMask;
    source.qualityFixedRange = qualityFixedRange;
    source.qualityRangeMin = qualityRangeMin;
    source.qualityRangeMax = qualityRangeMax;
    if (source.qualityRangeMin > source.qualityRangeMax)
        std::swap(source.qualityRangeMin, source.qualityRangeMax);
    source.qualityCenterOnZero = qualityCenterOnZero;
    source.qualityPercentileCrop = qualityPercentileCrop;
    source.wireRespectFaux = wireRespectFaux;
    source.mesh = &meshEntry.mesh;
    source.textureFilePaths = &meshEntry.textureFilePaths;
    source.textureAssets = &meshEntry.textureAssets;
    source.materialSet = &meshEntry.materialSet;

    const MeshGpuResourceCache::EnsureStats stats = m_gpuCache->ensureMeshResources(
        rhi,
        cb,
        source,
        fillVariant,
        pointVariant,
        needFill,
        needWire,
        needEdges,
        needPoints,
        needBoundingBox,
        needSelection,
        needDecoratorNormals,
        needDecoratorBoundaries,
        edgeVariant);

    if (stats.anyRebuilt()) {
        QStringList rebuiltPasses;
        if (stats.rebuiltFill)
            rebuiltPasses << tr("fill");
        if (stats.rebuiltWire)
            rebuiltPasses << tr("wire");
        if (stats.rebuiltEdges)
            rebuiltPasses << tr("edges");
        if (stats.rebuiltPoints)
            rebuiltPasses << tr("points");
        if (stats.rebuiltBoundingBox)
            rebuiltPasses << tr("bbox");
        if (stats.rebuiltSelection)
            rebuiltPasses << tr("selection");
        if (stats.rebuiltDecoratorNormals)
            rebuiltPasses << tr("decorator normals");
        if (stats.rebuiltDecoratorBoundaries)
            rebuiltPasses << tr("decorator boundaries");
        writeLog(
            tr("GPU buffers built for '%1': %2 in %3 ms")
                .arg(meshEntry.name)
                .arg(rebuiltPasses.join(QStringLiteral(", ")))
                .arg(QString::number(stats.elapsedMs, 'f', 2)),
            LogSource::Application, LogLevel::Debug);
    }
}

Document::FillPassGpuView Document::fillPassGpuView(
    QRhi *rhi, int meshIndex, FillGpuVariant variant) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->fillPassView(rhi, meshEntry.meshId, variant);
}

Document::WirePassGpuView Document::wirePassGpuView(QRhi *rhi, int meshIndex) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->wirePassView(rhi, meshEntry.meshId);
}

Document::EdgePassGpuView Document::edgePassGpuView(QRhi *rhi, int meshIndex, EdgeGpuVariant variant) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->edgePassView(rhi, meshEntry.meshId, variant);
}

Document::EdgeFatPassGpuView Document::edgeFatPassGpuView(QRhi *rhi, int meshIndex, EdgeGpuVariant variant) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->edgeFatPassView(rhi, meshEntry.meshId, variant);
}

Document::PointsPassGpuView Document::pointsPassGpuView(
    QRhi *rhi, int meshIndex, PointGpuVariant variant) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->pointsPassView(rhi, meshEntry.meshId, variant);
}

Document::BBoxPassGpuView Document::bboxPassGpuView(QRhi *rhi, int meshIndex) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->bboxPassView(rhi, meshEntry.meshId);
}

Document::SelectionPassGpuView Document::selectionPassGpuView(QRhi *rhi, int meshIndex) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->selectionPassView(rhi, meshEntry.meshId);
}

Document::DecoratorPassGpuView Document::decoratorPassGpuView(
    QRhi *rhi, int meshIndex) const
{
    if (!m_gpuCache || !rhi || meshIndex < 0 || meshIndex >= meshCount())
        return {};

    const MeshEntry &meshEntry = mesh(meshIndex);
    return m_gpuCache->decoratorPassView(rhi, meshEntry.meshId);
}

void Document::releaseRhiGpuResources(QRhi *rhi)
{
    if (!m_gpuCache || !rhi)
        return;
    m_gpuCache->releaseRhiResources(rhi);
}

void Document::clearAllGpuResources()
{
    if (!m_gpuCache)
        return;
    m_gpuCache->clearAll();
}

void Document::purgeMeshGpuResources(std::uint64_t meshId)
{
    if (!m_gpuCache)
        return;
    m_gpuCache->purgeMesh(meshId);
}
