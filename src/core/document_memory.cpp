#include "document_internal.h"
#include "documentundomanager.h"

#include <QImage>

#include <set>

using namespace DocumentInternal;

namespace {

bool accountImage(const QImage &image, std::set<const uchar *> &seen, qint64 &bytes)
{
    const uchar *storage = image.isNull() ? nullptr : image.constBits();
    if (!storage || !seen.insert(storage).second)
        return false;
    bytes += qint64(image.sizeInBytes());
    return true;
}

} // namespace

std::vector<Document::CpuMeshMemoryStats> Document::cpuMeshMemoryStats() const
{
    std::vector<CpuMeshMemoryStats> result;
    result.reserve(m_meshes.size());
    for (int i = 0; i < meshCount(); ++i) {
        const MeshEntry &entry = mesh(i);
        CpuMeshMemoryStats s;
        s.meshId = entry.meshId;
        s.meshIndex = i;
        s.name = entry.name;
        s.vertexCapacity = static_cast<int>(entry.mesh.vert.capacity());
        s.edgeCapacity   = static_cast<int>(entry.mesh.edge.capacity());
        s.faceCapacity   = static_cast<int>(entry.mesh.face.capacity());
        s.vertexBytes    = qint64(entry.mesh.vert.capacity()) * sizeof(VCGVertex);
        s.vertexOcfBytes = vcgVertexOcfBytes(entry.mesh);
        s.edgeBytes      = qint64(entry.mesh.edge.capacity()) * sizeof(VCGEdge);
        s.faceBytes      = qint64(entry.mesh.face.capacity()) * sizeof(VCGFace);
        s.faceOcfBytes   = vcgFaceOcfBytes(entry.mesh);
        s.customAttributeBytes = vcgCustomAttributeBytes(entry.mesh);
        for (const auto &[ownerKey, data] : entry.pluginData) {
            if (data)
                s.pluginDataBytes += qint64(data->approximateBytes());
        }
        result.push_back(s);
    }
    return result;
}

Document::CpuImageMemoryStats Document::cpuImageMemoryStats() const
{
    CpuImageMemoryStats stats;
    std::set<const uchar *> seen;

    for (const auto &entry : m_meshes) {
        for (const MeshIOTextureAsset &asset : entry->textureAssets) {
            if (accountImage(asset.image, seen, stats.meshTextureBytes))
                ++stats.uniqueMeshTextureImages;
        }
        for (const MeshIOTextureAsset &asset : entry->materialSet.textureAssets) {
            if (accountImage(asset.image, seen, stats.meshTextureBytes))
                ++stats.uniqueMeshTextureImages;
        }
    }

    for (const auto &entry : m_rasters) {
        for (const RasterPlane &plane : entry->planes) {
            if (accountImage(plane.image, seen, stats.rasterImageBytes))
                ++stats.uniqueRasterImages;
        }
    }
    return stats;
}

UndoMemoryStats Document::undoMemoryStats() const
{
    return m_undoManager->memoryStats();
}


