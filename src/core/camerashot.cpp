#include "camerashot.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
vcg::Point3f toVcgPoint(const QVector3D &p)
{
    return vcg::Point3f(p.x(), p.y(), p.z());
}

QVector3D toQVector(const vcg::Point3f &p)
{
    return QVector3D(p[0], p[1], p[2]);
}

vcg::Camera<float>::CameraType toVcgCameraType(CameraShot::CameraType type)
{
    using VcgCamera = vcg::Camera<float>;
    switch (type) {
    case CameraShot::CameraType::Perspective:  return VcgCamera::PERSPECTIVE;
    case CameraShot::CameraType::Orthographic: return VcgCamera::ORTHO;
    }
    return VcgCamera::PERSPECTIVE;
}

CameraShot::CameraType fromVcgCameraType(vcg::Camera<float>::CameraType type)
{
    using VcgCamera = vcg::Camera<float>;
    switch (type) {
    case VcgCamera::PERSPECTIVE: return CameraShot::CameraType::Perspective;
    case VcgCamera::ORTHO:       return CameraShot::CameraType::Orthographic;
    case VcgCamera::ISOMETRIC:
    case VcgCamera::CAVALIERI:   break;
    }
    return CameraShot::CameraType::Perspective;
}

QVector2D invalidPoint2()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    return QVector2D(nan, nan);
}

QMatrix4x4 toQMatrix(const vcg::Matrix44f &m)
{
    QMatrix4x4 result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            result(r, c) = m[r][c];
    return result;
}

vcg::Matrix44f toVcgMatrix(const QMatrix4x4 &m)
{
    vcg::Matrix44f result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            result[r][c] = m(r, c);
    return result;
}

} // namespace

CameraShot::CameraShot() = default;

CameraShot CameraShot::fromVcgShot(const VcgShot &shot)
{
    CameraShot out;
    out.m_shot = shot;
    return out;
}

CameraShot CameraShot::defaultPerspectiveForImageSize(
    const QSize &imageSize,
    float focalMm,
    float sensorWidthMm)
{
    CameraShot out;
    if (imageSize.width() <= 0 || imageSize.height() <= 0)
        return out;

    const float pixelSize = sensorWidthMm / float(imageSize.width());
    out.m_shot.Intrinsics.cameraType = vcg::Camera<float>::PERSPECTIVE;
    out.m_shot.Intrinsics.ViewportPx = vcg::Point2i(imageSize.width(), imageSize.height());
    out.m_shot.Intrinsics.CenterPx =
        vcg::Point2f(float(imageSize.width()) * 0.5f, float(imageSize.height()) * 0.5f);
    out.m_shot.Intrinsics.DistorCenterPx = out.m_shot.Intrinsics.CenterPx;
    out.m_shot.Intrinsics.PixelSizeMm = vcg::Point2f(pixelSize, pixelSize);
    out.m_shot.Intrinsics.FocalMm = focalMm;
    return out;
}

CameraShot::VcgShot CameraShot::toVcgShot() const
{
    return m_shot;
}

bool CameraShot::isValid() const
{
    return m_shot.IsValid()
        && m_shot.Intrinsics.ViewportPx[0] > 0
        && m_shot.Intrinsics.ViewportPx[1] > 0
        && m_shot.Intrinsics.FocalMm > 0.0f;
}

CameraShot::CameraType CameraShot::cameraType() const
{
    return fromVcgCameraType(m_shot.Intrinsics.cameraType);
}

void CameraShot::setCameraType(CameraType type)
{
    m_shot.Intrinsics.cameraType = toVcgCameraType(type);
}

QSize CameraShot::viewportPx() const
{
    return QSize(m_shot.Intrinsics.ViewportPx[0], m_shot.Intrinsics.ViewportPx[1]);
}

void CameraShot::setViewportPx(const QSize &size)
{
    m_shot.Intrinsics.ViewportPx =
        vcg::Point2i(std::max(0, size.width()), std::max(0, size.height()));
}

QVector2D CameraShot::centerPx() const
{
    return QVector2D(m_shot.Intrinsics.CenterPx[0], m_shot.Intrinsics.CenterPx[1]);
}

void CameraShot::setCenterPx(const QVector2D &center)
{
    m_shot.Intrinsics.CenterPx = vcg::Point2f(center.x(), center.y());
}

QVector2D CameraShot::pixelSizeMm() const
{
    return QVector2D(m_shot.Intrinsics.PixelSizeMm[0], m_shot.Intrinsics.PixelSizeMm[1]);
}

void CameraShot::setPixelSizeMm(const QVector2D &pixelSize)
{
    m_shot.Intrinsics.PixelSizeMm = vcg::Point2f(pixelSize.x(), pixelSize.y());
}

