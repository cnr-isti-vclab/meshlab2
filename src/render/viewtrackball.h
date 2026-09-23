#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QSize>
#include <QVector3D>
#include <QVector4D>

class QMouseEvent;
class QWheelEvent;
class QJsonObject;
class QString;

class ViewTrackball
{
public:
    struct State {
        QVector3D center = QVector3D(0.0f, 0.0f, 0.0f);
        QQuaternion rotation;
        float distance = 3.0f;
        float radius = 1.0f;
        float fovYDeg = 45.0f;
        float nearClipRatio = 0.0033333333f;
        float gizmoBaseRadius = 1.0f;
        float gizmoReferenceDistance = 3.0f;
        float gizmoReferenceFovYDeg = 45.0f;
    };

    ViewTrackball();

    // Parse a trackball "State" from a JSON object of the form written by
    // RenderWidget::cameraStateJson() (the "trackball" sub-object). Missing
    // fields keep their default; malformed present fields fail. Lives here (in
    // MeshLab2Core) so filters can reconstruct a camera from a CameraState param.
    static bool stateFromJson(const QJsonObject &obj, State &outState, QString *error = nullptr);

    void setFrame(const QVector3D &center, float radius, float distance);
    void resetToFrame(const QVector3D &center, float radius, float distance);
    // Derive the trackball state from an eye position and center point.
    // The rotation is computed so that the camera looks from eye toward center.
    void setFromLookAt(
        const QVector3D &eye,
        const QVector3D &center,
        float fovYDeg = 45.0f);
    State state() const;
    void setState(const State &state);
    void setCenter(const QVector3D &center) { m_center = center; }
    QVector3D center() const { return m_center; }
    QVector3D cameraEyePosition() const
    {
        return m_center + m_rotation.inverted().rotatedVector(QVector3D(0.0f, 0.0f, m_distance));
    }
    QVector3D cameraViewDirection() const
    {
        return m_rotation.inverted().rotatedVector(QVector3D(0.0f, 0.0f, -1.0f)).normalized();
    }
    float radius() const { return m_radius; }
    float fovYDegrees() const { return m_fovYDeg; }
    float nearClipRatio() const { return m_nearClipRatio; }
    float nearClipPlaneDistance() const;
    // Far plane must enclose the whole scene, not just the framing radius — when
    // zoomed onto a small detail m_radius is tiny but the scene extends far. The
    // scene radius (set per frame by the view) keeps the far plane wide enough.
    float farClipPlaneDistance() const { return m_distance + 4.0f * qMax(m_radius, m_sceneRadius); }
    void setSceneRadius(float sceneRadius) { m_sceneRadius = qMax(0.0f, sceneRadius); }
    float gizmoWorldRadius() const;

    QMatrix4x4 viewMatrix() const;
    // `minFarDistance` pushes the far plane out far enough to hold something that is
    // not part of the scene -- a peer view's camera gizmo, whose far plane can sit well
    // outside this view's own. Widening the far plane is close to free: the depth
    // resolution at a given distance Z goes as Z^2/near and barely depends on far.
    QMatrix4x4 projectionMatrix(float aspect, float minFarDistance = 0.0f) const;

    void mousePress(const QMouseEvent *e, const QSize &viewportSize);
    void mouseRelease(const QMouseEvent *e);
    bool mouseMove(const QMouseEvent *e, const QSize &viewportSize);
    bool wheel(const QWheelEvent *e);

private:
    enum class NavigationMode {
        None,
        Rotate,
        Pan
    };

    QVector3D projectOnArcball(const QPointF &pos, const QSize &viewportSize) const;
    QVector3D cameraRight() const;
    QVector3D cameraUp() const;
    QMatrix4x4 viewMatrixForRotation(const QQuaternion &rotation) const;
    QVector3D viewPointForRotation(const QQuaternion &rotation) const;
    bool viewRayFromWindow(const QPointF &pos, const QSize &viewportSize, const QQuaternion &rotation, QVector3D &rayOrigin, QVector3D &rayDir) const;
    bool hitViewPlane(const QPointF &pos, const QSize &viewportSize, const QQuaternion &rotation, QVector3D &hit) const;
    bool hitHyper(const QPointF &pos, const QSize &viewportSize, const QQuaternion &rotation, QVector3D &hit) const;
    bool hitSphereLikeVcg(const QPointF &pos, const QSize &viewportSize, const QQuaternion &rotation, QVector3D &hit) const;

    NavigationMode m_navigationMode = NavigationMode::None;
    QPointF m_lastMousePos;
    QVector3D m_lastArcballVec = QVector3D(0.0f, 0.0f, 1.0f);
    QVector3D m_dragStartHit;
    bool m_dragStartHitValid = false;
    QQuaternion m_dragStartRotation;
    QVector3D m_dragLastAxis = QVector3D(0.0f, 1.0f, 0.0f);
    bool m_dragLastAxisValid = false;
    QQuaternion m_rotation;
    QVector3D m_center;
    float m_distance = 3.0f;
    float m_radius = 1.0f;
    float m_sceneRadius = 1.0f; // whole-scene extent for far-plane clipping (transient)
    float m_fovYDeg = 45.0f;
    float m_nearClipRatio = 0.0033333333f;
    float m_gizmoBaseRadius = 1.0f;
    float m_gizmoReferenceDistance = 3.0f;
    float m_gizmoReferenceFovYDeg = 45.0f;
};
