#include "iglquantities.h"

#include "document.h"
#include "filterparam.h"
#include "libiglmeshadapter.h"

#include <igl/avg_edge_length.h>
#include <igl/exact_geodesic.h>
#include <igl/gaussian_curvature.h>
#include <igl/heat_geodesics.h>
#include <igl/hessian_energy.h>
#include <igl/massmatrix.h>
#include <igl/principal_curvature.h>
#include <Eigen/SparseCholesky>
#include <wrap/io_trimesh/io_mask.h>
#include <QObject>
#include <algorithm>
#include <cmath>
#include <exception>
#include <vector>

namespace {

constexpr QLatin1StringView kGaussian("compute_gaussian_curvature_libigl");
constexpr QLatin1StringView kPrincipal("compute_principal_curvature_directions_libigl");
constexpr QLatin1StringView kExactGeodesic("compute_exact_geodesic_distance_from_selection_libigl");
constexpr QLatin1StringView kHeatGeodesic("compute_heat_geodesic_distance_from_selection_libigl");
constexpr QLatin1StringView kHessianSmooth("smooth_vertex_scalar_by_hessian_energy_libigl");
namespace IglAdapter = meshlab::libigl;
using Mask = vcg::tri::io::Mask;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

MeshFilterRunResult scalarSuccess(int meshIndex, const QStringList &messages)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = messages;
    result.visualizationHints.push_back({
        meshIndex,
        MeshFilterVisualizationAttribute::VertexQuality
    });
    return result;
}

Eigen::VectorXd curvatureScalar(
    const Eigen::VectorXd &maximum,
    const Eigen::VectorXd &minimum,
    const QString &mapping)
{
    if (mapping == QStringLiteral("gaussian"))
        return maximum.cwiseProduct(minimum);
    if (mapping == QStringLiteral("min"))
        return maximum.cwiseMin(minimum);
    if (mapping == QStringLiteral("max"))
        return maximum.cwiseMax(minimum);
    return 0.5 * (maximum + minimum);
}

} // namespace

bool isIglQuantityFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kGaussian)
        || filterId == QString::fromLatin1(kPrincipal)
        || filterId == QString::fromLatin1(kExactGeodesic)
        || filterId == QString::fromLatin1(kHeatGeodesic)
        || filterId == QString::fromLatin1(kHessianSmooth);
}

