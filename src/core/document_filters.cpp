#include "document_internal.h"

using namespace DocumentInternal;

QStringList Document::loadedPluginSummaries() const
{
    return m_pluginManager->loadedPluginSummaries();
}

QStringList Document::loadedFilterPluginSummaries() const
{
    if (!m_filterPluginManager)
        return {};
    return m_filterPluginManager->loadedPluginSummaries();
}

std::vector<Document::FilterInfo> Document::filterInfos() const
{
    std::vector<FilterInfo> infos;
    if (!m_filterPluginManager)
        return infos;

    const auto managedInfos = m_filterPluginManager->filterInfos(*this);
    infos.reserve(managedInfos.size());
    for (const auto &managedInfo : managedInfos) {
        FilterInfo info;
        info.key = managedInfo.key;
        info.pluginId = managedInfo.pluginId;
        info.pluginName = managedInfo.pluginName;
        info.descriptor = managedInfo.descriptor;
        info.applicable = managedInfo.applicable;
        info.applicabilityError = managedInfo.applicabilityError;
        infos.push_back(std::move(info));
    }
    return infos;
}

bool Document::validateFilterInvocation(
    const QString &filterKey,
    const MeshFilterParameterValues &parameters,
    QString &errorMessage) const
{
    if (!m_filterPluginManager) {
        errorMessage = tr("Filter manager is not available.");
        return false;
    }
    return m_filterPluginManager->validateFilterInvocation(filterKey, parameters, *this, errorMessage);
}

MeshFilterRunResult Document::runFilter(
    const QString &filterKey,
    const MeshFilterParameterValues &parameters)
{
    if (!m_filterPluginManager) {
        return {
            false,
            false,
            tr("No filter plugin manager is available.")
        };
    }

    // Resolved before the run: a filter may add, remove or rename meshes, and the
    // completion line should carry the filter's display name rather than its internal key.
    const auto info = m_filterPluginManager->filterInfo(filterKey, *this);
    const QString filterName = info ? info->descriptor.name : filterKey;

    // Bracket the whole run: most filters call doc.progressCallback() without ever
    // calling beginFilterProgress, so without this their callback mode would stay
    // None and their progress would not be reported at all. Done without emitting the
    // started/finished signals, so filters that *do* drive the lifecycle themselves
    // still own the progress bar.
    Document *previousCallbackDocument = g_callbackDocument;
    const CallbackMode previousCallbackMode = m_callbackMode;
    g_callbackDocument = this;
    m_callbackMode = CallbackMode::Filter;

    QElapsedTimer timer;
    timer.start();
    MeshFilterRunResult result = m_filterPluginManager->runFilter(filterKey, parameters, *this);
    const double elapsedMs = double(timer.nsecsElapsed()) / 1e6;

    m_callbackMode = previousCallbackMode;
    g_callbackDocument = previousCallbackDocument;
    // The transient progress line must never outlive its run, including for filters
    // that never called finishFilterProgress.
    clearProgressLog();

    // Reported here rather than in the callers so that every entry point gets it — the
    // filters menu, the filter panel's Apply, the Python API and the interactive tools all
    // funnel through this one call. Info, not Debug: how long a filter took is something
    // the user always wants, and the run is over so it costs one line.
    const QString elapsedText = QString::number(elapsedMs, 'f', 2);
    writeLog(
        result.success
            ? tr("Filter '%1' completed in %2 ms").arg(filterName, elapsedText)
            : tr("Filter '%1' failed after %2 ms").arg(filterName, elapsedText),
        LogSource::Application);
    return result;
}

