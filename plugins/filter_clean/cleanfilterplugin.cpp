#include "cleanfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/closest.h>
#include <vcg/complex/algorithms/create/ball_pivoting.h>
#include <vcg/complex/algorithms/stat.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/algorithms/update/texture.h>
#include <vcg/space/distance3.h>
#include <vcg/space/index/grid_static_ptr.h>
#include <vcg/space/triangle3.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {
constexpr QLatin1StringView kFilterBallPivoting("reconstruct_surface_by_ball_pivoting_vcglib");
constexpr QLatin1StringView kFilterRemoveWrtQ("remove_vertices_by_scalar");
constexpr QLatin1StringView kFilterRemoveIsolatedComplexity("remove_isolated_components_by_face_count");
constexpr QLatin1StringView kFilterRemoveIsolatedDiameter("remove_isolated_components_by_diameter");
constexpr QLatin1StringView kFilterRemoveTVertex("remove_t_vertices");
constexpr QLatin1StringView kFilterSnapMismatchedBorder("repair_mismatched_borders");
constexpr QLatin1StringView kFilterMergeCloseVertex("merge_close_vertices");
constexpr QLatin1StringView kFilterMergeWedgeTex("merge_close_wedge_uvs");
constexpr QLatin1StringView kFilterRemoveDuplicateFace("remove_duplicate_faces");
constexpr QLatin1StringView kFilterRemoveFoldFace("remove_isolated_folded_faces_by_edge_flip");
constexpr QLatin1StringView kFilterRepairNonManifEdge("repair_non_manifold_edges");
constexpr QLatin1StringView kFilterRemoveNonManifVert("repair_non_manifold_vertices_by_splitting");
constexpr QLatin1StringView kFilterRemoveUnrefVertex("remove_unreferenced_vertices");
constexpr QLatin1StringView kFilterRemoveDuplicatedVertex("remove_duplicate_vertices_vcglib");
constexpr QLatin1StringView kFilterRemoveFaceZeroArea("remove_zero_area_faces");

struct CurrentMeshRef {
    int index = -1;
    Document::MeshEntry *entry = nullptr;
};

std::optional<CurrentMeshRef> currentMesh(Document &doc, QString &errorMessage)
{
    const int index = doc.currentMeshIndex();
    if (index < 0 || index >= doc.meshCount()) {
        errorMessage = QObject::tr("No current mesh selected.");
        return std::nullopt;
    }
    return CurrentMeshRef{ index, &doc.mesh(index) };
}

