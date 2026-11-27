#include "meshgpuresourcecache.h"
#include "linerenderer.h"
#include "textureassociationutils.h"
#include "meshioplugin.h"
#include "parallelfor.h"

#include <vcg/complex/algorithms/update/selection.h>
#include <wrap/io_trimesh/io_mask.h>
#include <rhi/qrhi.h>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QString>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace {
struct EdgeVertexGpu {
    float position[3];
    std::uint8_t vertexColor[4];
    std::uint8_t edgeColor[4];
    float quality[4];
};

struct FatEdgeVertexGpu {
    float p0[3];
    float p1[3];
    float along;
    float side;
    std::uint8_t vertexColor[4];
    std::uint8_t edgeColor[4];
    float quality[4];
};

static_assert(sizeof(EdgeVertexGpu) == MeshGpuResourceCache::kEdgeVertexStrideBytes);
static_assert(
    offsetof(EdgeVertexGpu, vertexColor)
    == MeshGpuResourceCache::kEdgeVertexColorOffsetBytes);
static_assert(
    offsetof(EdgeVertexGpu, edgeColor)
    == MeshGpuResourceCache::kEdgeColorOffsetBytes);
static_assert(
    offsetof(EdgeVertexGpu, quality)
    == MeshGpuResourceCache::kEdgeQualityOffsetBytes);
static_assert(sizeof(FatEdgeVertexGpu) == MeshGpuResourceCache::kFatEdgeVertexStrideBytes);
static_assert(
    offsetof(FatEdgeVertexGpu, quality)
    == MeshGpuResourceCache::kFatEdgeQualityOffsetBytes);
static_assert(
    offsetof(FatEdgeVertexGpu, vertexColor)
    == MeshGpuResourceCache::kFatEdgeVertexColorOffsetBytes);
static_assert(
    offsetof(FatEdgeVertexGpu, edgeColor)
    == MeshGpuResourceCache::kFatEdgeColorOffsetBytes);

constexpr int kFillVertexStrideFloats = 13;
constexpr int kPointsVertexStrideFloats = 11;
constexpr size_t kMaxExpandedFillBatchBytes = 256u * 1024u * 1024u;
constexpr size_t kExpandedFillTriangleBytes =
    3u * size_t(kFillVertexStrideFloats) * sizeof(float);
constexpr size_t kMaxExpandedFillBatchFaces =
    kMaxExpandedFillBatchBytes / kExpandedFillTriangleBytes;
static_assert(kMaxExpandedFillBatchFaces > 0);
static_assert(kMaxExpandedFillBatchBytes <= std::numeric_limits<quint32>::max());

int fillVariantIndex(MeshGpuResourceCache::FillVariant variant)
{
    switch (variant) {
    case MeshGpuResourceCache::FillVariant::Constant: return 0;
    case MeshGpuResourceCache::FillVariant::PerVertex: return 1;
    case MeshGpuResourceCache::FillVariant::PerFace: return 2;
    case MeshGpuResourceCache::FillVariant::PerVertexQuality: return 3;
    case MeshGpuResourceCache::FillVariant::PerFaceQuality: return 4;
    case MeshGpuResourceCache::FillVariant::Texture: return 5;
    }
    return 0;
}

int pointVariantIndex(MeshGpuResourceCache::PointVariant variant)
{
    switch (variant) {
    case MeshGpuResourceCache::PointVariant::Constant: return 0;
    case MeshGpuResourceCache::PointVariant::PerVertex: return 1;
    case MeshGpuResourceCache::PointVariant::PerVertexQuality: return 2;
    }
    return 0;
}

bool isPerVertexQualityFillVariant(MeshGpuResourceCache::FillVariant variant)
{
    return variant == MeshGpuResourceCache::FillVariant::PerVertexQuality;
}

bool isPerFaceQualityFillVariant(MeshGpuResourceCache::FillVariant variant)
{
    return variant == MeshGpuResourceCache::FillVariant::PerFaceQuality;
}

// The four color slots of a quality vertex: the raw value with a LUT flag, -1 for a per-vertex
// value and -2 for a per-face one, which the shader maps through the matching half of the
// view's qualityMap uniform. A value that is not finite gets the plain-color flag (0)
// instead, so it shows in the fill color, as having no data.
void writeQualitySlots(float *color, float quality, bool faceQuality)
{
    const bool finite = std::isfinite(quality);
    color[0] = finite ? quality : 0.0f;
    color[1] = 0.0f;
    color[2] = 0.0f;
    color[3] = finite ? (faceQuality ? -2.0f : -1.0f) : 0.0f;
}

// The revision of what a fill or point variant colors with: the per-element colors, the
// scalars, or nothing that changes apart from the geometry.
std::uint64_t colorSourceRevision(const MeshGpuResourceCache::MeshSource &source, int variantIndex,
                                  bool points)
{
    using Fill = MeshGpuResourceCache::FillVariant;
    using Point = MeshGpuResourceCache::PointVariant;
    if (points)
        return variantIndex == int(Point::PerVertex) ? source.colorRevision
             : variantIndex == int(Point::PerVertexQuality) ? source.scalarRevision : 0;
    return variantIndex == int(Fill::PerVertex) || variantIndex == int(Fill::PerFace) ? source.colorRevision
         : variantIndex == int(Fill::PerVertexQuality) || variantIndex == int(Fill::PerFaceQuality)
             ? source.scalarRevision : 0;
}
}

QRhiVertexInputLayout MeshGpuResourceCache::edgeInputLayout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({ { kEdgeVertexStrideBytes } });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
        { 0, 1, QRhiVertexInputAttribute::UNormByte4, kEdgeVertexColorOffsetBytes },
        { 0, 2, QRhiVertexInputAttribute::UNormByte4, kEdgeColorOffsetBytes },
        { 0, 3, QRhiVertexInputAttribute::Float4, kEdgeQualityOffsetBytes }
    });
    return layout;
}

QRhiVertexInputLayout MeshGpuResourceCache::fatEdgeInputLayout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({ { kFatEdgeVertexStrideBytes } });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float3, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float3, 3 * sizeof(float) },
        { 0, 2, QRhiVertexInputAttribute::Float, 6 * sizeof(float) },
        { 0, 3, QRhiVertexInputAttribute::Float, 7 * sizeof(float) },
        { 0, 4, QRhiVertexInputAttribute::UNormByte4, kFatEdgeVertexColorOffsetBytes },
        { 0, 5, QRhiVertexInputAttribute::UNormByte4, kFatEdgeColorOffsetBytes },
        { 0, 6, QRhiVertexInputAttribute::Float4, kFatEdgeQualityOffsetBytes }
    });
    return layout;
}

struct MeshGpuResourceCache::CacheState
{
    // The buffers of one batched vertex stream, and the views handed to the draw code.
    struct BatchedBuffer {
        std::vector<std::unique_ptr<QRhiBuffer>> vbufs;
        std::vector<MeshGpuResourceCache::BufferBatchView> views; // one per buffer in vbufs
        void clear() { vbufs.clear(); views.clear(); }
        MeshGpuResourceCache::BufferBatches batches() const
        {
            return { views.data(), int(views.size()) };
        }
    };

    struct FillBatchGpu {
        std::unique_ptr<QRhiBuffer> vbuf;
        std::unique_ptr<QRhiBuffer> ibuf;
        std::unique_ptr<QRhiTexture> baseColorTexture;
        std::unique_ptr<QRhiTexture> normalTexture;
        std::unique_ptr<QRhiTexture> occlusionTexture;
        std::unique_ptr<QRhiTexture> roughnessTexture;
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

    struct FillVariantGpu {
        std::uint64_t geometryRevision = 0;
        std::uint64_t materialRevision = 0;
        std::uint64_t colorSourceRevision = 0; // colorSourceRevision()
        bool valid = false;
        std::vector<FillBatchGpu> batches;
        mutable std::vector<FillBatchView> viewBatches;
    };

    struct WireGpu {
        std::uint64_t geometryRevision = 0;
        bool wireRespectFaux = true;
        bool valid = false;
        BatchedBuffer buffer;
    };

    struct EdgeGpu {
        std::uint64_t geometryRevision = 0;
        std::uint64_t colorRevision = 0;
        std::uint64_t scalarRevision = 0;
        bool valid = false;
        std::unique_ptr<QRhiBuffer> vbuf;
        int vertexCount = 0;
        std::unique_ptr<QRhiBuffer> fatVbuf;
        int fatVertexCount = 0;
    };

    struct PointsVariantGpu {
        std::uint64_t geometryRevision = 0;
        std::uint64_t colorSourceRevision = 0;
        bool valid = false;
        BatchedBuffer buffer;
    };

    struct BBoxGpu {
        std::uint64_t geometryRevision = 0;
        bool valid = false;
        std::unique_ptr<QRhiBuffer> vbuf;
        int vertexCount = 0;
    };

    struct SelectionGpu {
        std::uint64_t geometryRevision = 0;
        std::uint64_t materialRevision = 0;
        std::uint64_t selectionRevision = 0;
        bool valid = false;
        BatchedBuffer selectedFaces;
        BatchedBuffer selectedVertices;
        std::unique_ptr<QRhiBuffer> selectedEdgesVbuf;
        int selectedEdgesVertexCount = 0;
        std::unique_ptr<QRhiBuffer> selectedEdgesFatVbuf;
        int selectedEdgesFatVertexCount = 0;
    };

    struct DecoratorNormalsGpu {
        std::uint64_t geometryRevision = 0;
        bool valid = false;
        BatchedBuffer vertexNormals;
        BatchedBuffer faceNormals;
        BatchedBuffer curvatureDirPD1;
        BatchedBuffer curvatureDirPD2;
    };

    struct DecoratorBoundaryGpu {
        std::uint64_t geometryRevision = 0;
        bool valid = false;
        std::unique_ptr<QRhiBuffer> boundaryEdgesVbuf;
        int boundaryEdgesVertexCount = 0;
        std::unique_ptr<QRhiBuffer> boundaryEdgesFatVbuf;
        int boundaryEdgesFatVertexCount = 0;
        std::unique_ptr<QRhiBuffer> textureSeamsVbuf;
        int textureSeamsVertexCount = 0;
        std::unique_ptr<QRhiBuffer> textureSeamsFatVbuf;
        int textureSeamsFatVertexCount = 0;
        std::unique_ptr<QRhiBuffer> nonManifoldEdgesVbuf;
        int nonManifoldEdgesVertexCount = 0;
        std::unique_ptr<QRhiBuffer> nonManifoldEdgesFatVbuf;
        int nonManifoldEdgesFatVertexCount = 0;
        std::unique_ptr<QRhiBuffer> nonManifoldVerticesVbuf;
        int nonManifoldVerticesVertexCount = 0;
    };

    struct MeshGpu {
        std::array<FillVariantGpu, 6> fill;
        std::array<PointsVariantGpu, 3> points;
        WireGpu wire;
        EdgeGpu edges;
        BBoxGpu bbox;
        SelectionGpu selection;
        DecoratorNormalsGpu decoratorNormals;
        DecoratorBoundaryGpu decoratorBoundaries;
    };

