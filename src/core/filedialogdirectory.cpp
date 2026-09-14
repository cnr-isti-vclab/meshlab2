#include "filedialogdirectory.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace {

const QString kGroup = QStringLiteral("fileDialogDirectories");
// The most recent directory used for any purpose, so a dialog opened for the first time
// starts somewhere the user has actually been.
const QString kSharedKey = QStringLiteral("_last");

QString readDirectory(const QString &key)
{
    QSettings settings;
    settings.beginGroup(kGroup);
    const QString path = settings.value(key).toString().trimmed();
    settings.endGroup();
    // A remembered directory can have been deleted or unmounted since. Returning it anyway
    // gives a dialog that opens on nothing, so fall through to the caller's fallback.
    if (path.isEmpty() || !QFileInfo(path).isDir())
        return QString();
    return path;
}

} // namespace

namespace FileDialogDirectory {

QString startingDirectory(const QString &purpose)
{
    const QString forPurpose = readDirectory(purpose);
    if (!forPurpose.isEmpty())
        return forPurpose;
    return readDirectory(kSharedKey);
}

QString startingPath(const QString &purpose, const QString &suggestedFileName)
{
    const QString directory = startingDirectory(purpose);
    if (directory.isEmpty())
        return suggestedFileName;
    return QDir(directory).filePath(suggestedFileName);
}

void remember(const QString &purpose, const QString &chosenPath)
{
    const QString trimmed = chosenPath.trimmed();
    if (trimmed.isEmpty())
        return;
    // The path may name a file that does not exist yet -- a save dialog's target -- so take
    // the directory from the string rather than asking the filesystem about the file.
    const QString directory = QFileInfo(trimmed).absolutePath();
    if (directory.isEmpty())
        return;

    QSettings settings;
    settings.beginGroup(kGroup);
    settings.setValue(purpose, directory);
    settings.setValue(kSharedKey, directory);
    settings.endGroup();
}

} // namespace FileDialogDirectory
