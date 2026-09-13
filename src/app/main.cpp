// Python.h must be included before any Qt headers to avoid macro conflicts.
#ifdef MESHLAB2_PYTHON_CONSOLE
#define PY_SSIZE_T_CLEAN
#include <Python.h>
// Forward-declare the init function for the statically-linked _qmeshlab module.
// The symbol is provided by src/python/bindings/module.cpp via nanobind NB_STATIC.
extern "C" PyObject *PyInit__qmeshlab();
#include "PythonHost.h"
#include "document.h"
#endif

#include "helperprocess.h"
#include "mainwindow.h"
#include <QApplication>
#include <QIcon>
#include <QImageReader>
#include <QStringList>

int main(int argc, char *argv[])
{
    const QStringList args = [&]() {
        QStringList list;
        for (int i = 0; i < argc; ++i)
            list << QString::fromUtf8(argv[i]);
        return list;
    }();

#ifdef MESHLAB2_PYTHON_CONSOLE
    // Early-exit mode: generate Python API documentation then quit.
    if (args.size() >= 2 && args[1] == QStringLiteral("--generate-docs")) {
        // Register the built-in _qmeshlab extension.
        PyImport_AppendInittab("_qmeshlab", &PyInit__qmeshlab);

        QApplication app(argc, argv);

        Document doc;
        // Initialize interpreter + import _qmeshlab.  Pass nullptr for view —
        // no MlGui binding is needed during doc generation.
        PythonHost::instance().initialize(&doc, nullptr);

        // The docs directory is the second argument, or "docs" if omitted.
        const QString docsDir = (args.size() >= 3 && !args[2].isEmpty())
            ? args[2]
            : QStringLiteral("docs");

        // Run the generator script.  We restore sys.stdout/stderr first so
        // that print() calls in the script reach the terminal.
        const QByteArray code = QStringLiteral(
            "import sys\n"
            "sys.stdout = sys.__stdout__\n"
            "sys.stderr = sys.__stderr__\n"
            "exec(open('%1/generate_api.py').read())\n"
            "generate('%1')\n"
        ).arg(docsDir).toUtf8();

        const int r = PyRun_SimpleString(code.constData());
        if (r != 0) {
            fprintf(stderr, "Documentation generation script failed (code=%d)\n", r);
            PyErr_Print();
            return 1;
        }

        PythonHost::instance().finalize();
        return 0;
    }
#endif

#ifdef MESHLAB2_PYTHON_CONSOLE
    // Register the built-in _qmeshlab extension before Py_Initialize so that
    // `import _qmeshlab` works inside the embedded interpreter.
    // This must happen before QApplication (and certainly before PythonHost::initialize).
    PyImport_AppendInittab("_qmeshlab", &PyInit__qmeshlab);
#endif

    QApplication app(argc, argv);
    // Last resort: whatever route the application takes out, no helper it started is
    // left behind holding a temporary directory and a CPU.
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &HelperProcess::terminateAll);
    // Allow loading very large textures (e.g. high-res atlas images in glb/gltf).
    // Qt defaults to 256 MB decoded-image cap, which can reject valid assets.
    QImageReader::setAllocationLimit(0);
    app.setOrganizationName(QStringLiteral("QMeshLab"));
    app.setOrganizationDomain(QStringLiteral("meshlab.net"));
    app.setApplicationName(QStringLiteral("QMeshLab"));
    const QIcon appIcon(QStringLiteral(":/img/MeshLab_Icon_512x512.png"));
    if (!appIcon.isNull())
        app.setWindowIcon(appIcon);

    MainWindow w;
    if (!appIcon.isNull())
        w.setWindowIcon(appIcon);
    w.show();

    return app.exec();
}
