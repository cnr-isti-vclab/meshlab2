#include "viewtrackball.h"

#include "preferences.h"
#include "rendersettingsjson.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMouseEvent>
#include <QString>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <cmath>
#include <limits>

namespace {
// Local JSON helpers for stateFromJson (kept file-local; the render layer has
// its own producer-side variants).
bool jsonReadFloat(const QJsonValue &v, float &out)
{
    if (!v.isDouble())
        return false;
    out = float(v.toDouble());
    return true;
}
bool jsonReadVec3(const QJsonValue &v, QVector3D &out)
{
    if (!v.isArray())
        return false;
    const QJsonArray a = v.toArray();
    if (a.size() != 3)
        return false;
    out = QVector3D(float(a[0].toDouble()), float(a[1].toDouble()), float(a[2].toDouble()));
    return true;
}
bool jsonReadQuat(const QJsonValue &v, QQuaternion &out)
{
    if (!v.isArray())
        return false;
    const QJsonArray a = v.toArray();
    if (a.size() != 4)
        return false;
    // Stored as [x, y, z, w]; QQuaternion ctor is (scalar/w, x, y, z).
    out = QQuaternion(float(a[3].toDouble()), float(a[0].toDouble()),
                      float(a[1].toDouble()), float(a[2].toDouble()));
    return true;
}
} // namespace

bool ViewTrackball::stateFromJson(const QJsonObject &obj, State &outState, QString *error)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    const auto readFloat = [&](const char *key, float &dst) {
        return !obj.contains(QLatin1String(key)) || jsonReadFloat(obj.value(QLatin1String(key)), dst);
    };
    if (obj.contains(QStringLiteral("projection"))) {
        const QString name = obj.value(QStringLiteral("projection")).toString();
        if (name == projectionJsonName(Projection::Perspective))
            outState.projection = Projection::Perspective;
        else if (name == projectionJsonName(Projection::Orthographic))
            outState.projection = Projection::Orthographic;
        else
            return fail(QStringLiteral("'projection' must be \"perspective\" or \"orthographic\"."));
    }
    if (obj.contains(QStringLiteral("center"))
        && !jsonReadVec3(obj.value(QStringLiteral("center")), outState.center))
        return fail(QStringLiteral("'center' must be [x, y, z]."));
    if (obj.contains(QStringLiteral("rotation_xyzw"))
        && !jsonReadQuat(obj.value(QStringLiteral("rotation_xyzw")), outState.rotation))
        return fail(QStringLiteral("'rotation_xyzw' must be [x, y, z, w]."));
    if (!readFloat("distance", outState.distance)
        || !readFloat("radius", outState.radius)
        || !readFloat("fov_y_degrees", outState.fovYDeg)
        || !readFloat("near_clip_ratio", outState.nearClipRatio)
        || !readFloat("gizmo_base_radius", outState.gizmoBaseRadius)
        || !readFloat("gizmo_reference_distance", outState.gizmoReferenceDistance)
        || !readFloat("gizmo_reference_fov_y_degrees", outState.gizmoReferenceFovYDeg))
        return fail(QStringLiteral("a trackball numeric field is not a number."));
    return true;
}

QJsonObject ViewTrackball::stateToJson(const State &state, const State *defaults)
{
    using RenderSettingsJson::fuzzyFloatEqual;
    QJsonObject trackball;
    const State def = defaults ? *defaults : State{};
    auto putFloat = [&](const QString &key, float value, float defValue) {
        if (!defaults || !fuzzyFloatEqual(value, defValue))
            trackball.insert(key, value);
    };
    if (!defaults || !fuzzyFloatEqual(state.center.x(), def.center.x())
        || !fuzzyFloatEqual(state.center.y(), def.center.y())
        || !fuzzyFloatEqual(state.center.z(), def.center.z()))
        trackball.insert(QStringLiteral("center"), QJsonArray{state.center.x(), state.center.y(), state.center.z()});
    const QQuaternion &q = state.rotation;
    if (!defaults || !fuzzyFloatEqual(q.x(), def.rotation.x()) || !fuzzyFloatEqual(q.y(), def.rotation.y())
        || !fuzzyFloatEqual(q.z(), def.rotation.z()) || !fuzzyFloatEqual(q.scalar(), def.rotation.scalar()))
        trackball.insert(QStringLiteral("rotation_xyzw"), QJsonArray{q.x(), q.y(), q.z(), q.scalar()});
    putFloat(QStringLiteral("distance"), state.distance, def.distance);
    putFloat(QStringLiteral("radius"), state.radius, def.radius);
    putFloat(QStringLiteral("fov_y_degrees"), state.fovYDeg, def.fovYDeg);
    if (!defaults || state.projection != def.projection)
        trackball.insert(QStringLiteral("projection"), projectionJsonName(state.projection));
    putFloat(QStringLiteral("near_clip_ratio"), state.nearClipRatio, def.nearClipRatio);
    putFloat(QStringLiteral("gizmo_base_radius"), state.gizmoBaseRadius, def.gizmoBaseRadius);
    putFloat(QStringLiteral("gizmo_reference_distance"), state.gizmoReferenceDistance,
             def.gizmoReferenceDistance);
    putFloat(QStringLiteral("gizmo_reference_fov_y_degrees"), state.gizmoReferenceFovYDeg,
             def.gizmoReferenceFovYDeg);
    return trackball;
}

