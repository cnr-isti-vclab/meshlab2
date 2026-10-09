#include "document_internal.h"
#include "documentundomanager.h"

using namespace DocumentInternal;

void Document::beginUndoStep(const QString &label)
{
    // Named type, not {}: against these overloads a braced-init-list converts to int
    // ahead of ScriptAction, so `{}` silently selected the selection-delta overload and
    // turned every plain step -- Add Mesh, Rename, Open Meshes -- into a delta on mesh 0
    // that carries no snapshot to restore from.
    beginUndoStep(label, ScriptAction{});
}

void Document::beginUndoStep(
    const QString &label,
    const ScriptAction &scriptAction)
{
    m_undoManager->beginStep(label, scriptAction);
}

void Document::beginUndoStep(
    const QString &label,
    int meshIndexForDelta,
    std::uint32_t kinds)
{
    m_undoManager->beginDeltaStep(label, meshIndexForDelta, kinds);
}

void Document::beginUndoStep(
    const QString &label,
    const ScriptAction &scriptAction,
    int meshIndexForDelta,
    std::uint32_t kinds)
{
    m_undoManager->beginDeltaStep(label, meshIndexForDelta, kinds, scriptAction);
}

void Document::endUndoStep(bool commit, bool restoreOnCancel)
{
    m_undoManager->endStep(commit, restoreOnCancel);
}

bool Document::canUndo() const
{
    return m_undoManager->canUndo();
}

bool Document::isRestoringUndoRedo() const
{
    return m_undoManager->isRestoring();
}

bool Document::undoStepActive() const
{
    return m_undoManager->isStepActive();
}

void Document::setViewStateFunctions(
    std::function<ViewState()> capture,
    std::function<void(const ViewState &, bool)> restore)
{
    m_captureViewState = std::move(capture);
    m_restoreViewState = std::move(restore);
}

void Document::restoreViewState(const ViewState &state, bool restoreCamera)
{
    if (m_restoreViewState)
        m_restoreViewState(state, restoreCamera);
}

void Document::setRenderStateSnapshotFunction(
    std::function<bool(const QString &, const QSize &, QImage &, CameraShot &, QString &)> capture)
{
    m_captureRenderStateSnapshot = std::move(capture);
}

bool Document::renderSnapshotFromStateJson(
    const QString &renderStateJson,
    const QSize &pixelSize,
    QImage &outImage,
    CameraShot &outShot,
    QString *errorMessage) const
{
    if (!m_captureRenderStateSnapshot) {
        if (errorMessage)
            *errorMessage = tr("No render-state snapshot provider is available.");
        return false;
    }

    QString localError;
    const bool ok = m_captureRenderStateSnapshot(
        renderStateJson,
        pixelSize,
        outImage,
        outShot,
        localError);
    if (!ok && errorMessage)
        *errorMessage = localError;
    if (ok && errorMessage)
        errorMessage->clear();
    return ok;
}

bool Document::updateUndoNodeCamera(int nodeId)
{
    if (!m_captureViewState)
        return false;
    return m_undoManager->updateNodeCamera(nodeId, m_captureViewState());
}

bool Document::canRedo() const
{
    return m_undoManager->canRedo();
}

QString Document::undoText() const
{
    return m_undoManager->undoText();
}

QString Document::redoText() const
{
    return m_undoManager->redoText();
}

QStringList Document::undoHistoryLabels() const
{
    return m_undoManager->undoHistoryLabels();
}

QStringList Document::undoStackLabels() const
{
    return m_undoManager->undoStackLabels();
}

int Document::undoCursorPosition() const
{
    return m_undoManager->undoCursorPosition();
}

int Document::undoCurrentNodeId() const
{
    return m_undoManager->currentNodeId();
}

std::vector<UndoTreeNodeInfo> Document::undoTreeInfo() const
{
    return m_undoManager->undoTreeInfo();
}

bool Document::jumpToUndoNode(int nodeId, bool restoreCamera)
{
    return m_undoManager->jumpToNode(nodeId, restoreCamera);
}

bool Document::undo()
{
    return m_undoManager->undo();
}

bool Document::redo()
{
    return m_undoManager->redo();
}

void Document::clearUndoHistory()
{
    m_undoManager->clear();
}

bool Document::makeUndoRoot(int nodeId)
{
    return m_undoManager->makeRoot(nodeId);
}

bool Document::purgeUndoBranch(int nodeId)
{
    return m_undoManager->purgeBranch(nodeId);
}

bool Document::linearizeUndoHistory()
{
    return m_undoManager->linearizeHistory();
}

void Document::setUndoLimit(int limit)
{
    m_undoManager->setUndoLimit(limit);
}

int Document::undoLimit() const
{
    return m_undoManager->undoLimit();
}

qint64 Document::undoMemoryLimitBytes() const
{
    return m_undoManager->undoMemoryLimitBytes();
}

void Document::setUndoMemoryLimitBytes(qint64 bytes)
{
    m_undoManager->setUndoMemoryLimitBytes(bytes);
}

UndoPruneResult Document::pruneUndoToMemoryBudget(qint64 maximumBytes)
{
    return m_undoManager->pruneToMemoryBudget(maximumBytes);
}

void Document::handleUndoMemoryPressure(bool critical)
{
    m_undoManager->handleMemoryPressure(critical);
}

void Document::setSuppressUndo(bool s)
{
    m_undoManager->setSuppressUndo(s);
}

std::optional<ScriptAction> Document::undoNodeAction(int nodeId) const
{
    return m_undoManager->nodeAction(nodeId);
}

std::vector<ScriptAction> Document::undoNodeScriptActions(int nodeId) const
{
    return m_undoManager->nodeScriptActions(nodeId);
}

void Document::recordScriptAction(const ScriptAction &scriptAction)
{
    m_undoManager->recordScriptAction(scriptAction);
}
