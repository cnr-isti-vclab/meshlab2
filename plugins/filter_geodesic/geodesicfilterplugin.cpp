#include "geodesicfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/geodesic.h>
#include <vcg/complex/algorithms/geodesic_heat.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <limits>

namespace {
constexpr QLatin1StringView kFilterBorderGeodesic("compute_geodesic_distance_from_border");
constexpr QLatin1StringView kFilterPointGeodesic("compute_geodesic_distance_from_point");
constexpr QLatin1StringView kFilterSelectedGeodesic("compute_geodesic_distance_from_selection_vcglib");
constexpr QLatin1StringView kFilterHeatGeodesic("compute_heat_geodesic_distance_from_selection_vcglib");
constexpr QLatin1StringView kFilterGeodesicPath("create_polyline_from_geodesic_path");

VCGMesh::VertexPointer closestVertex(VCGMesh &mesh, const QVector3D &p)
{
    const vcg::Point3f sp(p.x(), p.y(), p.z());
    VCGMesh::VertexPointer best = nullptr;
    float minDistSq = std::numeric_limits<float>::max();
    for (auto &v : mesh.vert) {
        if (v.IsD()) continue;
        const float dsq = vcg::SquaredDistance(sp, v.P());
        if (dsq < minDistSq) { minDistSq = dsq; best = &v; }
    }
    return best;
}

MeshFilterRunResult vertexQualityResult(int meshIndex, const QString &message)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = { message };
    result.visualizationHints.push_back({
        meshIndex,
        MeshFilterVisualizationAttribute::VertexQuality
    });
    return result;
}
} // namespace

QString GeodesicFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.geodesic");
}

QString GeodesicFilterPlugin::name() const
{
    return QObject::tr("Geodesic Filters");
}

