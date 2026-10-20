#pragma once

#include <QColor>
#include <QCursor>
#include <QList>
#include <QString>
#include <QVector3D>

#include <memory>
#include <vector>

class RenderWidget;
class QMouseEvent;
class QKeyEvent;
class QPainter;
class QMatrix4x4;
class QSize;

// Result of an asynchronous GPU surface pick (see RenderWidget::requestSurfacePick).
struct SurfacePick
{
    bool hit = false;
    int meshIndex = -1;
    QVector3D worldPos;
};

struct ToolLineSegment
{
    QVector3D a;
    QVector3D b;
};

// One of a tool's options or actions. A tool describes each once; the view builds the tool
// strip's buttons from the list and routes the keys to the same place, so a button and its key
// always do the same thing and the strip always shows what the keys are.
struct ToolControl
{
    enum class Kind {
        Toggle,   // shows whether it is on (isControlChecked); a choice is a few of these
        Action,   // does something once
    };
    QString id;
    Kind kind = Kind::Action;
    QString label;       // the button's text
    QString help;        // its tooltip
    QList<int> keys;     // Qt::Key values that trigger it; the first is shown on the button
    QString group;       // buttons of one group sit together, groups are set apart
};

// Base class for interactive editing tools (selection, transform, measure, …).
//
// A tool is a thin interactive front-end: it captures mouse/keyboard input in a
// RenderWidget, gives live visual feedback, and — when it produces a lasting
// document change — normally commits that change by calling Document::runFilter()
// exactly once. This produces one replayable undo node. Operations derived only
// from transient UI state (for example exporting measurement segments) may use a
// Document layer operation, which still owns its undo step.
//
// A tool is active in at most one view at a time. activate()/deactivate() bracket
// the session; deactivate(false) means the session was cancelled and any pending
// (uncommitted) change must be discarded.
class InteractiveTool
{
public:
    virtual ~InteractiveTool() = default;

    virtual QString id() const = 0;
    virtual QString name() const = 0;
    // What the mouse does, in a few words, for the tool strip ("Drag to select · Shift add").
    // The options and actions are not listed here: they are the strip's buttons (controls()).
    virtual QString statusHint() const { return {}; }
    // What the tool is in the middle of, when that changes as it works (a transform's readout,
    // the measurement count). Shown in the strip in place of the hint. Empty = nothing.
    virtual QString statusDetail() const { return {}; }
    // Toolbar/menu icon resource path (e.g. ":/img/tool_select_rect.png"). Empty = none.
    virtual QString iconPath() const { return {}; }
    // Cursor shown while the tool owns the mouse (not suspended).
    virtual QCursor cursor() const { return QCursor(Qt::CrossCursor); }
    // The 3D scene is supported by every tool. Only tools that explicitly opt in
    // receive input in the parametrization view.
    virtual bool supportsUvView() const { return false; }

    virtual void activate(RenderWidget &view) { m_view = &view; }
    virtual void deactivate(bool commit) { (void)commit; m_view = nullptr; }

    // Return true when the event is consumed; the view then skips camera
    // navigation for that event. Returning false lets the trackball handle it.
    virtual bool mousePress(QMouseEvent *e) { (void)e; return false; }
    virtual bool mouseMove(QMouseEvent *e) { (void)e; return false; }
    virtual bool mouseRelease(QMouseEvent *e) { (void)e; return false; }
    // The second press of a double click arrives here instead of as a press. Without a
    // handler the view treats it as a request to recentre on the picked point.
    virtual bool mouseDoubleClick(QMouseEvent *e) { (void)e; return false; }
    // Keys that are part of a gesture rather than a control -- typing a value, closing a polygon.
    // The view tries the controls' keys first.
    virtual bool keyPress(QKeyEvent *e) { (void)e; return false; }

    // The tool's options and actions, in the order the strip shows them. The list may depend on
    // the tool's state; the view rebuilds the strip when the ids change.
    virtual std::vector<ToolControl> controls() const { return {}; }
    virtual bool isControlChecked(const QString &id) const { (void)id; return false; }
    // A disabled control is shown greyed, and its key goes on to keyPress().
    virtual bool isControlEnabled(const QString &id) const { (void)id; return true; }
    // Runs a control, from its button or from its key, with the modifiers held at the time.
    virtual void triggerControl(const QString &id, Qt::KeyboardModifiers modifiers)
    {
        (void)id;
        (void)modifiers;
    }

    // Optional screen-space overlay. The matrix maps world coordinates to clip
    // space; viewportSize is in logical widget pixels.
    virtual void paintOverlay(
        QPainter &painter,
        const QMatrix4x4 &worldToClip,
        const QSize &viewportSize)
    {
        (void)painter;
        (void)worldToClip;
        (void)viewportSize;
    }
    // World-space segments drawn with the standard depth cue: dotted where
    // occluded by scene geometry and solid where visible.
    virtual std::vector<ToolLineSegment> depthCuedLines() const { return {}; }
    // Colour for this tool's depth-cued lines. Lets a tool encode meaning in the
    // line itself -- the transform tool colours a constraint axis red/green/blue.
    virtual QColor toolLineColor() const { return QColor(170, 255, 255); }

    // Delivered (asynchronously) after a RenderWidget::requestSurfacePick issued
    // by this tool completes. result.hit is false when the click missed all meshes.
    virtual void onSurfacePicked(const SurfacePick &result) { (void)result; }

    // Called when an external document change happens mid-gesture (load, filter,
    // undo/redo). Per the lifecycle contract the tool discards any uncommitted
    // in-progress gesture but stays engaged. No-op for tools without one.
    virtual void cancelGesture() {}
    // True while a modal gesture is running. The view gives such a tool first
    // refusal on Esc and Tab, so that Esc cancels the gesture rather than
    // exiting the tool outright.
    virtual bool gestureInFlight() const { return false; }

protected:
    RenderWidget *m_view = nullptr;
};

// Builds the set of built-in interactive tools, mirroring
// registerBuiltinMeshFilterPlugins() for the filter side.
std::vector<std::unique_ptr<InteractiveTool>> createBuiltinInteractiveTools();
