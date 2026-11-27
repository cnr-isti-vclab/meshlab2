// Linked into every Qt test binary (see the loop after the tests in CMakeLists.txt).
//
// A test binary sets no organization of its own, and on macOS QSettings then falls back to
// the MeshLab application's own store. Whatever the developer had chosen in the app leaked
// into the tests -- an OBJ importer preference sent every OBJ through TrueForm, which keeps
// neither polygons nor textures -- and tests wrote their own values back into it.
//
// Each run gets a fresh, empty INI store in a temporary directory instead, so a test sees
// the factory defaults on every machine. A test that needs a stored value sets it itself.

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

namespace {

void isolateSettings()
{
    static QTemporaryDir directory;
    if (!directory.isValid())
        qFatal("Cannot create a temporary directory for the test settings");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(isolateSettings)