MeshFilterRunResult GeodesicFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return { false, false, QObject::tr("No current mesh selected.") };

    using Mask = vcg::tri::io::Mask;
    VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    const QString &meshName = doc.mesh(meshIndex).name;

    if (filterId == QString::fromLatin1(kFilterBorderGeodesic)) {
        const bool hasBorder = vcg::tri::Geodesic<VCGMesh>::DistanceFromBorder(mesh);
        if (!hasBorder) {
            return { true, false, QObject::tr("Mesh '%1' has no borders — no geodesic distance computed.").arg(meshName) };
        }

        // Zero out unreached vertices (quality == max float)
        int unreachedCnt = 0;
        const float unreached = std::numeric_limits<float>::max();
        for (auto &v : mesh.vert)
            if (v.Q() == unreached) { v.Q() = 0.f; ++unreachedCnt; }

        doc.mesh(meshIndex).ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(meshIndex,
            QObject::tr("Geodesic distance from border on '%1'").arg(meshName));

        QString msg = QObject::tr("Geodesic distance from border computed on '%1'.").arg(meshName);
        if (unreachedCnt > 0)
            msg += QObject::tr(" Warning: %1 vertices were unreachable (isolated components).").arg(unreachedCnt);
        return vertexQualityResult(meshIndex, msg);
    }

    if (filterId == QString::fromLatin1(kFilterPointGeodesic)) {
        const QVector3D startPt = params.getPoint3f(QStringLiteral("startPoint"));
        const float maxDist = float(params.getDouble(QStringLiteral("maxDistance")));

        // Find the closest vertex to the given point
        const vcg::Point3f sp(startPt.x(), startPt.y(), startPt.z());
        VCGMesh::VertexPointer startVertex = nullptr;
        float minDistSq = std::numeric_limits<float>::max();
        for (auto &v : mesh.vert) {
            const float dsq = vcg::SquaredDistance(sp, v.P());
            if (dsq < minDistSq) { minDistSq = dsq; startVertex = &v; }
        }
        if (!startVertex)
            return { false, false, QObject::tr("Mesh '%1' has no vertices.").arg(meshName) };

        vcg::tri::EuclideanDistance<VCGMesh> dd;
        vcg::tri::Geodesic<VCGMesh>::Compute(
            mesh, std::vector<VCGMesh::VertexPointer>(1, startVertex), dd,
            maxDist > 0.f ? maxDist : std::numeric_limits<float>::max());

        // Zero out unreached vertices
        int unreachedCnt = 0;
        const float unreached = std::numeric_limits<float>::max();
        for (auto &v : mesh.vert)
            if (v.Q() == unreached) { v.Q() = 0.f; ++unreachedCnt; }

        doc.mesh(meshIndex).ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(meshIndex,
            QObject::tr("Geodesic distance from point on '%1'").arg(meshName));

        QString msg = QObject::tr("Geodesic distance from point computed on '%1'.").arg(meshName);
        if (unreachedCnt > 0)
            msg += QObject::tr(" Warning: %1 vertices were unreachable.").arg(unreachedCnt);
        return vertexQualityResult(meshIndex, msg);
    }

    if (filterId == QString::fromLatin1(kFilterSelectedGeodesic)) {
        const float maxDist = float(params.getDouble(QStringLiteral("maxDistance")));

        std::vector<VCGMesh::VertexPointer> seedVec;
        for (auto &v : mesh.vert)
            if (v.IsS()) seedVec.push_back(&v);

        if (seedVec.empty())
            return { false, false, QObject::tr("No vertices are selected — aborting geodesic computation.") };

        vcg::tri::EuclideanDistance<VCGMesh> dd;
        vcg::tri::Geodesic<VCGMesh>::Compute(
            mesh, seedVec, dd,
            maxDist > 0.f ? maxDist : std::numeric_limits<float>::max());

        // Zero out unreached vertices
        int unreachedCnt = 0;
        const float unreached = std::numeric_limits<float>::max();
        for (auto &v : mesh.vert)
            if (v.Q() == unreached) { v.Q() = 0.f; ++unreachedCnt; }

        doc.mesh(meshIndex).ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(meshIndex,
            QObject::tr("Geodesic distance from selection on '%1'").arg(meshName));

        QString msg = QObject::tr("Geodesic distance from %1 selected vertices computed on '%2'.")
            .arg(seedVec.size()).arg(meshName);
        if (unreachedCnt > 0)
            msg += QObject::tr(" Warning: %1 vertices were unreachable.").arg(unreachedCnt);
        return vertexQualityResult(meshIndex, msg);
    }

    if (filterId == QString::fromLatin1(kFilterGeodesicPath)) {
        VCGMesh::VertexPointer startVertex = closestVertex(mesh, params.getPoint3f(QStringLiteral("startPoint")));
        VCGMesh::VertexPointer endVertex = closestVertex(mesh, params.getPoint3f(QStringLiteral("endPoint")));
        if (!startVertex)
            return { false, false, QObject::tr("Mesh '%1' has no vertices.").arg(meshName) };
        if (startVertex == endVertex)
            return { false, false, QObject::tr("Both points are closest to the same vertex: choose two points farther apart.") };

        // The distance field from the end vertex, with its shortest-path tree: following the
        // parents from the start vertex walks back along mesh edges to the end vertex.
        // Geodesic::Compute writes the distances into the vertex quality, which belongs to
        // the user: keep it.
        std::vector<float> savedQuality;
        savedQuality.reserve(mesh.vert.size());
        for (const auto &v : mesh.vert) savedQuality.push_back(v.cQ());
        auto parent = vcg::tri::Allocator<VCGMesh>::AddPerVertexAttribute<VCGMesh::VertexPointer>(mesh);
        vcg::tri::EuclideanDistance<VCGMesh> dd;
        vcg::tri::Geodesic<VCGMesh>::Compute(mesh, std::vector<VCGMesh::VertexPointer>(1, endVertex), dd,
                                             std::numeric_limits<float>::max(), nullptr, nullptr, &parent);
        const bool reached = startVertex->cQ() < std::numeric_limits<float>::max();
        const double estimate = startVertex->cQ();
        std::vector<vcg::Point3f> path;
        if (reached)
            for (VCGMesh::VertexPointer v = startVertex; ; v = parent[v]) {
                path.push_back(v->cP());
                if (v == endVertex || path.size() > mesh.vert.size()) break;
            }
        vcg::tri::Allocator<VCGMesh>::DeletePerVertexAttribute(mesh, parent);
        for (size_t i = 0; i < mesh.vert.size(); ++i) mesh.vert[i].Q() = savedQuality[i];
        if (!reached)
            return { false, false, QObject::tr("The two points lie on different connected components of '%1'.").arg(meshName) };

        VCGMesh poly;
        vcg::tri::Allocator<VCGMesh>::AddVertices(poly, int(path.size()));
        vcg::tri::Allocator<VCGMesh>::AddEdges(poly, int(path.size()) - 1);
        for (size_t i = 0; i < path.size(); ++i) poly.vert[i].P() = path[i];
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            poly.edge[i].V(0) = &poly.vert[i];
            poly.edge[i].V(1) = &poly.vert[i + 1];
        }
        vcg::tri::UpdateBounding<VCGMesh>::Box(poly);
        double length = 0;
        for (size_t i = 0; i + 1 < path.size(); ++i) length += vcg::Distance(path[i], path[i + 1]);
        const int idx = doc.addMesh(poly, {}, Mask::IOM_EDGEINDEX);
        if (idx < 0)
            return { false, false, QObject::tr("Failed to create the geodesic path layer.") };
        doc.setMeshTransform(idx, doc.mesh(meshIndex).transform);

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices = { idx };
        result.infoMessages = { QObject::tr("Geodesic path: %1 edges, length %2 (geodesic distance estimate %3).")
                                    .arg(poly.EN()).arg(length).arg(estimate) };
        result.outputValues["edges"] = poly.EN();
        result.outputValues["length"] = length;
        result.outputValues["geodesicDistance"] = estimate;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterHeatGeodesic)) {
        const float m = float(params.getDouble(QStringLiteral("m")));

        std::vector<VCGMesh::VertexPointer> seedVec;
        for (auto &v : mesh.vert)
            if (v.IsS()) seedVec.push_back(&v);

        if (seedVec.empty())
            return { false, false, QObject::tr("No vertices are selected — aborting heat geodesic computation.") };

        VCGMeshVFAdjScope vfScope(mesh);
        VCGMeshFFAdjScope ffScope(mesh);
        const bool ok = vcg::tri::GeodesicHeat<VCGMesh>::Compute(mesh, seedVec, m);
        if (!ok)
            return { false, false,
                QObject::tr("Heat geodesic failed on '%1'. The mesh may be badly conditioned (near-degenerate triangles) or have disconnected components.").arg(meshName) };

        doc.mesh(meshIndex).ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(meshIndex,
            QObject::tr("Heat geodesic distance from selection on '%1'").arg(meshName));

        return vertexQualityResult(
            meshIndex,
            QObject::tr("Heat geodesic distance from %1 selected vertices computed on '%2'.")
                .arg(seedVec.size()).arg(meshName));
    }

    return { false, false, QObject::tr("Unknown filter id: %1").arg(filterId) };
}

void registerGeodesicFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<GeodesicFilterPlugin>());
}
