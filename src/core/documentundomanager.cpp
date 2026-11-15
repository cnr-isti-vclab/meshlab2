#include "documentundomanager.h"
#include "document.h"
#include "document_internal.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>

#include <algorithm>
#include <atomic>
#include <set>

std::uint64_t nextUndoNodeSerial()
{
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

using namespace DocumentInternal;

namespace {

qint64 attributeDeltaBytes(const MeshAttributeDelta &delta)
{
    const qint64 words =
        qint64(delta.vertexSelectionBits.capacity() + delta.faceSelectionBits.capacity()
               + delta.edgeSelectionBits.capacity() + delta.faceEdgeSelectionBits.capacity())
        + qint64(delta.vertexColors.capacity() + delta.faceColors.capacity()
                 + delta.edgeColors.capacity());
    const qint64 floats =
        qint64(delta.vertexScalars.capacity() + delta.faceScalars.capacity()
               + delta.edgeScalars.capacity());
    return words * qint64(sizeof(std::uint32_t)) + floats * qint64(sizeof(float));
}

qint64 referencedGeometryBytes(const UndoState &state)
{
    qint64 bytes = 0;
    for (const MeshSnapshot &snapshot : state.meshes) {
        if (snapshot.geometry)
            bytes += vcgMeshCpuBytes(*snapshot.geometry);
        if (snapshot.selection)
            bytes += attributeDeltaBytes(*snapshot.selection);
    }
    return bytes;
}

// A state's geometry and its selection, each counted once however many states share it.
void accountGeometry(
    const UndoState &state,
    std::set<const void *> &seen,
    qint64 &bytes,
    int *count = nullptr)
{
    for (const MeshSnapshot &snapshot : state.meshes) {
        const VCGMesh *geometry = snapshot.geometry.get();
        if (geometry && seen.insert(geometry).second) {
            bytes += vcgMeshCpuBytes(*geometry);
            if (count)
                ++*count;
        }
        const MeshAttributeDelta *selection = snapshot.selection.get();
        if (selection && seen.insert(selection).second)
            bytes += attributeDeltaBytes(*selection);
    }
}

void accountImage(
    const QImage &image,
    std::set<const uchar *> &seen,
    qint64 &bytes,
    int *count = nullptr)
{
    const uchar *storage = image.isNull() ? nullptr : image.constBits();
    if (!storage || !seen.insert(storage).second)
        return;
    bytes += qint64(image.sizeInBytes());
    if (count)
        ++*count;
}

void accountTextureAssets(
    const std::vector<MeshIOTextureAsset> &assets,
    std::set<const uchar *> &seen,
    qint64 &bytes,
    int *count = nullptr)
{
    for (const MeshIOTextureAsset &asset : assets)
        accountImage(asset.image, seen, bytes, count);
}

void accountStateImages(
    const UndoState &state,
    std::set<const uchar *> &seen,
    qint64 &bytes,
    int *count = nullptr)
{
    for (const MeshSnapshot &mesh : state.meshes) {
        accountTextureAssets(mesh.textureAssets, seen, bytes, count);
        accountTextureAssets(mesh.materialSet.textureAssets, seen, bytes, count);
    }
    for (const RasterSnapshot &raster : state.rasters) {
        for (const RasterPlane &plane : raster.planes)
            accountImage(plane.image, seen, bytes, count);
    }
}

// What the history holds beyond the live document, a node at a time: every geometry,
// selection and image counted once however many states share it. Images the live document
// still shows are not counted, as pruning would not free them.
struct HistoryBytes {
    std::set<const void *> geometry;
    std::set<const uchar *> images;
    UndoMemoryStats stats;

    explicit HistoryBytes(const Document &doc)
    {
        qint64 live = 0;
        for (int i = 0; i < doc.meshCount(); ++i) {
            accountTextureAssets(doc.mesh(i).textureAssets, images, live);
            accountTextureAssets(doc.mesh(i).materialSet.textureAssets, images, live);
        }
        for (int i = 0; i < doc.rasterCount(); ++i) {
            for (const RasterPlane &plane : doc.raster(i).planes)
                accountImage(plane.image, images, live);
        }
    }
    void add(const UndoNode &node)
    {
        accountGeometry(node.state, geometry, stats.geometryBytes, &stats.uniqueGeometryCount);
        if (node.beforeAttributes)
            stats.deltaBytes += attributeDeltaBytes(*node.beforeAttributes);
        if (node.afterAttributes)
            stats.deltaBytes += attributeDeltaBytes(*node.afterAttributes);
        accountStateImages(node.state, images, stats.historyImageBytes, &stats.uniqueHistoryImageCount);
    }
};

} // namespace

DocumentUndoManager::DocumentUndoManager(Document &doc)
    : m_doc(doc)
{
}

void DocumentUndoManager::beginStep(const QString &label, const ScriptAction &scriptAction)
{
    if (m_restoringUndoRedo || m_undoStepActive || m_suppressUndo)
        return;

    m_undoStepActive = true;
    m_undoStepLabel = label.trimmed();
    if (m_undoStepLabel.isEmpty())
        m_undoStepLabel = QObject::tr("Edit");
    m_pendingScriptAction = scriptAction;
    m_pendingUndoBefore = m_doc.captureUndoState();
}

void DocumentUndoManager::endStep(bool commit, bool restoreOnCancel)
{
    if (!m_undoStepActive)
        return;

    // Delta mode: defer to endDeltaStep for selection-only operations.
    if (m_pendingDeltaMeshIndex.has_value()) {
        if (!commit) {
            if (restoreOnCancel && m_pendingDeltaBefore.has_value()) {
                m_restoringUndoRedo = true;
                m_doc.applyAttributeDelta(*m_pendingDeltaBefore);
                m_restoringUndoRedo = false;
            }
            m_pendingDeltaBefore.reset();
            m_pendingDeltaMeshIndex.reset();
            m_pendingScriptAction.reset();
            m_undoStepActive = false;
            m_undoStepLabel.clear();
            applyDeferredMemoryPressure();
            return;
        }
        endDeltaStep();
        return;
    }

    const QString label = m_undoStepLabel;
    std::optional<UndoState> before = std::move(m_pendingUndoBefore);
    std::optional<ScriptAction> scriptAction = std::move(m_pendingScriptAction);
    m_pendingUndoBefore.reset();
    m_pendingScriptAction.reset();
    m_undoStepActive = false;
    m_undoStepLabel.clear();

    if (!before.has_value()) {
        applyDeferredMemoryPressure();
        return;
    }

    if (!commit) {
        if (restoreOnCancel) {
            m_restoringUndoRedo = true;
            m_doc.restoreUndoState(*before, false);
            m_restoringUndoRedo = false;
        }
        applyDeferredMemoryPressure();
        return;
    }

    pushStep(label, std::move(*before), m_doc.captureUndoState(), std::move(scriptAction));
}

void DocumentUndoManager::beginDeltaStep(const QString &label, int meshIndex,
                                        std::uint32_t kinds,
                                        std::optional<ScriptAction> scriptAction)
{
    if (m_restoringUndoRedo || m_undoStepActive || m_suppressUndo)
        return;

    m_undoStepActive = true;
    m_undoStepLabel = label.trimmed();
    if (m_undoStepLabel.isEmpty())
        m_undoStepLabel = QObject::tr("Edit");
    m_pendingDeltaBefore = m_doc.captureAttributeDelta(meshIndex, kinds);
    m_pendingDeltaMeshIndex = meshIndex;
    m_pendingDeltaKinds = kinds;
    m_pendingScriptAction = std::move(scriptAction);
    m_pendingUndoBefore.reset();
}

void DocumentUndoManager::endDeltaStep()
{
    if (!m_undoStepActive || !m_pendingDeltaMeshIndex.has_value())
        return;

    const QString label = m_undoStepLabel;
    MeshAttributeDelta before = std::move(*m_pendingDeltaBefore);
    const int meshIndex = *m_pendingDeltaMeshIndex;
    const std::uint32_t kinds = m_pendingDeltaKinds;
    std::optional<ScriptAction> scriptAction = std::move(m_pendingScriptAction);
    m_pendingDeltaBefore.reset();
    m_pendingDeltaMeshIndex.reset();
    m_pendingDeltaKinds = MeshAttributeNone;
    m_pendingScriptAction.reset();
    m_undoStepActive = false;
    m_undoStepLabel.clear();

    MeshAttributeDelta after = m_doc.captureAttributeDelta(meshIndex, kinds);
    pushDeltaStep(label, std::move(before), std::move(after), std::move(scriptAction));
    applyDeferredMemoryPressure();
}

bool DocumentUndoManager::canUndo() const
{
    if (m_undoCurrentNode < 0 || m_undoCurrentNode >= static_cast<int>(m_undoNodes.size()))
        return false;
    return m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].parentId >= 0;
}

