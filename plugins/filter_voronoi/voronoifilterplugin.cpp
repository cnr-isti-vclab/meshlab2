#include "voronoifilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/point_sampling.h>
#include <vcg/complex/algorithms/polygon_support.h>
#include <vcg/complex/algorithms/smooth.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/color.h>
#include <vcg/complex/algorithms/update/curvature.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/algorithms/voronoi_processing.h>
#include <vcg/complex/algorithms/voronoi_volume_sampling.h>

#include <QObject>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <memory>
#include <vector>

namespace {

constexpr QLatin1StringView kVoronoiSampling("sample_surface_by_voronoi_relaxation");
constexpr QLatin1StringView kVolumeSampling("sample_volume");
constexpr QLatin1StringView kVoronoiScaffolding("create_voronoi_scaffolding");
constexpr QLatin1StringView kSolidWireframe("create_solid_wireframe");

using Mask = vcg::tri::io::Mask;
using Scalar = VCGMesh::ScalarType;
using Point = VCGMesh::CoordType;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = message;
    return result;
}

MeshFilterRunResult success(const QStringList &info, const QVector<int> &newMeshIndices = {})
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    result.newMeshIndices = newMeshIndices;
    return result;
}

void finalizeMesh(VCGMesh &mesh)
{
    if (mesh.FN() > 0)
        vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
    if (mesh.FN() > 0) {
        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFace(mesh);
    }
    if (mesh.VN() > 0)
        vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    else
        mesh.bbox.SetNull();
}

int addDerivedMesh(Document &doc, int sourceMeshIndex, VCGMesh &mesh, int ioMask)
{
    finalizeMesh(mesh);
    const int newIndex = doc.addMesh(mesh, {}, ioMask);
    if (newIndex >= 0 && sourceMeshIndex >= 0 && sourceMeshIndex < doc.meshCount())
        doc.setMeshTransform(newIndex, doc.meshTransform(sourceMeshIndex), QString());
    return newIndex;
}

float maxDimension(const VCGMesh &mesh)
{
    return std::max(mesh.bbox.DimX(), std::max(mesh.bbox.DimY(), mesh.bbox.DimZ()));
}

float autoVolumePoissonRadius(const VCGMesh &mesh, int montecarloSampleCount)
{
    const float diag = std::max(mesh.bbox.Diag(), 1e-6f);
    float volume = mesh.bbox.DimX() * mesh.bbox.DimY() * mesh.bbox.DimZ();
    if (!std::isfinite(volume) || volume <= 0.0f)
        volume = diag * diag * diag;

    const int targetSeedCount = std::max(4, montecarloSampleCount / 50);
    const float spacing = std::cbrt(volume / float(targetSeedCount));
    return std::max(spacing * 0.55f, diag * 1e-5f);
}

// Leaves the chosen scalar in vertex quality, measured on the final diagram. What the relaxation
// itself left there depends on how it stopped: the seed distance after its last pass, the border
// distance when it converged early, and the region area was never stored at all.
// Distances are geodesic over the surface, whatever metric drove the relaxation.
void computeVoronoiScalar(VCGMesh &mesh, std::vector<VCGVertex *> &seeds, const QString &field)
{
    auto sources = vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<VCGVertex *>(mesh, std::string("sources"));
    std::vector<std::pair<float, VCGVertex *>> regionArea(mesh.vert.size(), {0.0f, nullptr});
    std::vector<VCGVertex *> frontier;
    vcg::tri::VoronoiProcessing<VCGMesh>::GetAreaAndFrontier(mesh, sources, regionArea, frontier);
    if (field == QStringLiteral("region_area")) {
        for (auto &v : mesh.vert)
            if (!v.IsD())
                v.Q() = sources[v] ? regionArea[vcg::tri::Index(mesh, sources[v])].first : 0.0f;
        return;
    }
    vcg::tri::Geodesic<VCGMesh>::Compute(mesh, field == QStringLiteral("border_distance") ? frontier : seeds);
}

int elementTypeId(const QString &id)
{
    if (id == QStringLiteral("seed"))
        return 0;
    if (id == QStringLiteral("face"))
        return 2;
    return 1;
}

