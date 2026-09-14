#pragma once

#include "camerashot.h"
#include "document_undo_types.h"
#include "meshfilterplugin.h"
#include "meshioplugin.h"
#include "meshgpuresourcecache.h"
#include "rasterplane.h"
#include "layerdata.h"
#include "vcgmesh.h"
#include "viewstate.h"
#include <QObject>
#include <QElapsedTimer>
#include <QImage>
#include <QMatrix4x4>
#include <QSize>
#include <cstdint>
#include <functional>
#include <QString>
#include <QStringList>
#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class MeshIOPluginManager;
class MeshFilterPluginManager;
class DocumentUndoManager;
class QRhi;
class QRhiCommandBuffer;

class Document : public QObject
{
    Q_OBJECT
public:
    // Who is speaking. Orthogonal to LogLevel: the source answers "where did this come
    // from", the level answers "does the user want to hear it".
    enum class LogSource {
        Application,
        VCG
    };

    // Severity, ordered from loudest to quietest so a verbosity threshold is a plain
    // comparison: an entry is shown when level <= threshold.
    //   Error   the operation failed and the user must know
    //   Warning it went ahead, but degraded or partially
    //   Info    the normal narration of what happened (the default)
    //   Debug   timings, counters, mask dumps, cache and GPU bookkeeping
    enum class LogLevel {
        Error,
        Warning,
        Info,
        Debug
    };

    struct LogEntry {
        QString message;
        LogSource source = LogSource::Application;
        LogLevel level = LogLevel::Info;
        // Wall-clock stamp of when the entry was written. Kept out of the message text so
        // the view decides whether and how to render it; see
        // applicationStartMSecsSinceEpoch() for showing it as an offset from startup.
        qint64 epochMs = 0;
    };

    struct MeshEntry {
        std::uint64_t meshId = 0;
        std::uint64_t geometryRevision = 0;
        std::uint64_t materialRevision = 0;
        // Bumped on selection-bit changes only, so the GPU selection overlay can
        // rebuild without invalidating the (far larger) fill/wire/point buffers.
        std::uint64_t selectionRevision = 0;
        QMatrix4x4 transform;
        QString name;
        QString sourcePath;
        QStringList textureFileNames;
        QStringList textureFilePaths;
        std::vector<MeshIOTextureAsset> textureAssets;
        MeshIOMaterialSet materialSet;
        bool visible = true;
        bool modified = false;
        int ioMask = 0;
        // Logical face count for faux-edge polygon meshes; -1 for triangle meshes.
        int polygonFaceCount = -1;
        // Plugin-owned intermediates keyed by owner (see setLayerData). Immutable and
        // shared, so copying this map is O(entries) and never touches the payloads.
        std::map<QString, LayerDataPtr> pluginData;
        VCGMesh mesh;
    };

    struct RasterEntry {
        std::uint64_t rasterId = 0;
        std::uint64_t imageRevision = 0;
        std::uint64_t cameraRevision = 0;
        QString name;
        QString sourcePath;
        bool visible = true;
        CameraShot shot;
        std::vector<RasterPlane> planes;
        int currentPlaneIndex = -1;

        RasterPlane *currentPlane()
        {
            return (currentPlaneIndex >= 0 && currentPlaneIndex < int(planes.size()))
                ? &planes[size_t(currentPlaneIndex)]
                : nullptr;
        }

        const RasterPlane *currentPlane() const
        {
            return (currentPlaneIndex >= 0 && currentPlaneIndex < int(planes.size()))
                ? &planes[size_t(currentPlaneIndex)]
                : nullptr;
        }
    };

    struct ImportPluginInfo {
        QString id;
        QString name;
        QStringList extensions;
        QHash<QString, MeshIOCapabilities> capabilities;
    };

    struct ExportPluginInfo {
        QString id;
        QString name;
        QStringList extensions;
        QHash<QString, MeshIOCapabilities> capabilities;
    };

