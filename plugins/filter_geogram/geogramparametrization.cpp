#include "geogramparametrization.h"

#include "document.h"
#include "filterparam.h"
#include "geogrammeshadapter.h"

#include <geogram/parameterization/mesh_ABF.h>
#include <geogram/parameterization/mesh_LSCM.h>

#include <vcg/complex/algorithms/clean.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <QStringList>
#include <exception>

namespace {

constexpr QLatin1StringView kLscm("parametrize_by_least_squares_conformal_maps_geogram");
constexpr QLatin1StringView kSpectral("parametrize_by_spectral_conformal_maps_geogram");
constexpr QLatin1StringView kAbf("parametrize_by_angle_based_flattening_geogram");

using Mask = vcg::tri::io::Mask;
namespace GeoAdapter = meshlab::geogram;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

// Both flatteners map a surface with a boundary into the plane, so a closed
// surface has no answer for them to give. Checked here rather than left to
// geogram, which returns a degenerate layout instead of refusing.
bool hasBoundary(VCGMesh &mesh)
{
    int totalEdges = 0;
    int boundaryEdges = 0;
    int nonManifoldEdges = 0;
    vcg::tri::Clean<VCGMesh>::CountEdgeNum(mesh, totalEdges, boundaryEdges, nonManifoldEdges);
    return boundaryEdges > 0;
}

// Mirrors iglparametrization's write-back so both backends leave a layer in
// the same state: per-vertex UVs always, per-wedge UVs refreshed from them
// when the layer already carried some.
bool writeVertexTexCoords(
    Document::MeshEntry &entry,
    const GeoAdapter::GeoMesh &source,
    const std::vector<std::array<float, 2>> &uv,
    QString &error)
{
    if (uv.size() != source.vertexToSourceIndex.size()) {
        error = QObject::tr("geogram returned %1 texture coordinates for %2 vertices.")
                    .arg(uv.size())
                    .arg(source.vertexToSourceIndex.size());
        return false;
    }

    VCGMesh &mesh = entry.mesh;
    mesh.vert.EnableTexCoord();
    for (size_t row = 0; row < uv.size(); ++row) {
        const int sourceIndex = source.vertexToSourceIndex[row];
        if (sourceIndex < 0 || size_t(sourceIndex) >= mesh.vert.size())
            continue;
        VCGVertex &vertex = mesh.vert[size_t(sourceIndex)];
        if (vertex.IsD())
            continue;
        vertex.T().U() = uv[row][0];
        vertex.T().V() = uv[row][1];
        vertex.T().N() = 0;
    }

    if ((entry.ioMask & Mask::IOM_WEDGTEXCOORD) != 0) {
        mesh.face.EnableWedgeTexCoord();
        for (VCGFace &face : mesh.face) {
            if (face.IsD())
                continue;
            for (int corner = 0; corner < 3; ++corner) {
                if (const VCGVertex *vertex = face.cV(corner)) {
                    face.WT(corner).U() = vertex->cT().U();
                    face.WT(corner).V() = vertex->cT().V();
                    face.WT(corner).N() = vertex->cT().N();
                }
            }
        }
    }

    entry.ioMask |= Mask::IOM_VERTTEXCOORD;
    return true;
}

} // namespace

MeshFilterRunResult runGeogramParametrizationFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    Q_UNUSED(params);

    const bool isLscm = filterId == QString::fromLatin1(kLscm);
    const bool isSpectral = filterId == QString::fromLatin1(kSpectral);
    const bool isAbf = filterId == QString::fromLatin1(kAbf);
    if (!isLscm && !isSpectral && !isAbf)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const QString label = isLscm
        ? QObject::tr("least squares conformal maps")
        : isSpectral ? QObject::tr("spectral conformal maps")
                     : QObject::tr("angle-based flattening");

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    if (!hasBoundary(entry.mesh)) {
        return fail(QObject::tr(
            "Parametrization requires a mesh with a boundary; '%1' is closed. "
            "Cut it open first, or use an atlas filter, which segments the surface "
            "into charts before flattening each one.").arg(entry.name));
    }

    GeoAdapter::ensureInitialized();
    if (isSpectral && !GeoAdapter::arpackAvailable()) {
        return fail(QObject::tr(
            "Spectral conformal maps need geogram's ARPACK eigensolver, which could not "
            "be loaded. Use Parametrize by Least Squares Conformal Maps (geogram) instead, "
            "or reinstall the application: the eigensolver ships inside it."));
    }

    QString error;
    GeoAdapter::GeoMesh input;
    if (!GeoAdapter::meshToGeo(entry.mesh, input, error))
        return fail(error);

    // The flatteners navigate the surface through facet adjacency, which a
    // freshly built GEO::Mesh does not have. connect() derives it from vertex
    // indices, so unlike geogram's own mesh_repair it leaves vertex numbering
    // -- and therefore vertexToSourceIndex -- untouched. The cost is that
    // coincident-but-distinct vertices stay split, which the help text warns
    // about rather than silently welding.
    input.mesh.facets.connect();

    doc.beginFilterProgress(QObject::tr("Parametrize by %1").arg(label));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Flattening...");

    QStringList geoLog;
    try {
        GeoAdapter::LogCapture capture(geoLog);
        if (isLscm || isSpectral)
            ::GEO::mesh_compute_LSCM(input.mesh, "tex_coord", isSpectral);
        else
            ::GEO::mesh_compute_ABF_plus_plus(input.mesh, "tex_coord");
    } catch (const std::exception &e) {
        error = QObject::tr("geogram parametrization failed: %1")
                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("geogram parametrization failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(90, "Writing texture coordinates...");

    std::vector<std::array<float, 2>> uv;
    if (!GeoAdapter::readVertexTexCoords(input.mesh, uv, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }
    if (!writeVertexTexCoords(entry, input, uv, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Computed %1 for '%2'").arg(label, entry.name));
    doc.finishFilterProgress(true, QObject::tr("Computed UV parametrization."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Computed %1 for '%2'.").arg(label, entry.name)
        << QObject::tr("Parametrized vertices: %1").arg(input.vertexToSourceIndex.size())
        << QObject::tr("Faces: %1").arg(input.faceToSourceIndex.size());
    if (input.skippedFaces > 0)
        result.infoMessages << QObject::tr("Skipped %1 invalid or degenerate face(s).").arg(input.skippedFaces);
    result.infoMessages << geoLog;
    return result;
}

bool isGeogramParametrizationFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kLscm)
        || filterId == QString::fromLatin1(kSpectral)
        || filterId == QString::fromLatin1(kAbf);
}
