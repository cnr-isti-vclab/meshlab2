#pragma once

#include <QQuaternion>
#include <QVector3D>
#include <QWidget>

#include <array>

// Blender-style view orientation gizmo: six axis handles drawn in a corner of
// the viewport, clicked to look down that axis. Deliberately a plain QWidget
// painted with QPainter rather than a pass in the RHI frame - it costs nothing
// on the GPU, but it is composited by the widget stack and so never shows up in
// offscreen captures (renderOffscreenToImage) or headless renders.
class ViewAxisGizmo : public QWidget
{
    Q_OBJECT
public:
    explicit ViewAxisGizmo(QWidget *parent = nullptr);

    // World-to-camera rotation, i.e. ViewTrackball::State::rotation.
    void setOrientation(const QQuaternion &rotation);

signals:
    // Unit world-space direction to move the camera along: the requested eye
    // position is center + axis * distance.
    void axisPicked(const QVector3D &axis);

    // Drag gesture, in gizmo-local coordinates. The view feeds these to its own
    // trackball using the gizmo rect as the viewport, so dragging here is the
    // same arcball as the viewport - just over a much smaller square, which is
    // what makes a small gizmo orbit at a usable speed.
    void orbitBegan(const QPointF &pos);
    void orbitMoved(const QPointF &pos);
    void orbitEnded();

protected:
    void paintEvent(QPaintEvent *e) override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;

public:
    struct Handle {
        QPointF center;      // widget coordinates
        float depth = 0.0f;  // camera-space z; larger is nearer the viewer
        int id = 0;          // axis * 2 + (negative ? 1 : 0)
        int axis = 0;        // 0 = X, 1 = Y, 2 = Z
        bool negative = false;
    };

    // Where each handle lands in widget coordinates, and how near it is. Public because
    // the depth-order test works out for itself where one handle crosses another's stem;
    // it is pure geometry with no side effects.
    std::array<Handle, 6> projectedHandles() const;

    static qreal handleRadius();

private:
    // Returns a handle id, or -1 when the point misses every handle.
    int handleAt(const QPointF &pos) const;
    void setHovered(int id);

    QQuaternion m_rotation;
    int m_hovered = -1;
    // Handle under the press, or -1. Held until release: the snap cannot commit
    // on press without knowing yet whether the gesture turns into a drag.
    int m_pressedHandle = -1;
    QPointF m_pressPos;
    bool m_dragging = false;
};
