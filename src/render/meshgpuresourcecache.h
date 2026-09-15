#pragma once

#include "meshioplugin.h"
#include "vcgmesh.h"
#include <cstdint>
#include <memory>
#include <QString>
#include <QStringList>

class QRhi;
class QRhiBuffer;
class QRhiTexture;
class QRhiCommandBuffer;

class MeshGpuResourceCache
{
public:
    enum class FillVariant : int {
        Constant = 0,
        PerVertex = 1,
        PerFace = 2,
        PerVertexQuality = 3,
        PerFaceQuality = 4,
        Texture = 5
    };

    enum class EdgeVariant : int {
        Constant = 0,
        PerEdge = 1
    };

    enum class PointVariant : int {
        Constant = 0,
        PerVertex = 1,
        PerVertexQuality = 2
    };

    struct MeshSource {
        std::uint64_t meshId = 0;
        std::uint64_t geometryRevision = 0;
        std::uint64_t materialRevision = 0;
        std::uint64_t selectionRevision = 0;
        int ioMask = 0;
        bool qualityFixedRange = false;
        float qualityRangeMin = 0.0f;
        float qualityRangeMax = 1.0f;
        bool qualityCenterOnZero = false;
        float qualityPercentileCrop = 0.0f;
        bool wireRespectFaux = true;
        const VCGMesh *mesh = nullptr;
        const QStringList *textureFilePaths = nullptr;
        const std::vector<MeshIOTextureAsset> *textureAssets = nullptr;
        const MeshIOMaterialSet *materialSet = nullptr;
    };

    struct FillBatchView {
        QRhiBuffer *vertexBuffer = nullptr;
        QRhiBuffer *indexBuffer = nullptr;
        QRhiTexture *baseColorTexture = nullptr;
        QRhiTexture *normalTexture = nullptr;
        QRhiTexture *occlusionTexture = nullptr;
        QRhiTexture *roughnessTexture = nullptr;
        QString baseColorTexturePath;
        QString normalTexturePath;
        QString occlusionTexturePath;
        QString roughnessTexturePath;
        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        float roughnessFactor = 1.0f;
        int vertexCount = 0;
        int indexCount = 0;
        int textureGroupIndex = -1;
    };

    struct FillPassView {
        const FillBatchView *batches = nullptr;
        int batchCount = 0;
        bool valid = false;
    };

    struct WirePassView {
        QRhiBuffer *vertexBuffer = nullptr;
        int vertexCount = 0;
        bool valid = false;
    };

    struct EdgePassView {
        QRhiBuffer *vertexBuffer = nullptr;
        int vertexCount = 0;
        bool valid = false;
    };

    struct EdgeFatPassView {
        QRhiBuffer *vertexBuffer = nullptr;
        int vertexCount = 0;
        bool valid = false;
    };

    struct PointsPassView {
        QRhiBuffer *vertexBuffer = nullptr;
        int vertexCount = 0;
        bool valid = false;
    };

    struct BBoxPassView {
        QRhiBuffer *vertexBuffer = nullptr;
        int vertexCount = 0;
        bool valid = false;
    };

    struct DecoratorPassView {
        QRhiBuffer *vertexNormalsBuffer = nullptr;
        int vertexNormalsVertexCount = 0;
        QRhiBuffer *faceNormalsBuffer = nullptr;
        int faceNormalsVertexCount = 0;
        QRhiBuffer *curvatureDirPD1Buffer = nullptr;
        int curvatureDirPD1VertexCount = 0;
        QRhiBuffer *curvatureDirPD2Buffer = nullptr;
        int curvatureDirPD2VertexCount = 0;
        QRhiBuffer *boundaryEdgesBuffer = nullptr;
        int boundaryEdgesVertexCount = 0;
        QRhiBuffer *boundaryEdgesFatBuffer = nullptr;
        int boundaryEdgesFatVertexCount = 0;
        QRhiBuffer *textureSeamsBuffer = nullptr;
        int textureSeamsVertexCount = 0;
        QRhiBuffer *textureSeamsFatBuffer = nullptr;
        int textureSeamsFatVertexCount = 0;
        QRhiBuffer *nonManifoldEdgesBuffer = nullptr;
        int nonManifoldEdgesVertexCount = 0;
        QRhiBuffer *nonManifoldEdgesFatBuffer = nullptr;
        int nonManifoldEdgesFatVertexCount = 0;
        QRhiBuffer *nonManifoldVerticesBuffer = nullptr;
        int nonManifoldVerticesVertexCount = 0;
        bool valid = false;
    };

