#include "geogrambooleans.h"

#include "document.h"
#include "filterparam.h"
#include "geogrammeshadapter.h"

#include <geogram/mesh/mesh_surface_intersection.h>

#include <wrap/io_trimesh/io_mask.h>

#include <vcg/complex/algorithms/closest.h>
#include <vcg/space/index/grid_static_ptr.h>

#include <QObject>
#include <QStringList>
#include <array>
#include <exception>
#include <memory>

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

// Attribute transfer, matching what the libigl booleans offer.
//
// geogram cannot help here: mesh_boolean_operation copies only positions and
// connectivity out of its operands (copy_operand in
// mesh_surface_intersection.cpp), so MESH_BOOL_OPS_ATTRIBS interpolates
// attributes that are already on the result -- and there is no hook between
// the copy and the intersection to put ours there. There is also no birth-face
// array of the kind libigl returns.
//
// So the correspondence is recovered geometrically, which is exact rather than
// approximate for this particular problem: every facet of a boolean result
// lies *on* one of the two operand surfaces, so the closest operand face to a
// result face's centroid is the face it came from. The only ambiguity is where
// the two surfaces coincide, and there no answer is more right than the other.
class OperandLookup
{
public:
    // The query point is in world space while the operand is stored in its own
    // local space, so points are pulled back through the inverse layer matrix
    // rather than copying and transforming the whole mesh. Distances are then
    // measured in each operand's local space; that only matters for tie-breaks
    // between two operands scaled differently, where both surfaces are
    // coincident anyway.
    OperandLookup(const Document::MeshEntry &entry)
        : m_mesh(const_cast<VCGMesh &>(entry.mesh))
        , m_mark(m_mesh)
        , m_ioMask(entry.ioMask)
        , m_inverse(entry.transform.inverted())
        , m_maxDist(m_mesh.bbox.Diag())
    {
        m_grid.Set(m_mesh.face.begin(), m_mesh.face.end());
    }

    // Closest face to `worldPoint`, with the barycentric coordinates of the
    // closest point on it. Returns nullptr when nothing is within range.
    const VCGFace *closest(const vcg::Point3f &worldPoint, vcg::Point3f &bary, float &distance) const
    {
        const QVector3D local = m_inverse
            * QVector3D(worldPoint.X(), worldPoint.Y(), worldPoint.Z());
        const vcg::Point3f query(local.x(), local.y(), local.z());

        vcg::tri::FaceTmark<VCGMesh> marker(&m_mesh);
        vcg::face::PointDistanceBaseFunctor<float> distFunctor;
        float dist = m_maxDist;
        vcg::Point3f closestPoint;
        const VCGFace *face = m_grid.GetClosest(distFunctor, marker, query, m_maxDist, dist, closestPoint);
        if (!face)
            return nullptr;
        distance = dist;
        vcg::InterpolationParameters<VCGFace, float>(*face, face->cN(), closestPoint, bary);
        return face;
    }

    int ioMask() const { return m_ioMask; }

private:
    VCGMesh &m_mesh;
    VCGMeshMarkScope m_mark;
    int m_ioMask = 0;
    QMatrix4x4 m_inverse;
    float m_maxDist = 0.0f;
    mutable vcg::GridStaticPtr<VCGFace, float> m_grid;
};

struct TransferOptions
{
    bool faceColor = false;
    bool faceScalar = false;
    bool vertexColor = false;
    bool vertexScalar = false;

    bool any() const { return faceColor || faceScalar || vertexColor || vertexScalar; }
    bool anyFace() const { return faceColor || faceScalar; }
    bool anyVertex() const { return vertexColor || vertexScalar; }
};