bool DocumentUndoManager::canRedo() const
{
    if (m_undoCurrentNode < 0 || m_undoCurrentNode >= static_cast<int>(m_undoNodes.size()))
        return false;
    return !m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].children.empty();
}

QString DocumentUndoManager::undoText() const
{
    if (!canUndo())
        return {};
    return m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].label;
}

QString DocumentUndoManager::redoText() const
{
    if (!canRedo())
        return {};
    const auto &node = m_undoNodes[static_cast<size_t>(m_undoCurrentNode)];
    const int preferred = node.preferredChild >= 0 ? node.preferredChild
                                                   : node.children.front();
    return m_undoNodes[static_cast<size_t>(preferred)].label;
}

int DocumentUndoManager::undoCursorPosition() const
{
    if (m_undoCurrentNode < 0 || m_undoCurrentNode >= static_cast<int>(m_undoNodes.size()))
        return 0;
    int depth = 0;
    int id = m_undoCurrentNode;
    while (m_undoNodes[static_cast<size_t>(id)].parentId >= 0) {
        ++depth;
        id = m_undoNodes[static_cast<size_t>(id)].parentId;
    }
    return depth;
}

std::vector<UndoTreeNodeInfo> DocumentUndoManager::undoTreeInfo() const
{
    if (m_undoNodes.empty())
        return {};

    std::set<int> onPath;
    {
        int id = m_undoCurrentNode;
        while (id >= 0) {
            onPath.insert(id);
            id = m_undoNodes[static_cast<size_t>(id)].parentId;
        }
    }

    std::vector<UndoTreeNodeInfo> result;
    result.reserve(m_undoNodes.size());
    struct Frame { int id; int depth; };
    std::vector<Frame> stack = {{ 0, 0 }};
    while (!stack.empty()) {
        const auto [id, depth] = stack.back();
        stack.pop_back();
        const auto &node = m_undoNodes[static_cast<size_t>(id)];
        UndoTreeNodeInfo info;
        info.nodeId = id;
        info.serial = node.serial;
        info.parentId = node.parentId;
        info.depth = depth;
        info.isCurrent = (id == m_undoCurrentNode);
        info.attributeDelta = node.storageKind == UndoStorageKind::Delta;
        info.isOnCurrentPath = onPath.count(id) > 0;
        info.label = node.label;
        if (node.actionRecord.has_value()
            && node.actionRecord->kind == QStringLiteral("filter")) {
            info.filterKey = node.actionRecord->filterKey;
        }
        result.push_back(std::move(info));
        for (int i = static_cast<int>(node.children.size()) - 1; i >= 0; --i)
            stack.push_back({ node.children[static_cast<size_t>(i)], depth + 1 });
    }
    return result;
}

