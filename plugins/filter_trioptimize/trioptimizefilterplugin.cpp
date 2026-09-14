#include "trioptimizefilterplugin.h"

#include "curvedgeflip.h"
#include "document.h"
#include "meshfilterpluginmanager.h"

#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/local_optimization.h>
#include <vcg/complex/algorithms/local_optimization/tri_edge_flip.h>
#include <vcg/complex/algorithms/smooth.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/math/base.h>
#include <ctime>
#include <limits>
#include <memory>

namespace {
constexpr QLatin1StringView kPlanarFlip("flip_edges_by_planarity");
constexpr QLatin1StringView kCurvatureFlip("flip_edges_by_curvature");
constexpr QLatin1StringView kNearLaplacian("smooth_vertices_by_surface_preserving_laplacian_vcglib");

using Mask = vcg::tri::io::Mask;

class NSMCEFlip;
class MeanCEFlip;
class AbsCEFlip;

class NSMCEFlip : public vcg::tri::CurvEdgeFlip<VCGMesh, NSMCEFlip, vcg::NSMCEval>
{
public:
    NSMCEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::CurvEdgeFlip<VCGMesh, NSMCEFlip, vcg::NSMCEval>(pos, mark, pp)
    {
    }
};

class MeanCEFlip : public vcg::tri::CurvEdgeFlip<VCGMesh, MeanCEFlip, vcg::MeanCEval>
{
public:
    MeanCEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::CurvEdgeFlip<VCGMesh, MeanCEFlip, vcg::MeanCEval>(pos, mark, pp)
    {
    }
};

class AbsCEFlip : public vcg::tri::CurvEdgeFlip<VCGMesh, AbsCEFlip, vcg::AbsCEval>
{
public:
    AbsCEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::CurvEdgeFlip<VCGMesh, AbsCEFlip, vcg::AbsCEval>(pos, mark, pp)
    {
    }
};

class MyTriEFlip : public vcg::tri::TriEdgeFlip<VCGMesh, MyTriEFlip>
{
public:
    MyTriEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::TriEdgeFlip<VCGMesh, MyTriEFlip>(pos, mark, pp)
    {
    }
};

class MyTopoEFlip : public vcg::tri::TopoEdgeFlip<VCGMesh, MyTopoEFlip>
{
public:
    MyTopoEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::TopoEdgeFlip<VCGMesh, MyTopoEFlip>(pos, mark, pp)
    {
    }
};

class QEFlip : public vcg::tri::PlanarEdgeFlip<VCGMesh, QEFlip>
{
public:
    QEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::PlanarEdgeFlip<VCGMesh, QEFlip>(pos, mark, pp)
    {
    }
};

class QRadiiEFlip
    : public vcg::tri::PlanarEdgeFlip<VCGMesh, QRadiiEFlip, vcg::QualityRadii>
{
public:
    QRadiiEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::PlanarEdgeFlip<VCGMesh, QRadiiEFlip, vcg::QualityRadii>(pos, mark, pp)
    {
    }
};

class QMeanRatioEFlip
    : public vcg::tri::PlanarEdgeFlip<VCGMesh, QMeanRatioEFlip, vcg::QualityMeanRatio>
{
public:
    QMeanRatioEFlip(PosType pos, int mark, vcg::BaseParameterClass *pp)
        : vcg::tri::PlanarEdgeFlip<VCGMesh, QMeanRatioEFlip, vcg::QualityMeanRatio>(pos, mark, pp)
    {
    }
};

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

MeshFilterRunResult success(const QStringList &info = {})
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    return result;
}

int selectedFaceCount(const VCGMesh &mesh)
{
    int count = 0;
    for (const VCGFace &face : mesh.face) {
        if (!face.IsD() && face.IsS())
            ++count;
    }
    return count;
}

void setAllWritable(VCGMesh &mesh)
{
    for (VCGFace &face : mesh.face) {
        if (!face.IsD())
            face.SetW();
    }
    for (VCGVertex &vertex : mesh.vert) {
        if (!vertex.IsD())
            vertex.SetW();
    }
}

