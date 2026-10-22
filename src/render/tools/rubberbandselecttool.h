#pragma once

#include "interactivetool.h"

#include <QPoint>
#include <QVariantMap>
#include <QVector>

class QRubberBand;
class QWidget;

// Session tool: drag a rectangle to select the current mesh's faces (by centroid)
// or vertices. On release it commits exactly one select_by_screen_rectangle filter call
// (camera + normalized rect) → one undo node, fully scriptable. Modifiers:
// Shift = add, Ctrl = subtract, otherwise replace. Its options (controls(), the strip's
// buttons): Faces (F) or Vertices (V), Visible Only (B), growth to whole Components (C) or
// whole UV Islands (T), and the Lasso (P).
//
// All (A), None (N), Invert (I), Dilate (+), Erode (-) and Remove (X) edit the selection
// already made, each by running one ordinary selection filter on whichever of faces or
// vertices the tool is picking (see selectionshortcuts.h).
//
// The lasso: click the corners, then close it with a double click, Enter, or a click on the
// first corner; Backspace takes back the last corner, Esc drops the polygon. It commits one
// select_by_screen_polygon call, and the modifiers and the options apply to it as to the
// rectangle.
class RubberBandSelectTool final : public InteractiveTool
{
public:
    QString id() const override;
    QString name() const override;
    QString statusHint() const override;
    QString iconPath() const override;
    QCursor cursor() const override;
    bool supportsUvView() const override { return true; }

    void deactivate(bool commit) override;
    bool mousePress(QMouseEvent *e) override;
    bool mouseMove(QMouseEvent *e) override;
    bool mouseRelease(QMouseEvent *e) override;
    bool mouseDoubleClick(QMouseEvent *e) override;
    bool keyPress(QKeyEvent *e) override;
    std::vector<ToolControl> controls() const override;
    bool isControlChecked(const QString &id) const override;
    bool isControlEnabled(const QString &id) const override;
    void triggerControl(const QString &id, Qt::KeyboardModifiers modifiers) override;
    void cancelGesture() override;
    bool gestureInFlight() const override { return m_dragging || !m_polygon.isEmpty(); }

private:
    void endDrag();
    void runEditingShortcut(int key);
    // Runs the selection filter for a region the caller has put in `regionParams`
    // (rect_* or polygon), with everything else -- the camera, the elements, the mode
    // from `modifiers`, visible-only and the growth -- filled in here.
    void commitSelection(const QString &filterId, QVariantMap regionParams,
                         Qt::KeyboardModifiers modifiers);
    void commitPolygon(Qt::KeyboardModifiers modifiers);
    // A point of the view in the filter's screen coordinates: [0, 1] across the rect the scene is
    // drawn in (RenderWidget::sceneRect, which leaves out the histogram's strip), y up.
    QPointF normalizedScenePoint(const QPoint &widgetPos) const;
    void clearPolygon();
    void updateLassoOverlay();

    enum class Shape { Rectangle, Polygon };
    Shape m_shape = Shape::Rectangle;
    QVector<QPoint> m_polygon;   // the corners clicked so far, in view pixels
    QPoint m_hover;              // where the pointer is, for the closing edge
    bool m_swallowRelease = false;
    QWidget *m_lassoOverlay = nullptr;

    QRubberBand *m_band = nullptr;
    QPoint m_origin;
    bool m_dragging = false;
    bool m_selectFaces = true;
    bool m_visibleOnly = false; // faces: keep only faces not occluded from the viewpoint
    // What the hit set grows to, the filter's expand_to. C and T each toggle their own
    // choice, so pressing one switches the other off.
    enum class Expansion { None, Components, UvIslands };
    Expansion m_expansion = Expansion::None;
};