QString ViewTrackball::cameraStateJson(const State &state, const QSize &viewportPx)
{
    const State defaults;
    const QJsonObject trackball = stateToJson(state, &defaults);
    QJsonObject root;
    root.insert(QStringLiteral("kind"), QStringLiteral("MeshLab.CameraState"));
    root.insert(QStringLiteral("version"), 1);
    if (!trackball.isEmpty())
        root.insert(QStringLiteral("trackball"), trackball);
    if (!viewportPx.isEmpty())
        root.insert(QStringLiteral("viewport_px"), QJsonArray{viewportPx.width(), viewportPx.height()});
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

bool ViewTrackball::stateFromCameraStateJson(const QString &json, State &outState, QString *error,
                                             QSize *viewportPx)
{
    auto fail = [&](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json.trimmed().toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError)
        return fail(QStringLiteral("Invalid JSON: %1").arg(parseError.errorString()));
    if (!doc.isObject())
        return fail(QStringLiteral("Invalid camera JSON: root must be an object."));
    const QJsonObject root = doc.object();
    const QString kind = root.value(QStringLiteral("kind")).toString();
    // Both kinds: state written before the 2026-09 rename says CameraTrackballState.
    if (!kind.isEmpty() && kind != QStringLiteral("MeshLab.CameraState")
        && kind != QStringLiteral("MeshLab.CameraTrackballState"))
        return fail(QStringLiteral("Unsupported camera JSON kind: %1").arg(kind));
    QJsonObject trackball = root;
    if (root.contains(QStringLiteral("trackball"))) {
        if (!root.value(QStringLiteral("trackball")).isObject())
            return fail(QStringLiteral("Invalid camera JSON: 'trackball' must be an object."));
        trackball = root.value(QStringLiteral("trackball")).toObject();
    }
    if (viewportPx) {
        const QJsonArray size = root.value(QStringLiteral("viewport_px")).toArray();
        *viewportPx = size.size() == 2 ? QSize(size[0].toInt(), size[1].toInt()) : QSize();
    }
    return stateFromJson(trackball, outState, error);
}

namespace {
// The min/max below stay compile-time: they are the hard range the preference is
// declared within (resources/preferences.json), not settings themselves.
constexpr float kDefaultTrackballFovYDeg = 45.0f;
constexpr float kMinTrackballFovYDeg = 10.0f;
constexpr float kMaxTrackballFovYDeg = 120.0f;
constexpr float kDefaultNearClipRatio = 1.0f / 300.0f;
constexpr float kMinNearClipRatio = 1e-5f;
constexpr float kMaxNearClipRatio = 2.0f;
constexpr float kTrackballHyperbolaCutAngleDeg = 45.0f;
const QQuaternion kIdentityRotation;
QQuaternion defaultTrackballRotation()
{
    return kIdentityRotation;
}
}

ViewTrackball::ViewTrackball()
{
    m_rotation = defaultTrackballRotation();
}

QString ViewTrackball::projectionJsonName(Projection projection)
{
    return projection == Projection::Orthographic ? QStringLiteral("orthographic")
                                                  : QStringLiteral("perspective");
}

ViewTrackball::State ViewTrackball::state() const
{
    State s;
    s.center = m_center;
    s.rotation = m_rotation;
    s.distance = m_distance;
    s.radius = m_radius;
    s.fovYDeg = m_fovYDeg;
    s.projection = m_projection;
    s.nearClipRatio = m_nearClipRatio;
    s.gizmoBaseRadius = m_gizmoBaseRadius;
    s.gizmoReferenceDistance = m_gizmoReferenceDistance;
    s.gizmoReferenceFovYDeg = m_gizmoReferenceFovYDeg;
    return s;
}

void ViewTrackball::setState(const State &state)
{
    m_center = state.center;
    m_radius = qMax(1e-4f, state.radius);
    m_distance = qMax(0.01f * m_radius, state.distance);
    m_fovYDeg = std::clamp(state.fovYDeg, kMinTrackballFovYDeg, kMaxTrackballFovYDeg);
    m_projection = state.projection;
    m_nearClipRatio = std::clamp(state.nearClipRatio, kMinNearClipRatio, kMaxNearClipRatio);

    const float rotLen2 = state.rotation.lengthSquared();
    if (rotLen2 > 1e-12f)
        m_rotation = state.rotation.normalized();
    else
        m_rotation = defaultTrackballRotation();

    m_gizmoBaseRadius = qMax(1e-4f, state.gizmoBaseRadius);
    m_gizmoReferenceDistance = qMax(1e-4f, state.gizmoReferenceDistance);
    m_gizmoReferenceFovYDeg =
        std::clamp(state.gizmoReferenceFovYDeg, kMinTrackballFovYDeg, kMaxTrackballFovYDeg);

    m_navigationMode = NavigationMode::None;
    m_dragStartHitValid = false;
    m_dragLastAxisValid = false;
    m_lastArcballVec = QVector3D(0.0f, 0.0f, 1.0f);
}

void ViewTrackball::setFrame(const QVector3D &center, float radius, float distance)
{
    m_center = center;
    m_radius = qMax(1e-4f, radius);
    m_distance = qMax(0.01f * m_radius, distance);
    // Keep gizmo screen size stable under dolly zoom, using this frame as reference.
    m_gizmoBaseRadius = m_radius * 1.02f;
    m_gizmoReferenceDistance = m_distance;
    m_gizmoReferenceFovYDeg = m_fovYDeg;
}

void ViewTrackball::setFromLookAt(
    const QVector3D &eye, const QVector3D &center, float fovYDeg)
{
    QVector3D dir = (eye - center);
    float d = dir.length();
    if (d < 1e-6f) return;

    m_center = center;
    m_distance = d;
    m_radius = qMax(1e-4f, m_radius);
    m_fovYDeg = qBound(kMinTrackballFovYDeg, fovYDeg, kMaxTrackballFovYDeg);

    // Compute rotation: maps camera (0,0,1) → world-space direction from center to eye
    QVector3D up(0, 1, 0);
    if (std::abs(QVector3D::dotProduct(dir.normalized(), up)) > 0.99f)
        up = QVector3D(1, 0, 0);
    QVector3D right = QVector3D::crossProduct(up, dir).normalized();
    QVector3D alignedUp = QVector3D::crossProduct(dir.normalized(), right).normalized();

    // Build rotation matrix: columns are right, up, forward
    QMatrix3x3 rotMat;
    rotMat(0, 0) = right.x(); rotMat(0, 1) = right.y(); rotMat(0, 2) = right.z();
    rotMat(1, 0) = alignedUp.x(); rotMat(1, 1) = alignedUp.y(); rotMat(1, 2) = alignedUp.z();
    rotMat(2, 0) = dir.x() / d; rotMat(2, 1) = dir.y() / d; rotMat(2, 2) = dir.z() / d;

    // QQuaternion::fromRotationMatrix interprets rows as directions to align.
    // m_rotation maps world_to_camera, so its inverse maps camera_to_world.
    // The rotation matrix above maps from world to camera (column 0 = world right
    // → camera x, etc.).  Convert to quaternion.
    m_rotation = QQuaternion::fromRotationMatrix(rotMat);

    m_gizmoBaseRadius = m_radius * 1.02f;
    m_gizmoReferenceDistance = m_distance;
    m_gizmoReferenceFovYDeg = m_fovYDeg;
}

QString ViewTrackball::eyeFramingText(EyeFraming result)
{
    const auto tr = [](const char *text) { return QCoreApplication::translate("ViewTrackball", text); };
    switch (result) {
    case EyeFraming::Framed: return tr("Framed from the current viewpoint");
    case EyeFraming::Orthographic: return tr("Framing from a viewpoint needs a perspective camera");
    case EyeFraming::NoPoints: return tr("Nothing to frame");
    case EyeFraming::BehindEye: return tr("Cannot frame from this viewpoint: part of the scene is behind it");
    case EyeFraming::TooClose: return tr("Cannot frame from this viewpoint: part of the scene is too close to it");
    case EyeFraming::TooWide: return tr("Cannot frame from this viewpoint: the scene needs a wider field of view than the camera allows");
    case EyeFraming::TooNarrow: return tr("Cannot frame from this viewpoint: the scene is too far for the narrowest field of view");
    }
    return {};
}

ViewTrackball::EyeFraming ViewTrackball::frameFromEye(const std::vector<QVector3D> &points, float &aspect,
                                                      bool fitAspect)
{
    if (m_projection == Projection::Orthographic)
        return EyeFraming::Orthographic;
    if (points.empty() || !(fitAspect || aspect > 0.0f))
        return EyeFraming::NoPoints;

    QVector3D boxMin = points.front(), boxMax = points.front();
    for (const QVector3D &p : points) {
        boxMin = QVector3D(std::min(boxMin.x(), p.x()), std::min(boxMin.y(), p.y()), std::min(boxMin.z(), p.z()));
        boxMax = QVector3D(std::max(boxMax.x(), p.x()), std::max(boxMax.y(), p.y()), std::max(boxMax.z(), p.z()));
    }
    const QVector3D middle = 0.5f * (boxMin + boxMax);
    const float radius = qMax(1e-4f, 0.5f * (boxMax - boxMin).length());

    const QVector3D eye = cameraEyePosition();
    const QVector3D oldUp = cameraUp();
    const QVector3D oldRight = cameraRight();
    QVector3D dir = (middle - eye).normalized();
    QVector3D right, up;
    float xMin = 0.0f, xMax = 0.0f, yMin = 0.0f, yMax = 0.0f, minDepth = 0.0f;
    // Aim, measure where the points fall on the image plane at unit depth, and turn the axis
    // until that extent is centred. The turn is half the angle by which the extent is off
    // centre, sideways and then up: turning about the up axis shifts the angle of every point
    // in the horizontal plane by just that much, where aiming at the middle of the extent on
    // the image plane would overshoot by 1 + x^2 far off the axis, and diverge in a wide view.
    // Turning up changes the horizontal angles a little, so a handful of rounds settle both.
    for (int round = 0;; ++round) {
        if (dir.isNull())
            return EyeFraming::BehindEye;
        right = QVector3D::crossProduct(dir, oldUp);
        if (right.lengthSquared() < 1e-8f) // looking along the old up: keep the old right
            right = oldRight - QVector3D::dotProduct(oldRight, dir) * dir;
        right.normalize();
        up = QVector3D::crossProduct(right, dir);

        xMin = yMin = minDepth = std::numeric_limits<float>::max();
        xMax = yMax = std::numeric_limits<float>::lowest();
        for (const QVector3D &p : points) {
            const QVector3D d = p - eye;
            const float depth = QVector3D::dotProduct(d, dir);
            if (!(depth > 0.0f))
                return EyeFraming::BehindEye;
            minDepth = std::min(minDepth, depth);
            const float x = QVector3D::dotProduct(d, right) / depth;
            const float y = QVector3D::dotProduct(d, up) / depth;
            xMin = std::min(xMin, x);
            xMax = std::max(xMax, x);
            yMin = std::min(yMin, y);
            yMax = std::max(yMax, y);
        }
        const float yaw = 0.5f * (std::atan(xMin) + std::atan(xMax));
        const float pitch = 0.5f * (std::atan(yMin) + std::atan(yMax));
        if ((std::abs(yaw) < 1e-6f && std::abs(pitch) < 1e-6f) || round == 32)
            break;
        const QVector3D turned = std::cos(yaw) * dir + std::sin(yaw) * right;
        dir = (std::cos(pitch) * turned + std::sin(pitch) * up).normalized();
    }

    const float halfWidth = std::max(xMax, -xMin);
    const float halfHeight = std::max(yMax, -yMin);
    const float tanHalfFov = fitAspect ? halfHeight : std::max(halfHeight, halfWidth / aspect);
    const float fovYDeg = qRadiansToDegrees(2.0f * std::atan(tanHalfFov));
    if (fovYDeg > kMaxTrackballFovYDeg)
        return EyeFraming::TooWide;
    if (fovYDeg < kMinTrackballFovYDeg)
        return EyeFraming::TooNarrow;
    // The near plane as nearClipPlaneDistance() will put it for the new distance and radius.
    const float distance = std::max(QVector3D::dotProduct(middle - eye, dir), minDepth);
    const float nearPlane = std::clamp(distance * m_nearClipRatio, 1e-5f * radius, 1000.0f * radius);
    if (distance < 0.01f * radius || minDepth < nearPlane)
        return EyeFraming::TooClose;

    // Rows are the camera axes in world space, as in setFromLookAt; the camera looks down -z.
    QMatrix3x3 rotMat;
    rotMat(0, 0) = right.x(); rotMat(0, 1) = right.y(); rotMat(0, 2) = right.z();
    rotMat(1, 0) = up.x(); rotMat(1, 1) = up.y(); rotMat(1, 2) = up.z();
    rotMat(2, 0) = -dir.x(); rotMat(2, 1) = -dir.y(); rotMat(2, 2) = -dir.z();
    m_rotation = QQuaternion::fromRotationMatrix(rotMat);
    if (fitAspect)
        aspect = halfWidth / halfHeight;
    m_fovYDeg = fovYDeg;
    // Not setFrame, which would size the gizmo to `radius`: a sphere of the points' half-diagonal
    // does not fit a view framed this tightly. The gizmo keeps its size on screen, as in a zoom.
    m_center = eye + distance * dir;
    m_radius = radius;
    m_distance = distance;
    return EyeFraming::Framed;
}

void ViewTrackball::resetToFrame(const QVector3D &center, float radius, float distance)
{
    m_fovYDeg = std::clamp(
        float(Preferences::instance().doubleValue(QStringLiteral("view.fieldOfView"))),
        kMinTrackballFovYDeg,
        kMaxTrackballFovYDeg);
    m_nearClipRatio = std::clamp(
        float(Preferences::instance().doubleValue(QStringLiteral("view.nearClipRatio"))),
        kMinNearClipRatio,
        kMaxNearClipRatio);
    setFrame(center, radius, distance);
    m_rotation = defaultTrackballRotation();
    m_navigationMode = NavigationMode::None;
    m_dragStartHitValid = false;
    m_dragLastAxisValid = false;
    m_lastArcballVec = QVector3D(0.0f, 0.0f, 1.0f);
}

QMatrix4x4 ViewTrackball::viewMatrix() const
{
    return viewMatrixForRotation(m_rotation);
}

QMatrix4x4 ViewTrackball::viewMatrixForRotation(const QQuaternion &rotation) const
{
    QMatrix4x4 view;
    view.translate(0.0f, 0.0f, -m_distance);
    view.rotate(rotation);
    view.translate(-m_center);
    return view;
}

float ViewTrackball::gizmoWorldRadius() const
{
    const float refDist = qMax(1e-4f, m_gizmoReferenceDistance);
    const float fovRad = qDegreesToRadians(m_fovYDeg);
    const float refFovRad = qDegreesToRadians(m_gizmoReferenceFovYDeg);
    const float tanHalfFov = qMax(1e-6f, std::tan(0.5f * fovRad));
    const float tanHalfFovRef = qMax(1e-6f, std::tan(0.5f * refFovRad));
    const float scale = (m_distance / refDist) * (tanHalfFov / tanHalfFovRef);
    return qMax(1e-4f, m_gizmoBaseRadius * scale);
}

bool ViewTrackball::orthographicDepthRange(float &nearDist, float &farDist) const
{
    if (!m_sceneBoundsValid)
        return false;
    // The eight corners of the box, as distances in front of the eye.
    const QMatrix4x4 view = viewMatrix();
    float lo = std::numeric_limits<float>::infinity();
    float hi = -lo;
    for (int corner = 0; corner < 8; ++corner) {
        const QVector3D p((corner & 1) ? m_sceneMax.x() : m_sceneMin.x(),
                          (corner & 2) ? m_sceneMax.y() : m_sceneMin.y(),
                          (corner & 4) ? m_sceneMax.z() : m_sceneMin.z());
        const float dist = -(view * p).z();
        lo = std::min(lo, dist);
        hi = std::max(hi, dist);
    }
    // The gizmo about the orbit centre, which is `distance` in front of the eye, is drawn in
    // the scene's depth and reaches past a small box.
    const float gizmo = gizmoWorldRadius();
    lo = std::min(lo, m_distance - gizmo);
    hi = std::max(hi, m_distance + gizmo);
    // A little slack, so that surfaces lying on the box faces, wide lines and the tips of
    // normals are not cut; and some for a box flat against the view, which has no depth.
    const float height = 2.0f * m_distance * std::tan(0.5f * qDegreesToRadians(m_fovYDeg));
    const float pad = 0.05f * (hi - lo) + 1.0e-3f * height;
    nearDist = lo - pad;
    farDist = hi + pad;
    return true;
}

float ViewTrackball::farClipPlaneDistance() const
{
    float nearDist, farDist;
    if (m_projection == Projection::Orthographic && orthographicDepthRange(nearDist, farDist))
        return farDist;
    return m_distance + depthHalfSpan();
}

float ViewTrackball::nearClipPlaneDistance() const
{
    if (m_projection == Projection::Orthographic) {
        float nearDist, farDist;
        if (orthographicDepthRange(nearDist, farDist))
            return nearDist;
        return m_distance - depthHalfSpan();
    }
    const float r = qMax(1e-4f, m_radius);
    const float dist = qMax(1e-4f, m_distance);
    return std::clamp(dist * m_nearClipRatio, 1e-5f * r, 1000.0f * r);
}

void ViewTrackball::mousePress(const QMouseEvent *e, const QSize &viewportSize)
{
    m_lastMousePos = e->position();
    m_lastArcballVec = projectOnArcball(m_lastMousePos, viewportSize);

    const bool ctrlPan = (e->button() == Qt::LeftButton)
        && (e->modifiers() & Qt::ControlModifier);
    if (e->button() == Qt::MiddleButton || e->button() == Qt::RightButton || ctrlPan) {
        m_navigationMode = NavigationMode::Pan;
        m_dragStartHitValid = false;
        m_dragLastAxisValid = false;
    } else if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier)) {
        m_navigationMode = NavigationMode::Roll;
        m_dragStartHitValid = false;
        m_dragLastAxisValid = false;
    } else if (e->button() == Qt::LeftButton) {
        m_navigationMode = NavigationMode::Rotate;
        m_dragStartRotation = m_rotation;
        m_dragLastAxisValid = false;
        m_dragStartHitValid =
            hitSphereLikeVcg(m_lastMousePos, viewportSize, kIdentityRotation, m_dragStartHit);
    } else {
        m_navigationMode = NavigationMode::None;
        m_dragStartHitValid = false;
        m_dragLastAxisValid = false;
    }
}

