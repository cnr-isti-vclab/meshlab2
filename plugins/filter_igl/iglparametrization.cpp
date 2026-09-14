#include "iglparametrization.h"

#include "document.h"
#include "filterparam.h"
#include "libiglmeshadapter.h"
#include "meshfilterpluginmanager.h"

#include <igl/arap.h>
#include <igl/boundary_loop.h>
#include <igl/flipped_triangles.h>
#include <igl/harmonic.h>
#include <igl/lscm.h>
#include <igl/map_vertices_to_circle.h>
#include <igl/slim.h>
#include <wrap/io_trimesh/io_mask.h>
#include <QObject>
#include <algorithm>
#include <exception>
#include <memory>

namespace {

constexpr QLatin1StringView kHarmonic("parametrize_by_harmonic_map_libigl");
constexpr QLatin1StringView kLscm("parametrize_by_least_squares_conformal_maps_libigl");
constexpr QLatin1StringView kArap("parametrize_by_as_rigid_as_possible_libigl");
constexpr QLatin1StringView kSlim("parametrize_by_slim_libigl");
using Mask = vcg::tri::io::Mask;
namespace IglAdapter = meshlab::libigl;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

MeshFilterRunResult success(const QStringList &info)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    return result;
}

bool harmonicMap(
    const IglAdapter::EigenMesh &mesh,
    int order,
    Eigen::VectorXi &boundary,
    Eigen::MatrixXd &uv,
    QString &error)
{
    igl::boundary_loop(mesh.faces, boundary);
    if (boundary.size() == 0) {
        error = QObject::tr("Parametrization requires a mesh with a boundary.");
        return false;
    }
    if (boundary.size() >= mesh.vertices.rows()) {
        error = QObject::tr("Parametrization requires at least one non-boundary vertex.");
        return false;
    }

    Eigen::MatrixXd boundaryUv;
    igl::map_vertices_to_circle(mesh.vertices, boundary, boundaryUv);
    if (!igl::harmonic(mesh.vertices, mesh.faces, boundary, boundaryUv, order, uv)) {
        error = QObject::tr("Harmonic initialization failed.");
        return false;
    }
    return true;
}