vcg::Color4b interpolatedColor(const VCGFace &face, const vcg::Point3f &bary)
{
    float c[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    for (int k = 0; k < 3; ++k) {
        const vcg::Color4b &src = face.cV(k)->cC();
        for (int ch = 0; ch < 4; ++ch)
            c[ch] += bary[k] * float(src[ch]);
    }
    return vcg::Color4b(
        (unsigned char)std::clamp(c[0], 0.0f, 255.0f),
        (unsigned char)std::clamp(c[1], 0.0f, 255.0f),
        (unsigned char)std::clamp(c[2], 0.0f, 255.0f),
        (unsigned char)std::clamp(c[3], 0.0f, 255.0f));
}

void transferAttributes(
    VCGMesh &result,
    int &ioMask,
    const OperandLookup &first,
    const OperandLookup &second,
    const TransferOptions &options)
{
    if (!options.any())
        return;

    using Mask_ = vcg::tri::io::Mask;
    if (options.faceColor) {
        result.face.EnableColor();
        ioMask |= Mask_::IOM_FACECOLOR;
    }
    if (options.faceScalar) {
        result.face.EnableQuality();
        ioMask |= Mask_::IOM_FACEQUALITY;
    }
    if (options.vertexColor)
        ioMask |= Mask_::IOM_VERTCOLOR;
    if (options.vertexScalar)
        ioMask |= Mask_::IOM_VERTQUALITY;

    // Picks whichever operand owns the surface under `point`.
    const auto resolve = [&](const vcg::Point3f &point, vcg::Point3f &bary, const OperandLookup *&owner) {
        vcg::Point3f baryA;
        vcg::Point3f baryB;
        float distA = 0.0f;
        float distB = 0.0f;
        const VCGFace *faceA = first.closest(point, baryA, distA);
        const VCGFace *faceB = second.closest(point, baryB, distB);
        if (faceA && (!faceB || distA <= distB)) {
            bary = baryA;
            owner = &first;
            return faceA;
        }
        if (faceB) {
            bary = baryB;
            owner = &second;
            return faceB;
        }
        owner = nullptr;
        return static_cast<const VCGFace *>(nullptr);
    };

    if (options.anyFace()) {
        for (VCGFace &face : result.face) {
            if (face.IsD())
                continue;
            const vcg::Point3f centroid =
                (face.cV(0)->cP() + face.cV(1)->cP() + face.cV(2)->cP()) / 3.0f;
            vcg::Point3f bary;
            const OperandLookup *owner = nullptr;
            const VCGFace *source = resolve(centroid, bary, owner);
            if (!source)
                continue;
            if (options.faceColor && (owner->ioMask() & Mask_::IOM_FACECOLOR) != 0)
                face.C() = source->cC();
            if (options.faceScalar && (owner->ioMask() & Mask_::IOM_FACEQUALITY) != 0)
                face.Q() = source->cQ();
        }
    }

    if (options.anyVertex()) {
        for (VCGVertex &vertex : result.vert) {
            if (vertex.IsD())
                continue;
            vcg::Point3f bary;
            const OperandLookup *owner = nullptr;
            const VCGFace *source = resolve(vertex.cP(), bary, owner);
            if (!source)
                continue;
            // Barycentric rather than nearest-vertex: a boolean creates
            // vertices along the intersection curve that match no operand
            // vertex at all, and those are exactly the ones a nearest-vertex
            // rule gets visibly wrong.
            if (options.vertexColor && (owner->ioMask() & Mask_::IOM_VERTCOLOR) != 0)
                vertex.C() = interpolatedColor(*source, bary);
            if (options.vertexScalar && (owner->ioMask() & Mask_::IOM_VERTQUALITY) != 0) {
                vertex.Q() = bary[0] * source->cV(0)->cQ()
                    + bary[1] * source->cV(1)->cQ()
                    + bary[2] * source->cV(2)->cQ();
            }
        }
    }
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

    TransferOptions transfer;
    transfer.faceColor = params.getBool(QStringLiteral("transferFaceColor"), false);
    transfer.faceScalar = params.getBool(QStringLiteral("transferFaceScalar"), false);
    transfer.vertexColor = params.getBool(QStringLiteral("transferVertexColor"), false);
    transfer.vertexScalar = params.getBool(QStringLiteral("transferVertexScalar"), false);

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

    int outputMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    if (transfer.any()) {
        if (vcg::CallBackPos *cb = doc.progressCallback())
            (*cb)(95, "Transferring attributes...");
        OperandLookup firstLookup(firstEntry);
        OperandLookup secondLookup(secondEntry);
        transferAttributes(output, outputMask, firstLookup, secondLookup, transfer);
    }

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