void restrictWritableToSelectedFaces(VCGMesh &mesh)
{
    for (VCGFace &face : mesh.face) {
        if (face.IsD())
            continue;
        if (face.IsS())
            face.SetW();
        else
            face.ClearW();
    }

    vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceLoose(mesh);
    for (VCGVertex &vertex : mesh.vert) {
        if (vertex.IsD())
            continue;
        if (vertex.IsS())
            vertex.SetW();
        else
            vertex.ClearW();
    }
}

void normalizeGeometryAfterOptimization(VCGMesh &mesh)
{
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
}

QString planarMetricName(const QString &metric)
{
    if (metric == QStringLiteral("normalized_radius_ratio"))
        return QObject::tr("normalized radius ratio");
    if (metric == QStringLiteral("mean_ratio"))
        return QObject::tr("mean ratio");
    if (metric == QStringLiteral("delaunay"))
        return QObject::tr("Delaunay");
    if (metric == QStringLiteral("topology"))
        return QObject::tr("topology");
    return QObject::tr("area/max side");
}

QString curvatureMetricName(const QString &metric)
{
    if (metric == QStringLiteral("norm_squared"))
        return QObject::tr("norm squared mean curvature");
    if (metric == QStringLiteral("absolute"))
        return QObject::tr("absolute curvature");
    return QObject::tr("mean curvature");
}

template <typename FlipType>
int runSinglePassEdgeFlip(VCGMesh &mesh, vcg::tri::PlanarEdgeFlipParameter &pp)
{
    vcg::LocalOptimization<VCGMesh> optim(mesh, &pp);
    optim.Init<FlipType>();
    optim.SetTargetMetric(-std::numeric_limits<float>::epsilon());
    optim.DoOptimization();
    optim.h.clear();
    return optim.nPerformedOps;
}

} // namespace

QString TriOptimizeFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.trioptimize");
}

QString TriOptimizeFilterPlugin::name() const
{
    return QObject::tr("TriOptimize Filters");
}

