#include "mlsfilterplugin.h"

#include "apss.h"
#include "document.h"
#include "implicits.h"
#include "meshfilterpluginmanager.h"
#include "mlscompat.h"
#include "mlsmarchingcube.h"
#include "mlssurface.h"
#include "rimls.h"
#include "smallcomponentselection.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/create/marching_cubes.h>
#include <vcg/complex/algorithms/refine.h>
#include <vcg/complex/algorithms/refine_loop.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/append.h>
#include <QMatrix4x4>
#include <QVector4D>
#include <cmath>
#include <memory>

namespace {
constexpr QLatin1StringView kIdRimlsProjection("project_vertices_onto_mls_surface_rimls");
constexpr QLatin1StringView kIdApssProjection("project_vertices_onto_mls_surface_apss");
constexpr QLatin1StringView kIdRimlsMcube("reconstruct_surface_by_marching_cubes_rimls");
constexpr QLatin1StringView kIdApssMcube("reconstruct_surface_by_marching_cubes_apss");
constexpr QLatin1StringView kIdRimlsCurvatureQuality("compute_curvature_rimls");
constexpr QLatin1StringView kIdApssCurvatureQuality("compute_curvature_apss");
constexpr QLatin1StringView kIdRadiusFromDensity("estimate_radius_from_density");
constexpr QLatin1StringView kIdSelectSmallComponents("select_small_disconnected_components");

enum CurvatureType {
    CT_MEAN = 0,
    CT_GAUSS = 1,
    CT_K1 = 2,
    CT_K2 = 3,
    CT_APSS = 4
};

using Mask = vcg::tri::io::Mask;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

MeshFilterRunResult success(bool modified, const QStringList &info = {})
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = modified;
    result.infoMessages = info;
    return result;
}

MeshFilterRunResult qualitySuccess(int meshIndex, const QStringList &info = {})
{
    MeshFilterRunResult result = success(true, info);
    result.visualizationHints.push_back({
        meshIndex,
        MeshFilterVisualizationAttribute::VertexQuality
    });
    return result;
}

