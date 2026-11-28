#include "plugins/io_e57/e57importplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"

#include <wrap/io_trimesh/io_mask.h>

#include <QFileInfo>
#include <QImage>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <E57SimpleReader.h>
#include <E57SimpleWriter.h>
#include <vcg/complex/allocate.h>
#include <vcg/math/matrix33.h>
#include <vcg/math/quaternion.h>

namespace {
constexpr int kErrSaveUnsupportedFormat = -1000;
constexpr int kErrSaveNoPoints = -1001;
constexpr int kErrSaveWriteFailed = -1002;
constexpr int kErrLoadLibrary = -5;

QString normalizedExtension(const QString &filename)
{
    return QFileInfo(filename).suffix().trimmed().toLower();
}

int saveMaskCapabilityForExtension(const QString &ext)
{
    if (ext != QLatin1String("e57"))
        return 0;
    return vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTCOLOR
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_VERTQUALITY;
}

int effectiveSaveMask(const VCGMesh &mesh, int requestedMask)
{
    const int capabilityMask = saveMaskCapabilityForExtension(QStringLiteral("e57"));
    int mask = (requestedMask != 0) ? requestedMask : capabilityMask;
    mask &= capabilityMask;
    if (mesh.VN() > 0)
        mask |= vcg::tri::io::Mask::IOM_VERTCOORD;
    return mask;
}

void reportProgress(vcg::CallBackPos *cb, int pos, const QString &msg, bool replaceLast = false)
{
    if (!cb)
        return;
    const QByteArray raw = replaceLast
        ? (msg + QStringLiteral("\r")).toLocal8Bit()
        : msg.toLocal8Bit();
    cb(pos, raw.constData());
}

// A pinhole shot at centre, its camera frame given as OpenCV has it (x right, y down, looking
// down +z) by the rows of worldToCamera. vcg's camera looks down -z with y up, and puts pixel
// centres at k + 0.5 with rows from the bottom; centerPx is already in that convention.
vcg::Shot<float> pinholeShot(const vcg::Matrix33d &worldToCamera, const vcg::Point3d &centre, double focalMm,
    const vcg::Point2d &pixelSizeMm, const vcg::Point2i &size, const vcg::Point2d &centerPx)
{
    vcg::Shot<float> shot;
    vcg::Matrix44f rot;
    rot.SetIdentity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            rot[r][c] = float(r == 0 ? worldToCamera[r][c] : -worldToCamera[r][c]);
    shot.Extrinsics.SetRot(rot);
    shot.Extrinsics.SetTra(vcg::Point3f::Construct(centre));
    vcg::Camera<float> &camera = shot.Intrinsics;
    camera.cameraType = vcg::Camera<float>::PERSPECTIVE;
    camera.FocalMm = float(focalMm);
    camera.PixelSizeMm = vcg::Point2f::Construct(pixelSizeMm);
    camera.ViewportPx = size;
    camera.CenterPx = vcg::Point2f::Construct(centerPx);
    return shot;
}

// A spherical (equirectangular) E57 image resampled into pinhole views of about 90 degrees,
// which MeshLab's rasters can hold: one row of views along a band, the six faces of a cube for
// a full sphere. Pixels the panorama does not cover are left transparent.
//
// The mapping was measured on two files whose panoramas are posed as the standard says,
// p_file = R p_image + t. On a full sphere whose points are colored from it (Station018.e57),
// it matches to 0.2 degrees: azimuth theta and elevation in the image frame give
// col = W/2 - (theta - 90 deg) / pixelWidth, row = H/2 - elevation / pixelHeight,
// so the centre column looks along the image's +y and columns turn clockwise. On 44.5 degree
// bands (manitou.e57) the image frame is tilted about its x axis by the band's own pitch, the
// photos are level: elevation is measured from the scanner's up, and the band's centre row is
// at the elevation of the image's +y.
std::vector<MeshIORaster> panoramaViews(const QImage &panorama, const QString &name, const vcg::Matrix33d &imageRotation,
    const vcg::Point3d &centre, const vcg::Point3d &up, double pixelWidth, double pixelHeight)
{
    const QImage source = panorama.convertToFormat(QImage::Format_ARGB32);
    const int W = source.width(), H = source.height();
    const vcg::Point3d forward0 = imageRotation.GetColumn(1);
    const double bandPitch = std::asin(std::clamp(forward0 * up, -1.0, 1.0));
    const vcg::Point3d heading = (forward0 - up * (forward0 * up)).Normalize();
    const vcg::Point3d side = heading ^ up;   // azimuth 0, with heading at 90 degrees
    const double turn = W * pixelWidth, band = H * pixelHeight;
    const bool fullTurn = turn > 2 * M_PI - 2 * pixelWidth;
    // The views' axes (forward, right). A sphere, or nearly, is the six faces of a cube, which
    // tile it exactly; a band is one row of views as wide as 90 degrees at most, each tall enough
    // for its corners to reach the band's edges, where a pinhole's elevation span is narrowest.
    std::vector<std::pair<vcg::Point3d, vcg::Point3d>> axes;
    auto level = [&](double azimuth, double pitch) {
        const vcg::Point3d forward = (side * std::cos(azimuth) + heading * std::sin(azimuth)) * std::cos(pitch)
            + up * std::sin(pitch);
        axes.emplace_back(forward, (forward ^ up).Normalize());
    };
    double hfov = M_PI / 2, vfov = M_PI / 2;
    if (band > 0.8 * M_PI) {
        for (int k = 0; k < 4; ++k)
            level(M_PI / 2 - k * M_PI / 2, 0);
        axes.emplace_back(up, side);
        axes.emplace_back(-up, side);
    } else {
        const int columns = std::max(1, int(std::ceil(turn / (M_PI / 2) - 1e-6)));
        hfov = turn / columns;
        vfov = std::min(2 * std::atan(1.1 * std::tan(band / 2 + std::abs(bandPitch) / 4) / std::cos(hfov / 2)), 0.8 * M_PI);
        for (int c = 0; c < columns; ++c)
            level(M_PI / 2 + turn / 2 - (c + 0.5) * hfov, bandPitch);
    }
    const double focalPx = 1.0 / std::min(pixelWidth, pixelHeight);
    const int w = int(std::lround(2 * focalPx * std::tan(hfov / 2)));
    const int h = int(std::lround(2 * focalPx * std::tan(vfov / 2)));

    // Bilinear sample at a continuous panorama coordinate (pixel k spans [k, k + 1)).
    auto sample = [&](double col, double row) -> QRgb {
        if (row < 0 || row >= H || (!fullTurn && (col < 0 || col >= W)))
            return qRgba(0, 0, 0, 0);
        const double x = col - 0.5, y = std::clamp(row - 0.5, 0.0, H - 1.0);
        const int x0 = int(std::floor(x)), y0 = int(y), y1 = std::min(y0 + 1, H - 1);
        const double fx = x - x0, fy = y - y0;
        auto at = [&](int cx, int cy) {
            cx = fullTurn ? ((cx % W) + W) % W : std::clamp(cx, 0, W - 1);
            return reinterpret_cast<const QRgb *>(source.constScanLine(cy))[cx];
        };
        const QRgb p00 = at(x0, y0), p10 = at(x0 + 1, y0), p01 = at(x0, y1), p11 = at(x0 + 1, y1);
        auto mix = [&](int (*channel)(QRgb)) {
            return int(std::lround((channel(p00) * (1 - fx) + channel(p10) * fx) * (1 - fy)
                + (channel(p01) * (1 - fx) + channel(p11) * fx) * fy));
        };
        return qRgba(mix(qRed), mix(qGreen), mix(qBlue), mix(qAlpha));
    };

    std::vector<MeshIORaster> views;
    for (const auto &[forward, right] : axes) {
        const vcg::Point3d down = forward ^ right;
        QImage image(w, h, QImage::Format_ARGB32);
        for (int j = 0; j < h; ++j) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(j));
            for (int i = 0; i < w; ++i) {
                const vcg::Point3d d = forward + right * ((i + 0.5 - w / 2.0) / focalPx)
                    + down * ((j + 0.5 - h / 2.0) / focalPx);
                const double theta = std::atan2(d * heading, d * side);
                const double elevation = std::atan2(d * up, (d - up * (d * up)).Norm());
                double col = W / 2.0 - std::remainder(theta - M_PI / 2, 2 * M_PI) / pixelWidth;
                if (fullTurn)
                    col = std::fmod(col + W, double(W));
                line[i] = sample(col, H / 2.0 - (elevation - bandPitch) / pixelHeight);
            }
        }
        MeshIORaster view;
        view.name = axes.size() > 1 ? QObject::tr("%1 (view %2)").arg(name).arg(views.size() + 1) : name;
        view.image = std::move(image);
        vcg::Matrix33d worldToCamera;
        worldToCamera.SetRow(0, right);
        worldToCamera.SetRow(1, down);
        worldToCamera.SetRow(2, forward);
        const double pixelSizeMm = 0.01;   // only its ratio to the focal length matters
        view.shot = pinholeShot(worldToCamera, centre, focalPx * pixelSizeMm, vcg::Point2d(pixelSizeMm, pixelSizeMm),
            vcg::Point2i(w, h), vcg::Point2d(w / 2.0, h / 2.0));
        views.push_back(std::move(view));
    }
    return views;
}