bool DocumentUndoManager::updateNodeCamera(int nodeId, const ViewState &viewState)
{
    if (nodeId < 0 || nodeId >= static_cast<int>(m_undoNodes.size()))
        return false;
    m_undoNodes[static_cast<size_t>(nodeId)].state.viewState = viewState;
    return true;
}

bool DocumentUndoManager::jumpToNode(int nodeId, bool restoreCamera)
{
    if (nodeId < 0 || nodeId >= static_cast<int>(m_undoNodes.size()))
        return false;
    if (nodeId == m_undoCurrentNode) {
        m_doc.restoreViewState(m_undoNodes[static_cast<size_t>(nodeId)].state.viewState, restoreCamera);
        return true;
    }
    if (m_undoStepActive)
        return false;

    // Walk up from both nodes to find the lowest common ancestor (LCA).
    // Build the path from target to root.
    std::vector<int> targetPath;
    {
        int id = nodeId;
        while (id >= 0) {
            targetPath.push_back(id);
            id = m_undoNodes[static_cast<size_t>(id)].parentId;
        }
    }
    // Build ancestor set for current node.
    std::set<int> currentAncestors;
    {
        int id = m_undoCurrentNode;
        while (id >= 0) {
            currentAncestors.insert(id);
            id = m_undoNodes[static_cast<size_t>(id)].parentId;
        }
    }
    // The LCA is the first node in targetPath that is also an ancestor of current.
    int lca = -1;
    for (int id : targetPath) {
        if (currentAncestors.count(id)) { lca = id; break; }
    }
    if (lca < 0)
        return false;

    // The redo side, LCA excluded, in order from the LCA down to the target.
    std::vector<int> redoPath(targetPath.begin(),
                              std::find(targetPath.begin(), targetPath.end(), lca));
    std::reverse(redoPath.begin(), redoPath.end());

    // Where the walk passes, the preferred children point back the way it came, as single
    // undos and redos would leave them: undo, then redo, returns to where the jump started.
    bool walkHasFullStep = false;
    for (int id = m_undoCurrentNode; id != lca; id = m_undoNodes[static_cast<size_t>(id)].parentId) {
        UndoNode &node = m_undoNodes[static_cast<size_t>(id)];
        m_undoNodes[static_cast<size_t>(node.parentId)].preferredChild = id;
        walkHasFullStep |= node.storageKind == UndoStorageKind::FullSnapshot;
    }
    for (int id : redoPath) {
        UndoNode &node = m_undoNodes[static_cast<size_t>(id)];
        m_undoNodes[static_cast<size_t>(node.parentId)].preferredChild = id;
        walkHasFullStep |= node.storageKind == UndoStorageKind::FullSnapshot;
    }

    // One state is restored, not every one in between: the target's, or, for a delta node,
    // its nearest full snapshot followed by the deltas below it. Only a walk made of delta
    // steps alone is replayed, as each of those is cheaper than a restore.
    m_suppressUndoRedoSignals = true;
    m_restoringUndoRedo = true;
    if (walkHasFullStep) {
        std::vector<int> deltas;
        int base = nodeId;
        while (m_undoNodes[static_cast<size_t>(base)].storageKind == UndoStorageKind::Delta) {
            deltas.push_back(base);
            base = m_undoNodes[static_cast<size_t>(base)].parentId;
        }
        m_restoreCamera = deltas.empty() && restoreCamera;
        m_doc.restoreUndoState(m_undoNodes[static_cast<size_t>(base)].state);
        for (auto it = deltas.rbegin(); it != deltas.rend(); ++it) {
            const UndoNode &node = m_undoNodes[static_cast<size_t>(*it)];
            if (node.afterAttributes)
                m_doc.applyAttributeDelta(*node.afterAttributes);
        }
    } else {
        for (int id = m_undoCurrentNode; id != lca; id = m_undoNodes[static_cast<size_t>(id)].parentId)
            m_doc.applyAttributeDelta(*m_undoNodes[static_cast<size_t>(id)].beforeAttributes);
        for (int id : redoPath)
            m_doc.applyAttributeDelta(*m_undoNodes[static_cast<size_t>(id)].afterAttributes);
    }
    m_undoCurrentNode = nodeId;
    m_restoringUndoRedo = false;
    // A delta node's state carries no view of its own (its afterAttributes are the state);
    // a full one's was put back by the restore above.
    if (!walkHasFullStep && m_undoNodes[static_cast<size_t>(nodeId)].storageKind == UndoStorageKind::FullSnapshot)
        m_doc.restoreViewState(m_undoNodes[static_cast<size_t>(nodeId)].state.viewState, restoreCamera);
    m_doc.writeLog(QObject::tr("Jump to '%1': %2 restored state, %3")
        .arg(m_undoNodes[static_cast<size_t>(nodeId)].label)
        .arg(walkHasFullStep ? 1 : 0)
        .arg(walkHasFullStep ? QObject::tr("then the deltas below it") : QObject::tr("deltas only")),
        Document::LogSource::Application, Document::LogLevel::Debug);

    m_restoreCamera = true;
    m_suppressUndoRedoSignals = false;
    emitStateChanged();
    return true;
}

