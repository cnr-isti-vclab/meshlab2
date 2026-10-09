#include "documentundomanager.h"
#include "document.h"
#include "document_internal.h"

#include <QDateTime>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QVector>

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

qint64 selectionDeltaBytes(const SelectionDelta &delta)
{
    return qint64(delta.vertexSelectionBits.capacity() + delta.faceSelectionBits.capacity()
                  + delta.edgeSelectionBits.capacity())
         * qint64(sizeof(std::uint32_t));
}

qint64 referencedGeometryBytes(const UndoState &state)
{
    qint64 bytes = 0;
    for (const MeshSnapshot &snapshot : state.meshes) {
        if (snapshot.geometry)
            bytes += vcgMeshCpuBytes(*snapshot.geometry);
    }
    return bytes;
}

void accountGeometry(
    const UndoState &state,
    std::set<const VCGMesh *> &seen,
    qint64 &bytes,
    int *count = nullptr)
{
    for (const MeshSnapshot &snapshot : state.meshes) {
        const VCGMesh *geometry = snapshot.geometry.get();
        if (!geometry || !seen.insert(geometry).second)
            continue;
        bytes += vcgMeshCpuBytes(*geometry);
        if (count)
            ++*count;
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
                m_doc.applySelectionDelta(*m_pendingDeltaBefore);
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
            m_doc.restoreUndoState(*before);
            m_restoringUndoRedo = false;
        }
        applyDeferredMemoryPressure();
        return;
    }

    pushStep(label, std::move(*before), m_doc.captureUndoState(), std::move(scriptAction));
}

void DocumentUndoManager::beginDeltaStep(const QString &label, int meshIndex,
                                        std::optional<ScriptAction> scriptAction)
{
    if (m_restoringUndoRedo || m_undoStepActive || m_suppressUndo)
        return;

    m_undoStepActive = true;
    m_undoStepLabel = label.trimmed();
    if (m_undoStepLabel.isEmpty())
        m_undoStepLabel = QObject::tr("Edit");
    m_pendingDeltaBefore = m_doc.captureSelectionDelta(meshIndex);
    m_pendingDeltaMeshIndex = meshIndex;
    m_pendingScriptAction = std::move(scriptAction);
    m_pendingUndoBefore.reset();
}

void DocumentUndoManager::endDeltaStep()
{
    if (!m_undoStepActive || !m_pendingDeltaMeshIndex.has_value())
        return;

    const QString label = m_undoStepLabel;
    SelectionDelta before = std::move(*m_pendingDeltaBefore);
    const int meshIndex = *m_pendingDeltaMeshIndex;
    std::optional<ScriptAction> scriptAction = std::move(m_pendingScriptAction);
    m_pendingDeltaBefore.reset();
    m_pendingDeltaMeshIndex.reset();
    m_pendingScriptAction.reset();
    m_undoStepActive = false;
    m_undoStepLabel.clear();

    SelectionDelta after = m_doc.captureSelectionDelta(meshIndex);
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

QStringList DocumentUndoManager::undoHistoryLabels() const
{
    QStringList labels;
    if (m_undoCurrentNode < 0)
        return labels;
    int id = m_undoCurrentNode;
    while (id >= 0) {
        const auto &node = m_undoNodes[static_cast<size_t>(id)];
        if (!node.label.isEmpty())
            labels.prepend(node.label);
        id = node.parentId;
    }
    return labels;
}

QStringList DocumentUndoManager::undoStackLabels() const
{
    return undoHistoryLabels();
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

    {
        std::vector<int> redoPreview;
        for (int id : targetPath) {
            if (id == lca) break;
            redoPreview.push_back(id);
        }
        std::reverse(redoPreview.begin(), redoPreview.end());
        qDebug() << "[JUMP] target=" << nodeId
                 << "current=" << m_undoCurrentNode
                 << "lca=" << lca
                 << "undos=" << (m_undoCurrentNode != lca ? "yes" : "no")
                 << "redoPath=" << QVector<int>(redoPreview.begin(), redoPreview.end());
    }

    // Suppress per-step signals during the multi-step navigation so that
    // intermediate states don't trigger thumbnail capture (grabFramebuffer)
    // or other GUI refreshes that could interfere with the GPU/render state.
    // Camera is never restored for intermediate steps; only the final redo
    // restores it (controlled by the restoreCamera parameter).
    m_suppressUndoRedoSignals = true;
    m_restoreCamera = false;

    // Undo until we reach the LCA.
    while (m_undoCurrentNode != lca) {
        if (!undo()) {
            m_restoreCamera = true;
            m_suppressUndoRedoSignals = false;
            emitStateChanged();
            return false;
        }
    }
    // Now redo along the path from LCA to target.
    // Collect the path from LCA to target (excluding LCA).
    std::vector<int> redoPath;
    for (int id : targetPath) {
        if (id == lca) break;
        redoPath.push_back(id);
    }
    std::reverse(redoPath.begin(), redoPath.end());
    for (int i = 0; i < static_cast<int>(redoPath.size()); ++i) {
        const int id = redoPath[static_cast<size_t>(i)];
        // Restore camera only on the very last redo step.
        m_restoreCamera = (i == static_cast<int>(redoPath.size()) - 1) ? restoreCamera : false;
        // Set the preferred child so redo() follows the correct branch.
        m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].preferredChild = id;
        if (!redo()) {
            m_restoreCamera = true;
            m_suppressUndoRedoSignals = false;
            emitStateChanged();
            return false;
        }
    }
    // If there were no redo steps (target == lca), the undo steps ran with
    // m_restoreCamera=false so render modes were restored but camera was not.
    // Now explicitly re-apply the view with the correct restoreCamera value.
    if (redoPath.empty()) {
        m_doc.restoreViewState(m_undoNodes[static_cast<size_t>(m_undoCurrentNode)].state.viewState, restoreCamera);
    }

    m_restoreCamera = true;
    m_suppressUndoRedoSignals = false;
    emitStateChanged();
    return m_undoCurrentNode == nodeId;
}