class E57ImportPlugin final : public MeshIOPlugin
{
public:
    QString pluginId() const override
    {
        return QStringLiteral("io_e57");
    }

    QString name() const override
    {
        return QObject::tr("E57 Point Cloud Import/Export");
    }

    QStringList supportedExtensions() const override
    {
        return { QStringLiteral("e57") };
    }

    bool canLoad(const QString &filename) const override
    {
        return normalizedExtension(filename) == QLatin1String("e57");
    }

    bool canSave(const QString &filename) const override
    {
        return normalizedExtension(filename) == QLatin1String("e57");
    }

    MeshIOCapabilities loadCapabilities(const QString &) const override
    {
        return { saveMaskCapabilityForExtension(QStringLiteral("e57")), false, false };
    }

    int load(
        const QString &filename,
        VCGMesh &mesh,
        vcg::CallBackPos *cb,
        int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet,
        MeshIOLoadContext &context) const override
    {
        if (outMaterialSet)
            outMaterialSet->clear();
        const int err = loadOrReport(filename, mesh, cb, outLoadMask, context.origin);
        // The E57 standard (ASTM E2807) has its Cartesian coordinates in meters.
        if (err == 0)
            context.unit = LengthUnit::Meter;
        return err;
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask) const override
    {
        std::optional<vcg::Point3d> origin;
        return loadOrReport(filename, mesh, cb, outLoadMask, origin);
    }

