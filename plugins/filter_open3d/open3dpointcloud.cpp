#include "open3dpointcloud.h"

#include "document.h"
#include "filterparam.h"
#include "open3dmeshadapter.h"

#include "open3d/geometry/KDTreeSearchParam.h"
#include "open3d/geometry/PointCloud.h"

#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <QStringList>
#include <algorithm>
#include <exception>
#include <vector>

namespace {

constexpr QLatin1StringView kVoxel("sample_point_cloud_by_voxel_grid_open3d");
constexpr QLatin1StringView kRadius("select_point_cloud_outliers_by_radius_open3d");
constexpr QLatin1StringView kStatistical("select_point_cloud_outliers_statistically_open3d");
constexpr QLatin1StringView kNormals("compute_point_cloud_normals_open3d");

using Mask = vcg::tri::io::Mask;
namespace Adapter = meshlab::open3d;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

// Every length these filters take is in the layer's own units, which means no
// fixed default can be right: 0.01 is sensible for a unit cube and absurd for a
// scan in millimetres. Zero therefore means "derive it from this layer", scaled
// off the bounding box diagonal, so the filter does something reasonable on
// first open instead of refusing until the user guesses a number.
double resolveLength(const VCGMesh &mesh, double given, double fractionOfDiagonal)
{
    if (given > 0.0)
        return given;
    const double diagonal = double(mesh.bbox.Diag());
    return diagonal > 0.0 ? diagonal * fractionOfDiagonal : 0.0;
}

// Open3D's search parameters are three separate types with no common setter, so
// the enum is resolved to one of them here rather than at every call site.
std::shared_ptr<open3d::geometry::KDTreeSearchParam> searchParam(
    const QString &mode, int neighbourCount, double radius)
{
    if (mode == QLatin1StringView("radius"))
        return std::make_shared<open3d::geometry::KDTreeSearchParamRadius>(radius);
    if (mode == QLatin1StringView("hybrid"))
        return std::make_shared<open3d::geometry::KDTreeSearchParamHybrid>(
            radius, neighbourCount);
    return std::make_shared<open3d::geometry::KDTreeSearchParamKNN>(neighbourCount);
}

// The outlier filters return the indices they *kept*. MeshLab selects what the
// filter rejected and never deletes anything, so the kept set is inverted here
// against the mapping back to source vertices.
MeshFilterRunResult selectOutliers(
    Document &doc,
    Document::MeshEntry &entry,
    int meshIndex,
    const Adapter::CloudMapping &mapping,
    const std::vector<size_t> &kept,
    const QString &label)
{
    std::vector<bool> survives(mapping.vertexToSourceIndex.size(), false);
    for (size_t row : kept) {
        if (row < survives.size())
            survives[row] = true;
    }

    VCGMesh &mesh = entry.mesh;
    for (VCGVertex &v : mesh.vert) {
        if (!v.IsD())
            v.ClearS();
    }

    int selected = 0;
    for (size_t row = 0; row < survives.size(); ++row) {
        if (survives[row])
            continue;
        const int sourceIndex = mapping.vertexToSourceIndex[row];
        if (sourceIndex < 0 || size_t(sourceIndex) >= mesh.vert.size())
            continue;
        VCGVertex &v = mesh.vert[size_t(sourceIndex)];
        if (v.IsD())
            continue;
        v.SetS();
        ++selected;
    }

    doc.markMeshSelectionChanged(
        meshIndex,
        QObject::tr("Selected %1 outlier(s) in '%2'").arg(selected).arg(entry.name));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Method: %1").arg(label)
        << QObject::tr("Examined %1 point(s), selected %2 as outliers.")
               .arg(mapping.vertexToSourceIndex.size()).arg(selected);
    if (selected == 0)
        result.infoMessages << QObject::tr("No point met the rejection criteria.");
    return result;
}

} // namespace

