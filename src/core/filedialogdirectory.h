#pragma once

#include <QString>

// Where a file dialog should open.
//
// Qt's static QFileDialog helpers take a starting path, and passing an empty one leaves the
// dialog wherever the platform decides -- in practice the process's working directory, which
// is never where the user keeps their models. Remembering the last directory is the caller's
// job, and doing it per call site meant most of them simply did not.
//
// Directories are kept per purpose ("mesh", "raster", "texture", ...) so that opening a mesh
// does not move the texture dialog, with the most recent directory used for anything as the
// fallback the first time a given purpose is used. Related files usually live together, so
// starting a new kind of dialog next to the last one beats starting at the working directory.
namespace FileDialogDirectory {

// Directory to open in for `purpose`, or an empty string when nothing is remembered yet and
// no fallback exists -- which is what Qt's own default handling expects.
QString startingDirectory(const QString &purpose);

// The same, joined with a suggested file name, for save dialogs. Returns just the name when
// no directory is remembered, which is the behaviour those call sites had before.
QString startingPath(const QString &purpose, const QString &suggestedFileName);

// Records the directory of `chosenPath` for `purpose`, and as the shared fallback. Accepts
// the file path the dialog returned rather than a directory, since that is what callers have.
void remember(const QString &purpose, const QString &chosenPath);

} // namespace FileDialogDirectory