QVector2D CameraShot::distortionCenterPx() const
{
    return QVector2D(m_shot.Intrinsics.DistorCenterPx[0], m_shot.Intrinsics.DistorCenterPx[1]);
}

void CameraShot::setDistortionCenterPx(const QVector2D &center)
{
    m_shot.Intrinsics.DistorCenterPx = vcg::Point2f(center.x(), center.y());
}

std::array<float, 4> CameraShot::distortionK() const
{
    return {
        m_shot.Intrinsics.k[0],
        m_shot.Intrinsics.k[1],
        m_shot.Intrinsics.k[2],
        m_shot.Intrinsics.k[3]
    };
}

void CameraShot::setDistortionK(const std::array<float, 4> &k)
{
    m_shot.Intrinsics.k = k;
}

float CameraShot::focalMm() const
{
    return m_shot.Intrinsics.FocalMm;
}

void CameraShot::setFocalMm(float focalMm)
{
    m_shot.Intrinsics.FocalMm = focalMm;
}

QVector3D CameraShot::viewPoint() const
{
    return toQVector(m_shot.GetViewPoint());
}

void CameraShot::setViewPoint(const QVector3D &point)
{
    m_shot.SetViewPoint(toVcgPoint(point));
}

QVector3D CameraShot::referenceAxis(int axisIndex) const
{
    if (axisIndex < 0 || axisIndex > 2)
        return QVector3D();
    return toQVector(m_shot.Axis(axisIndex));
}

QVector3D CameraShot::viewDirection() const
{
    return (-referenceAxis(2)).normalized();
}

QVector2D CameraShot::project(const QVector3D &worldPoint) const
{
    if (!isValid())
        return invalidPoint2();

    const vcg::Point2f projected = m_shot.Project(toVcgPoint(worldPoint));
    return QVector2D(projected[0], projected[1]);
}

QVector3D CameraShot::unproject(const QVector2D &pixelPoint, float depth) const
{
    if (!isValid() || !std::isfinite(depth) || depth <= 0.0f) {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        return QVector3D(nan, nan, nan);
    }

    const vcg::Point3f unprojected =
        m_shot.UnProject(vcg::Point2f(pixelPoint.x(), pixelPoint.y()), depth);
    return toQVector(unprojected);
}

float CameraShot::depth(const QVector3D &worldPoint) const
{
    if (!isValid())
        return std::numeric_limits<float>::quiet_NaN();
    return m_shot.Depth(toVcgPoint(worldPoint));
}

bool CameraShot::viewRayThrough(const QVector2D &pixelPoint, QVector3D &origin,
                                QVector3D &direction) const
{
    if (!isValid())
        return false;
    // One unit in front of the viewpoint is enough to give either projection its direction:
    // perspective rays pass through the viewpoint, orthographic ones are parallel to the axis.
    const QVector3D eye = viewPoint();
    const QVector3D onPlane = unproject(pixelPoint, 1.0f);
    if (!std::isfinite(onPlane.x()) || !std::isfinite(onPlane.y()) || !std::isfinite(onPlane.z()))
        return false;
    if (isOrthographic()) {
        direction = viewDirection();
        origin = onPlane - direction;
    } else {
        direction = (onPlane - eye).normalized();
        origin = eye;
    }
    return !direction.isNull();
}

QVector3D CameraShot::directionToCamera(const QVector3D &worldPoint) const
{
    if (isOrthographic())
        return -viewDirection();
    return (viewPoint() - worldPoint).normalized();
}

QMatrix4x4 CameraShot::viewMatrix() const
{
    if (!isValid())
        return {};

    return toQMatrix(m_shot.GetWorldToExtrinsicsMatrix());
}

QMatrix4x4 CameraShot::projectionMatrix(float nearPlane, float farPlane) const
{
    if (!isValid()
        || !std::isfinite(nearPlane)
        || !std::isfinite(farPlane)
        // A perspective volume starts in front of the eye; an orthographic one can start
        // behind it.
        || (nearPlane <= 0.0f && !isOrthographic())
        || farPlane <= nearPlane) {
        return {};
    }

    vcg::Camera<float> intrinsics = m_shot.Intrinsics;
    return toQMatrix(intrinsics.GetMatrix(nearPlane, farPlane));
}

void CameraShot::applyRigidTransformation(const QMatrix4x4 &m)
{
    m_shot.ApplyRigidTransformation(toVcgMatrix(m));
}

void CameraShot::rescalingWorld(float scale)
{
    m_shot.RescalingWorld(scale, true);
}

void CameraShot::applySimilarity(const QMatrix4x4 &m)
{
    m_shot.ApplySimilarity(toVcgMatrix(m));
}