template<typename Scalar>
vcg::Point3<Scalar> qMatrixMapPoint(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &point)
{
    const QVector4D mapped = matrix * QVector4D(point.X(), point.Y(), point.Z(), 1.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

template<typename Scalar>
vcg::Point3<Scalar> qMatrixMapDirection(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &dir)
{
    const QVector4D mapped = matrix * QVector4D(dir.X(), dir.Y(), dir.Z(), 0.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

std::unique_ptr<VCGMesh> transformedCopy(const Document::MeshEntry &entry)
{
    auto copy = std::make_unique<VCGMesh>();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(*copy, entry.mesh);
    const QMatrix4x4 transform = entry.transform;
    for (VCGVertex &v : copy->vert) {
        v.P() = qMatrixMapPoint<float>(transform, v.cP());
        v.N() = qMatrixMapDirection<float>(transform, v.cN());
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(*copy);
    return copy;
}

bool inverseTransformMeshToLocal(const QMatrix4x4 &transform, const VCGMesh &worldMesh, VCGMesh &localMesh)
{
    bool invertible = false;
    const QMatrix4x4 invTransform = transform.inverted(&invertible);
    if (!invertible)
        return false;

    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(localMesh, worldMesh);
    for (VCGVertex &v : localMesh.vert) {
        v.P() = qMatrixMapPoint<float>(invTransform, v.cP());
        v.N() = qMatrixMapDirection<float>(invTransform, v.cN());
        if (v.N().Norm() > 1e-12f)
            v.N().Normalize();
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(localMesh);
    return true;
}

bool ensureVertexNormals(VCGMesh &mesh)
{
    if (mesh.VN() <= 0)
        return false;
    if (mesh.FN() <= 0) {
        for (VCGVertex &v : mesh.vert) {
            if (v.cN().Norm() <= 1e-12f)
                return false;
        }
    } else {
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
    }
    for (VCGVertex &v : mesh.vert) {
        if (v.N().Norm() > 1e-12f)
            v.N().Normalize();
    }
    return true;
}

bool initMlsMesh(VCGMesh &mesh, int nNeighbors = 16)
{
    if (mesh.VN() < 2)
        return false;
    if (mesh.FN() > 0)
        vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
    vcg::tri::Allocator<VCGMesh>::CompactVertexVector(mesh);
    if (mesh.FN() > 0)
        vcg::tri::Allocator<VCGMesh>::CompactFaceVector(mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    GaelMls::computeVertexRadius(mesh, nNeighbors);
    return true;
}

std::unique_ptr<GaelMls::MlsSurface<VCGMesh>> createRimls(VCGMesh &points, const FilterParams &params)
{
    auto rimls = std::make_unique<GaelMls::RIMLS<VCGMesh>>(points);
    rimls->setFilterScale(float(params.getDouble(QStringLiteral("FilterScale"), 2.0)));
    rimls->setMaxProjectionIters(std::max(1, params.getInt(QStringLiteral("MaxProjectionIters"), 15)));
    rimls->setProjectionAccuracy(float(params.getDouble(QStringLiteral("ProjectionAccuracy"), 1e-4)));
    rimls->setMaxRefittingIters(std::max(0, params.getInt(QStringLiteral("MaxRefittingIters"), 3)));
    rimls->setSigmaN(float(params.getDouble(QStringLiteral("SigmaN"), 0.75)));
    return rimls;
}

std::unique_ptr<GaelMls::MlsSurface<VCGMesh>> createApss(
    VCGMesh &points,
    const FilterParams &params,
    bool curvatureQualityMode)
{
    auto apss = std::make_unique<GaelMls::APSS<VCGMesh>>(points);
    apss->setFilterScale(float(params.getDouble(QStringLiteral("FilterScale"), 2.0)));
    apss->setMaxProjectionIters(std::max(1, params.getInt(QStringLiteral("MaxProjectionIters"), 15)));
    apss->setProjectionAccuracy(float(params.getDouble(QStringLiteral("ProjectionAccuracy"), 1e-4)));
    apss->setSphericalParameter(float(params.getDouble(QStringLiteral("SphericalParameter"), 1.0)));
    if (!curvatureQualityMode) {
        apss->setGradientHint(
            params.getBool(QStringLiteral("AccurateNormal"), true)
                ? GaelMls::MLS_DERIVATIVE_ACCURATE
                : GaelMls::MLS_DERIVATIVE_APPROX);
    }
    return apss;
}

template<class MeshType, typename Scalar>
struct EdgeAnglePredicate
{
    Scalar thCosAngle;
    bool operator()(vcg::face::Pos<typename MeshType::FaceType> ep) const
    {
        return (ep.F()->cN() * ep.FFlip()->cN()) < thCosAngle;
    }
};

void computeProjection(
    VCGMesh &mesh,
    bool selectionOnly,
    int maxSubdivisions,
    float creaseAngleDeg,
    GaelMls::MlsSurface<VCGMesh> &mls,
    vcg::CallBackPos *cb)
{
    VCGMeshFFAdjScope _ffAdj(mesh);
    if (selectionOnly)
        vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);

    EdgeAnglePredicate<VCGMesh, float> edgePred;
    edgePred.thCosAngle = std::cos(float(M_PI) * creaseAngleDeg / 180.0f);

    for (int k = 0; k < maxSubdivisions + 1; ++k) {
        if (k != 0) {
            vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerFace(mesh);
            vcg::tri::UpdateNormal<VCGMesh>::NormalizePerFace(mesh);
            vcg::tri::RefineOddEvenE<VCGMesh, vcg::tri::OddPointLoop<VCGMesh>, vcg::tri::EvenPointLoop<VCGMesh>>(
                mesh,
                vcg::tri::OddPointLoop<VCGMesh>(mesh),
                vcg::tri::EvenPointLoop<VCGMesh>(),
                edgePred,
                selectionOnly,
                cb);
        }

        const int totalVerts = std::max(1, int(mesh.vert.size()));
        for (int i = 0; i < int(mesh.vert.size()); ++i) {
            if (cb)
                cb(1 + 98 * i / totalVerts, "Projecting onto the MLS surface...");
            if ((!selectionOnly) || mesh.vert[size_t(i)].IsS())
                mesh.vert[size_t(i)].P() = mls.project(mesh.vert[size_t(i)].P(), &mesh.vert[size_t(i)].N());
        }
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
}

void computeCurvatureQuality(
    VCGMesh &mesh,
    bool selectionOnly,
    int curvatureType,
    GaelMls::MlsSurface<VCGMesh> &mls,
    vcg::CallBackPos *cb)
{
    const int size = int(mesh.vert.size());
    for (int i = 0; i < size; ++i) {
        if (cb)
            cb(1 + 98 * i / std::max(1, size), "MLS colorization...");

        VCGVertex &v = mesh.vert[size_t(i)];
        if (selectionOnly && !v.IsS())
            continue;

        const vcg::Point3f p = mls.project(v.P());
        float c = 0.0f;
        if (curvatureType == CT_APSS) {
            auto *apss = dynamic_cast<GaelMls::APSS<VCGMesh> *>(&mls);
            c = apss ? apss->approxMeanCurvature(p) : 0.0f;
        } else {
            int errorMask = GaelMls::MLS_OK;
            vcg::Point3f grad = mls.gradient(p, &errorMask);
            if (errorMask == GaelMls::MLS_OK && grad.Norm() > 1e-8f) {
                const vcg::Matrix33f hess = mls.hessian(p);
                vcg::implicits::WeingartenMap<float> W(grad, hess);
                v.PD1() = W.K1Dir();
                v.PD2() = W.K2Dir();
                v.K1() = W.K1();
                v.K2() = W.K2();
                switch (curvatureType) {
                case CT_MEAN: c = W.MeanCurvature(); break;
                case CT_GAUSS: c = W.GaussCurvature(); break;
                case CT_K1: c = W.K1(); break;
                case CT_K2: c = W.K2(); break;
                default: c = W.MeanCurvature(); break;
                }
            }
        }
        v.Q() = c;
    }
}

int computeMarchingCubes(
    VCGMesh &mesh,
    int resolution,
    GaelMls::MlsSurface<VCGMesh> &mls,
    vcg::CallBackPos *cb)
{
    using MlsWalker = vcg::tri::MlsWalker<VCGMesh, GaelMls::MlsSurface<VCGMesh>>;
    using MlsMarchingCubes = vcg::tri::MarchingCubes<VCGMesh, MlsWalker>;

    MlsWalker walker;
    walker.resolution = std::max(16, resolution);
    MlsMarchingCubes mc(mesh, walker);
    walker.BuildMesh<MlsMarchingCubes>(mesh, mls, mc, cb);

    const int totalVerts = std::max(1, int(mesh.vert.size()));
    for (int i = 0; i < int(mesh.vert.size()); ++i) {
        if (cb)
            cb(1 + 98 * i / totalVerts, "Projecting onto the MLS surface...");
        mesh.vert[size_t(i)].P() = mls.project(mesh.vert[size_t(i)].P(), &mesh.vert[size_t(i)].N());
    }

    mesh.face.EnableFFAdjacency();
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
    vcg::tri::SmallComponent<VCGMesh>::Select(mesh, 0.1f);
    vcg::tri::SmallComponent<VCGMesh>::DeleteFaceVert(mesh);
    mesh.face.DisableFFAdjacency();
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
    return mesh.FN();
}

QStringList radiusInfo(VCGMesh &mesh)
{
    QStringList info;
    auto h = vcg::tri::Allocator<VCGMesh>::FindPerVertexAttribute<float>(mesh, "radius");
    if (!vcg::tri::Allocator<VCGMesh>::IsValidHandle<float>(mesh, h))
        return info;

    float minR = std::numeric_limits<float>::max();
    float maxR = 0.0f;
    for (size_t i = 0; i < mesh.vert.size(); ++i) {
        minR = std::min(minR, h[i]);
        maxR = std::max(maxR, h[i]);
        mesh.vert[i].Q() = h[i];
    }
    info << QObject::tr("Estimated local radius attribute for %1 vertices.").arg(mesh.VN());
    info << QObject::tr("Radius range copied into vertex quality: [%1, %2].")
                .arg(QString::number(minR, 'f', 6))
                .arg(QString::number(maxR, 'f', 6));
    return info;
}

std::unique_ptr<GaelMls::MlsSurface<VCGMesh>> createMlsForFilter(
    const QString &filterId,
    VCGMesh &points,
    const FilterParams &params,
    bool curvatureQualityMode)
{
    if (filterId == QString::fromLatin1(kIdApssProjection)
        || filterId == QString::fromLatin1(kIdApssMcube)
        || filterId == QString::fromLatin1(kIdApssCurvatureQuality)) {
        return createApss(points, params, curvatureQualityMode);
    }
    return createRimls(points, params);
}

QString projectedMeshName(const Document::MeshEntry &proxyEntry, const QString &variant)
{
    return QObject::tr("%1 (%2)").arg(proxyEntry.name, variant);
}

QString marchingMeshName(const Document::MeshEntry &sourceEntry, const QString &variant)
{
    return QObject::tr("%1 %2").arg(sourceEntry.name, variant);
}

} // namespace

QString MlsFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.mls");
}

QString MlsFilterPlugin::name() const
{
    return QObject::tr("MLS (APSS/RIMLS) Filters");
}

MeshFilterRunResult MlsFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int currentIndex = doc.currentMeshIndex();
    if (currentIndex < 0 || currentIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    if (filterId == QString::fromLatin1(kIdRadiusFromDensity)) {
        Document::MeshEntry &entry = doc.mesh(currentIndex);
        if (!initMlsMesh(entry.mesh, std::max(2, params.getInt(QStringLiteral("NbNeighbors"), 16))))
            return fail(QObject::tr("At least two vertices are required to estimate radius from density."));
        entry.ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(currentIndex, QObject::tr("Estimated MLS radius attribute for '%1'").arg(entry.name));
        return success(true, radiusInfo(entry.mesh));
    }

    if (filterId == QString::fromLatin1(kIdSelectSmallComponents)) {
        Document::MeshEntry &entry = doc.mesh(currentIndex);
        vcg::tri::SmallComponent<VCGMesh>::Select(
            entry.mesh,
            float(params.getDouble(QStringLiteral("NbFaceRatio"), 0.1)),
            params.getBool(QStringLiteral("NonClosedOnly"), false));
        doc.markMeshSelectionChanged(currentIndex, QObject::tr("Selected small disconnected components on '%1'").arg(entry.name));
        // How many faces were marked is appended by the framework, the same way for
        // every selection filter.
        return success(true);
    }

    if (filterId == QString::fromLatin1(kIdApssProjection)
        || filterId == QString::fromLatin1(kIdRimlsProjection)) {
        const int controlIndex = params.getMesh(QStringLiteral("ControlMesh"), currentIndex);
        const int proxyIndex = params.getMesh(QStringLiteral("ProxyMesh"), currentIndex);

        const Document::MeshEntry &controlEntry = doc.mesh(controlIndex);
        Document::MeshEntry &proxyEntry = doc.mesh(proxyIndex);
        auto controlMesh = transformedCopy(controlEntry);
        auto proxyMesh = transformedCopy(proxyEntry);
        if (!ensureVertexNormals(*controlMesh))
            return fail(QObject::tr("Control mesh '%1' must provide oriented vertex normals.").arg(controlEntry.name));
        if (!ensureVertexNormals(*proxyMesh))
            return fail(QObject::tr("Proxy mesh '%1' must provide oriented vertex normals.").arg(proxyEntry.name));
        if (!initMlsMesh(*controlMesh))
            return fail(QObject::tr("Control mesh '%1' needs at least two vertices.").arg(controlEntry.name));

        auto mls = createMlsForFilter(filterId, *controlMesh, params, false);
        computeProjection(
            *proxyMesh,
            params.getBool(QStringLiteral("SelectionOnly"), false),
            std::max(0, params.getInt(QStringLiteral("MaxSubdivisions"), 0)),
            float(params.getDouble(QStringLiteral("ThAngleInDegree"), 2.0)),
            *mls,
            doc.progressCallback());

        VCGMesh localResult;
        if (!inverseTransformMeshToLocal(proxyEntry.transform, *proxyMesh, localResult))
            return fail(QObject::tr("Proxy mesh transform is not invertible."));
        proxyEntry.mesh.Clear();
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(proxyEntry.mesh, localResult);
        proxyEntry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        doc.markMeshGeometryChanged(proxyIndex, QObject::tr("Projected '%1' onto MLS surface of '%2'").arg(proxyEntry.name, controlEntry.name));
        return success(true, {
            QObject::tr("Projected %1 onto the %2 MLS surface of %3.")
                .arg(proxyEntry.name)
                .arg(filterId == QString::fromLatin1(kIdApssProjection) ? QStringLiteral("APSS") : QStringLiteral("RIMLS"))
                .arg(controlEntry.name)
        });
    }

    if (filterId == QString::fromLatin1(kIdApssMcube)
        || filterId == QString::fromLatin1(kIdRimlsMcube)) {
        const Document::MeshEntry &entry = doc.mesh(currentIndex);
        auto controlMesh = transformedCopy(entry);
        if (!ensureVertexNormals(*controlMesh))
            return fail(QObject::tr("Current mesh '%1' must provide oriented vertex normals.").arg(entry.name));
        if (!initMlsMesh(*controlMesh))
            return fail(QObject::tr("Current mesh '%1' needs at least two vertices.").arg(entry.name));

        auto mls = createMlsForFilter(filterId, *controlMesh, params, false);
        VCGMesh resultMesh;
        computeMarchingCubes(resultMesh, params.getInt(QStringLiteral("Resolution"), 200), *mls, doc.progressCallback());
        const int newIndex = doc.addMesh(
            resultMesh,
            marchingMeshName(entry, filterId == QString::fromLatin1(kIdApssMcube) ? QStringLiteral("APSS MC") : QStringLiteral("RIMLS MC")),
            Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL);
        if (newIndex >= 0) {
            doc.mesh(newIndex).transform.setToIdentity();
            doc.mesh(newIndex).ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        }
        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices = { newIndex };
        result.infoMessages = {
            QObject::tr("Generated marching-cubes MLS surface with %1 vertices and %2 faces.")
                .arg(resultMesh.VN())
                .arg(resultMesh.FN())
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kIdApssCurvatureQuality)
        || filterId == QString::fromLatin1(kIdRimlsCurvatureQuality)) {
        Document::MeshEntry &entry = doc.mesh(currentIndex);
        VCGMesh workMesh;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(workMesh, entry.mesh);
        workMesh.vert.EnableCurvatureDir();
        if (!ensureVertexNormals(workMesh))
            return fail(QObject::tr("Current mesh '%1' must provide oriented vertex normals.").arg(entry.name));
        if (!initMlsMesh(workMesh))
            return fail(QObject::tr("Current mesh '%1' needs at least two vertices.").arg(entry.name));

        auto mls = createMlsForFilter(filterId, workMesh, params, true);
        const QString curv = params.getEnum(QStringLiteral("CurvatureType"));
        int curvatureType = CT_MEAN;
        if (curv == QStringLiteral("gauss")) curvatureType = CT_GAUSS;
        else if (curv == QStringLiteral("k1")) curvatureType = CT_K1;
        else if (curv == QStringLiteral("k2")) curvatureType = CT_K2;
        else if (curv == QStringLiteral("approx_mean")) curvatureType = CT_APSS;

        computeCurvatureQuality(
            workMesh,
            params.getBool(QStringLiteral("SelectionOnly"), false),
            curvatureType,
            *mls,
            doc.progressCallback());

        entry.mesh.Clear();
        entry.mesh.vert.EnableCurvatureDir();
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(entry.mesh, workMesh);
        entry.ioMask |= Mask::IOM_VERTQUALITY | Mask::IOM_VERTNORMAL;
        doc.markMeshGeometryChanged(currentIndex, QObject::tr("Computed MLS curvature quality for '%1'").arg(entry.name));
        return qualitySuccess(currentIndex, {
            QObject::tr("Computed %1 MLS curvature and stored it in vertex quality.")
                .arg(filterId == QString::fromLatin1(kIdApssCurvatureQuality) ? QStringLiteral("APSS") : QStringLiteral("RIMLS"))
        });
    }

    return fail(QObject::tr("Unknown MLS filter '%1'.").arg(filterId));
}

void registerMlsFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<MlsFilterPlugin>());
}
