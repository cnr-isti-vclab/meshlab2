#include "qslimfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <stdmix.h>
#include <MxQSlim.h>
#include <mixmsg.h>

#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace {

constexpr QLatin1StringView kQSlimFilter("simplify_by_quadric_edge_collapse_qslim");
using Mask = vcg::tri::io::Mask;

std::mutex &qslimMutex()
{
    static std::mutex mutex;
    return mutex;
}

bool qslimMessage(MxMsgInfo *info)
{
    if (info && info->severity <= MXMSG_ERROR)
        throw std::runtime_error(info->message ? info->message : "Unknown QSlim error");
    return true;
}

void initializeQSlim()
{
    static std::once_flag once;
    std::call_once(once, [] { mxmsg_set_handler(qslimMessage); });
}

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.errorMessage = message;
    return result;
}

void progress(Document &doc, int percentage, const char *message)
{
    if (vcg::CallBackPos *callback = doc.progressCallback())
        (*callback)(percentage, message);
}

bool copyToQSlim(const VCGMesh &source, MxStdModel &target,
                 int &skippedFaces, QString &error)
{
    std::vector<MxVertexID> vertexIds(source.vert.size(), MXID_NIL);
    for (std::size_t i = 0; i < source.vert.size(); ++i) {
        const VCGVertex &vertex = source.vert[i];
        if (vertex.IsD())
            continue;
        const vcg::Point3f &p = vertex.cP();
        if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z())) {
            error = QObject::tr("QSlim requires finite vertex coordinates.");
            return false;
        }
        vertexIds[i] = target.add_vertex(p.X(), p.Y(), p.Z());
    }

    const VCGVertex *vertexBase = source.vert.empty() ? nullptr : &source.vert.front();
    skippedFaces = 0;
    for (const VCGFace &face : source.face) {
        if (face.IsD())
            continue;
        std::array<MxVertexID, 3> index{};
        bool valid = vertexBase != nullptr;
        for (int corner = 0; corner < 3 && valid; ++corner) {
            const VCGVertex *vertex = face.cV(corner);
            const ptrdiff_t rawIndex = vertex ? vertex - vertexBase : -1;
            valid = rawIndex >= 0 && std::size_t(rawIndex) < vertexIds.size()
                && vertexIds[std::size_t(rawIndex)] != MXID_NIL;
            if (valid)
                index[corner] = vertexIds[std::size_t(rawIndex)];
        }
        if (!valid || index[0] == index[1] || index[1] == index[2]
            || index[2] == index[0]) {
            ++skippedFaces;
            continue;
        }
        target.add_face(index[0], index[1], index[2]);
    }

    if (target.face_count() == 0) {
        error = QObject::tr("QSlim received no valid triangles.");
        return false;
    }
    return true;
}

bool copyFromQSlim(MxStdModel &source, VCGMesh &target, QString &error)
{
    std::vector<int> vertexMap(source.vert_count(), -1);
    int vertexCount = 0;
    for (MxVertexID id = 0; id < source.vert_count(); ++id)
        if (source.vertex_is_valid(id))
            vertexMap[id] = vertexCount++;

    std::vector<std::array<int, 3>> faces;
    faces.reserve(source.face_count());
    for (MxFaceID id = 0; id < source.face_count(); ++id) {
        if (!source.face_is_valid(id))
            continue;
        const MxFace &face = source.face(id);
        const std::array<int, 3> indices = {
            vertexMap[face[0]], vertexMap[face[1]], vertexMap[face[2]]
        };
        if (indices[0] < 0 || indices[1] < 0 || indices[2] < 0
            || indices[0] == indices[1] || indices[1] == indices[2]
            || indices[2] == indices[0])
            continue;
        faces.push_back(indices);
    }

    if (vertexCount == 0 || faces.empty()) {
        error = QObject::tr("QSlim produced an empty mesh.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::AddVertices(target, vertexCount);
    for (MxVertexID id = 0; id < source.vert_count(); ++id) {
        const int outputIndex = vertexMap[id];
        if (outputIndex < 0)
            continue;
        const MxVertex &vertex = source.vertex(id);
        target.vert[std::size_t(outputIndex)].P() =
            vcg::Point3f(vertex[0], vertex[1], vertex[2]);
    }

    vcg::tri::Allocator<VCGMesh>::AddFaces(target, int(faces.size()));
    for (std::size_t i = 0; i < faces.size(); ++i)
        for (int corner = 0; corner < 3; ++corner)
            target.face[i].V(corner) = &target.vert[std::size_t(faces[i][corner])];

    vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(target);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(target);
    vcg::tri::UpdateBounding<VCGMesh>::Box(target);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(target);
    return true;
}

int placementPolicy(const QString &value)
{
    if (value == QStringLiteral("endpoints"))
        return MX_PLACE_ENDPOINTS;
    if (value == QStringLiteral("end_or_midpoint"))
        return MX_PLACE_ENDORMID;
    if (value == QStringLiteral("line"))
        return MX_PLACE_LINE;
    return MX_PLACE_OPTIMAL;
}

int weightingPolicy(const QString &value)
{
    if (value == QStringLiteral("uniform"))
        return MX_WEIGHT_UNIFORM;
    if (value == QStringLiteral("angle"))
        return MX_WEIGHT_ANGLE;
    return MX_WEIGHT_AREA;
}

} // namespace

QString QSlimFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.qslim");
}