bool DocumentUndoManager::undo()
{
    if (!canUndo() || m_undoStepActive)
        return false;

    const auto &node = m_undoNodes[static_cast<size_t>(m_undoCurrentNode)];
    const int parentId = node.parentId;
    const auto &target = m_undoNodes[static_cast<size_t>(parentId)];
    qDebug() << "[UNDO]  from=" << m_undoCurrentNode << '(' << node.label << ')'
             << "to=" << parentId << '(' << target.label << ')';

    if (node.storageKind == UndoStorageKind::Delta && node.beforeSelection.has_value()) {
        // Delta undo: revert selection on live mesh, no full state restore.
        m_restoringUndoRedo = true;
        m_doc.applySelectionDelta(*node.beforeSelection);
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
    qDebug() << "[REDO]  from=" << m_undoCurrentNode << '(' << node.label << ')'
             << "to=" << childId << '(' << target.label << ')'
             << "prefChild=" << node.preferredChild
             << "children=" << QVector<int>(node.children.begin(), node.children.end());
    if (target.storageKind == UndoStorageKind::Delta && target.afterSelection.has_value()) {
        // Delta redo: apply after-selection on live mesh, no full state restore.
        m_restoringUndoRedo = true;
        m_doc.applySelectionDelta(*target.afterSelection);
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
    m_undoCurrentNode = -1;
    m_undoStepActive = false;
    m_undoStepLabel.clear();
    m_pendingUndoBefore.reset();
    m_pendingDeltaBefore.reset();
    m_pendingDeltaMeshIndex.reset();
    m_pendingMemoryPressure = 0;
    emitStateChanged();
}

bool DocumentUndoManager::makeRoot(int nodeId)
{
    if (nodeId < 0 || nodeId >= static_cast<int>(m_undoNodes.size()))
        return false;
    if (m_undoStepActive)
        return false;
    // Already the only node (trivial root) – nothing to do.
    if (m_undoNodes[static_cast<size_t>(nodeId)].parentId < 0 &&
        m_undoNodes.size() == 1)
        return false;

    // Collect all nodes reachable from nodeId (the node itself + all descendants).
    std::vector<int> reachable;
    {
        std::vector<int> stack = { nodeId };
        while (!stack.empty()) {
            const int id = stack.back(); stack.pop_back();
            reachable.push_back(id);
            for (int c : m_undoNodes[static_cast<size_t>(id)].children)
                stack.push_back(c);
        }
    }
    std::sort(reachable.begin(), reachable.end());

    // If the current node is not in the surviving subtree, reset it to nodeId.
    const bool currentSurvives =
        std::binary_search(reachable.begin(), reachable.end(), m_undoCurrentNode);
    const int effectiveCurrent = currentSurvives ? m_undoCurrentNode : nodeId;

    // Compact: remap old ids → new compact ids.
    std::map<int, int> remap;
    for (int ni = 0; ni < static_cast<int>(reachable.size()); ++ni)
        remap[reachable[ni]] = ni;

    std::vector<UndoNode> compacted;
    compacted.reserve(reachable.size());
    for (int oldId : reachable) {
        UndoNode n = std::move(m_undoNodes[static_cast<size_t>(oldId)]);
        n.parentId = (n.parentId >= 0 && remap.count(n.parentId)) ? remap[n.parentId] : -1;
        std::vector<int> newChildren;
        for (int c : n.children)
            if (remap.count(c)) newChildren.push_back(remap[c]);
        n.children = std::move(newChildren);
        if (n.preferredChild >= 0 && remap.count(n.preferredChild))
            n.preferredChild = remap[n.preferredChild];
        else
            n.preferredChild = n.children.empty() ? -1 : n.children.back();
        compacted.push_back(std::move(n));
    }
    // The new root (nodeId) has no parent and no label (it is a root state).
    UndoNode &newRoot = compacted[remap[nodeId]];
    newRoot.parentId = -1;
    newRoot.label.clear();
    // Actions that produced the new baseline must not be replayed. Informational
    // calls made afterwards remain valid and stay in trailingActionRecords.
    newRoot.prefixActionRecords.clear();
    newRoot.actionRecord.reset();

    m_undoNodes = std::move(compacted);
    m_undoCurrentNode = remap.count(effectiveCurrent) ? remap[effectiveCurrent] : remap[nodeId];
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

    // Collect the set of strictly descendant ids (not including nodeId itself).
    std::set<int> descendants;
    {
        std::vector<int> stack;
        for (int c : m_undoNodes[static_cast<size_t>(nodeId)].children)
            stack.push_back(c);
        while (!stack.empty()) {
            const int id = stack.back(); stack.pop_back();
            descendants.insert(id);
            for (int c : m_undoNodes[static_cast<size_t>(id)].children)
                stack.push_back(c);
        }
    }

    // Collect surviving nodes: everything that is not a descendant of nodeId.
    std::vector<int> reachable;
    for (int i = 0; i < static_cast<int>(m_undoNodes.size()); ++i)
        if (!descendants.count(i))
            reachable.push_back(i);
    // reachable is already sorted (sequential iteration).

    // The root is always index 0 and is never a descendant, so it always survives.
    const int rootId = reachable.front(); // always 0

    // If the current node was a descendant, reset it to nodeId.
    const bool currentSurvives = !descendants.count(m_undoCurrentNode);
    const int effectiveCurrent = currentSurvives ? m_undoCurrentNode : nodeId;

    // Compact: remap old ids → new compact ids.
    std::map<int, int> remap;
    for (int ni = 0; ni < static_cast<int>(reachable.size()); ++ni)
        remap[reachable[ni]] = ni;

    std::vector<UndoNode> compacted;
    compacted.reserve(reachable.size());
    for (int oldId : reachable) {
        UndoNode n = std::move(m_undoNodes[static_cast<size_t>(oldId)]);
        n.parentId = (n.parentId >= 0 && remap.count(n.parentId)) ? remap[n.parentId] : -1;
        std::vector<int> newChildren;
        for (int c : n.children)
            if (remap.count(c)) newChildren.push_back(remap[c]);
        n.children = std::move(newChildren);
        if (n.preferredChild >= 0 && remap.count(n.preferredChild))
            n.preferredChild = remap[n.preferredChild];
        else
            n.preferredChild = n.children.empty() ? -1 : n.children.back();
        compacted.push_back(std::move(n));
    }
    compacted[remap[rootId]].parentId = -1;

    m_undoNodes = std::move(compacted);
    m_undoCurrentNode = remap.count(effectiveCurrent) ? remap[effectiveCurrent] : remap[nodeId];
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

    // Keep only path nodes, sorted ascending (they already are after the reverse).
    std::sort(path.begin(), path.end());

    // Remap old ids → new compact ids (0 = root, 1 = next, …, n-1 = current).
    std::map<int, int> remap;
    for (int ni = 0; ni < static_cast<int>(path.size()); ++ni)
        remap[path[ni]] = ni;

    std::vector<UndoNode> compacted;
    compacted.reserve(path.size());
    for (int oldId : path) {
        UndoNode n = std::move(m_undoNodes[static_cast<size_t>(oldId)]);
        n.parentId = (n.parentId >= 0 && remap.count(n.parentId)) ? remap[n.parentId] : -1;
        // Keep only the single child that is also on the path.
        std::vector<int> newChildren;
        for (int c : n.children)
            if (remap.count(c)) newChildren.push_back(remap[c]);
        n.children = std::move(newChildren);
        n.preferredChild = n.children.empty() ? -1 : n.children.front();
        compacted.push_back(std::move(n));
    }
    compacted[0].parentId = -1; // root has no parent

    m_undoNodes = std::move(compacted);
    m_undoCurrentNode = remap[m_undoCurrentNode];
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

    qint64 currentBytes = memoryStats().totalBytes();
    while (currentBytes > targetBytes && canUndo()) {
        std::vector<int> path;
        int id = m_undoCurrentNode;
        while (id >= 0) {
            path.push_back(id);
            id = m_undoNodes[static_cast<size_t>(id)].parentId;
        }
        std::reverse(path.begin(), path.end());

        // A delta node cannot become a root because it has no complete document
        // state. Keep at least one transition and root only at a full snapshot.
        int newRoot = -1;
        for (int i = 1; i + 1 < static_cast<int>(path.size()); ++i) {
            if (m_undoNodes[static_cast<size_t>(path[i])].storageKind
                == UndoStorageKind::FullSnapshot) {
                newRoot = path[i];
                break;
            }
        }
        if (newRoot < 0 || !makeRoot(newRoot))
            break;

        // Shared geometry can make one root advance reclaim zero bytes while a
        // later advance releases an old revision. The path is strictly shorter,
        // so continuing is finite even when this individual step has no effect.
        currentBytes = memoryStats().totalBytes();
    }

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

    {
        const auto &n = m_undoNodes[static_cast<size_t>(newId)];
        qDebug() << "[STATE SAVED]" << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                 << "idx=" << newId
                 << "parent=" << n.parentId
                 << "label=" << n.label;
    }

    pruneTreeToLimit();
    enforceConfiguredMemoryLimit();
    emitStateChanged();
    applyDeferredMemoryPressure();
}

void DocumentUndoManager::pushDeltaStep(
    const QString &label,
    SelectionDelta &&before,
    SelectionDelta &&after,
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
    child.beforeSelection = std::move(before);
    child.afterSelection = std::move(after);
    if (scriptAction.has_value())
        child.actionRecord = UndoActionRecord::fromScriptAction(*scriptAction);

    m_undoNodes.push_back(std::move(child));
    auto &parent = m_undoNodes[static_cast<size_t>(m_undoCurrentNode)];
    parent.children.push_back(newId);
    parent.preferredChild = newId;
    m_undoCurrentNode = newId;

    {
        const auto &n = m_undoNodes[static_cast<size_t>(newId)];
        qDebug() << "[STATE SAVED]" << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
                 << "idx=" << newId
                 << "parent=" << n.parentId
                 << "label=" << n.label
                 << "(selection delta)";
    }

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
    const int stepsToRemove = depth - 1 - m_undoLimit;
    int newRoot = m_undoCurrentNode;
    for (int i = 0; i < m_undoLimit; ++i)
        newRoot = m_undoNodes[static_cast<size_t>(newRoot)].parentId;
    // newRoot is the node to become the new root; everything above it is discarded.
    // Use a BFS/DFS to mark all reachable node ids from newRoot; drop the rest.
    // We do this by building a remapping of old ids → new compact ids.
    std::vector<int> reachable;
    {
        std::vector<int> stack = { newRoot };
        while (!stack.empty()) {
            const int id = stack.back(); stack.pop_back();
            reachable.push_back(id);
            for (int c : m_undoNodes[static_cast<size_t>(id)].children)
                stack.push_back(c);
        }
    }
    std::sort(reachable.begin(), reachable.end());
    // Build old→new id map.
    std::map<int,int> remap;
    for (int ni = 0; ni < static_cast<int>(reachable.size()); ++ni)
        remap[reachable[ni]] = ni;

    std::vector<UndoNode> pruned;
    pruned.reserve(reachable.size());
    for (int oldId : reachable) {
        UndoNode n = std::move(m_undoNodes[static_cast<size_t>(oldId)]);
        n.parentId = (n.parentId >= 0 && remap.count(n.parentId)) ? remap[n.parentId] : -1;
        std::vector<int> newChildren;
        for (int c : n.children)
            if (remap.count(c))
                newChildren.push_back(remap[c]);
        n.children = std::move(newChildren);
        if (n.preferredChild >= 0 && remap.count(n.preferredChild))
            n.preferredChild = remap[n.preferredChild];
        else
            n.preferredChild = n.children.empty() ? -1 : n.children.back();
        pruned.push_back(std::move(n));
    }
    pruned[0].parentId = -1;  // new root has no parent
    m_undoCurrentNode = remap[m_undoCurrentNode];
    m_undoNodes = std::move(pruned);
    (void)stepsToRemove;
}

void DocumentUndoManager::emitStateChanged()
{
    if (m_suppressUndoRedoSignals)
        return;
    m_doc.undoRedoStateChanged(canUndo(), canRedo(), undoText(), redoText());
}

UndoMemoryStats DocumentUndoManager::memoryStats() const
{
    UndoMemoryStats stats;
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
        info.selectionDelta = node.storageKind == UndoStorageKind::Delta;
        if (info.selectionDelta) {
            if (node.beforeSelection)
                info.selectionBytes += selectionDeltaBytes(*node.beforeSelection);
            if (node.afterSelection)
                info.selectionBytes += selectionDeltaBytes(*node.afterSelection);
        } else {
            info.referencedGeometryBytes = referencedGeometryBytes(node.state);
        }
        stats.steps.push_back(info);
    }

    std::set<const VCGMesh *> seenGeometry;
    for (const auto &node : m_undoNodes) {
        accountGeometry(node.state, seenGeometry, stats.geometryBytes, &stats.uniqueGeometryCount);
        if (node.beforeSelection)
            stats.selectionBytes += selectionDeltaBytes(*node.beforeSelection);
        if (node.afterSelection)
            stats.selectionBytes += selectionDeltaBytes(*node.afterSelection);
    }

    // QImage copies in snapshots are implicit shares. Seed the set with live
    // document images so the undo total reflects buffers that purging can
    // actually release, rather than counting references to live storage.
    std::set<const uchar *> seenImages;
    qint64 ignoredLiveImageBytes = 0;
    for (int i = 0; i < m_doc.meshCount(); ++i) {
        const Document::MeshEntry &entry = m_doc.mesh(i);
        accountTextureAssets(entry.textureAssets, seenImages, ignoredLiveImageBytes);
        accountTextureAssets(entry.materialSet.textureAssets, seenImages, ignoredLiveImageBytes);
    }
    for (int i = 0; i < m_doc.rasterCount(); ++i) {
        for (const RasterPlane &plane : m_doc.raster(i).planes)
            accountImage(plane.image, seenImages, ignoredLiveImageBytes);
    }
    for (const UndoNode &node : m_undoNodes) {
        accountStateImages(
            node.state,
            seenImages,
            stats.historyImageBytes,
            &stats.uniqueHistoryImageCount);
    }

    if (m_pendingUndoBefore) {
        accountGeometry(
            *m_pendingUndoBefore,
            seenGeometry,
            stats.pendingGeometryBytes);
        accountStateImages(
            *m_pendingUndoBefore,
            seenImages,
            stats.pendingImageBytes);
    }
    if (m_pendingDeltaBefore)
        stats.pendingSelectionBytes = selectionDeltaBytes(*m_pendingDeltaBefore);
    return stats;
}
