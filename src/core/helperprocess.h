#pragma once

#include <QString>
#include <QStringList>

class Document;

// Runs a bundled command-line helper as a child process while the synchronous filter
// UI stays responsive.
//
// Every helper is registered while it runs, so the application always knows what it
// started: the log names the command and its pid, the helper's own output is echoed as
// it arrives, and terminateAll() can stop anything still alive. Without that last part a
// hung helper outlives its window -- the filter sits in this pump, the main event loop
// never regains control, and MeshLab can only be killed from outside.
namespace HelperProcess {

struct Request
{
    QString program;
    QStringList arguments;
    QString workingDirectory;
    // Names the helper in the log and in runningLabels(); defaults to the file name.
    QString label;
    // Helpers have no callback API, so their progress is whatever they print. A line
    // containing stageMarkers[i] moves the bar to the (i+1)-th step between
    // progressBegin and progressEnd. Left empty, the bar simply stays put.
    QStringList stageMarkers;
    int progressBegin = -1;
    int progressEnd = -1;
};

// Blocks until the helper exits, is cancelled through Document::isOperationCancelRequested(),
// or is stopped by terminateAll(). Returns false with `error` set unless it exited zero.
bool run(const Request &request, Document &doc, QString &error);

bool anyRunning();
QStringList runningLabels();

// Stops every live helper, hard if it does not go quietly. Safe to call when none are
// running, and safe to call from inside run()'s own event pump -- which is where a
// window close lands, since the filter that started the helper is still on the stack.
void terminateAll();

} // namespace HelperProcess