    std::unordered_map<QRhi *, std::unordered_map<std::uint64_t, MeshGpu>> byRhi;
};

MeshGpuResourceCache::MeshGpuResourceCache()
    : m_state(std::make_unique<CacheState>())
{
}

MeshGpuResourceCache::~MeshGpuResourceCache() = default;

// Adds the bytes of one mesh's buffers and textures, by product, to `s`. A template only
// because the cache's state is private to the class: the members calling it deduce the type.
template <class MeshGpu>
static void addMeshGpuBytes(MeshGpuResourceCache::GpuMeshMemoryStats &s, const MeshGpu &meshGpu)
{
    auto bufBytes = [](const std::unique_ptr<QRhiBuffer> &buf) -> qint64 {
        return buf ? qint64(buf->size()) : 0LL;
    };
    auto batchedBytes = [&](const auto &buffer) -> qint64 {
        qint64 bytes = 0;
        for (const auto &vbuf : buffer.vbufs)
            bytes += bufBytes(vbuf);
        return bytes;
    };
    auto texBytes = [](const std::unique_ptr<QRhiTexture> &tex) -> qint64 {
        if (!tex)
            return 0LL;
        const QSize sz = tex->pixelSize();
        return qint64(sz.width()) * sz.height() * 4; // RGBA8 = 4 bytes/pixel, 1 mip level
    };

    for (const auto &fv : meshGpu.fill) {
        if (!fv.valid)
            continue;
        for (const auto &batch : fv.batches) {
            s.fillBufferBytes += bufBytes(batch.vbuf) + bufBytes(batch.ibuf);
            s.textureBytes += texBytes(batch.baseColorTexture)
                            + texBytes(batch.normalTexture)
                            + texBytes(batch.occlusionTexture)
                            + texBytes(batch.roughnessTexture);
        }
    }
    if (meshGpu.wire.valid)
        s.wireBufferBytes += batchedBytes(meshGpu.wire.buffer);
    if (meshGpu.edges.valid)
        s.edgeBufferBytes += bufBytes(meshGpu.edges.vbuf) + bufBytes(meshGpu.edges.fatVbuf);
    for (const auto &pv : meshGpu.points) {
        if (pv.valid)
            s.pointsBufferBytes += batchedBytes(pv.buffer);
    }
    if (meshGpu.bbox.valid)
        s.bboxBufferBytes += bufBytes(meshGpu.bbox.vbuf);
    if (meshGpu.selection.valid)
        s.selectionBufferBytes += batchedBytes(meshGpu.selection.selectedFaces)
                                + batchedBytes(meshGpu.selection.selectedVertices);
    if (meshGpu.decoratorNormals.valid)
        s.decoratorNormalsBytes += batchedBytes(meshGpu.decoratorNormals.vertexNormals)
                                 + batchedBytes(meshGpu.decoratorNormals.faceNormals)
                                 + batchedBytes(meshGpu.decoratorNormals.curvatureDirPD1)
                                 + batchedBytes(meshGpu.decoratorNormals.curvatureDirPD2);
    if (meshGpu.decoratorBoundaries.valid)
        s.decoratorBoundariesBytes +=
            bufBytes(meshGpu.decoratorBoundaries.boundaryEdgesVbuf)
            + bufBytes(meshGpu.decoratorBoundaries.boundaryEdgesFatVbuf)
            + bufBytes(meshGpu.decoratorBoundaries.textureSeamsVbuf)
            + bufBytes(meshGpu.decoratorBoundaries.textureSeamsFatVbuf)
            + bufBytes(meshGpu.decoratorBoundaries.nonManifoldEdgesVbuf)
            + bufBytes(meshGpu.decoratorBoundaries.nonManifoldEdgesFatVbuf)
            + bufBytes(meshGpu.decoratorBoundaries.nonManifoldVerticesVbuf);
}

MeshGpuResourceCache::EnsureStats MeshGpuResourceCache::ensureMeshResources(
    QRhi *rhi,
    QRhiCommandBuffer *cb,
    const MeshSource &source,
    FillVariant fillVariant,
    PointVariant pointVariant,
    const Needs &needs)
{
    EnsureStats stats;
    if (!m_state || !rhi || !cb || !source.mesh || source.meshId == 0)
        return stats;

    QElapsedTimer timer;
    timer.start();

    const VCGMesh &meshData = *source.mesh;
    static const QStringList kEmptyTexturePaths;
    const QStringList &texturePaths =
        source.textureFilePaths ? *source.textureFilePaths : kEmptyTexturePaths;
    static const std::vector<MeshIOTextureAsset> kEmptyTextureAssets;
    const std::vector<MeshIOTextureAsset> &textureAssets =
        source.textureAssets ? *source.textureAssets : kEmptyTextureAssets;
    static const MeshIOMaterialSet kEmptyMaterialSet;
    const MeshIOMaterialSet &materialSet =
        source.materialSet ? *source.materialSet : kEmptyMaterialSet;
    auto &meshCache = m_state->byRhi[rhi][source.meshId];

    QRhiResourceUpdateBatch *updates = nullptr;
    auto ensureUpdates = [&]() -> QRhiResourceUpdateBatch * {
        if (!updates)
            updates = rhi->nextResourceUpdateBatch();
        return updates;
    };

    auto rebuildFillVariant =
        [&](CacheState::FillVariantGpu &dst, FillVariant variant) -> bool {
            // Quality goes in raw; the shader maps it through the range uniform, so a range
            // change rebuilds nothing.
            const std::uint64_t colorRevision = colorSourceRevision(source, fillVariantIndex(variant), false);
            if (dst.valid
                && dst.geometryRevision == source.geometryRevision
                && dst.materialRevision == source.materialRevision
                && dst.colorSourceRevision == colorRevision)
                return false;

            dst.valid = true;
            dst.geometryRevision = source.geometryRevision;
            dst.materialRevision = source.materialRevision;
            dst.colorSourceRevision = colorRevision;
            dst.batches.clear();
            dst.viewBatches.clear();

            if (meshData.FN() <= 0)
                return true;

            // Gate every optional-component read on the mesh's ACTUAL capability, not
            // just the ioMask intent bit. Per-wedge/vertex texcoords, colours and
            // quality are vcg OCF components: calling cWT()/cC()/cQ()/cT() when the
            // component is not enabled dereferences a null backing store and crashes.
            // ioMask (an I/O hint) can legitimately disagree with the live mesh — e.g.
            // after an undo/redo that pairs a restored ioMask with interned geometry —
            // so it must never alone authorize a component access.
            const bool meshHasFaceColor =
                (source.ioMask & vcg::tri::io::Mask::IOM_FACECOLOR) != 0
                && vcg::tri::HasPerFaceColor(meshData);
            const bool meshHasVertexColor =
                (source.ioMask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0
                && vcg::tri::HasPerVertexColor(meshData);
            const bool meshHasVertexQuality =
                (source.ioMask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0
                && vcg::tri::HasPerVertexQuality(meshData);
            const bool meshHasFaceQuality =
                (source.ioMask & vcg::tri::io::Mask::IOM_FACEQUALITY) != 0
                && vcg::tri::HasPerFaceQuality(meshData);
            const bool meshHasVertexTexcoord =
                (source.ioMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0
                && vcg::tri::HasPerVertexTexCoord(meshData);
            const bool meshHasWedgeTexcoord =
                (source.ioMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0
                && vcg::tri::HasPerWedgeTexCoord(meshData);

            const bool useFaceColor = (variant == FillVariant::PerFace) && meshHasFaceColor;
            const bool useVertexColor = (variant == FillVariant::PerVertex) && meshHasVertexColor;
            const bool useVertexQuality = isPerVertexQualityFillVariant(variant) && meshHasVertexQuality;
            const bool useFaceQuality = isPerFaceQualityFillVariant(variant) && meshHasFaceQuality;
            const bool hasTextureCoords = meshHasWedgeTexcoord || meshHasVertexTexcoord;
            const int textureAssociationCount = std::max(int(texturePaths.size()), int(textureAssets.size()));
            const bool hasTextureSlots = hasTextureCoords && textureAssociationCount > 0;
            const bool useTextureColor = (variant == FillVariant::Texture) && hasTextureSlots;
            const bool useVertexStyleColor = useVertexColor || useVertexQuality;
            const bool useFaceStyleColor = useFaceColor || useFaceQuality;
            const bool expandTriangles = useFaceStyleColor || useTextureColor;

            if (expandTriangles) {
                struct PreparedTexture {
                    bool attempted = false;
                    bool ready = false;
                    std::unique_ptr<QRhiTexture> texture;
                    QImage image;
                };

                struct PreparedGroup {
                    bool attempted = false;
                    bool ready = false;
                    QString basePath;
                    QString normalPath;
                    QString occlusionPath;
                    QString roughnessPath;
                    PreparedTexture base;
                    PreparedTexture normal;
                    PreparedTexture occlusion;
                    PreparedTexture roughness;
                };

                std::unordered_map<int, PreparedGroup> preparedGroups;

                enum class TextureChannel {
                    BaseColor,
                    Normal,
                    Occlusion,
                    Roughness
                };

                auto materialEntryForGroup = [&](int textureGroup) -> const MeshIOMaterialSlot * {
                    if (textureGroup < 0)
                        return nullptr;
                    if (textureGroup >= 0 && textureGroup < int(materialSet.entries.size()))
                        return &materialSet.entries[size_t(textureGroup)];
                    if (textureGroup >= 0 && textureGroup < texturePaths.size()) {
                        const QString groupBase = texturePaths.at(textureGroup);
                        for (const MeshIOMaterialSlot &slot : materialSet.entries) {
                            if (slot.baseColorTexture.filePath == groupBase)
                                return &slot;
                        }
                    }
                    if (materialSet.entries.size() == 1)
                        return &materialSet.entries.front();
                    return nullptr;
                };

                auto textureAssetForGroup = [&](int textureGroup) -> const MeshIOTextureAsset * {
                    if (const MeshIOMaterialSlot *slot = materialEntryForGroup(textureGroup)) {
                        const int assetIndex = slot->baseColorTexture.assetIndex;
                        if (assetIndex >= 0 && assetIndex < int(textureAssets.size()))
                            return &textureAssets[size_t(assetIndex)];
                        return nullptr;
                    }
                    if (textureGroup < 0 || textureGroup >= int(textureAssets.size()))
                        return nullptr;
                    return &textureAssets[size_t(textureGroup)];
                };

                auto textureAssetForRef = [&](const MeshIOMaterialTextureRef &ref)
                    -> const MeshIOTextureAsset * {
                    if (ref.assetIndex < 0 || ref.assetIndex >= int(textureAssets.size()))
                        return nullptr;
                    return &textureAssets[size_t(ref.assetIndex)];
                };

                auto channelTexturePath = [&](int textureGroup, TextureChannel channel) -> QString {
                    const MeshIOMaterialSlot *slot = materialEntryForGroup(textureGroup);
                    switch (channel) {
                    case TextureChannel::BaseColor:
                        if (slot && !slot->baseColorTexture.filePath.trimmed().isEmpty())
                            return slot->baseColorTexture.filePath.trimmed();
                        if (textureGroup >= 0 && textureGroup < texturePaths.size())
                            return texturePaths.at(textureGroup).trimmed();
                        if (const MeshIOTextureAsset *asset = textureAssetForGroup(textureGroup))
                            return asset->sourcePath.trimmed();
                        return QString();
                    case TextureChannel::Normal:
                        return (slot ? slot->normalTexture.filePath.trimmed() : QString());
                    case TextureChannel::Occlusion:
                        return (slot ? slot->occlusionTexture.filePath.trimmed() : QString());
                    case TextureChannel::Roughness:
                        return (slot ? slot->roughnessTexture.filePath.trimmed() : QString());
                    }
                    return QString();
                };

                auto prepareTextureFromPath = [&](PreparedTexture &prepared,
                                                  const QString &texturePath,
                                                  const MeshIOTextureAsset *asset) -> bool {
                    if (prepared.attempted)
                        return prepared.ready;
                    prepared.attempted = true;
                    QImage image;
                    if (asset && asset->hasImage()) {
                        image = asset->image;
                    } else {
                        if (texturePath.isEmpty())
                            return false;
                        // Through readImageFile, not QImageReader: it adds the stb
                        // fallback, without which anything Qt's plugins decline -- a
                        // Targa lacking the TrueVision 2.0 footer, say -- silently
                        // renders untextured.
                        QString imageError;
                        if (!TextureAssociationUtils::readImageFile(texturePath, image, imageError))
                            return false;
                    }
                    if (image.isNull())
                        return false;

                    image = image.convertToFormat(QImage::Format_RGBA8888);
                    prepared.texture.reset(
                        rhi->newTexture(QRhiTexture::RGBA8, image.size(), 1));
                    if (!prepared.texture || !prepared.texture->create()) {
                        prepared.texture.reset();
                        return false;
                    }

                    prepared.image = std::move(image);
                    prepared.ready = true;
                    return true;
                };

                auto ensureGroupPrepared = [&](int textureGroup) -> bool {
                    if (textureGroup < 0)
                        return false;
                    PreparedGroup &group = preparedGroups[textureGroup];
                    if (group.attempted)
                        return group.ready;
                    group.attempted = true;

                    const MeshIOTextureAsset *baseAsset = textureAssetForGroup(textureGroup);
                    group.basePath = channelTexturePath(textureGroup, TextureChannel::BaseColor);
                    if (!prepareTextureFromPath(group.base, group.basePath, baseAsset))
                        return false;

                    group.normalPath = channelTexturePath(textureGroup, TextureChannel::Normal);
                    group.occlusionPath = channelTexturePath(textureGroup, TextureChannel::Occlusion);
                    group.roughnessPath = channelTexturePath(textureGroup, TextureChannel::Roughness);
                    const MeshIOMaterialSlot *slot = materialEntryForGroup(textureGroup);
                    prepareTextureFromPath(group.normal, group.normalPath,
                        slot ? textureAssetForRef(slot->normalTexture) : nullptr);
                    prepareTextureFromPath(group.occlusion, group.occlusionPath,
                        slot ? textureAssetForRef(slot->occlusionTexture) : nullptr);
                    prepareTextureFromPath(group.roughness, group.roughnessPath,
                        slot ? textureAssetForRef(slot->roughnessTexture) : nullptr);

                    group.ready = true;
                    return true;
                };

                auto packExpandedFace =
                    [&](const VCGFace &f,
                        float *groupData,
                        size_t startBase,
                        bool useTextureForFace) {
                        QVector3D faceRgb(1.0f, 1.0f, 1.0f);
                        if (useFaceColor) {
                            const auto fc = f.cC();
                            faceRgb = QVector3D(
                                static_cast<float>(fc[0]) / 255.0f,
                                static_cast<float>(fc[1]) / 255.0f,
                                static_cast<float>(fc[2]) / 255.0f);
                        }

                        for (int corner = 0; corner < 3; ++corner) {
                            const auto *vertex = f.cV(corner);
                            QVector3D vertexRgb(1.0f, 1.0f, 1.0f);
                            if (useVertexColor) {
                                const auto vc = vertex->cC();
                                vertexRgb = QVector3D(
                                    static_cast<float>(vc[0]) / 255.0f,
                                    static_cast<float>(vc[1]) / 255.0f,
                                    static_cast<float>(vc[2]) / 255.0f);
                            }
                            const size_t base =
                                startBase + size_t(corner * kFillVertexStrideFloats);

                            groupData[base + 0] = vertex->cP()[0];
                            groupData[base + 1] = vertex->cP()[1];
                            groupData[base + 2] = vertex->cP()[2];
                            groupData[base + 3] = vertex->cN()[0];
                            groupData[base + 4] = vertex->cN()[1];
                            groupData[base + 5] = vertex->cN()[2];
                            if (useFaceQuality) {
                                writeQualitySlots(&groupData[base + 6], float(f.cQ()), true);
                            } else if (useVertexQuality) {
                                writeQualitySlots(&groupData[base + 6], float(vertex->cQ()), false);
                            } else {
                                groupData[base + 6] = useFaceStyleColor
                                    ? faceRgb.x()
                                    : (useVertexStyleColor ? vertexRgb.x() : 1.0f);
                                groupData[base + 7] = useFaceStyleColor
                                    ? faceRgb.y()
                                    : (useVertexStyleColor ? vertexRgb.y() : 1.0f);
                                groupData[base + 8] = useFaceStyleColor
                                    ? faceRgb.z()
                                    : (useVertexStyleColor ? vertexRgb.z() : 1.0f);
                                groupData[base + 9] =
                                    (useFaceStyleColor || useVertexStyleColor) ? 1.0f : 0.0f;
                            }

                            if (useTextureForFace) {
                                if (meshHasWedgeTexcoord) {
                                    const auto &wt = f.cWT(corner);
                                    groupData[base + 10] = wt.U();
                                    groupData[base + 11] = wt.V();
                                } else if (meshHasVertexTexcoord) {
                                    const auto &vt = vertex->cT();
                                    groupData[base + 10] = vt.U();
                                    groupData[base + 11] = vt.V();
                                } else {
                                    groupData[base + 10] = 0.0f;
                                    groupData[base + 11] = 0.0f;
                                }
                                groupData[base + 12] = 1.0f;
                            } else {
                                groupData[base + 10] = 0.0f;
                                groupData[base + 11] = 0.0f;
                                groupData[base + 12] = 0.0f;
                            }
                        }
                    };

                const size_t faceContainerSize = meshData.face.size();
                const bool compactFaces = faceContainerSize == size_t(meshData.FN());
                for (size_t firstFace = 0; firstFace < faceContainerSize;
                     firstFace += kMaxExpandedFillBatchFaces) {
                    const size_t lastFace = firstFace + std::min(
                        kMaxExpandedFillBatchFaces, faceContainerSize - firstFace);
                    // Built as QByteArrays so each one is moved into the upload, not copied.
                    std::map<int, QByteArray> groupedTriangles;

                    if (!useTextureColor && compactFaces) {
                        QByteArray &groupBytes = groupedTriangles[-1];
                        groupBytes = QByteArray(
                            qsizetype((lastFace - firstFace) * kExpandedFillTriangleBytes),
                            Qt::Uninitialized);
                        float *groupData = reinterpret_cast<float *>(groupBytes.data());
                        parallelFor(lastFace - firstFace, [&](size_t first, size_t last) {
                            for (size_t localFi = first; localFi < last; ++localFi) {
                                packExpandedFace(
                                    meshData.face[firstFace + localFi],
                                    groupData,
                                    localFi * 3u * size_t(kFillVertexStrideFloats),
                                    false);
                            }
                        });
                    } else {
                        for (size_t fi = firstFace; fi < lastFace; ++fi) {
                            const auto &f = meshData.face[fi];
                            if (f.IsD())
                                continue;
                            int textureGroup = -1;
                            bool useTextureForFace = false;
                            const int textureIndex =
                                useTextureColor ? vcgFaceTextureGroup(source.ioMask, f) : 0;

                            // A face with no texture is drawn untextured, not with texture 0
                            // read at UVs that mean nothing.
                            if (useTextureColor && textureIndex >= 0) {
                                if (ensureGroupPrepared(textureIndex)) {
                                    textureGroup = textureIndex;
                                    useTextureForFace = true;
                                } else if (ensureGroupPrepared(0)) {
                                    textureGroup = 0;
                                    useTextureForFace = true;
                                }
                            }

                            QByteArray &groupBytes = groupedTriangles[textureGroup];
                            const size_t startBase = size_t(groupBytes.size()) / sizeof(float);
                            groupBytes.resize(
                                groupBytes.size() + qsizetype(kExpandedFillTriangleBytes));
                            packExpandedFace(
                                f, reinterpret_cast<float *>(groupBytes.data()), startBase,
                                useTextureForFace);
                        }
                    }

                    for (auto &groupEntry : groupedTriangles) {
                        if (groupEntry.second.isEmpty())
                            continue;

                        const size_t bufferBytes = size_t(groupEntry.second.size());
                        CacheState::FillBatchGpu batch;
                        batch.textureGroupIndex = groupEntry.first;
                        batch.vertexCount = static_cast<int>(
                            bufferBytes / (sizeof(float) * kFillVertexStrideFloats));
                        batch.indexCount = 0;
                        batch.vbuf.reset(
                            rhi->newBuffer(
                                QRhiBuffer::Immutable,
                                QRhiBuffer::VertexBuffer,
                                static_cast<quint32>(bufferBytes)));
                        if (!batch.vbuf || !batch.vbuf->create()) {
                            batch.vbuf.reset();
                            continue;
                        }
                        ensureUpdates()->uploadStaticBuffer(
                            batch.vbuf.get(), std::move(groupEntry.second));

                        QImage baseTextureUploadImage;
                        QImage normalTextureUploadImage;
                        QImage occlusionTextureUploadImage;
                        QImage roughnessTextureUploadImage;
                        if (groupEntry.first >= 0) {
                            auto it = preparedGroups.find(groupEntry.first);
                            if (it != preparedGroups.end() && it->second.ready) {
                                PreparedGroup &group = it->second;
                                batch.baseColorTexturePath = group.basePath;
                                if (group.base.ready) {
                                    batch.baseColorTexture = std::move(group.base.texture);
                                    baseTextureUploadImage = std::move(group.base.image);
                                }
                                if (group.normal.ready) {
                                    batch.normalTexture = std::move(group.normal.texture);
                                    batch.normalTexturePath = group.normalPath;
                                    normalTextureUploadImage = std::move(group.normal.image);
                                }
                                if (group.occlusion.ready) {
                                    batch.occlusionTexture = std::move(group.occlusion.texture);
                                    batch.occlusionTexturePath = group.occlusionPath;
                                    occlusionTextureUploadImage = std::move(group.occlusion.image);
                                }
                                if (group.roughness.ready) {
                                    batch.roughnessTexture = std::move(group.roughness.texture);
                                    batch.roughnessTexturePath = group.roughnessPath;
                                    roughnessTextureUploadImage = std::move(group.roughness.image);
                                }
                            }
                            if (batch.baseColorTexturePath.isEmpty()) {
                                batch.baseColorTexturePath = channelTexturePath(
                                    groupEntry.first,
                                    TextureChannel::BaseColor);
                            }
                            if (const MeshIOMaterialSlot *slot =
                                    materialEntryForGroup(groupEntry.first)) {
                                batch.normalScale = slot->normalScale;
                                batch.occlusionStrength = slot->occlusionStrength;
                                batch.roughnessFactor = slot->roughnessFactor;
                            }
                        }

                        if (batch.baseColorTexture && !baseTextureUploadImage.isNull()) {
                            QRhiTextureUploadEntry textureEntry(
                                0, 0, QRhiTextureSubresourceUploadDescription(baseTextureUploadImage));
                            ensureUpdates()->uploadTexture(
                                batch.baseColorTexture.get(),
                                QRhiTextureUploadDescription({ textureEntry }));
                        }
                        if (batch.normalTexture && !normalTextureUploadImage.isNull()) {
                            QRhiTextureUploadEntry textureEntry(
                                0, 0, QRhiTextureSubresourceUploadDescription(normalTextureUploadImage));
                            ensureUpdates()->uploadTexture(
                                batch.normalTexture.get(),
                                QRhiTextureUploadDescription({ textureEntry }));
                        }
                        if (batch.occlusionTexture && !occlusionTextureUploadImage.isNull()) {
                            QRhiTextureUploadEntry textureEntry(
                                0, 0, QRhiTextureSubresourceUploadDescription(occlusionTextureUploadImage));
                            ensureUpdates()->uploadTexture(
                                batch.occlusionTexture.get(),
                                QRhiTextureUploadDescription({ textureEntry }));
                        }
                        if (batch.roughnessTexture && !roughnessTextureUploadImage.isNull()) {
                            QRhiTextureUploadEntry textureEntry(
                                0, 0, QRhiTextureSubresourceUploadDescription(roughnessTextureUploadImage));
                            ensureUpdates()->uploadTexture(
                                batch.roughnessTexture.get(),
                                QRhiTextureUploadDescription({ textureEntry }));
                        }

                        dst.batches.push_back(std::move(batch));
                    }
                }

                QSet<int> preparedTextureGroups;
                for (const auto &batch : dst.batches) {
                    if (batch.textureGroupIndex >= 0)
                        preparedTextureGroups.insert(batch.textureGroupIndex);
                }

                const int textureGroupCount = std::max(
                    textureAssociationCount,
                    int(materialSet.entries.size()));
                for (int textureGroup = 0; textureGroup < textureGroupCount; ++textureGroup) {
                    if (preparedTextureGroups.contains(textureGroup))
                        continue;

                    PreparedGroup *group = nullptr;
                    if (ensureGroupPrepared(textureGroup)) {
                        auto it = preparedGroups.find(textureGroup);
                        if (it != preparedGroups.end() && it->second.ready)
                            group = &it->second;
                    }

                    CacheState::FillBatchGpu batch;
                    batch.textureGroupIndex = textureGroup;
                    batch.vertexCount = 0;
                    batch.indexCount = 0;

                    QImage baseTextureUploadImage;
                    QImage normalTextureUploadImage;
                    QImage occlusionTextureUploadImage;
                    QImage roughnessTextureUploadImage;

                    if (group) {
                        batch.baseColorTexturePath = group->basePath;
                        if (group->base.ready) {
                            batch.baseColorTexture = std::move(group->base.texture);
                            baseTextureUploadImage = std::move(group->base.image);
                        }
                        if (group->normal.ready) {
                            batch.normalTexture = std::move(group->normal.texture);
                            batch.normalTexturePath = group->normalPath;
                            normalTextureUploadImage = std::move(group->normal.image);
                        }
                        if (group->occlusion.ready) {
                            batch.occlusionTexture = std::move(group->occlusion.texture);
                            batch.occlusionTexturePath = group->occlusionPath;
                            occlusionTextureUploadImage = std::move(group->occlusion.image);
                        }
                        if (group->roughness.ready) {
                            batch.roughnessTexture = std::move(group->roughness.texture);
                            batch.roughnessTexturePath = group->roughnessPath;
                            roughnessTextureUploadImage = std::move(group->roughness.image);
                        }
                    }

                    if (batch.baseColorTexturePath.isEmpty()) {
                        batch.baseColorTexturePath = channelTexturePath(
                            textureGroup,
                            TextureChannel::BaseColor);
                    }
                    if (const MeshIOMaterialSlot *slot = materialEntryForGroup(textureGroup)) {
                        batch.normalScale = slot->normalScale;
                        batch.occlusionStrength = slot->occlusionStrength;
                        batch.roughnessFactor = slot->roughnessFactor;
                        if (batch.normalTexturePath.isEmpty())
                            batch.normalTexturePath = slot->normalTexture.filePath.trimmed();
                        if (batch.occlusionTexturePath.isEmpty())
                            batch.occlusionTexturePath = slot->occlusionTexture.filePath.trimmed();
                        if (batch.roughnessTexturePath.isEmpty())
                            batch.roughnessTexturePath = slot->roughnessTexture.filePath.trimmed();
                    }

                    if (batch.baseColorTexture && !baseTextureUploadImage.isNull()) {
                        QRhiTextureUploadEntry textureEntry(
                            0, 0, QRhiTextureSubresourceUploadDescription(baseTextureUploadImage));
                        ensureUpdates()->uploadTexture(
                            batch.baseColorTexture.get(),
                            QRhiTextureUploadDescription({ textureEntry }));
                    }
                    if (batch.normalTexture && !normalTextureUploadImage.isNull()) {
                        QRhiTextureUploadEntry textureEntry(
                            0, 0, QRhiTextureSubresourceUploadDescription(normalTextureUploadImage));
                        ensureUpdates()->uploadTexture(
                            batch.normalTexture.get(),
                            QRhiTextureUploadDescription({ textureEntry }));
                    }
                    if (batch.occlusionTexture && !occlusionTextureUploadImage.isNull()) {
                        QRhiTextureUploadEntry textureEntry(
                            0, 0, QRhiTextureSubresourceUploadDescription(occlusionTextureUploadImage));
                        ensureUpdates()->uploadTexture(
                            batch.occlusionTexture.get(),
                            QRhiTextureUploadDescription({ textureEntry }));
                    }
                    if (batch.roughnessTexture && !roughnessTextureUploadImage.isNull()) {
                        QRhiTextureUploadEntry textureEntry(
                            0, 0, QRhiTextureSubresourceUploadDescription(roughnessTextureUploadImage));
                        ensureUpdates()->uploadTexture(
                            batch.roughnessTexture.get(),
                            QRhiTextureUploadDescription({ textureEntry }));
                    }

                    if (batch.baseColorTexture
                        || batch.normalTexture
                        || batch.occlusionTexture
                        || batch.roughnessTexture
                        || !batch.baseColorTexturePath.isEmpty()
                        || !batch.normalTexturePath.isEmpty()
                        || !batch.occlusionTexturePath.isEmpty()
                        || !batch.roughnessTexturePath.isEmpty()) {
                        dst.batches.push_back(std::move(batch));
                    }
                }
            } else {
                if (meshData.VN() <= 0 || meshData.FN() <= 0)
                    return true;

                const float useMeshColor = useVertexStyleColor ? 1.0f : 0.0f;
                auto packVertex = [&](const VCGVertex &v, float *out) {
                    QVector3D vertexRgb(1.0f, 1.0f, 1.0f);
                    if (useVertexColor) {
                        const auto vc = v.cC();
                        vertexRgb = QVector3D(
                            static_cast<float>(vc[0]) / 255.0f,
                            static_cast<float>(vc[1]) / 255.0f,
                            static_cast<float>(vc[2]) / 255.0f);
                    }
                    out[0] = v.cP()[0];
                    out[1] = v.cP()[1];
                    out[2] = v.cP()[2];
                    out[3] = v.cN()[0];
                    out[4] = v.cN()[1];
                    out[5] = v.cN()[2];
                    if (useVertexQuality) {
                        writeQualitySlots(out + 6, float(v.cQ()), false);
                    } else {
                        out[6] = vertexRgb.x();
                        out[7] = vertexRgb.y();
                        out[8] = vertexRgb.z();
                        out[9] = useMeshColor;
                    }
                    out[10] = 0.0f;
                    out[11] = 0.0f;
                    out[12] = 0.0f;
                };

                // One vertex buffer for the whole mesh wrapped its quint32 size above about 82M
                // vertices. The faces are cut, in order, into batches whose vertices and indices
                // each fit the fill's byte budget; every batch has its own vertex buffer and its
                // faces are re-indexed into it, so a vertex used on both sides of a cut is
                // stored twice. In a scan's face order a batch's faces are close together and
                // few vertices repeat. Built as QByteArrays, moved into the upload.
                constexpr quint32 kNoIndex = std::numeric_limits<quint32>::max();
                const size_t vertexBytes = size_t(kFillVertexStrideFloats) * sizeof(float);
                const size_t faceBytes = 3u * sizeof(quint32);
                const size_t maxBatchVertices = kMaxExpandedFillBatchBytes / vertexBytes;
                const size_t maxBatchFaces = kMaxExpandedFillBatchBytes / faceBytes;
                std::vector<quint32> localIndex(meshData.vert.size(), kNoIndex);
                std::vector<size_t> batchVertices; // the mesh index of each batch vertex
                const size_t faceCount = meshData.face.size();
                // The faces' vertex indices, read in parallel: the remapping below has to run
                // in order, and over the faces themselves it was bound by one thread's memory
                // bandwidth. A deleted face is marked by kNoIndex in its first corner.
                std::vector<quint32> corners(faceCount * 3u);
                parallelFor(faceCount, [&](size_t first, size_t last) {
                    for (size_t i = first; i < last; ++i) {
                        const VCGFace &f = meshData.face[i];
                        for (int c = 0; c < 3; ++c)
                            corners[i * 3 + c] = f.IsD()
                                ? kNoIndex
                                : quint32(vcg::tri::Index(meshData, f.cV(c)));
                    }
                });
                size_t fi = 0;
                while (fi < faceCount) {
                    batchVertices.clear();
                    QByteArray ibytes(
                        qsizetype(std::min(maxBatchFaces, faceCount - fi) * faceBytes),
                        Qt::Uninitialized);
                    quint32 *idata = reinterpret_cast<quint32 *>(ibytes.data());
                    size_t batchFaces = 0;
                    for (; fi < faceCount; ++fi) {
                        const quint32 *vi = &corners[fi * 3];
                        if (vi[0] == kNoIndex)
                            continue;
                        size_t fresh = 0;
                        for (int c = 0; c < 3; ++c)
                            fresh += localIndex[vi[c]] == kNoIndex;
                        if (batchFaces == maxBatchFaces
                            || batchVertices.size() + fresh > maxBatchVertices)
                            break;
                        for (int c = 0; c < 3; ++c) {
                            if (localIndex[vi[c]] == kNoIndex) {
                                localIndex[vi[c]] = quint32(batchVertices.size());
                                batchVertices.push_back(vi[c]);
                            }
                            idata[batchFaces * 3 + c] = localIndex[vi[c]];
                        }
                        ++batchFaces;
                    }
                    for (const size_t vi : batchVertices)
                        localIndex[vi] = kNoIndex;
                    if (batchFaces == 0)
                        continue;
                    ibytes.resize(qsizetype(batchFaces * faceBytes));

                    QByteArray vbytes(qsizetype(batchVertices.size() * vertexBytes),
                                      Qt::Uninitialized);
                    float *vdata = reinterpret_cast<float *>(vbytes.data());
                    parallelFor(batchVertices.size(), [&](size_t first, size_t last) {
                        for (size_t i = first; i < last; ++i)
                            packVertex(meshData.vert[batchVertices[i]],
                                       vdata + i * size_t(kFillVertexStrideFloats));
                    });

                    CacheState::FillBatchGpu batch;
                    batch.vbuf.reset(rhi->newBuffer(
                        QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(vbytes.size())));
                    batch.ibuf.reset(rhi->newBuffer(
                        QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, quint32(ibytes.size())));
                    if (!batch.vbuf || !batch.ibuf || !batch.vbuf->create()
                        || !batch.ibuf->create()) {
                        dst.batches.clear();
                        return true;
                    }
                    batch.vertexCount = int(batchVertices.size());
                    batch.indexCount = int(size_t(ibytes.size()) / sizeof(quint32));
                    ensureUpdates()->uploadStaticBuffer(batch.vbuf.get(), std::move(vbytes));
                    ensureUpdates()->uploadStaticBuffer(batch.ibuf.get(), std::move(ibytes));
                    dst.batches.push_back(std::move(batch));
                }
            }
            return true;
        };

    // Packs `count` items into vertex buffers of at most kMaxExpandedFillBatchBytes, the
    // fill's budget: a QRhi buffer size is a quint32, and one buffer for a large mesh wrapped.
    // Each item is `itemFloats` floats and `itemVertices` vertices; pack(i, out) writes item i
    // to out and returns false to leave it out. A dense stream keeps every item and packs in
    // parallel; a sparse one appends what it keeps, so its memory follows what is kept. Each
    // batch is built in a QByteArray and moved into the update batch, since QRhi copies the
    // data of the pointer overload.
    auto buildBatched = [&](CacheState::BatchedBuffer &dst, size_t count, size_t itemFloats,
                            int itemVertices, bool dense, auto &&pack) {
        dst.clear();
        const size_t itemBytes = itemFloats * sizeof(float);
        const size_t perBatch = kMaxExpandedFillBatchBytes / itemBytes;
        QByteArray bytes;
        size_t packed = 0;
        auto flush = [&]() -> bool {
            if (packed == 0)
                return true;
            std::unique_ptr<QRhiBuffer> vbuf(rhi->newBuffer(
                QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, quint32(bytes.size())));
            if (!vbuf || !vbuf->create()) {
                dst.clear();
                return false;
            }
            ensureUpdates()->uploadStaticBuffer(vbuf.get(), std::move(bytes));
            dst.views.push_back({ vbuf.get(), int(packed) * itemVertices });
            dst.vbufs.push_back(std::move(vbuf));
            bytes = QByteArray();
            packed = 0;
            return true;
        };
        if (dense) {
            for (size_t first = 0; first < count; first += perBatch) {
                const size_t n = std::min(perBatch, count - first);
                bytes = QByteArray(qsizetype(n * itemBytes), Qt::Uninitialized);
                float *out = reinterpret_cast<float *>(bytes.data());
                parallelFor(n, [&](size_t a, size_t b) {
                    for (size_t i = a; i < b; ++i)
                        pack(first + i, out + i * itemFloats);
                });
                packed = n;
                if (!flush())
                    return;
            }
            return;
        }
        std::vector<float> item(itemFloats);
        for (size_t i = 0; i < count; ++i) {
            if (!pack(i, item.data()))
                continue;
            bytes.append(reinterpret_cast<const char *>(item.data()), qsizetype(itemBytes));
            if (++packed == perBatch && !flush())
                return;
        }
        flush();
    };

    auto rebuildWire = [&](CacheState::WireGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision
            && dst.wireRespectFaux == source.wireRespectFaux)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.wireRespectFaux = source.wireRespectFaux;
        dst.buffer.clear();

        if (meshData.FN() <= 0)
            return true;

        // Three corners of position + barycentric coordinates per face.
        auto packFace = [&](size_t fi, float *out) -> bool {
            const VCGFace &f = meshData.face[fi];
            if (f.IsD())
                return false;
            // Standard barycentric assignment: corner k → (0…1…0) with 1 at position k.
            // To suppress a FAUX edge e (between corners e and (e+1)%3):
            // set component (e+2)%3 to 1.0 at both those corners, so that
            // component never reaches 0 along the suppressed edge.
            float bary[3][3] = {
                { 1.0f, 0.0f, 0.0f },
                { 0.0f, 1.0f, 0.0f },
                { 0.0f, 0.0f, 1.0f }
            };
            if (source.wireRespectFaux) {
                for (int e = 0; e < 3; ++e) {
                    if (f.IsF(e)) {
                        const int k = (e + 2) % 3;
                        bary[e][k]           = 1.0f;
                        bary[(e + 1) % 3][k] = 1.0f;
                    }
                }
            }
            for (int corner = 0; corner < 3; ++corner) {
                const auto *vertex = f.cV(corner);
                float *o = out + corner * 6;
                o[0] = vertex->cP()[0];
                o[1] = vertex->cP()[1];
                o[2] = vertex->cP()[2];
                o[3] = bary[corner][0];
                o[4] = bary[corner][1];
                o[5] = bary[corner][2];
            }
            return true;
        };
        const bool compactFaces = meshData.face.size() == size_t(meshData.FN());
        buildBatched(dst.buffer, meshData.face.size(), 18, 3, compactFaces, packFace);
        return true;
    };

    auto rebuildPointsVariant =
        [&](CacheState::PointsVariantGpu &dst, PointVariant variant) -> bool {
            const std::uint64_t colorRevision = colorSourceRevision(source, pointVariantIndex(variant), true);
            if (dst.valid && dst.geometryRevision == source.geometryRevision
                && dst.colorSourceRevision == colorRevision)
                return false;

            dst.valid = true;
            dst.geometryRevision = source.geometryRevision;
            dst.colorSourceRevision = colorRevision;
            dst.buffer.clear();

            if (meshData.VN() <= 0)
                return true;

            const bool meshHasVertexColor = (source.ioMask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0;
            const bool meshHasVertexQuality =
                (source.ioMask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0;
            const bool meshHasVertexNormal =
                (source.ioMask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0;
            const bool useVertexColor = (variant == PointVariant::PerVertex) && meshHasVertexColor;
            const bool useVertexQuality =
                (variant == PointVariant::PerVertexQuality) && meshHasVertexQuality;
            const bool useVertexStyleColor = useVertexColor || useVertexQuality;
            const float useMeshColor = useVertexStyleColor ? 1.0f : 0.0f;

            auto packVertex = [&](size_t vi, float *out) -> bool {
                const auto &v = meshData.vert[vi];
                QVector3D vertexRgb(1.0f, 1.0f, 1.0f);
                if (useVertexColor) {
                    const auto vc = v.cC();
                    vertexRgb = QVector3D(
                        static_cast<float>(vc[0]) / 255.0f,
                        static_cast<float>(vc[1]) / 255.0f,
                        static_cast<float>(vc[2]) / 255.0f);
                }
                out[0] = v.cP()[0];
                out[1] = v.cP()[1];
                out[2] = v.cP()[2];
                if (useVertexQuality) {
                    writeQualitySlots(out + 3, float(v.cQ()), false);
                } else {
                    out[3] = vertexRgb.x();
                    out[4] = vertexRgb.y();
                    out[5] = vertexRgb.z();
                    out[6] = useMeshColor;
                }
                const float nx = v.cN()[0];
                const float ny = v.cN()[1];
                const float nz = v.cN()[2];
                const float nLen2 = nx * nx + ny * ny + nz * nz;
                const bool normalFinite =
                    std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz);
                const bool hasUsableNormal =
                    meshHasVertexNormal && normalFinite
                    && nLen2 > 1e-12f && nLen2 < 1e12f;
                out[7] = hasUsableNormal ? nx : 0.0f;
                out[8] = hasUsableNormal ? ny : 0.0f;
                out[9] = hasUsableNormal ? nz : 1.0f;
                out[10] = hasUsableNormal ? 1.0f : 0.0f;
                return true;
            };
            buildBatched(dst.buffer, size_t(meshData.VN()), kPointsVertexStrideFloats, 1, true,
                         packVertex);
            return true;
        };

    auto rebuildEdges = [&](CacheState::EdgeGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision
            && dst.colorRevision == source.colorRevision
            && dst.scalarRevision == source.scalarRevision)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.colorRevision = source.colorRevision;
        dst.scalarRevision = source.scalarRevision;
        dst.vbuf.reset();
        dst.vertexCount = 0;
        dst.fatVbuf.reset();
        dst.fatVertexCount = 0;

        if (meshData.EN() <= 0)
            return true;

        std::vector<EdgeVertexGpu> vdata;
        std::vector<FatEdgeVertexGpu> fatVdata;
        vdata.reserve(static_cast<size_t>(meshData.EN()) * 2);
        fatVdata.reserve(static_cast<size_t>(meshData.EN()) * 6);

        const auto rgba = [](const vcg::Color4b &color) {
            return std::array<std::uint8_t, 4> {
                color[0], color[1], color[2], color[3]
            };
        };
        for (int ei = 0; ei < meshData.EN(); ++ei) {
            const auto &e = meshData.edge[ei];
            if (e.IsD())
                continue;
            const auto *v0 = e.cV(0);
            const auto *v1 = e.cV(1);
            if (!v0 || !v1)
                continue;

            const auto &p0 = v0->cP();
            const auto &p1 = v1->cP();
            const auto vertexColor0 = rgba(v0->cC());
            const auto vertexColor1 = rgba(v1->cC());
            const auto edgeColor = rgba(e.cC());
            const auto quality = [&e](const VCGVertex *v) {
                const float vq = float(v->cQ());
                const float eq = float(e.cQ());
                const bool vf = std::isfinite(vq);
                const bool ef = std::isfinite(eq);
                return std::array<float, 4> { vf ? vq : 0.0f, vf ? 1.0f : 0.0f,
                                              ef ? eq : 0.0f, ef ? 1.0f : 0.0f };
            };
            const auto quality0 = quality(v0);
            const auto quality1 = quality(v1);

            const auto appendThinVertex =
                [&](const auto &point, const std::array<std::uint8_t, 4> &vertexColor,
                    const std::array<float, 4> &q) {
                    EdgeVertexGpu vertex {};
                    std::copy_n(point.V(), 3, vertex.position);
                    std::copy(vertexColor.begin(), vertexColor.end(), vertex.vertexColor);
                    std::copy(edgeColor.begin(), edgeColor.end(), vertex.edgeColor);
                    std::copy(q.begin(), q.end(), vertex.quality);
                    vdata.push_back(vertex);
                };
            appendThinVertex(p0, vertexColor0, quality0);
            appendThinVertex(p1, vertexColor1, quality1);

            for (const auto &tpl : LineRenderer::kFatTriTemplate) {
                FatEdgeVertexGpu vertex {};
                std::copy_n(p0.V(), 3, vertex.p0);
                std::copy_n(p1.V(), 3, vertex.p1);
                vertex.along = tpl[0];
                vertex.side = tpl[1];
                const auto &vertexColor = tpl[0] < 0.5f ? vertexColor0 : vertexColor1;
                const auto &q = tpl[0] < 0.5f ? quality0 : quality1;
                std::copy(vertexColor.begin(), vertexColor.end(), vertex.vertexColor);
                std::copy(edgeColor.begin(), edgeColor.end(), vertex.edgeColor);
                std::copy(q.begin(), q.end(), vertex.quality);
                fatVdata.push_back(vertex);
            }
        }

        if (vdata.empty())
            return true;

        dst.vbuf.reset(
            rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(vdata.size() * sizeof(EdgeVertexGpu))));
        if (!dst.vbuf || !dst.vbuf->create()) {
            dst.vbuf.reset();
            return true;
        }

        ensureUpdates()->uploadStaticBuffer(dst.vbuf.get(), vdata.data());
        dst.vertexCount = static_cast<int>(vdata.size());

        if (!fatVdata.empty()) {
            dst.fatVbuf.reset(
                rhi->newBuffer(
                    QRhiBuffer::Immutable,
                    QRhiBuffer::VertexBuffer,
                    static_cast<quint32>(fatVdata.size() * sizeof(FatEdgeVertexGpu))));
            if (!dst.fatVbuf || !dst.fatVbuf->create()) {
                dst.fatVbuf.reset();
            } else {
                ensureUpdates()->uploadStaticBuffer(dst.fatVbuf.get(), fatVdata.data());
                dst.fatVertexCount = static_cast<int>(fatVdata.size());
            }
        }
        return true;
    };

    auto uploadLineBuffer = [&](const std::vector<float> &lineData,
                                std::unique_ptr<QRhiBuffer> &dstBuffer,
                                int &dstVertexCount) {
        dstBuffer.reset();
        dstVertexCount = 0;
        if (lineData.empty())
            return;

        dstBuffer.reset(
            rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(lineData.size() * sizeof(float))));
        if (!dstBuffer || !dstBuffer->create()) {
            dstBuffer.reset();
            return;
        }

        ensureUpdates()->uploadStaticBuffer(dstBuffer.get(), lineData.data());
        dstVertexCount = static_cast<int>(lineData.size() / 3);
    };

    auto uploadFatLineBuffer = [&](const std::vector<float> &lineData,
                                   std::unique_ptr<QRhiBuffer> &dstBuffer,
                                   int &dstVertexCount) {
        dstBuffer.reset();
        dstVertexCount = 0;
        if (lineData.size() < LineRenderer::kLineStrideFloats)
            return;

        std::vector<float> fatData = LineRenderer::buildFatLineVertices(lineData);
        if (fatData.empty())
            return;

        dstBuffer.reset(
            rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(fatData.size() * sizeof(float))));
        if (!dstBuffer || !dstBuffer->create()) {
            dstBuffer.reset();
            return;
        }

        ensureUpdates()->uploadStaticBuffer(dstBuffer.get(), fatData.data());
        dstVertexCount = static_cast<int>(fatData.size() / LineRenderer::kFatLineStrideFloats);
    };

    auto rebuildSelection = [&](CacheState::SelectionGpu &dst) -> bool {
        if (dst.valid
            && dst.geometryRevision == source.geometryRevision
            && dst.materialRevision == source.materialRevision
            && dst.selectionRevision == source.selectionRevision) {
            return false;
        }

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.materialRevision = source.materialRevision;
        dst.selectionRevision = source.selectionRevision;
        dst.selectedFaces.clear();
        dst.selectedVertices.clear();
        dst.selectedEdgesVbuf.reset();
        dst.selectedEdgesVertexCount = 0;
        dst.selectedEdgesFatVbuf.reset();
        dst.selectedEdgesFatVertexCount = 0;

        if (meshData.VN() <= 0)
            return true;

        // Batched and appended as kept, so the memory follows the selection, not the mesh.
        buildBatched(dst.selectedFaces, size_t(meshData.FN()), 9, 3, false,
                     [&](size_t fi, float *out) {
            const auto &f = meshData.face[fi];
            if (f.IsD() || !f.IsS())
                return false;
            for (int corner = 0; corner < 3; ++corner) {
                const auto *v = f.cV(corner);
                if (!v)
                    return false;
                out[corner * 3 + 0] = v->cP()[0];
                out[corner * 3 + 1] = v->cP()[1];
                out[corner * 3 + 2] = v->cP()[2];
            }
            return true;
        });
        buildBatched(dst.selectedVertices, size_t(meshData.VN()), 3, 1, false,
                     [&](size_t vi, float *out) {
            const auto &v = meshData.vert[vi];
            if (v.IsD() || !v.IsS())
                return false;
            out[0] = v.cP()[0];
            out[1] = v.cP()[1];
            out[2] = v.cP()[2];
            return true;
        });

        // Selected edges come from two unrelated places. A triangle mesh keeps them as
        // per-face bits, duplicated on both faces of an interior edge, so they have to
        // be de-duplicated or every shared edge is drawn twice. A polyline layer keeps
        // real edge elements with a selection bit of their own. A layer is one or the
        // other, so both feed the same buffer.
        std::vector<float> selectedEdgeLines;
        std::unordered_set<std::uint64_t> seenEdges;
        const auto pushSegment = [&](const VCGVertex *a, const VCGVertex *b) {
            if (!a || !b)
                return;
            selectedEdgeLines.push_back(a->cP()[0]);
            selectedEdgeLines.push_back(a->cP()[1]);
            selectedEdgeLines.push_back(a->cP()[2]);
            selectedEdgeLines.push_back(b->cP()[0]);
            selectedEdgeLines.push_back(b->cP()[1]);
            selectedEdgeLines.push_back(b->cP()[2]);
        };

        for (int fi = 0; fi < meshData.FN(); ++fi) {
            const auto &f = meshData.face[fi];
            if (f.IsD())
                continue;
            for (int corner = 0; corner < 3; ++corner) {
                if (!f.IsFaceEdgeS(corner))
                    continue;
                const auto *a = f.cV(corner);
                const auto *b = f.cV((corner + 1) % 3);
                if (!a || !b)
                    continue;
                const std::uint32_t ia = std::uint32_t(vcg::tri::Index(meshData, a));
                const std::uint32_t ib = std::uint32_t(vcg::tri::Index(meshData, b));
                const std::uint64_t key = ia < ib
                    ? (std::uint64_t(ia) << 32) | ib
                    : (std::uint64_t(ib) << 32) | ia;
                if (!seenEdges.insert(key).second)
                    continue;
                pushSegment(a, b);
            }
        }

        for (const auto &e : meshData.edge) {
            if (e.IsD() || !e.IsS())
                continue;
            pushSegment(e.cV(0), e.cV(1));
        }

        // As quads, which the view widens to the selected edge width, and as plain lines,
        // which stand in where quads cannot be drawn -- the same pair a boundary decorator keeps.
        uploadLineBuffer(selectedEdgeLines, dst.selectedEdgesVbuf, dst.selectedEdgesVertexCount);
        uploadFatLineBuffer(selectedEdgeLines, dst.selectedEdgesFatVbuf, dst.selectedEdgesFatVertexCount);

        return true;
    };

    auto rebuildBBox = [&](CacheState::BBoxGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.vbuf.reset();
        dst.vertexCount = 0;

        if (meshData.bbox.IsNull())
            return true;

        const float mn[3] = {
            meshData.bbox.min[0], meshData.bbox.min[1], meshData.bbox.min[2] };
        const float mx[3] = {
            meshData.bbox.max[0], meshData.bbox.max[1], meshData.bbox.max[2] };
        // Holds both styles back to back; the draw picks the range it wants.
        const std::vector<float> bd =
            LineRenderer::buildBoundingBoxVertices(mn, mx, LineRenderer::kBoundingBoxBracketFraction);

        dst.vbuf.reset(
            rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(bd.size() * sizeof(float))));
        if (!dst.vbuf || !dst.vbuf->create()) {
            dst.vbuf.reset();
            return true;
        }
        ensureUpdates()->uploadStaticBuffer(dst.vbuf.get(), bd.data());
        dst.vertexCount = LineRenderer::kBoundingBoxVertexCount;
        return true;
    };

    auto rebuildDecoratorNormals = [&](CacheState::DecoratorNormalsGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.vertexNormals.clear();
        dst.faceNormals.clear();
        dst.curvatureDirPD1.clear();
        dst.curvatureDirPD2.clear();

        if (meshData.VN() <= 0)
            return true;

        const float diag = meshData.bbox.Diag();
        const float normalLength = std::max(1e-4f, diag * 0.02f);

        buildBatched(dst.vertexNormals, size_t(meshData.VN()), 6, 2, false,
                     [&](size_t vi, float *out) {
            const auto &v = meshData.vert[vi];
            const float nx = v.cN()[0];
            const float ny = v.cN()[1];
            const float nz = v.cN()[2];
            const bool finite =
                std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz);
            const float nLen2 = nx * nx + ny * ny + nz * nz;
            if (!finite || nLen2 < 1e-12f || nLen2 > 1e12f)
                return false;

            const float invLen = 1.0f / std::sqrt(nLen2);
            const float dx = nx * invLen * normalLength;
            const float dy = ny * invLen * normalLength;
            const float dz = nz * invLen * normalLength;
            const float px = v.cP()[0];
            const float py = v.cP()[1];
            const float pz = v.cP()[2];
            out[0] = px;
            out[1] = py;
            out[2] = pz;
            out[3] = px + dx;
            out[4] = py + dy;
            out[5] = pz + dz;
            return true;
        });

        buildBatched(dst.faceNormals, size_t(meshData.FN()), 6, 2, false,
                     [&](size_t fi, float *out) {
            const auto &f = meshData.face[fi];
            const auto *v0 = f.cV(0);
            const auto *v1 = f.cV(1);
            const auto *v2 = f.cV(2);
            const float cx = (v0->cP()[0] + v1->cP()[0] + v2->cP()[0]) / 3.0f;
            const float cy = (v0->cP()[1] + v1->cP()[1] + v2->cP()[1]) / 3.0f;
            const float cz = (v0->cP()[2] + v1->cP()[2] + v2->cP()[2]) / 3.0f;

            float nx = f.cN()[0];
            float ny = f.cN()[1];
            float nz = f.cN()[2];
            const float nLen2 = nx * nx + ny * ny + nz * nz;
            if (!(std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz))
                || nLen2 < 1e-12f || nLen2 > 1e12f) {
                const float e1x = v1->cP()[0] - v0->cP()[0];
                const float e1y = v1->cP()[1] - v0->cP()[1];
                const float e1z = v1->cP()[2] - v0->cP()[2];
                const float e2x = v2->cP()[0] - v0->cP()[0];
                const float e2y = v2->cP()[1] - v0->cP()[1];
                const float e2z = v2->cP()[2] - v0->cP()[2];
                nx = e1y * e2z - e1z * e2y;
                ny = e1z * e2x - e1x * e2z;
                nz = e1x * e2y - e1y * e2x;
            }
            const float finalLen2 = nx * nx + ny * ny + nz * nz;
            if (finalLen2 < 1e-12f || finalLen2 > 1e12f)
                return false;
            const float invLen = 1.0f / std::sqrt(finalLen2);
            const float dx = nx * invLen * normalLength;
            const float dy = ny * invLen * normalLength;
            const float dz = nz * invLen * normalLength;
            out[0] = cx;
            out[1] = cy;
            out[2] = cz;
            out[3] = cx + dx;
            out[4] = cy + dy;
            out[5] = cz + dz;
            return true;
        });

