#include "camerashot.h"

#include <QObject>
#include <QXmlStreamAttributes>
#include <QXmlStreamWriter>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

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

// The first `count` numbers of a space-separated list; false when there are fewer or one is
// not a number. Lists may be longer: TranslationVector carries a trailing 1.
template <class T>
bool parseNumbers(QStringView text, int count, std::vector<T> &values)
{
    const QStringList parts = text.toString().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < count)
        return false;
    values.resize(size_t(count));
    for (int i = 0; i < count; ++i) {
        bool ok = false;
        values[size_t(i)] = std::is_integral_v<T> ? T(parts.at(i).toInt(&ok)) : T(parts.at(i).toDouble(&ok));
        if (!ok)
            return false;
    }
    return true;
}

// The value of a 4-channel pixel at (x, y), in pixel-index coordinates, interpolated
// bilinearly; transparent (all zero) outside the image, with the border pixels reaching half a
// pixel beyond their centres.
template <class T>
void sampleBilinear(const QImage &src, double x, double y, T *out)
{
    const int w = src.width();
    const int h = src.height();
    if (!(x >= -0.5 && y >= -0.5 && x <= w - 0.5 && y <= h - 0.5)) {
        std::fill(out, out + 4, T(0));
        return;
    }
    x = std::clamp(x, 0.0, double(w - 1));
    y = std::clamp(y, 0.0, double(h - 1));
    const int x0 = std::min(int(x), w - 2 < 0 ? 0 : w - 2);
    const int y0 = std::min(int(y), h - 2 < 0 ? 0 : h - 2);
    const int x1 = std::min(x0 + 1, w - 1);
    const int y1 = std::min(y0 + 1, h - 1);
    const double fx = x - x0;
    const double fy = y - y0;
    const T *r0 = reinterpret_cast<const T *>(src.constScanLine(y0));
    const T *r1 = reinterpret_cast<const T *>(src.constScanLine(y1));
    for (int c = 0; c < 4; ++c) {
        const double top = r0[4 * x0 + c] * (1 - fx) + r0[4 * x1 + c] * fx;
        const double bottom = r1[4 * x0 + c] * (1 - fx) + r1[4 * x1 + c] * fx;
        const double v = top * (1 - fy) + bottom * fy;
        out[c] = std::is_integral_v<T> ? T(std::lround(v)) : T(v);
    }
}

template <class T>
void undistortInto(const QImage &src, QImage &dst, const CameraLens &lens,
                   double fx, double fy, double cx, double cy)
{
    for (int row = 0; row < dst.height(); ++row) {
        T *line = reinterpret_cast<T *>(dst.scanLine(row));
        for (int col = 0; col < dst.width(); ++col) {
            // Pixel centres at k + 0.5, as COLMAP and OpenCV's calibration have them
            const vcg::Point2d n((col + 0.5 - cx) / fx, (row + 0.5 - cy) / fy);
            if (!lens.IsInvertibleAt(n)) {
                std::fill(line + 4 * col, line + 4 * col + 4, T(0));
                continue;
            }
            const vcg::Point2d d = lens.Distort(n);
            sampleBilinear(src, d[0] * fx + cx - 0.5, d[1] * fy + cy - 0.5, line + 4 * col);
        }
    }
}