MeshFilterRunResult runIglQuantityFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    IglAdapter::EigenMesh source;
    QString error;
    if (!IglAdapter::meshToEigen(entry.mesh, source, error))
        return fail(error);

    const bool isGeodesic = filterId == QString::fromLatin1(kExactGeodesic)
        || filterId == QString::fromLatin1(kHeatGeodesic);
    Eigen::VectorXi selected;
    if (isGeodesic && !IglAdapter::selectedVertexRows(entry.mesh, source, selected, error))
        return fail(error);

    const QString label = filterId == QString::fromLatin1(kGaussian)
        ? QObject::tr("Gaussian Curvature (libigl)")
        : filterId == QString::fromLatin1(kPrincipal)
            ? QObject::tr("Principal Curvature Directions (libigl)")
            : filterId == QString::fromLatin1(kExactGeodesic)
                ? QObject::tr("Exact Geodesic Distance (libigl)")
                : filterId == QString::fromLatin1(kHeatGeodesic)
                    ? QObject::tr("Heat Geodesic Distance (libigl)")
                    : QObject::tr("Natural-Boundary Hessian Scalar Smoothing (libigl)");
    doc.beginFilterProgress(label);
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(10, "Converting mesh to libigl matrices...");

    Eigen::VectorXd values;
    int invalidCurvatureVertices = 0;
    try {
        if (filterId == QString::fromLatin1(kGaussian)) {
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(35, "Computing angle deficits...");
            igl::gaussian_curvature(source.vertices, source.faces, values);
        } else if (filterId == QString::fromLatin1(kPrincipal)) {
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(30, "Fitting principal curvatures...");

            Eigen::MatrixXd maximumDirections, minimumDirections;
            Eigen::VectorXd maximumValues, minimumValues;
            std::vector<int> badVertices;
            igl::principal_curvature(
                source.vertices,
                source.faces,
                maximumDirections,
                minimumDirections,
                maximumValues,
                minimumValues,
                badVertices,
                unsigned(std::max(1, params.getInt(QStringLiteral("neighborhood_radius")))),
                params.getBool(QStringLiteral("use_k_ring")));

            if (maximumDirections.rows() != source.vertices.rows()
                || minimumDirections.rows() != source.vertices.rows()
                || maximumDirections.cols() != 3 || minimumDirections.cols() != 3
                || maximumValues.size() != source.vertices.rows()
                || minimumValues.size() != source.vertices.rows()) {
                error = QObject::tr("libigl returned an unexpected principal curvature result size.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }

            std::vector<bool> invalid(size_t(source.vertices.rows()), false);
            for (int row : badVertices)
                if (row >= 0 && size_t(row) < invalid.size())
                    invalid[size_t(row)] = true;
            for (Eigen::Index row = 0; row < source.vertices.rows(); ++row) {
                if (!maximumDirections.row(row).allFinite()
                    || !minimumDirections.row(row).allFinite()
                    || !std::isfinite(maximumValues(row))
                    || !std::isfinite(minimumValues(row))) {
                    invalid[size_t(row)] = true;
                }
                if (!invalid[size_t(row)])
                    continue;
                maximumDirections.row(row).setZero();
                minimumDirections.row(row).setZero();
                maximumValues(row) = minimumValues(row) = 0.0;
                ++invalidCurvatureVertices;
            }

            if (!IglAdapter::writeVertexCurvature(
                    entry.mesh,
                    source,
                    maximumDirections,
                    minimumDirections,
                    maximumValues,
                    minimumValues,
                    error)) {
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
            values = curvatureScalar(
                maximumValues,
                minimumValues,
                params.getEnum(QStringLiteral("quality_mapping")));
        } else if (filterId == QString::fromLatin1(kExactGeodesic)) {
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(25, "Propagating exact geodesics...");
            const Eigen::VectorXi targets = Eigen::VectorXi::LinSpaced(
                source.vertices.rows(), 0, int(source.vertices.rows()) - 1);
            const Eigen::VectorXi noFaces;
            igl::exact_geodesic(
                source.vertices,
                source.faces,
                selected,
                noFaces,
                targets,
                noFaces,
                values);
        } else if (filterId == QString::fromLatin1(kHeatGeodesic)) {
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(30, "Factorizing heat-method operators...");
            const double averageEdge = igl::avg_edge_length(source.vertices, source.faces);
            const double multiplier = params.getDouble(QStringLiteral("time_step_multiplier"));
            if (!std::isfinite(averageEdge) || averageEdge <= 0.0) {
                error = QObject::tr("Cannot choose a heat timestep from a zero or invalid average edge length.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }

            igl::HeatGeodesicsData<double> data;
            data.use_intrinsic_delaunay = params.getBool(QStringLiteral("use_intrinsic_delaunay"));
            if (!igl::heat_geodesics_precompute(
                    source.vertices,
                    source.faces,
                    multiplier * averageEdge * averageEdge,
                    data)) {
                error = QObject::tr(
                    "libigl could not factorize the heat-method system. The mesh may contain degenerate or poorly conditioned triangles.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(75, "Solving heat distance...");
            igl::heat_geodesics_solve(data, selected, values);
        } else if (filterId == QString::fromLatin1(kHessianSmooth)) {
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(30, "Building Hessian and mass matrices...");
            const double weight = std::clamp(
                params.getDouble(QStringLiteral("smoothing_weight")), 0.0, 0.999999);
            Eigen::VectorXd input(source.vertices.rows());
            for (Eigen::Index row = 0; row < input.size(); ++row)
                input(row) = entry.mesh.vert[size_t(source.vertexToSourceIndex[size_t(row)])].cQ();

            Eigen::SparseMatrix<double> hessian, mass;
            igl::hessian_energy(source.vertices, source.faces, hessian);
            igl::massmatrix(
                source.vertices, source.faces, igl::MASSMATRIX_TYPE_VORONOI, mass);
            const Eigen::SparseMatrix<double> system = weight * hessian + (1.0 - weight) * mass;
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> solver(system);
            if (solver.info() != Eigen::Success) {
                error = QObject::tr("Could not factorize the Hessian smoothing system.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
            values = solver.solve((1.0 - weight) * mass * input);
            if (solver.info() != Eigen::Success) {
                error = QObject::tr("Could not solve the Hessian smoothing system.");
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
        } else {
            return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
        }
    } catch (const std::exception &e) {
        error = QObject::tr("%1 failed: %2").arg(label, QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("%1 failed with an unknown error.").arg(label);
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(90, "Writing vertex attributes...");
    if (!IglAdapter::writeVertexScalars(entry.mesh, source, values, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    entry.ioMask |= Mask::IOM_VERTQUALITY;
    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Computed %1 for '%2'").arg(label, entry.name));
    doc.finishFilterProgress(true, QObject::tr("Computed %1.").arg(label));

    QStringList messages = {
        QObject::tr("Computed %1 for %2 vertices on '%3'.")
            .arg(label)
            .arg(source.vertices.rows())
            .arg(entry.name)
    };
    if (isGeodesic)
        messages << QObject::tr("Source vertices: %1").arg(selected.size());
    if (invalidCurvatureVertices > 0) {
        messages << QObject::tr("Assigned zero curvature to %1 vertex/vertices where fitting failed.")
                        .arg(invalidCurvatureVertices);
    }
    if (source.skippedFaces > 0)
        messages << QObject::tr("Skipped %1 invalid or degenerate face(s).").arg(source.skippedFaces);
    return scalarSuccess(meshIndex, messages);
}