void ViewTrackball::mouseRelease(const QMouseEvent *e)
{
    if (e->buttons() == Qt::NoButton) {
        m_navigationMode = NavigationMode::None;
        m_dragStartHitValid = false;
        m_dragLastAxisValid = false;
    }
}

bool ViewTrackball::mouseMove(const QMouseEvent *e, const QSize &viewportSize)
{
    const QPointF currentPos = e->position();
    const QPointF delta = currentPos - m_lastMousePos;
    const QPointF previousPos = m_lastMousePos;
    m_lastMousePos = currentPos;

    if (m_navigationMode == NavigationMode::Roll && (e->buttons() & Qt::LeftButton)) {
        // The scene follows the cursor as it goes round the centre of the viewport, counter-clockwise
        // on screen for a positive angle (the screen's y runs down). A rotation about the view's own
        // axis (+z in view space, towards the viewer) composes on the left of the camera rotation.
        // Close to the centre the angle is all noise, so nothing turns there.
        const QPointF centre(0.5 * viewportSize.width(), 0.5 * viewportSize.height());
        const QPointF from = previousPos - centre;
        const QPointF to = currentPos - centre;
        constexpr double kMinRadiusPx = 32.0;
        if (std::hypot(from.x(), from.y()) < kMinRadiusPx
            || std::hypot(to.x(), to.y()) < kMinRadiusPx)
            return false;
        const double angle = std::atan2(-to.y(), to.x()) - std::atan2(-from.y(), from.x());
        // atan2 wraps at +-pi, which the cursor crossing the negative x axis would show as a jump.
        const double wrapped = std::remainder(angle, 2.0 * M_PI);
        m_rotation = (QQuaternion::fromAxisAndAngle(
                          QVector3D(0.0f, 0.0f, 1.0f), float(qRadiansToDegrees(wrapped)))
                      * m_rotation).normalized();
        return true;
    }

    if (m_navigationMode == NavigationMode::Rotate && (e->buttons() & Qt::LeftButton)) {
        QVector3D hitNew;
        const bool hitNewValid =
            hitSphereLikeVcg(currentPos, viewportSize, kIdentityRotation, hitNew);
        if (m_dragStartHitValid && hitNewValid) {
            const QVector3D from = m_dragStartHit - m_center;
            const QVector3D to = hitNew - m_center;
            QVector3D axis = QVector3D::crossProduct(to, from);
            const float axisLen2 = axis.lengthSquared();
            QVector3D fromN = from;
            QVector3D toN = to;
            const float fromLen = fromN.length();
            const float toLen = toN.length();
            if (fromLen > 1e-12f && toLen > 1e-12f) {
                fromN /= fromLen;
                toN /= toLen;
            }
            const float dotVal = std::clamp(QVector3D::dotProduct(toN, fromN), -1.0f, 1.0f);
            const float angleRad = std::acos(dotVal);

            if (axisLen2 > 1e-12f) {
                axis.normalize();
                m_dragLastAxis = axis;
                m_dragLastAxisValid = true;
            } else if (dotVal > 0.999999f) {
                // Cursor returned to drag start: exact identity wrt press orientation.
                m_rotation = m_dragStartRotation;
                return true;
            } else if (dotVal < -0.999999f) {
                // 180-degree singularity: keep the previous stable axis to avoid inversion flicker.
                if (m_dragLastAxisValid) {
                    axis = m_dragLastAxis;
                } else {
                    axis = QVector3D::crossProduct(fromN, QVector3D(1.0f, 0.0f, 0.0f));
                    if (axis.lengthSquared() < 1e-12f)
                        axis = QVector3D::crossProduct(fromN, QVector3D(0.0f, 1.0f, 0.0f));
                    if (axis.lengthSquared() < 1e-12f)
                        axis = QVector3D(0.0f, 0.0f, 1.0f);
                    axis.normalize();
                    m_dragLastAxis = axis;
                    m_dragLastAxisValid = true;
                }
            } else {
                // Near-singular numeric case: keep current orientation and wait next sample.
                return true;
            }

            const float radius = qMax(1e-6f, gizmoWorldRadius());
            const float chordOverR = (hitNew - m_dragStartHit).length() / radius;
            const float phi = qMax(angleRad, chordOverR);

            const QQuaternion deltaRot =
                QQuaternion::fromAxisAndAngle(axis, -qRadiansToDegrees(phi));
            m_rotation = (deltaRot * m_dragStartRotation).normalized();
        } else {
            // Fallback for degenerate cases.
            const QVector3D currentVec = projectOnArcball(currentPos, viewportSize);
            QVector3D axis = QVector3D::crossProduct(m_lastArcballVec, currentVec);
            const float axisLen2 = axis.lengthSquared();
            if (axisLen2 > 1e-12f) {
                const float dotVal = std::clamp(
                    QVector3D::dotProduct(m_lastArcballVec, currentVec),
                    -1.0f,
                    1.0f);
                const float angleRad = std::atan2(std::sqrt(axisLen2), dotVal);
                axis.normalize();
                const QQuaternion deltaRot =
                    QQuaternion::fromAxisAndAngle(axis, qRadiansToDegrees(angleRad));
                m_rotation = (deltaRot * m_rotation).normalized();
            }
            m_lastArcballVec = currentVec;
        }
        return true;
    }

    const bool panButtons = (e->buttons() & (Qt::MiddleButton | Qt::RightButton))
        || ((e->buttons() & Qt::LeftButton) && (e->modifiers() & Qt::ControlModifier));
    if (m_navigationMode == NavigationMode::Pan && panButtons) {
        const float viewportH = float(qMax(1, viewportSize.height()));
        const float fovYRad = qDegreesToRadians(m_fovYDeg);
        const float worldPerPixel = (2.0f * m_distance * std::tan(0.5f * fovYRad)) / viewportH;
        const QVector3D panWorld =
            (-cameraRight() * float(delta.x()) + cameraUp() * float(delta.y())) * worldPerPixel;
        m_center += panWorld;
        return true;
    }

    return false;
}