bool writeVertexTexcoords(
    Document::MeshEntry &entry,
    const IglAdapter::EigenMesh &source,
    const Eigen::MatrixXd &uv,
    QString &error)
{
    if (uv.rows() != source.vertices.rows() || uv.cols() < 2) {
        error = QObject::tr("libigl returned an unexpected UV matrix.");
        return false;
    }

    VCGMesh &mesh = entry.mesh;
    mesh.vert.EnableTexCoord();
    for (Eigen::Index row = 0; row < uv.rows(); ++row) {
        const int sourceIndex = source.vertexToSourceIndex[size_t(row)];
        if (sourceIndex < 0 || size_t(sourceIndex) >= mesh.vert.size())
            continue;
        VCGVertex &vertex = mesh.vert[size_t(sourceIndex)];
        if (vertex.IsD())
            continue;
        vertex.T().U() = float(uv(row, 0));
        vertex.T().V() = float(uv(row, 1));
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

MeshFilterRunResult runIglParametrizationFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    if (entry.mesh.VN() <= 0 || entry.mesh.FN() <= 0)
        return fail(QObject::tr("Parametrization requires a mesh with vertices and faces."));

    IglAdapter::EigenMesh eigenMesh;
    QString error;
    if (!IglAdapter::meshToEigen(entry.mesh, eigenMesh, error))
        return fail(error);

    Eigen::VectorXi boundary;
    Eigen::MatrixXd uv;
    QString label;

    try {
        if (filterId == QString::fromLatin1(kHarmonic)) {
            const int harmonicOrder = std::max(1, params.getInt(QStringLiteral("harm_function")));
            label = QObject::tr("Parametrize by Harmonic Map (libigl)");
            doc.beginFilterProgress(label);
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(10, "Finding boundary loop...");

            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(55, "Solving harmonic map...");
            if (!harmonicMap(eigenMesh, harmonicOrder, boundary, uv, error)) {
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
        } else if (filterId == QString::fromLatin1(kLscm)) {
            label = QObject::tr("Parametrize by Least Squares Conformal Maps (libigl)");
            doc.beginFilterProgress(label);
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(10, "Finding boundary loop...");

            igl::boundary_loop(eigenMesh.faces, boundary);
            if (boundary.size() == 0) {
                error = QObject::tr(
                    "Least Squares Conformal Maps can be applied only to meshes that have a boundary.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }

            Eigen::VectorXi boundaryPoints(2);
            boundaryPoints(0) = boundary(0);
            boundaryPoints(1) = boundary(boundary.size() / 2);

            Eigen::MatrixXd boundaryConditions(2, 2);
            boundaryConditions << 0.0, 0.0,
                                  1.0, 0.0;

            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(55, "Solving LSCM map...");
            if (!igl::lscm(
                    eigenMesh.vertices,
                    eigenMesh.faces,
                    boundaryPoints,
                    boundaryConditions,
                    uv)) {
                error = QObject::tr("Least Squares Conformal Maps parametrization failed.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
        } else if (filterId == QString::fromLatin1(kArap)
                   || filterId == QString::fromLatin1(kSlim)) {
            const bool arap = filterId == QString::fromLatin1(kArap);
            label = arap
                ? QObject::tr("As-Rigid-As-Possible Parametrization (libigl)")
                : QObject::tr("Scalable Locally Injective Parametrization (libigl)");
            doc.beginFilterProgress(label);
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(20, "Computing harmonic initialization...");
            if (!harmonicMap(eigenMesh, 1, boundary, uv, error)) {
                doc.finishFilterProgress(false, error);
                return fail(error);
            }

            const int iterations = std::max(1, params.getInt(QStringLiteral("iterations")));
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(55, arap ? "Optimizing ARAP map..." : "Optimizing SLIM map...");
            if (arap) {
                igl::ARAPData data;
                data.with_dynamics = true;
                data.max_iter = iterations;
                const Eigen::VectorXi fixed;
                const Eigen::MatrixXd fixedUv;
                if (!igl::arap_precomputation(
                        eigenMesh.vertices, eigenMesh.faces, 2, fixed, data)
                    || !igl::arap_solve(fixedUv, data, uv)) {
                    error = QObject::tr("libigl ARAP optimization failed.");
                    doc.finishFilterProgress(false, error);
                    return fail(error);
                }
            } else {
                // SLIM requires an injective initial map. Cotangent harmonic maps
                // can flip on poor triangles, so retry with uniform weights.
                if (igl::flipped_triangles(uv, eigenMesh.faces).size() > 0) {
                    Eigen::MatrixXd boundaryUv;
                    igl::map_vertices_to_circle(eigenMesh.vertices, boundary, boundaryUv);
                    if (!igl::harmonic(eigenMesh.faces, boundary, boundaryUv, 1, uv)) {
                        error = QObject::tr("Could not build an injective SLIM initialization.");
                        doc.finishFilterProgress(false, error);
                        return fail(error);
                    }
                }
                if (igl::flipped_triangles(uv, eigenMesh.faces).size() > 0) {
                    error = QObject::tr(
                        "SLIM requires an injective initial map, but both harmonic initializations contain flipped triangles.");
                    doc.finishFilterProgress(false, error);
                    return fail(error);
                }

                igl::MappingEnergyType energy = igl::SYMMETRIC_DIRICHLET;
                const QString energyId = params.getEnum(QStringLiteral("energy"));
                if (energyId == QStringLiteral("arap"))
                    energy = igl::ARAP;
                else if (energyId == QStringLiteral("conformal"))
                    energy = igl::CONFORMAL;

                igl::SLIMData data;
                const Eigen::VectorXi fixed;
                const Eigen::MatrixXd fixedUv;
                igl::slim_precompute(
                    eigenMesh.vertices,
                    eigenMesh.faces,
                    uv,
                    data,
                    energy,
                    fixed,
                    fixedUv,
                    0.0);
                uv = igl::slim_solve(data, iterations);
            }
        } else {
            return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
        }
    } catch (const std::exception &e) {
        error = QObject::tr("libigl parametrization failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("libigl parametrization failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(90, "Writing texture coordinates...");
    if (!writeVertexTexcoords(entry, eigenMesh, uv, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Computed %1 for '%2'").arg(label, entry.name));
    doc.finishFilterProgress(true, QObject::tr("Computed UV parametrization."));

    QStringList info;
    info << QObject::tr("Computed %1 for '%2'.").arg(label, entry.name)
         << QObject::tr("Parameterized vertices: %1").arg(eigenMesh.vertexToSourceIndex.size())
         << QObject::tr("Faces: %1").arg(eigenMesh.faces.rows());
    if (eigenMesh.skippedFaces > 0)
        info << QObject::tr("Skipped %1 invalid or degenerate face(s).").arg(eigenMesh.skippedFaces);

    return success(info);
}


bool isIglParametrizationFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kHarmonic)
        || filterId == QString::fromLatin1(kLscm)
        || filterId == QString::fromLatin1(kArap)
        || filterId == QString::fromLatin1(kSlim);
}
