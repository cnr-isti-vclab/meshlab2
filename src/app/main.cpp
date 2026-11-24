// Python.h must be included before any Qt headers to avoid macro conflicts.
#ifdef MESHLAB2_PYTHON_CONSOLE
#define PY_SSIZE_T_CLEAN
#include <Python.h>
// Forward-declare the init function for the statically-linked _meshlab module.
// The symbol is provided by src/python/bindings/module.cpp via nanobind NB_STATIC.
extern "C" PyObject *PyInit__meshlab();
#include "PythonHost.h"
#include "document.h"
#endif

#include "helperprocess.h"
#include "mainwindow.h"
#include "preferences.h"
#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <clocale>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

// The application was called MeshLab until 2026-09; QSettings keyed its store on that
// name, so renaming it would leave every existing user looking at defaults with no error
// to explain why. Copy the old store across once.
//
// Keys already present are never overwritten. That matters here: the new name resolves to
// the same settings domain the released MeshLab uses (organizationDomain "meshlab.net" +
// applicationName "MeshLab"), which is the intended consequence of shipping as MeshLab's
// in-place successor -- but it means the destination may not be empty, and nothing of
// MeshLab's should be clobbered.
void migrateSettingsFromMeshLab()
{
    QSettings current;
    if (current.value(QStringLiteral("settings.migratedFromMeshLab")).toBool())
        return;

    // Where the old store lives is platform-dependent: QSettings keys macOS on the
    // organization *domain* and Windows on the organization *name*, and the two were
    // different strings. Try both rather than guessing.
    int copied = 0;
    for (const QString &organization : {QCoreApplication::organizationDomain(),
                                        QStringLiteral("MeshLab")}) {
        QSettings legacy(QSettings::NativeFormat, QSettings::UserScope, organization,
                         QStringLiteral("MeshLab"));
        const QStringList keys = legacy.allKeys();
        for (const QString &key : keys) {
            if (current.contains(key))
                continue;
            current.setValue(key, legacy.value(key));
            ++copied;
        }
    }
    current.setValue(QStringLiteral("settings.migratedFromMeshLab"), true);
    current.sync();
    if (copied > 0)
        qInfo("Migrated %d settings from the previous MeshLab store.", copied);
}

#ifdef MESHLAB2_PYTHON_CONSOLE
// Runs `code` in the embedded interpreter, on a document with no window bound to it as
// `ms`, the way the console dock does. Shared by --script and --generate-docs.
// `pythonArgv` becomes sys.argv; `code` may set `_meshlab_exit` to the exit status,
// and otherwise it is 0, or 1 when `code` raises.
int runHeadlessPython(int argc, char *argv[], const QStringList &pythonArgv, const char *code)
{
#ifdef Q_OS_WIN
    // A GUI-subsystem exe has no console; borrow the caller's so the script's
    // output reaches the terminal it was started from.
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *unused = nullptr;
        freopen_s(&unused, "CONOUT$", "w", stdout);
        freopen_s(&unused, "CONOUT$", "w", stderr);
    }
#endif
#ifdef Q_OS_LINUX
    // On a machine with no display, batch work needs none.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM") && qEnvironmentVariableIsEmpty("DISPLAY")
        && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    PyImport_AppendInittab("_meshlab", &PyInit__meshlab);

    QApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C"); // see the main QApplication below
    QImageReader::setAllocationLimit(0);

    Document doc;
    PythonHost::instance().initialize(&doc, nullptr);

    PyObject *pyArgv = PyList_New(0);
    for (const QString &arg : pythonArgv) {
        PyObject *item = PyUnicode_FromString(arg.toUtf8().constData());
        PyList_Append(pyArgv, item);
        Py_DECREF(item);
    }
    PySys_SetObject("argv", pyArgv);
    Py_DECREF(pyArgv);

    // The host captures stdout and stderr for the console dock; here they go to the terminal.
    const QByteArray source = QByteArray(
        "import sys\n"
        "sys.stdout, sys.stderr = sys.__stdout__, sys.__stderr__\n") + code;
    PyObject *mainDict = PyModule_GetDict(PyImport_AddModule("__main__"));
    PyObject *result = PyRun_String(source.constData(), Py_file_input, mainDict, mainDict);
    int status = 0;
    if (!result) {
        PyErr_Print();
        status = 1;
    } else {
        Py_DECREF(result);
        if (PyObject *exitCode = PyDict_GetItemString(mainDict, "_meshlab_exit"))
            status = int(PyLong_AsLong(exitCode));
    }

    PythonHost::instance().finalize();
    HelperProcess::terminateAll();
    return status;
}
#endif

} // namespace

