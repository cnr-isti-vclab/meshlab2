// Python.h must be the very first include; on some platforms it redefines
// 'slots', which conflicts with Qt.  Including it before any Qt header avoids
// the macro collision.
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "PythonHost.h"

#include "bindings/meshset_core.h"
#include "mlgui.h"
#include "document.h"

#include <nanobind/nanobind.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <string>

namespace nb = nanobind;

// ---------------------------------------------------------------------------
// Python setup code run once after Py_Initialize.
//
// Installs lightweight capture objects as sys.stdout / sys.stderr so that
// all Python output is intercepted.  The captured text is read back via
// _stdout_capture.getvalue() / _stderr_capture.getvalue().
// ---------------------------------------------------------------------------
static const char *kSetupCode = R"python(
import sys
import code

class _CaptureIO:
    def __init__(self):
        self._buf = []
    def write(self, text):
        if text:
            self._buf.append(str(text))
    def flush(self):
        pass
    def getvalue(self):
        v = ''.join(self._buf)
        self._buf.clear()
        return v

_stdout_capture = _CaptureIO()
_stderr_capture = _CaptureIO()
sys.stdout = _stdout_capture
sys.stderr = _stderr_capture
)python";

// ---------------------------------------------------------------------------
// Interpreter home.
//
// libpython carries the prefix of the tree that built *it*: for the vcpkg
// dependency that is some build directory on some machine, which a binary-cache
// hit then propagates into unrelated trees.  Left to itself the interpreter
// looks for its standard library there and fails with "Could not find platform
// independent libraries".  So the home is resolved from the running executable
// instead, which covers the build tree and a deployed bundle alike.
// ---------------------------------------------------------------------------
static QString resolvePythonHome()
{
    // Python looks for its standard library under the home: Lib on Windows,
    // lib/pythonX.Y elsewhere.  os.py is the landmark it uses itself to decide
    // that a directory really holds one.
#ifdef Q_OS_WIN
    const QString landmark = QStringLiteral("/Lib/os.py");
#else
    const QString landmark = QStringLiteral("/lib/python%1.%2/os.py")
                                 .arg(PY_MAJOR_VERSION)
                                 .arg(PY_MINOR_VERSION);
#endif
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QStringList candidates {
#ifdef Q_OS_MACOS
        // Deployed bundle: MeshLab.app/Contents/Resources/python.
        appDir.absoluteFilePath(QStringLiteral("../Resources/python")),
#endif
        appDir.absoluteFilePath(QStringLiteral("python")),
        // Build tree: the prefix Python was found under when CMake configured.
        QStringLiteral(MESHLAB2_PYTHON_HOME),
    };
    for (const QString &candidate : candidates) {
        const QString home = QDir::cleanPath(candidate);
        if (QFileInfo::exists(home + landmark))
            return home;
    }
    return {};
}

// ---------------------------------------------------------------------------
// PythonHost
// ---------------------------------------------------------------------------

PythonHost::PythonHost(QObject *parent)
    : QObject(parent)
{}

PythonHost::~PythonHost()
{
    finalize();
}

PythonHost &PythonHost::instance()
{
    static PythonHost host;
    return host;
}

void PythonHost::initialize(Document *doc, RenderWidget *view)
{
    if (m_initialized)
        return;

    // PyImport_AppendInittab("_meshlab", ...) must already have been called
    // from main() before this point.
    if (!Py_IsInitialized()) {
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        // An explicit PYTHONHOME stays authoritative: leaving config.home unset
        // lets the interpreter read the environment as usual.
        if (qEnvironmentVariableIsEmpty("PYTHONHOME")) {
            const QString home = resolvePythonHome();
            if (home.isEmpty()) {
                qWarning("Python: no standard library found next to the executable; "
                         "falling back to the prefix compiled into libpython.");
            } else {
                const std::wstring homeW = home.toStdWString();
                PyConfig_SetString(&config, &config.home, homeW.c_str());
            }
        }
        Py_InitializeFromConfig(&config);
        PyConfig_Clear(&config);
    }

    m_initialized = true;

    // Install capture objects for sys.stdout / sys.stderr.
    if (PyRun_SimpleString(kSetupCode) != 0) {
        // Something went wrong; clear any exception so the rest still works.
        PyErr_Clear();
    }

    // Cache references to the capture objects so flushOutput() is cheap.
    PyObject *mainModule = PyImport_AddModule("__main__");
    if (mainModule) {
        PyObject *mainDict = PyModule_GetDict(mainModule);  // borrowed
        PyObject *stdoutObj = PyDict_GetItemString(mainDict, "_stdout_capture");
        PyObject *stderrObj = PyDict_GetItemString(mainDict, "_stderr_capture");
        Py_XINCREF(stdoutObj);
        Py_XINCREF(stderrObj);
        m_stdoutCapture = stdoutObj;
        m_stderrCapture = stderrObj;
    }

    setupConsole(doc);
    // Inject mlgui after the console is ready.
    if (view)
        injectMlGui(view);
}

