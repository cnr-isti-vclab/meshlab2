#pragma once

#include <QObject>
#include <QString>

class Document;
class RenderWidget;

// Singleton that owns the embedded CPython interpreter lifetime.
//
// Usage from main.cpp (before QApplication):
//   PyImport_AppendInittab("_meshlab", &PyInit__meshlab);
//
// Usage from MainWindow constructor (after building widgets):
//   PythonHost::instance().initialize(m_doc);
//
// The host redirects sys.stdout / sys.stderr so that all Python output
// is delivered via the outputWritten / errorWritten signals rather than
// writing to the terminal.  Connect those signals to PythonConsoleWidget.
class PythonHost : public QObject
{
    Q_OBJECT
public:
    static PythonHost &instance();

    // Initializes the interpreter (Py_Initialize), redirects stdio, and
    // creates a code.InteractiveConsole with `meshset` bound to *doc* in
    // its locals namespace.  If *view* is non-null, a `mlgui` binding for
    // desktop GUI access is also injected.
    // Must be called after PyImport_AppendInittab("_meshlab", ...) in main().
    void initialize(Document *doc, RenderWidget *view = nullptr);

    // Finalises the interpreter (Py_Finalize).  Called automatically on
    // destruction; also safe to call explicitly before ~QApplication.
    void finalize();

    bool isInitialized() const { return m_initialized; }

    // Feed one line to code.InteractiveConsole::push().
    // Returns true  when the statement is complete (or an error occurred).
    // Returns false when more input is expected (incomplete multi-line stmt).
    bool runLine(const QString &line);

    // Execute an arbitrary multi-line code block.  Output is routed through
    // the same capture/redirect system as runLine().
    void runCode(const QString &code);

    // Reset the console input buffer (discard any partial multi-line stmt).
    void resetConsole();

    // Re-inject a fresh `meshset` binding after a significant document
    // change (e.g. the document is replaced in MainWindow).
    void injectMeshSet(Document *doc);

    // Update or inject the `mlgui` binding (desktop GUI access object).
    // Called when the active RenderWidget changes.
    void injectMlGui(RenderWidget *view);

signals:
    void outputWritten(const QString &text);
    void errorWritten(const QString &text);

private:
    explicit PythonHost(QObject *parent = nullptr);
    ~PythonHost() override;

    // Read and emit any text accumulated in the capture buffers.
    void flushOutput();

    // Create the code.InteractiveConsole and inject meshset.
    void setupConsole(Document *doc);

    bool m_initialized = false;

    // Stored as void* to keep Python.h out of this header.
    void *m_console       = nullptr;  // PyObject* — code.InteractiveConsole
    void *m_stdoutCapture = nullptr;  // PyObject* — _CaptureIO instance
    void *m_stderrCapture = nullptr;  // PyObject* — _CaptureIO instance
};
