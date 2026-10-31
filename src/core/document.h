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
#include <unordered_map>
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
    //
    // label and transform, when given, are the layer's from the start. A project supplies
    // them, and applying them after the load announced the layer meant it was first shown
    // at the origin under its file name and then moved.
    int loadMesh(const QString &filename, QString *errorMessage = nullptr,
                 const QString &label = QString(), const QMatrix4x4 *transform = nullptr);
    int reloadMesh(int index);
    // STL stores every triangle with its own three corners, so a mesh read from it shares no
    // vertices. When set (the default), opening or reloading an STL merges vertices at the
    // same position, as Remove Duplicate Vertices does. The app sets it from the
    // document.mergeStlDuplicateVertices preference.
    bool mergeStlDuplicateVertices() const { return m_mergeStlDuplicateVertices; }
    void setMergeStlDuplicateVertices(bool merge) { m_mergeStlDuplicateVertices = merge; }
    int saveMesh(int index, const QString &filename, const MeshIOSaveOptions &options);
    int saveMesh(int index, const QString &filename);
    int saveCurrentMesh(const QString &filename, const MeshIOSaveOptions &options);
    int saveCurrentMesh(const QString &filename);
    // Whether a layer, or one of its textures, is what a file holds. NotStored: no file holds
    // it; Changed: one did when it was last read or written, and the layer has changed since
    // (a selection counts, as PLY keeps it); Stored: the file holds what the document holds.
    // These are facts about the disk, kept outside the undo state: undoing past a save shows
    // the layer changed, and undoing back to what was saved shows it stored again.
    enum class StoredState { NotStored, Changed, Stored };
    StoredState meshStoredState(int index) const;
    StoredState meshTextureStoredState(int meshIndex, int textureIndex) const;
    StoredState rasterPlaneStoredState(int rasterIndex, int planeIndex) const;
    // Save one texture of a layer (an index into textureAssets), or one raster plane, as an
    // image file of whatever format the path's suffix names. Saving names the texture's file:
    // when it is not the one the layer's mesh file refers to, the layer differs from its file
    // until the mesh is saved too. Not undo steps: they change the disk, not the document.
    bool saveMeshTexture(int meshIndex, int textureIndex, const QString &path, QString *error = nullptr);
    bool saveRasterPlaneImage(int rasterIndex, int planeIndex, const QString &path, QString *error = nullptr);
    void beginUndoStep(const QString &label);
    void beginUndoStep(const QString &label,
                       const ScriptAction &scriptAction);
    // Delta-storage steps: instead of a mesh deep-copy, record only the attribute
    // classes named in `kinds` (MeshAttributeDeltaKind) for one mesh. Valid only when
    // the action genuinely touches nothing else -- geometry is not captured, so a
    // position moved under a colour-only step would not be undone.
    void beginUndoStep(const QString &label,
                       int meshIndexForDelta,
                       std::uint32_t kinds);
    // Same, recording a ScriptAction so the filter stays reproducible/scriptable.
    void beginUndoStep(const QString &label,
                       const ScriptAction &scriptAction,
                       int meshIndexForDelta,
                       std::uint32_t kinds);
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
    // True while a project is loading its layers; see bulkLoadFinished().
    bool isBulkLoading() const { return m_bulkLoading; }
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
    // Moves the layer at `from` so that it ends up at position `to` (both positions in the
    // current order), shifting the layers between. Its id, and everything else about it, stay;
    // only the position changes, which is the order of the layer list, of grid arrangements
    // and of a saved project. The current layer is still the same layer. One undo step.
    void moveMesh(int from, int to);
    // The same for the raster list; a raster's id (rasterId) stays with it.
    void moveRaster(int from, int to);
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
    // Whether a project saved in folder would refer to a mesh or raster file outside it, which
    // it refuses unless told to copy them in (MeshLabProjectSaveOptions::copyFiles).
    bool referencesFilesOutside(const QString &folder, bool onlyVisibleMeshes = false) const;
    // The project last loaded or saved, as an absolute path; empty for a document that has
    // never been one, and again after clearAllLayers(). Filters write relative output names
    // next to it when their layer has no file of its own.
    QString projectFilePath() const { return m_projectFilePath; }
    // The document's folder: where new saves start, what layer paths are shown against, and
    // where a filter's file given as a bare name goes. The project's folder once there is a
    // project; before that, the folder of the first mesh loaded or saved; or what the user
    // sets. It never resolves a mesh's own textures, which stay relative to that mesh's file.
    // Empty for a new document, and again after clearAllLayers().
    QString directory() const { return m_directory; }
    void setDirectory(const QString &directory);
    // A path as the user should see it: relative to directory() when inside it or one level
    // up, absolute otherwise (another volume, far away, or no folder yet).
    QString displayPath(const QString &path) const;
    int addRaster(const RasterEntry &raster);
    int addRasterImage(
        const QImage &image,
        const QString &name = {},
        const QString &sourcePath = {},
        const CameraShot &shot = CameraShot());
    void removeRaster(int index);
    void setMeshVisible(int index, bool visible);

    // The unit the coordinates are measured in, as far as the user has said: most mesh formats
    // do not, so it starts Unknown and only a choice (or a project that holds one) sets it.
    // It labels the lengths the interface shows -- "abs" becomes "mm" in an abs/perc parameter --
    // and does not change the meshes. Part of the project and of the undo state; File > New
    // forgets it. Setting it is an undo step.
    LengthUnit unit() const { return m_unit; }
    void setUnit(LengthUnit unit);

    // Visibility sets: a name and the layers that show together, for jumping between the
    // arrangements of a project (before / after, one scan at a time...). They hold persistent
    // mesh ids, so they follow a layer through moves and renames, and a layer that is gone
    // is simply not in the set any more. They are part of the project (the .mlp) and of the
    // undo state: defining, redefining, renaming and deleting one are undo steps. Showing one
    // is not, any more than the layer panel's eye is.
    using VisibilitySet = ::VisibilitySet;
    const std::vector<VisibilitySet> &visibilitySets() const { return m_visibilitySets; }
    // The first set with this name, or -1.
    int indexOfVisibilitySet(const QString &name) const;
    // The layers of set `index` that still exist, as indices in document order.
    std::vector<int> visibilitySetMeshIndices(int index) const;
    // The ids of the layers that are visible now.
    std::vector<std::uint64_t> visibleMeshIds() const;
    int addVisibilitySet(const QString &name, std::vector<std::uint64_t> meshIds);
    // The name with " 2", " 3" ... appended if another set has it, as uniqueMeshName does for
    // layers: how a filter's set is named, so that each run leaves one of its own.
    QString uniqueVisibilitySetName(const QString &desired) const;
    // Makes the set the layers that are visible now.
    void updateVisibilitySet(int index);
    // Puts these layers into the set, or takes them out of it: the fine edits of a set. A call
    // that changes nothing takes no undo step.
    void addToVisibilitySet(int index, const std::vector<std::uint64_t> &meshIds);
    void removeFromVisibilitySet(int index, const std::vector<std::uint64_t> &meshIds);
    void renameVisibilitySet(int index, const QString &name);
    void removeVisibilitySet(int index);
    // Puts the set at position `to` (counting after it has left its old one): the order is the
    // panel's, and Alt+1 .. Alt+9 follow it.
    void moveVisibilitySet(int from, int to);
    // Shows the set's layers and hides the others -- or, with `addToVisible`, leaves the
    // visible ones as they are. Goes through setMeshVisible, so it acts on the current view.
    void showVisibilitySet(int index, bool addToVisible = false);
    // Whether the layers visible now are exactly the set's.
    bool visibilitySetIsShown(int index) const;
    void setMeshName(int index, const QString &name);
    // Name a layer the framework has just created. Unlike setMeshName this writes no
    // "Renamed" line and opens no undo step of its own: the placeholder it replaces
    // was never shown to anyone, so there is nothing to rename from.
    void setGeneratedMeshName(int index, const QString &name);
    // The given name if no other layer holds it, otherwise the same with " 2", " 3"
    // ... appended. addMesh() passes every name through this. `ignoreIndex` excludes
    // one layer from the check, so renaming a layer to what it already holds is not
    // mistaken for a clash with itself.
    QString uniqueMeshName(const QString &desired, int ignoreIndex = -1) const;
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
    // Write the layer matrix into the vertex positions and normals and reset it to the
    // identity: what Freeze Matrix does, and what the framework does for a filter whose
    // transformResult is Bake positions. Nothing happens to a layer already at identity.
    void freezeMeshTransform(int index, const QString &contextMessage = {});
    // Move a mesh's vertices by `matrix`. Vertex normals follow its inverse transpose, and a
    // mesh with faces then has its normals recomputed from the moved geometry.
    static void transformMeshGeometry(VCGMesh &mesh, const QMatrix4x4 &matrix);
    void setCurrentMeshIndex(int index);
    void markMeshGeometryChanged(int index, const QString &contextMessage = {});
    // Recount an existing polygon mesh; fauxEdgesModified also reconciles its polygon flag.
    void refreshMeshPolygonFaceCount(int index, bool fauxEdgesModified = false);
    void markMeshMaterialChanged(int index, const QString &contextMessage = {});
    // Selection is stored in per-vertex/per-face BitFlags, which are captured by the
    // undo geometry snapshot.  Changes to selection must therefore bump geometryRevision
    // so the undo cache produces a fresh deep-copy for the "after" checkpoint.
    void markMeshSelectionChanged(int index, const QString &contextMessage = {});
    MeshAttributeDelta captureAttributeDelta(int meshIndex, std::uint32_t kinds) const;
    void applyAttributeDelta(const MeshAttributeDelta &delta);
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

    // Names this document among all that have existed in the process, which mesh ids do not:
    // each document numbers its own meshes from the start. For anything keyed on a mesh that
    // outlives a document.
    std::uint64_t instanceId() const { return m_instanceId; }
    int meshCount() const { return static_cast<int>(m_meshes.size()); }
    MeshEntry &mesh(int i) { return *m_meshes[i]; }
    const MeshEntry &mesh(int i) const { return *m_meshes[i]; }
    // Index of the layer carrying this persistent MeshEntry::meshId, or -1 if no layer
    // does. Ids are minted from 1 and never reused, so a stale handle resolves to -1
    // instead of silently naming whichever layer now sits at that position -- which is
    // what indices do, since every removal shifts the ones above it.
    int indexOfMeshId(std::uint64_t id) const;
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
    // One filter's descriptor, without the applicability pass filterInfos() makes over every
    // filter. Empty when no loaded plugin declares the key.
    std::optional<MeshFilterDescriptor> filterDescriptor(const QString &filterKey) const;
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
    // A project load has put all its layers in. While it runs, meshAdded and its companions
    // are still emitted per layer, but views that rebuild themselves from the whole
    // document (the layer tree, the filter menus) wait for this instead: a rebuild that
    // costs more with every layer, run once per layer, made loading 40 meshes quadratic.
    void bulkLoadFinished();
    void meshAdded(int index);
    void meshRemoved(int index);
    // A layer changed position (moveMesh): everything positional -- a view's per-layer
    // visibility -- has to move with it. Not emitted for the wholesale rebuild of an undo.
    void meshMoved(int from, int to);
    void rasterMoved(int from, int to);
    void meshVisibilityChanged(int index, bool visible);
    // A visibility set was added, removed, renamed or redefined.
    void visibilitySetsChanged();
    void unitChanged();
    void directoryChanged();
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
    static std::uint64_t nextInstanceId();
    const std::uint64_t m_instanceId = nextInstanceId();

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
    void mergeImportedStlVertices(const QString &path, VCGMesh &mesh);
    // What a file that has just been loaded says its unit is (Unknown when it says none). It
    // becomes the document's unit only if nothing else has said so: the file's layer is the
    // only one and the unit is not set. Otherwise it is reported when it differs from the
    // unit the user chose, and nothing is converted.
    void adoptFileUnit(LengthUnit fileUnit, const QString &filename);

    std::unique_ptr<MeshIOPluginManager> m_pluginManager;
    std::unique_ptr<MeshFilterPluginManager> m_filterPluginManager;
    std::unique_ptr<MeshGpuResourceCache> m_gpuCache;
    std::vector<std::unique_ptr<MeshEntry>> m_meshes;
    std::vector<VisibilitySet> m_visibilitySets;
    LengthUnit m_unit = LengthUnit::Unknown;
    QString m_projectFilePath;
    QString m_directory;
    // Sets directory() to the folder of file when it is not set yet.
    void adoptDirectoryOf(const QString &file);
    std::vector<std::unique_ptr<RasterEntry>> m_rasters;
    std::uint64_t m_nextMeshId = 1;
    std::uint64_t m_nextRasterId = 1;
    // Globally monotonic counter for geometry revisions. Never reset or restored
    // during undo/redo, so every distinct geometry snapshot always gets a unique
    // (meshId, geometryRevision) key — preventing cross-branch cache collisions.
    // Material and selection revisions draw from it too, so a revision a layer was saved
    // at is never reached again by a different state after an undo.
    std::uint64_t m_nextGeometryRevision = 1;
    // What the files hold, as of the last load or save (meshStoredState). Never restored by
    // undo. A layer's file and the revisions it was read or written at, by meshId; and, by
    // QImage::cacheKey, the file holding exactly that texture image -- the mesh file itself
    // for one packed inside it (glb, 3mf), which no other mesh file can refer to.
    struct StoredMesh {
        QString path;
        std::uint64_t geometryRevision = 0;
        std::uint64_t materialRevision = 0;
        std::uint64_t selectionRevision = 0;
    };
    struct StoredImage {
        QString path;
        bool packed = false;
    };
    std::unordered_map<std::uint64_t, StoredMesh> m_storedMeshes;
    std::unordered_map<qint64, StoredImage> m_storedImages;
    // Records that the layer's file is path. savedTexturePaths is what a save reported
    // (MeshIOTextureContext); null when the layer was just read from path, whose textures
    // that have an image and no file of their own were packed inside it.
    void recordMeshStored(MeshEntry &entry, const QString &path, const QStringList *savedTexturePaths);
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
    bool m_mergeStlDuplicateVertices = true;
};