bool ViewTrackball::wheel(const QWheelEvent *e)
{
    float steps = e->angleDelta().y() / 120.0f;
    if (qFuzzyIsNull(steps) && !e->pixelDelta().isNull())
        steps = e->pixelDelta().y() / 120.0f;
    if (qFuzzyIsNull(steps))
        return false;

    // Ctrl+wheel does not arrive here: the view keeps that chord for the clipping plane and
    // returns before calling this. It used to scale m_nearClipRatio, which cut into the
    // object by moving the near plane -- and wrecked the depth buffer whenever it was
    // scrubbed back out. The ratio is now a depth-precision setting and nothing else, set
    // from the preferences.
    const bool fovMode = (e->modifiers() & Qt::ShiftModifier);
    if (fovMode) {
        // The field-of-view wheel trades field of view for distance at constant H: forward
        // narrows the field of view, flattening the perspective, and the limit of that is the
        // orthographic view. So the wheel does not stop at the narrowest field of view: it
        // goes on into orthographic, and turning it back leaves orthographic for perspective
        // at that same field of view. H is untouched throughout, so nothing jumps.
        if (m_projection == Projection::Orthographic) {
            if (steps > 0.0f)
                return false; // nothing flatter than parallel
            m_projection = Projection::Perspective;
        }

        const float oldFovRad = qDegreesToRadians(m_fovYDeg);
        const float oldTanHalfFov = qMax(1e-6f, std::tan(0.5f * oldFovRad));
        const float constantScreenScale = m_distance * oldTanHalfFov;

        float newFov = m_fovYDeg * std::pow(0.90f, steps);
        if (newFov < kMinTrackballFovYDeg && steps > 0.0f)
            m_projection = Projection::Orthographic;
        newFov = std::clamp(newFov, kMinTrackballFovYDeg, kMaxTrackballFovYDeg);
        m_fovYDeg = newFov;

        const float newFovRad = qDegreesToRadians(m_fovYDeg);
        const float newTanHalfFov = qMax(1e-6f, std::tan(0.5f * newFovRad));
        m_distance = constantScreenScale / newTanHalfFov;
    } else {
        m_distance *= std::pow(0.85f, steps);
    }

    const float minDist = qMax(1e-4f, 0.01f * m_radius);
    const float maxDist = qMax(minDist * 2.0f, 1000.0f * m_radius);
    m_distance = std::clamp(m_distance, minDist, maxDist);
    return true;
}