bool DocumentUndoManager::undo()
{
    if (!canUndo() || m_undoStepActive)
        return false;

    const auto &node = m_undoNodes[static_cast<size_t>(m_undoCurrentNode)];
    const int parentId = node.parentId;
    const auto &target = m_undoNodes[static_cast<size_t>(parentId)];

    if (node.storageKind == UndoStorageKind::Delta && node.beforeAttributes.has_value()) {
        // Delta undo: revert selection on live mesh, no full state restore.
        m_restoringUndoRedo = true;
        m_doc.applyAttributeDelta(*node.beforeAttributes);
        m_undoNodes[static_cast<size_t>(parentId)].preferredChild = m_undoCurrentNode;
        m_undoCurrentNode = parentId;
        emitStateChanged();
        m_restoringUndoRedo = false;
        m_doc.writeLog(QObject::tr("Undo '%1' (selection delta)").arg(node.label),
            Document::LogSource::Application);
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    m_restoringUndoRedo = true;
    m_doc.restoreUndoState(target.state);
    // Remember which child to return to on redo.
    m_undoNodes[static_cast<size_t>(parentId)].preferredChild = m_undoCurrentNode;
    m_undoCurrentNode = parentId;
    emitStateChanged();
    m_restoringUndoRedo = false;
    m_doc.writeLog(QObject::tr("Undo '%1': %2 ms (%3 meshes, %4 rasters)")
        .arg(node.label)
        .arg(timer.elapsed())
        .arg(target.state.meshes.size())
        .arg(target.state.rasters.size()),
        Document::LogSource::Application, Document::LogLevel::Debug);
    return true;
}

bool DocumentUndoManager::redo()
{
    if (!canRedo() || m_undoStepActive)
        return false;

    auto &node = m_undoNodes[static_cast<size_t>(m_undoCurrentNode)];
    const int childId = node.preferredChild >= 0 ? node.preferredChild
                                                  : node.children.front();
    const auto &target = m_undoNodes[static_cast<size_t>(childId)];
    if (target.storageKind == UndoStorageKind::Delta && target.afterAttributes.has_value()) {
        // Delta redo: apply after-selection on live mesh, no full state restore.
        m_restoringUndoRedo = true;
        m_doc.applyAttributeDelta(*target.afterAttributes);
        m_undoCurrentNode = childId;
        emitStateChanged();
        m_restoringUndoRedo = false;
        m_doc.writeLog(QObject::tr("Redo '%1' (selection delta)").arg(target.label),
            Document::LogSource::Application);
        return true;
    }

    QElapsedTimer timer;
    timer.start();
    m_restoringUndoRedo = true;
    m_doc.restoreUndoState(target.state);
    m_undoCurrentNode = childId;
    emitStateChanged();
    m_restoringUndoRedo = false;
    m_doc.writeLog(QObject::tr("Redo '%1': %2 ms (%3 meshes, %4 rasters)")
        .arg(target.label)
        .arg(timer.elapsed())
        .arg(target.state.meshes.size())
        .arg(target.state.rasters.size()),
        Document::LogSource::Application, Document::LogLevel::Debug);
    return true;
}

void DocumentUndoManager::clear()
{
    if (m_undoNodes.empty() && m_undoCurrentNode < 0 && !m_undoStepActive)
        return;

    m_undoNodes.clear();
    m_undoGeometryCache.clear();
    m_undoSelectionCache.clear();
    m_undoCurrentNode = -1;
    m_undoStepActive = false;
    m_undoStepLabel.clear();
    m_pendingUndoBefore.reset();
    m_pendingDeltaBefore.reset();
    m_pendingDeltaMeshIndex.reset();
    m_pendingMemoryPressure = 0;
    emitStateChanged();
}

// nodeId and every node below it, in ascending id order.
std::vector<int> DocumentUndoManager::subtreeNodes(int nodeId) const
{
    std::vector<int> nodes;
    std::vector<int> stack = { nodeId };
    while (!stack.empty()) {
        const int id = stack.back();
        stack.pop_back();
        nodes.push_back(id);
        for (int c : m_undoNodes[static_cast<size_t>(id)].children)
            stack.push_back(c);
    }
    std::sort(nodes.begin(), nodes.end());
    return nodes;
}

// Keeps the nodes in keepSorted (ascending ids, the current node among them), renumbers
// them in that order, and drops every link to a node that is not kept. A node whose
// preferred child is gone prefers its newest remaining child.
void DocumentUndoManager::compactNodes(const std::vector<int> &keepSorted)
{
    std::vector<int> remap(m_undoNodes.size(), -1);
    for (int ni = 0; ni < static_cast<int>(keepSorted.size()); ++ni)
        remap[static_cast<size_t>(keepSorted[static_cast<size_t>(ni)])] = ni;
    const auto mapped = [&remap](int id) { return id >= 0 ? remap[static_cast<size_t>(id)] : -1; };

    std::vector<UndoNode> compacted;
    compacted.reserve(keepSorted.size());
    for (int oldId : keepSorted) {
        UndoNode n = std::move(m_undoNodes[static_cast<size_t>(oldId)]);
        n.parentId = mapped(n.parentId);
        std::vector<int> children;
        for (int c : n.children) {
            if (const int m = mapped(c); m >= 0)
                children.push_back(m);
        }
        n.children = std::move(children);
        n.preferredChild = mapped(n.preferredChild);
        if (n.preferredChild < 0)
            n.preferredChild = n.children.empty() ? -1 : n.children.back();
        compacted.push_back(std::move(n));
    }
    m_undoCurrentNode = mapped(m_undoCurrentNode);
    m_undoNodes = std::move(compacted);
}

// Drops everything outside nodeId's subtree. The current node must be in it.
void DocumentUndoManager::rootAt(int nodeId)
{
    const std::vector<int> keep = subtreeNodes(nodeId);
    compactNodes(keep);
    UndoNode &newRoot = m_undoNodes[static_cast<size_t>(
        std::lower_bound(keep.begin(), keep.end(), nodeId) - keep.begin())];
    // A root state has no label, and the actions that produced it must not be replayed.
    // Informational calls made afterwards remain valid and stay in trailingActionRecords.
    newRoot.label.clear();
    newRoot.prefixActionRecords.clear();
    newRoot.actionRecord.reset();
}

bool DocumentUndoManager::makeRoot(int nodeId)
{
    if (nodeId < 0 || nodeId >= static_cast<int>(m_undoNodes.size()))
        return false;
    if (m_undoStepActive)
        return false;
    // A delta node holds no complete document state to start the history from.
    if (m_undoNodes[static_cast<size_t>(nodeId)].storageKind == UndoStorageKind::Delta)
        return false;
    // Already the only node (trivial root) – nothing to do.
    if (m_undoNodes[static_cast<size_t>(nodeId)].parentId < 0 &&
        m_undoNodes.size() == 1)
        return false;

    // If the current node is not in the surviving subtree, go to nodeId first, so that the
    // live document is the state the tree then calls current.
    const std::vector<int> keep = subtreeNodes(nodeId);
    if (!std::binary_search(keep.begin(), keep.end(), m_undoCurrentNode)
        && !jumpToNode(nodeId, false))
        return false;
    rootAt(nodeId);
    emitStateChanged();
    return true;
}

bool DocumentUndoManager::purgeBranch(int nodeId)
{
    if (nodeId < 0 || nodeId >= static_cast<int>(m_undoNodes.size()))
        return false;
    if (m_undoStepActive)
        return false;
    // Nothing to purge if the node is already a leaf.
    if (m_undoNodes[static_cast<size_t>(nodeId)].children.empty())
        return false;

    // Everything but nodeId's strict descendants survives; the root always does.
    std::vector<bool> purged(m_undoNodes.size(), false);
    for (int id : subtreeNodes(nodeId))
        purged[static_cast<size_t>(id)] = id != nodeId;
    std::vector<int> keep;
    for (int i = 0; i < static_cast<int>(m_undoNodes.size()); ++i)
        if (!purged[static_cast<size_t>(i)])
            keep.push_back(i);

    // If the current node is being purged, go back to nodeId first, so that the live
    // document is the state the tree then calls current.
    if (purged[static_cast<size_t>(m_undoCurrentNode)] && !jumpToNode(nodeId, false))
        return false;
    compactNodes(keep);
    emitStateChanged();
    return true;
}

bool DocumentUndoManager::linearizeHistory()
{
    if (m_undoNodes.empty() || m_undoStepActive)
        return false;

    // Build the set of node ids on the current path: root → m_undoCurrentNode.
    std::vector<int> path;
    {
        int id = m_undoCurrentNode;
        while (id >= 0) {
            path.push_back(id);
            id = m_undoNodes[static_cast<size_t>(id)].parentId;
        }
    }
    // path is currently ordered current→root; reverse to get root→current.
    std::reverse(path.begin(), path.end());

    // If the history is already linear (no side branches), nothing to do.
    if (static_cast<int>(path.size()) == static_cast<int>(m_undoNodes.size()))
        return false;

    std::sort(path.begin(), path.end());
    compactNodes(path);
    emitStateChanged();
    return true;
}

void DocumentUndoManager::setUndoLimit(int limit)
{
    m_undoLimit = std::max(1, limit);
    pruneTreeToLimit();
}

void DocumentUndoManager::setUndoMemoryLimitBytes(qint64 bytes)
{
    m_undoMemoryLimitBytes = std::max<qint64>(0, bytes);
    if (m_undoStepActive || m_undoMemoryLimitBytes == 0)
        return;

    const UndoPruneResult result = enforceConfiguredMemoryLimit();
    if (result.changed())
        emitStateChanged();
}

UndoPruneResult DocumentUndoManager::pruneToMemoryBudget(qint64 maximumBytes)
{
    if (m_undoStepActive || maximumBytes <= 0)
        return {};
    const UndoPruneResult result = pruneToMemoryBudgetInternal(
        maximumBytes,
        maximumBytes * 4 / 5,
        true);
    if (result.changed()) {
        logAutomaticPrune(result, QObject::tr("manual memory budget"));
        emitStateChanged();
    }
    return result;
}

UndoPruneResult DocumentUndoManager::pruneToMemoryBudgetInternal(
    qint64 triggerBytes,
    qint64 targetBytes,
    bool allowClear)
{
    UndoPruneResult result;
    result.beforeBytes = memoryStats().totalBytes();
    result.afterBytes = result.beforeBytes;
    if (triggerBytes <= 0 || result.beforeBytes <= triggerBytes || m_undoNodes.empty())
        return result;

    targetBytes = std::max<qint64>(0, std::min(targetBytes, triggerBytes));
    const int oldNodeCount = static_cast<int>(m_undoNodes.size());
    const bool previousSignalSuppression = m_suppressUndoRedoSignals;
    m_suppressUndoRedoSignals = true;

    // Alternate futures are least valuable under pressure and can retain whole
    // geometry revisions, so discard them before shortening the current path.
    linearizeHistory();

    // Count the path from the tip back, so that the bytes kept by rooting at each node are
    // known in one pass: root at the oldest full snapshot whose states fit the target, the
    // present root meaning nothing to prune. A delta node has no complete document state to
    // start the history from, and at least one transition stays.
    std::vector<int> path;
    for (int id = m_undoCurrentNode; id >= 0; id = m_undoNodes[static_cast<size_t>(id)].parentId)
        path.push_back(id);
    HistoryBytes kept(m_doc);
    int newRoot = -1;
    for (size_t i = 0; i < path.size(); ++i) {
        const UndoNode &node = m_undoNodes[static_cast<size_t>(path[i])];
        kept.add(node);
        if (i == 0 || (node.storageKind != UndoStorageKind::FullSnapshot && node.parentId >= 0))
            continue;
        if (newRoot >= 0 && kept.stats.totalBytes() > targetBytes)
            break;
        newRoot = path[i];
    }
    if (newRoot >= 0 && newRoot != path.back())
        makeRoot(newRoot);
    qint64 currentBytes = memoryStats().totalBytes();

    // The newest complete checkpoint may itself exceed the budget. Because the
    // user explicitly enabled a byte cap, clearing is preferable to pretending
    // the cap is enforced while retaining an unreclaimable baseline snapshot.
    if (allowClear && currentBytes > triggerBytes) {
        clear();
        result.cleared = true;
        currentBytes = 0;
    }

    m_suppressUndoRedoSignals = previousSignalSuppression;
    result.afterBytes = currentBytes;
    result.removedNodes = oldNodeCount - static_cast<int>(m_undoNodes.size());
    return result;
}

UndoPruneResult DocumentUndoManager::enforceConfiguredMemoryLimit()
{
    if (m_undoMemoryLimitBytes <= 0 || m_undoStepActive)
        return {};
    const UndoPruneResult result = pruneToMemoryBudgetInternal(
        m_undoMemoryLimitBytes,
        m_undoMemoryLimitBytes * 4 / 5,
        true);
    if (result.changed())
        logAutomaticPrune(result, QObject::tr("configured memory budget"));
    return result;
}

void DocumentUndoManager::handleMemoryPressure(bool critical)
{
    if (m_undoStepActive) {
        m_pendingMemoryPressure = std::max(m_pendingMemoryPressure, critical ? 2 : 1);
        return;
    }

    UndoPruneResult result;
    if (critical) {
        result.beforeBytes = memoryStats().totalBytes();
        const int oldNodeCount = static_cast<int>(m_undoNodes.size());
        if (oldNodeCount > 0) {
            const bool previousSignalSuppression = m_suppressUndoRedoSignals;
            m_suppressUndoRedoSignals = true;
            clear();
            m_suppressUndoRedoSignals = previousSignalSuppression;
            result.cleared = true;
            result.removedNodes = oldNodeCount;
        }
        result.afterBytes = 0;
    } else if (m_undoMemoryLimitBytes > 0) {
        result = pruneToMemoryBudgetInternal(
            m_undoMemoryLimitBytes * 3 / 4,
            m_undoMemoryLimitBytes / 2,
            true);
    }

    if (result.changed()) {
        logAutomaticPrune(
            result,
            critical ? QObject::tr("critical system memory pressure")
                     : QObject::tr("system memory pressure"));
        emitStateChanged();
    }
}

void DocumentUndoManager::applyDeferredMemoryPressure()
{
    if (m_undoStepActive || m_pendingMemoryPressure == 0)
        return;
    const int pending = std::exchange(m_pendingMemoryPressure, 0);
    handleMemoryPressure(pending >= 2);
}

void DocumentUndoManager::logAutomaticPrune(
    const UndoPruneResult &result,
    const QString &reason)
{
    if (!result.changed())
        return;
    m_doc.writeLog(
        QObject::tr("Undo history pruned for %1: %2 MiB -> %3 MiB (%4 node(s) removed)%5")
            .arg(reason)
            .arg(result.beforeBytes / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(result.afterBytes / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(result.removedNodes)
            .arg(result.cleared ? QObject::tr("; history cleared") : QString()),
        Document::LogSource::Application,
        Document::LogLevel::Warning);
}


void DocumentUndoManager::pushStep(const QString &label, UndoState &&before, UndoState &&after,
                            std::optional<ScriptAction> scriptAction)
{
    // If there is no tree yet, create the root node from the "before" state.
    if (m_undoCurrentNode < 0) {
        UndoNode root;
        root.state   = std::move(before);
        root.label   = {};
        root.parentId = -1;
        m_undoNodes.push_back(std::move(root));
        m_undoCurrentNode = 0;
    } else {
        // Update the current node's state to be the "before" snapshot.
        // (It should already match, but this keeps things consistent if the
        //  caller uses an older capture.)
        m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].state = std::move(before);
    }

    // Append a new child node carrying the "after" state.
    const int newId = static_cast<int>(m_undoNodes.size());
    UndoNode child;
    child.state    = std::move(after);
    child.storageKind = UndoStorageKind::FullSnapshot;
    child.label    = label;
    child.parentId = m_undoCurrentNode;
    child.prefixActionRecords = std::move(
        m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].trailingActionRecords);
    if (scriptAction.has_value())
        child.actionRecord = UndoActionRecord::fromScriptAction(*scriptAction);

    m_undoNodes.push_back(std::move(child));

    // Link parent → new child and make it the preferred redo target.
    auto &parent = m_undoNodes[static_cast<size_t>(m_undoCurrentNode)];
    parent.children.push_back(newId);
    parent.preferredChild = newId;

    m_undoCurrentNode = newId;

    pruneTreeToLimit();
    enforceConfiguredMemoryLimit();
    emitStateChanged();
    applyDeferredMemoryPressure();
}

void DocumentUndoManager::pushDeltaStep(
    const QString &label,
    MeshAttributeDelta &&before,
    MeshAttributeDelta &&after,
    std::optional<ScriptAction> scriptAction)
{
    if (m_undoCurrentNode < 0) {
        UndoNode root;
        root.state = m_doc.captureUndoState();
        root.label = {};
        root.parentId = -1;
        m_undoNodes.push_back(std::move(root));
        m_undoCurrentNode = 0;
    }
    // Don't overwrite current node's state — for delta steps the parent
    // already carries the correct full snapshot from a prior action.

    const int newId = static_cast<int>(m_undoNodes.size());
    UndoNode child;
    child.storageKind = UndoStorageKind::Delta;
    child.label = label;
    child.parentId = m_undoCurrentNode;
    child.prefixActionRecords = std::move(
        m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].trailingActionRecords);
    child.beforeAttributes = std::move(before);
    child.afterAttributes = std::move(after);
    if (scriptAction.has_value())
        child.actionRecord = UndoActionRecord::fromScriptAction(*scriptAction);

    m_undoNodes.push_back(std::move(child));
    auto &parent = m_undoNodes[static_cast<size_t>(m_undoCurrentNode)];
    parent.children.push_back(newId);
    parent.preferredChild = newId;
    m_undoCurrentNode = newId;

    pruneTreeToLimit();
    enforceConfiguredMemoryLimit();
    emitStateChanged();
}

