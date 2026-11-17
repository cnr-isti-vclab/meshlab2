#pragma once

#include "document_undo_types.h"

#include <array>
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
    void setRestoring(bool restoring) { m_restoringUndoRedo = restoring; }
    void emitStateChanged();

    // Key: (meshId, geometryRevision, materialRevision) — the content identity of a mesh
    // apart from its selection, colors and scalars, which states keep as layers of their
    // own (attributeCache), so that changing one of them does not cost the next state a
    // copy of the mesh. Material covers textures and custom attributes: the Define Custom
    // Attribute filters bump only it, and without it in the key the "after" checkpoint
    // reused the "before" copy, so redo lost the attribute. A copy remembers the attribute
    // revisions it was taken at (MeshSnapshot::geometryLayerRevisions).
    using GeometryKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    struct GeometryCopy {
        std::weak_ptr<const VCGMesh> mesh;
        std::array<std::uint64_t, 3> layerRevisions {};
    };
    std::map<GeometryKey, GeometryCopy> &geometryCache() { return m_undoGeometryCache; }
    // Key: (meshId, geometryRevision, MeshAttributeKind, that kind's revision). A layer
    // holds only the values that differ from its geometry copy, hence the geometry revision.
    using AttributeKey = std::tuple<std::uint64_t, std::uint64_t, std::uint32_t, std::uint64_t>;
    std::map<AttributeKey, std::weak_ptr<const MeshAttributeLayer>> &attributeCache()
    {
        return m_undoAttributeCache;
    }

    const std::vector<UndoNode> &nodes() const { return m_undoNodes; }
    int currentNode() const { return m_undoCurrentNode; }

private:
    void pushStep(
        const QString &label,
        UndoState &&before,
        UndoState &&after,
        std::optional<ScriptAction> scriptAction = {});
    void pruneTreeToLimit();
    std::vector<int> subtreeNodes(int nodeId) const;
    void compactNodes(const std::vector<int> &keepSorted);
    void rootAt(int nodeId);
    UndoPruneResult pruneToMemoryBudgetInternal(
        qint64 triggerBytes,
        qint64 targetBytes,
        bool allowClear);
    UndoPruneResult enforceConfiguredMemoryLimit();
    void applyDeferredMemoryPressure();
    void logAutomaticPrune(const UndoPruneResult &result, const QString &reason);

    Document &m_doc;
    std::map<GeometryKey, GeometryCopy> m_undoGeometryCache;
    std::map<AttributeKey, std::weak_ptr<const MeshAttributeLayer>> m_undoAttributeCache;
    std::vector<UndoNode> m_undoNodes;
    int m_undoCurrentNode = -1;
    int m_undoLimit = 20;
    qint64 m_undoMemoryLimitBytes = 0;
    int m_pendingMemoryPressure = 0; // 0 none, 1 warning, 2 critical
    bool m_undoStepActive = false;
    QString m_undoStepLabel;
    std::optional<ScriptAction> m_pendingScriptAction;
    std::optional<UndoState> m_pendingUndoBefore;
    bool m_restoringUndoRedo = false;
    bool m_suppressUndo = false;
    bool m_suppressUndoRedoSignals = false;
    bool m_restoreCamera = true;
};
