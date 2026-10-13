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
// Shift = add, Ctrl = subtract, otherwise replace. Keys: F/V switch faces/vertices,
// B toggles visible-only, C and T toggle growth to whole connected components or whole
// UV islands, Esc cancels the drag.
//
// P switches the shape to a polygonal lasso: click the corners, then close it with a
// double click, Enter, or a click on the first corner; Backspace takes back the last
// corner, Esc drops the polygon. It commits one select_by_screen_polygon call, and the
// modifiers, the faces/vertices choice, B, C and T apply to it as to the rectangle.
class RubberBandSelectTool final : public InteractiveTool
{
public:
    QString id() const override;
    QString name() const override;
    QString statusHint() const override;
    QString badgeDetail() const override;
    QString iconPath() const override;
    QCursor cursor() const override;
    bool supportsUvView() const override { return true; }

    void deactivate(bool commit) override;
    bool mousePress(QMouseEvent *e) override;
    bool mouseMove(QMouseEvent *e) override;
    bool mouseRelease(QMouseEvent *e) override;
    bool mouseDoubleClick(QMouseEvent *e) override;
    bool keyPress(QKeyEvent *e) override;
    void cancelGesture() override;
    bool gestureInFlight() const override { return m_dragging || !m_polygon.isEmpty(); }

private:
    void endDrag();
    // Runs the selection filter for a region the caller has put in `regionParams`
    // (rect_* or polygon), with everything else -- the camera, the elements, the mode
    // from `modifiers`, visible-only and the growth -- filled in here.
    void commitSelection(const QString &filterId, QVariantMap regionParams,
                         Qt::KeyboardModifiers modifiers);
    void commitPolygon(Qt::KeyboardModifiers modifiers);
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