// Only the action that produced this state, unlike nodeScriptActions() which also returns
// the informational calls ordered around it. Reopening a filter must use this one.
std::optional<ScriptAction> DocumentUndoManager::nodeAction(int nodeId) const
{
    if (nodeId < 0 || nodeId >= static_cast<int>(m_undoNodes.size()))
        return std::nullopt;
    const UndoNode &node = m_undoNodes[static_cast<size_t>(nodeId)];
    if (!node.actionRecord.has_value())
        return std::nullopt;
    return node.actionRecord->toScriptAction();
}

std::vector<ScriptAction> DocumentUndoManager::nodeScriptActions(int nodeId) const
{
    std::vector<ScriptAction> actions;
    if (nodeId < 0 || nodeId >= static_cast<int>(m_undoNodes.size()))
        return actions;
    const UndoNode &node = m_undoNodes[static_cast<size_t>(nodeId)];
    actions.reserve(node.prefixActionRecords.size() + node.trailingActionRecords.size() + 1);
    for (const UndoActionRecord &record : node.prefixActionRecords)
        actions.push_back(record.toScriptAction());
    if (node.actionRecord.has_value())
        actions.push_back(node.actionRecord->toScriptAction());
    for (const UndoActionRecord &record : node.trailingActionRecords)
        actions.push_back(record.toScriptAction());
    return actions;
}