        // Curvature principal directions (PD1 = max, PD2 = min), drawn symmetrically.
        if (meshData.VN() > 0 && !meshData.vert.empty()
            && meshData.vert[0].IsCurvatureDirEnabled()) {
            // A segment along the direction, centered on the vertex.
            auto packDirection = [&](bool pd1) {
                return [&, pd1](size_t vi, float *out) {
                    const auto &v = meshData.vert[vi];
                    if (v.IsD())
                        return false;
                    const auto d = pd1 ? v.cPD1() : v.cPD2();
                    const float dLen2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    if (!(std::isfinite(d[0]) && std::isfinite(d[1]) && std::isfinite(d[2]))
                        || dLen2 <= 1e-12f || dLen2 >= 1e12f)
                        return false;
                    const float scale = normalLength / std::sqrt(dLen2);
                    for (int k = 0; k < 3; ++k) {
                        out[k] = v.cP()[k] - d[k] * scale;
                        out[3 + k] = v.cP()[k] + d[k] * scale;
                    }
                    return true;
                };
            };
            buildBatched(dst.curvatureDirPD1, size_t(meshData.VN()), 6, 2, false,
                         packDirection(true));
            buildBatched(dst.curvatureDirPD2, size_t(meshData.VN()), 6, 2, false,
                         packDirection(false));
        }
        return true;
    };

    auto rebuildDecoratorBoundaries = [&](CacheState::DecoratorBoundaryGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.boundaryEdgesVbuf.reset();
        dst.boundaryEdgesVertexCount = 0;
        dst.boundaryEdgesFatVbuf.reset();
        dst.boundaryEdgesFatVertexCount = 0;
        dst.textureSeamsVbuf.reset();
        dst.textureSeamsVertexCount = 0;
        dst.textureSeamsFatVbuf.reset();
        dst.textureSeamsFatVertexCount = 0;
        dst.nonManifoldEdgesVbuf.reset();
        dst.nonManifoldEdgesVertexCount = 0;
        dst.nonManifoldEdgesFatVbuf.reset();
        dst.nonManifoldEdgesFatVertexCount = 0;
        dst.nonManifoldVerticesVbuf.reset();
        dst.nonManifoldVerticesVertexCount = 0;

        if (meshData.VN() <= 0)
            return true;

        std::vector<float> boundaryEdgeLines;
        std::vector<float> textureSeamLines;
        std::vector<float> nonManifoldEdgeLines;
        boundaryEdgeLines.reserve(static_cast<size_t>(meshData.FN()) * 6);
        textureSeamLines.reserve(static_cast<size_t>(meshData.FN()) * 6);
        nonManifoldEdgeLines.reserve(static_cast<size_t>(meshData.FN()) * 6);

        const bool hasWedgeTex =
            (source.ioMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0
            && vcg::tri::HasPerWedgeTexCoord(meshData);
        const bool hasVertTex =
            !hasWedgeTex
            && (source.ioMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0
            && vcg::tri::HasPerVertexTexCoord(meshData);
        const bool hasTexCoords = hasWedgeTex || hasVertTex;

        struct EdgeUvSample {
            float u0 = 0.0f;
            float v0 = 0.0f;
            float u1 = 0.0f;
            float v1 = 0.0f;
            int tex0 = 0;
            int tex1 = 0;
        };
        struct EdgeAccum {
            std::array<float, 3> p0 { 0.0f, 0.0f, 0.0f };
            std::array<float, 3> p1 { 0.0f, 0.0f, 0.0f };
            // The vertices p0 and p1 came from, and the segments of later faces whose own
            // vertices differ: only a mesh with a vertexIdentity has any.
            std::array<int, 2> firstVertices { -1, -1 };
            std::vector<std::array<float, 6>> otherSides;
            int incidentCount = 0;
            // For the first two faces on the edge, their corners at p0 and at p1.
            std::array<std::array<int, 2>, 2> corners {};
            std::vector<EdgeUvSample> uvSamples;
        };

        auto makeEdgeKey = [](int a, int b) -> std::uint64_t {
            return (std::uint64_t(std::uint32_t(a)) << 32) | std::uint64_t(std::uint32_t(b));
        };
        auto readCornerUv = [&](const VCGFace &f, int corner, float &u, float &v, int &texId) -> bool {
            if (hasWedgeTex) {
                const auto &wt = f.cWT(corner);
                u = wt.U();
                v = wt.V();
                texId = int(wt.N());
                return std::isfinite(u) && std::isfinite(v);
            }
            if (hasVertTex) {
                const auto *vv = f.cV(corner);
                if (!vv)
                    return false;
                const auto &vt = vv->cT();
                u = vt.U();
                v = vt.V();
                texId = int(vt.N());
                return std::isfinite(u) && std::isfinite(v);
            }
            return false;
        };
        auto uvSampleDiffers = [](const EdgeUvSample &a, const EdgeUvSample &b) {
            constexpr float kUvEps = 1e-6f;
            auto diff = [](float x, float y) {
                return std::abs(x - y) > kUvEps;
            };
            return a.tex0 != b.tex0
                || a.tex1 != b.tex1
                || diff(a.u0, b.u0)
                || diff(a.v0, b.v0)
                || diff(a.u1, b.u1)
                || diff(a.v1, b.v1);
        };

        std::unordered_map<std::uint64_t, EdgeAccum> edges;
        edges.reserve(static_cast<size_t>(meshData.FN()) * 3);

        // Edges are keyed by vertex identity: the vertex index, or the source vertex a
        // UV-layout vertex stands for.
        const std::vector<int> *identity = source.vertexIdentity;
        const int keyCount = identity && !identity->empty()
            ? *std::max_element(identity->begin(), identity->end()) + 1
            : meshData.VN();
        const auto vertexKey = [&](const VCGVertex *v) {
            const int index = int(vcg::tri::Index(meshData, v));
            return identity ? (*identity)[size_t(index)] : index;
        };

        for (int fi = 0; fi < meshData.FN(); ++fi) {
            const auto &f = meshData.face[fi];
            if (f.IsD())
                continue;
            for (int i = 0; i < 3; ++i) {
                const int next = (i + 1) % 3;
                const auto *v0 = f.cV(i);
                const auto *v1 = f.cV(next);
                if (!v0 || !v1)
                    continue;

                int a = vertexKey(v0);
                int b = vertexKey(v1);
                if (a < 0 || b < 0 || a == b)
                    continue;

                bool swapped = false;
                std::array<float, 3> p0 = { v0->cP()[0], v0->cP()[1], v0->cP()[2] };
                std::array<float, 3> p1 = { v1->cP()[0], v1->cP()[1], v1->cP()[2] };
                std::array<int, 2> vertices = { int(vcg::tri::Index(meshData, v0)),
                                                int(vcg::tri::Index(meshData, v1)) };
                int c0 = 3 * fi + i;
                int c1 = 3 * fi + next;
                if (a > b) {
                    std::swap(a, b);
                    std::swap(p0, p1);
                    std::swap(vertices[0], vertices[1]);
                    std::swap(c0, c1);
                    swapped = true;
                }

                EdgeAccum &acc = edges[makeEdgeKey(a, b)];
                if (acc.incidentCount == 0) {
                    acc.p0 = p0;
                    acc.p1 = p1;
                    acc.firstVertices = vertices;
                } else if (vertices != acc.firstVertices) {
                    acc.otherSides.push_back({ p0[0], p0[1], p0[2], p1[0], p1[1], p1[2] });
                }
                if (acc.incidentCount < 2)
                    acc.corners[size_t(acc.incidentCount)] = { c0, c1 };
                ++acc.incidentCount;

                if (!hasTexCoords)
                    continue;

                float u0 = 0.0f;
                float v0uv = 0.0f;
                float u1 = 0.0f;
                float v1uv = 0.0f;
                int t0 = 0;
                int t1 = 0;
                if (!readCornerUv(f, i, u0, v0uv, t0) || !readCornerUv(f, next, u1, v1uv, t1))
                    continue;
                if (swapped) {
                    std::swap(u0, u1);
                    std::swap(v0uv, v1uv);
                    std::swap(t0, t1);
                }
                acc.uvSamples.push_back({ u0, v0uv, u1, v1uv, t0, t1 });
            }
        }

        // Non-manifold vertices are those whose faces make more than one fan -- two cones
        // touching at the tip -- and that lie on no non-manifold edge, which has its own
        // decorator: the definition of vcglib's CountNonManifoldVertexFF, which the info panel
        // counts with. That walks FF adjacency, which this const mesh does not carry, so the
        // fans are found from the edge map instead: the corners at each end of a manifold edge
        // are joined, and a vertex whose corners end up in more than one group has more than
        // one fan.
        std::vector<int> fan(static_cast<size_t>(meshData.FN()) * 3);
        std::iota(fan.begin(), fan.end(), 0);
        const auto fanOf = [&fan](int c) {
            while (fan[size_t(c)] != c) {
                fan[size_t(c)] = fan[size_t(fan[size_t(c)])];
                c = fan[size_t(c)];
            }
            return c;
        };
        std::vector<bool> onNonManifoldEdge(static_cast<size_t>(keyCount), false);
        const auto appendSides = [](std::vector<float> &lines, const EdgeAccum &acc) {
            lines.insert(lines.end(), { acc.p0[0], acc.p0[1], acc.p0[2], acc.p1[0], acc.p1[1], acc.p1[2] });
            for (const auto &side : acc.otherSides)
                lines.insert(lines.end(), side.begin(), side.end());
        };

        for (const auto &kv : edges) {
            const EdgeAccum &acc = kv.second;
            if (acc.incidentCount == 1)
                appendSides(boundaryEdgeLines, acc);

            if (acc.incidentCount == 2) {
                fan[size_t(fanOf(acc.corners[0][0]))] = fanOf(acc.corners[1][0]);
                fan[size_t(fanOf(acc.corners[0][1]))] = fanOf(acc.corners[1][1]);
            }

            // Non-manifold edges: more than 2 incident faces.
            if (acc.incidentCount > 2) {
                appendSides(nonManifoldEdgeLines, acc);
                onNonManifoldEdge[size_t(kv.first >> 32)] = true;
                onNonManifoldEdge[size_t(kv.first & 0xffffffffu)] = true;
            }

            if (!hasTexCoords)
                continue;

            bool isTextureBorder = false;
            if (acc.incidentCount == 1) {
                // Keep previous behavior: texture-border also includes geometric boundaries
                // when texture coordinates are available.
                isTextureBorder = !acc.uvSamples.empty();
            } else if (acc.uvSamples.size() < static_cast<size_t>(acc.incidentCount)) {
                // Missing/invalid UVs on at least one incident face -> treat as seam.
                isTextureBorder = true;
            } else if (!acc.uvSamples.empty()) {
                const EdgeUvSample &ref = acc.uvSamples.front();
                for (size_t si = 1; si < acc.uvSamples.size(); ++si) {
                    if (uvSampleDiffers(acc.uvSamples[si], ref)) {
                        isTextureBorder = true;
                        break;
                    }
                }
            }

            if (isTextureBorder)
                appendSides(textureSeamLines, acc);
        }

        std::vector<int> firstFan(static_cast<size_t>(keyCount), -1);
        std::vector<bool> manyFans(static_cast<size_t>(keyCount), false);
        std::vector<const VCGVertex *> keyVertex(static_cast<size_t>(keyCount), nullptr);
        for (int fi = 0; fi < meshData.FN(); ++fi) {
            const auto &f = meshData.face[fi];
            if (f.IsD())
                continue;
            for (int k = 0; k < 3; ++k) {
                if (!f.cV(k))
                    continue;
                const size_t vi = size_t(vertexKey(f.cV(k)));
                keyVertex[vi] = f.cV(k);
                const int group = fanOf(3 * fi + k);
                if (firstFan[vi] < 0)
                    firstFan[vi] = group;
                else if (firstFan[vi] != group)
                    manyFans[vi] = true;
            }
        }
        std::vector<float> nonManifoldVertexPoints;
        for (size_t vi = 0; vi < manyFans.size(); ++vi) {
            if (!manyFans[vi] || onNonManifoldEdge[vi])
                continue;
            const auto &p = keyVertex[vi]->cP();
            nonManifoldVertexPoints.insert(nonManifoldVertexPoints.end(), { p[0], p[1], p[2] });
        }

        uploadLineBuffer(
            boundaryEdgeLines, dst.boundaryEdgesVbuf, dst.boundaryEdgesVertexCount);
        uploadFatLineBuffer(
            boundaryEdgeLines, dst.boundaryEdgesFatVbuf, dst.boundaryEdgesFatVertexCount);
        uploadLineBuffer(
            textureSeamLines, dst.textureSeamsVbuf, dst.textureSeamsVertexCount);
        uploadFatLineBuffer(
            textureSeamLines, dst.textureSeamsFatVbuf, dst.textureSeamsFatVertexCount);
        uploadLineBuffer(
            nonManifoldEdgeLines, dst.nonManifoldEdgesVbuf, dst.nonManifoldEdgesVertexCount);
        uploadFatLineBuffer(
            nonManifoldEdgeLines, dst.nonManifoldEdgesFatVbuf, dst.nonManifoldEdgesFatVertexCount);
        uploadLineBuffer(
            nonManifoldVertexPoints, dst.nonManifoldVerticesVbuf, dst.nonManifoldVerticesVertexCount);
        return true;
    };

    if (needs.fill) {
        auto &fill = meshCache.fill[fillVariantIndex(fillVariant)];
        stats.rebuiltFill = rebuildFillVariant(fill, fillVariant);
    }
    if (needs.wire)
        stats.rebuiltWire = rebuildWire(meshCache.wire);
    if (needs.edges)
        stats.rebuiltEdges = rebuildEdges(meshCache.edges);
    if (needs.points) {
        auto &points = meshCache.points[pointVariantIndex(pointVariant)];
        stats.rebuiltPoints = rebuildPointsVariant(points, pointVariant);
    }
    // A variant of an older revision is rebuilt before it is drawn again, so its buffers
    // only hold memory: release them. One at the current revision stays, since another
    // view may be drawing the mesh with that color source.
    for (size_t i = 0; i < meshCache.fill.size(); ++i) {
        auto &fill = meshCache.fill[i];
        if (fill.valid && (fill.geometryRevision != source.geometryRevision
                           || fill.materialRevision != source.materialRevision
                           || fill.colorSourceRevision != colorSourceRevision(source, int(i), false)))
            fill = CacheState::FillVariantGpu{};
    }
    for (size_t i = 0; i < meshCache.points.size(); ++i) {
        auto &points = meshCache.points[i];
        if (points.valid && (points.geometryRevision != source.geometryRevision
                             || points.colorSourceRevision != colorSourceRevision(source, int(i), true)))
            points = CacheState::PointsVariantGpu{};
    }
    if (needs.boundingBox)
        stats.rebuiltBoundingBox = rebuildBBox(meshCache.bbox);
    if (needs.selection)
        stats.rebuiltSelection = rebuildSelection(meshCache.selection);
    if (needs.decoratorNormals)
        stats.rebuiltDecoratorNormals = rebuildDecoratorNormals(meshCache.decoratorNormals);
    if (needs.decoratorBoundaries)
        stats.rebuiltDecoratorBoundaries =
            rebuildDecoratorBoundaries(meshCache.decoratorBoundaries);

    if (updates)
        cb->resourceUpdate(updates);

    stats.elapsedMs = float(timer.nsecsElapsed() / 1000000.0);
    stats.bytes.meshId = source.meshId;
    addMeshGpuBytes(stats.bytes, meshCache);
    return stats;
}

MeshGpuResourceCache::FillPassView MeshGpuResourceCache::fillPassView(
    QRhi *rhi, std::uint64_t meshId, FillVariant variant) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const CacheState::FillVariantGpu &fill = meshIt->second.fill[fillVariantIndex(variant)];
    if (!fill.valid)
        return {};

    fill.viewBatches.clear();
    fill.viewBatches.reserve(fill.batches.size());
    for (const auto &batch : fill.batches) {
        const CacheState::FillBatchGpu *textureBatch = &batch;
        if (batch.textureGroupIndex >= 0
            && !batch.baseColorTexture
            && !batch.normalTexture
            && !batch.occlusionTexture
            && !batch.roughnessTexture) {
            const auto it = std::find_if(
                fill.batches.begin(),
                fill.batches.end(),
                [&](const CacheState::FillBatchGpu &candidate) {
                    return candidate.textureGroupIndex == batch.textureGroupIndex
                        && (candidate.baseColorTexture
                            || candidate.normalTexture
                            || candidate.occlusionTexture
                            || candidate.roughnessTexture);
                });
            if (it != fill.batches.end())
                textureBatch = &*it;
        }
        fill.viewBatches.push_back({
            batch.vbuf.get(),
            batch.ibuf.get(),
            textureBatch->baseColorTexture.get(),
            textureBatch->normalTexture.get(),
            textureBatch->occlusionTexture.get(),
            textureBatch->roughnessTexture.get(),
            batch.baseColorTexturePath,
            batch.normalTexturePath,
            batch.occlusionTexturePath,
            batch.roughnessTexturePath,
            batch.normalScale,
            batch.occlusionStrength,
            batch.roughnessFactor,
            batch.vertexCount,
            batch.indexCount,
            batch.textureGroupIndex
        });
    }

    FillPassView view;
    view.valid = true;
    view.batchCount = static_cast<int>(fill.viewBatches.size());
    view.batches = fill.viewBatches.empty() ? nullptr : fill.viewBatches.data();
    return view;
}