bool qualityRangeIsUsable(const VCGMesh &mesh)
{
    if (mesh.VN() <= 0)
        return false;
    const auto minMax = vcg::tri::Stat<VCGMesh>::ComputePerVertexQualityMinMax(mesh);
    return std::isfinite(minMax.first)
        && std::isfinite(minMax.second)
        && std::abs(minMax.second - minMax.first) > std::numeric_limits<float>::epsilon();
}

void ensureVoronoiBaseAttributes(VCGMesh &mesh)
{
    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<VCGVertex *>(
        mesh,
        std::string("sources"));
    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<bool>(
        mesh,
        std::string("fixed"));
    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<float>(
        mesh,
        std::string("area"));
}

// The names vcglib's Voronoi functions pass state to one another by, geod being the geodesic
// visit's mailbox. None is a result: sources holds pointers into this very vertex array and
// area is zero unless the relaxation was restricted, so they go once the diagram is built.
void removeVoronoiAttributes(VCGMesh &mesh)
{
    for (const char *name : {"sources", "fixed", "area", "geod"})
        vcg::tri::Allocator<VCGMesh>::DeletePerVertexAttribute(mesh, name);
}

MeshFilterRunResult runVoronoiSampling(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("Voronoi Sampling requires a current mesh."));

    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    if (mesh.VN() <= 0 || mesh.FN() <= 0)
        return fail(QObject::tr("Voronoi Sampling requires a mesh with vertices and faces."));
    if (mesh.bbox.Diag() <= 0.0f)
        return fail(QObject::tr("Voronoi Sampling requires a mesh with a non-zero bounding box."));

    const QString distanceType = params.getEnum(QStringLiteral("distanceType"));
    if (distanceType == QStringLiteral("quality_weighted")) {
        if ((entry.ioMask & Mask::IOM_VERTQUALITY) == 0)
            return fail(QObject::tr("Quality Weighted distance requires per-vertex quality."));
        if (!qualityRangeIsUsable(mesh))
            return fail(QObject::tr("Quality Weighted distance requires a non-constant vertex quality field."));
    }

    doc.beginFilterProgress(QObject::tr("Sample Surface by Voronoi Relaxation"));
    vcg::CallBackPos *cb = doc.progressCallback();

    try {
        if (cb)
            (*cb)(1, "Computing initial surface samples...");
        vcg::tri::UpdateCurvature<VCGMesh>::PerVertexBasicRadialCrossField(mesh);

        std::vector<VCGMesh::VertexPointer> seedVertices;
        std::vector<Point> seedPoints;
        std::vector<bool> fixedSeeds;
        Scalar radius = 0.0f;
        const RandomSeed seed = params.getRandomSeed();
        // Seed perturbation during relaxation draws from a generator that is static
        // per VoronoiProcessing instantiation, so each distance functor used below
        // needs its own initialize() — VoronoiProcessing<VCGMesh> already is the
        // EuclideanDistance instantiation (it is the default template argument).
        vcg::tri::VoronoiProcessing<VCGMesh>::RandomGenerator().initialize(seed.value);
        vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::IsotropicDistance<VCGMesh>>
            ::RandomGenerator().initialize(seed.value);
        vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::AnisotropicDistance<VCGMesh>>
            ::RandomGenerator().initialize(seed.value);
        vcg::tri::PoissonSampling<VCGMesh>(
            mesh,
            seedPoints,
            params.getInt(QStringLiteral("sampleNum")),
            radius,
            Scalar(params.getDouble(QStringLiteral("radiusVariance"))),
            Scalar(0),
            seed.value);

        if (seedPoints.empty()) {
            doc.finishFilterProgress(false, QObject::tr("The Poisson sampler produced no seed points."));
            return fail(QObject::tr("The Poisson sampler produced no seed points."));
        }

        vcg::tri::VoronoiProcessingParameter vpp;
        const QString relaxType = params.getEnum(QStringLiteral("relaxType"));
        vpp.geodesicRelaxFlag = (relaxType == QStringLiteral("geodesic"));
        vpp.deleteUnreachedRegionFlag = true;
        vpp.refinementRatio = float(params.getInt(QStringLiteral("refineFactor")));
        vpp.seedPerturbationAmount = float(params.getDouble(QStringLiteral("perturbAmount")));
        vpp.seedPerturbationProbability = float(params.getDouble(QStringLiteral("perturbProbability")));
        vpp.lcb = cb ? cb : vcg::DummyCallBackPos;

        if (params.getBool(QStringLiteral("preprocessFlag"))) {
            if (cb)
                (*cb)(8, "Preprocessing mesh for Voronoi relaxation...");
            vcg::tri::VoronoiProcessing<VCGMesh>::PreprocessForVoronoi(mesh, radius, vpp);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
        }

        ensureVoronoiBaseAttributes(mesh);
        vcg::tri::VoronoiProcessing<VCGMesh>::SeedToVertexConversion(mesh, seedPoints, seedVertices);
        if (seedVertices.empty()) {
            doc.finishFilterProgress(false, QObject::tr("No seed point could be associated to a mesh vertex."));
            return fail(QObject::tr("No seed point could be associated to a mesh vertex."));
        }
        fixedSeeds.assign(seedPoints.size(), false);

        if (distanceType == QStringLiteral("quality_weighted")) {
            vcg::tri::IsotropicDistance<VCGMesh> distance(mesh, float(params.getDouble(QStringLiteral("radiusVariance"))));
            vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::IsotropicDistance<VCGMesh>>::VoronoiRelaxing(
                mesh,
                seedVertices,
                params.getInt(QStringLiteral("iterNum")),
                distance,
                vpp,
                cb);
        } else if (distanceType == QStringLiteral("anisotropic")) {
            for (int i = 0; i < params.getInt(QStringLiteral("iterNum")); ++i) {
                if (cb)
                    (*cb)(10 + (70 * i) / std::max(1, params.getInt(QStringLiteral("iterNum"))),
                          "Relaxing anisotropic Voronoi diagram...");
                vcg::tri::BasicCrossFunctor<VCGMesh> crossFunctor(mesh);
                vcg::tri::AnisotropicDistance<VCGMesh> distance(mesh, crossFunctor);
                vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::AnisotropicDistance<VCGMesh>>::VoronoiRelaxing(
                    mesh,
                    seedVertices,
                    1,
                    distance,
                    vpp,
                    cb);
            }
            if (params.getInt(QStringLiteral("iterNum")) == 0) {
                vcg::tri::BasicCrossFunctor<VCGMesh> crossFunctor(mesh);
                vcg::tri::AnisotropicDistance<VCGMesh> distance(mesh, crossFunctor);
                vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::AnisotropicDistance<VCGMesh>>::ComputePerVertexSources(
                    mesh,
                    seedVertices,
                    distance);
            }
        } else {
            vcg::tri::EuclideanDistance<VCGMesh> distance;
            if (relaxType == QStringLiteral("restricted")) {
                for (int i = 0; i < params.getInt(QStringLiteral("iterNum")); ++i) {
                    if (cb)
                        (*cb)(10 + (70 * i) / std::max(1, params.getInt(QStringLiteral("iterNum"))),
                              "Relaxing restricted Voronoi diagram...");
                    vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::EuclideanDistance<VCGMesh>>::RestrictedVoronoiRelaxing(
                        mesh,
                        seedPoints,
                        fixedSeeds,
                        10,
                        vpp);
                    vcg::tri::VoronoiProcessing<VCGMesh>::SeedToVertexConversion(mesh, seedPoints, seedVertices);
                    vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::EuclideanDistance<VCGMesh>>::ComputePerVertexSources(
                        mesh,
                        seedVertices,
                        distance);
                }
                if (params.getInt(QStringLiteral("iterNum")) == 0) {
                    vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::EuclideanDistance<VCGMesh>>::ComputePerVertexSources(
                        mesh,
                        seedVertices,
                        distance);
                }
            } else {
                vcg::tri::VoronoiProcessing<VCGMesh, vcg::tri::EuclideanDistance<VCGMesh>>::VoronoiRelaxing(
                    mesh,
                    seedVertices,
                    params.getInt(QStringLiteral("iterNum")),
                    distance,
                    vpp,
                    cb);
            }
        }

        computeVoronoiScalar(mesh, seedVertices, params.getEnum(QStringLiteral("scalarField")));

        // The cells, their boundaries and the Delaunay triangulation exist only when every
        // region is a disk. A region can instead wrap around a thin part of the mesh (a tube,
        // the inside of a torus) and touch itself: the sampling is still good, but such a cell
        // has two boundary loops and is no fan around its seed, so those layers are skipped.
        if (cb)
            (*cb)(84, "Converting Voronoi diagram to mesh layers...");
        VCGMesh voronoiMesh;
        VCGMesh voronoiPoly;
        VCGMesh delaunayMesh;
        const bool diskRegions =
            vcg::tri::VoronoiProcessing<VCGMesh>::CheckVoronoiTopology(mesh, seedVertices);
        if (diskRegions) {
            voronoiMesh.vert.EnableVFAdjacency();
            voronoiMesh.face.EnableVFAdjacency();
            voronoiMesh.face.EnableFFAdjacency();
            voronoiMesh.face.EnableMark();
            vcg::tri::VoronoiProcessing<VCGMesh>::ConvertVoronoiDiagramToMesh(
                mesh, voronoiMesh, voronoiPoly, seedVertices, vpp);
            vcg::tri::VoronoiProcessing<VCGMesh>::ConvertDelaunayTriangulationToMesh(
                mesh, delaunayMesh, seedVertices, false, params.getBool(QStringLiteral("delaunayToBorder")));
        }
        VCGMesh samples;
        for (VCGMesh::VertexPointer seedVertex : seedVertices) {
            auto vi = vcg::tri::Allocator<VCGMesh>::AddVertex(samples, seedVertex->P());
            vi->N() = seedVertex->N();
        }
        removeVoronoiAttributes(mesh);

        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
        entry.ioMask |= Mask::IOM_VERTQUALITY | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Computed Voronoi sampling with %1 seeds.").arg(seedVertices.size()));

        // Added in the order of the declared tags, the samples last: the last layer a filter adds
        // is left current, and a sampling filter leaves its samples current.
        QVector<int> newMeshes;
        QStringList info;
        if (diskRegions) {
            newMeshes << addDerivedMesh(doc, meshIndex, voronoiMesh,
                                        Mask::IOM_VERTCOLOR | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL
                                            | Mask::IOM_BITPOLYGONAL)
                      << addDerivedMesh(doc, meshIndex, voronoiPoly, Mask::IOM_VERTCOLOR | Mask::IOM_EDGEINDEX)
                      << addDerivedMesh(doc, meshIndex, delaunayMesh,
                                        Mask::IOM_VERTCOLOR | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL);
            info << QObject::tr("Built %1 Voronoi cells and a Delaunay triangulation of %2 triangles.")
                        .arg(seedVertices.size())
                        .arg(delaunayMesh.FN());
        } else {
            doc.writeLog(QObject::tr("Some Voronoi regions are not disks (they wrap around a thin part of "
                                     "the mesh), so only the samples were created, not the cells, their "
                                     "boundaries or the Delaunay triangulation. More samples make the "
                                     "regions smaller."),
                         Document::LogSource::Application, Document::LogLevel::Warning);
        }
        newMeshes << addDerivedMesh(doc, meshIndex, samples, Mask::IOM_VERTNORMAL);
        if (newMeshes.contains(-1)) {
            doc.finishFilterProgress(false, QObject::tr("Failed to add Voronoi output meshes."));
            return fail(QObject::tr("Failed to add Voronoi output meshes."));
        }

        doc.finishFilterProgress(true, QObject::tr("Generated Voronoi sampling layers."));
        info << QObject::tr("Generated %1 Voronoi seeds.").arg(seedVertices.size()) << seed.message();
        MeshFilterRunResult result = success(info, newMeshes);
        if (!diskRegions)
            result.outputTags = { QStringLiteral("voronoi samples") };
        result.visualizationHints.push_back({ meshIndex, MeshFilterVisualizationAttribute::VertexQuality });
        return result;
    } catch (const std::exception &e) {
        const QString message =
            QObject::tr("Voronoi Sampling failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    } catch (...) {
        const QString message = QObject::tr("Voronoi Sampling failed with an unknown error.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

MeshFilterRunResult runVolumeSampling(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("Volumetric Sampling requires a current mesh."));

    VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() <= 0 || mesh.FN() <= 0)
        return fail(QObject::tr("Volumetric Sampling requires a mesh with vertices and faces."));
    if (mesh.bbox.Diag() <= 0.0f)
        return fail(QObject::tr("Volumetric Sampling requires a mesh with a non-zero bounding box."));

    doc.beginFilterProgress(QObject::tr("Sample Volume"));
    vcg::CallBackPos *cb = doc.progressCallback();

    try {
        vcg::tri::VoronoiVolumeSampling<VCGMesh> sampler(mesh);
        sampler.seedDomainTree = nullptr;
        sampler.cb = cb;

        // Init() copies this generator over the surface sampler's static one and
        // BuildMontecarloVolumeSampling draws the volume points from it, so it has
        // to be set before Init.
        const RandomSeed seed = params.getRandomSeed();
        sampler.rng.initialize(seed.value);

        if (cb)
            (*cb)(5, "Sampling surface...");
        sampler.Init(Scalar(params.getDouble(QStringLiteral("sampleSurfRadius"))));

        if (cb)
            (*cb)(30, "Sampling volume...");
        Scalar poissonRadius = Scalar(params.getDouble(QStringLiteral("poissonRadius")));
        if (!std::isfinite(poissonRadius) || poissonRadius <= 0.0f)
            poissonRadius = autoVolumePoissonRadius(mesh, params.getInt(QStringLiteral("sampleVolNum")));
        sampler.BuildVolumeSampling(
            params.getInt(QStringLiteral("sampleVolNum")), poissonRadius, int(seed.value));

        VCGMesh montecarloVolume;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopy(montecarloVolume, sampler.montecarloVolumeMesh);
        if (montecarloVolume.VN() > 0)
            vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(montecarloVolume);

        VCGMesh surfaceSampling;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopy(surfaceSampling, sampler.psd.poissonSurfaceMesh);

        QVector<int> newMeshes;
        const int montecarloIndex = addDerivedMesh(
            doc,
            meshIndex,
            montecarloVolume,
            Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY);
        if (montecarloIndex >= 0)
            newMeshes.push_back(montecarloIndex);

        if (params.getBool(QStringLiteral("poissonFiltering"))) {
            VCGMesh poissonVolume;
            vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopy(poissonVolume, sampler.seedMesh);
            if (poissonVolume.VN() > 0)
                vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(poissonVolume);
            const int poissonIndex = addDerivedMesh(
                doc,
                meshIndex,
                poissonVolume,
                Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY);
            if (poissonIndex >= 0)
                newMeshes.push_back(poissonIndex);
        }

        const int surfaceIndex = addDerivedMesh(
            doc,
            meshIndex,
            surfaceSampling,
            Mask::IOM_VERTNORMAL | Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY);
        if (surfaceIndex >= 0)
            newMeshes.push_back(surfaceIndex);

        if (newMeshes.isEmpty()) {
            doc.finishFilterProgress(false, QObject::tr("Volumetric Sampling produced no output meshes."));
            return fail(QObject::tr("Volumetric Sampling produced no output meshes."));
        }

        doc.finishFilterProgress(true, QObject::tr("Generated volumetric sampling layers."));
        return success(
            {
                QObject::tr("Generated %1 Monte Carlo samples and %2 surface samples.")
                    .arg(sampler.montecarloVolumeMesh.VN())
                    .arg(sampler.psd.poissonSurfaceMesh.VN()),
                seed.message()
            },
            newMeshes);
    } catch (const std::exception &e) {
        const QString message =
            QObject::tr("Volumetric Sampling failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    } catch (...) {
        const QString message = QObject::tr("Volumetric Sampling failed with an unknown error.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

MeshFilterRunResult runVoronoiScaffolding(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("Voronoi Scaffolding requires a current mesh."));

    VCGMesh &mesh = doc.mesh(meshIndex).mesh;
    if (mesh.VN() <= 0 || mesh.FN() <= 0)
        return fail(QObject::tr("Voronoi Scaffolding requires a mesh with vertices and faces."));
    if (mesh.bbox.Diag() <= 0.0f)
        return fail(QObject::tr("Voronoi Scaffolding requires a mesh with a non-zero bounding box."));

    doc.beginFilterProgress(QObject::tr("Create Voronoi Scaffolding"));
    vcg::CallBackPos *cb = doc.progressCallback();

    try {
        vcg::tri::VoronoiVolumeSampling<VCGMesh> sampler(mesh);
        sampler.seedDomainTree = nullptr;
        sampler.cb = cb;

        // Must precede Init(), which copies this generator over the surface
        // sampler's static one.
        const RandomSeed seed = params.getRandomSeed();
        sampler.rng.initialize(seed.value);

        if (cb)
            (*cb)(10, "Sampling surface...");
        sampler.Init(Scalar(params.getDouble(QStringLiteral("sampleSurfRadius"))));

        if (cb)
            (*cb)(30, "Sampling volume...");
        const int volumeSamples = params.getInt(QStringLiteral("sampleVolNum"));
        const Scalar poissonRadius = autoVolumePoissonRadius(mesh, volumeSamples);
        sampler.BuildVolumeSampling(volumeSamples, poissonRadius, int(seed.value));
        if (sampler.seedMesh.VN() <= 0) {
            doc.finishFilterProgress(false, QObject::tr("No Voronoi seeds were generated."));
            return fail(QObject::tr("No Voronoi seeds were generated."));
        }

        QStringList notes;
        const int relaxStep = params.getInt(QStringLiteral("relaxStep"));
        if (relaxStep > 0) {
            if (sampler.montecarloVolumeMesh.VN() > sampler.seedMesh.VN() * 20) {
                if (cb)
                    (*cb)(45, "Relaxing volume seeds...");
                sampler.BarycentricRelaxVoronoiSamples(relaxStep);
            } else {
                notes.push_back(
                    QObject::tr("Skipped Lloyd relaxation because the volume sampling is not dense enough for the generated seed count."));
            }
        }

        if (cb)
            (*cb)(60, "Building scaffolding volume...");
        vcg::tri::VoronoiVolumeSampling<VCGMesh>::Param scaffoldParams;
        const float voxelSide = std::max(maxDimension(mesh) / float(params.getInt(QStringLiteral("voxelRes"))), 1e-6f);
        scaffoldParams.voxelSide = voxelSide;
        scaffoldParams.isoThr = Scalar(params.getDouble(QStringLiteral("isoThr"))) * voxelSide;
        scaffoldParams.surfFlag = params.getBool(QStringLiteral("surfFlag"));
        scaffoldParams.elemType = elementTypeId(params.getEnum(QStringLiteral("elemType")));

        VCGMesh scaffolding;
        sampler.BuildScaffoldingMesh(scaffolding, scaffoldParams);
        if (params.getInt(QStringLiteral("smoothStep")) > 0 && scaffolding.VN() > 0) {
            if (cb)
                (*cb)(90, "Smoothing scaffolding mesh...");
            vcg::tri::Smooth<VCGMesh>::VertexCoordLaplacian(
                scaffolding,
                params.getInt(QStringLiteral("smoothStep")));
        }

        VCGMesh montecarloVolume;
        VCGMesh poissonSurface;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopy(montecarloVolume, sampler.montecarloVolumeMesh);
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopy(poissonSurface, sampler.psd.poissonSurfaceMesh);
        if (montecarloVolume.VN() > 0)
            vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(montecarloVolume);

        QVector<int> newMeshes;
        const int scaffoldIndex = addDerivedMesh(
            doc,
            meshIndex,
            scaffolding,
            Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL);
        if (scaffoldIndex >= 0)
            newMeshes.push_back(scaffoldIndex);

        const int montecarloIndex = addDerivedMesh(
            doc,
            meshIndex,
            montecarloVolume,
            Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY);
        if (montecarloIndex >= 0)
            newMeshes.push_back(montecarloIndex);

        const int surfaceIndex = addDerivedMesh(
            doc,
            meshIndex,
            poissonSurface,
            Mask::IOM_VERTNORMAL | Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY);
        if (surfaceIndex >= 0)
            newMeshes.push_back(surfaceIndex);

        if (newMeshes.isEmpty()) {
            doc.finishFilterProgress(false, QObject::tr("Voronoi Scaffolding produced no output meshes."));
            return fail(QObject::tr("Voronoi Scaffolding produced no output meshes."));
        }

        QStringList info = {
            QObject::tr("Generated scaffolding with %1 vertices and %2 faces from %3 volume seeds.")
                .arg(doc.mesh(scaffoldIndex).mesh.VN())
                .arg(doc.mesh(scaffoldIndex).mesh.FN())
                .arg(sampler.seedMesh.VN())
        };
        info.append(notes);
        info << seed.message();

        doc.finishFilterProgress(true, QObject::tr("Generated Voronoi scaffolding layers."));
        return success(info, newMeshes);
    } catch (const std::exception &e) {
        const QString message =
            QObject::tr("Voronoi Scaffolding failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    } catch (...) {
        const QString message = QObject::tr("Voronoi Scaffolding failed with an unknown error.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

MeshFilterRunResult runSolidWireframe(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("Create Solid Wireframe requires a current mesh."));

    const Document::MeshEntry &entry = doc.mesh(meshIndex);
    if (entry.mesh.VN() <= 0)
        return fail(QObject::tr("Create Solid Wireframe requires a mesh with vertices."));

    const bool buildEdgeCylinders = params.getBool(QStringLiteral("edgeCylFlag"));
    const bool buildVertexCylinders = params.getBool(QStringLiteral("vertCylFlag"));
    const bool buildVertexSpheres = params.getBool(QStringLiteral("vertSphFlag"));
    const bool buildFacePrisms = params.getBool(QStringLiteral("faceExtFlag"));
    if (!buildEdgeCylinders && !buildVertexCylinders && !buildVertexSpheres && !buildFacePrisms)
        return fail(QObject::tr("Create Solid Wireframe needs at least one output element type enabled."));
    if (buildFacePrisms && entry.mesh.FN() <= 0)
        return fail(QObject::tr("Face prism generation requires a mesh with faces."));
    if (buildEdgeCylinders && entry.mesh.EN() <= 0 && entry.mesh.FN() <= 0)
        return fail(QObject::tr("Edge cylinder generation requires mesh edges or faces."));

    VCGMesh input;
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(input, entry.mesh);
    vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(input);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(input);
    vcg::tri::UpdateBounding<VCGMesh>::Box(input);
    if (input.FN() > 0) {
        input.face.EnableFFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(input);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFace(input);
    }

    VCGMesh output;
    output.face.EnableFFAdjacency();
    try {
        if (buildEdgeCylinders) {
            vcg::tri::BuildCylinderEdgeShell(
                input,
                output,
                float(params.getDouble(QStringLiteral("edgeCylRadius"))),
                params.getInt(QStringLiteral("cylinderSideNum")));
        }
        if (buildVertexCylinders) {
            vcg::tri::BuildCylinderVertexShell(
                input,
                output,
                float(params.getDouble(QStringLiteral("vertCylRadius"))),
                float(params.getDouble(QStringLiteral("edgeCylRadius"))),
                params.getInt(QStringLiteral("cylinderSideNum")));
        }
        if (buildVertexSpheres) {
            vcg::tri::BuildSphereVertexShell(
                input,
                output,
                float(params.getDouble(QStringLiteral("vertSphRadius"))));
        }
        if (buildFacePrisms) {
            vcg::tri::BuildPrismFaceShell(
                input,
                output,
                float(params.getDouble(QStringLiteral("faceExtHeight"))),
                float(params.getDouble(QStringLiteral("faceExtInset"))));
        }
    } catch (const std::exception &e) {
        return fail(QObject::tr("Create Solid Wireframe failed: %1").arg(QString::fromLocal8Bit(e.what())));
    } catch (...) {
        return fail(QObject::tr("Create Solid Wireframe failed with an unknown error."));
    }

    if (output.VN() <= 0)
        return fail(QObject::tr("Create Solid Wireframe produced an empty mesh."));

    const int newIndex = addDerivedMesh(
        doc,
        meshIndex,
        output,
        Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL);
    if (newIndex < 0)
        return fail(QObject::tr("Failed to add the solid wireframe mesh."));

    return success(
        {
            QObject::tr("Generated solid wireframe with %1 vertices and %2 faces.")
                .arg(doc.mesh(newIndex).mesh.VN())
                .arg(doc.mesh(newIndex).mesh.FN())
        },
        { newIndex });
}

} // namespace

QString VoronoiFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.voronoi");
}

QString VoronoiFilterPlugin::name() const
{
    return QObject::tr("Voronoi Filters");
}

MeshFilterRunResult VoronoiFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kVoronoiSampling))
        return runVoronoiSampling(params, doc);
    if (filterId == QString::fromLatin1(kVolumeSampling))
        return runVolumeSampling(params, doc);
    if (filterId == QString::fromLatin1(kVoronoiScaffolding))
        return runVoronoiScaffolding(params, doc);
    if (filterId == QString::fromLatin1(kSolidWireframe))
        return runSolidWireframe(params, doc);

    return fail(QObject::tr("Unsupported Voronoi filter: %1").arg(filterId));
}

void registerVoronoiFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<VoronoiFilterPlugin>());
}
