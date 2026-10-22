#pragma once

#include <QString>
#include <QStringList>

class Document;
class QRhi;

// What a report from someone else's machine needs to say about it: what the graphics backend is
// and can do, and what went wrong on the way. Shown in the About box's System tab, which copies
// it, and summed up in the log when the first view starts drawing.
namespace SystemInfo {

// Sends Qt's warnings -- a shader that does not compile, a pipeline the driver refuses -- and
// MeshLab's own qWarning()s to the document's log as well as to stderr, which nobody reads for a
// program started from a desktop launcher. Each distinct warning is logged once, however often it
// repeats. The last few are kept for recentWarnings(). Call once, from the main thread.
void forwardQtWarnings(Document *doc);
QStringList recentWarnings();

// The windowing system, the graphics API Qt picked and the device under it, the driver's version
// where the API says it (OpenGL), the features and limits the renderer leans on.
QString graphicsReport(QRhi *rhi);
// One line of it, for the log: API, device, driver.
QString graphicsSummary(QRhi *rhi);

} // namespace SystemInfo