    struct FilterInfo {
        QString key;
        QString pluginId;
        QString pluginName;
        MeshFilterDescriptor descriptor;
        bool applicable = true;
        QString applicabilityError;
    };

    struct CpuMeshMemoryStats {
        std::uint64_t meshId = 0;
        int meshIndex = -1;
        QString name;
        int vertexCapacity = 0;
        int edgeCapacity = 0;
        int faceCapacity = 0;
        qint64 vertexBytes = 0;
        qint64 vertexOcfBytes = 0;
        qint64 edgeBytes = 0;
        qint64 faceBytes = 0;
        qint64 faceOcfBytes = 0;
        qint64 customAttributeBytes = 0;
        // Whatever the plugins attached to this layer say they occupy. Self-reported, so
        // a plugin that does not implement approximateBytes() simply contributes nothing.
        qint64 pluginDataBytes = 0;
        qint64 totalBytes() const
        {
            return vertexBytes + vertexOcfBytes + edgeBytes + faceBytes + faceOcfBytes
                + customAttributeBytes + pluginDataBytes;
        }
    };

    struct CpuImageMemoryStats {
        int uniqueMeshTextureImages = 0;
        int uniqueRasterImages = 0;
        qint64 meshTextureBytes = 0;
        qint64 rasterImageBytes = 0;
        qint64 totalBytes() const { return meshTextureBytes + rasterImageBytes; }
    };

    using FillGpuVariant = MeshGpuResourceCache::FillVariant;
    using PointGpuVariant = MeshGpuResourceCache::PointVariant;
    using FillBatchGpuView = MeshGpuResourceCache::FillBatchView;
    using FillPassGpuView = MeshGpuResourceCache::FillPassView;
    using WirePassGpuView = MeshGpuResourceCache::WirePassView;
    using EdgePassGpuView = MeshGpuResourceCache::EdgePassView;
    using EdgeFatPassGpuView = MeshGpuResourceCache::EdgeFatPassView;
    using PointsPassGpuView = MeshGpuResourceCache::PointsPassView;
    using BBoxPassGpuView = MeshGpuResourceCache::BBoxPassView;
    using SelectionPassGpuView = MeshGpuResourceCache::SelectionPassView;
    using DecoratorPassGpuView = MeshGpuResourceCache::DecoratorPassView;

    explicit Document(QObject *parent = nullptr);
    ~Document() override;