int main(int argc, char *argv[])
{
    const QStringList args = [&]() {
        QStringList list;
        for (int i = 0; i < argc; ++i)
            list << QString::fromUtf8(argv[i]);
        return list;
    }();

#ifdef MESHLAB2_PYTHON_CONSOLE
    // Batch mode: run a Python script on a document with no window, then quit.
    if (args.size() >= 2 && args[1] == QStringLiteral("--script")) {
        if (args.size() < 3 || !QFileInfo(args[2]).isFile()) {
            fprintf(stderr, "Usage: MeshLab --script <script.py> [arguments...]\n");
            return 2;
        }
        // The application's settings store, so filters read the user's preferences as they
        // do in the GUI. --generate-docs leaves it unset: the render probes it runs must
        // not depend on anyone's settings.
        QCoreApplication::setOrganizationName(QStringLiteral("MeshLab"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("meshlab.net"));
        QCoreApplication::setApplicationName(QStringLiteral("MeshLab"));
        // The script runs as `python script.py` would run it: as __main__, its own
        // directory first on sys.path, sys.exit() setting the exit status. It catches
        // SystemExit itself because PyErr_Print() would exit the process on one,
        // skipping the interpreter's and the helpers' cleanup.
        return runHeadlessPython(argc, argv, args.mid(2), R"python(
import os, runpy, traceback

def _meshlab_run(path):
    sys.path.insert(0, os.path.dirname(os.path.abspath(path)))
    try:
        runpy.run_path(path, init_globals={"ms": ms}, run_name="__main__")
    except SystemExit as e:
        if e.code is None or isinstance(e.code, int):
            return e.code or 0
        print(e.code, file=sys.stderr)
        return 1
    except BaseException:
        traceback.print_exc()
        return 1
    return 0

_meshlab_exit = _meshlab_run(sys.argv[0])
)python");
    }

    // Generate the Python API documentation then quit. The docs directory is the
    // second argument, or "docs" if omitted.
    if (args.size() >= 2 && args[1] == QStringLiteral("--generate-docs")) {
        const QString docsDir = (args.size() >= 3 && !args[2].isEmpty())
            ? args[2]
            : QStringLiteral("docs");
        return runHeadlessPython(argc, argv, { QString(), docsDir }, R"python(
exec(open(sys.argv[1] + '/generate_api.py').read())
generate(sys.argv[1])
)python");
    }
#else
    if (args.size() >= 2
        && (args[1] == QStringLiteral("--script") || args[1] == QStringLiteral("--generate-docs"))) {
        fprintf(stderr, "This build of MeshLab has no Python support.\n");
        return 2;
    }
#endif

#ifdef MESHLAB2_PYTHON_CONSOLE
    // Register the built-in _meshlab extension before Py_Initialize so that
    // `import _meshlab` works inside the embedded interpreter.
    // This must happen before QApplication (and certainly before PythonHost::initialize).
    PyImport_AppendInittab("_meshlab", &PyInit__meshlab);
#endif

    QApplication app(argc, argv);
    // On Unix the constructor sets the C locale from the environment (setlocale(LC_ALL, "")),
    // so in an Italian or German session atof, sscanf and printf would read and write a
    // comma as the decimal separator, and every library parsing text files with them would
    // misread "0.5" as 0 and write files no one can read. Qt's documented remedy; the
    // interface is unaffected, since QLocale formats numbers for display on its own.
    std::setlocale(LC_NUMERIC, "C");
    // Last resort: whatever route the application takes out, no helper it started is
    // left behind holding a temporary directory and a CPU.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &HelperProcess::terminateAll);
    // Allow loading very large textures (e.g. high-res atlas images in glb/gltf).
    // Qt defaults to 256 MB decoded-image cap, which can reject valid assets.
    QImageReader::setAllocationLimit(0);
    app.setOrganizationName(QStringLiteral("MeshLab"));
    app.setOrganizationDomain(QStringLiteral("meshlab.net"));
    app.setApplicationName(QStringLiteral("MeshLab"));
    migrateSettingsFromMeshLab();
    const QIcon appIcon(QStringLiteral(":/img/MeshLab_Icon_512x512.png"));
    if (!appIcon.isNull())
        app.setWindowIcon(appIcon);

    // GPU frame timing needs the window's graphics context created with timestamps on, which Qt
    // does when QSG_RHI_PROFILE is set (it adds debug markers too). It has to be set before the
    // first window is, so the preference applies from the next start; one set by hand is kept.
    if (Preferences::instance().boolValue(QStringLiteral("advanced.gpuTiming"))
        && !qEnvironmentVariableIsSet("QSG_RHI_PROFILE"))
        qputenv("QSG_RHI_PROFILE", "1");

    // Capture the real application chrome and render view for documentation/website images.
    // The project load is synchronous; the timer lets the window and QRhi render a frame.
    const bool captureWindow = args.size() >= 2 && args[1] == QStringLiteral("--capture-window");
    if (captureWindow && (args.size() != 4 || !QFileInfo::exists(args[2]))) {
        fprintf(stderr, "Usage: MeshLab --capture-window <mesh-or-project> <output.png>\n");
        return 2;
    }

    MainWindow w;
    if (!appIcon.isNull())
        w.setWindowIcon(appIcon);
    if (captureWindow)
        w.resize(1440, 900);
    w.show();
    if (captureWindow) {
        QString error;
        if (!w.loadMeshFromPath(args[2], &error)) {
            fprintf(stderr, "Could not load capture input: %s\n", qPrintable(error));
            return 1;
        }
        QTimer::singleShot(1500, &w, [&app, &w, output = args[3]] {
            QImage content = w.grab().toImage();
            if (content.isNull()) {
                fprintf(stderr, "Could not capture the MeshLab window.\n");
                app.exit(1);
                return;
            }
            const qreal dpr = content.devicePixelRatio();
            // Draw in image pixels; QWidget::grab() carries the display's DPR.
            content.setDevicePixelRatio(1.0);

            // QWidget::grab() omits native window decorations on macOS. A small,
            // consistent frame keeps the real title visible without requiring
            // screen-recording permission or capturing other desktop windows.
            const int border = qMax(1, qRound(dpr));
            const int titleHeight = qRound(40 * dpr);
            QImage framed(content.width() + 2 * border,
                          content.height() + titleHeight + 2 * border,
                          QImage::Format_RGB32);
            framed.fill(QColor(QStringLiteral("#d7dcdf")));
            QPainter painter(&framed);
            painter.fillRect(border, border, content.width(), titleHeight,
                             QColor(QStringLiteral("#f3f5f5")));
            QFont titleFont = painter.font();
            titleFont.setPixelSize(qRound(14 * dpr));
            painter.setFont(titleFont);
            painter.setPen(QColor(QStringLiteral("#24343a")));
            painter.drawText(QRect(border, border, content.width(), titleHeight),
                             Qt::AlignCenter, w.windowTitle());
            painter.drawImage(border, titleHeight + border, content);
            painter.end();
            if (!framed.save(output, "PNG")) {
                fprintf(stderr, "Could not save window capture: %s\n", qPrintable(output));
                app.exit(1);
                return;
            }
            app.quit();
        });
        return app.exec();
    }
    // Files named on the command line open as if chosen in the Open dialog.
    for (const QString &arg : app.arguments().mid(1)) {
        if (QFileInfo::exists(arg))
            w.loadMeshFromPath(arg);
    }

    return app.exec();
}