// LensDistortion is vcglib's old one-parameter radial model, which vcg::Camera no longer has
// and no project is known to have used: it is read, so that the file still validates, and a
// non-zero value is dropped with a warning.
void warnIgnoredLensDistortion(float k1, float k2)
{
    if (k1 != 0.0f || k2 != 0.0f)
        qWarning("Lens distortion %g %g is not supported; reading the camera without it.", k1, k2);
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

void CameraShot::rescaleViewportPx(const QSize &size)
{
    m_shot.Intrinsics.RescaleViewportPx(vcg::Point2i(size.width(), size.height()));
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

// Unused; see the header.
// void CameraShot::rescalingWorld(float scale)
// {
//     m_shot.RescalingWorld(scale, true);
// }

void CameraShot::applySimilarity(const QMatrix4x4 &m)
{
    m_shot.ApplySimilarity(toVcgMatrix(m));
}

QImage CameraShot::undistortImage(const QImage &photo, const CameraLens &lens) const
{
    if (lens.IsIdentity() || photo.isNull() || !isValid() || isOrthographic())
        return photo;

    // 8-bit images stay 8-bit; deeper ones (16-bit, float) are resampled in float
    const bool wide = photo.depth() > 32;
    const QImage src = photo.convertToFormat(wide ? QImage::Format_RGBA32FPx4 : QImage::Format_RGBA8888);
    QImage dst(src.size(), src.format());

    // The intrinsics in pixels of this image, y down: vcg counts CenterPx from the bottom row
    const vcg::Camera<float> &cam = m_shot.Intrinsics;
    const double sx = double(src.width()) / cam.ViewportPx[0];
    const double sy = double(src.height()) / cam.ViewportPx[1];
    const double fx = cam.FocalMm / cam.PixelSizeMm[0] * sx;
    const double fy = cam.FocalMm / cam.PixelSizeMm[1] * sy;
    const double cx = cam.CenterPx[0] * sx;
    const double cy = (cam.ViewportPx[1] - cam.CenterPx[1]) * sy;
    if (wide)
        undistortInto<float>(src, dst, lens, fx, fy, cx, cy);
    else
        undistortInto<uchar>(src, dst, lens, fx, fy, cx, cy);
    return dst;
}

bool CameraShot::readVcgCameraXml(
    const QXmlStreamAttributes &attrs, CameraShot &outShot, CameraLens &outLens, QString *errorMessage)
{
    const auto fail = [&](const QString &message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    VcgShot shot;
    vcg::Camera<float> &cam = shot.Intrinsics;

    if (attrs.hasAttribute(QStringLiteral("CameraType"))) {
        bool ok = false;
        const int type = attrs.value(QStringLiteral("CameraType")).toInt(&ok);
        if (!ok)
            return fail(QObject::tr("Invalid CameraType attribute"));
        // Only perspective and orthographic are drawn; the other vcg types (isometric,
        // cavalieri) are read as perspective rather than refused, so the project still opens.
        cam.cameraType = type == vcg::Camera<float>::ORTHO ? vcg::Camera<float>::ORTHO
                                                            : vcg::Camera<float>::PERSPECTIVE;
        if (type != vcg::Camera<float>::PERSPECTIVE && type != vcg::Camera<float>::ORTHO)
            qWarning("Camera type %d is not supported; reading it as perspective.", type);
    }

    if (attrs.value(QStringLiteral("BinaryData")).trimmed() == QStringLiteral("1")) {
        const auto bytes = [&](const char *name) {
            return QByteArray::fromBase64(attrs.value(QLatin1String(name)).toLocal8Bit());
        };
        const QByteArray tra = bytes("TranslationVector");
        const QByteArray rot = bytes("RotationMatrix");
        const QByteArray focal = bytes("FocalMm");
        const QByteArray viewport = bytes("ViewportPx");
        const QByteArray center = bytes("CenterPx");
        const QByteArray pixel = bytes("PixelSizeMm");
        const QByteArray lens = bytes("LensDistortion");
        if (tra.size() < int(sizeof(float) * 3) || rot.size() < int(sizeof(float) * 16)
            || focal.size() < int(sizeof(float)) || viewport.size() < int(sizeof(int) * 2)
            || center.size() < int(sizeof(float) * 2) || pixel.size() < int(sizeof(float) * 2)
            || lens.size() < int(sizeof(float) * 2))
            return fail(QObject::tr("Incomplete binary VCGCamera payload"));

        vcg::Point3f t;
        memcpy(t.V(), tra.constData(), sizeof(float) * 3);
        shot.Extrinsics.SetTra(-t);
        vcg::Matrix44f r;
        memcpy(r.V(), rot.constData(), sizeof(float) * 16);
        shot.Extrinsics.SetRot(r);
        memcpy(&cam.FocalMm, focal.constData(), sizeof(float));
        memcpy(&cam.ViewportPx, viewport.constData(), sizeof(int) * 2);
        memcpy(&cam.CenterPx, center.constData(), sizeof(float) * 2);
        memcpy(&cam.PixelSizeMm, pixel.constData(), sizeof(float) * 2);
        float k[2];
        memcpy(k, lens.constData(), sizeof(float) * 2);
        warnIgnoredLensDistortion(k[0], k[1]);
    } else {
        std::vector<float> tra, rot, center, pixel, k;
        std::vector<int> viewport;
        if (!parseNumbers(attrs.value(QStringLiteral("TranslationVector")), 3, tra)
            || !parseNumbers(attrs.value(QStringLiteral("RotationMatrix")), 16, rot))
            return fail(QObject::tr("Invalid camera extrinsics"));
        shot.Extrinsics.SetTra(vcg::Point3f(-tra[0], -tra[1], -tra[2]));
        vcg::Matrix44f r;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                r[row][col] = rot[size_t(col + 4 * row)];
        shot.Extrinsics.SetRot(r);

        bool ok = false;
        cam.FocalMm = attrs.value(QStringLiteral("FocalMm")).toFloat(&ok);
        if (!ok)
            return fail(QObject::tr("Invalid FocalMm attribute"));
        if (!parseNumbers(attrs.value(QStringLiteral("ViewportPx")), 2, viewport)
            || !parseNumbers(attrs.value(QStringLiteral("CenterPx")), 2, center)
            || !parseNumbers(attrs.value(QStringLiteral("PixelSizeMm")), 2, pixel)
            || !parseNumbers(attrs.value(QStringLiteral("LensDistortion")), 2, k))
            return fail(QObject::tr("Invalid camera intrinsics"));
        cam.ViewportPx = vcg::Point2i(viewport[0], viewport[1]);
        cam.CenterPx = vcg::Point2f(center[0], center[1]);
        cam.PixelSizeMm = vcg::Point2f(pixel[0], pixel[1]);
        warnIgnoredLensDistortion(k[0], k[1]);
    }

    // The lens, always as text: k1 k2 p1 p2 k3 k4 k5 k6, OpenCV's order
    CameraLens lens;
    const QStringView model = attrs.value(QStringLiteral("LensModel"));
    if (!model.isEmpty()) {
        std::vector<double> p;
        if (model != QStringLiteral("brown"))
            return fail(QObject::tr("Unsupported lens model '%1'").arg(model.toString()));
        if (!parseNumbers(attrs.value(QStringLiteral("LensParams")), 8, p))
            return fail(QObject::tr("Invalid LensParams attribute: 8 numbers expected"));
        lens.k1 = p[0]; lens.k2 = p[1]; lens.p1 = p[2]; lens.p2 = p[3];
        lens.k3 = p[4]; lens.k4 = p[5]; lens.k5 = p[6]; lens.k6 = p[7];
    }

    outShot = fromVcgShot(shot);
    if (!outShot.isValid())
        return fail(QObject::tr("Parsed VCGCamera is not valid"));
    outLens = lens;
    return true;
}

void CameraShot::writeVcgCameraXml(QXmlStreamWriter &xml, const CameraLens &lens) const
{
    xml.writeStartElement(QStringLiteral("VCGCamera"));
    if (isValid()) {
        const vcg::Camera<float> &cam = m_shot.Intrinsics;
        // Translation negated, as the reader has it
        const vcg::Point3f tra = m_shot.Extrinsics.Tra();
        xml.writeAttribute(QStringLiteral("TranslationVector"),
            QStringLiteral("%1 %2 %3 1").arg(-tra[0], 0, 'f', 6).arg(-tra[1], 0, 'f', 6).arg(-tra[2], 0, 'f', 6));
        const vcg::Matrix44f rot = m_shot.Extrinsics.Rot();
        QStringList rotText;
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col)
                rotText << QString::number(rot[row][col], 'f', 6);
        xml.writeAttribute(QStringLiteral("RotationMatrix"), rotText.join(QLatin1Char(' ')));
        xml.writeAttribute(QStringLiteral("CameraType"), QString::number(int(cam.cameraType)));
        xml.writeAttribute(QStringLiteral("FocalMm"), QString::number(cam.FocalMm, 'f', 4));
        // vcg::Camera has no LensDistortion any more; old MeshLab requires the attribute
        xml.writeAttribute(QStringLiteral("LensDistortion"), QStringLiteral("0 0"));
        if (!lens.IsIdentity()) {
            xml.writeAttribute(QStringLiteral("LensModel"), QStringLiteral("brown"));
            QStringList params;
            for (double v : {lens.k1, lens.k2, lens.p1, lens.p2, lens.k3, lens.k4, lens.k5, lens.k6})
                params << QString::number(v, 'g', 17);
            xml.writeAttribute(QStringLiteral("LensParams"), params.join(QLatin1Char(' ')));
        }
        xml.writeAttribute(QStringLiteral("PixelSizeMm"),
            QStringLiteral("%1 %2").arg(cam.PixelSizeMm[0], 0, 'f', 6).arg(cam.PixelSizeMm[1], 0, 'f', 6));
        xml.writeAttribute(QStringLiteral("ViewportPx"),
            QStringLiteral("%1 %2").arg(cam.ViewportPx[0]).arg(cam.ViewportPx[1]));
        xml.writeAttribute(QStringLiteral("CenterPx"),
            QStringLiteral("%1 %2").arg(cam.CenterPx[0], 0, 'f', 2).arg(cam.CenterPx[1], 0, 'f', 2));
    }
    xml.writeAttribute(QStringLiteral("BinaryData"), QStringLiteral("0"));
    xml.writeEndElement();
}
