#include "plugins/io_e57/e57importplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"

#include <wrap/io_trimesh/io_mask.h>

#include <QFileInfo>
#include <QObject>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include <E57SimpleReader.h>
#include <E57SimpleWriter.h>
#include <vcg/complex/allocate.h>
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
        LengthUnit *outUnit) const override
    {
        if (outMaterialSet)
            outMaterialSet->clear();
        const int err = load(filename, mesh, cb, outLoadMask);
        // The E57 standard (ASTM E2807) has its Cartesian coordinates in meters.
        if (err == 0 && outUnit)
            *outUnit = LengthUnit::Meter;
        return err;
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask) const override
    {
        if (outLoadMask)
            *outLoadMask = 0;
        try {
            return loadScans(filename, mesh, cb, outLoadMask);
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
    static int loadScans(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask)
    {
        e57::Reader reader(filename.toStdString(), {});
        if (!reader.IsOpen())
            return -1;

        const int64_t scanCount = reader.GetData3DCount();
        if (scanCount <= 0)
            return -2;

        mesh.Clear();
        int loadMask = vcg::tri::io::Mask::IOM_VERTCOORD;

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
                    vi->P() = vcg::Point3f::Construct(rotation.Rotate(p) + translation);
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
        if (outLoadMask)
            *outLoadMask = loadMask;
        return 0;
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

            const double x = double(p[0]);
            const double y = double(p[1]);
            const double z = double(p[2]);
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
