#include "geogrambooleans.h"

#include "document.h"
#include "filterparam.h"
#include "geogrammeshadapter.h"

#include <geogram/mesh/mesh_surface_intersection.h>

#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <QStringList>
#include <exception>

namespace {

constexpr QLatin1StringView kUnion("mesh_union_geogram");
constexpr QLatin1StringView kIntersection("mesh_intersection_geogram");
constexpr QLatin1StringView kDifference("mesh_difference_geogram");
constexpr QLatin1StringView kXor("mesh_symmetric_difference_geogram");
constexpr QLatin1StringView kRepair("repair_self_intersections_geogram");

using Mask = vcg::tri::io::Mask;
namespace GeoAdapter = meshlab::geogram;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

// "A+B", "A*B", "A-B" and "B-A" are the only operations geogram's boolean
// evaluator accepts; symmetric difference is composed from three of them.
bool operationForFilter(const QString &filterId, QString &expression, QString &name)
{
    if (filterId == QString::fromLatin1(kUnion)) {
        expression = QStringLiteral("A+B");
        name = QObject::tr("union");
        return true;
    }
    if (filterId == QString::fromLatin1(kIntersection)) {
        expression = QStringLiteral("A*B");
        name = QObject::tr("intersection");
        return true;
    }
    if (filterId == QString::fromLatin1(kDifference)) {
        expression = QStringLiteral("A-B");
        name = QObject::tr("difference");
        return true;
    }
    if (filterId == QString::fromLatin1(kXor)) {
        expression.clear();
        name = QObject::tr("symmetric difference");
        return true;
    }
    return false;
}

::GEO::MeshBooleanOperationFlags booleanFlags(bool simplifyCoplanarFacets)
{
    return simplifyCoplanarFacets
        ? ::GEO::MESH_BOOL_OPS_DEFAULT
        : ::GEO::MESH_BOOL_OPS_NO_SIMPLIFY;
}

void runBoolean(
    ::GEO::Mesh &result,
    const ::GEO::Mesh &a,
    const ::GEO::Mesh &b,
    const QString &expression,
    bool simplifyCoplanarFacets)
{
    ::GEO::mesh_boolean_operation(
        result,
        a,
        b,
        expression.toStdString(),
        booleanFlags(simplifyCoplanarFacets));
}

MeshFilterRunResult runRepairSelfIntersections(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh."));

    const int maxIterations = params.getInt(QStringLiteral("maxIterations"), 3);
    const Document::MeshEntry &entry = doc.mesh(meshIndex);

    GeoAdapter::ensureInitialized();

    QString error;
    GeoAdapter::GeoMesh input;
    if (!GeoAdapter::meshToGeo(entry.mesh, input, error))
        return fail(error);

    doc.beginFilterProgress(QObject::tr("Repair self-intersections"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Resolving self-intersections...");

    QStringList geoLog;
    try {
        GeoAdapter::LogCapture capture(geoLog);
        ::GEO::mesh_remove_intersections(input.mesh, ::GEO::index_t(maxIterations));
    } catch (const std::exception &e) {
        error = QObject::tr("geogram failed to resolve self-intersections: %1")
                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("geogram failed to resolve self-intersections with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(80, "Converting output mesh...");

    VCGMesh output;
    if (!GeoAdapter::geoToMesh(input.mesh, output, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    const int outputMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    const int newMeshIndex = doc.addMesh(output, {}, outputMask);
    if (newMeshIndex < 0) {
        error = QObject::tr("Failed to add the repaired mesh to the document.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    doc.finishFilterProgress(true, QObject::tr("Resolved self-intersections."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.newMeshIndices.push_back(newMeshIndex);
    result.infoMessages
        << QObject::tr("Input mesh: '%1' (%2 faces).").arg(entry.name).arg(input.faceToSourceIndex.size())
        << QObject::tr("Output mesh: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    if (input.skippedFaces > 0)
        result.infoMessages << QObject::tr("Skipped %1 invalid or degenerate face(s).").arg(input.skippedFaces);
    result.infoMessages << geoLog;
    return result;
}

} // namespace

MeshFilterRunResult runGeogramBooleanFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    if (filterId == QString::fromLatin1(kRepair))
        return runRepairSelfIntersections(params, doc);

    QString expression;
    QString operationName;
    if (!operationForFilter(filterId, expression, operationName))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int firstIndex = params.getMesh(QStringLiteral("firstMesh"), doc.currentMeshIndex());
    const int secondIndex = params.getMesh(QStringLiteral("secondMesh"), doc.currentMeshIndex());
    if (firstIndex < 0 || firstIndex >= doc.meshCount()
        || secondIndex < 0 || secondIndex >= doc.meshCount()) {
        return fail(QObject::tr("Boolean operation requires two valid mesh operands."));
    }
    if (firstIndex == secondIndex)
        return fail(QObject::tr("Boolean operation requires two different mesh operands."));

    const bool simplifyCoplanarFacets = params.getBool(QStringLiteral("simplifyCoplanarFacets"), true);

    const Document::MeshEntry &firstEntry = doc.mesh(firstIndex);
    const Document::MeshEntry &secondEntry = doc.mesh(secondIndex);

    GeoAdapter::ensureInitialized();

    QString error;
    GeoAdapter::GeoMesh first;
    GeoAdapter::GeoMesh second;
    if (!GeoAdapter::meshToGeo(firstEntry.mesh, first, error, &firstEntry.transform))
        return fail(QObject::tr("First mesh: %1").arg(error));
    if (!GeoAdapter::meshToGeo(secondEntry.mesh, second, error, &secondEntry.transform))
        return fail(QObject::tr("Second mesh: %1").arg(error));

    doc.beginFilterProgress(QObject::tr("Boolean %1").arg(operationName));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Running geogram boolean operation...");

    ::GEO::Mesh resultMesh;
    QStringList geoLog;
    try {
        GeoAdapter::LogCapture capture(geoLog);
        if (filterId == QString::fromLatin1(kXor)) {
            // Geogram has no symmetric difference, so it is composed:
            // (A-B) + (B-A), three boolean evaluations instead of one.
            ::GEO::Mesh aMinusB;
            ::GEO::Mesh bMinusA;
            runBoolean(aMinusB, first.mesh, second.mesh, QStringLiteral("A-B"), simplifyCoplanarFacets);
            runBoolean(bMinusA, first.mesh, second.mesh, QStringLiteral("B-A"), simplifyCoplanarFacets);
            runBoolean(resultMesh, aMinusB, bMinusA, QStringLiteral("A+B"), simplifyCoplanarFacets);
        } else {
            runBoolean(resultMesh, first.mesh, second.mesh, expression, simplifyCoplanarFacets);
        }
    } catch (const std::exception &e) {
        error = QObject::tr("geogram boolean operation failed: %1")
                    .arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("geogram boolean operation failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(80, "Converting boolean output mesh...");

    VCGMesh output;
    if (!GeoAdapter::geoToMesh(resultMesh, output, error)) {
        error = QObject::tr(
                    "The %1 produced no geometry (%2). Both operands should be closed, "
                    "consistently oriented and free of self-intersections.")
                    .arg(operationName, error);
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    const int outputMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    const int newMeshIndex = doc.addMesh(output, {}, outputMask);
    if (newMeshIndex < 0) {
        error = QObject::tr("Failed to add the boolean result mesh to the document.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    doc.finishFilterProgress(true, QObject::tr("Generated boolean result mesh."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.newMeshIndices.push_back(newMeshIndex);
    result.infoMessages
        << QObject::tr("Operation: %1").arg(operationName)
        << QObject::tr("First mesh: '%1' (%2 faces).").arg(firstEntry.name).arg(first.faceToSourceIndex.size())
        << QObject::tr("Second mesh: '%1' (%2 faces).").arg(secondEntry.name).arg(second.faceToSourceIndex.size())
        << QObject::tr("Output mesh: %1 vertices, %2 faces.").arg(output.VN()).arg(output.FN());
    if (first.skippedFaces > 0)
        result.infoMessages << QObject::tr("Skipped %1 invalid or degenerate face(s) from the first mesh.").arg(first.skippedFaces);
    if (second.skippedFaces > 0)
        result.infoMessages << QObject::tr("Skipped %1 invalid or degenerate face(s) from the second mesh.").arg(second.skippedFaces);
    result.infoMessages << geoLog;
    return result;
}

bool isGeogramBooleanFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kUnion)
        || filterId == QString::fromLatin1(kIntersection)
        || filterId == QString::fromLatin1(kDifference)
        || filterId == QString::fromLatin1(kXor)
        || filterId == QString::fromLatin1(kRepair);
}
