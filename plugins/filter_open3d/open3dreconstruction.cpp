#include "open3dreconstruction.h"

#include "document.h"
#include "filterparam.h"
#include "open3dmeshadapter.h"

#include "open3d/geometry/PointCloud.h"
#include "open3d/geometry/TriangleMesh.h"

#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <exception>
#include <vector>

namespace {

constexpr QLatin1StringView kBallPivoting("reconstruct_surface_by_ball_pivoting_open3d");

using Mask = vcg::tri::io::Mask;
namespace Adapter = meshlab::open3d;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

} // namespace

MeshFilterRunResult runOpen3DReconstructionFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    if (filterId != QString::fromLatin1(kBallPivoting))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh."));
    const Document::MeshEntry &entry = doc.mesh(meshIndex);

    QString error;
    open3d::geometry::PointCloud cloud;
    Adapter::CloudMapping mapping;
    if (!Adapter::meshToCloud(entry.mesh, cloud, mapping, error, &entry.transform))
        return fail(error);

    // Open3D asserts on a cloud without normals rather than refusing, and the
    // algorithm is meaningless without them in any case: the ball rolls on the
    // oriented surface.
    if (cloud.normals_.size() != cloud.points_.size()) {
        return fail(QObject::tr(
            "Ball pivoting needs oriented normals, and '%1' has none. Run "
            "Compute Point Cloud Normals first, then orient them.").arg(entry.name));
    }

    // The radii are a list because the algorithm makes one pass per ball,
    // each pass filling what the previous one was too large to reach.
    std::vector<double> radii;
    const QString raw = params.getString(QStringLiteral("radii")).trimmed();
    if (!raw.isEmpty()) {
        const QStringList parts =
            raw.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts);
        for (const QString &part : parts) {
            bool ok = false;
            const double value = part.toDouble(&ok);
            if (!ok || !(value > 0.0)) {
                return fail(QObject::tr(
                    "'%1' is not a positive number. Give the ball radii as a list, "
                    "for example \"0.5 1 2\".").arg(part));
            }
            radii.push_back(value);
        }
    }

    if (radii.empty()) {
        // A scale-free default: the diagonal is the only length the filter can
        // know about without the user supplying one.
        const double diagonal = double(entry.mesh.bbox.Diag());
        if (!(diagonal > 0.0))
            return fail(QObject::tr("The layer has no extent, so no default radius can be derived."));
        for (double factor : { 0.005, 0.01, 0.02, 0.04 })
            radii.push_back(diagonal * factor);
    }
    std::sort(radii.begin(), radii.end());

    doc.beginFilterProgress(QObject::tr("Ball pivoting"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Rolling the ball...");

    std::shared_ptr<open3d::geometry::TriangleMesh> reconstructed;
    try {
        reconstructed =
            open3d::geometry::TriangleMesh::CreateFromPointCloudBallPivoting(cloud, radii);
    } catch (const std::exception &e) {
        error = QObject::tr("Open3D ball pivoting failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("Open3D ball pivoting failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    VCGMesh output;
    if (!reconstructed || !Adapter::triangleMeshToMesh(*reconstructed, output, error)) {
        error = QObject::tr(
                    "Ball pivoting produced no surface (%1). The radii are given in the "
                    "layer's own units; try larger ones, or a denser cloud.").arg(error);
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    int outputMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    if (mapping.hadColors)
        outputMask |= Mask::IOM_VERTCOLOR;

    const int newMeshIndex = doc.addMesh(output, {}, outputMask);
    if (newMeshIndex < 0) {
        error = QObject::tr("Failed to add the reconstructed mesh to the document.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }
    doc.finishFilterProgress(true, QObject::tr("Reconstructed the surface."));

    QStringList radiiText;
    for (double r : radii)
        radiiText << QString::number(r, 'g', 4);

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.newMeshIndices.push_back(newMeshIndex);
    result.infoMessages
        << QObject::tr("Ball radii: %1").arg(radiiText.join(QStringLiteral(", ")))
        << QObject::tr("Input: %1 oriented point(s).").arg(cloud.points_.size())
        << QObject::tr("Output: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());

    const int unused = int(cloud.points_.size()) - output.VN();
    if (unused > 0) {
        result.infoMessages << QObject::tr(
            "%1 input point(s) are not referenced by any face; the ball could not "
            "reach them at these radii.").arg(unused);
    }
    return result;
}

bool isOpen3DReconstructionFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kBallPivoting);
}
