#include "document_internal.h"

#include <QDateTime>

using namespace DocumentInternal;

namespace {
// Latched during static initialisation rather than on the first log call, so an elapsed
// timestamp really is measured from process start.
const qint64 g_applicationStartEpochMs = QDateTime::currentMSecsSinceEpoch();
} // namespace

qint64 Document::applicationStartMSecsSinceEpoch()
{
    return g_applicationStartEpochMs;
}

void Document::clearLog()
{
    if (m_logMessages.empty())
        return;

    m_logMessages.clear();
    m_progressLogActive = false;
    emit logCleared();
}

void Document::clearProgressLog()
{
    if (!m_progressLogActive)
        return;
    m_progressLogActive = false;
    // Only remove it if it really is still the last entry. writeLog maintains that
    // invariant, but refusing to pop otherwise means a future code path that appends
    // behind our back costs a stale progress line rather than a deleted real message.
    if (m_logMessages.size() != m_progressLogIndex + 1)
        return;
    m_logMessages.pop_back();
    emit logLastEntryRemoved(static_cast<int>(m_progressLogIndex));
}

// Shared tail of writeLog and the progress line: normalise, timestamp, store, notify.
void Document::appendOrReplaceLog(const QString &message, LogSource source, LogLevel level, bool replaceLast)
{
    QString normalizedMessage = message;
    if (!replaceLast && normalizedMessage.endsWith('\r'))
        replaceLast = true;

    while (!normalizedMessage.isEmpty() && (normalizedMessage.endsWith('\n') || normalizedMessage.endsWith('\r')))
        normalizedMessage.chop(1);
    normalizedMessage = normalizedMessage.trimmed();

    if (normalizedMessage.isEmpty())
        return;

    // Timestamped but not prefixed: the stamp travels alongside the text so the view can
    // format or omit it, and so assertions and searches see the message itself.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (replaceLast && !m_logMessages.empty()) {
        m_logMessages.back() = LogEntry{normalizedMessage, source, level, nowMs};
    } else {
        m_logMessages.push_back(LogEntry{normalizedMessage, source, level, nowMs});
    }

    emit logMessageAdded(normalizedMessage, source, level, replaceLast);
}

void Document::writeLog(const QString &message, LogSource source, LogLevel level, bool replaceLast)
{
    // Keep the invariant that the transient progress line is the last entry: a real
    // message removes it, and the next progress tick appends a fresh one at the end.
    clearProgressLog();
    appendOrReplaceLog(message, source, level, replaceLast);
}

void Document::writeProgressLog(const QString &message)
{
    // Replace in place while a progress line is already up, so only the latest
    // percentage is ever visible. Info, not Debug: it is the live feedback of an
    // operation the user started, and it erases itself when that operation ends.
    appendOrReplaceLog(message, LogSource::VCG, LogLevel::Info, m_progressLogActive);
    m_progressLogActive = true;
    m_progressLogIndex = m_logMessages.empty() ? 0 : m_logMessages.size() - 1;
}

vcg::CallBackPos *Document::logCallback()
{
    return &Document::dispatchLogCallback;
}

bool Document::handleLogCallback(int pos, const char *message)
{
    ++m_loadCallbackCount;

    const int clampedPos = std::clamp(pos, 0, 100);
    const bool isLoadCallback = (m_callbackMode == CallbackMode::Load);
    const bool isFilterCallback = (m_callbackMode == CallbackMode::Filter);
    const qint64 nowMs = m_loadCallbackTimer.isValid() ? m_loadCallbackTimer.elapsed() : 0;
    const bool forceUiUpdate = (clampedPos == 0 || clampedPos == 100);
    const bool progressChanged = (clampedPos != m_lastProgressPos);
    // A project load pumps events between every few percent of every layer, and each pump
    // repaints the progress bar and the log; at the single-file rate that was a fifth of
    // the load time. The layer count is the progress that matters there, so ask less often.
    const qint64 uiThrottleMs = m_bulkLoading ? 120 : 33;
    const qint64 pumpThrottleMs = m_bulkLoading ? 250 : 80;
    const bool uiThrottleElapsed =
        (m_lastProgressEmitMs < 0) || (nowMs - m_lastProgressEmitMs >= uiThrottleMs);

    QString text;
    bool textDecoded = false;
    auto decodeText = [&]() {
        if (textDecoded)
            return;
        const QByteArray rawMessage(message ? message : "");
        text = QString::fromLocal8Bit(rawMessage);
        while (!text.isEmpty() && (text.endsWith('\n') || text.endsWith('\r')))
            text.chop(1);
        text = text.trimmed();
        textDecoded = true;
    };

    if (progressChanged && (forceUiUpdate || uiThrottleElapsed)) {
        m_lastProgressPos = clampedPos;
        m_lastProgressEmitMs = nowMs;
        decodeText();
        if (isLoadCallback || isFilterCallback) {
            if (isLoadCallback)
                emit loadProgressUpdated(clampedPos, text);
            else
                emit filterProgressUpdated(clampedPos, text);
            ++m_loadProgressEmitCount;

            // One transient line, refreshed on the same throttle as the progress bar
            // rather than the old every-10% append, so it reads as a live counter.
            if (text.isEmpty())
                writeProgressLog(tr("Progress %1%").arg(clampedPos));
            else
                writeProgressLog(tr("%1% - %2").arg(clampedPos, 3).arg(text));
        }

        const bool processEventsThrottleElapsed =
            (m_lastProcessEventsMs < 0) || (nowMs - m_lastProcessEventsMs >= pumpThrottleMs);
        if ((isLoadCallback || isFilterCallback) && (forceUiUpdate || processEventsThrottleElapsed)) {
            QElapsedTimer processTimer;
            processTimer.start();
            const QEventLoop::ProcessEventsFlags flags = isFilterCallback
                ? QEventLoop::AllEvents
                : QEventLoop::ExcludeUserInputEvents;
            QCoreApplication::processEvents(flags);
            m_loadProcessEventsNs += processTimer.nsecsElapsed();
            m_lastProcessEventsMs = nowMs;
            ++m_loadProcessEventsCount;
        }
    }

    return !m_cancelRequested.load(std::memory_order_relaxed);
}

bool Document::dispatchLogCallback(int pos, const char *message)
{
    if (!g_callbackDocument)
        return true;

    return g_callbackDocument->handleLogCallback(pos, message);
}