    // errorMessage, when given, receives why the load failed -- the importer's own
    // diagnosis, which the return code alone cannot carry. Callers that open several files
    // at once need it: a per-file status-bar message is overwritten by the next file.
    int loadMesh(const QString &filename, QString *errorMessage = nullptr);
    int reloadMesh(int index);
    int saveMesh(int index, const QString &filename, const MeshIOSaveOptions &options);
    int saveMesh(int index, const QString &filename);
    int saveCurrentMesh(const QString &filename, const MeshIOSaveOptions &options);
    int saveCurrentMesh(const QString &filename);
    void beginUndoStep(const QString &label);
    void beginUndoStep(const QString &label,
                       const ScriptAction &scriptAction);
    void beginUndoStep(const QString &label,
                       int meshIndexForSelectionDelta);
    // Delta-storage step that also records a ScriptAction (reproducible selection
    // filters): cheap bit-packed undo for a selection-only change on one mesh.
    void beginUndoStep(const QString &label,
                       const ScriptAction &scriptAction,
                       int meshIndexForSelectionDelta);
    void endUndoStep(bool commit = true, bool restoreOnCancel = false);
    void setViewStateFunctions(std::function<ViewState()> capture,
                                std::function<void(const ViewState &, bool restoreCamera)> restore);
    void restoreViewState(const ViewState &state, bool restoreCamera);
    void setRenderStateSnapshotFunction(
        std::function<bool(const QString &, const QSize &, QImage &, CameraShot &, QString &)> capture);
    bool renderSnapshotFromStateJson(
        const QString &renderStateJson,
        const QSize &pixelSize,
        QImage &outImage,
        CameraShot &outShot,
        QString *errorMessage = nullptr) const;
    bool canUndo() const;
    bool canRedo() const;
    QString undoText() const;
    QString redoText() const;
    bool isRestoringUndoRedo() const;
    // True while an undo step opened by an outer operation is still running. Nested
    // operations test this (with isRestoringUndoRedo()) to decide whether they own the
    // step, so that a caller bracketing several of them collapses into one undo entry.
    bool undoStepActive() const;
    QStringList undoHistoryLabels() const;
    QStringList undoStackLabels() const;
    int undoCursorPosition() const;
    std::vector<UndoTreeNodeInfo> undoTreeInfo() const;
    std::vector<ScriptAction> undoNodeScriptActions(int nodeId) const;
    // The single action that produced a node's state, if it had one. Distinct from
    // undoNodeScriptActions(), which also returns the informational filter calls recorded
    // around it — those did not produce the state and must not be replayed as if they had.
    std::optional<ScriptAction> undoNodeAction(int nodeId) const;
    void recordScriptAction(const ScriptAction &scriptAction);
    int undoCurrentNodeId() const;
    bool jumpToUndoNode(int nodeId, bool restoreCamera = true);
    bool updateUndoNodeCamera(int nodeId);
    bool makeUndoRoot(int nodeId);
    bool purgeUndoBranch(int nodeId);
    bool linearizeUndoHistory();
    bool undo();
    bool redo();
    void clearUndoHistory();
    void clearAllLayers();
    int undoLimit() const;
    void setUndoLimit(int limit);
    qint64 undoMemoryLimitBytes() const;
    void setUndoMemoryLimitBytes(qint64 bytes);
    UndoPruneResult pruneUndoToMemoryBudget(qint64 maximumBytes);
    void handleUndoMemoryPressure(bool critical);
    void setSuppressUndo(bool s);
    int addMesh(const VCGMesh &mesh, const QString &name = {}, int ioMask = 0);
    void removeMesh(int index);
    int duplicateMesh(int sourceIndex, const QString &newName = {});
    int loadRasterImage(const QString &filename);
    int loadMeshLabProject(const QString &filename);

    struct MeshLabProjectSaveOptions {
        bool onlyVisibleMeshes = false;
        bool saveModifiedMeshes = true;
        bool copyFiles = false;  // copy external files into project dir when saving to a new location
    };
    bool saveMeshLabProject(const QString &filename,
                            const MeshLabProjectSaveOptions &options,
                            QString *error = nullptr);
    int addRaster(const RasterEntry &raster);
    int addRasterImage(
        const QImage &image,
        const QString &name = {},
        const QString &sourcePath = {},
        const CameraShot &shot = CameraShot());
    void removeRaster(int index);
    void setMeshVisible(int index, bool visible);
    void setMeshName(int index, const QString &name);
    void setRasterVisible(int index, bool visible);
    void setRasterName(int index, const QString &name);
    void setRasterShot(int index, const CameraShot &shot, const QString &contextMessage = {});
    void setCurrentRasterIndex(int index);
    void setCurrentRasterPlaneIndex(int rasterIndex, int planeIndex);
    static void ensureRasterPlaneImage(RasterPlane &plane);
    void markRasterImageChanged(int index, const QString &contextMessage = {});
    QMatrix4x4 meshTransform(int index) const;
    void setMeshTransform(
        int index,
        const QMatrix4x4 &transform,
        const QString &contextMessage = {});
    void setCurrentMeshIndex(int index);
    void markMeshGeometryChanged(int index, const QString &contextMessage = {});
    // Recount an existing polygon mesh; fauxEdgesModified also reconciles its polygon flag.
    void refreshMeshPolygonFaceCount(int index, bool fauxEdgesModified = false);
    void markMeshMaterialChanged(int index, const QString &contextMessage = {});
    // Selection is stored in per-vertex/per-face BitFlags, which are captured by the
    // undo geometry snapshot.  Changes to selection must therefore bump geometryRevision
    // so the undo cache produces a fresh deep-copy for the "after" checkpoint.
    void markMeshSelectionChanged(int index, const QString &contextMessage = {});
    SelectionDelta captureSelectionDelta(int meshIndex) const;
    void applySelectionDelta(const SelectionDelta &delta);
    // Wall-clock stamp taken when MeshLab2Core loaded, so a view can render an entry's
    // time as an elapsed offset from application start instead of a bare clock reading.
    static qint64 applicationStartMSecsSinceEpoch();

