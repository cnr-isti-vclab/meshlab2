#include "instantmeshes_adapter.h"

// Qt's emit macro conflicts with a oneTBB member function.
#ifdef emit
#undef emit
#endif

#include "upstream/src/adjacency.h"
#include "upstream/src/bvh.h"
#include "upstream/src/dedge.h"
#include "upstream/src/extract.h"
#include "upstream/src/field.h"
#include "upstream/src/hierarchy.h"
#include "upstream/src/meshstats.h"
#include "upstream/src/normal.h"
#include "upstream/src/subdivide.h"

#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/allocate.h>

#include <tbb/global_control.h>

#include <QObject>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <set>
#include <vector>

namespace {

bool copyInput(const VCGMesh &mesh, MatrixXu &faces, MatrixXf &vertices, QString &error)
{
    std::vector<uint32_t> vertexMap(mesh.vert.size(), INVALID);
    uint32_t vertexCount = 0;
    for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
        const VCGVertex &vertex = mesh.vert[i];
        if (vertex.IsD())
            continue;
        const vcg::Point3f &p = vertex.cP();
        if (!std::isfinite(p.X()) || !std::isfinite(p.Y()) || !std::isfinite(p.Z())) {
            error = QObject::tr("Instant Meshes requires finite vertex coordinates.");
            return false;
        }
        vertexMap[i] = vertexCount++;
    }

    const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    std::vector<std::array<uint32_t, 3>> validFaces;
    validFaces.reserve(std::size_t(mesh.FN()));
    for (const VCGFace &face : mesh.face) {
        if (face.IsD())
            continue;
        std::array<uint32_t, 3> indices { INVALID, INVALID, INVALID };
        for (int corner = 0; corner < 3; ++corner) {
            const ptrdiff_t index = base && face.cV(corner) ? face.cV(corner) - base : -1;
            if (index < 0 || std::size_t(index) >= vertexMap.size())
                break;
            indices[std::size_t(corner)] = vertexMap[std::size_t(index)];
        }
        if (indices[0] == INVALID || indices[1] == INVALID || indices[2] == INVALID
            || indices[0] == indices[1] || indices[1] == indices[2]
            || indices[2] == indices[0])
            continue;
        validFaces.push_back(indices);
    }
    if (vertexCount == 0 || validFaces.empty()) {
        error = QObject::tr("Instant Meshes requires a non-empty triangle mesh.");
        return false;
    }

    vertices.resize(3, vertexCount);
    for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
        const uint32_t target = vertexMap[i];
        if (target == INVALID)
            continue;
        const vcg::Point3f &p = mesh.vert[i].cP();
        vertices.col(target) << p.X(), p.Y(), p.Z();
    }
    faces.resize(3, Eigen::Index(validFaces.size()));
    for (std::size_t i = 0; i < validFaces.size(); ++i)
        for (int corner = 0; corner < 3; ++corner)
            faces(corner, Eigen::Index(i)) = validFaces[i][std::size_t(corner)];
    return true;
}

bool copyOutput(const MatrixXu &faces, const MatrixXf &vertices, VCGMesh &mesh, QString &error)
{
    if (vertices.cols() == 0 || faces.cols() == 0 || (faces.rows() != 3 && faces.rows() != 4)) {
        error = QObject::tr("Instant Meshes produced an empty or unsupported mesh.");
        return false;
    }
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, int(vertices.cols()));
    for (Eigen::Index i = 0; i < vertices.cols(); ++i) {
        const Vector3f p = vertices.col(i);
        if (!p.allFinite()) {
            error = QObject::tr("Instant Meshes produced non-finite vertex coordinates.");
            return false;
        }
        mesh.vert[std::size_t(i)].P() = vcg::Point3f(p.x(), p.y(), p.z());
    }

    for (Eigen::Index i = 0; i < faces.cols(); ++i) {
        const uint32_t a = faces(0, i), b = faces(1, i), c = faces(2, i);
        const uint32_t d = faces.rows() == 4 ? faces(3, i) : c;
        if (a >= uint32_t(vertices.cols()) || b >= uint32_t(vertices.cols())
            || c >= uint32_t(vertices.cols()) || d >= uint32_t(vertices.cols())) {
            error = QObject::tr("Instant Meshes produced an invalid face index.");
            return false;
        }
        if (a == b || b == c || c == a)
            continue;
        VCGFace *first = &*vcg::tri::Allocator<VCGMesh>::AddFaces(mesh, 1);
        first->V(0) = &mesh.vert[a];
        first->V(1) = &mesh.vert[b];
        first->V(2) = &mesh.vert[c];
        if (d == c)
            continue;
        if (c == d || d == a || d == b)
            continue;
        first->SetF(2);
        VCGFace *second = &*vcg::tri::Allocator<VCGMesh>::AddFaces(mesh, 1);
        second->V(0) = &mesh.vert[a];
        second->V(1) = &mesh.vert[c];
        second->V(2) = &mesh.vert[d];
        second->SetF(0);
    }
    if (mesh.FN() == 0) {
        error = QObject::tr("Instant Meshes produced no valid faces.");
        return false;
    }
    vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    {
        VCGMeshFFAdjScope adjacencyScope(mesh);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerBitPolygonFaceNormalized(mesh);
    }
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
    return true;
}

} // namespace

