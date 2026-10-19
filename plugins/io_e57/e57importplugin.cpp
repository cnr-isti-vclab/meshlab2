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

namespace {
constexpr int kErrSaveUnsupportedFormat = -1000;
constexpr int kErrSaveNoPoints = -1001;
constexpr int kErrSaveWriteFailed = -1002;

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
        return { vcg::tri::io::Mask::IOM_VERTCOORD, false, false };
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

        e57::Reader reader(filename.toStdString(), {});
        if (!reader.IsOpen())
            return -1;

        const int64_t scanCount = reader.GetData3DCount();
        if (scanCount <= 0) {
            reader.Close();
            return -2;
        }

        mesh.Clear();

        for (int64_t scanIndex = 0; scanIndex < scanCount; ++scanIndex) {
            e57::Data3D scanHeader{};
            if (!reader.ReadData3D(scanIndex, scanHeader)) {
                reader.Close();
                return -3;
            }

            int64_t rowCount = 0, colCount = 0, pointCount = 0, groupCount = 0, countSize = 0;
            bool columnIndex = false;
            if (!reader.GetData3DSizes(scanIndex, rowCount, colCount, pointCount, groupCount, countSize, columnIndex)) {
                reader.Close();
                return -4;
            }

            if (pointCount <= 0)
                continue;

            std::vector<double> cartesianX(static_cast<size_t>(pointCount));
            std::vector<double> cartesianY(static_cast<size_t>(pointCount));
            std::vector<double> cartesianZ(static_cast<size_t>(pointCount));
            std::vector<int8_t> cartesianInvalid(scanHeader.pointFields.cartesianInvalidStateField
                ? static_cast<size_t>(pointCount)
                : 0);

            e57::Data3DPointsData_t<double> pointsData{};
            pointsData.cartesianX = cartesianX.data();
            pointsData.cartesianY = cartesianY.data();
            pointsData.cartesianZ = cartesianZ.data();
            if (scanHeader.pointFields.cartesianInvalidStateField)
                pointsData.cartesianInvalidState = cartesianInvalid.data();

            e57::CompressedVectorReader dataReader = reader.SetUpData3DPointsData(scanIndex, pointCount, pointsData);

            size_t readCount = 0;
            while ((readCount = dataReader.read()) > 0) {
                for (size_t i = 0; i < readCount; ++i) {
                    if (!cartesianInvalid.empty() && cartesianInvalid[i] != 0)
                        continue;

                    vcg::tri::Allocator<VCGMesh>::AddVertex(mesh, VCGMesh::CoordType(
                        static_cast<float>(cartesianX[i]),
                        static_cast<float>(cartesianY[i]),
                        static_cast<float>(cartesianZ[i])));
                }
            }
            dataReader.close();

            if (cb != nullptr) {
                const int progress = int((scanIndex + 1) * 100 / scanCount);
                cb(progress, "Loading E57 points\r");
            }
        }

        reader.Close();
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
        uint16_t cMinR = std::numeric_limits<uint16_t>::max();
        uint16_t cMinG = std::numeric_limits<uint16_t>::max();
        uint16_t cMinB = std::numeric_limits<uint16_t>::max();
        uint16_t cMaxR = 0;
        uint16_t cMaxG = 0;
        uint16_t cMaxB = 0;
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
                const uint16_t cr = uint16_t(c[0]) * uint16_t(257);
                const uint16_t cg = uint16_t(c[1]) * uint16_t(257);
                const uint16_t cbv = uint16_t(c[2]) * uint16_t(257);
                colorR[outIndex] = cr;
                colorG[outIndex] = cg;
                colorB[outIndex] = cbv;
                cMinR = std::min(cMinR, cr);
                cMinG = std::min(cMinG, cg);
                cMinB = std::min(cMinB, cbv);
                cMaxR = std::max(cMaxR, cr);
                cMaxG = std::max(cMaxG, cg);
                cMaxB = std::max(cMaxB, cbv);
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
                data3D.colorLimits.colorRedMinimum = cMinR;
                data3D.colorLimits.colorRedMaximum = cMaxR;
                data3D.colorLimits.colorGreenMinimum = cMinG;
                data3D.colorLimits.colorGreenMaximum = cMaxG;
                data3D.colorLimits.colorBlueMinimum = cMinB;
                data3D.colorLimits.colorBlueMaximum = cMaxB;
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
        case -4: return QObject::tr("Failed to read E57 scan sizes.");
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