void compactAndUpdateGeometry(VCGMesh &mesh)
{
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (mesh.FN() > 0)
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

struct SnapBorderResult {
    int splitFaces = 0;
    bool interrupted = false;
};

SnapBorderResult snapMismatchedBorder(
    VCGMesh &mesh,
    const float threshold,
    vcg::CallBackPos *cb)
{
    using Scalar = VCGMesh::ScalarType;
    using Point = vcg::Point3<Scalar>;
    using FacePointer = VCGMesh::FacePointer;
    using MetroMeshFaceGrid = vcg::GridStaticPtr<VCGMesh::FaceType, Scalar>;

    VCGMeshFFAdjScope _ffAdj(mesh);
    VCGMeshMarkScope _mark(mesh);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
    vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);
    vcg::tri::UpdateFlags<VCGMesh>::VertexBorderFromFaceBorder(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);

    MetroMeshFaceGrid unifGridFace;
    vcg::tri::FaceTmark<VCGMesh> markerFunctor(&mesh);
    vcg::face::PointDistanceBaseFunctor<Scalar> pointDistanceFunctor;
    vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);
    unifGridFace.Set(mesh.face.begin(), mesh.face.end());

    const int kClosestCount = 20;
    const float maxDist = mesh.bbox.Diag() / 20.0f;
    std::vector<Point> splitVertices;
    std::vector<FacePointer> splitFaces;
    std::vector<int> splitEdges;

    const int vn = std::max(1, mesh.VN());
    for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
        if (!vi->IsB())
            continue;

        if (cb) {
            const int progress = std::clamp(
                int((int(vcg::tri::Index(mesh, *vi)) * 100) / vn),
                0,
                100);
            if (!(*cb)(progress, "Snapping vertices"))
                return { int(splitVertices.size()), true };
        }

        std::vector<FacePointer> faceVec;
        std::vector<float> distVec;
        std::vector<Point> pointVec;
        const Point startPt = vi->P();
        const int faceFound = unifGridFace.GetKClosest(
            pointDistanceFunctor,
            markerFunctor,
            kClosestCount,
            startPt,
            maxDist,
            faceVec,
            distVec,
            pointVec);

        FacePointer bestFace = nullptr;
        float bestDist = std::numeric_limits<float>::max();
        int bestEdge = -1;
        Point bestPoint = vi->cP();
        for (int i = 0; i < faceFound; ++i) {
            const float epsilonSmall = 1e-5f;
            const float epsilonBig = 1e-2f;
            FacePointer fp = faceVec[static_cast<size_t>(i)];
            Point bary;
            vcg::InterpolationParameters(*fp, fp->cN(), pointVec[static_cast<size_t>(i)], bary);

            for (int j = 0; j < 3; ++j) {
                if (!vcg::face::IsBorder(*fp, j) || fp->IsV())
                    continue;
                if (bary[(j + 0) % 3] <= epsilonBig || bary[(j + 1) % 3] <= epsilonBig)
                    continue;
                if (bary[(j + 2) % 3] >= epsilonSmall)
                    continue;

                const float d = distVec[static_cast<size_t>(i)];
                if (d >= bestDist)
                    continue;
                bestDist = d;
                bestFace = fp;
                bestEdge = j;
                bestPoint = vi->cP();
            }
        }

        if (!bestFace || bestEdge < 0)
            continue;

        const float localThr =
            threshold * vcg::Distance(bestFace->P0(bestEdge), bestFace->P1(bestEdge));
        if (bestDist >= localThr || bestFace->IsV())
            continue;

        bestFace->SetV();
        vi->SetS();
        splitVertices.push_back(bestPoint);
        splitEdges.push_back(bestEdge);
        splitFaces.push_back(bestFace);
    }

    vcg::tri::Allocator<VCGMesh>::PointerUpdater<VCGMesh::FacePointer> pu;
    auto firstVert = vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, splitVertices.size());
    auto firstFace = vcg::tri::Allocator<VCGMesh>::AddFaces(mesh, splitVertices.size(), pu);

    for (size_t i = 0; i < splitVertices.size(); ++i) {
        firstVert->P() = splitVertices[i];
        const int edgeIndex = splitEdges[i];
        FacePointer fp = splitFaces[i];
        pu.Update(fp);

        firstFace->V(0) = &(*firstVert);
        firstFace->V(1) = fp->V2(edgeIndex);
        firstFace->V(2) = fp->V0(edgeIndex);
        fp->V0(edgeIndex) = &(*firstVert);
        ++firstFace;
        ++firstVert;
    }

    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
    return { int(splitVertices.size()), false };
}

}

QString CleanFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.clean");
}

QString CleanFilterPlugin::name() const
{
    return QObject::tr("Repair Filters");
}