    // E57 coordinates are survey coordinates, often in the millions of meters.
    bool isGeoreferenced(const QString &) const override
    {
        return true;
    }

    static int loadOrReport(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask,
        std::optional<vcg::Point3d> &origin)
    {
        if (outLoadMask)
            *outLoadMask = 0;
        try {
            return loadScans(filename, mesh, cb, outLoadMask, origin);
        } catch (const e57::E57Exception &e) {
            reportProgress(cb, 100, QObject::tr("E57 import failed: %1 (%2)")
                .arg(QString::fromStdString(e.errorStr()), QString::fromStdString(e.context())), true);
        } catch (const std::exception &e) {
            reportProgress(cb, 100, QObject::tr("E57 import failed: %1").arg(QString::fromLocal8Bit(e.what())), true);
        }
        mesh.Clear();
        return kErrLoadLibrary;
    }

    // Every scan of the file into one point cloud, placed by its pose (the standard's
    // rigid-body transform from the scan's own frame to the file's). Each scan contributes
    // the fields it has: Cartesian or spherical coordinates, color, intensity (into vertex
    // quality, unscaled, as the writer stores it) and surface normals (E57_EXT_surface_normals).
    // A field only some scans have is defaulted on the others: white, zero, no normal.
    //
    // Positions are made relative to origin in double precision, before they become float
    // (MeshIOLoadContext::origin). Given none, the points are first read relative to the first
    // one, rounded, which keeps them precise whatever the file's coordinates; if the cloud then
    // proves far from zero, the origin is the corner of its bounding box rounded down to whole
    // meters, and none otherwise.
    static int loadScans(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask,
        std::optional<vcg::Point3d> &origin)
    {
        e57::Reader reader(filename.toStdString(), {});
        if (!reader.IsOpen())
            return -1;

        const int64_t scanCount = reader.GetData3DCount();
        if (scanCount <= 0)
            return -2;

        mesh.Clear();
        int loadMask = vcg::tri::io::Mask::IOM_VERTCOORD;
        const bool originGiven = origin.has_value();
        vcg::Point3d shift = origin.value_or(vcg::Point3d(0, 0, 0));
        bool shiftChosen = originGiven;
        vcg::Box3d bounds;   // in the file's coordinates

        for (int64_t scanIndex = 0; scanIndex < scanCount; ++scanIndex) {
            e57::Data3D header;
            if (!reader.ReadData3D(scanIndex, header))
                return -3;
            const e57::PointStandardizedFieldsAvailable &fields = header.pointFields;
            const bool cartesian = fields.cartesianXField && fields.cartesianYField && fields.cartesianZField;
            const bool spherical = !cartesian && fields.sphericalRangeField
                && fields.sphericalAzimuthField && fields.sphericalElevationField;
            if ((!cartesian && !spherical) || header.pointCount <= 0)
                continue;
            const bool color = fields.colorRedField && fields.colorGreenField && fields.colorBlueField;
            const bool normal = fields.normalXField && fields.normalYField && fields.normalZField;
            const bool intensity = fields.intensityField;
            if (color)
                loadMask |= vcg::tri::io::Mask::IOM_VERTCOLOR;
            if (normal)
                loadMask |= vcg::tri::io::Mask::IOM_VERTNORMAL;
            if (intensity)
                loadMask |= vcg::tri::io::Mask::IOM_VERTQUALITY;

            // Read in chunks: a whole scan in one buffer doubled the memory of a large file.
            const size_t chunk = std::min<size_t>(size_t(header.pointCount), size_t(1) << 20);
            auto buffer = [chunk](auto &v, bool active) {
                v.resize(active ? chunk : 0);
                return active ? v.data() : nullptr;
            };
            std::vector<double> a, b, c, inten;
            std::vector<float> nx, ny, nz;
            std::vector<uint16_t> red, green, blue;
            std::vector<int8_t> invalid, colorInvalid, intensityInvalid;
            e57::Data3DPointsDouble data;
            (cartesian ? data.cartesianX : data.sphericalRange) = buffer(a, true);
            (cartesian ? data.cartesianY : data.sphericalAzimuth) = buffer(b, true);
            (cartesian ? data.cartesianZ : data.sphericalElevation) = buffer(c, true);
            (cartesian ? data.cartesianInvalidState : data.sphericalInvalidState) = buffer(invalid,
                cartesian ? fields.cartesianInvalidStateField : fields.sphericalInvalidStateField);
            data.colorRed = buffer(red, color);
            data.colorGreen = buffer(green, color);
            data.colorBlue = buffer(blue, color);
            data.isColorInvalid = buffer(colorInvalid, color && fields.isColorInvalidField);
            data.intensity = buffer(inten, intensity);
            data.isIntensityInvalid = buffer(intensityInvalid, intensity && fields.isIntensityInvalidField);
            data.normalX = buffer(nx, normal);
            data.normalY = buffer(ny, normal);
            data.normalZ = buffer(nz, normal);

            // Colors are given within the scan's color limits; without them, assume 8 bits.
            const e57::ColorLimits &limits = header.colorLimits;
            auto toByte = [](uint16_t value, double lo, double hi) {
                if (hi <= lo) { lo = 0; hi = 255; }
                return (unsigned char)(std::clamp((double(value) - lo) * 255.0 / (hi - lo), 0.0, 255.0) + 0.5);
            };
            const e57::Quaternion &q = header.pose.rotation;
            const vcg::Quaterniond rotation(q.w, q.x, q.y, q.z);
            const vcg::Point3d translation(header.pose.translation.x, header.pose.translation.y,
                header.pose.translation.z);

            e57::CompressedVectorReader dataReader = reader.SetUpData3DPointsData(scanIndex, chunk, data);
            size_t readCount = 0;
            while ((readCount = dataReader.read()) > 0) {
                // A point with any invalid coordinate (direction only, or none) has no position.
                size_t valid = readCount;
                if (!invalid.empty())
                    valid = size_t(std::count(invalid.begin(), invalid.begin() + readCount, int8_t(0)));
                auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, valid);
                for (size_t i = 0; i < readCount; ++i) {
                    if (!invalid.empty() && invalid[i] != 0)
                        continue;
                    vcg::Point3d p(a[i], b[i], c[i]);
                    if (spherical) {
                        const double r = a[i], az = b[i], el = c[i];
                        p = vcg::Point3d(r * std::cos(el) * std::cos(az), r * std::cos(el) * std::sin(az),
                            r * std::sin(el));
                    }
                    const vcg::Point3d position = rotation.Rotate(p) + translation;
                    if (!shiftChosen) {
                        shift = vcg::Point3d(std::round(position[0]), std::round(position[1]), std::round(position[2]));
                        shiftChosen = true;
                    }
                    bounds.Add(position);
                    vi->P() = vcg::Point3f::Construct(position - shift);
                    vi->C() = vcg::Color4b::White;
                    if (color && (colorInvalid.empty() || colorInvalid[i] == 0))
                        vi->C() = vcg::Color4b(toByte(red[i], limits.colorRedMinimum, limits.colorRedMaximum),
                            toByte(green[i], limits.colorGreenMinimum, limits.colorGreenMaximum),
                            toByte(blue[i], limits.colorBlueMinimum, limits.colorBlueMaximum), 255);
                    vi->Q() = intensity && (intensityInvalid.empty() || intensityInvalid[i] == 0)
                        ? float(inten[i]) : 0.0f;
                    vi->N() = normal ? vcg::Point3f::Construct(rotation.Rotate(vcg::Point3d(nx[i], ny[i], nz[i])))
                                     : vcg::Point3f(0, 0, 0);
                    ++vi;
                }
            }
            dataReader.close();
            reportProgress(cb, int((scanIndex + 1) * 100 / scanCount), QObject::tr("Loading E57 points"), true);
        }