void DocumentUndoManager::recordScriptAction(const ScriptAction &scriptAction)
{
    if (m_restoringUndoRedo || m_suppressUndo)
        return;
    if (m_undoCurrentNode < 0) {
        UndoNode root;
        root.state = m_doc.captureUndoState();
        root.parentId = -1;
        m_undoNodes.push_back(std::move(root));
        m_undoCurrentNode = 0;
    }
    m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].trailingActionRecords.push_back(
        UndoActionRecord::fromScriptAction(scriptAction));
    emitStateChanged();
}

// Prune the oldest ancestor nodes until the tree has at most m_undoLimit nodes
// (not counting the root which serves as the "no history" sentinel).
// The current path to m_undoCurrentNode is always preserved.
void DocumentUndoManager::pruneTreeToLimit()
{
    // Count nodes along the current path (root to current).
    int depth = 0;
    {
        int id = m_undoCurrentNode;
        while (id >= 0) {
            ++depth;
            id = m_undoNodes[static_cast<size_t>(id)].parentId;
        }
    }
    if (depth - 1 <= m_undoLimit)  // depth-1 = number of transitions
        return;

    // Find the ancestor of m_undoCurrentNode that sits at depth m_undoLimit from
    // the current tip, i.e. the new root after pruning.
    int newRoot = m_undoCurrentNode;
    for (int i = 0; i < m_undoLimit; ++i)
        newRoot = m_undoNodes[static_cast<size_t>(newRoot)].parentId;
    // A delta node holds no complete document state to start the history from: keep the
    // few steps back to the nearest full snapshot.
    while (m_undoNodes[static_cast<size_t>(newRoot)].storageKind == UndoStorageKind::Delta
           && m_undoNodes[static_cast<size_t>(newRoot)].parentId >= 0)
        newRoot = m_undoNodes[static_cast<size_t>(newRoot)].parentId;
    if (m_undoNodes[static_cast<size_t>(newRoot)].parentId < 0)
        return;
    rootAt(newRoot);
}