void MeshGpuResourceCache::BufferBatches::draw(QRhiCommandBuffer *cb, bool pointQuads) const
{
    for (const BufferBatchView &batch : *this) {
        const QRhiCommandBuffer::VertexInput binding(batch.vertexBuffer, 0);
        cb->setVertexInput(0, 1, &binding);
        if (pointQuads)
            cb->draw(4, quint32(batch.vertexCount));
        else
            cb->draw(quint32(batch.vertexCount));
    }
}

MeshGpuResourceCache::WirePassView MeshGpuResourceCache::wirePassView(QRhi *rhi, std::uint64_t meshId) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const auto &wire = meshIt->second.wire;
    if (!wire.valid)
        return {};

    return { wire.buffer.batches(), true };
}

MeshGpuResourceCache::EdgePassView MeshGpuResourceCache::edgePassView(
    QRhi *rhi, std::uint64_t meshId) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const auto &edges = meshIt->second.edges;
    if (!edges.valid)
        return {};

    return { edges.vbuf.get(), edges.vertexCount, true };
}

MeshGpuResourceCache::EdgeFatPassView MeshGpuResourceCache::edgeFatPassView(
    QRhi *rhi, std::uint64_t meshId) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const auto &edges = meshIt->second.edges;
    if (!edges.valid)
        return {};

    const bool hasFatBuffer =
        edges.fatVbuf && edges.fatVertexCount > 0;
    return { edges.fatVbuf.get(), edges.fatVertexCount, hasFatBuffer };
}