    // ---- Plugin-owned layer data -------------------------------------------------
    // A plugin attaches an intermediate result to a layer under a key it namespaces with
    // its own pluginId ("<pluginId>/<what>"); Core rejects anything else, because a
    // collision between two plugins would otherwise be silent. The value is immutable, so
    // undo captures and restores it by sharing the pointer and never copies the payload.
    //
    // Dropped automatically when the layer's geometry changes, unless the data says it
    // survives that. Not written to project files.
    void setLayerData(int meshIndex, const QString &ownerKey, LayerDataPtr data);
    LayerDataPtr layerData(int meshIndex, const QString &ownerKey) const;
    void clearLayerData(int meshIndex, const QString &ownerKey);

    void clearLog();
    void writeLog(
        const QString &message,
        LogSource source = LogSource::Application,
        LogLevel level = LogLevel::Info,
        bool replaceLast = false);

    // Progress is written as a single transient entry that is always the last line,
    // overwritten in place as the operation advances, and removed when it ends — a
    // running operation should leave no trace in the log once it is done. Cleared
    // automatically at both ends of a filter run and around a load; any ordinary
    // writeLog also drops it first, so the transient line never gets stranded
    // above a real message.
    void clearProgressLog();

    int meshCount() const { return static_cast<int>(m_meshes.size()); }
    MeshEntry &mesh(int i) { return *m_meshes[i]; }
    const MeshEntry &mesh(int i) const { return *m_meshes[i]; }
    int rasterCount() const { return static_cast<int>(m_rasters.size()); }
    RasterEntry &raster(int i) { return *m_rasters[i]; }
    const RasterEntry &raster(int i) const { return *m_rasters[i]; }
    static int meshTextureAssociationCount(const MeshEntry &entry);
    static bool hasMeshTextureAssociation(const MeshEntry &entry);
    static QString meshTextureDisplayName(const MeshEntry &entry, int textureIndex);
    static QString meshTextureSourcePath(const MeshEntry &entry, int textureIndex);
    static const MeshIOTextureAsset *meshTextureAsset(const MeshEntry &entry, int textureIndex);
    static QString rasterPlaneDisplayName(const RasterPlane &plane, int planeIndex = 0);
    static QString rasterPlaneSourcePath(const RasterPlane &plane);
    int currentMeshIndex() const { return m_currentMeshIndex; }
    int currentRasterIndex() const { return m_currentRasterIndex; }
    CurrentLayerKind currentLayerKind() const { return m_currentLayerKind; }
    const std::vector<LogEntry> &logMessages() const { return m_logMessages; }
    QString openDialogFilter() const;
    QString saveDialogFilter() const;
    int saveMaskCapability(const QString &filename) const;
    QStringList loadedPluginSummaries() const;
    QStringList loadedFilterPluginSummaries() const;
    std::vector<ImportPluginInfo> importPluginInfos() const;
    std::vector<ExportPluginInfo> exportPluginInfos() const;
    std::vector<FilterInfo> filterInfos() const;
    bool validateFilterInvocation(
        const QString &filterKey,
        const MeshFilterParameterValues &parameters,
        QString &errorMessage) const;
    MeshFilterRunResult runFilter(
        const QString &filterKey,
        const MeshFilterParameterValues &parameters = {});
    // Outcome of applying one filter to every visible layer in turn.
    struct MultiMeshFilterResult
    {
        struct SkippedLayer
        {
            int meshIndex = -1;
            QString layerName;
            QString reason;
        };
        bool success = false;          // at least one layer was applied
        bool documentModified = false;
        int appliedCount = 0;
        int targetCount = 0;           // visible layers the sweep set out to cover
        QVector<int> newMeshIndices;
        QVector<SkippedLayer> skipped;
        QString errorMessage;          // set only when the sweep could not start at all
    };
    // Applies filterKey to every currently visible mesh. The whole sweep is one undo
    // step: the user performed a single action, so a single undo takes it back, and a
    // layer the filter fails on cannot be stranded in a half-modified state that only
    // its own neighbours can be undone around.
    MultiMeshFilterResult runFilterOnVisibleMeshes(
        const QString &filterKey,
        const MeshFilterParameterValues &parameters = {});
    vcg::CallBackPos *progressCallback();
    void beginFilterProgress(const QString &label);
    void finishFilterProgress(bool success, const QString &message);
    void requestOperationCancel();
    bool isOperationCancelRequested() const;
    QStringList importSupportedExtensions() const;
    QStringList exportSupportedExtensions() const;
    QString preferredImportPluginForExtension(const QString &extension) const;
    void setPreferredImportPluginForExtension(const QString &extension, const QString &pluginId);
    void ensureMeshGpuResources(QRhi *rhi,
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
                                bool qualityFixedRange = false,
                                float qualityRangeMin = 0.0f,
                                float qualityRangeMax = 1.0f,
                                bool needSelection = false,
                                bool wireRespectFaux = true,
                                bool qualityCenterOnZero = false,
                                float qualityPercentileCrop = 0.0f);
    FillPassGpuView fillPassGpuView(QRhi *rhi, int meshIndex, FillGpuVariant variant) const;
    WirePassGpuView wirePassGpuView(QRhi *rhi, int meshIndex) const;
    EdgePassGpuView edgePassGpuView(QRhi *rhi, int meshIndex) const;
    EdgeFatPassGpuView edgeFatPassGpuView(QRhi *rhi, int meshIndex) const;
    PointsPassGpuView pointsPassGpuView(QRhi *rhi, int meshIndex, PointGpuVariant variant) const;
    BBoxPassGpuView bboxPassGpuView(QRhi *rhi, int meshIndex) const;
    SelectionPassGpuView selectionPassGpuView(QRhi *rhi, int meshIndex) const;
    DecoratorPassGpuView decoratorPassGpuView(QRhi *rhi, int meshIndex) const;
    void releaseRhiGpuResources(QRhi *rhi);
    void clearAllGpuResources();