QVector3D ViewTrackball::projectOnArcball(const QPointF &pos, const QSize &viewportSize) const
{
    const float w = float(qMax(1, viewportSize.width()));
    const float h = float(qMax(1, viewportSize.height()));
    float x = (2.0f * float(pos.x()) - w) / w;
    float y = (h - 2.0f * float(pos.y())) / h;

    const float len2 = x * x + y * y;
    float z = 0.0f;
    if (len2 <= 1.0f) {
        z = std::sqrt(1.0f - len2);
    } else {
        const float invLen = 1.0f / std::sqrt(len2);
        x *= invLen;
        y *= invLen;
    }
    return QVector3D(x, y, z).normalized();
}

QVector3D ViewTrackball::cameraRight() const
{
    return m_rotation.conjugated().rotatedVector(QVector3D(1.0f, 0.0f, 0.0f));
}

QVector3D ViewTrackball::cameraUp() const
{
    return m_rotation.conjugated().rotatedVector(QVector3D(0.0f, 1.0f, 0.0f));
}

QMatrix4x4 ViewTrackball::projectionMatrix(float aspect, float minFarDistance,
                                           float maxNearDistance) const
{
    const float nearPlane = nearClipPlaneDistance();
    float farPlane = farClipPlaneDistance();
    if (std::isfinite(minFarDistance) && minFarDistance > farPlane) {
        // Capped only to keep the matrix finite if a caller hands us something absurd. A
        // tighter bound would not buy precision: the loss saturates at 1/(1 - near/far)
        // however far out the plane goes, so it is already nearly all paid at 10x.
        farPlane = qMin(minFarDistance, farPlane * 1.0e4f);
    }
    QMatrix4x4 proj;
    if (m_projection == Projection::Orthographic) {
        // The volume is H tall, H = 2 * distance * tan(fovY / 2), the height perspective
        // frames at the orbit centre, and as wide as the aspect makes it. Depth is linear,
        // so the planes cost no precision however far apart they are; `near` may be negative.
        float orthoNear = nearPlane;
        if (std::isfinite(maxNearDistance) && maxNearDistance < orthoNear)
            orthoNear = qMax(maxNearDistance, orthoNear - 1.0e4f * (farPlane - orthoNear));
        const float halfH = m_distance * std::tan(0.5f * qDegreesToRadians(m_fovYDeg));
        const float halfW = halfH * aspect;
        proj.ortho(-halfW, halfW, -halfH, halfH, orthoNear, farPlane);
        return proj;
    }
    proj.perspective(m_fovYDeg, aspect, nearPlane, farPlane);
    return proj;
}