    struct SelectionPassView {
        QRhiBuffer *selectedFacesBuffer = nullptr;
        int selectedFacesVertexCount = 0;
        QRhiBuffer *selectedVerticesBuffer = nullptr;
        int selectedVerticesVertexCount = 0;
        bool valid = false;
    };

    struct EnsureStats {
        bool rebuiltFill = false;
        bool rebuiltWire = false;
        bool rebuiltEdges = false;
        bool rebuiltPoints = false;
        bool rebuiltBoundingBox = false;
        bool rebuiltSelection = false;
        bool rebuiltDecoratorNormals = false;
        bool rebuiltDecoratorBoundaries = false;
        bool uploadedResources = false;
        float elapsedMs = 0.0f;

        bool anyRebuilt() const
        {
            return rebuiltFill || rebuiltWire || rebuiltEdges || rebuiltPoints || rebuiltBoundingBox
                || rebuiltSelection || rebuiltDecoratorNormals || rebuiltDecoratorBoundaries;
        }
    };

    struct GpuMeshMemoryStats {
        std::uint64_t meshId = 0;
        qint64 fillBufferBytes = 0;
        qint64 textureBytes = 0;
        qint64 wireBufferBytes = 0;
        qint64 edgeBufferBytes = 0;
        qint64 pointsBufferBytes = 0;
        qint64 bboxBufferBytes = 0;
        qint64 selectionBufferBytes = 0;
        qint64 decoratorBufferBytes = 0;
        qint64 totalBytes() const
        {
            return fillBufferBytes + textureBytes + wireBufferBytes + edgeBufferBytes
                + pointsBufferBytes + bboxBufferBytes + selectionBufferBytes + decoratorBufferBytes;
        }
    };

    MeshGpuResourceCache();
    ~MeshGpuResourceCache();

    EnsureStats ensureMeshResources(QRhi *rhi,
                                    QRhiCommandBuffer *cb,
                                    const MeshSource &source,
                                    FillVariant fillVariant,
                                    PointVariant pointVariant,
                                    bool needFill,
                                    bool needWire,
                                    bool needEdges,
                                    bool needPoints,
                                    bool needBoundingBox,
                                    bool needSelection,
                                    bool needDecoratorNormals,
                                    bool needDecoratorBoundaries,
                                    EdgeVariant edgeVariant = EdgeVariant::Constant);

    FillPassView fillPassView(QRhi *rhi, std::uint64_t meshId, FillVariant variant) const;
    WirePassView wirePassView(QRhi *rhi, std::uint64_t meshId) const;
    EdgePassView edgePassView(QRhi *rhi, std::uint64_t meshId,
                                EdgeVariant variant = EdgeVariant::Constant) const;
    EdgeFatPassView edgeFatPassView(QRhi *rhi, std::uint64_t meshId,
                                EdgeVariant variant = EdgeVariant::Constant) const;
    PointsPassView pointsPassView(QRhi *rhi, std::uint64_t meshId, PointVariant variant) const;
    BBoxPassView bboxPassView(QRhi *rhi, std::uint64_t meshId) const;
    SelectionPassView selectionPassView(QRhi *rhi, std::uint64_t meshId) const;
    DecoratorPassView decoratorPassView(QRhi *rhi, std::uint64_t meshId) const;

    void purgeMesh(std::uint64_t meshId);
    void releaseRhiResources(QRhi *rhi);
    void clearAll();
    std::vector<GpuMeshMemoryStats> gpuMemoryStats() const;

private:
    struct CacheState;
    std::unique_ptr<CacheState> m_state;
};