Document::MultiMeshFilterResult Document::runFilterOnVisibleMeshes(
    const QString &filterKey,
    const MeshFilterParameterValues &parameters)
{
    MultiMeshFilterResult out;

    if (!m_filterPluginManager) {
        out.errorMessage = tr("No filter plugin manager is available.");
        return out;
    }

    const auto info = m_filterPluginManager->filterInfo(filterKey, *this);
    const QString filterName = info ? info->descriptor.name : filterKey;

    // The sweep covers the layers visible when it starts, identified by mesh id rather
    // than by index. A filter may add or remove layers underneath us -- 55 of them take a
    // single mesh and emit new ones -- so a live meshCount() bound would feed the sweep
    // its own output, and "Duplicate Current Layer" would never terminate.
    std::vector<std::uint64_t> targetIds;
    for (int mi = 0; mi < meshCount(); ++mi) {
        if (mesh(mi).visible)
            targetIds.push_back(mesh(mi).meshId);
    }
    out.targetCount = int(targetIds.size());
    if (targetIds.empty()) {
        out.errorMessage = tr("There is no visible layer to apply '%1' to.").arg(filterName);
        return out;
    }

    const int previousCurrent = m_currentMeshIndex;
    const std::uint64_t previousCurrentId =
        (previousCurrent >= 0 && previousCurrent < meshCount()) ? mesh(previousCurrent).meshId : 0;

    // One undo entry for the whole sweep. The nested runFilter() calls below see this
    // step and decline to open their own (MeshFilterPluginManager::runFilter).
    const bool ownUndoStep = !isRestoringUndoRedo() && !undoStepActive();
    if (ownUndoStep)
        beginUndoStep(tr("%1 (all visible layers)").arg(filterName));

    for (const std::uint64_t meshId : targetIds) {
        const int mi = indexOfMeshId(meshId);
        if (mi < 0)
            continue; // the layer was removed by an earlier iteration of this sweep
        const QString layerName = mesh(mi).name;
        {
            // Narrowly scoped: only the layer switch is silenced. The filters themselves
            // must still emit, or no view would learn what they changed.
            const QSignalBlocker blocker(this);
            setCurrentMeshIndex(mi);
        }

        QString reason;
        if (!validateFilterInvocation(filterKey, parameters, reason)) {
            out.skipped.push_back({mi, layerName, reason});
            continue;
        }

        const MeshFilterRunResult result = runFilter(filterKey, parameters);
        if (!result.success) {
            out.skipped.push_back({mi, layerName, result.errorMessage});
            continue;
        }

        ++out.appliedCount;
        out.documentModified = out.documentModified || result.documentModified;
        for (const int newIndex : result.newMeshIndices)
            out.newMeshIndices.push_back(newIndex);
    }

    // Put the user back on the layer they had selected, before the step is committed so
    // that the recorded "after" state carries their selection rather than whichever
    // layer happened to come last.
    const int restored = previousCurrentId ? indexOfMeshId(previousCurrentId) : -1;
    setCurrentMeshIndex(restored >= 0 ? restored : (meshCount() > 0 ? 0 : -1));

    if (ownUndoStep)
        endUndoStep(out.documentModified);

    out.success = out.appliedCount > 0;
    if (!out.success)
        out.errorMessage = tr("'%1' could not be applied to any visible layer.").arg(filterName);

    // A skipped layer used to vanish without a word, which made a partial sweep look
    // exactly like a complete one.
    if (!out.skipped.isEmpty()) {
        writeLog(
            tr("Filter '%1' applied to %2 of %3 visible layers")
                .arg(filterName)
                .arg(out.appliedCount)
                .arg(out.targetCount),
            LogSource::Application,
            LogLevel::Warning);
        for (const MultiMeshFilterResult::SkippedLayer &skip : out.skipped) {
            writeLog(
                tr("Skipped layer '%1': %2").arg(skip.layerName, skip.reason),
                LogSource::Application,
                LogLevel::Warning);
        }
    }
    return out;
}

vcg::CallBackPos *Document::progressCallback()
{
    return logCallback();
}

void Document::beginFilterProgress(const QString &label)
{
    m_lastProgressPos = -1;
    m_loadCallbackCount = 0;
    m_loadProgressEmitCount = 0;
    m_loadProcessEventsCount = 0;
    m_loadProcessEventsNs = 0;
    m_lastProgressEmitMs = -1;
    m_lastProcessEventsMs = -1;
    m_loadCallbackTimer.invalidate();
    m_loadCallbackTimer.start();
    m_cancelRequested.store(false, std::memory_order_relaxed);
    m_callbackMode = CallbackMode::Filter;
    // Drop any stale progress line left by an earlier operation.
    clearProgressLog();

    const QString normalizedLabel = label.trimmed().isEmpty() ? tr("Filter") : label.trimmed();
    emit filterProgressStarted(normalizedLabel);
    emit filterProgressUpdated(0, normalizedLabel);
}

void Document::finishFilterProgress(bool success, const QString &message)
{
    const QString normalizedMessage = message.trimmed();
    m_callbackMode = CallbackMode::None;
    m_cancelRequested.store(false, std::memory_order_relaxed);
    // The operation is over, so its progress line goes away; whatever the filter
    // reports lands as ordinary log entries instead.
    clearProgressLog();
    emit filterProgressFinished(success, normalizedMessage);
}

void Document::requestOperationCancel()
{
    m_cancelRequested.store(true, std::memory_order_relaxed);
}

bool Document::isOperationCancelRequested() const
{
    return m_cancelRequested.load(std::memory_order_relaxed);
}