    std::vector<CpuMeshMemoryStats> cpuMeshMemoryStats() const;
    CpuImageMemoryStats cpuImageMemoryStats() const;
    UndoMemoryStats undoMemoryStats() const;
    std::vector<MeshGpuResourceCache::GpuMeshMemoryStats> gpuMemoryStats() const;

signals:
    void meshAdded(int index);
    void meshRemoved(int index);
    void meshVisibilityChanged(int index, bool visible);
    void currentMeshChanged(int index);
    void meshDataChanged(int index);
    // Selection-bit-only change. Distinct from meshDataChanged so consumers that
    // don't care about selection (e.g. the filter menu/panel, whose applicability
    // never depends on selection) can ignore it — a selection change must stay
    // cheap even on huge meshes.
    void meshSelectionChanged(int index);
    void currentLayerChanged(CurrentLayerKind kind, int index);
    void rasterAdded(int index);
    void rasterRemoved(int index);
    void rasterVisibilityChanged(int index, bool visible);
    void currentRasterChanged(int index);
    void rasterDataChanged(int index);
    void loadProgressStarted(const QString &filePath);
    void loadProgressUpdated(int percent, const QString &message);
    void loadProgressFinished(bool success, const QString &message);
    void filterProgressStarted(const QString &label);
    void filterProgressUpdated(int percent, const QString &message);
    void filterProgressFinished(bool success, const QString &message);
    void logCleared();
    void logMessageAdded(
        const QString &message,
        Document::LogSource source,
        Document::LogLevel level,
        bool replaceLast);
    // The last entry was removed; see clearProgressLog(). The index is passed so a view
    // that filters entries out can tell whether it ever displayed the one being removed.
    void logLastEntryRemoved(int index);
    // A restore (undo, redo, jump-to-node, or a rolled-back step) has finished. Views that
    // suppress their per-mesh refreshes while isRestoringUndoRedo() is true — every signal
    // the restore emits is emitted with that flag still set — rebuild once from here.
    void undoRestoreCompleted();
    void undoRedoStateChanged(
        bool canUndo,
        bool canRedo,
        const QString &undoText,
        const QString &redoText);

public:
    // Undo state capture/restore — used by DocumentUndoManager.
    // Made public to avoid a friend declaration.
    UndoState captureUndoState() const;
    void restoreUndoState(const UndoState &state);

private:
    enum class CallbackMode {
        None,
        Load,
        Filter,
        Save
    };
    vcg::CallBackPos *logCallback();
    bool handleLogCallback(int pos, const char *message);
    void appendOrReplaceLog(const QString &message, LogSource source, LogLevel level, bool replaceLast);
    void writeProgressLog(const QString &message);
    static bool dispatchLogCallback(int pos, const char *message);
    void purgeMeshGpuResources(std::uint64_t meshId);