void DocumentUndoManager::emitStateChanged()
{
    if (m_suppressUndoRedoSignals)
        return;
    m_doc.undoRedoStateChanged(canUndo(), canRedo(), undoText(), redoText());
}

UndoMemoryStats DocumentUndoManager::memoryStats() const
{
    HistoryBytes history(m_doc);
    for (const UndoNode &node : m_undoNodes)
        history.add(node);
    UndoMemoryStats stats = std::move(history.stats);
    stats.nodeCount = static_cast<int>(m_undoNodes.size());
    std::vector<int> path;
    {
        int id = m_undoCurrentNode;
        while (id >= 0) {
            path.push_back(id);
            id = m_undoNodes[static_cast<size_t>(id)].parentId;
        }
        std::reverse(path.begin(), path.end());
    }

    for (int pi = 1; pi < static_cast<int>(path.size()); ++pi) {
        UndoStepMemoryInfo info;
        const UndoNode &node = m_undoNodes[static_cast<size_t>(path[pi])];
        info.label = node.label;
        info.attributeDelta = node.storageKind == UndoStorageKind::Delta;
        if (info.attributeDelta) {
            if (node.beforeAttributes)
                info.deltaBytes += attributeDeltaBytes(*node.beforeAttributes);
            if (node.afterAttributes)
                info.deltaBytes += attributeDeltaBytes(*node.afterAttributes);
        } else {
            info.referencedGeometryBytes = referencedGeometryBytes(node.state);
        }
        stats.steps.push_back(info);
    }

    if (m_pendingUndoBefore) {
        accountGeometry(*m_pendingUndoBefore, history.geometry, stats.pendingGeometryBytes);
        accountStateImages(*m_pendingUndoBefore, history.images, stats.pendingImageBytes);
    }
    if (m_pendingDeltaBefore)
        stats.pendingDeltaBytes = attributeDeltaBytes(*m_pendingDeltaBefore);
    return stats;
}