MeshGpuResourceCache::PointsPassView MeshGpuResourceCache::pointsPassView(
    QRhi *rhi, std::uint64_t meshId, PointVariant variant) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const auto &points = meshIt->second.points[pointVariantIndex(variant)];
    if (!points.valid)
        return {};

    return { points.buffer.batches(), true };
}

MeshGpuResourceCache::BBoxPassView MeshGpuResourceCache::bboxPassView(QRhi *rhi, std::uint64_t meshId) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const auto &bbox = meshIt->second.bbox;
    if (!bbox.valid)
        return {};

    return { bbox.vbuf.get(), bbox.vertexCount, true };
}

MeshGpuResourceCache::SelectionPassView MeshGpuResourceCache::selectionPassView(
    QRhi *rhi, std::uint64_t meshId) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const auto &selection = meshIt->second.selection;
    if (!selection.valid)
        return {};

    SelectionPassView view;
    view.valid = true;
    view.selectedFaces = selection.selectedFaces.batches();
    view.selectedVertices = selection.selectedVertices.batches();
    view.selectedEdgesBuffer = selection.selectedEdgesVbuf.get();
    view.selectedEdgesVertexCount = selection.selectedEdgesVertexCount;
    view.selectedEdgesFatBuffer = selection.selectedEdgesFatVbuf.get();
    view.selectedEdgesFatVertexCount = selection.selectedEdgesFatVertexCount;
    return view;
}

