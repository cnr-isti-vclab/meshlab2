#include "iglqueries.h"

#include "document.h"
#include "filterparam.h"
#include "libiglmeshadapter.h"

#include <igl/fast_winding_number.h>
#include <igl/winding_number.h>
#include <wrap/io_trimesh/io_mask.h>
#include <QObject>
#include <exception>

namespace {

constexpr QLatin1StringView kWinding("compute_generalized_winding_number_libigl");
namespace IglAdapter = meshlab::libigl;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

} // namespace

bool isIglQueryFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kWinding);
}

MeshFilterRunResult runIglQueryFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    if (!isIglQueryFilter(filterId))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int queryIndex = doc.currentMeshIndex();
    const int surfaceIndex = params.getMesh(QStringLiteral("surface_mesh"), -1);
    if (queryIndex < 0 || queryIndex >= doc.meshCount()
        || surfaceIndex < 0 || surfaceIndex >= doc.meshCount()) {
        return fail(QObject::tr("Winding number requires valid query and surface layers."));
    }
    if (queryIndex == surfaceIndex)
        return fail(QObject::tr("Choose a surface layer different from the current query layer."));

    Document::MeshEntry &queryEntry = doc.mesh(queryIndex);
    const Document::MeshEntry &surfaceEntry = doc.mesh(surfaceIndex);
    IglAdapter::EigenMesh surface;
    IglAdapter::VertexMatrix query;
    std::vector<int> queryToSource;
    QString error;
    if (!IglAdapter::meshToEigen(
            surfaceEntry.mesh, surface, error, &surfaceEntry.transform))
        return fail(QObject::tr("Surface layer: %1").arg(error));
    if (!IglAdapter::meshVerticesToEigen(
            queryEntry.mesh, query, queryToSource, error, &queryEntry.transform))
        return fail(QObject::tr("Query layer: %1").arg(error));

    const bool exact = params.getEnum(QStringLiteral("method")) == QStringLiteral("exact");
    const QString label = exact
        ? QObject::tr("Exact Generalized Winding Number (libigl)")
        : QObject::tr("Fast Generalized Winding Number (libigl)");
    doc.beginFilterProgress(label);
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, exact ? "Computing exact winding numbers..." : "Building fast winding hierarchy...");

    Eigen::VectorXd values;
    try {
        if (exact) {
            igl::winding_number(surface.vertices, surface.faces, query, values);
        } else {
            igl::FastWindingNumberBVH hierarchy;
            igl::fast_winding_number(
                surface.vertices.cast<float>(), surface.faces, 2, hierarchy);
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(65, "Evaluating fast winding numbers...");
            Eigen::VectorXf approximate;
            igl::fast_winding_number(hierarchy, 2.0f, query.cast<float>(), approximate);
            values = approximate.cast<double>();
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

    if (!IglAdapter::writeVertexScalars(queryEntry.mesh, queryToSource, values, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }
    queryEntry.ioMask |= vcg::tri::io::Mask::IOM_VERTQUALITY;
    doc.markMeshGeometryChanged(
        queryIndex,
        QObject::tr("Computed %1 for '%2'").arg(label, queryEntry.name));
    doc.finishFilterProgress(true, QObject::tr("Computed winding numbers."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = {
        QObject::tr("Computed %1 at %2 vertices of '%3' using '%4'.")
            .arg(label)
            .arg(query.rows())
            .arg(queryEntry.name, surfaceEntry.name)
    };
    result.visualizationHints.push_back({
        queryIndex,
        MeshFilterVisualizationAttribute::VertexQuality
    });
    return result;
}
