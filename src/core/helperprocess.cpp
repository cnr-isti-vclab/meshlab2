#include "helperprocess.h"

#include "document.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QObject>
#include <QProcess>
#include <QScopeGuard>
#include <QVector>

#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

// Bounded so a chatty helper cannot grow the failure message without limit.
constexpr qsizetype kOutputTailLimit = 64 * 1024;
// Likewise for the log: a helper that narrates per patch or per element would otherwise
// decide on its own how many entries the log panel holds. The tail above still keeps the
// last 64 KB for the failure message, so nothing needed for diagnosis is lost.
constexpr int kEchoedLineLimit = 2000;

struct Running
{
    QProcess *process = nullptr;
    QString label;
};

// Helpers are started from the filter call, which runs on the GUI thread; the registry
// is only ever touched from there and from terminateAll(), reached through this same
// thread's event pump.
QVector<Running> &registry()
{
    static QVector<Running> processes;
    return processes;
}

bool &shuttingDown()
{
    static bool value = false;
    return value;
}

void stopProcess(QProcess &process)
{
    if (process.state() == QProcess::NotRunning)
        return;

    const qint64 pid = process.processId();
#ifdef Q_OS_UNIX
    // run() puts the helper in its own process group, so signal the group: a helper that
    // spawns children of its own would otherwise leave them behind.
    if (pid > 0)
        ::killpg(pid_t(pid), SIGTERM);
#else
    Q_UNUSED(pid);
#endif
    process.terminate();
    if (process.waitForFinished(2000))
        return;

#ifdef Q_OS_UNIX
    if (pid > 0)
        ::killpg(pid_t(pid), SIGKILL);
#endif
    process.kill();
    process.waitForFinished(2000);
}

void appendTail(QByteArray &tail, const QByteArray &chunk)
{
    tail += chunk;
    if (tail.size() > kOutputTailLimit)
        tail.remove(0, tail.size() - kOutputTailLimit);
}

} // namespace

namespace HelperProcess {

bool run(const Request &request, Document &doc, QString &error)
{
    const QString label = request.label.isEmpty()
        ? QFileInfo(request.program).fileName()
        : request.label;

    // A previous terminateAll() may have been a window close the user then abandoned;
    // this run is a fresh intent to start something.
    shuttingDown() = false;

    QProcess process;
    process.setWorkingDirectory(request.workingDirectory);
    process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
#endif
#ifdef Q_OS_WIN
    // Helpers built against MeshLab's vcpkg tree load its DLLs (TextureConsolidation's
    // libpng, libjpeg and zlib), which sit beside the application rather than beside the
    // helper. Windows looks for a program's DLLs in its own directory, then on PATH.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"),
                       QDir::toNativeSeparators(QCoreApplication::applicationDirPath()) + QLatin1Char(';')
                           + environment.value(QStringLiteral("PATH")));
    process.setProcessEnvironment(environment);
#endif
    process.start(request.program, request.arguments);
    if (!process.waitForStarted()) {
        error = QObject::tr("Could not start helper '%1': %2").arg(label, process.errorString());
        return false;
    }

    registry().append({&process, label});
    const auto unregister = qScopeGuard([&process] {
        QVector<Running> &processes = registry();
        for (int i = processes.size() - 1; i >= 0; --i) {
            if (processes[i].process == &process)
                processes.remove(i);
        }
    });

    doc.writeLog(
        QObject::tr("Helper '%1' started as pid %2: %3 %4")
            .arg(label)
            .arg(process.processId())
            .arg(request.program, request.arguments.join(QLatin1Char(' '))),
        Document::LogSource::Application);

    QElapsedTimer timer;
    timer.start();
    QByteArray outputTail;
    QByteArray partialLine;
    int stage = 0;
    int echoed = 0;

    // Echo whole lines only, so a helper that draws a counter with \r does not flood the
    // log, and watch them for the stage banners that drive the progress bar.
    const auto consume = [&](const QByteArray &chunk) {
        if (chunk.isEmpty())
            return;
        appendTail(outputTail, chunk);
        partialLine += chunk;
        qsizetype newline = -1;
        while ((newline = partialLine.indexOf('\n')) >= 0) {
            const QString line =
                QString::fromLocal8Bit(partialLine.left(newline)).trimmed();
            partialLine.remove(0, newline + 1);
            if (line.isEmpty())
                continue;
            if (echoed < kEchoedLineLimit) {
                doc.writeLog(
                    QStringLiteral("[%1] %2").arg(label, line), Document::LogSource::Application);
            } else if (echoed == kEchoedLineLimit) {
                doc.writeLog(
                    QObject::tr("[%1] ... further output is not echoed (%2 lines).")
                        .arg(label).arg(kEchoedLineLimit),
                    Document::LogSource::Application);
            }
            ++echoed;

            if (stage >= request.stageMarkers.size() || request.progressBegin < 0)
                continue;
            const QString marker = request.stageMarkers.at(stage);
            if (!line.contains(marker, Qt::CaseInsensitive))
                continue;
            ++stage;
            if (vcg::CallBackPos *callback = doc.progressCallback()) {
                const int span = request.progressEnd - request.progressBegin;
                const int position = request.progressBegin
                    + (span * stage) / (request.stageMarkers.size() + 1);
                // The marker, not the line it was found in: helpers draw their phase
                // banners in rules of dashes, which belong in the log and not in the
                // status bar.
                const QByteArray text = QStringLiteral("%1: %2").arg(label, marker).toLocal8Bit();
                (*callback)(position, text.constData());
            }
        }
    };

    for (;;) {
        process.waitForFinished(50);
        consume(process.readAll());
        // The state, not waitForFinished()'s result: once processEvents() below has
        // reaped the child, waitForFinished() reports false for a process that has
        // already exited, and looping on that spins forever at full tilt.
        if (process.state() == QProcess::NotRunning)
            break;

        // The helpers have no callback API, so keep the synchronous filter UI and its
        // Cancel button responsive while their process is running.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

        if (doc.isOperationCancelRequested() || shuttingDown()) {
            stopProcess(process);
            consume(process.readAll());
            error = QObject::tr("Helper '%1' was interrupted.").arg(label);
            return false;
        }
    }
    consume(process.readAll());

    doc.writeLog(
        QObject::tr("Helper '%1' finished in %2 ms (exit %3).")
            .arg(label)
            .arg(timer.elapsed())
            .arg(process.exitCode()),
        Document::LogSource::Application);

    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return true;

    QString details = QString::fromLocal8Bit(outputTail).trimmed();
    if (details.isEmpty())
        details = process.errorString();
    error = QObject::tr("Helper '%1' failed: %2").arg(label, details);
    return false;
}

bool anyRunning()
{
    return !registry().isEmpty();
}

QStringList runningLabels()
{
    QStringList labels;
    for (const Running &running : registry())
        labels << running.label;
    return labels;
}

void terminateAll()
{
    if (registry().isEmpty())
        return;

    // Set before stopping anything: run() is still on the stack for each of these, and
    // must report an interruption rather than treating the kill as a helper failure.
    shuttingDown() = true;
    // stopProcess() pumps no events, so the registry cannot change under this loop.
    for (const Running &running : registry())
        stopProcess(*running.process);
}

} // namespace HelperProcess