MeshGpuResourceCache::DecoratorPassView MeshGpuResourceCache::decoratorPassView(
    QRhi *rhi, std::uint64_t meshId) const
{
    if (!m_state || !rhi || meshId == 0)
        return {};

    const auto rhiIt = m_state->byRhi.find(rhi);
    if (rhiIt == m_state->byRhi.end())
        return {};
    const auto meshIt = rhiIt->second.find(meshId);
    if (meshIt == rhiIt->second.end())
        return {};

    const auto &normalDecor = meshIt->second.decoratorNormals;
    const auto &boundaryDecor = meshIt->second.decoratorBoundaries;
    if (!normalDecor.valid && !boundaryDecor.valid)
        return {};

    DecoratorPassView view;
    view.valid = true;
    view.vertexNormals = normalDecor.vertexNormals.batches();
    view.faceNormals = normalDecor.faceNormals.batches();
    view.curvatureDirPD1 = normalDecor.curvatureDirPD1.batches();
    view.curvatureDirPD2 = normalDecor.curvatureDirPD2.batches();
    view.boundaryEdgesBuffer = boundaryDecor.boundaryEdgesVbuf.get();
    view.boundaryEdgesVertexCount = boundaryDecor.boundaryEdgesVertexCount;
    view.boundaryEdgesFatBuffer = boundaryDecor.boundaryEdgesFatVbuf.get();
    view.boundaryEdgesFatVertexCount = boundaryDecor.boundaryEdgesFatVertexCount;
    view.textureSeamsBuffer = boundaryDecor.textureSeamsVbuf.get();
    view.textureSeamsVertexCount = boundaryDecor.textureSeamsVertexCount;
    view.textureSeamsFatBuffer = boundaryDecor.textureSeamsFatVbuf.get();
    view.textureSeamsFatVertexCount = boundaryDecor.textureSeamsFatVertexCount;
    view.nonManifoldEdgesBuffer = boundaryDecor.nonManifoldEdgesVbuf.get();
    view.nonManifoldEdgesVertexCount = boundaryDecor.nonManifoldEdgesVertexCount;
    view.nonManifoldEdgesFatBuffer = boundaryDecor.nonManifoldEdgesFatVbuf.get();
    view.nonManifoldEdgesFatVertexCount = boundaryDecor.nonManifoldEdgesFatVertexCount;
    view.nonManifoldVerticesBuffer = boundaryDecor.nonManifoldVerticesVbuf.get();
    view.nonManifoldVerticesVertexCount = boundaryDecor.nonManifoldVerticesVertexCount;
    return view;
}

void MeshGpuResourceCache::purgeMesh(std::uint64_t meshId)
{
    if (!m_state || meshId == 0)
        return;
    for (auto &bucket : m_state->byRhi)
        bucket.second.erase(meshId);
}

void MeshGpuResourceCache::releaseRhiResources(QRhi *rhi)
{
    if (!m_state || !rhi)
        return;
    m_state->byRhi.erase(rhi);
}

std::vector<MeshGpuResourceCache::GpuMeshMemoryStats> MeshGpuResourceCache::gpuMemoryStats() const
{
    std::map<std::uint64_t, GpuMeshMemoryStats> byMesh;
    for (const auto &[rhi, byId] : m_state->byRhi) {
        for (const auto &[meshId, meshGpu] : byId) {
            auto &s = byMesh[meshId];
            s.meshId = meshId;
            addMeshGpuBytes(s, meshGpu);
        }
    }

    std::vector<GpuMeshMemoryStats> result;
    result.reserve(byMesh.size());
    for (auto &[id, s] : byMesh)
        result.push_back(s);
    return result;
}