        reader.Close();
        if (!originGiven) {
            if (isFarFromZero(bounds))
                origin = vcg::Point3d(std::floor(bounds.min[0]), std::floor(bounds.min[1]), std::floor(bounds.min[2]));
            // From the provisional shift to the origin, or back to the file's own coordinates.
            const vcg::Point3d move = shift - origin.value_or(vcg::Point3d(0, 0, 0));
            if (move != vcg::Point3d(0, 0, 0))
                for (auto &v : mesh.vert)
                    v.P() = vcg::Point3f::Construct(vcg::Point3d::Construct(v.P()) + move);
        }
        if (outLoadMask)
            *outLoadMask = loadMask;
        return 0;
    }

    // The photos, as rasters in the coordinates load() places the points in.
    //
    // Pinhole images:
    //
    // The image pose is read as measured on a Leica ScanStation 2 file (parking-lot-updated.e57,
    // whose image names give each photo's azimuth and elevation, matched by all 285 of them to
    // 0.55 degrees): it takes a point from the associated scan's own frame into the camera's,
    // p_cam = R(q) (p_scan - t), the camera looking down +z with image x right and y down, rows
    // from the top. libE57's notes describe the inverse, an image-to-file pose with z < 0 in
    // front; the file's scan poses do follow the standard, its image poses this. A pixel
    // centre is at an integer coordinate (511.5 is the centre of a 1024-pixel image).
    // Spherical images become pinhole views (panoramaViews); cylindrical ones are skipped.
    std::vector<MeshIORaster> loadRasters(
        const QString &filename, const MeshIOLoadContext &context, QStringList &warnings) const override
    {
        const vcg::Point3d origin = context.origin.value_or(vcg::Point3d(0, 0, 0));
        std::vector<MeshIORaster> rasters;
        try {
            e57::Reader reader(filename.toStdString(), {});
            if (!reader.IsOpen() || reader.GetImage2DCount() <= 0)
                return rasters;
            auto rotation = [](const e57::Quaternion &q) {
                const vcg::Quaterniond r(q.w, q.x, q.y, q.z);
                vcg::Matrix33d m;
                for (int c = 0; c < 3; ++c) {
                    vcg::Point3d axis(0, 0, 0);
                    axis[c] = 1;
                    m.SetColumn(c, r.Rotate(axis));
                }
                return m;
            };
            auto translation = [](const e57::Translation &t) { return vcg::Point3d(t.x, t.y, t.z); };
            std::map<std::string, e57::RigidBodyTransform> scanPose;
            for (int64_t i = 0; i < reader.GetData3DCount(); ++i) {
                e57::Data3D scan;
                if (reader.ReadData3D(i, scan))
                    scanPose[scan.guid] = scan.pose;
            }
            int skipped = 0;
            for (int64_t i = 0; i < reader.GetImage2DCount(); ++i) {
                e57::Image2D header;
                e57::Image2DProjection projection;
                e57::Image2DType type, maskType, visualType;
                int64_t width = 0, height = 0, size = 0;
                if (!reader.ReadImage2D(i, header)
                    || !reader.GetImage2DSizes(i, projection, type, width, height, size, maskType, visualType))
                    continue;
                if (projection != e57::ProjectionPinhole && projection != e57::ProjectionSpherical) {
                    ++skipped;
                    continue;
                }
                QByteArray bytes(qsizetype(size), Qt::Uninitialized);
                reader.ReadImage2DData(i, projection, type, bytes.data(), 0, size);
                QString name = QString::fromStdString(header.name);
                if (name.isEmpty())
                    name = QObject::tr("Image %1").arg(i);
                QImage image;
                if (!image.loadFromData(bytes)) {
                    warnings << QObject::tr("E57 image '%1' could not be decoded.").arg(name);
                    continue;
                }

                const auto scan = scanPose.find(header.associatedData3DGuid);
                const e57::RigidBodyTransform pose = scan != scanPose.end() ? scan->second : e57::RigidBodyTransform{};
                const vcg::Matrix33d scanRotation = rotation(pose.rotation);
                if (projection == e57::ProjectionSpherical) {
                    const e57::SphericalRepresentation &sphere = header.sphericalRepresentation;
                    std::vector<MeshIORaster> views = panoramaViews(image, name, rotation(header.pose.rotation),
                        translation(header.pose.translation) - origin, scanRotation.GetColumn(2),
                        sphere.pixelWidth, sphere.pixelHeight);
                    std::move(views.begin(), views.end(), std::back_inserter(rasters));
                    continue;
                }
                const e57::PinholeRepresentation &pinhole = header.pinholeRepresentation;
                MeshIORaster raster;
                raster.name = name;
                raster.image = std::move(image);
                raster.shot = pinholeShot(rotation(header.pose.rotation) * scanRotation.transpose(),
                    translation(pose.translation) + scanRotation * translation(header.pose.translation) - origin,
                    pinhole.focalLength * 1000, vcg::Point2d(pinhole.pixelWidth * 1000, pinhole.pixelHeight * 1000),
                    vcg::Point2i(int(width), int(height)),
                    vcg::Point2d(pinhole.principalPointX + 0.5, height - pinhole.principalPointY - 0.5));
                rasters.push_back(std::move(raster));
            }
            if (skipped > 0)
                warnings << QObject::tr("%n cylindrical E57 image(s) skipped: their mapping is not supported yet.",
                    nullptr, skipped);
        } catch (const e57::E57Exception &e) {
            warnings << QObject::tr("E57 images could not be read: %1 (%2)")
                .arg(QString::fromStdString(e.errorStr()), QString::fromStdString(e.context()));
        } catch (const std::exception &e) {
            warnings << QObject::tr("E57 images could not be read: %1").arg(QString::fromLocal8Bit(e.what()));
        }
        return rasters;
    }

    QString filterString() const override
    {
        return QObject::tr("E57 Files (*.e57)");
    }

    int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb) const override
    {
        if (!canSave(filename))
            return kErrSaveUnsupportedFormat;

        const int saveMask = effectiveSaveMask(mesh, options.mask);
        if ((saveMask & vcg::tri::io::Mask::IOM_VERTCOORD) == 0)
            return kErrSaveNoPoints;

        const bool exportColors = (saveMask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0;
        const bool exportNormals = (saveMask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0;
        const bool exportIntensity = (saveMask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0;

        size_t pointCount = 0;
        for (const auto &v : mesh.vert) {
            if (v.IsD())
                continue;
            const auto p = v.cP();
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                continue;
            ++pointCount;
        }
        if (pointCount == 0)
            return kErrSaveNoPoints;

        std::vector<double> cartesianX(pointCount);
        std::vector<double> cartesianY(pointCount);
        std::vector<double> cartesianZ(pointCount);
        std::vector<uint16_t> colorR(exportColors ? pointCount : 0);
        std::vector<uint16_t> colorG(exportColors ? pointCount : 0);
        std::vector<uint16_t> colorB(exportColors ? pointCount : 0);
        std::vector<float> normalX(exportNormals ? pointCount : 0);
        std::vector<float> normalY(exportNormals ? pointCount : 0);
        std::vector<float> normalZ(exportNormals ? pointCount : 0);
        std::vector<double> intensity(exportIntensity ? pointCount : 0);

        double xMin = std::numeric_limits<double>::infinity();
        double yMin = std::numeric_limits<double>::infinity();
        double zMin = std::numeric_limits<double>::infinity();
        double xMax = -std::numeric_limits<double>::infinity();
        double yMax = -std::numeric_limits<double>::infinity();
        double zMax = -std::numeric_limits<double>::infinity();
        double iMin = std::numeric_limits<double>::infinity();
        double iMax = -std::numeric_limits<double>::infinity();

        reportProgress(cb, 0, QObject::tr("Preparing E57 export..."), true);

        size_t outIndex = 0;
        size_t processed = 0;
        const size_t totalVertices = mesh.vert.size();
        for (const auto &v : mesh.vert) {
            ++processed;
            if (v.IsD())
                continue;
            const auto p = v.cP();
            if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
                continue;

            // True coordinates: the document origin added back in double.
            const double x = double(p[0]) + options.origin[0];
            const double y = double(p[1]) + options.origin[1];
            const double z = double(p[2]) + options.origin[2];
            cartesianX[outIndex] = x;
            cartesianY[outIndex] = y;
            cartesianZ[outIndex] = z;
            xMin = std::min(xMin, x);
            yMin = std::min(yMin, y);
            zMin = std::min(zMin, z);
            xMax = std::max(xMax, x);
            yMax = std::max(yMax, y);
            zMax = std::max(zMax, z);

            if (exportColors) {
                const auto c = v.cC();
                colorR[outIndex] = uint16_t(c[0]) * uint16_t(257);
                colorG[outIndex] = uint16_t(c[1]) * uint16_t(257);
                colorB[outIndex] = uint16_t(c[2]) * uint16_t(257);
            }

            if (exportNormals) {
                const auto n = v.cN();
                normalX[outIndex] = std::isfinite(n[0]) ? n[0] : 0.0f;
                normalY[outIndex] = std::isfinite(n[1]) ? n[1] : 0.0f;
                normalZ[outIndex] = std::isfinite(n[2]) ? n[2] : 1.0f;
            }

            if (exportIntensity) {
                const double q = double(v.cQ());
                const double iq = std::isfinite(q) ? q : 0.0;
                intensity[outIndex] = iq;
                iMin = std::min(iMin, iq);
                iMax = std::max(iMax, iq);
            }

            ++outIndex;
            if (cb && (processed % 50000 == 0 || processed == totalVertices)) {
                const int pos = std::clamp(
                    int((processed * 90) / std::max<size_t>(size_t(1), totalVertices)),
                    0,
                    90);
                reportProgress(cb, pos, QObject::tr("Preparing E57 points..."), true);
            }
        }

        if (outIndex == 0)
            return kErrSaveNoPoints;

        try {
            e57::WriterOptions writerOptions {};
            e57::Writer writer(filename.toStdString(), writerOptions);
            if (!writer.IsOpen())
                return kErrSaveWriteFailed;

            e57::Data3D data3D {};
            data3D.name = QFileInfo(filename).completeBaseName().toStdString();
            data3D.pointCount = outIndex;
            data3D.pointFields.cartesianXField = true;
            data3D.pointFields.cartesianYField = true;
            data3D.pointFields.cartesianZField = true;
            data3D.pointFields.pointRangeNodeType = e57::NumericalNodeType::Double;
            data3D.cartesianBounds.xMinimum = xMin;
            data3D.cartesianBounds.xMaximum = xMax;
            data3D.cartesianBounds.yMinimum = yMin;
            data3D.cartesianBounds.yMaximum = yMax;
            data3D.cartesianBounds.zMinimum = zMin;
            data3D.cartesianBounds.zMaximum = zMax;

            if (exportColors) {
                data3D.pointFields.colorRedField = true;
                data3D.pointFields.colorGreenField = true;
                data3D.pointFields.colorBlueField = true;
                // The limits are the range a color can take, as the E57 standard defines them, not
                // the data's: 8-bit colors scaled to 16 bits. The data's own range, used before,
                // collapsed to a single value on a channel that never changes (an all-white
                // mesh), and libE57Format then refused to write it (ErrorValueOutOfBounds).
                data3D.colorLimits.colorRedMinimum = 0;
                data3D.colorLimits.colorRedMaximum = 65535;
                data3D.colorLimits.colorGreenMinimum = 0;
                data3D.colorLimits.colorGreenMaximum = 65535;
                data3D.colorLimits.colorBlueMinimum = 0;
                data3D.colorLimits.colorBlueMaximum = 65535;
            }

            if (exportNormals) {
                data3D.pointFields.normalXField = true;
                data3D.pointFields.normalYField = true;
                data3D.pointFields.normalZField = true;
            }

            if (exportIntensity) {
                data3D.pointFields.intensityField = true;
                data3D.pointFields.intensityNodeType = e57::NumericalNodeType::Double;
                if (std::isfinite(iMin) && std::isfinite(iMax)) {
                    data3D.intensityLimits.intensityMinimum = iMin;
                    data3D.intensityLimits.intensityMaximum = iMax;
                }
            }

            e57::Data3DPointsDouble points {};
            points.cartesianX = cartesianX.data();
            points.cartesianY = cartesianY.data();
            points.cartesianZ = cartesianZ.data();
            if (exportColors) {
                points.colorRed = colorR.data();
                points.colorGreen = colorG.data();
                points.colorBlue = colorB.data();
            }
            if (exportNormals) {
                points.normalX = normalX.data();
                points.normalY = normalY.data();
                points.normalZ = normalZ.data();
            }
            if (exportIntensity)
                points.intensity = intensity.data();

            reportProgress(cb, 95, QObject::tr("Writing E57 file..."), true);
            writer.WriteData3DData(data3D, points);
            writer.Close();
            reportProgress(cb, 100, QObject::tr("Saved E57: %1").arg(QFileInfo(filename).fileName()), true);
            return 0;
        } catch (const e57::E57Exception &e) {
            // libE57Format says what it refused in the exception; keep it for the log.
            const QString why = QStringLiteral("%1 (%2)").arg(QString::fromStdString(e.errorStr()), QString::fromStdString(e.context()));
            reportProgress(cb, 100, QObject::tr("E57 export failed: %1").arg(why), true);
            return kErrSaveWriteFailed;
        } catch (const std::exception &e) {
            // libE57Format reports what it refused in the exception; keep it for the log.
            reportProgress(cb, 100, QObject::tr("E57 export failed: %1").arg(QString::fromLocal8Bit(e.what())), true);
            return kErrSaveWriteFailed;
        } catch (...) {
            return kErrSaveWriteFailed;
        }
    }

    QString saveFilterString() const override
    {
        return QObject::tr("E57 (*.e57)");
    }

    int saveMaskCapability(const QString &filename) const override
    {
        return saveMaskCapabilityForExtension(normalizedExtension(filename));
    }

    QString errorString(int errCode) const override
    {
        switch (errCode) {
        case -1: return QObject::tr("Cannot open E57 file.");
        case -2: return QObject::tr("No point clouds found in E57 file.");
        case -3: return QObject::tr("Failed to read E57 scan metadata.");
        case kErrLoadLibrary: return QObject::tr("The E57 library could not read the file (see the log).");
        case kErrSaveUnsupportedFormat: return QObject::tr("Unsupported E57 export format.");
        case kErrSaveNoPoints: return QObject::tr("Cannot save E57: mesh has no valid points.");
        case kErrSaveWriteFailed: return QObject::tr("Failed to write E57 file.");
        default: return QObject::tr("Unknown E57 I/O error.");
        }
    }
};
}

void registerE57ImportPlugin(MeshIOPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<E57ImportPlugin>());
}
