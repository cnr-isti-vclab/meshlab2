#include "meshfixfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <tin.h>
#undef MAX
#undef MIN

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <array>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

constexpr QLatin1StringView kRepairWatertightMesh("repair_watertight_mesh_meshfix");
using Mask = vcg::tri::io::Mask;
using MeshFixMesh = T_MESH::Basic_TMesh;

std::mutex &meshFixMutex()
{
    static std::mutex mutex;
    return mutex;
}

void meshFixMessage(const char *message, int action)
{
    if (action == DISPMSG_ACTION_ERRORDIALOG)
        throw std::runtime_error(message ? message : "Unknown MeshFix error");
}

void initializeMeshFix()
{
    static std::once_flag once;
    std::call_once(once, [] {
        T_MESH::TMesh::init(meshFixMessage);
        T_MESH::TMesh::quiet = true;
    });
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

bool copyToMeshFix(const VCGMesh &source, MeshFixMesh &target,
                   int &skippedFaces, QString &error)
{
    std::vector<std::unique_ptr<T_MESH::ExtVertex>> ownedVertices(source.vert.size());
    std::vector<T_MESH::ExtVertex *> indexedVertices(source.vert.size(), nullptr);

    for (std::size_t i = 0; i < source.vert.size(); ++i) {
        const VCGVertex &vertex = source.vert[i];
        if (vertex.IsD())
            continue;
        const vcg::Point3f &p = vertex.cP();
        if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z())) {
            error = QObject::tr("MeshFix requires finite vertex coordinates.");
            return false;
        }
        T_MESH::Vertex *meshFixVertex = target.newVertex(p.X(), p.Y(), p.Z());
        target.V.appendTail(meshFixVertex);
        ownedVertices[i] = std::make_unique<T_MESH::ExtVertex>(meshFixVertex);
        indexedVertices[i] = ownedVertices[i].get();
    }

    const VCGVertex *vertexBase = source.vert.empty() ? nullptr : &source.vert.front();
    skippedFaces = 0;
    for (const VCGFace &face : source.face) {
        if (face.IsD())
            continue;
        std::array<int, 3> index{};
        bool valid = vertexBase != nullptr;
        for (int corner = 0; corner < 3 && valid; ++corner) {
            const VCGVertex *vertex = face.cV(corner);
            const ptrdiff_t rawIndex = vertex ? vertex - vertexBase : -1;
            valid = rawIndex >= 0
                && std::size_t(rawIndex) < indexedVertices.size()
                && indexedVertices[std::size_t(rawIndex)] != nullptr;
            if (valid)
                index[corner] = int(rawIndex);
        }
        if (!valid || index[0] == index[1] || index[1] == index[2] || index[2] == index[0]
            || !target.CreateIndexedTriangle(
                indexedVertices.data(), index[0], index[1], index[2])) {
            ++skippedFaces;
        }
    }

    if (target.T.numels() == 0) {
        error = QObject::tr("MeshFix received no valid triangles.");
        return false;
    }
    target.fixConnectivity();
    return true;
}

bool copyFromMeshFix(MeshFixMesh &source, VCGMesh &target, QString &error)
{
    std::vector<T_MESH::Vertex *> vertices;
    vertices.reserve(std::size_t(source.V.numels()));
    std::unordered_map<const T_MESH::Vertex *, int> vertexIndex;
    vertexIndex.reserve(std::size_t(source.V.numels()));

    for (T_MESH::Node *node = source.V.head(); node; node = node->next()) {
        auto *vertex = static_cast<T_MESH::Vertex *>(node->data);
        if (!vertex->isLinked())
            continue;
        vertexIndex.emplace(vertex, int(vertices.size()));
        vertices.push_back(vertex);
    }

    std::vector<std::array<int, 3>> triangles;
    triangles.reserve(std::size_t(source.T.numels()));
    for (T_MESH::Node *node = source.T.head(); node; node = node->next()) {
        auto *triangle = static_cast<T_MESH::Triangle *>(node->data);
        if (!triangle->isLinked())
            continue;
        const auto a = vertexIndex.find(triangle->v1());
        const auto b = vertexIndex.find(triangle->v2());
        const auto c = vertexIndex.find(triangle->v3());
        if (a == vertexIndex.end() || b == vertexIndex.end() || c == vertexIndex.end())
            continue;
        triangles.push_back({a->second, b->second, c->second});
    }

    if (vertices.empty() || triangles.empty()) {
        error = QObject::tr("MeshFix produced an empty mesh.");
        return false;
    }

    vcg::tri::Allocator<VCGMesh>::AddVertices(target, int(vertices.size()));
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const T_MESH::Vertex *vertex = vertices[i];
        target.vert[i].P() = vcg::Point3f(
            TMESH_TO_FLOAT(vertex->x),
            TMESH_TO_FLOAT(vertex->y),
            TMESH_TO_FLOAT(vertex->z));
    }

    vcg::tri::Allocator<VCGMesh>::AddFaces(target, int(triangles.size()));
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        for (int corner = 0; corner < 3; ++corner)
            target.face[i].V(corner) = &target.vert[std::size_t(triangles[i][corner])];
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(target);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(target);
    return true;
}

} // namespace

