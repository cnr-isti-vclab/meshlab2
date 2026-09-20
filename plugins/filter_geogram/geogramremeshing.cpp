#include "geogramremeshing.h"

#include "document.h"
#include "filterparam.h"
#include "geogrammeshadapter.h"

#include <geogram/mesh/mesh_geometry.h>
#include <geogram/mesh/mesh_remesh.h>

#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <QStringList>
#include <algorithm>
#include <exception>

namespace {

constexpr QLatin1StringView kRemesh("remesh_by_centroidal_voronoi_tessellation_geogram");

using Mask = vcg::tri::io::Mask;
namespace GeoAdapter = meshlab::geogram;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

} // namespace

MeshFilterRunResult runGeogramRemeshingFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    if (filterId != QString::fromLatin1(kRemesh))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh."));
    Document::MeshEntry &entry = doc.mesh(meshIndex);

    const int targetVertexCount =
        std::max(4, params.getInt(QStringLiteral("targetVertexCount"), 10000));
    const bool anisotropic = params.getBool(QStringLiteral("anisotropic"), false);
    const double anisotropy = params.getDouble(QStringLiteral("anisotropy"), 0.04);
    const int lloydIterations = std::max(0, params.getInt(QStringLiteral("lloydIterations"), 5));
    const int newtonIterations = std::max(0, params.getInt(QStringLiteral("newtonIterations"), 30));
    const int newtonHessianSamples =
        std::max(1, params.getInt(QStringLiteral("newtonHessianSamples"), 7));
    const bool adjustToSource = params.getBool(QStringLiteral("adjustToSource"), true);
    const double adjustMaxEdgeDistance =
        params.getDouble(QStringLiteral("adjustMaxEdgeDistance"), 0.5);

    const int inputVertexCount = entry.mesh.VN();
    const int inputFaceCount = entry.mesh.FN();

    GeoAdapter::ensureInitialized();

    QString error;
    GeoAdapter::GeoMesh input;
    if (!GeoAdapter::meshToGeo(entry.mesh, input, error))
        return fail(error);
    input.mesh.facets.connect();

    doc.beginFilterProgress(QObject::tr("Remesh by centroidal Voronoi tessellation"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(10, "Building the Voronoi tessellation...");

    ::GEO::Mesh output;
    QStringList geoLog;
    try {
        GeoAdapter::LogCapture capture(geoLog);
        // Anisotropy is not a flag on remesh_smooth: set_anisotropy raises the
        // mesh to six dimensions and writes the scaled normals into
        // coordinates 3-5, and dim=6 then tells the remesher to work in that
        // space. dim=3 is the isotropic case. This is geogram's own idiom.
        ::GEO::coord_index_t dim = 3;
        if (anisotropic && anisotropy > 0.0) {
            ::GEO::set_anisotropy(input.mesh, anisotropy);
            dim = 6;
        }
        ::GEO::remesh_smooth(
            input.mesh,
            output,
            ::GEO::index_t(targetVertexCount),
            dim,
            ::GEO::index_t(lloydIterations),
            ::GEO::index_t(newtonIterations),
            ::GEO::index_t(newtonHessianSamples),
            adjustToSource,
            adjustMaxEdgeDistance);
    } catch (const std::exception &e) {
        error = QObject::tr("geogram remeshing failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("geogram remeshing failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(90, "Converting the remeshed surface...");

    // The remesher replaces the surface outright, so geoToMesh rebuilds the
    // layer in place -- it clears the target first. Per-vertex and per-face
    // attributes do not survive, because no correspondence to the old
    // tessellation exists; the help says so.
    if (!GeoAdapter::geoToMesh(output, entry.mesh, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }
    entry.ioMask |= Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;

    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Remeshed '%1'").arg(entry.name));
    doc.finishFilterProgress(true, QObject::tr("Remeshed the surface."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Mode: %1").arg(
               anisotropic && anisotropy > 0.0
                   ? QObject::tr("anisotropic (strength %1)").arg(anisotropy)
                   : QObject::tr("isotropic"))
        << QObject::tr("Input: %1 vertices, %2 faces.").arg(inputVertexCount).arg(inputFaceCount)
        << QObject::tr("Output: %1 vertices, %2 faces.").arg(entry.mesh.VN()).arg(entry.mesh.FN());
    if (input.skippedFaces > 0)
        result.infoMessages << QObject::tr("Skipped %1 invalid or degenerate face(s).").arg(input.skippedFaces);
    result.infoMessages << geoLog;
    return result;
}

bool isGeogramRemeshingFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kRemesh);
}
