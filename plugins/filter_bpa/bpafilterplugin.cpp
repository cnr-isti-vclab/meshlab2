#include "bpafilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include "upstream/bpa.h"

#include <QObject>
#include <QStringList>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <wrap/io_trimesh/io_mask.h>

namespace {

constexpr QLatin1StringView kFilterBallPivotingGruber(
    "reconstruct_surface_by_ball_pivoting_gruber");
using Mask = vcg::tri::io::Mask;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = message;
    return result;
}

// The reconstruction hands back loose triangles -- three bare positions each, no indices.
// Welding those by proximity would lose the correspondence to the input points, which is the
// one property that makes ball pivoting worth using, so instead each returned position is
// looked up against the points that went in. That is exact rather than approximate: the
// algorithm copies positions through and never computes new ones, so the bits come back
// unchanged.
struct PositionKey
{
    float x = 0.0f, y = 0.0f, z = 0.0f;

    bool operator==(const PositionKey &other) const
    {
        return std::memcmp(this, &other, sizeof(PositionKey)) == 0;
    }
};

struct PositionHash
{
    std::size_t operator()(const PositionKey &key) const
    {
        std::uint32_t bits[3];
        std::memcpy(bits, &key.x, sizeof(bits));
        std::size_t h = 1469598103934665603ull;
        for (std::uint32_t part : bits) {
            h ^= std::size_t(part);
            h *= 1099511628211ull;
        }
        return h;
    }
};

PositionKey keyOf(const glm::vec3 &p) { return PositionKey{p.x, p.y, p.z}; }

} // namespace

QString BpaFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.bpa");
}

QString BpaFilterPlugin::name() const
{
    return QStringLiteral("Ball Pivoting (Gruber)");
}

MeshFilterRunResult BpaFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId != QString::fromLatin1(kFilterBallPivotingGruber))
        return fail(QObject::tr("Unknown filter: %1").arg(filterId));

    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(index);
    VCGMesh &mesh = entry.mesh;
    if (mesh.VN() <= 0)
        return fail(QObject::tr("Current mesh has no vertices."));

    const double radius = params.getDouble(QStringLiteral("ballRadius"));
    if (radius <= 0.0)
        return fail(QObject::tr(
            "The pivoting ball radius must be greater than zero. This implementation has no "
            "radius guess of its own; use Reconstruct Surface by Ball Pivoting (vcglib) if "
            "you want one."));
    const bool deleteFaces = params.getBool(QStringLiteral("deleteInitialFaces"), true);

    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Ball Pivoting (Gruber)"));

    std::vector<bpa::Point> points;
    std::vector<int> vertexOfPoint;
    points.reserve(std::size_t(mesh.VN()));
    vertexOfPoint.reserve(std::size_t(mesh.VN()));
    for (int i = 0; i < int(mesh.vert.size()); ++i) {
        const VCGVertex &v = mesh.vert[std::size_t(i)];
        if (v.IsD())
            continue;
        const auto &p = v.cP();
        const auto &n = v.cN();
        points.push_back({glm::vec3(float(p.X()), float(p.Y()), float(p.Z())),
                          glm::vec3(float(n.X()), float(n.Y()), float(n.Z()))});
        vertexOfPoint.push_back(i);
    }
    // Oriented normals are not optional here: the pivot direction is derived from them, and
    // a cloud without them silently reconstructs nothing.
    bool anyNormal = false;
    for (const bpa::Point &p : points)
        anyNormal = anyNormal || glm::dot(p.normal, p.normal) > 1e-12f;
    if (!anyNormal) {
        doc.finishFilterProgress(false, QObject::tr("The points carry no normals."));
        return fail(QObject::tr(
            "The points carry no normals. Ball pivoting needs oriented normals to know which "
            "side of the cloud the surface is on: run Compute Normals for Point Sets first."));
    }

    if (points.size() < 3) {
        doc.finishFilterProgress(false, QObject::tr("Not enough points."));
        return fail(QObject::tr("Ball pivoting needs at least three points."));
    }

    const std::vector<bpa::Triangle> triangles = bpa::reconstruct(points, float(radius));
    if (doc.isOperationCancelRequested()) {
        doc.finishFilterProgress(false, QObject::tr("Cancelled."));
        return fail(QObject::tr("Cancelled."));
    }
    if (triangles.empty()) {
        doc.finishFilterProgress(false, QObject::tr("Nothing was reconstructed."));
        return fail(QObject::tr(
            "No seed triangle was found, so nothing was reconstructed. The ball has to fit "
            "through three neighbouring points without enclosing a fourth: try a radius "
            "closer to the spacing of the cloud, and check the points carry normals."));
    }

    std::unordered_map<PositionKey, int, PositionHash> vertexAt;
    vertexAt.reserve(points.size() * 2);
    for (std::size_t i = 0; i < points.size(); ++i)
        vertexAt[keyOf(points[i].pos)] = vertexOfPoint[i];

    if (deleteFaces) {
        mesh.face.clear();
        mesh.fn = 0;
    }
    const int beforeFaceCount = mesh.FN();

    int unresolved = 0;
    for (const bpa::Triangle &t : triangles) {
        int corner[3];
        bool resolved = true;
        for (int k = 0; k < 3; ++k) {
            const auto found = vertexAt.find(keyOf(t[std::size_t(k)]));
            if (found == vertexAt.end()) {
                resolved = false;
                break;
            }
            corner[k] = found->second;
        }
        if (!resolved) {
            ++unresolved;
            continue;
        }
        VCGMesh::FaceIterator fi = vcg::tri::Allocator<VCGMesh>::AddFaces(mesh, 1);
        for (int k = 0; k < 3; ++k)
            fi->V(k) = &mesh.vert[std::size_t(corner[k])];
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (mesh.FN() > 0)
        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
    entry.ioMask |= Mask::IOM_FACENORMAL | Mask::IOM_VERTNORMAL;
    doc.markMeshGeometryChanged(
        index,
        QObject::tr("Ball-pivoting reconstruction (Gruber) on '%1'").arg(entry.name));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Reconstructed surface. Added %1 faces over %2 points.")
               .arg(mesh.FN() - beforeFaceCount).arg(points.size());
    // One seed triangle, and the front stops where it stops: anything the ball could not
    // reach from there is simply not covered. That is the algorithm as published, and the
    // main thing that separates this from the vcglib implementation.
    if (mesh.FN() - beforeFaceCount > 0 && int(points.size()) > 3 * (mesh.FN() - beforeFaceCount))
        result.infoMessages << QObject::tr(
            "Most of the cloud was left untouched. This implementation grows from a single "
            "seed triangle, so a cloud in several pieces only ever gets one of them; "
            "Reconstruct Surface by Ball Pivoting (vcglib) re-seeds and covers them all.");
    if (unresolved > 0)
        result.infoMessages << QObject::tr(
            "%1 triangles named positions that were not in the input and were dropped.")
                                   .arg(unresolved);
    doc.finishFilterProgress(true, QObject::tr("Reconstructed the surface."));
    return result;
}

void registerBpaFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<BpaFilterPlugin>());
}