MeshFilterRunResult CleanFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    using Mask = vcg::tri::io::Mask;

    QString errorMessage;
    auto current = currentMesh(doc, errorMessage);
    if (!current)
        return { false, false, errorMessage };

    Document::MeshEntry &entry = *current->entry;
    VCGMesh &mesh = entry.mesh;
    vcg::CallBackPos *cb = doc.progressCallback();

    auto interruptedResult = []() -> MeshFilterRunResult {
        return { false, false, QObject::tr("Filter interrupted by user.") };
    };

    auto successResult = [](bool modified, const QStringList &info) {
        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = modified;
        result.infoMessages = info;
        return result;
    };

    if (filterId == QString::fromLatin1(kFilterBallPivoting)) {
        if (mesh.VN() <= 0)
            return { false, false, QObject::tr("Current mesh has no vertices.") };

        const float radius =
            float(params.getDouble(QStringLiteral("ball_radius")));
        const float clustering =
            float(params.getDouble(QStringLiteral("clustering_percent")) / 100.0);
        const float creaseThrDeg =
            float(params.getDouble(QStringLiteral("crease_threshold_deg")));
        const float creaseThr = vcg::math::ToRad(creaseThrDeg);
        const bool deleteFaces = params.getBool(QStringLiteral("delete_initial_faces"));
        if (deleteFaces) {
            mesh.fn = 0;
            mesh.face.clear();
            // inputPrepare built VF adjacency over the faces just dropped, and AdvancingFront
            // both chains its new faces onto the vertices' VF pointers and walks them with a
            // VFIterator. Left stale they point into freed storage and the walk never
            // terminates. Rebuilding over the now-empty face vector nulls them.
            vcg::tri::UpdateTopology<VCGMesh>::VertexFace(mesh);
        }

        const int beforeFaceCount = mesh.FN();
        vcg::tri::BallPivoting<VCGMesh> pivot(mesh, radius, clustering, creaseThr);
        pivot.BuildMesh(cb);
        if (doc.isOperationCancelRequested())
            return interruptedResult();

        compactAndUpdateGeometry(mesh);
        entry.ioMask |= Mask::IOM_FACENORMAL | Mask::IOM_VERTNORMAL;
        doc.markMeshGeometryChanged(
            current->index,
            QObject::tr("Ball-pivoting reconstruction on '%1'").arg(entry.name));
        return successResult(
            true,
            {
                QObject::tr("Reconstructed surface. Added %1 faces.")
                    .arg(mesh.FN() - beforeFaceCount)
            });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveWrtQ)) {
        if (mesh.VN() <= 0)
            return { false, false, QObject::tr("Current mesh has no vertices.") };

        const float threshold =
            float(params.getDouble(QStringLiteral("max_quality_thr")));
        int deletedVertices = 0;
        int deletedFaces = 0;
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            if (vi->Q() < threshold) {
                vcg::tri::Allocator<VCGMesh>::DeleteVertex(mesh, *vi);
                ++deletedVertices;
            }
        }
        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (fi->V(0)->IsD() || fi->V(1)->IsD() || fi->V(2)->IsD()) {
                vcg::tri::Allocator<VCGMesh>::DeleteFace(mesh, *fi);
                ++deletedFaces;
            }
        }

        if (deletedVertices > 0 || deletedFaces > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed low-quality vertices from '%1'").arg(entry.name));
        }

        return successResult(
            deletedVertices > 0 || deletedFaces > 0,
            {
                QObject::tr(
                    "Deleted %1 vertices and %2 faces with quality lower than %3.")
                    .arg(deletedVertices)
                    .arg(deletedFaces)
                    .arg(QString::number(threshold, 'f', 6))
            });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveIsolatedDiameter)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const float minComponentDiag =
            float(params.getDouble(QStringLiteral("min_component_diag")));
        const auto delInfo =
            vcg::tri::Clean<VCGMesh>::RemoveSmallConnectedComponentsDiameter(mesh, minComponentDiag);

        const bool modified = delInfo.second > 0;
        if (modified) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed isolated components by diameter on '%1'").arg(entry.name));
        }
        return successResult(
            modified,
            {
                QObject::tr("Removed %1 connected components out of %2.")
                    .arg(delInfo.second)
                    .arg(delInfo.first)
            });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveIsolatedComplexity)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const int minComponentSize =
            std::max(0, params.getInt(QStringLiteral("min_component_size")));
        const auto delInfo =
            vcg::tri::Clean<VCGMesh>::RemoveSmallConnectedComponentsSize(mesh, minComponentSize);

        const bool modified = delInfo.second > 0;
        if (modified) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed isolated components by face count on '%1'").arg(entry.name));
        }
        return successResult(
            modified,
            {
                QObject::tr("Removed %1 connected components out of %2.")
                    .arg(delInfo.second)
                    .arg(delInfo.first)
            });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveTVertex)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };

        const QString method = params.getEnum(QStringLiteral("method"));
        const float threshold = float(params.getDouble(QStringLiteral("threshold")));
        const bool repeat = params.getBool(QStringLiteral("repeat"));

        int total = 0;
        if (method == QStringLiteral("edge_flip")) {
            if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0
                || vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(mesh) > 0) {
                return { false, false, QObject::tr("Non manifold mesh. Please clean the mesh first.") };
            }
            VCGMeshFFAdjScope _ffAdj(mesh);
            total = vcg::tri::Clean<VCGMesh>::RemoveTVertexByFlip(mesh, threshold, repeat);
        } else {
            total = vcg::tri::Clean<VCGMesh>::RemoveTVertexByCollapse(mesh, threshold, repeat);
        }

        if (total > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed T-vertices on '%1'").arg(entry.name));
        }
        return successResult(
            total > 0,
            { QObject::tr("Successfully removed %1 t-vertices.").arg(total) });
    }

    if (filterId == QString::fromLatin1(kFilterSnapMismatchedBorder)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const float edgeDistRatio =
            float(params.getDouble(QStringLiteral("edge_dist_ratio")));
        const bool unifyVertices = params.getBool(QStringLiteral("unify_vertices"));

        const SnapBorderResult snapResult = snapMismatchedBorder(mesh, edgeDistRatio, cb);
        if (snapResult.interrupted || doc.isOperationCancelRequested())
            return interruptedResult();

        int mergedVertices = 0;
        if (unifyVertices)
            mergedVertices = vcg::tri::Clean<VCGMesh>::MergeCloseVertex(mesh, 0.0f);

        const bool modified = (snapResult.splitFaces > 0) || (mergedVertices > 0);
        if (modified) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Snapped mismatched borders on '%1'").arg(entry.name));
        }

        return successResult(
            modified,
            {
                QObject::tr("Successfully split %1 faces to snap borders.").arg(snapResult.splitFaces),
                QObject::tr("Merged %1 exact-duplicate vertices after snapping.").arg(mergedVertices)
            });
    }

    if (filterId == QString::fromLatin1(kFilterMergeCloseVertex)) {
        if (mesh.VN() <= 0)
            return { false, false, QObject::tr("Current mesh has no vertices.") };
        const float threshold = float(params.getDouble(QStringLiteral("threshold")));
        const int total = vcg::tri::Clean<VCGMesh>::MergeCloseVertex(mesh, threshold);
        if (total > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Merged close vertices on '%1'").arg(entry.name));
        }
        return successResult(
            total > 0,
            { QObject::tr("Successfully merged %1 vertices.").arg(total) });
    }

    if (filterId == QString::fromLatin1(kFilterMergeWedgeTex)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const float mergeThr = float(params.getDouble(QStringLiteral("merge_thr")));
        const int total = vcg::tri::UpdateTexture<VCGMesh>::WedgeTexMergeClose(mesh, mergeThr);
        entry.ioMask |= Mask::IOM_WEDGTEXCOORD;
        if (total > 0) {
            doc.markMeshMaterialChanged(
                current->index,
                QObject::tr("Merged wedge texture coordinates on '%1'").arg(entry.name));
        }
        return successResult(
            total > 0,
            {
                QObject::tr("Successfully merged %1 wedge tex coords with threshold %2.")
                    .arg(total)
                    .arg(QString::number(mergeThr, 'f', 8))
            });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveDuplicateFace)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const int total = vcg::tri::Clean<VCGMesh>::RemoveDuplicateFace(mesh);
        if (total > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed duplicate faces on '%1'").arg(entry.name));
        }
        return successResult(
            total > 0,
            { QObject::tr("Successfully deleted %1 duplicated faces.").arg(total) });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveFoldFace)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        if (entry.polygonFaceCount >= 0
            || (entry.ioMask & Mask::IOM_BITPOLYGONAL) != 0) {
            return {
                false,
                false,
                QObject::tr("This filter requires a triangle mesh; convert polygonal faces to triangles first.")
            };
        }
        if (!mesh.face.empty() && mesh.face.front().IsWedgeTexCoordEnabled()) {
            return {
                false,
                false,
                QObject::tr(
                    "This filter cannot safely preserve per-wedge texture seams. "
                    "Use a mesh without wedge texture coordinates.")
            };
        }
        if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh, false) > 0
            || vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(mesh, false, false) > 0) {
            return { false, false, QObject::tr("This filter requires a 2-manifold mesh.") };
        }
        if (!vcg::tri::Clean<VCGMesh>::IsCoherentlyOrientedMesh(mesh)) {
            return {
                false,
                false,
                QObject::tr("This filter requires consistently oriented faces.")
            };
        }

        const float threshold = float(params.getDouble(QStringLiteral("normal_threshold_deg")));
        // inputPrepare already built FF adjacency, so avoid a second O(F log F) rebuild.
        const int total = vcg::tri::Clean<VCGMesh>::RemoveFaceFoldByFlip(
            mesh, threshold, true, false);
        if (total > 0) {
            compactAndUpdateGeometry(mesh);
            entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Repaired folded faces on '%1'").arg(entry.name));
        }
        return successResult(
            total > 0,
            { QObject::tr("Successfully repaired %1 folded faces by edge flip.").arg(total) });
    }

    if (filterId == QString::fromLatin1(kFilterRepairNonManifEdge)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const QString method = params.getEnum(QStringLiteral("method"));
        int total = 0;
        QString info;
        if (method == QStringLiteral("split_vertices")) {
            return {
                false,
                false,
                QObject::tr(
                    "Split Vertices mode is not supported by the current mesh container type. "
                    "Use 'Remove Faces' method instead.")
            };
        } else {
            total = vcg::tri::Clean<VCGMesh>::RemoveNonManifoldFace(mesh);
            info = QObject::tr("Successfully removed %1 non-manifold faces.").arg(total);
        }

        if (total > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Repaired non-manifold edges on '%1'").arg(entry.name));
        }
        return successResult(total > 0, { info });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveNonManifVert)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const float vertDispRatio = float(params.getDouble(QStringLiteral("vert_disp_ratio")));
        const int total = vcg::tri::Clean<VCGMesh>::SplitNonManifoldVertex(mesh, vertDispRatio);
        if (total > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Repaired non-manifold vertices on '%1'").arg(entry.name));
        }
        return successResult(
            total > 0,
            { QObject::tr("Successfully split %1 non-manifold vertices.").arg(total) });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveUnrefVertex)) {
        if (mesh.VN() <= 0)
            return { false, false, QObject::tr("Current mesh has no vertices.") };
        const int delVert = vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
        if (delVert > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed unreferenced vertices on '%1'").arg(entry.name));
        }
        return successResult(
            delVert > 0,
            { QObject::tr("Removed %1 unreferenced vertices.").arg(delVert) });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveDuplicatedVertex)) {
        if (mesh.VN() <= 0)
            return { false, false, QObject::tr("Current mesh has no vertices.") };
        const int delVert = vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(mesh);
        if (delVert > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed duplicated vertices on '%1'").arg(entry.name));
        }
        return successResult(
            delVert > 0,
            { QObject::tr("Removed %1 duplicated vertices.").arg(delVert) });
    }

    if (filterId == QString::fromLatin1(kFilterRemoveFaceZeroArea)) {
        if (mesh.FN() <= 0)
            return { false, false, QObject::tr("Current mesh has no faces.") };
        const int nullFaces = vcg::tri::Clean<VCGMesh>::RemoveFaceOutOfRangeArea(mesh, 0.0f);
        if (nullFaces > 0) {
            compactAndUpdateGeometry(mesh);
            doc.markMeshGeometryChanged(
                current->index,
                QObject::tr("Removed zero-area faces on '%1'").arg(entry.name));
        }
        return successResult(
            nullFaces > 0,
            { QObject::tr("Removed %1 null faces.").arg(nullFaces) });
    }

    return { false, false, QObject::tr("Unknown filter id: %1").arg(filterId) };
}

void registerCleanFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<CleanFilterPlugin>());
}
