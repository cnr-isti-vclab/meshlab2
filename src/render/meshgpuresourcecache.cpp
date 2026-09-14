#include "meshgpuresourcecache.h"
#include "linerenderer.h"
#include "textureassociationutils.h"
#include "meshioplugin.h"
#include "qualityrange.h"

#include <wrap/io_trimesh/io_mask.h>
#include <rhi/qrhi.h>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QSemaphore>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {
constexpr int kFillVertexStrideFloats = 13;
constexpr int kPointsVertexStrideFloats = 11;
constexpr size_t kMaxExpandedFillBatchBytes = 256u * 1024u * 1024u;
constexpr size_t kExpandedFillTriangleBytes =
    3u * size_t(kFillVertexStrideFloats) * sizeof(float);
constexpr size_t kMaxExpandedFillBatchFaces =
    kMaxExpandedFillBatchBytes / kExpandedFillTriangleBytes;
static_assert(kMaxExpandedFillBatchFaces > 0);
static_assert(kMaxExpandedFillBatchBytes <= std::numeric_limits<quint32>::max());

template <typename Function>
void parallelFor(size_t count, Function &&function)
{
    constexpr size_t kGrainSize = 64u * 1024u;
    const size_t chunkCount = count / kGrainSize + (count % kGrainSize != 0);
    QThreadPool *pool = QThreadPool::globalInstance();
    const int workerCount = int(std::min(
        chunkCount, size_t(std::max(1, pool->maxThreadCount()))));
    if (workerCount <= 1) {
        function(0, count);
        return;
    }

    std::atomic_size_t next { 0 };
    QSemaphore finished;
    auto work = [&] {
        for (;;) {
            const size_t first = next.fetch_add(kGrainSize, std::memory_order_relaxed);
            if (first >= count)
                return;
            function(first, first + std::min(kGrainSize, count - first));
        }
    };

    for (int i = 1; i < workerCount; ++i)
        pool->start([&] { work(); finished.release(); });
    work();
    finished.acquire(workerCount - 1);
}

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
}

