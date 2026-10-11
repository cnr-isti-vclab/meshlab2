#pragma once

#include <QMatrix4x4>
#include <QPointF>
#include <QQuaternion>
#include <QSize>
#include <QVector3D>
#include <QVector4D>
#include <limits>

class QMouseEvent;
class QWheelEvent;
class QJsonObject;
class QString;

// How the scene is projected onto the view.
enum class Projection {
    Perspective = 0,
    // Parallel rays. The view volume is as tall as the perspective view's visible height at
    // the orbit centre, 2 * distance * tan(fovY / 2), so switching projection keeps the
    // framing of everything on the plane through the centre; see ViewTrackball::State.
    Orthographic
};

// A ray through a pixel, in world space. `direction` is a unit vector.
struct ViewRay {
    QVector3D origin;
    QVector3D direction;
};

class ViewTrackball
{
public:
    // The camera is an orbit about `center`: the eye sits `distance` away along the view axis,
    // and `fovYDeg` and `distance` together fix H = 2 * distance * tan(fovY / 2), the world
    // height visible at the centre. Perspective and orthographic both frame that height, which
    // is why the same distance and field of view serve for both, why switching between them
    // changes nothing about what is framed, and why pan, zoom and the gizmo radius -- all
    // statements about H -- do not depend on the projection. What differs is the eye: in
    // perspective rays leave it, in orthographic they are parallel and it only says where
    // depth is measured from.
    struct State {
        QVector3D center = QVector3D(0.0f, 0.0f, 0.0f);
        QQuaternion rotation;
        float distance = 3.0f;
        float radius = 1.0f;
        float fovYDeg = 45.0f;
        // Kept in both projections, so that switching back restores the perspective view.
        Projection projection = Projection::Perspective;
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
    // The other two axes of the same basis. Public alongside the view direction because a
    // screen-space drag has to be turned into a world rotation somewhere, and the view is
    // where that happens.
    QVector3D cameraRight() const;
    QVector3D cameraUp() const;
    float radius() const { return m_radius; }
    float fovYDegrees() const { return m_fovYDeg; }
    Projection projection() const { return m_projection; }
    bool isOrthographic() const { return m_projection == Projection::Orthographic; }
    // Changes only the projection: centre, distance and field of view stay, and with them the
    // height visible at the centre, so what was framed stays framed.
    void setProjection(Projection projection) { m_projection = projection; }
    float nearClipRatio() const { return m_nearClipRatio; }
    // Distances of the clipping planes from the eye. Perspective: the near plane is a fraction
    // of `distance`, and the far plane a generous reach from the orbit centre (below).
    //
    // Orthographic has no depth-precision reason for a near plane close to the eye, so it is
    // fitted: the volume starts just before the nearest point of the scene bounding box, which
    // the view sets with setSceneBounds(), and ends just after the farthest, widened to hold
    // the trackball gizmo about the orbit centre. That can put it *behind the eye*, a negative
    // distance, whenever the centre is closer to the eye than the scene is deep. Without
    // bounds -- before the first frame, or a camera rebuilt from JSON -- it falls back to the
    // generous symmetric reach the perspective far plane uses.
    float nearClipPlaneDistance() const;
    // Far plane must enclose the whole scene, not just the framing radius — when
    // zoomed onto a small detail m_radius is tiny but the scene extends far. The
    // scene radius (set per frame by the view) keeps the far plane wide enough.
    float farClipPlaneDistance() const;
    // The JSON spelling of a projection, as the camera and render-state payloads carry it.
    static QString projectionJsonName(Projection projection);
    void setSceneRadius(float sceneRadius) { m_sceneRadius = qMax(0.0f, sceneRadius); }
    // Every matrix here is in OpenGL convention (clip depth [-1, 1], Y up). What a backend
    // needs on top of that -- Metal and Direct3D keep depth in [0, 1], Vulkan has Y down -- is
    // QRhi::clipSpaceCorrMatrix(), which the view multiplies in where it writes a matrix to the
    // GPU; nothing in the camera depends on the backend.
    // The world-space box of everything visible (transient, set per frame by the view); it
    // fits the orthographic depth range. clearSceneBounds() when there is nothing to fit.
    void setSceneBounds(const QVector3D &minCorner, const QVector3D &maxCorner)
    {
        m_sceneMin = minCorner;
        m_sceneMax = maxCorner;
        m_sceneBoundsValid = true;
    }
    void clearSceneBounds() { m_sceneBoundsValid = false; }
    float gizmoWorldRadius() const;

    QMatrix4x4 viewMatrix() const;
    // `minFarDistance` pushes the far plane out far enough to hold something that is
    // not part of the scene -- a peer view's camera gizmo, whose far plane can sit well
    // outside this view's own.
    //
    // The cost is small but not zero. Depth resolution goes as Z^2/near * (1 - near/far),
    // so with the default near it is unmeasurable; the term only bites when near has been
    // pushed out to cut into the object, where widening far to any distance costs at most
    // 1/(1 - near/far) -- about 1.4x at a cut halfway through a framed object. That goes
    // away once the clipping plane retires the near-plane cutting trick.
    //
    // `maxNearDistance` is the same for the other end: an orthographic view can start its
    // volume behind the eye, and does so as far back as this says. Perspective cannot see
    // behind the eye and ignores it.
    QMatrix4x4 projectionMatrix(float aspect, float minFarDistance = 0.0f,
                                float maxNearDistance = std::numeric_limits<float>::infinity()) const;
    // The ray through the pixel `pos` of a viewport of `viewportSize`, for the current
    // rotation. Perspective: it starts at the eye and passes through the pixel. Orthographic:
    // it runs along the view direction and starts where the pixel meets the near plane.
    // False for a degenerate viewport or matrix.
    bool viewRay(const QPointF &pos, const QSize &viewportSize, ViewRay &ray) const;

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

    // How far the scene can reach from the orbit centre along the view axis, with margin.
    float depthHalfSpan() const { return 4.0f * qMax(m_radius, m_sceneRadius); }
    // The distances, in front of the eye, of the nearest and farthest things an orthographic
    // view must hold; false when the scene bounds are not known.
    bool orthographicDepthRange(float &nearDist, float &farDist) const;
    QVector3D projectOnArcball(const QPointF &pos, const QSize &viewportSize) const;
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
    QVector3D m_sceneMin;       // whole-scene box, for the orthographic depth range (transient)
    QVector3D m_sceneMax;
    bool m_sceneBoundsValid = false;
    float m_fovYDeg = 45.0f;
    Projection m_projection = Projection::Perspective;
    float m_nearClipRatio = 0.0033333333f;
    float m_gizmoBaseRadius = 1.0f;
    float m_gizmoReferenceDistance = 3.0f;
    float m_gizmoReferenceFovYDeg = 45.0f;
};
