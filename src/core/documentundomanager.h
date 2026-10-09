#pragma once

#include "document_undo_types.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

class Document;

class DocumentUndoManager
{
public:
    explicit DocumentUndoManager(Document &doc);

    void beginStep(const QString &label, const ScriptAction &scriptAction = {});
    void endStep(bool commit = true, bool restoreOnCancel = false);

    // Attribute delta steps — skip full snapshot capture for a change confined to the
    // attribute classes in `kinds`. An optional scriptAction is recorded on the node so
    // such filters stay reproducible/scriptable despite the lightweight storage.
    void beginDeltaStep(const QString &label, int meshIndex, std::uint32_t kinds,
                        std::optional<ScriptAction> scriptAction = std::nullopt);
    void endDeltaStep();

    bool canUndo() const;
    bool canRedo() const;
    QString undoText() const;
    QString redoText() const;
    bool isRestoring() const { return m_restoringUndoRedo; }
    bool isStepActive() const { return m_undoStepActive; }
    void setSuppressUndo(bool suppress) { m_suppressUndo = suppress; }
    int undoLimit() const { return m_undoLimit; }
    void setUndoLimit(int limit);
    qint64 undoMemoryLimitBytes() const { return m_undoMemoryLimitBytes; }
    void setUndoMemoryLimitBytes(qint64 bytes);
    UndoPruneResult pruneToMemoryBudget(qint64 maximumBytes);
    void handleMemoryPressure(bool critical);

    QStringList undoHistoryLabels() const;
    QStringList undoStackLabels() const;
    int undoCursorPosition() const;
    int currentNodeId() const { return m_undoCurrentNode; }
    std::vector<UndoTreeNodeInfo> undoTreeInfo() const;
    std::vector<ScriptAction> nodeScriptActions(int nodeId) const;
    // Just the state-changing action, without the informational calls around it.
    std::optional<ScriptAction> nodeAction(int nodeId) const;
    void recordScriptAction(const ScriptAction &scriptAction);

    bool jumpToNode(int nodeId, bool restoreCamera = true);
    bool updateNodeCamera(int nodeId, const ViewState &viewState);
    bool makeRoot(int nodeId);
    bool purgeBranch(int nodeId);
    bool linearizeHistory();
    bool undo();
    bool redo();
    void clear();

    UndoMemoryStats memoryStats() const;

    bool restoreCamera() const { return m_restoreCamera; }
    bool suppressSignals() const { return m_suppressUndoRedoSignals; }
    void setRestoring(bool restoring) { m_restoringUndoRedo = restoring; }
    void emitStateChanged();

    // Key: (meshId, geometryRevision, selectionRevision) — the full content
    // identity of a mesh, so selection-only changes still get their own interned
    // copy even though they no longer bump geometryRevision.
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::weak_ptr<const VCGMesh>> &geometryCache()
    {
        return m_undoGeometryCache;
    }

    const std::vector<UndoNode> &nodes() const { return m_undoNodes; }
    int currentNode() const { return m_undoCurrentNode; }

private:
    void pushStep(
        const QString &label,
        UndoState &&before,
        UndoState &&after,
        std::optional<ScriptAction> scriptAction = {});
    void pushDeltaStep(
        const QString &label,
        MeshAttributeDelta &&before,
        MeshAttributeDelta &&after,
        std::optional<ScriptAction> scriptAction = {});
    void pruneTreeToLimit();
    UndoPruneResult pruneToMemoryBudgetInternal(
        qint64 triggerBytes,
        qint64 targetBytes,
        bool allowClear);
    UndoPruneResult enforceConfiguredMemoryLimit();
    void applyDeferredMemoryPressure();
    void logAutomaticPrune(const UndoPruneResult &result, const QString &reason);

    Document &m_doc;
    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, std::weak_ptr<const VCGMesh>> m_undoGeometryCache;
    std::vector<UndoNode> m_undoNodes;
    int m_undoCurrentNode = -1;
    int m_undoLimit = 20;
    qint64 m_undoMemoryLimitBytes = 0;
    int m_pendingMemoryPressure = 0; // 0 none, 1 warning, 2 critical
    bool m_undoStepActive = false;
    QString m_undoStepLabel;
    std::optional<ScriptAction> m_pendingScriptAction;
    std::optional<UndoState> m_pendingUndoBefore;
    std::optional<MeshAttributeDelta> m_pendingDeltaBefore;
    std::optional<int> m_pendingDeltaMeshIndex;
    std::uint32_t m_pendingDeltaKinds = MeshAttributeNone;
    bool m_restoringUndoRedo = false;
    bool m_suppressUndo = false;
    bool m_suppressUndoRedoSignals = false;
    bool m_restoreCamera = true;
};
