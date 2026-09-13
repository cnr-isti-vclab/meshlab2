#include "measurefilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"
#include <vcg/complex/algorithms/bitquad_support.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/inertia.h>
#include <vcg/complex/algorithms/mesh_to_matrix.h>
#include <vcg/complex/algorithms/stat.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/append.h>
#include <Eigen/Eigenvalues>
#include <QMatrix4x4>
#include <QVector4D>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace {
constexpr QLatin1StringView kFilterTopo("measure_topological_properties");
constexpr QLatin1StringView kFilterTopoQuad("measure_topological_properties_for_quad_mesh");
constexpr QLatin1StringView kFilterGeom("measure_geometric_properties");
constexpr QLatin1StringView kFilterSelectionArea("measure_selection_area_and_perimeter");
constexpr QLatin1StringView kFilterVertStat("measure_vertex_scalar_statistics");
constexpr QLatin1StringView kFilterFaceStat("measure_face_scalar_statistics");
constexpr QLatin1StringView kFilterVertHist("measure_vertex_scalar_histogram");
constexpr QLatin1StringView kFilterFaceHist("measure_face_scalar_histogram");

using Histogramf = vcg::Histogram<float>;
using Distributionf = vcg::Distribution<float>;

bool isIdentityTransform(const QMatrix4x4 &matrix, float eps = 1e-6f)
{
    QMatrix4x4 identity;
    identity.setToIdentity();
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (std::abs(matrix(r, c) - identity(r, c)) > eps)
                return false;
        }
    }
    return true;
}

template<typename Scalar>
vcg::Point3<Scalar> qMatrixMapPoint(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &point)
{
    const QVector4D mapped = matrix * QVector4D(point.X(), point.Y(), point.Z(), 1.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

QString formatFloat(double value, int decimals = 6)
{
    return QString::number(value, 'f', decimals);
}

QString formatPoint(const vcg::Point3f &p, int decimals = 6)
{
    return QObject::tr("(%1, %2, %3)")
        .arg(formatFloat(p.X(), decimals))
        .arg(formatFloat(p.Y(), decimals))
        .arg(formatFloat(p.Z(), decimals));
}

QString formatMatrixRow(const vcg::Matrix33f &m, int row, int decimals = 6)
{
    return QObject::tr("| %1 %2 %3 |")
        .arg(formatFloat(m[row][0], decimals), 12)
        .arg(formatFloat(m[row][1], decimals), 12)
        .arg(formatFloat(m[row][2], decimals), 12);
}

struct SavedSelection
{
    std::vector<bool> faceSelected;
    std::vector<bool> vertexSelected;
};

SavedSelection saveSelectionBits(const VCGMesh &mesh)
{
    SavedSelection saved;
    saved.faceSelected.reserve(mesh.face.size());
    for (const VCGFace &f : mesh.face)
        saved.faceSelected.push_back(f.IsS());
    saved.vertexSelected.reserve(mesh.vert.size());
    for (const VCGVertex &v : mesh.vert)
        saved.vertexSelected.push_back(v.IsS());
    return saved;
}

void restoreSelectionBits(VCGMesh &mesh, const SavedSelection &saved)
{
    vcg::tri::UpdateSelection<VCGMesh>::VertexClear(mesh);
    vcg::tri::UpdateSelection<VCGMesh>::FaceClear(mesh);
    for (size_t i = 0; i < saved.faceSelected.size() && i < mesh.face.size(); ++i) {
        if (saved.faceSelected[i])
            mesh.face[i].SetS();
    }
    for (size_t i = 0; i < saved.vertexSelected.size() && i < mesh.vert.size(); ++i) {
        if (saved.vertexSelected[i])
            mesh.vert[i].SetS();
    }
}

std::unique_ptr<VCGMesh> transformedCopy(const Document::MeshEntry &entry)
{
    auto copy = std::make_unique<VCGMesh>();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(*copy, entry.mesh);
    const QMatrix4x4 transform = entry.transform;
    if (!isIdentityTransform(transform)) {
        for (VCGVertex &v : copy->vert) {
            v.P() = qMatrixMapPoint<float>(transform, v.cP());
        }
        vcg::tri::UpdateBounding<VCGMesh>::Box(*copy);
    }
    return copy;
}

vcg::Matrix33f computePrincipalAxisCloud(const VCGMesh &mesh)
{
    vcg::Matrix33f cov;
    vcg::Point3f bary(0.0f, 0.0f, 0.0f);
    std::vector<vcg::Point3f> points;
    points.reserve(size_t(mesh.VN()));
    for (const VCGVertex &v : mesh.vert) {
        points.push_back(v.cP());
        bary += v.cP();
    }
    if (points.empty()) {
        vcg::Matrix33f identity;
        identity.SetIdentity();
        return identity;
    }
    bary /= float(points.size());
    cov.Covariance(points, bary);

    Eigen::Matrix3d em;
    cov.ToEigenMatrix(em);
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> solver(em);
    vcg::Matrix33f eigenVecMatrix;
    eigenVecMatrix.FromEigenMatrix(solver.eigenvectors());
    return eigenVecMatrix;
}

MeshFilterRunResult successInfo(const QStringList &info)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = false;
    result.infoMessages = info;
    return result;
}

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

QString histogramCountString(double value, bool areaWeighted)
{
    return areaWeighted ? formatFloat(value, 7) : QString::number(std::llround(value));
}

} // namespace