MeshFilterRunResult runOpen3DPointCloudFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    const bool isVoxel = filterId == QString::fromLatin1(kVoxel);
    const bool isRadius = filterId == QString::fromLatin1(kRadius);
    const bool isStatistical = filterId == QString::fromLatin1(kStatistical);
    const bool isNormals = filterId == QString::fromLatin1(kNormals);
    if (!isVoxel && !isRadius && !isStatistical && !isNormals)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh."));
    Document::MeshEntry &entry = doc.mesh(meshIndex);

    QString error;
    open3d::geometry::PointCloud cloud;
    Adapter::CloudMapping mapping;
    if (!Adapter::meshToCloud(entry.mesh, cloud, mapping, error))
        return fail(error);

    try {
        if (isVoxel) {
            const double voxelSize = resolveLength(
                entry.mesh, params.getDouble(QStringLiteral("voxelSize"), 0.0), 0.005);
            if (!(voxelSize > 0.0)) {
                return fail(QObject::tr(
                    "The layer has no extent, so no voxel size can be derived. Give one "
                    "explicitly; it is in the layer's own units."));
            }

            doc.beginFilterProgress(QObject::tr("Voxel sampling"));
            const auto sampled = cloud.VoxelDownSample(voxelSize);

            VCGMesh output;
            if (!Adapter::cloudToMesh(*sampled, output, error)) {
                doc.finishFilterProgress(false, error);
                return fail(error);
            }

            int outputMask = Mask::IOM_VERTCOORD;
            if (mapping.hadNormals)
                outputMask |= Mask::IOM_VERTNORMAL;
            if (mapping.hadColors)
                outputMask |= Mask::IOM_VERTCOLOR;

            const int newMeshIndex = doc.addMesh(output, {}, outputMask);
            if (newMeshIndex < 0) {
                error = QObject::tr("Failed to add the sampled point cloud to the document.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
            doc.finishFilterProgress(true, QObject::tr("Sampled the point cloud."));

            MeshFilterRunResult result;
            result.success = true;
            result.documentModified = true;
            result.newMeshIndices.push_back(newMeshIndex);
            result.infoMessages
                << QObject::tr("Voxel size: %1").arg(voxelSize)
                << QObject::tr("Input: %1 point(s).").arg(mapping.vertexToSourceIndex.size())
                << QObject::tr("Output: %1 point(s).").arg(output.VN());
            if (!mapping.hadNormals)
                result.infoMessages << QObject::tr("The input had no normals, so none were averaged.");
            return result;
        }

        if (isRadius || isStatistical) {
            doc.beginFilterProgress(QObject::tr("Selecting outliers"));
            std::vector<size_t> kept;
            QString label;
            if (isRadius) {
                const int minimumNeighbours =
                    std::max(1, params.getInt(QStringLiteral("minimumNeighbours"), 16));
                const double searchRadius = resolveLength(
                    entry.mesh, params.getDouble(QStringLiteral("searchRadius"), 0.0), 0.02);
                if (!(searchRadius > 0.0)) {
                    doc.finishFilterProgress(false, QString());
                    return fail(QObject::tr(
                        "The layer has no extent, so no search radius can be derived. Give "
                        "one explicitly; it is in the layer's own units."));
                }
                std::tie(std::ignore, kept) =
                    cloud.RemoveRadiusOutliers(size_t(minimumNeighbours), searchRadius);
                label = QObject::tr("radius, %1 neighbour(s) within %2")
                            .arg(minimumNeighbours).arg(searchRadius);
            } else {
                const int neighbourCount =
                    std::max(1, params.getInt(QStringLiteral("neighbourCount"), 20));
                const double standardDeviationRatio =
                    params.getDouble(QStringLiteral("standardDeviationRatio"), 2.0);
                std::tie(std::ignore, kept) = cloud.RemoveStatisticalOutliers(
                    size_t(neighbourCount), standardDeviationRatio);
                label = QObject::tr("statistical, %1 neighbour(s), %2 sigma")
                            .arg(neighbourCount).arg(standardDeviationRatio);
            }
            doc.finishFilterProgress(true, QObject::tr("Selected outliers."));
            return selectOutliers(doc, entry, meshIndex, mapping, kept, label);
        }

        // Normals.
        const QString mode =
            params.getEnum(QStringLiteral("searchMode"), QStringLiteral("knn"));
        const int neighbourCount =
            std::max(1, params.getInt(QStringLiteral("neighbourCount"), 30));
        const double searchRadius = resolveLength(
            entry.mesh, params.getDouble(QStringLiteral("searchRadius"), 0.0), 0.02);
        if (mode != QLatin1StringView("knn") && !(searchRadius > 0.0)) {
            return fail(QObject::tr(
                "The layer has no extent, so no search radius can be derived for the %1 "
                "search mode. Give one explicitly.").arg(mode));
        }

        doc.beginFilterProgress(QObject::tr("Estimating normals"));
        cloud.EstimateNormals(*searchParam(mode, neighbourCount, searchRadius),
                              params.getBool(QStringLiteral("fastNormalComputation"), true));

        const bool orient = params.getBool(QStringLiteral("orientConsistently"), false);
        if (orient) {
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(60, "Orienting normals...");
            cloud.OrientNormalsConsistentTangentPlane(size_t(neighbourCount));
        }

        if (cloud.normals_.size() != mapping.vertexToSourceIndex.size()) {
            error = QObject::tr("Open3D returned %1 normal(s) for %2 point(s).")
                        .arg(cloud.normals_.size())
                        .arg(mapping.vertexToSourceIndex.size());
            doc.finishFilterProgress(false, error);
            return fail(error);
        }

        VCGMesh &mesh = entry.mesh;
        for (size_t row = 0; row < cloud.normals_.size(); ++row) {
            const int sourceIndex = mapping.vertexToSourceIndex[row];
            if (sourceIndex < 0 || size_t(sourceIndex) >= mesh.vert.size())
                continue;
            VCGVertex &v = mesh.vert[size_t(sourceIndex)];
            if (v.IsD())
                continue;
            const auto &n = cloud.normals_[row];
            v.N() = vcg::Point3f(float(n.x()), float(n.y()), float(n.z()));
        }
        entry.ioMask |= Mask::IOM_VERTNORMAL;

        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Computed point cloud normals for '%1'").arg(entry.name));
        doc.finishFilterProgress(true, QObject::tr("Computed point cloud normals."));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages
            << QObject::tr("Estimated %1 normal(s).").arg(cloud.normals_.size());
        if (!orient) {
            result.infoMessages << QObject::tr(
                "The normals are unoriented: neighbouring normals may point to opposite "
                "sides of the surface. Enable consistent orientation, or use one of the "
                "Orient filters.");
        }
        return result;
    } catch (const std::exception &e) {
        error = QObject::tr("Open3D failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("Open3D failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }
}

bool isOpen3DPointCloudFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kVoxel)
        || filterId == QString::fromLatin1(kRadius)
        || filterId == QString::fromLatin1(kStatistical)
        || filterId == QString::fromLatin1(kNormals);
}