QVector3D ViewTrackball::viewPointForRotation(const QQuaternion &rotation) const
{
    bool ok = false;
    const QMatrix4x4 invView = viewMatrixForRotation(rotation).inverted(&ok);
    if (!ok)
        return QVector3D(0.0f, 0.0f, 0.0f);
    const QVector4D vp = invView * QVector4D(0.0f, 0.0f, 0.0f, 1.0f);
    return vp.toVector3D();
}

bool ViewTrackball::viewRay(const QPointF &pos, const QSize &viewportSize, ViewRay &ray) const
{
    return viewRayFromWindow(pos, viewportSize, m_rotation, ray.origin, ray.direction);
}

bool ViewTrackball::viewRayFromWindow(
    const QPointF &pos,
    const QSize &viewportSize,
    const QQuaternion &rotation,
    QVector3D &rayOrigin,
    QVector3D &rayDir) const
{
    if (viewportSize.width() <= 1 || viewportSize.height() <= 1)
        return false;

    const float aspect = float(viewportSize.width()) / float(viewportSize.height());
    const QMatrix4x4 mvp = projectionMatrix(aspect) * viewMatrixForRotation(rotation);
    bool okInv = false;
    const QMatrix4x4 invMvp = mvp.inverted(&okInv);
    if (!okInv)
        return false;

    const float xNdc = (2.0f * float(pos.x()) / float(viewportSize.width())) - 1.0f;
    const float yNdc = 1.0f - (2.0f * float(pos.y()) / float(viewportSize.height()));

    QVector4D pFar = invMvp * QVector4D(xNdc, yNdc, 1.0f, 1.0f);
    if (qFuzzyIsNull(pFar.w()))
        return false;
    pFar /= pFar.w();

    // Every ray leaves the eye in perspective; in orthographic the rays are parallel and
    // the pixel decides where each one starts, on the near plane.
    if (m_projection == Projection::Orthographic) {
        QVector4D pNear = invMvp * QVector4D(xNdc, yNdc, -1.0f, 1.0f);
        if (qFuzzyIsNull(pNear.w()))
            return false;
        pNear /= pNear.w();
        rayOrigin = pNear.toVector3D();
    } else {
        rayOrigin = viewPointForRotation(rotation);
    }
    rayDir = pFar.toVector3D() - rayOrigin;
    const float d2 = rayDir.lengthSquared();
    if (d2 < 1e-12f)
        return false;
    rayDir /= std::sqrt(d2);
    return true;
}