bool runInstantMeshes(
    const VCGMesh &input,
    VCGMesh &output,
    const InstantMeshesParameters &p,
    QString &error)
{
    if (!(p.targetEdgeLength > 0.0f) || !std::isfinite(p.targetEdgeLength)
        || p.smoothingIterations < 0 || p.threads < 0) {
        error = QObject::tr("Invalid Instant Meshes parameters.");
        return false;
    }

    std::unique_ptr<tbb::global_control> threadLimit;
    if (p.threads > 0)
        threadLimit = std::make_unique<tbb::global_control>(
            tbb::global_control::max_allowed_parallelism, std::size_t(p.threads));

    MatrixXu faces;
    MatrixXf vertices, normals;
    if (!copyInput(input, faces, vertices, error))
        return false;

    const MeshStats stats = compute_mesh_stats(faces, vertices, p.deterministic);
    VectorXu vertexToEdge, edgeToEdge;
    VectorXb boundary, nonManifold;
    if (stats.mMaximumEdgeLength * 2 > p.targetEdgeLength
        || stats.mMaximumEdgeLength > stats.mAverageEdgeLength * 2) {
        build_dedge(faces, vertices, vertexToEdge, edgeToEdge, boundary, nonManifold);
        subdivide(faces, vertices, vertexToEdge, edgeToEdge, boundary, nonManifold,
            std::min(p.targetEdgeLength / 2, Float(stats.mAverageEdgeLength * 2)),
            p.deterministic);
    }

    build_dedge(faces, vertices, vertexToEdge, edgeToEdge, boundary, nonManifold);
    AdjacencyMatrix adjacency =
        generate_adjacency_matrix_uniform(faces, vertexToEdge, edgeToEdge, nonManifold);

    std::set<uint32_t> creaseIn, creaseOut;
    if (p.creaseAngleDegrees >= 0.0f)
        generate_crease_normals(faces, vertices, vertexToEdge, edgeToEdge,
            boundary, nonManifold, p.creaseAngleDegrees, normals, creaseIn);
    else
        generate_smooth_normals(
            faces, vertices, vertexToEdge, edgeToEdge, nonManifold, normals);

    VectorXf areas;
    compute_dual_vertex_areas(
        faces, vertices, vertexToEdge, edgeToEdge, nonManifold, areas);

    MultiResolutionHierarchy hierarchy;
    hierarchy.setE2E(std::move(edgeToEdge));
    hierarchy.setAdj(std::move(adjacency));
    hierarchy.setF(std::move(faces));
    hierarchy.setV(std::move(vertices));
    hierarchy.setA(std::move(areas));
    hierarchy.setN(std::move(normals));
    hierarchy.setScale(p.targetEdgeLength);
    hierarchy.build(p.deterministic);
    hierarchy.resetSolution();

    if (p.alignBoundaries) {
        hierarchy.clearConstraints();
        for (uint32_t i = 0; i < uint32_t(3 * hierarchy.F().cols()); ++i) {
            if (hierarchy.E2E()[i] != INVALID)
                continue;
            const uint32_t i0 = hierarchy.F()(i % 3, i / 3);
            const uint32_t i1 = hierarchy.F()((i + 1) % 3, i / 3);
            Vector3f edge = hierarchy.V().col(i1) - hierarchy.V().col(i0);
            if (edge.squaredNorm() == 0.0f)
                continue;
            edge.normalize();
            hierarchy.CO().col(i0) = hierarchy.V().col(i0);
            hierarchy.CO().col(i1) = hierarchy.V().col(i1);
            hierarchy.CQ().col(i0) = hierarchy.CQ().col(i1) = edge;
            hierarchy.CQw()[i0] = hierarchy.CQw()[i1] = 1.0f;
            hierarchy.COw()[i0] = hierarchy.COw()[i1] = 1.0f;
        }
        hierarchy.propagateConstraints(4, 4);
    }

    std::unique_ptr<BVH> bvh;
    if (p.smoothingIterations > 0) {
        bvh = std::make_unique<BVH>(
            &hierarchy.F(), &hierarchy.V(), &hierarchy.N(), stats.mAABB);
        bvh->build();
    }

    Optimizer optimizer(hierarchy, false);
    optimizer.setRoSy(4);
    optimizer.setPoSy(4);
    optimizer.setExtrinsic(p.extrinsic);
    optimizer.optimizeOrientations(-1);
    optimizer.notify();
    optimizer.wait();
    optimizer.optimizePositions(-1);
    optimizer.notify();
    optimizer.wait();
    optimizer.shutdown();

    MatrixXf outputVertices, outputNormals, outputFaceNormals;
    std::vector<std::vector<TaggedLink>> outputAdjacency;
    extract_graph(hierarchy, p.extrinsic, 4, 4, outputAdjacency,
        outputVertices, outputNormals, creaseIn, creaseOut, p.deterministic);
    MatrixXu outputFaces;
    extract_faces(outputAdjacency, outputVertices, outputNormals, outputFaceNormals,
        outputFaces, 4, hierarchy.scale(), creaseOut, true, p.pureQuads,
        bvh.get(), p.smoothingIterations);
    return copyOutput(outputFaces, outputVertices, output, error);
}
