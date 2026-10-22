#pragma once

#include "interactivetool.h"

#include <QMatrix4x4>
#include <QPoint>
#include <QPointF>
#include <QVector3D>

// Session tool: Blender-style modal transforms of the current layer.
//
// The tool is engaged once and stays engaged; inside it, Move (G), Rotate (R) and
// Scale (S) each start a modal *gesture* that follows the mouse until Enter/click
// commits it or Esc cancels; X, Y and Z constrain it, with Shift to the plane across. That mapping keeps the project's tool contract (engage -> many
// operations -> disengage) while matching Blender's muscle memory, and it lets
// G/R/S switch freely mid-gesture instead of tearing a tool down to build another.
//
// While a gesture runs the preview writes MeshEntry::transform directly rather
// than going through Document::setMeshTransform: that avoids an undo step and a
// meshDataChanged emission (which rebuilds the whole filter menu) on every mouse
// move. On commit the original matrix is restored *first* and then exactly one
// filter call re-applies the change -- otherwise runFilter's "before" snapshot
// would already contain the preview and undo would be a no-op.
class TransformTool final : public InteractiveTool
{
public:
    enum class Gesture { None, Translate, Rotate, Scale };

    QString id() const override;
    QString name() const override;
    QString statusHint() const override;
    QString statusDetail() const override;
    QString iconPath() const override;
    QCursor cursor() const override;

    void activate(RenderWidget &view) override;
    void deactivate(bool commit) override;

    bool mousePress(QMouseEvent *e) override;
    bool mouseMove(QMouseEvent *e) override;
    bool mouseRelease(QMouseEvent *e) override;
    bool keyPress(QKeyEvent *e) override;
    std::vector<ToolControl> controls() const override;
    bool isControlChecked(const QString &id) const override;
    bool isControlEnabled(const QString &id) const override;
    void triggerControl(const QString &id, Qt::KeyboardModifiers modifiers) override;

    void paintOverlay(QPainter &painter) override;

    std::vector<ToolLineSegment> depthCuedLines() const override;
    QColor toolLineColor() const override;

    bool gestureInFlight() const override { return m_gesture != Gesture::None; }
    void cancelGesture() override;

private:
    bool beginGesture(Gesture g);
    void updatePreview();
    void commitGesture();
    void abortGesture();
    void clearGesture();

    QVector3D pivotWorld() const;
    QPointF pivotScreen() const;
    QMatrix4x4 gestureMatrix() const;
    QString readout() const;
    double rotationDegrees() const;
    float worldUnitsPerPixel() const;
    QVector3D viewForward() const;
    float axisDrawLength() const;

    Gesture m_gesture = Gesture::None;
    int m_meshIndex = -1;
    QMatrix4x4 m_originalTransform;   // restored before committing
    QVector3D m_pivot;
    QPointF m_startMouse;
    QPointF m_currentMouse;
    // A gesture started from the strip's button has its pointer on the button, outside the
    // scene: it starts from where the pointer first moves in the view instead.
    bool m_awaitingFirstMove = false;
    int m_axis = -1;                  // -1 = unconstrained, 0/1/2 = X/Y/Z
    bool m_planeConstraint = false;   // constrain to the plane normal to m_axis
    QString m_numeric;                // typed value, empty = follow the mouse
};