bool ViewTrackball::hitViewPlane(
    const QPointF &pos,
    const QSize &viewportSize,
    const QQuaternion &rotation,
    QVector3D &hit) const
{
    QVector3D rayOrigin, rayDir;
    if (!viewRayFromWindow(pos, viewportSize, rotation, rayOrigin, rayDir))
        return false;

    QVector3D plNorm = viewPointForRotation(rotation) - m_center;
    const float nLen = plNorm.length();
    if (nLen < 1e-12f)
        return false;
    plNorm /= nLen;

    const float den = QVector3D::dotProduct(plNorm, rayDir);
    if (std::abs(den) < 1e-12f)
        return false;
    const float t = QVector3D::dotProduct(plNorm, (m_center - rayOrigin)) / den;
    hit = rayOrigin + rayDir * t;
    return true;
}

bool ViewTrackball::hitHyper(
    const QPointF &pos,
    const QSize &viewportSize,
    const QQuaternion &rotation,
    QVector3D &hit) const
{
    QVector3D hitPlane;
    if (!hitViewPlane(pos, viewportSize, rotation, hitPlane))
        return false;

    const QVector3D vp = viewPointForRotation(rotation);
    const float hitPlaneY = (hitPlane - m_center).length();
    const float viewPointX = (vp - m_center).length();
    if (hitPlaneY < 1e-12f || viewPointX < 1e-12f)
        return false;

    const float r = qMax(1e-6f, gizmoWorldRadius());
    const float a = hitPlaneY / viewPointX;
    const float b = -hitPlaneY;
    const float c = (r * r) / 2.0f;
    const float delta = b * b - 4.0f * a * c;
    if (delta <= 0.0f)
        return false;

    const float x1 = (-b - std::sqrt(delta)) / (2.0f * a);
    const float x2 = (-b + std::sqrt(delta)) / (2.0f * a);
    const float xval = qMin(x1, x2);
    if (std::abs(xval) < 1e-12f)
        return false;
    const float yval = c / xval;

    QVector3D dirRadial = hitPlane - m_center;
    const float drLen = dirRadial.length();
    if (drLen < 1e-12f)
        return false;
    dirRadial /= drLen;

    QVector3D dirView = vp - m_center;
    const float dvLen = dirView.length();
    if (dvLen < 1e-12f)
        return false;
    dirView /= dvLen;

    hit = m_center + dirRadial * yval + dirView * xval;
    return true;
}