QString MeasureFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.measure");
}

QString MeasureFilterPlugin::name() const
{
    return QObject::tr("Measurement Filters");
}

MeshFilterRunResult MeasureFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    const QString meshName = entry.name;

    if (filterId == QString::fromLatin1(kFilterTopo)) {
        const SavedSelection saved = saveSelectionBits(mesh);

        const int edgeNonManifFFNum = vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh, true);
        const int faceEdgeManif = vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh);
        vcg::tri::UpdateSelection<VCGMesh>::VertexClear(mesh);
        vcg::tri::UpdateSelection<VCGMesh>::FaceClear(mesh);

        const int vertManifNum = vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(mesh, true);
        vcg::tri::UpdateSelection<VCGMesh>::FaceFromVertexLoose(mesh);
        const int faceVertManif = vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh);

        int edgeNum = 0, edgeBorderNum = 0, edgeNonManifNum = 0;
        vcg::tri::Clean<VCGMesh>::CountEdgeNum(mesh, edgeNum, edgeBorderNum, edgeNonManifNum);
        restoreSelectionBits(mesh, saved);

        const int unrefVertNum = vcg::tri::Clean<VCGMesh>::CountUnreferencedVertex(mesh);
        const int connectedComponentsNum = vcg::tri::Clean<VCGMesh>::CountConnectedComponents(mesh);
        const bool isTwoManifold = edgeNonManifFFNum == 0 && vertManifNum == 0;

        QStringList info;
        info << QObject::tr("Mesh: %1").arg(meshName);
        info << QObject::tr("V: %1  E: %2  F: %3").arg(mesh.VN()).arg(edgeNum).arg(mesh.FN());
        info << QObject::tr("Unreferenced vertices: %1").arg(unrefVertNum);
        info << QObject::tr("Boundary edges: %1").arg(edgeBorderNum);
        info << QObject::tr("Connected components: %1").arg(connectedComponentsNum);
        info << QObject::tr("Two-manifold: %1").arg(isTwoManifold ? QObject::tr("yes") : QObject::tr("no"));
        info << QObject::tr("Non two-manifold edges: %1 (incident faces: %2)")
                    .arg(edgeNonManifFFNum)
                    .arg(faceEdgeManif);
        info << QObject::tr("Non two-manifold vertices: %1 (incident faces: %2)")
                    .arg(vertManifNum)
                    .arg(faceVertManif);

        int holeNum = -1, genus = -1;
        if (vertManifNum == 0 && edgeNonManifFFNum == 0) {
            holeNum = vcg::tri::Clean<VCGMesh>::CountHoles(mesh);
            genus = vcg::tri::Clean<VCGMesh>::MeshGenus(
                mesh.VN() - unrefVertNum,
                edgeNum,
                mesh.FN(),
                holeNum,
                connectedComponentsNum);
            info << QObject::tr("Holes: %1").arg(holeNum);
            info << QObject::tr("Genus: %1").arg(genus);
        } else {
            info << QObject::tr("Holes: undefined (non 2-manifold mesh)");
            info << QObject::tr("Genus: undefined (non 2-manifold mesh)");
        }
        if (edgeNonManifFFNum != edgeNonManifNum) {
            info << QObject::tr("Warning: non-manifold edge counters disagreed (%1 vs %2).")
                        .arg(edgeNonManifFFNum)
                        .arg(edgeNonManifNum);
        }
        MeshFilterRunResult result = successInfo(info);
        result.outputValues["vertices_number"] = mesh.VN();
        result.outputValues["edges_number"] = edgeNum;
        result.outputValues["faces_number"] = mesh.FN();
        result.outputValues["unreferenced_vertices"] = unrefVertNum;
        result.outputValues["boundary_edges"] = edgeBorderNum;
        result.outputValues["connected_components_number"] = connectedComponentsNum;
        result.outputValues["is_mesh_two_manifold"] = isTwoManifold;
        result.outputValues["non_two_manifold_edges"] = edgeNonManifFFNum;
        result.outputValues["incident_faces_on_non_two_manifold_edges"] = faceEdgeManif;
        result.outputValues["non_two_manifold_vertices"] = vertManifNum;
        result.outputValues["incident_faces_on_non_two_manifold_vertices"] = faceVertManif;
        result.outputValues["number_holes"] = (vertManifNum == 0 && edgeNonManifFFNum == 0) ? holeNum : -1;
        result.outputValues["genus"] = (vertManifNum == 0 && edgeNonManifFFNum == 0) ? genus : -1;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterTopoQuad)) {
        if (!vcg::tri::Clean<VCGMesh>::IsFFAdjacencyConsistent(mesh))
            return fail(QObject::tr("Error: mesh has inconsistent face-face adjacency."));
        if (!vcg::tri::Clean<VCGMesh>::HasConsistentPerFaceFauxFlag(mesh))
            return fail(QObject::tr("Quad mesh problem: mesh has inconsistent faux-edge tagging."));

        int nQuads = vcg::tri::Clean<VCGMesh>::CountBitQuads(mesh);
        const int nTris = vcg::tri::Clean<VCGMesh>::CountBitTris(mesh);
        const int nPolys = vcg::tri::Clean<VCGMesh>::CountBitPolygons(mesh);
        const int nLargePolys = vcg::tri::Clean<VCGMesh>::CountBitLargePolygons(mesh);
        if (nLargePolys > 0)
            nQuads = 0;

        if (!vcg::tri::Clean<VCGMesh>::IsBitTriQuadOnly(mesh))
            return fail(QObject::tr("Quad mesh problem: the mesh is not TriQuadOnly."));

        vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);
        Distributionf angleDist;
        Distributionf ratioDist;
        for (VCGMesh::FaceIterator fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (fi->IsV())
                continue;
            fi->SetV();
            vcg::Point3f qv[4];
            bool quadFound = false;
            for (int i = 0; i < 3; ++i) {
                if (fi->IsF(i) && !fi->IsF((i + 1) % 3) && !fi->IsF((i + 2) % 3)) {
                    qv[0] = fi->V0(i)->P();
                    qv[1] = fi->FFp(i)->V2(fi->FFi(i))->P();
                    qv[2] = fi->V1(i)->P();
                    qv[3] = fi->V2(i)->P();
                    quadFound = true;
                    break;
                }
            }
            if (!quadFound)
                return fail(QObject::tr("Quad mesh problem: current mesh does not contain valid quads."));

            for (int i = 0; i < 4; ++i) {
                const float discrepancy = std::abs(90.0f - vcg::math::ToDeg(vcg::Angle(
                    qv[(i + 0) % 4] - qv[(i + 1) % 4],
                    qv[(i + 2) % 4] - qv[(i + 1) % 4])));
                angleDist.Add(discrepancy);
            }

            float edgeLen[4];
            for (int i = 0; i < 4; ++i)
                edgeLen[i] = vcg::Distance(qv[(i + 0) % 4], qv[(i + 1) % 4]);
            std::sort(edgeLen, edgeLen + 4);
            if (edgeLen[3] > 0.0f)
                ratioDist.Add(edgeLen[0] / edgeLen[3]);
        }

        QStringList info;
        info << QObject::tr("Mesh: %1").arg(meshName);
        info << QObject::tr("Triangles: %1").arg(nTris);
        info << QObject::tr("Quads: %1").arg(nQuads);
        info << QObject::tr("Polygons: %1").arg(nPolys);
        info << QObject::tr("Large polygons (with internal faux vertices): %1").arg(nLargePolys);
        info << QObject::tr("Right angle discrepancy: avg %1, min %2, max %3, stddev %4, perc 5%% %5, perc 95%% %6")
                    .arg(formatFloat(angleDist.Avg(), 3))
                    .arg(formatFloat(angleDist.Min(), 3))
                    .arg(formatFloat(angleDist.Max(), 3))
                    .arg(formatFloat(angleDist.StandardDeviation(), 3))
                    .arg(formatFloat(angleDist.Percentile(0.05f), 3))
                    .arg(formatFloat(angleDist.Percentile(0.95f), 3));
        info << QObject::tr("Quad ratio: avg %1, min %2, max %3")
                    .arg(formatFloat(ratioDist.Avg(), 3))
                    .arg(formatFloat(ratioDist.Min(), 3))
                    .arg(formatFloat(ratioDist.Max(), 3));
        MeshFilterRunResult result = successInfo(info);
        result.outputValues["triangles_number"] = nTris;
        result.outputValues["quads_number"] = nQuads;
        result.outputValues["polys_number"] = nPolys;
        result.outputValues["large_polys_number"] = nLargePolys;
        result.outputValues["right_angle_discrepancy_avg"] = angleDist.Avg();
        result.outputValues["right_angle_discrepancy_min"] = angleDist.Min();
        result.outputValues["right_angle_discrepancy_max"] = angleDist.Max();
        result.outputValues["right_angle_discrepancy_stddev"] = angleDist.StandardDeviation();
        result.outputValues["right_angle_discrepancy_perc0.05"] = angleDist.Percentile(0.05f);
        result.outputValues["right_angle_discrepancy_perc95"] = angleDist.Percentile(0.95f);
        result.outputValues["quad_ratio_avg"] = ratioDist.Avg();
        result.outputValues["quad_ratio_min"] = ratioDist.Min();
        result.outputValues["quad_ratio_max"] = ratioDist.Max();
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterGeom)) {
        std::unique_ptr<VCGMesh> measureMesh = transformedCopy(entry);
        const bool pointcloud = (measureMesh->FN() == 0) && (measureMesh->VN() != 0);
        QStringList info;
        info << QObject::tr("Mesh: %1").arg(meshName);
        if (!isIdentityTransform(entry.transform))
            info << QObject::tr("Beware: measures are computed considering the current transformation matrix.");

        const vcg::Box3f &bbox = measureMesh->bbox;
        info << QObject::tr("Bounding box size: %1  %2  %3")
                    .arg(formatFloat(bbox.DimX()))
                    .arg(formatFloat(bbox.DimY()))
                    .arg(formatFloat(bbox.DimZ()));
        info << QObject::tr("Bounding box diagonal: %1").arg(formatFloat(bbox.Diag()));
        info << QObject::tr("Bounding box min: %1").arg(formatPoint(bbox.min));
        info << QObject::tr("Bounding box max: %1").arg(formatPoint(bbox.max));

        if (pointcloud) {
            vcg::Point3f bc = vcg::tri::Stat<VCGMesh>::ComputeCloudBarycenter(*measureMesh, false);
            info << QObject::tr("Pointcloud barycenter: %1").arg(formatPoint(bc));
            if (vcg::tri::HasPerVertexQuality(*measureMesh)) {
                bc = vcg::tri::Stat<VCGMesh>::ComputeCloudBarycenter(*measureMesh, true);
                info << QObject::tr("Quality-weighted pointcloud barycenter: %1").arg(formatPoint(bc));
            }
            const vcg::Matrix33f pca = computePrincipalAxisCloud(*measureMesh);
            info << QObject::tr("Principal axes:");
            info << formatMatrixRow(pca, 0);
            info << formatMatrixRow(pca, 1);
            info << formatMatrixRow(pca, 2);
        }

        const float area = vcg::tri::Stat<VCGMesh>::ComputeMeshArea(*measureMesh);
        info << QObject::tr("Surface area: %1").arg(formatFloat(area));

        Distributionf eDist;
        vcg::tri::Stat<VCGMesh>::ComputeFaceEdgeLengthDistribution(*measureMesh, eDist, false);
        info << QObject::tr("Total edge length: %1 over %2 edges (avg %3)")
                    .arg(formatFloat(eDist.Sum()))
                    .arg(int(eDist.Cnt()))
                    .arg(formatFloat(eDist.Avg()));
        vcg::tri::Stat<VCGMesh>::ComputeFaceEdgeLengthDistribution(*measureMesh, eDist, true);
        info << QObject::tr("Total edge length including faux edges: %1 over %2 edges (avg %3)")
                    .arg(formatFloat(eDist.Sum()))
                    .arg(int(eDist.Cnt()))
                    .arg(formatFloat(eDist.Avg()));

        vcg::Point3f bc = vcg::tri::Stat<VCGMesh>::ComputeShellBarycenter(*measureMesh);
        info << QObject::tr("Thin shell barycenter: %1").arg(formatPoint(bc));
        bc = vcg::tri::Stat<VCGMesh>::ComputeCloudBarycenter(*measureMesh, false);
        info << QObject::tr("Vertex barycenter: %1").arg(formatPoint(bc));

        int edgeNum = 0, edgeBorderNum = 0, edgeNonManifNum = 0;
        vcg::tri::Clean<VCGMesh>::CountEdgeNum(*measureMesh, edgeNum, edgeBorderNum, edgeNonManifNum);
        const bool watertight = (edgeBorderNum == 0) && (edgeNonManifNum == 0);
        if (watertight) {
            vcg::tri::Inertia<VCGMesh> inertia(*measureMesh);
            info << QObject::tr("Mesh volume: %1").arg(formatFloat(inertia.Mass()));
            info << QObject::tr("Center of mass: %1").arg(formatPoint(inertia.CenterOfMass()));
            vcg::Matrix33f inertiaTensor;
            inertia.InertiaTensor(inertiaTensor);
            info << QObject::tr("Inertia tensor:");
            info << formatMatrixRow(inertiaTensor, 0);
            info << formatMatrixRow(inertiaTensor, 1);
            info << formatMatrixRow(inertiaTensor, 2);
            vcg::Matrix33f pca;
            vcg::Point3f axisMomenta;
            inertia.InertiaTensorEigen(pca, axisMomenta);
            info << QObject::tr("Principal axes:");
            info << formatMatrixRow(pca, 0);
            info << formatMatrixRow(pca, 1);
            info << formatMatrixRow(pca, 2);
            info << QObject::tr("Axis momenta: %1").arg(formatPoint(axisMomenta));
        } else {
            info << QObject::tr("Mesh is not watertight; volume, center of mass, and inertia tensor are undefined.");
            const vcg::Matrix33f pca = computePrincipalAxisCloud(*measureMesh);
            info << QObject::tr("Principal axes:");
            info << formatMatrixRow(pca, 0);
            info << formatMatrixRow(pca, 1);
            info << formatMatrixRow(pca, 2);
        }
        MeshFilterRunResult result = successInfo(info);
        result.outputValues["bbox_dim_x"] = double(bbox.DimX());
        result.outputValues["bbox_dim_y"] = double(bbox.DimY());
        result.outputValues["bbox_dim_z"] = double(bbox.DimZ());
        result.outputValues["bbox_diagonal"] = double(bbox.Diag());
        result.outputValues["bbox_min_x"] = double(bbox.min.X());
        result.outputValues["bbox_min_y"] = double(bbox.min.Y());
        result.outputValues["bbox_min_z"] = double(bbox.min.Z());
        result.outputValues["bbox_max_x"] = double(bbox.max.X());
        result.outputValues["bbox_max_y"] = double(bbox.max.Y());
        result.outputValues["bbox_max_z"] = double(bbox.max.Z());
        result.outputValues["is_pointcloud"] = pointcloud;
        if (!pointcloud) {
            result.outputValues["surface_area"] = area;
            result.outputValues["is_watertight"] = watertight;
        }
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterSelectionArea)) {
        const int selectedFaceCount = vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh);
        if (selectedFaceCount == 0)
            return fail(QObject::tr("Cannot apply: there is no face selection."));

        const QMatrix4x4 transform = entry.transform;
        QStringList info;
        info << QObject::tr("Mesh: %1").arg(meshName);
        info << QObject::tr("Selected triangles: %1").arg(selectedFaceCount);
        if (!isIdentityTransform(transform))
            info << QObject::tr("Beware: area and perimeter are computed considering the current transformation matrix.");

        double selectedArea = 0.0;
        for (VCGMesh::FaceIterator fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (!fi->IsS())
                continue;
            const vcg::Point3f p0 = qMatrixMapPoint<float>(transform, fi->cP(0));
            const vcg::Point3f p1 = qMatrixMapPoint<float>(transform, fi->cP(1));
            const vcg::Point3f p2 = qMatrixMapPoint<float>(transform, fi->cP(2));
            selectedArea += double(((p1 - p0) ^ (p2 - p0)).Norm()) * 0.5;
        }
        info << QObject::tr("Selected surface area: %1").arg(formatFloat(selectedArea));

        int borderEdges = 0;
        double perimeter = 0.0;
        for (VCGMesh::FaceIterator fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (!fi->IsS())
                continue;
            for (int ei = 0; ei < 3; ++ei) {
                VCGMesh::FacePointer adjf = fi->FFp(ei);
                if (adjf == &(*fi) || !adjf || !adjf->IsS()) {
                    ++borderEdges;
                    const vcg::Point3f p0 = qMatrixMapPoint<float>(transform, fi->V(ei)->P());
                    const vcg::Point3f p1 = qMatrixMapPoint<float>(transform, fi->V((ei + 1) % 3)->P());
                    perimeter += double((p0 - p1).Norm());
                }
            }
        }
        info << QObject::tr("Border edges: %1").arg(borderEdges);
        info << QObject::tr("Perimeter: %1").arg(formatFloat(perimeter));
        MeshFilterRunResult result = successInfo(info);
        result.outputValues["selected_triangles_number"] = selectedFaceCount;
        result.outputValues["selected_surface_area"] = selectedArea;
        result.outputValues["border_edge_number"] = borderEdges;
        result.outputValues["perimeter"] = perimeter;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertStat)) {
        Distributionf dist;
        vcg::tri::Stat<VCGMesh>::ComputePerVertexQualityDistribution(mesh, dist, false);
        QStringList info;
        info << QObject::tr("Mesh: %1").arg(meshName);
        info << QObject::tr("Per-vertex quality min/max: %1 / %2")
                    .arg(formatFloat(dist.Min()))
                    .arg(formatFloat(dist.Max()));
        info << QObject::tr("Per-vertex quality avg/med: %1 / %2")
                    .arg(formatFloat(dist.Avg()))
                    .arg(formatFloat(dist.Percentile(0.5f)));
        info << QObject::tr("Per-vertex quality stddev: %1").arg(formatFloat(dist.StandardDeviation()));
        info << QObject::tr("Per-vertex quality variance: %1").arg(formatFloat(dist.Variance()));
        MeshFilterRunResult result = successInfo(info);
        result.outputValues["vertex_quality_min"] = dist.Min();
        result.outputValues["vertex_quality_max"] = dist.Max();
        result.outputValues["vertex_quality_avg"] = dist.Avg();
        result.outputValues["vertex_quality_median"] = dist.Percentile(0.5f);
        result.outputValues["vertex_quality_stddev"] = dist.StandardDeviation();
        result.outputValues["vertex_quality_variance"] = dist.Variance();
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterFaceStat)) {
        Distributionf dist;
        vcg::tri::Stat<VCGMesh>::ComputePerFaceQualityDistribution(mesh, dist, false);
        QStringList info;
        info << QObject::tr("Mesh: %1").arg(meshName);
        info << QObject::tr("Per-face quality min/max: %1 / %2")
                    .arg(formatFloat(dist.Min()))
                    .arg(formatFloat(dist.Max()));
        info << QObject::tr("Per-face quality avg/med: %1 / %2")
                    .arg(formatFloat(dist.Avg()))
                    .arg(formatFloat(dist.Percentile(0.5f)));
        info << QObject::tr("Per-face quality stddev: %1").arg(formatFloat(dist.StandardDeviation()));
        info << QObject::tr("Per-face quality variance: %1").arg(formatFloat(dist.Variance()));
        MeshFilterRunResult result = successInfo(info);
        result.outputValues["face_quality_min"] = dist.Min();
        result.outputValues["face_quality_max"] = dist.Max();
        result.outputValues["face_quality_avg"] = dist.Avg();
        result.outputValues["face_quality_median"] = dist.Percentile(0.5f);
        result.outputValues["face_quality_stddev"] = dist.StandardDeviation();
        result.outputValues["face_quality_variance"] = dist.Variance();
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertHist) || filterId == QString::fromLatin1(kFilterFaceHist)) {
        const bool vertexHistogram = (filterId == QString::fromLatin1(kFilterVertHist));
        const float rangeMin = float(params.getDouble(QStringLiteral("HistMin")));
        const float rangeMax = float(params.getDouble(QStringLiteral("HistMax")));
        const int binNum = std::max(1, params.getInt(QStringLiteral("binNum")));
        const bool areaWeighted = params.getBool(QStringLiteral("areaWeighted"));
        if (!(rangeMax > rangeMin)) {
            return fail(QObject::tr("Histogram max must be greater than histogram min."));
        }

        Histogramf hist;
        hist.SetRange(rangeMin, rangeMax, binNum);
        QStringList info;
        info << QObject::tr("Mesh: %1").arg(meshName);
        info << QObject::tr("%1 histogram, range [%2, %3], bins %4%5")
                    .arg(vertexHistogram ? QObject::tr("Per-vertex quality") : QObject::tr("Per-face quality"))
                    .arg(formatFloat(rangeMin, 7))
                    .arg(formatFloat(rangeMax, 7))
                    .arg(binNum)
                    .arg(areaWeighted ? QObject::tr(", area weighted") : QString());

        if (vertexHistogram) {
            std::vector<float> weights(size_t(mesh.VN()), 1.0f);
            if (areaWeighted)
                vcg::tri::MeshToMatrix<VCGMesh>::PerVertexArea(mesh, weights);
            for (int i = 0; i < mesh.VN(); ++i)
                hist.Add(mesh.vert[size_t(i)].Q(), weights[size_t(i)]);
        } else {
            std::vector<float> weights(size_t(mesh.FN()), 1.0f);
            if (areaWeighted)
                vcg::tri::MeshToMatrix<VCGMesh>::PerFaceArea(mesh, weights);
            for (int i = 0; i < mesh.FN(); ++i)
                hist.Add(mesh.face[size_t(i)].Q(), weights[size_t(i)]);
        }

        info << QObject::tr("(-inf .. %1): %2")
                    .arg(formatFloat(rangeMin, 7))
                    .arg(histogramCountString(hist.BinCountInd(0), areaWeighted));
        for (int i = 1; i <= binNum; ++i) {
            info << QObject::tr("[%1 .. %2): %3")
                        .arg(formatFloat(hist.BinLowerBound(i), 7))
                        .arg(formatFloat(hist.BinUpperBound(i), 7))
                        .arg(histogramCountString(hist.BinCountInd(i), areaWeighted));
        }
        info << QObject::tr("[%1 .. +inf): %2")
                    .arg(formatFloat(rangeMax, 7))
                    .arg(histogramCountString(hist.BinCountInd(binNum + 1), areaWeighted));
        MeshFilterRunResult result = successInfo(info);
        QString prefix = vertexHistogram ? QStringLiteral("vertex_hist") : QStringLiteral("face_hist");
        QList<double> binMin, binMax, binCount;
        for (int i = 0; i < binNum + 2; ++i) {
            binMin.append(double(hist.BinLowerBound(i)));
            binMax.append(double(hist.BinUpperBound(i)));
            binCount.append(double(hist.BinCountInd(i)));
        }
        result.outputValues[prefix + "_bin_min"] = QVariant::fromValue(binMin);
        result.outputValues[prefix + "_bin_max"] = QVariant::fromValue(binMax);
        result.outputValues[prefix + "_bin_count"] = QVariant::fromValue(binCount);
        result.outputValues[prefix + "_area_weighted"] = areaWeighted;
        return result;
    }

    return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
}

void registerMeasureFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<MeasureFilterPlugin>());
}