QString QSlimFilterPlugin::name() const
{
    return QObject::tr("QSlim Simplification Filters");
}

MeshFilterRunResult QSlimFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId != kQSlimFilter)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
    if (doc.currentMeshIndex() < 0)
        return fail(QObject::tr("No current mesh selected."));

    const Document::MeshEntry &input = doc.mesh(doc.currentMeshIndex());
    if (input.mesh.VN() == 0 || input.mesh.FN() == 0)
        return fail(QObject::tr("QSlim requires a non-empty triangular mesh."));
    const QString inputName = input.name;
    const QMatrix4x4 inputTransform = input.transform;
    const int targetFaces = std::clamp(
        params.getInt(QStringLiteral("TargetFaceNum")), 1, input.mesh.FN());

    std::lock_guard<std::mutex> lock(qslimMutex());
    try {
        initializeQSlim();
        MxStdModel model(
            unsigned(input.mesh.VN()), unsigned(input.mesh.FN()));
        int skippedFaces = 0;
        QString error;

        progress(doc, 5, "Converting mesh to QSlim...");
        if (!copyToQSlim(input.mesh, model, skippedFaces, error))
            return fail(error);

        progress(doc, 20, "Initializing original QSlim...");
        MxEdgeQSlim simplifier(model);
        simplifier.placement_policy =
            placementPolicy(params.getEnum(QStringLiteral("PlacementPolicy")));
        simplifier.weighting_policy =
            weightingPolicy(params.getEnum(QStringLiteral("WeightingPolicy")));
        simplifier.boundary_weight =
            params.getDouble(QStringLiteral("BoundaryWeight"));
        simplifier.compactness_ratio =
            params.getDouble(QStringLiteral("CompactnessRatio"));
        simplifier.meshing_penalty =
            params.getDouble(QStringLiteral("MeshingPenalty"));
        simplifier.initialize();

        progress(doc, 35, "Simplifying with original QSlim...");
        const bool reachedTarget = simplifier.decimate(unsigned(targetFaces));

        progress(doc, 90, "Converting QSlim result...");
        VCGMesh output;
        if (!copyFromQSlim(model, output, error))
            return fail(error);

        const int ioMask =
            Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        const int newIndex = doc.addMesh(
            output, QObject::tr("QSlim - %1").arg(inputName), ioMask);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to add the QSlim result to the document."));
        doc.setMeshTransform(newIndex, inputTransform);

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices.push_back(newIndex);
        result.infoMessages
            << QObject::tr("Simplified with the original QSlim implementation.")
            << QObject::tr("Output: %1 vertices, %2 faces.")
                   .arg(output.VN()).arg(output.FN());
        if (!reachedTarget)
            result.infoMessages << QObject::tr(
                "QSlim stopped at %1 faces before reaching the requested %2.")
                                       .arg(output.FN()).arg(targetFaces);
        if (skippedFaces > 0)
            result.infoMessages << QObject::tr("Skipped %1 invalid input face(s).")
                                       .arg(skippedFaces);
        progress(doc, 100, "QSlim simplification complete.");
        return result;
    } catch (const std::exception &exception) {
        return fail(QObject::tr("QSlim failed: %1")
                        .arg(QString::fromLocal8Bit(exception.what())));
    } catch (...) {
        return fail(QObject::tr("QSlim failed with an unknown error."));
    }
}

void registerQSlimFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<QSlimFilterPlugin>());
}