bool ViewTrackball::hitSphereLikeVcg(
    const QPointF &pos,
    const QSize &viewportSize,
    const QQuaternion &rotation,
    QVector3D &hit) const
{
    QVector3D rayOrigin, rayDir;
    if (!viewRayFromWindow(pos, viewportSize, rotation, rayOrigin, rayDir))
        return false;

    const float r = qMax(1e-6f, gizmoWorldRadius());
    const QVector3D oc = rayOrigin - m_center;
    const float a = QVector3D::dotProduct(rayDir, rayDir);
    const float b = 2.0f * QVector3D::dotProduct(oc, rayDir);
    const float c = QVector3D::dotProduct(oc, oc) - r * r;
    const float disc = b * b - 4.0f * a * c;

    bool resSp = false;
    QVector3D hitSphere;
    if (disc >= 0.0f) {
        const float sqrtDisc = std::sqrt(disc);
        const float inv2a = 1.0f / (2.0f * a);
        const float t1 = (-b - sqrtDisc) * inv2a;
        const float t2 = (-b + sqrtDisc) * inv2a;
        float t = std::numeric_limits<float>::max();
        if (t1 > 1e-6f)
            t = t1;
        if (t2 > 1e-6f && t2 < t)
            t = t2;
        if (t < std::numeric_limits<float>::max()) {
            hitSphere = rayOrigin + rayDir * t;
            resSp = true;
        }
    }

    QVector3D hitHp;
    const bool resHp = hitHyper(pos, viewportSize, rotation, hitHp);

    if (!resSp && !resHp) {
        // Degenerate fallback: closest point on ray to center.
        const float t = QVector3D::dotProduct((m_center - rayOrigin), rayDir);
        hit = rayOrigin + rayDir * t;
        return true;
    }
    if (resSp && !resHp) {
        hit = hitSphere;
        return true;
    }
    if (!resSp && resHp) {
        hit = hitHp;
        return true;
    }

    const QVector3D vp = viewPointForRotation(rotation);
    QVector3D aVec = vp - m_center;
    QVector3D bVec = hitSphere - m_center;
    const float aLen = aVec.length();
    const float bLen = bVec.length();
    if (aLen < 1e-12f || bLen < 1e-12f) {
        hit = hitSphere;
        return true;
    }
    aVec /= aLen;
    bVec /= bLen;
    const float angleDeg = qRadiansToDegrees(std::acos(std::clamp(QVector3D::dotProduct(aVec, bVec), -1.0f, 1.0f)));
    hit = (angleDeg < kTrackballHyperbolaCutAngleDeg) ? hitSphere : hitHp;
    return true;
}