void PythonHost::setupConsole(Document *doc)
{
    // Import _meshlab (registered via PyImport_AppendInittab).  This triggers
    // PyInit__meshlab which populates the nanobind type registry.
    PyObject *mod = PyImport_ImportModule("_meshlab");
    if (!mod) {
        PyErr_Print();
        return;
    }
    Py_DECREF(mod);

    // Create a MeshSetCore that borrows the live Document (no ownership).
    MeshSetCore *core = new MeshSetCore(doc);
    nb::object pyMeshset = nb::cast(core, nb::rv_policy::take_ownership);

    // Inject `meshset` into __main__ so it is available in the console.
    PyObject *mainModule = PyImport_AddModule("__main__");
    if (!mainModule) {
        PyErr_Print();
        return;
    }
    PyObject *mainDict = PyModule_GetDict(mainModule);  // borrowed
    PyDict_SetItemString(mainDict, "ms", pyMeshset.ptr());

    // Create code.InteractiveConsole(locals=__main__.__dict__) so that
    // any names defined at the console prompt are visible as globals and
    // the injected `meshset` binding is immediately accessible.
    PyObject *codeModule = PyImport_ImportModule("code");
    if (!codeModule) {
        PyErr_Print();
        return;
    }
    PyObject *consoleClass = PyObject_GetAttrString(codeModule, "InteractiveConsole");
    Py_DECREF(codeModule);
    if (!consoleClass) {
        PyErr_Print();
        return;
    }
    PyObject *console = PyObject_CallFunction(consoleClass, "O", mainDict);
    Py_DECREF(consoleClass);
    if (!console) {
        PyErr_Print();
        return;
    }

    Py_XDECREF(static_cast<PyObject *>(m_console));
    m_console = console;

    // Dynamically add one method per filter to the MeshSet class, mirroring
    // pymeshlab's approach.  Each method delegates to apply_filter() using
    // the filter's python_name as the resolution key.
    static const char *kBindFiltersCode = R"python(
import sys
import types

import _meshlab as _qml

pymeshlab = types.ModuleType("pymeshlab")
pymeshlab.__doc__ = "Public MeshLab/PyMeshLab2 scripting facade backed by _meshlab."
pymeshlab.Mesh = _qml.Mesh
pymeshlab.MeshSet = _qml.MeshSet
pymeshlab.FilterInfo = _qml.FilterInfo
pymeshlab.FilterRunResult = _qml.FilterRunResult
pymeshlab.MlGui = _qml.MlGui
pymeshlab.__all__ = ["Mesh", "MeshSet", "FilterInfo", "FilterRunResult", "MlGui"]
sys.modules["pymeshlab"] = pymeshlab

def _bind_filter_methods():
    def _make_filter(python_name):
        def _filter(self, **kwargs):
            return self.apply_filter(python_name, kwargs)
        _filter.__name__ = python_name
        return _filter
    for _fi in ms.list_filters():
        _name = _fi.python_name
        if _name and not hasattr(_qml.MeshSet, _name):
            setattr(_qml.MeshSet, _name, _make_filter(_name))
_bind_filter_methods()
del _bind_filter_methods
)python";
    if (PyRun_SimpleString(kBindFiltersCode) != 0) {
        PyErr_Print();
        // Non-fatal: the console still works, just without per-filter methods.
    }

    // Discard any setup noise captured before the user types anything.
    flushOutput();
}

