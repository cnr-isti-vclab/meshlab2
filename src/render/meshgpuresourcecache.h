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

    enum class PointVariant : int {
        Constant = 0,
        PerVertex = 1,
        PerVertexQuality = 2
    };

    static constexpr int kEdgeVertexStrideBytes = 20;
    static constexpr int kEdgeVertexColorOffsetBytes = 12;
    static constexpr int kEdgeColorOffsetBytes = 16;
    static constexpr int kFatEdgeVertexStrideBytes = 40;
    static constexpr int kFatEdgeVertexColorOffsetBytes = 32;
    static constexpr int kFatEdgeColorOffsetBytes = 36;

    struct MeshSource {
        std::uint64_t meshId = 0;
        std::uint64_t geometryRevision = 0;
        std::uint64_t materialRevision = 0;
        std::uint64_t selectionRevision = 0;
        std::uint64_t colorRevision = 0;
        std::uint64_t scalarRevision = 0;
        int ioMask = 0;
        bool wireRespectFaux = true;
        const VCGMesh *mesh = nullptr;
        const QStringList *textureFilePaths = nullptr;
        const std::vector<MeshIOTextureAsset> *textureAssets = nullptr;
        const MeshIOMaterialSet *materialSet = nullptr;
        // For a mesh laid out in UV space, where a vertex is split once per UV it has: the
        // source vertex each vertex stands for. The boundary and seam pass keys edges by it,
        // so the two sides of a seam meet, and draws such an edge on each of its sides.
        const std::vector<int> *vertexIdentity = nullptr;
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

    // A non-indexed vertex stream whose size grows with the mesh is split, like the fill,
    // into buffers of at most 256 MB: a QRhi buffer size is a quint32, and on a large mesh
    // one buffer wrapped and silently drew only part of it. Draw every batch in turn.
    struct BufferBatchView {
        QRhiBuffer *vertexBuffer = nullptr;
        int vertexCount = 0;
    };

    struct BufferBatches {
        const BufferBatchView *data = nullptr;
        int count = 0;
        const BufferBatchView *begin() const { return data; }
        const BufferBatchView *end() const { return data + count; }
        bool empty() const { return count == 0; }
        // Binds each batch as vertex input 0 and draws it, with whatever pipeline is set.
        void draw(QRhiCommandBuffer *cb) const;
    };

    struct WirePassView {
        BufferBatches batches;
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
        BufferBatches batches;
        bool valid = false;
    };

    struct BBoxPassView {
        QRhiBuffer *vertexBuffer = nullptr;
        int vertexCount = 0;
        bool valid = false;
    };

    struct DecoratorPassView {
        BufferBatches vertexNormals;
        BufferBatches faceNormals;
        BufferBatches curvatureDirPD1;
        BufferBatches curvatureDirPD2;
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
        BufferBatches selectedFaces;
        BufferBatches selectedVertices;
        QRhiBuffer *selectedEdgesBuffer = nullptr;
        int selectedEdgesVertexCount = 0;
        // The same edges as quads, in the fat-line layout of LineRenderer::buildFatLineVertices.
        QRhiBuffer *selectedEdgesFatBuffer = nullptr;
        int selectedEdgesFatVertexCount = 0;
        bool valid = false;
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
        qint64 decoratorNormalsBytes = 0;
        qint64 decoratorBoundariesBytes = 0;
        qint64 totalBytes() const
        {
            return fillBufferBytes + textureBytes + wireBufferBytes + edgeBufferBytes
                + pointsBufferBytes + bboxBufferBytes + selectionBufferBytes
                + decoratorNormalsBytes + decoratorBoundariesBytes;
        }
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
        float elapsedMs = 0.0f;
        // The mesh's buffers after the call, by product, for reporting what was built.
        GpuMeshMemoryStats bytes;

        bool anyRebuilt() const
        {
            return rebuiltFill || rebuiltWire || rebuiltEdges || rebuiltPoints || rebuiltBoundingBox
                || rebuiltSelection || rebuiltDecoratorNormals || rebuiltDecoratorBoundaries;
        }
    };

    MeshGpuResourceCache();
    ~MeshGpuResourceCache();

    // Which products a frame needs from one mesh.
    struct Needs {
        bool fill = false;
        bool wire = false;
        bool edges = false;
        bool points = false;
        bool boundingBox = false;
        bool selection = false;
        bool decoratorNormals = false;
        bool decoratorBoundaries = false;
    };

    EnsureStats ensureMeshResources(QRhi *rhi,
                                    QRhiCommandBuffer *cb,
                                    const MeshSource &source,
                                    FillVariant fillVariant,
                                    PointVariant pointVariant,
                                    const Needs &needs);

    FillPassView fillPassView(QRhi *rhi, std::uint64_t meshId, FillVariant variant) const;
    WirePassView wirePassView(QRhi *rhi, std::uint64_t meshId) const;
    EdgePassView edgePassView(QRhi *rhi, std::uint64_t meshId) const;
    EdgeFatPassView edgeFatPassView(QRhi *rhi, std::uint64_t meshId) const;
    PointsPassView pointsPassView(QRhi *rhi, std::uint64_t meshId, PointVariant variant) const;
    BBoxPassView bboxPassView(QRhi *rhi, std::uint64_t meshId) const;
    SelectionPassView selectionPassView(QRhi *rhi, std::uint64_t meshId) const;
    DecoratorPassView decoratorPassView(QRhi *rhi, std::uint64_t meshId) const;

    void purgeMesh(std::uint64_t meshId);
    void releaseRhiResources(QRhi *rhi);
    std::vector<GpuMeshMemoryStats> gpuMemoryStats() const;

private:
    struct CacheState;
    std::unique_ptr<CacheState> m_state;
};