struct MeshGpuResourceCache::CacheState
{
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
        bool qualityFixedRange = false;
        float qualityRangeMin = 0.0f;
        float qualityRangeMax = 1.0f;
        bool qualityCenterOnZero = false;
        float qualityPercentileCrop = 0.0f;
        bool valid = false;
        std::vector<FillBatchGpu> batches;
        mutable std::vector<FillBatchView> viewBatches;
    };

    struct WireGpu {
        std::uint64_t geometryRevision = 0;
        bool wireRespectFaux = true;
        bool valid = false;
        std::unique_ptr<QRhiBuffer> vbuf;
        int vertexCount = 0;
    };

    struct EdgeGpu {
        std::uint64_t geometryRevision = 0;
        bool valid = false;
        std::unique_ptr<QRhiBuffer> vbuf;
        int vertexCount = 0;
        std::unique_ptr<QRhiBuffer> fatVbuf;
        int fatVertexCount = 0;
    };

    struct PointsVariantGpu {
        std::uint64_t geometryRevision = 0;
        bool qualityFixedRange = false;
        float qualityRangeMin = 0.0f;
        float qualityRangeMax = 1.0f;
        bool qualityCenterOnZero = false;
        float qualityPercentileCrop = 0.0f;
        bool valid = false;
        std::unique_ptr<QRhiBuffer> vbuf;
        int vertexCount = 0;
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
        std::unique_ptr<QRhiBuffer> selectedFacesVbuf;
        int selectedFacesVertexCount = 0;
        std::unique_ptr<QRhiBuffer> selectedVerticesVbuf;
        int selectedVerticesVertexCount = 0;
    };

    struct DecoratorNormalsGpu {
        std::uint64_t geometryRevision = 0;
        bool valid = false;
        std::unique_ptr<QRhiBuffer> vertexNormalsVbuf;
        int vertexNormalsVertexCount = 0;
        std::unique_ptr<QRhiBuffer> faceNormalsVbuf;
        int faceNormalsVertexCount = 0;
        std::unique_ptr<QRhiBuffer> curvatureDirPD1Vbuf;
        int curvatureDirPD1VertexCount = 0;
        std::unique_ptr<QRhiBuffer> curvatureDirPD2Vbuf;
        int curvatureDirPD2VertexCount = 0;
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

MeshGpuResourceCache::EnsureStats MeshGpuResourceCache::ensureMeshResources(
    QRhi *rhi,
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
    bool needDecoratorBoundaries)
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
            const bool qualityVariant =
                isPerVertexQualityFillVariant(variant)
                || isPerFaceQualityFillVariant(variant);
            float requestedRangeMin = source.qualityRangeMin;
            float requestedRangeMax = source.qualityRangeMax;
            if (requestedRangeMin > requestedRangeMax)
                std::swap(requestedRangeMin, requestedRangeMax);
            const bool requestedRangeFinite =
                std::isfinite(requestedRangeMin) && std::isfinite(requestedRangeMax);
            const bool useFixedRange = qualityVariant && source.qualityFixedRange && requestedRangeFinite;
            const bool useCenterOnZero = qualityVariant && !useFixedRange && source.qualityCenterOnZero;
            const float requestedPercentileCrop = qualityVariant && !useFixedRange
                ? std::clamp(source.qualityPercentileCrop, 0.0f, 0.5f)
                : 0.0f;

            if (dst.valid
                && dst.geometryRevision == source.geometryRevision
                && dst.materialRevision == source.materialRevision) {
                if (!qualityVariant)
                    return false;
                const bool sameFixedRange =
                    dst.qualityFixedRange == useFixedRange
                    && (!useFixedRange
                        || (dst.qualityRangeMin == requestedRangeMin
                            && dst.qualityRangeMax == requestedRangeMax));
                const bool sameAutoRange =
                    useFixedRange
                    || (dst.qualityCenterOnZero == useCenterOnZero
                        && dst.qualityPercentileCrop == requestedPercentileCrop);
                if (sameFixedRange && sameAutoRange) {
                    return false;
                }
            }

            dst.valid = true;
            dst.geometryRevision = source.geometryRevision;
            dst.materialRevision = source.materialRevision;
            dst.qualityFixedRange = useFixedRange;
            dst.qualityRangeMin = requestedRangeMin;
            dst.qualityRangeMax = requestedRangeMax;
            dst.qualityCenterOnZero = useCenterOnZero;
            dst.qualityPercentileCrop = requestedPercentileCrop;
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

            auto collectVertexQuality = [&]() {
                std::vector<float> values;
                values.reserve(size_t(meshData.VN()));
                for (int vi = 0; vi < meshData.VN(); ++vi) {
                    const auto &v = meshData.vert[vi];
                    if (!v.IsD())
                        values.push_back(static_cast<float>(v.cQ()));
                }
                return values;
            };
            auto collectFaceQuality = [&]() {
                std::vector<float> values;
                values.reserve(size_t(meshData.FN()));
                for (int fi = 0; fi < meshData.FN(); ++fi) {
                    const auto &f = meshData.face[fi];
                    if (!f.IsD())
                        values.push_back(static_cast<float>(f.cQ()));
                }
                return values;
            };

            RenderQualityRange vertexQualityRange;
            if (useVertexQuality) {
                if (useFixedRange) {
                    vertexQualityRange = fixedRenderQualityRange(requestedRangeMin, requestedRangeMax);
                } else {
                    vertexQualityRange = sampledRenderQualityRange(
                        collectVertexQuality(),
                        useCenterOnZero,
                        requestedPercentileCrop);
                }
            }

            RenderQualityRange faceQualityRange;
            if (useFaceQuality) {
                if (useFixedRange) {
                    faceQualityRange = fixedRenderQualityRange(requestedRangeMin, requestedRangeMax);
                } else {
                    faceQualityRange = sampledRenderQualityRange(
                        collectFaceQuality(),
                        useCenterOnZero,
                        requestedPercentileCrop);
                }
            }

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
                        std::vector<float> &groupData,
                        size_t startBase,
                        bool useTextureForFace) {
                        QVector3D faceRgb(1.0f, 1.0f, 1.0f);
                        float faceQualityT = 0.5f;
                        if (useFaceColor) {
                            const auto fc = f.cC();
                            faceRgb = QVector3D(
                                static_cast<float>(fc[0]) / 255.0f,
                                static_cast<float>(fc[1]) / 255.0f,
                                static_cast<float>(fc[2]) / 255.0f);
                        } else if (useFaceQuality) {
                            faceQualityT = normalizedRenderQuality(
                                static_cast<float>(f.cQ()), faceQualityRange);
                        }

                        for (int corner = 0; corner < 3; ++corner) {
                            const auto *vertex = f.cV(corner);
                            QVector3D vertexRgb(1.0f, 1.0f, 1.0f);
                            float vertexQualityT = 0.5f;
                            if (useVertexColor) {
                                const auto vc = vertex->cC();
                                vertexRgb = QVector3D(
                                    static_cast<float>(vc[0]) / 255.0f,
                                    static_cast<float>(vc[1]) / 255.0f,
                                    static_cast<float>(vc[2]) / 255.0f);
                            } else if (useVertexQuality) {
                                vertexQualityT = normalizedRenderQuality(
                                    static_cast<float>(vertex->cQ()), vertexQualityRange);
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
                                groupData[base + 6] = faceQualityT;
                                groupData[base + 7] = 0.0f;
                                groupData[base + 8] = 0.0f;
                                groupData[base + 9] = -1.0f; // quality LUT lookup
                            } else if (useVertexQuality) {
                                groupData[base + 6] = vertexQualityT;
                                groupData[base + 7] = 0.0f;
                                groupData[base + 8] = 0.0f;
                                groupData[base + 9] = -1.0f; // quality LUT lookup
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
                    std::map<int, std::vector<float>> groupedTriangles;

                    if (!useTextureColor && compactFaces) {
                        std::vector<float> &groupData = groupedTriangles[-1];
                        groupData.resize(
                            (lastFace - firstFace) * 3u * size_t(kFillVertexStrideFloats));
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

                            if (useTextureColor) {
                                if (ensureGroupPrepared(textureIndex)) {
                                    textureGroup = textureIndex;
                                    useTextureForFace = true;
                                } else if (ensureGroupPrepared(0)) {
                                    textureGroup = 0;
                                    useTextureForFace = true;
                                }
                            }

                            std::vector<float> &groupData = groupedTriangles[textureGroup];
                            const size_t startBase = groupData.size();
                            groupData.resize(
                                groupData.size() + 3u * size_t(kFillVertexStrideFloats));
                            packExpandedFace(
                                f, groupData, startBase, useTextureForFace);
                        }
                    }

                    for (auto &groupEntry : groupedTriangles) {
                        if (groupEntry.second.empty())
                            continue;

                        const size_t bufferBytes = groupEntry.second.size() * sizeof(float);
                        CacheState::FillBatchGpu batch;
                        batch.textureGroupIndex = groupEntry.first;
                        batch.vertexCount =
                            static_cast<int>(groupEntry.second.size() / kFillVertexStrideFloats);
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
                            batch.vbuf.get(), groupEntry.second.data());

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
                CacheState::FillBatchGpu batch;
                const int vertexCount = meshData.VN();
                const int indexCount = meshData.FN() * 3;
                if (vertexCount <= 0 || indexCount <= 0)
                    return true;

                std::vector<float> vdata(
                    size_t(vertexCount) * size_t(kFillVertexStrideFloats));
                const float useMeshColor = useVertexStyleColor ? 1.0f : 0.0f;
                parallelFor(size_t(vertexCount), [&](size_t first, size_t last) {
                    for (size_t vi = first; vi < last; ++vi) {
                        const auto &v = meshData.vert[vi];
                        QVector3D vertexRgb(1.0f, 1.0f, 1.0f);
                        float vertexQualityT = 0.5f;
                        if (useVertexColor) {
                            const auto vc = v.cC();
                            vertexRgb = QVector3D(
                                static_cast<float>(vc[0]) / 255.0f,
                                static_cast<float>(vc[1]) / 255.0f,
                                static_cast<float>(vc[2]) / 255.0f);
                        } else if (useVertexQuality) {
                            const float vq = static_cast<float>(v.cQ());
                            vertexQualityT = normalizedRenderQuality(vq, vertexQualityRange);
                        }
                        const size_t base = vi * size_t(kFillVertexStrideFloats);
                        vdata[base + 0] = v.cP()[0];
                        vdata[base + 1] = v.cP()[1];
                        vdata[base + 2] = v.cP()[2];
                        vdata[base + 3] = v.cN()[0];
                        vdata[base + 4] = v.cN()[1];
                        vdata[base + 5] = v.cN()[2];
                        if (useVertexQuality) {
                            vdata[base + 6] = vertexQualityT;
                            vdata[base + 7] = 0.0f;
                            vdata[base + 8] = 0.0f;
                            vdata[base + 9] = -1.0f; // quality LUT lookup
                        } else {
                            vdata[base + 6] = vertexRgb.x();
                            vdata[base + 7] = vertexRgb.y();
                            vdata[base + 8] = vertexRgb.z();
                            vdata[base + 9] = useMeshColor;
                        }
                        vdata[base + 10] = 0.0f;
                        vdata[base + 11] = 0.0f;
                        vdata[base + 12] = 0.0f;
                    }
                });

                std::vector<quint32> idata(static_cast<size_t>(indexCount));
                parallelFor(size_t(meshData.FN()), [&](size_t first, size_t last) {
                    for (size_t fi = first; fi < last; ++fi) {
                        const auto &f = meshData.face[fi];
                        idata[fi * 3 + 0] =
                            static_cast<quint32>(vcg::tri::Index(meshData, f.cV(0)));
                        idata[fi * 3 + 1] =
                            static_cast<quint32>(vcg::tri::Index(meshData, f.cV(1)));
                        idata[fi * 3 + 2] =
                            static_cast<quint32>(vcg::tri::Index(meshData, f.cV(2)));
                    }
                });

                batch.vbuf.reset(
                    rhi->newBuffer(
                        QRhiBuffer::Immutable,
                        QRhiBuffer::VertexBuffer,
                        static_cast<quint32>(vdata.size() * sizeof(float))));
                batch.ibuf.reset(
                    rhi->newBuffer(
                        QRhiBuffer::Immutable,
                        QRhiBuffer::IndexBuffer,
                        static_cast<quint32>(idata.size() * sizeof(quint32))));
                    if (!batch.vbuf || !batch.ibuf || !batch.vbuf->create() || !batch.ibuf->create()) {
                        batch.vbuf.reset();
                        batch.ibuf.reset();
                        return true;
                    }

                ensureUpdates()->uploadStaticBuffer(batch.vbuf.get(), vdata.data());
                ensureUpdates()->uploadStaticBuffer(batch.ibuf.get(), idata.data());
                batch.vertexCount = vertexCount;
                batch.indexCount = indexCount;
                dst.batches.push_back(std::move(batch));
            }
            return true;
        };

    auto rebuildWire = [&](CacheState::WireGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision
            && dst.wireRespectFaux == source.wireRespectFaux)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.wireRespectFaux = source.wireRespectFaux;
        dst.vbuf.reset();
        dst.vertexCount = 0;

        if (meshData.FN() <= 0)
            return true;

        const int vertexCount = meshData.FN() * 3;
        std::vector<float> vdata(size_t(vertexCount) * 6u);
        auto packFace = [&](const VCGFace &f, size_t outFi) {
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
                const size_t base = (outFi * 3u + size_t(corner)) * 6u;
                vdata[base + 0] = vertex->cP()[0];
                vdata[base + 1] = vertex->cP()[1];
                vdata[base + 2] = vertex->cP()[2];
                vdata[base + 3] = bary[corner][0];
                vdata[base + 4] = bary[corner][1];
                vdata[base + 5] = bary[corner][2];
            }
        };

        if (meshData.face.size() == size_t(meshData.FN())) {
            parallelFor(meshData.face.size(), [&](size_t first, size_t last) {
                for (size_t fi = first; fi < last; ++fi)
                    packFace(meshData.face[fi], fi);
            });
        } else {
            size_t outFi = 0;
            for (const auto &f : meshData.face) {
                if (!f.IsD())
                    packFace(f, outFi++);
            }
        }

        dst.vbuf.reset(
            rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(vdata.size() * sizeof(float))));
        if (!dst.vbuf || !dst.vbuf->create()) {
            dst.vbuf.reset();
            return true;
        }

        ensureUpdates()->uploadStaticBuffer(dst.vbuf.get(), vdata.data());
        dst.vertexCount = vertexCount;
        return true;
    };

    auto rebuildPointsVariant =
        [&](CacheState::PointsVariantGpu &dst, PointVariant variant) -> bool {
            const bool qualityVariant = (variant == PointVariant::PerVertexQuality);
            float requestedRangeMin = source.qualityRangeMin;
            float requestedRangeMax = source.qualityRangeMax;
            if (requestedRangeMin > requestedRangeMax)
                std::swap(requestedRangeMin, requestedRangeMax);
            const bool requestedRangeFinite =
                std::isfinite(requestedRangeMin) && std::isfinite(requestedRangeMax);
            const bool useFixedRange = qualityVariant && source.qualityFixedRange && requestedRangeFinite;
            const bool useCenterOnZero = qualityVariant && !useFixedRange && source.qualityCenterOnZero;
            const float requestedPercentileCrop = qualityVariant && !useFixedRange
                ? std::clamp(source.qualityPercentileCrop, 0.0f, 0.5f)
                : 0.0f;

            if (dst.valid && dst.geometryRevision == source.geometryRevision) {
                if (!qualityVariant)
                    return false;
                const bool sameFixedRange =
                    dst.qualityFixedRange == useFixedRange
                    && (!useFixedRange
                        || (dst.qualityRangeMin == requestedRangeMin
                            && dst.qualityRangeMax == requestedRangeMax));
                const bool sameAutoRange =
                    useFixedRange
                    || (dst.qualityCenterOnZero == useCenterOnZero
                        && dst.qualityPercentileCrop == requestedPercentileCrop);
                if (sameFixedRange && sameAutoRange) {
                    return false;
                }
            }

            dst.valid = true;
            dst.geometryRevision = source.geometryRevision;
            dst.qualityFixedRange = useFixedRange;
            dst.qualityRangeMin = requestedRangeMin;
            dst.qualityRangeMax = requestedRangeMax;
            dst.qualityCenterOnZero = useCenterOnZero;
            dst.qualityPercentileCrop = requestedPercentileCrop;
            dst.vbuf.reset();
            dst.vertexCount = 0;

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

            RenderQualityRange vertexQualityRange;
            if (useVertexQuality) {
                if (useFixedRange) {
                    vertexQualityRange = fixedRenderQualityRange(requestedRangeMin, requestedRangeMax);
                } else {
                    std::vector<float> values;
                    values.reserve(size_t(meshData.VN()));
                    for (int vi = 0; vi < meshData.VN(); ++vi) {
                        const auto &v = meshData.vert[vi];
                        if (!v.IsD())
                            values.push_back(static_cast<float>(v.cQ()));
                    }
                    vertexQualityRange = sampledRenderQualityRange(
                        std::move(values),
                        useCenterOnZero,
                        requestedPercentileCrop);
                }
            }

            std::vector<float> pdata(
                size_t(meshData.VN()) * size_t(kPointsVertexStrideFloats));
            parallelFor(size_t(meshData.VN()), [&](size_t first, size_t last) {
                for (size_t vi = first; vi < last; ++vi) {
                    const auto &v = meshData.vert[vi];
                    QVector3D vertexRgb(1.0f, 1.0f, 1.0f);
                    float vertexQualityT = 0.5f;
                    if (useVertexColor) {
                        const auto vc = v.cC();
                        vertexRgb = QVector3D(
                            static_cast<float>(vc[0]) / 255.0f,
                            static_cast<float>(vc[1]) / 255.0f,
                            static_cast<float>(vc[2]) / 255.0f);
                    } else if (useVertexQuality) {
                        const float vq = static_cast<float>(v.cQ());
                        vertexQualityT = normalizedRenderQuality(vq, vertexQualityRange);
                    }
                    const size_t base = vi * size_t(kPointsVertexStrideFloats);
                    pdata[base + 0] = v.cP()[0];
                    pdata[base + 1] = v.cP()[1];
                    pdata[base + 2] = v.cP()[2];
                    if (useVertexQuality) {
                        pdata[base + 3] = vertexQualityT;
                        pdata[base + 4] = 0.0f;
                        pdata[base + 5] = 0.0f;
                        pdata[base + 6] = -1.0f; // quality LUT lookup
                    } else {
                        pdata[base + 3] = vertexRgb.x();
                        pdata[base + 4] = vertexRgb.y();
                        pdata[base + 5] = vertexRgb.z();
                        pdata[base + 6] = useMeshColor;
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
                    pdata[base + 7] = hasUsableNormal ? nx : 0.0f;
                    pdata[base + 8] = hasUsableNormal ? ny : 0.0f;
                    pdata[base + 9] = hasUsableNormal ? nz : 1.0f;
                    pdata[base + 10] = hasUsableNormal ? 1.0f : 0.0f;
                }
            });

            dst.vbuf.reset(
                rhi->newBuffer(
                    QRhiBuffer::Immutable,
                    QRhiBuffer::VertexBuffer,
                    static_cast<quint32>(pdata.size() * sizeof(float))));
            if (!dst.vbuf || !dst.vbuf->create()) {
                dst.vbuf.reset();
                return true;
            }
            ensureUpdates()->uploadStaticBuffer(dst.vbuf.get(), pdata.data());
            dst.vertexCount = meshData.VN();
            return true;
        };

    auto rebuildEdges = [&](CacheState::EdgeGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.vbuf.reset();
        dst.vertexCount = 0;
        dst.fatVbuf.reset();
        dst.fatVertexCount = 0;

        if (meshData.EN() <= 0)
            return true;

        std::vector<float> vdata;
        std::vector<float> fatVdata;
        vdata.reserve(static_cast<size_t>(meshData.EN()) * 6);
        fatVdata.reserve(static_cast<size_t>(meshData.EN()) * 6 * LineRenderer::kFatLineStrideFloats);
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
            vdata.push_back(p0[0]);
            vdata.push_back(p0[1]);
            vdata.push_back(p0[2]);
            vdata.push_back(p1[0]);
            vdata.push_back(p1[1]);
            vdata.push_back(p1[2]);
            LineRenderer::appendFatLineSegmentVertices(
                fatVdata, p0[0], p0[1], p0[2], p1[0], p1[1], p1[2]);
        }

        if (vdata.empty())
            return true;

        dst.vbuf.reset(
            rhi->newBuffer(
                QRhiBuffer::Immutable,
                QRhiBuffer::VertexBuffer,
                static_cast<quint32>(vdata.size() * sizeof(float))));
        if (!dst.vbuf || !dst.vbuf->create()) {
            dst.vbuf.reset();
            return true;
        }

        ensureUpdates()->uploadStaticBuffer(dst.vbuf.get(), vdata.data());
        dst.vertexCount = static_cast<int>(vdata.size() / 3);

        if (!fatVdata.empty()) {
            dst.fatVbuf.reset(
                rhi->newBuffer(
                    QRhiBuffer::Immutable,
                    QRhiBuffer::VertexBuffer,
                    static_cast<quint32>(fatVdata.size() * sizeof(float))));
            if (!dst.fatVbuf || !dst.fatVbuf->create()) {
                dst.fatVbuf.reset();
            } else {
                ensureUpdates()->uploadStaticBuffer(dst.fatVbuf.get(), fatVdata.data());
                dst.fatVertexCount =
                    static_cast<int>(fatVdata.size() / LineRenderer::kFatLineStrideFloats);
            }
        }
        return true;
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
        dst.selectedFacesVbuf.reset();
        dst.selectedFacesVertexCount = 0;
        dst.selectedVerticesVbuf.reset();
        dst.selectedVerticesVertexCount = 0;

        if (meshData.VN() <= 0)
            return true;

        std::vector<float> selectedFaceTriangles;
        selectedFaceTriangles.reserve(static_cast<size_t>(meshData.FN()) * 9);
        for (int fi = 0; fi < meshData.FN(); ++fi) {
            const auto &f = meshData.face[fi];
            if (f.IsD() || !f.IsS())
                continue;
            for (int corner = 0; corner < 3; ++corner) {
                const auto *v = f.cV(corner);
                if (!v)
                    continue;
                selectedFaceTriangles.push_back(v->cP()[0]);
                selectedFaceTriangles.push_back(v->cP()[1]);
                selectedFaceTriangles.push_back(v->cP()[2]);
            }
        }

        if (!selectedFaceTriangles.empty()) {
            dst.selectedFacesVbuf.reset(
                rhi->newBuffer(
                    QRhiBuffer::Immutable,
                    QRhiBuffer::VertexBuffer,
                    static_cast<quint32>(selectedFaceTriangles.size() * sizeof(float))));
            if (!dst.selectedFacesVbuf || !dst.selectedFacesVbuf->create()) {
                dst.selectedFacesVbuf.reset();
            } else {
                ensureUpdates()->uploadStaticBuffer(
                    dst.selectedFacesVbuf.get(), selectedFaceTriangles.data());
                dst.selectedFacesVertexCount =
                    static_cast<int>(selectedFaceTriangles.size() / 3);
            }
        }

        std::vector<float> selectedVertices;
        selectedVertices.reserve(static_cast<size_t>(meshData.VN()) * 3);
        for (int vi = 0; vi < meshData.VN(); ++vi) {
            const auto &v = meshData.vert[vi];
            if (v.IsD() || !v.IsS())
                continue;
            selectedVertices.push_back(v.cP()[0]);
            selectedVertices.push_back(v.cP()[1]);
            selectedVertices.push_back(v.cP()[2]);
        }

        if (!selectedVertices.empty()) {
            dst.selectedVerticesVbuf.reset(
                rhi->newBuffer(
                    QRhiBuffer::Immutable,
                    QRhiBuffer::VertexBuffer,
                    static_cast<quint32>(selectedVertices.size() * sizeof(float))));
            if (!dst.selectedVerticesVbuf || !dst.selectedVerticesVbuf->create()) {
                dst.selectedVerticesVbuf.reset();
            } else {
                ensureUpdates()->uploadStaticBuffer(
                    dst.selectedVerticesVbuf.get(), selectedVertices.data());
                dst.selectedVerticesVertexCount =
                    static_cast<int>(selectedVertices.size() / 3);
            }
        }

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

    auto rebuildDecoratorNormals = [&](CacheState::DecoratorNormalsGpu &dst) -> bool {
        if (dst.valid && dst.geometryRevision == source.geometryRevision)
            return false;

        dst.valid = true;
        dst.geometryRevision = source.geometryRevision;
        dst.vertexNormalsVbuf.reset();
        dst.vertexNormalsVertexCount = 0;
        dst.faceNormalsVbuf.reset();
        dst.faceNormalsVertexCount = 0;
        dst.curvatureDirPD1Vbuf.reset();
        dst.curvatureDirPD1VertexCount = 0;
        dst.curvatureDirPD2Vbuf.reset();
        dst.curvatureDirPD2VertexCount = 0;

        if (meshData.VN() <= 0)
            return true;

        const float diag = meshData.bbox.Diag();
        const float normalLength = std::max(1e-4f, diag * 0.02f);

        std::vector<float> vertexNormalLines;
        vertexNormalLines.reserve(static_cast<size_t>(meshData.VN()) * 6);
        for (int vi = 0; vi < meshData.VN(); ++vi) {
            const auto &v = meshData.vert[vi];
            const float nx = v.cN()[0];
            const float ny = v.cN()[1];
            const float nz = v.cN()[2];
            const bool finite =
                std::isfinite(nx) && std::isfinite(ny) && std::isfinite(nz);
            const float nLen2 = nx * nx + ny * ny + nz * nz;
            if (!finite || nLen2 < 1e-12f || nLen2 > 1e12f)
                continue;

            const float invLen = 1.0f / std::sqrt(nLen2);
            const float dx = nx * invLen * normalLength;
            const float dy = ny * invLen * normalLength;
            const float dz = nz * invLen * normalLength;
            const float px = v.cP()[0];
            const float py = v.cP()[1];
            const float pz = v.cP()[2];
            vertexNormalLines.push_back(px);
            vertexNormalLines.push_back(py);
            vertexNormalLines.push_back(pz);
            vertexNormalLines.push_back(px + dx);
            vertexNormalLines.push_back(py + dy);
            vertexNormalLines.push_back(pz + dz);
        }
        uploadLineBuffer(
            vertexNormalLines, dst.vertexNormalsVbuf, dst.vertexNormalsVertexCount);

        std::vector<float> faceNormalLines;
        faceNormalLines.reserve(static_cast<size_t>(meshData.FN()) * 6);
        for (int fi = 0; fi < meshData.FN(); ++fi) {
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
                continue;
            const float invLen = 1.0f / std::sqrt(finalLen2);
            const float dx = nx * invLen * normalLength;
            const float dy = ny * invLen * normalLength;
            const float dz = nz * invLen * normalLength;
            faceNormalLines.push_back(cx);
            faceNormalLines.push_back(cy);
            faceNormalLines.push_back(cz);
            faceNormalLines.push_back(cx + dx);
            faceNormalLines.push_back(cy + dy);
            faceNormalLines.push_back(cz + dz);
        }
        uploadLineBuffer(faceNormalLines, dst.faceNormalsVbuf, dst.faceNormalsVertexCount);

        // Curvature principal directions (PD1 = max, PD2 = min), drawn symmetrically.
        if (meshData.VN() > 0 && !meshData.vert.empty()
            && meshData.vert[0].IsCurvatureDirEnabled()) {
            std::vector<float> pd1Lines, pd2Lines;
            pd1Lines.reserve(static_cast<size_t>(meshData.VN()) * 6);
            pd2Lines.reserve(static_cast<size_t>(meshData.VN()) * 6);
            for (int vi = 0; vi < meshData.VN(); ++vi) {
                const auto &v = meshData.vert[vi];
                if (v.IsD())
                    continue;
                const float px = v.cP()[0];
                const float py = v.cP()[1];
                const float pz = v.cP()[2];
                // PD1 (max curvature direction)
                const float d1x = v.cPD1()[0];
                const float d1y = v.cPD1()[1];
                const float d1z = v.cPD1()[2];
                const float d1Len2 = d1x*d1x + d1y*d1y + d1z*d1z;
                if (std::isfinite(d1x) && std::isfinite(d1y) && std::isfinite(d1z)
                    && d1Len2 > 1e-12f && d1Len2 < 1e12f) {
                    const float inv = 1.0f / std::sqrt(d1Len2);
                    const float ex = d1x * inv * normalLength;
                    const float ey = d1y * inv * normalLength;
                    const float ez = d1z * inv * normalLength;
                    pd1Lines.push_back(px - ex); pd1Lines.push_back(py - ey); pd1Lines.push_back(pz - ez);
                    pd1Lines.push_back(px + ex); pd1Lines.push_back(py + ey); pd1Lines.push_back(pz + ez);
                }
                // PD2 (min curvature direction)
                const float d2x = v.cPD2()[0];
                const float d2y = v.cPD2()[1];
                const float d2z = v.cPD2()[2];
                const float d2Len2 = d2x*d2x + d2y*d2y + d2z*d2z;
                if (std::isfinite(d2x) && std::isfinite(d2y) && std::isfinite(d2z)
                    && d2Len2 > 1e-12f && d2Len2 < 1e12f) {
                    const float inv = 1.0f / std::sqrt(d2Len2);
                    const float ex = d2x * inv * normalLength;
                    const float ey = d2y * inv * normalLength;
                    const float ez = d2z * inv * normalLength;
                    pd2Lines.push_back(px - ex); pd2Lines.push_back(py - ey); pd2Lines.push_back(pz - ez);
                    pd2Lines.push_back(px + ex); pd2Lines.push_back(py + ey); pd2Lines.push_back(pz + ez);
                }
            }
            uploadLineBuffer(pd1Lines, dst.curvatureDirPD1Vbuf, dst.curvatureDirPD1VertexCount);
            uploadLineBuffer(pd2Lines, dst.curvatureDirPD2Vbuf, dst.curvatureDirPD2VertexCount);
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
            int incidentCount = 0;
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

                int a = vcg::tri::Index(meshData, v0);
                int b = vcg::tri::Index(meshData, v1);
                if (a < 0 || b < 0 || a == b)
                    continue;

                bool swapped = false;
                std::array<float, 3> p0 = { v0->cP()[0], v0->cP()[1], v0->cP()[2] };
                std::array<float, 3> p1 = { v1->cP()[0], v1->cP()[1], v1->cP()[2] };
                if (a > b) {
                    std::swap(a, b);
                    std::swap(p0, p1);
                    swapped = true;
                }

                EdgeAccum &acc = edges[makeEdgeKey(a, b)];
                if (acc.incidentCount == 0) {
                    acc.p0 = p0;
                    acc.p1 = p1;
                }
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

        // Track unique non-manifold vertex positions (to avoid duplicates in the point buffer).
        std::unordered_set<std::uint64_t> nonManifoldVertexSet;
        std::vector<float> nonManifoldVertexPoints;

        for (const auto &kv : edges) {
            const EdgeAccum &acc = kv.second;
            if (acc.incidentCount == 1) {
                boundaryEdgeLines.push_back(acc.p0[0]);
                boundaryEdgeLines.push_back(acc.p0[1]);
                boundaryEdgeLines.push_back(acc.p0[2]);
                boundaryEdgeLines.push_back(acc.p1[0]);
                boundaryEdgeLines.push_back(acc.p1[1]);
                boundaryEdgeLines.push_back(acc.p1[2]);
            }

            // Non-manifold edges: more than 2 incident faces.
            if (acc.incidentCount > 2) {
                nonManifoldEdgeLines.push_back(acc.p0[0]);
                nonManifoldEdgeLines.push_back(acc.p0[1]);
                nonManifoldEdgeLines.push_back(acc.p0[2]);
                nonManifoldEdgeLines.push_back(acc.p1[0]);
                nonManifoldEdgeLines.push_back(acc.p1[1]);
                nonManifoldEdgeLines.push_back(acc.p1[2]);
                // Record the two endpoint positions as non-manifold vertices.
                auto addNonManifoldVertex = [&](const std::array<float, 3> &p) {
                    // Build a dedup key from the raw float bits.
                    std::uint32_t bx, by, bz;
                    std::memcpy(&bx, &p[0], sizeof(bx));
                    std::memcpy(&by, &p[1], sizeof(by));
                    std::memcpy(&bz, &p[2], sizeof(bz));
                    const std::uint64_t key =
                        (std::uint64_t(bx) << 42) ^ (std::uint64_t(by) << 21) ^ std::uint64_t(bz);
                    if (nonManifoldVertexSet.insert(key).second) {
                        nonManifoldVertexPoints.push_back(p[0]);
                        nonManifoldVertexPoints.push_back(p[1]);
                        nonManifoldVertexPoints.push_back(p[2]);
                    }
                };
                addNonManifoldVertex(acc.p0);
                addNonManifoldVertex(acc.p1);
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

            if (!isTextureBorder)
                continue;

            textureSeamLines.push_back(acc.p0[0]);
            textureSeamLines.push_back(acc.p0[1]);
            textureSeamLines.push_back(acc.p0[2]);
            textureSeamLines.push_back(acc.p1[0]);
            textureSeamLines.push_back(acc.p1[1]);
            textureSeamLines.push_back(acc.p1[2]);
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

    if (needFill) {
        auto &fill = meshCache.fill[fillVariantIndex(fillVariant)];
        stats.rebuiltFill = rebuildFillVariant(fill, fillVariant);
    }
    if (needWire)
        stats.rebuiltWire = rebuildWire(meshCache.wire);
    if (needEdges)
        stats.rebuiltEdges = rebuildEdges(meshCache.edges);
    if (needPoints) {
        auto &points = meshCache.points[pointVariantIndex(pointVariant)];
        stats.rebuiltPoints = rebuildPointsVariant(points, pointVariant);
    }
    if (needBoundingBox)
        stats.rebuiltBoundingBox = rebuildBBox(meshCache.bbox);
    if (needSelection)
        stats.rebuiltSelection = rebuildSelection(meshCache.selection);
    if (needDecoratorNormals)
        stats.rebuiltDecoratorNormals = rebuildDecoratorNormals(meshCache.decoratorNormals);
    if (needDecoratorBoundaries)
        stats.rebuiltDecoratorBoundaries =
            rebuildDecoratorBoundaries(meshCache.decoratorBoundaries);

    if (updates) {
        stats.uploadedResources = true;
        cb->resourceUpdate(updates);
    }

    stats.elapsedMs = float(timer.nsecsElapsed() / 1000000.0);
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

    return { wire.vbuf.get(), wire.vertexCount, true };
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

    return { points.vbuf.get(), points.vertexCount, true };
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
    view.selectedFacesBuffer = selection.selectedFacesVbuf.get();
    view.selectedFacesVertexCount = selection.selectedFacesVertexCount;
    view.selectedVerticesBuffer = selection.selectedVerticesVbuf.get();
    view.selectedVerticesVertexCount = selection.selectedVerticesVertexCount;
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
    view.vertexNormalsBuffer = normalDecor.vertexNormalsVbuf.get();
    view.vertexNormalsVertexCount = normalDecor.vertexNormalsVertexCount;
    view.faceNormalsBuffer = normalDecor.faceNormalsVbuf.get();
    view.faceNormalsVertexCount = normalDecor.faceNormalsVertexCount;
    view.curvatureDirPD1Buffer = normalDecor.curvatureDirPD1Vbuf.get();
    view.curvatureDirPD1VertexCount = normalDecor.curvatureDirPD1VertexCount;
    view.curvatureDirPD2Buffer = normalDecor.curvatureDirPD2Vbuf.get();
    view.curvatureDirPD2VertexCount = normalDecor.curvatureDirPD2VertexCount;
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

void MeshGpuResourceCache::clearAll()
{
    if (!m_state)
        return;
    m_state->byRhi.clear();
}

std::vector<MeshGpuResourceCache::GpuMeshMemoryStats> MeshGpuResourceCache::gpuMemoryStats() const
{
    std::map<std::uint64_t, GpuMeshMemoryStats> byMesh;

    auto bufBytes = [](const std::unique_ptr<QRhiBuffer> &buf) -> qint64 {
        return buf ? qint64(buf->size()) : 0LL;
    };
    auto texBytes = [](const std::unique_ptr<QRhiTexture> &tex) -> qint64 {
        if (!tex)
            return 0LL;
        const QSize sz = tex->pixelSize();
        return qint64(sz.width()) * sz.height() * 4; // RGBA8 = 4 bytes/pixel, 1 mip level
    };

    for (const auto &[rhi, byId] : m_state->byRhi) {
        for (const auto &[meshId, meshGpu] : byId) {
            auto &s = byMesh[meshId];
            s.meshId = meshId;

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
                s.wireBufferBytes += bufBytes(meshGpu.wire.vbuf);
            if (meshGpu.edges.valid)
                s.edgeBufferBytes += bufBytes(meshGpu.edges.vbuf) + bufBytes(meshGpu.edges.fatVbuf);
            for (const auto &pv : meshGpu.points) {
                if (pv.valid)
                    s.pointsBufferBytes += bufBytes(pv.vbuf);
            }
            if (meshGpu.bbox.valid)
                s.bboxBufferBytes += bufBytes(meshGpu.bbox.vbuf);
            if (meshGpu.selection.valid)
                s.selectionBufferBytes += bufBytes(meshGpu.selection.selectedFacesVbuf)
                                        + bufBytes(meshGpu.selection.selectedVerticesVbuf);
            if (meshGpu.decoratorNormals.valid)
                s.decoratorBufferBytes += bufBytes(meshGpu.decoratorNormals.vertexNormalsVbuf)
                                        + bufBytes(meshGpu.decoratorNormals.faceNormalsVbuf)
                                        + bufBytes(meshGpu.decoratorNormals.curvatureDirPD1Vbuf)
                                        + bufBytes(meshGpu.decoratorNormals.curvatureDirPD2Vbuf);
            if (meshGpu.decoratorBoundaries.valid)
                s.decoratorBufferBytes +=
                    bufBytes(meshGpu.decoratorBoundaries.boundaryEdgesVbuf)
                    + bufBytes(meshGpu.decoratorBoundaries.boundaryEdgesFatVbuf)
                    + bufBytes(meshGpu.decoratorBoundaries.textureSeamsVbuf)
                    + bufBytes(meshGpu.decoratorBoundaries.textureSeamsFatVbuf)
                    + bufBytes(meshGpu.decoratorBoundaries.nonManifoldEdgesVbuf)
                    + bufBytes(meshGpu.decoratorBoundaries.nonManifoldEdgesFatVbuf)
                    + bufBytes(meshGpu.decoratorBoundaries.nonManifoldVerticesVbuf);
        }
    }

    std::vector<GpuMeshMemoryStats> result;
    result.reserve(byMesh.size());
    for (auto &[id, s] : byMesh)
        result.push_back(s);
    return result;
}