void PythonHost::finalize()
{
    if (!m_initialized)
        return;

    // Remove the MeshSet wrapper from __main__ BEFORE Py_Finalize so that
    // nanobind's module cleanup (triggered when _meshlab is unloaded) finds
    // no live instances and does not print "leaked instance/type/function"
    // warnings to stderr.
    if (PyObject *mainModule = PyImport_AddModule("__main__")) {
        if (PyObject *mainDict = PyModule_GetDict(mainModule)) {
            if (PyDict_GetItemString(mainDict, "ms"))
                PyDict_DelItemString(mainDict, "ms");
            if (PyDict_GetItemString(mainDict, "mlgui"))
                PyDict_DelItemString(mainDict, "mlgui");
        }
    }

    Py_XDECREF(static_cast<PyObject *>(m_console));
    m_console = nullptr;
    Py_XDECREF(static_cast<PyObject *>(m_stdoutCapture));
    m_stdoutCapture = nullptr;
    Py_XDECREF(static_cast<PyObject *>(m_stderrCapture));
    m_stderrCapture = nullptr;

    Py_Finalize();
    m_initialized = false;
}

void PythonHost::flushOutput()
{
    auto readCapture = [](void *captureObj, bool isError, PythonHost *host) {
        if (!captureObj)
            return;
        PyObject *result = PyObject_CallMethod(
            static_cast<PyObject *>(captureObj), "getvalue", nullptr);
        if (!result) {
            PyErr_Clear();
            return;
        }
        if (PyUnicode_Check(result)) {
            Py_ssize_t size = 0;
            const char *utf8 = PyUnicode_AsUTF8AndSize(result, &size);
            if (utf8 && size > 0) {
                const QString text = QString::fromUtf8(utf8, static_cast<int>(size));
                if (isError)
                    emit host->errorWritten(text);
                else
                    emit host->outputWritten(text);
            }
        }
        Py_DECREF(result);
    };

    readCapture(m_stdoutCapture, false, this);
    readCapture(m_stderrCapture, true,  this);
}

bool PythonHost::runLine(const QString &line)
{
    if (!m_initialized || !m_console)
        return true;

    const QByteArray utf8 = line.toUtf8();
    PyObject *pyLine = PyUnicode_FromStringAndSize(utf8.constData(), utf8.size());
    if (!pyLine) {
        PyErr_Clear();
        return true;
    }

    // push() returns 1 (truthy) if more input is needed, 0 if complete.
    PyObject *result = PyObject_CallMethod(
        static_cast<PyObject *>(m_console), "push", "O", pyLine);
    Py_DECREF(pyLine);

    // Flush captured output before examining the return value so that any
    // print() calls inside the executed code are emitted to the console.
    flushOutput();

    if (!result) {
        PyErr_Clear();
        return true;  // treat as complete (with error)
    }

    const bool needsMore = (PyObject_IsTrue(result) == 1);
    Py_DECREF(result);
    return !needsMore;
}

void PythonHost::runCode(const QString &code)
{
    if (!m_initialized)
        return;

    const QByteArray utf8 = code.toUtf8();
    if (PyRun_SimpleString(utf8.constData()) != 0) {
        // PyRun_SimpleString already prints errors to stderr.
        PyErr_Clear();
    }

    flushOutput();
}

void PythonHost::resetConsole()
{
    if (!m_initialized || !m_console)
        return;
    PyObject *result = PyObject_CallMethod(
        static_cast<PyObject *>(m_console), "resetbuffer", nullptr);
    Py_XDECREF(result);
    flushOutput();
}

void PythonHost::injectMeshSet(Document *doc)
{
    if (!m_initialized)
        return;

    // Replace the existing `meshset` binding with one wrapping the new doc.
    // The old MeshSetCore will be gc'd by Python (but won't delete the old doc
    // because m_ownsDocument == false).
    MeshSetCore *core = new MeshSetCore(doc);
    nb::object pyMeshset = nb::cast(core, nb::rv_policy::take_ownership);

    PyObject *mainModule = PyImport_AddModule("__main__");
    if (!mainModule)
        return;
    PyObject *mainDict = PyModule_GetDict(mainModule);
    PyDict_SetItemString(mainDict, "meshset", pyMeshset.ptr());
}

void PythonHost::injectMlGui(RenderWidget *view)
{
    if (!m_initialized)
        return;

    MlGui *gui = new MlGui(view);
    nb::object pyGui = nb::cast(gui, nb::rv_policy::take_ownership);

    PyObject *mainModule = PyImport_AddModule("__main__");
    if (!mainModule)
        return;
    PyObject *mainDict = PyModule_GetDict(mainModule);
    PyDict_SetItemString(mainDict, "mlgui", pyGui.ptr());
}