MeshFilterRunResult TriOptimizeFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    if (mesh.FN() <= 0)
        return fail(QObject::tr("Current mesh has no faces."));

    try {
        setAllWritable(mesh);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);

        if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0)
            return fail(QObject::tr("Edge flip optimization requires a two-manifold face topology."));

        if (filterId == QString::fromLatin1(kPlanarFlip)) {
            const bool selected = params.getBool(QStringLiteral("selection"));
            if (selected && selectedFaceCount(mesh) <= 0)
                return fail(QObject::tr("No selected faces available for planar edge flip optimization."));
            if (selected)
                restrictWritableToSelectedFaces(mesh);

            vcg::tri::PlanarEdgeFlipParameter pp;
            const float planarThresholdDeg = float(params.getDouble(QStringLiteral("pthreshold")));
            pp.CoplanarAngleThresholdDeg = planarThresholdDeg;

            const std::clock_t start = std::clock();
            const QString metric = params.getEnum(QStringLiteral("planartype"));
            int performed = 0;
            if (metric == QStringLiteral("normalized_radius_ratio"))
                performed = runSinglePassEdgeFlip<QRadiiEFlip>(mesh, pp);
            else if (metric == QStringLiteral("mean_ratio"))
                performed = runSinglePassEdgeFlip<QMeanRatioEFlip>(mesh, pp);
            else if (metric == QStringLiteral("delaunay"))
                performed = runSinglePassEdgeFlip<MyTriEFlip>(mesh, pp);
            else if (metric == QStringLiteral("topology"))
                performed = runSinglePassEdgeFlip<MyTopoEFlip>(mesh, pp);
            else
                performed = runSinglePassEdgeFlip<QEFlip>(mesh, pp);

            const int iterations = std::max(0, params.getInt(QStringLiteral("iterations")));
            if (iterations > 0) {
                vcg::tri::Smooth<VCGMesh>::VertexCoordPlanarLaplacian(
                    mesh,
                    iterations,
                    vcg::math::ToRad(planarThresholdDeg),
                    selected,
                    doc.progressCallback());
            }

            normalizeGeometryAfterOptimization(mesh);
            if (selected)
                vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);
            setAllWritable(mesh);

            entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
            if (metric == QStringLiteral("topology"))
                entry.ioMask |= Mask::IOM_VERTQUALITY;
            doc.markMeshGeometryChanged(
                meshIndex,
                QObject::tr("Applied planar edge flip optimization on '%1'").arg(entry.name));

            const double seconds = double(std::clock() - start) / double(CLOCKS_PER_SEC);
            return success({
                QObject::tr("%1 planar edge flips performed.").arg(performed),
                QObject::tr("Metric: %1").arg(planarMetricName(metric)),
                QObject::tr("Elapsed: %1 s").arg(QString::number(seconds, 'f', 2))
            });
        }

        if (filterId == QString::fromLatin1(kCurvatureFlip)) {
            const bool selected = params.getBool(QStringLiteral("selection"));
            if (selected && selectedFaceCount(mesh) <= 0)
                return fail(QObject::tr("No selected faces available for curvature edge flip optimization."));
            if (selected)
                restrictWritableToSelectedFaces(mesh);

            vcg::tri::UpdateTopology<VCGMesh>::VertexFace(mesh);
            vcg::tri::UpdateTopology<VCGMesh>::TestVertexFace(mesh);

            vcg::tri::PlanarEdgeFlipParameter pp;
            pp.CoplanarAngleThresholdDeg = float(params.getDouble(QStringLiteral("pthreshold")));

            const std::clock_t start = std::clock();
            const QString metric = params.getEnum(QStringLiteral("curvtype"));
            int performed = 0;
            if (metric == QStringLiteral("norm_squared"))
                performed = runSinglePassEdgeFlip<NSMCEFlip>(mesh, pp);
            else if (metric == QStringLiteral("absolute"))
                performed = runSinglePassEdgeFlip<AbsCEFlip>(mesh, pp);
            else
                performed = runSinglePassEdgeFlip<MeanCEFlip>(mesh, pp);

            normalizeGeometryAfterOptimization(mesh);
            if (selected)
                vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);
            setAllWritable(mesh);

            entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL | Mask::IOM_VERTQUALITY;
            doc.markMeshGeometryChanged(
                meshIndex,
                QObject::tr("Applied curvature edge flip optimization on '%1'").arg(entry.name));

            const double seconds = double(std::clock() - start) / double(CLOCKS_PER_SEC);
            return success({
                QObject::tr("%1 curvature edge flips performed.").arg(performed),
                QObject::tr("Metric: %1").arg(curvatureMetricName(metric)),
                QObject::tr("Elapsed: %1 s").arg(QString::number(seconds, 'f', 2))
            });
        }

        if (filterId == QString::fromLatin1(kNearLaplacian)) {
            const bool selected = params.getBool(QStringLiteral("selection"));
            if (selected && selectedFaceCount(mesh) <= 0)
                return fail(QObject::tr("No selected faces available for surface-preserving Laplacian smoothing."));
            if (selected)
                vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(mesh);

            const int iterations = std::max(1, params.getInt(QStringLiteral("iterations")));
            const float angleDeg = float(params.getDouble(QStringLiteral("AngleDeg")));
            vcg::tri::Smooth<VCGMesh>::VertexCoordPlanarLaplacian(
                mesh,
                iterations,
                vcg::math::ToRad(angleDeg),
                selected,
                doc.progressCallback());

            normalizeGeometryAfterOptimization(mesh);
            entry.ioMask |= Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
            doc.markMeshGeometryChanged(
                meshIndex,
                QObject::tr("Applied surface-preserving Laplacian smoothing on '%1'").arg(entry.name));
            return success({
                QObject::tr("Surface-preserving Laplacian smoothing completed."),
                QObject::tr("Iterations: %1").arg(iterations)
            });
        }

        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
    } catch (const vcg::MissingPreconditionException &e) {
        setAllWritable(mesh);
        return fail(QString::fromLocal8Bit(e.what()));
    } catch (const std::exception &e) {
        setAllWritable(mesh);
        return fail(QString::fromLocal8Bit(e.what()));
    } catch (...) {
        setAllWritable(mesh);
        return fail(QObject::tr("Unexpected TriOptimize filter error."));
    }
}

void registerTriOptimizeFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<TriOptimizeFilterPlugin>());
}