    std::unique_ptr<MeshIOPluginManager> m_pluginManager;
    std::unique_ptr<MeshFilterPluginManager> m_filterPluginManager;
    std::unique_ptr<MeshGpuResourceCache> m_gpuCache;
    std::vector<std::unique_ptr<MeshEntry>> m_meshes;
    std::vector<std::unique_ptr<RasterEntry>> m_rasters;
    std::uint64_t m_nextMeshId = 1;
    std::uint64_t m_nextRasterId = 1;
    // Globally monotonic counter for geometry revisions. Never reset or restored
    // during undo/redo, so every distinct geometry snapshot always gets a unique
    // (meshId, geometryRevision) key — preventing cross-branch cache collisions.
    std::uint64_t m_nextGeometryRevision = 1;
    void setCurrentMeshIndexInternal(int index, bool makeCurrentLayer);
    void setCurrentRasterIndexInternal(int index, bool makeCurrentLayer);
    int m_currentMeshIndex = -1;
    int m_currentRasterIndex = -1;
    CurrentLayerKind m_currentLayerKind = CurrentLayerKind::None;
    std::vector<LogEntry> m_logMessages;
    // True while the transient progress line is present, with its index in
    // m_logMessages so it is never confused with a real entry.
    bool m_progressLogActive = false;
    std::size_t m_progressLogIndex = 0;
    int m_lastProgressPos = -1;
    QElapsedTimer m_loadCallbackTimer;
    int m_loadCallbackCount = 0;
    int m_loadProgressEmitCount = 0;
    int m_loadProcessEventsCount = 0;
    qint64 m_loadProcessEventsNs = 0;
    qint64 m_lastProgressEmitMs = -1;
    qint64 m_lastProcessEventsMs = -1;
    std::unique_ptr<DocumentUndoManager> m_undoManager;
    CallbackMode m_callbackMode = CallbackMode::None;
    std::atomic<bool> m_cancelRequested = false;
    std::function<ViewState()> m_captureViewState;
    std::function<void(const ViewState &, bool restoreCamera)> m_restoreViewState;
    std::function<bool(const QString &, const QSize &, QImage &, CameraShot &, QString &)> m_captureRenderStateSnapshot;
    bool m_bulkLoading = false;
};