QString MeshFixFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.meshfix");
}

QString MeshFixFilterPlugin::name() const
{
    return QObject::tr("MeshFix Repair Filters");
}

MeshFilterRunResult MeshFixFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    Q_UNUSED(params);
    if (filterId != kRepairWatertightMesh)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
    if (doc.currentMeshIndex() < 0)
        return fail(QObject::tr("No current mesh selected."));

    const Document::MeshEntry &input = doc.mesh(doc.currentMeshIndex());
    if (input.mesh.VN() == 0 || input.mesh.FN() == 0)
        return fail(QObject::tr("MeshFix requires a non-empty triangular mesh."));
    const QString inputName = input.name;
    const QMatrix4x4 inputTransform = input.transform;

    std::lock_guard<std::mutex> lock(meshFixMutex());
    try {
        initializeMeshFix();
        MeshFixMesh repaired;
        int skippedFaces = 0;
        QString error;

        progress(doc, 5, "Converting mesh to MeshFix...");
        if (!copyToMeshFix(input.mesh, repaired, skippedFaces, error))
            return fail(error);

        progress(doc, 20, "Keeping the largest connected component...");
        const int removedComponents = repaired.removeSmallestComponents();

        progress(doc, 35, "Filling holes...");
        const int initialBoundaries = repaired.boundaries();
        const int filledBoundaries =
            initialBoundaries > 0 ? repaired.fillSmallBoundaries(0, true) : 0;

        progress(doc, 60, "Repairing degeneracies and intersections...");
        const bool fullyRepaired = repaired.boundaries() == 0 && repaired.meshclean();

        progress(doc, 90, "Converting MeshFix result...");
        VCGMesh output;
        if (!copyFromMeshFix(repaired, output, error))
            return fail(error);

        const int ioMask =
            Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        const int newIndex = doc.addMesh(
            output, QObject::tr("MeshFix - %1").arg(inputName), ioMask);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to add the MeshFix result to the document."));
        doc.setMeshTransform(newIndex, inputTransform);

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices.push_back(newIndex);
        result.infoMessages
            << QObject::tr("Created mesh '%1'.").arg(doc.mesh(newIndex).name)
            << QObject::tr("Output: %1 vertices, %2 faces.")
                   .arg(output.VN()).arg(output.FN());
        if (removedComponents > 0)
            result.infoMessages << QObject::tr("Removed %1 smaller connected component(s).")
                                       .arg(removedComponents);
        if (initialBoundaries > 0)
            result.infoMessages << QObject::tr("Filled %1 of %2 boundary loop(s).")
                                       .arg(filledBoundaries).arg(initialBoundaries);
        if (skippedFaces > 0)
            result.infoMessages << QObject::tr("Skipped %1 invalid input face(s).")
                                       .arg(skippedFaces);
        if (!fullyRepaired)
            result.infoMessages << QObject::tr(
                "MeshFix could not repair every defect; inspect the output mesh.");
        progress(doc, 100, "MeshFix repair complete.");
        return result;
    } catch (const std::exception &exception) {
        return fail(QObject::tr("MeshFix failed: %1")
                        .arg(QString::fromLocal8Bit(exception.what())));
    } catch (...) {
        return fail(QObject::tr("MeshFix failed with an unknown error."));
    }
}

void registerMeshFixFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<MeshFixFilterPlugin>());
}
