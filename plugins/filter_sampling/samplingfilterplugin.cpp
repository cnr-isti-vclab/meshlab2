#include "samplingfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "textureassociationutils.h"
#include "vcgmesh.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/clustering.h>
#include <vcg/complex/algorithms/point_sampling.h>
#include <vcg/complex/algorithms/create/resampler.h>
#include <vcg/complex/algorithms/update/color.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/position.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/algorithms/voronoi_processing.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <QMatrix4x4>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>

namespace {
constexpr QLatin1StringView kFilterElementSampling("sample_mesh_elements");
constexpr QLatin1StringView kFilterMontecarloSampling("sample_surface_by_monte_carlo");
constexpr QLatin1StringView kFilterStratifiedSampling("sample_surface_by_stratified_triangles");
constexpr QLatin1StringView kFilterClusteredSampling("sample_vertices_by_clustering");
constexpr QLatin1StringView kFilterPoissonDiskSampling("sample_surface_by_poisson_disk");
constexpr QLatin1StringView kFilterHausdorffDistance("measure_hausdorff_distance");
constexpr QLatin1StringView kFilterDistanceFromReference("compute_distance_from_reference_mesh");
constexpr QLatin1StringView kFilterTexelSampling("sample_texels");
constexpr QLatin1StringView kFilterVertexResampling("transfer_vertex_attributes_by_closest_point");
constexpr QLatin1StringView kFilterUniformMeshResampling("remesh_uniformly_by_volumetric_resampling");
constexpr QLatin1StringView kFilterVoronoiColoring("colorize_vertices_by_voronoi_regions");
constexpr QLatin1StringView kFilterDiskColoring("colorize_vertices_by_disk_distance");
constexpr QLatin1StringView kFilterRegularRecursiveSampling("sample_offset_surface_recursively");
constexpr QLatin1StringView kFilterPointCloudSimplification("simplify_point_cloud");

using Mask = vcg::tri::io::Mask;
namespace Tex = TextureAssociationUtils;
using Scalar = float;
using Point = vcg::Point3f;

class BaseSampler
{
public:
    BaseSampler(VCGMesh *mesh, bool copyColor, bool copyQuality)
        : m(mesh)
        , m_copyColor(copyColor)
        , m_copyQuality(copyQuality)
    {
    }

    VCGMesh *m = nullptr;
    const std::vector<QImage> *textureImages = nullptr;
    int forcedTextureSlot = -1;
    int texSamplingWidth = 0;
    int texSamplingHeight = 0;
    bool uvSpaceFlag = false;
    bool perFaceNormal = false;
    bool qualitySampling = false;

    void reset()
    {
        if (m)
            m->Clear();
    }

    void AddVert(const VCGVertex &v)
    {
        vcg::tri::Allocator<VCGMesh>::AddVertices(*m, 1);
        m->vert.back().ImportData(v);
    }

    void AddFace(const VCGFace &f, VCGMesh::CoordType bary)
    {
        vcg::tri::Allocator<VCGMesh>::AddVertices(*m, 1);
        VCGVertex &out = m->vert.back();
        out.P() = f.cP(0) * bary[0] + f.cP(1) * bary[1] + f.cP(2) * bary[2];
        if (perFaceNormal)
            out.N() = f.cN();
        else
            out.N() = f.cV(0)->N() * bary[0] + f.cV(1)->N() * bary[1] + f.cV(2)->N() * bary[2];

        if (m_copyQuality || qualitySampling) {
            out.Q() = f.cV(0)->Q() * bary[0] + f.cV(1)->Q() * bary[1] + f.cV(2)->Q() * bary[2];
        }

        if (m_copyColor) {
            const vcg::Color4b c0 = f.cV(0)->C();
            const vcg::Color4b c1 = f.cV(1)->C();
            const vcg::Color4b c2 = f.cV(2)->C();
            const auto mix = [&](int channel) {
                const float value =
                    c0[channel] * bary[0] + c1[channel] * bary[1] + c2[channel] * bary[2];
                return static_cast<unsigned char>(std::clamp(int(std::lround(value)), 0, 255));
            };
            out.C() = vcg::Color4b(mix(0), mix(1), mix(2), mix(3));
        }
    }

    void AddTextureSample(
        const VCGFace &f,
        const VCGMesh::CoordType &bary,
        const vcg::Point2i &texturePoint,
        float edgeDistance)
    {
        if (edgeDistance != 0.0f)
            return;

        vcg::tri::Allocator<VCGMesh>::AddVertices(*m, 1);
        VCGVertex &out = m->vert.back();
        if (uvSpaceFlag) {
            out.P() = Point(float(texturePoint[0]), float(texturePoint[1]), 0.0f);
        } else {
            out.P() = f.cP(0) * bary[0] + f.cP(1) * bary[1] + f.cP(2) * bary[2];
        }
        out.N() = f.cV(0)->N() * bary[0] + f.cV(1)->N() * bary[1] + f.cV(2)->N() * bary[2];
        const QImage *tex = nullptr;
        if (textureImages && !textureImages->empty()) {
            const int slot = forcedTextureSlot >= 0
                ? forcedTextureSlot
                : int(f.cWT(0).N());
            if (slot >= 0 && slot < int(textureImages->size()))
                tex = &textureImages->at(size_t(slot));
        }

        if (tex && !tex->isNull()) {
            const int texW = std::max(1, texSamplingWidth);
            const int texH = std::max(1, texSamplingHeight);
            int x = int(tex->width() * (float(texturePoint[0]) / float(texW))) % tex->width();
            int y = int(tex->height() * (1.0f - float(texturePoint[1]) / float(texH))) % tex->height();
            if (x < 0)
                x += tex->width();
            if (y < 0)
                y += tex->height();
            const QRgb value = tex->pixel(x, y);
            out.C() = vcg::Color4b(qRed(value), qGreen(value), qBlue(value), 255);
        }
    }

private:
    bool m_copyColor = false;
    bool m_copyQuality = false;
};

using SurfaceSampler = vcg::tri::SurfaceSampling<VCGMesh, BaseSampler>;

template<class Scalar>
vcg::Point3<Scalar> qMatrixMapPoint(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &point)
{
    const QVector4D mapped = matrix * QVector4D(point[0], point[1], point[2], 1.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

template<class Scalar>
vcg::Point3<Scalar> qMatrixMapDirection(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &direction)
{
    const QVector4D mapped = matrix * QVector4D(direction[0], direction[1], direction[2], 0.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

class LocalRedetailSampler
{
    using FaceMeshGrid = vcg::GridStaticPtr<VCGFace, Scalar>;
    using VertexMeshGrid = vcg::GridStaticPtr<VCGVertex, Scalar>;

public:
    VCGMesh *m = nullptr;
    vcg::CallBackPos *cb = nullptr;
    int sampleNum = 0;
    int sampleCnt = 0;
    FaceMeshGrid unifGridFace;
    VertexMeshGrid unifGridVert;
    bool useVertexSampling = false;
    vcg::tri::FaceTmark<VCGMesh> markerFunctor;

    bool coordFlag = false;
    bool colorFlag = false;
    bool normalFlag = false;
    bool qualityFlag = false;
    bool selectionFlag = false;
    bool storeDistanceAsQualityFlag = false;
    bool storeBarycentricCoordsAsAttributesFlag = false;
    VCGMesh::PerVertexAttributeHandle<Point> perVertBarycentricCoordsHandle;
    VCGMesh::PerVertexAttributeHandle<Scalar> perVertNearestFaceIndex;
    VCGMesh::PerVertexAttributeHandle<Scalar> perVertNearestVertexIndex;
    float distUpperBound = 0.0f;

    void init(VCGMesh *sourceMesh, VCGMesh *targetMesh, vcg::CallBackPos *callback)
    {
        m = sourceMesh;
        cb = callback;
        sampleNum = targetMesh ? targetMesh->VN() : 0;
        sampleCnt = 0;

        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(*m);
        if (m->FN() == 0)
            useVertexSampling = true;

        if (useVertexSampling)
            unifGridVert.Set(m->vert.begin(), m->vert.end());
        else
            unifGridFace.Set(m->face.begin(), m->face.end());
        markerFunctor.SetMesh(m);

        if (storeBarycentricCoordsAsAttributesFlag && targetMesh) {
            if (!useVertexSampling) {
                perVertBarycentricCoordsHandle =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Point>(
                        *targetMesh,
                        std::string("BarycentricCoords"));
                perVertNearestFaceIndex =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Scalar>(
                        *targetMesh,
                        std::string("NearestFaceIndex"));
            } else {
                perVertNearestVertexIndex =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Scalar>(
                        *targetMesh,
                        std::string("NearestVertexIndex"));
            }
        }
    }

    void AddVert(VCGVertex &vertex)
    {
        Point closestPoint;
        Scalar dist = Scalar(distUpperBound);
        const Point startPoint = vertex.cP();

        if (useVertexSampling) {
            VCGVertex *nearestVertex = vcg::tri::GetClosestVertex<VCGMesh, VertexMeshGrid>(
                *m,
                unifGridVert,
                startPoint,
                distUpperBound,
                dist);
            if (cb && sampleNum > 0)
                (*cb)(sampleCnt++ * 100 / sampleNum, "Resampling Vertex attributes");
            if (storeDistanceAsQualityFlag)
                vertex.Q() = dist;
            if (dist == distUpperBound) {
                if (storeBarycentricCoordsAsAttributesFlag)
                    perVertNearestVertexIndex[vertex] = -1.0f;
                return;
            }
            if (storeBarycentricCoordsAsAttributesFlag)
                perVertNearestVertexIndex[vertex] = float(vcg::tri::Index(*m, nearestVertex));
            if (coordFlag)
                vertex.P() = nearestVertex->P();
            if (colorFlag)
                vertex.C() = nearestVertex->C();
            if (normalFlag)
                vertex.N() = nearestVertex->N();
            if (qualityFlag)
                vertex.Q() = nearestVertex->Q();
            if (selectionFlag && nearestVertex->IsS())
                vertex.SetS();
            return;
        }

        VCGFace *nearestFace = nullptr;
        vcg::face::PointDistanceBaseFunctor<Scalar> pointDistanceFunctor;
        dist = Scalar(distUpperBound);
        if (cb && sampleNum > 0)
            (*cb)(sampleCnt++ * 100 / sampleNum, "Resampling Vertex attributes");
        nearestFace = unifGridFace.GetClosest(
            pointDistanceFunctor,
            markerFunctor,
            startPoint,
            distUpperBound,
            dist,
            closestPoint);

        if (!nearestFace && storeBarycentricCoordsAsAttributesFlag) {
            perVertBarycentricCoordsHandle[vertex] = Point(0, 0, 0);
            perVertNearestFaceIndex[vertex] = -1.0f;
        }
        if (dist == distUpperBound || !nearestFace)
            return;

        Point barycentric;
        vcg::InterpolationParameters(*nearestFace, nearestFace->cN(), closestPoint, barycentric);
        barycentric[2] = 1.0f - barycentric[1] - barycentric[0];
        if (storeBarycentricCoordsAsAttributesFlag) {
            perVertBarycentricCoordsHandle[vertex] = barycentric;
            perVertNearestFaceIndex[vertex] = float(vcg::tri::Index(*m, nearestFace));
        }
        if (coordFlag)
            vertex.P() = closestPoint;
        if (colorFlag)
            vertex.C().lerp(
                nearestFace->V(0)->C(),
                nearestFace->V(1)->C(),
                nearestFace->V(2)->C(),
                barycentric);
        if (normalFlag)
            vertex.N() = nearestFace->V(0)->N() * barycentric[0]
                       + nearestFace->V(1)->N() * barycentric[1]
                       + nearestFace->V(2)->N() * barycentric[2];
        if (qualityFlag)
            vertex.Q() = nearestFace->V(0)->Q() * barycentric[0]
                       + nearestFace->V(1)->Q() * barycentric[1]
                       + nearestFace->V(2)->Q() * barycentric[2];
        if (selectionFlag) {
            if (nearestFace->IsS()
                || nearestFace->V(0)->IsS()
                || nearestFace->V(1)->IsS()
                || nearestFace->V(2)->IsS()) {
                vertex.SetS();
            }
        }
    }
};

class SimpleDistanceSampler
{
    using FaceMeshGrid = vcg::GridStaticPtr<VCGFace, Scalar>;
    using VertexMeshGrid = vcg::GridStaticPtr<VCGVertex, Scalar>;

public:
    explicit SimpleDistanceSampler(VCGMesh *referenceMesh, bool signedDistance, double maxDistance)
        : m(referenceMesh)
        , markerFunctor(referenceMesh)
        , useSigned(signedDistance)
        , maxDistABS(maxDistance)
    {
        init();
    }

    VCGMesh *m = nullptr;
    VertexMeshGrid unifGridVert;
    FaceMeshGrid unifGridFace;
    bool useVertexSampling = false;
    Scalar distUpperBound = 0;
    vcg::tri::FaceTmark<VCGMesh> markerFunctor;
    bool useSigned = false;
    double maxDistABS = 0.0;
    int totalSamples = 0;
    double minDist = std::numeric_limits<double>::max();
    double maxDist = std::numeric_limits<double>::lowest();
    double meanDist = 0.0;
    double rmsDist = 0.0;

    float getMeanDist() const { return totalSamples > 0 ? float(meanDist / totalSamples) : 0.0f; }
    float getMinDist() const { return totalSamples > 0 ? float(minDist) : 0.0f; }
    float getMaxDist() const { return totalSamples > 0 ? float(maxDist) : 0.0f; }
    float getRMSDist() const { return totalSamples > 0 ? float(std::sqrt(rmsDist / totalSamples)) : 0.0f; }

    void init()
    {
        if (m->FN() == 0) {
            useVertexSampling = true;
            unifGridVert.Set(m->vert.begin(), m->vert.end());
        } else {
            useVertexSampling = false;
            unifGridFace.Set(m->face.begin(), m->face.end());
            markerFunctor.SetMesh(m);
        }
    }

    void AddVert(VCGVertex &vertex)
    {
        vertex.Q() = AddSample(vertex.cP(), vertex.cN());
    }

    float AddSample(const Point &startPoint, const Point &startNormal)
    {
        Point closestPoint;
        Point closestNormal;
        Scalar dist = Scalar(maxDistABS);
        vcg::face::PointDistanceBaseFunctor<Scalar> pointDistanceFunctor;

        if (useVertexSampling) {
            VCGVertex *nearestVertex = vcg::tri::GetClosestVertex<VCGMesh, VertexMeshGrid>(
                *m,
                unifGridVert,
                startPoint,
                maxDistABS,
                dist);
            if (!nearestVertex)
                return float(maxDistABS * 2.0);
            closestPoint = nearestVertex->P();
            closestNormal = nearestVertex->N();
        } else {
            VCGFace *nearestFace = unifGridFace.GetClosest(
                pointDistanceFunctor,
                markerFunctor,
                startPoint,
                maxDistABS,
                dist,
                closestPoint);
            if (!nearestFace)
                return float(maxDistABS * 2.0);
            closestNormal = nearestFace->N();
        }

        if (useSigned) {
            Point delta = startPoint - closestPoint;
            const auto len = delta.Norm();
            if (len > 1e-20f) {
                delta /= len;
                if (delta * closestNormal < 0.0)
                    dist = -dist;
            }
        }

        minDist = std::min(minDist, double(dist));
        maxDist = std::max(maxDist, double(dist));
        meanDist += dist;
        rmsDist += dist * dist;
        totalSamples++;
        return float(dist);
    }
};

MeshFilterRunResult failResult(const QString &message)
{
    return { false, false, message };
}

MeshFilterRunResult successResult(const QStringList &infoMessages, const QVector<int> &newMeshIndices)
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = !newMeshIndices.isEmpty();
    result.infoMessages = infoMessages;
    result.newMeshIndices = newMeshIndices;
    return result;
}

int pointCloudIoMask(
    const Document::MeshEntry &source,
    bool includeNormals,
    bool includeColor,
    bool includeQuality)
{
    int ioMask = 0;
    if (includeNormals || (source.ioMask & Mask::IOM_VERTNORMAL) != 0)
        ioMask |= Mask::IOM_VERTNORMAL;
    if (includeColor && (source.ioMask & Mask::IOM_VERTCOLOR) != 0)
        ioMask |= Mask::IOM_VERTCOLOR;
    if (includeQuality && (source.ioMask & Mask::IOM_VERTQUALITY) != 0)
        ioMask |= Mask::IOM_VERTQUALITY;
    return ioMask;
}

int addDerivedMesh(
    Document &doc,
    int sourceMeshIndex,
    const VCGMesh &mesh,
    const QString &name,
    int ioMask)
{
    const int newIndex = doc.addMesh(mesh, name, ioMask);
    doc.setMeshTransform(newIndex, doc.meshTransform(sourceMeshIndex), QString());
    return newIndex;
}

std::unique_ptr<VCGMesh> makePreparedSurfaceMesh(const VCGMesh &source)
{
    auto prepared = std::make_unique<VCGMesh>();
    // Append copies an optional component only when the destination already has it
    // enabled, so a plain copy drops the source's texture coordinates -- and a
    // filter that then reads them off the copy dereferences an empty OCF store
    // instead of getting an exception. Texel sampling did exactly that.
    if (source.face.IsWedgeTexCoordEnabled())
        prepared->face.EnableWedgeTexCoord();
    if (source.vert.IsTexCoordEnabled())
        prepared->vert.EnableTexCoord();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(*prepared, source);
    if (prepared->FN() > 0) {
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(*prepared);
        vcg::tri::UpdateBounding<VCGMesh>::Box(*prepared);
    }
    return prepared;
}

// Choose up to sampleNum distinct element indices out of count. Done here rather than
// through vcglib's VertexUniform/FaceUniform because those shuffle with a generator
// seeded from the container size -- the subset is then a function of the mesh alone and
// the filter's declared randomSeed has no effect whatsoever.
std::vector<int> chooseElementSubset(int count, int sampleNum, vcg::math::RandomGenerator &rng)
{
    std::vector<int> chosen(std::size_t(std::max(count, 0)));
    std::iota(chosen.begin(), chosen.end(), 0);
    if (sampleNum >= count)
        return chosen;
    // Partial Fisher-Yates: only the first sampleNum slots have to be settled.
    for (int i = 0; i < sampleNum; ++i) {
        const int j = i + int(rng.generate(unsigned(count - i)));
        std::swap(chosen[std::size_t(i)], chosen[std::size_t(j)]);
    }
    chosen.resize(std::size_t(sampleNum));
    return chosen;
}

bool hasSelection(const VCGMesh &mesh)
{
    return vcg::tri::UpdateSelection<VCGMesh>::VertexCount(mesh) > 0
        || vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh) > 0;
}

std::unique_ptr<VCGMesh> makeSelectedPointSet(const VCGMesh &source)
{
    auto selected = std::make_unique<VCGMesh>();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(*selected, source);
    if (vcg::tri::UpdateSelection<VCGMesh>::VertexCount(*selected) == 0
        && vcg::tri::UpdateSelection<VCGMesh>::FaceCount(*selected) > 0) {
        vcg::tri::UpdateSelection<VCGMesh>::VertexClear(*selected);
        vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceStrict(*selected);
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(*selected);
    return selected;
}

std::unique_ptr<VCGMesh> makeWorldMesh(const Document::MeshEntry &entry, bool recomputeNormals = false)
{
    auto mesh = makePreparedSurfaceMesh(entry.mesh);
    const QMatrix4x4 transform = entry.transform;
    for (VCGVertex &vertex : mesh->vert) {
        vertex.P() = qMatrixMapPoint(transform, vertex.cP());
        if (!recomputeNormals)
            vertex.N() = qMatrixMapDirection(transform, vertex.cN());
    }
    if (mesh->FN() > 0 && recomputeNormals)
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(*mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(*mesh);
    return mesh;
}

void copyPerVertexAppearance(
    const VCGMesh &source,
    VCGMesh &target,
    bool copyColor,
    bool copyQuality,
    bool copyNormal,
    bool copySelection)
{
    const int count = std::min(source.VN(), target.VN());
    for (int i = 0; i < count; ++i) {
        const VCGVertex &src = source.vert[i];
        VCGVertex &dst = target.vert[i];
        if (copyColor)
            dst.C() = src.C();
        if (copyQuality)
            dst.Q() = src.Q();
        if (copyNormal)
            dst.N() = src.N();
        if (copySelection) {
            if (src.IsS())
                dst.SetS();
            else
                dst.ClearS();
        }
    }
}

void copyWorldGeometryBackToLocal(
    const VCGMesh &worldMesh,
    VCGMesh &targetMesh,
    const QMatrix4x4 &targetTransform,
    bool copyPositions,
    bool copyNormals)
{
    bool invertible = false;
    const QMatrix4x4 inverse = targetTransform.inverted(&invertible);
    const QMatrix4x4 normalInverse = inverse.transposed();
    const int count = std::min(worldMesh.VN(), targetMesh.VN());
    for (int i = 0; i < count; ++i) {
        const VCGVertex &src = worldMesh.vert[i];
        VCGVertex &dst = targetMesh.vert[i];
        if (copyPositions) {
            dst.P() = invertible ? qMatrixMapPoint(inverse, src.cP()) : src.cP();
        }
        if (copyNormals) {
            vcg::Point3f n = invertible ? qMatrixMapDirection(normalInverse, src.cN()) : src.cN();
            const float len = std::sqrt(n.SquaredNorm());
            if (len > 1e-20f)
                n /= len;
            dst.N() = n;
        }
    }
}

int meshIndexFromParam(const FilterParams &params, const QString &paramId, const Document &doc)
{
    return params.getMesh(paramId, doc.currentMeshIndex());
}

QString meshName(const Document &doc, int meshIndex)
{
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return QObject::tr("mesh %1").arg(meshIndex);
    const QString name = doc.mesh(meshIndex).name.trimmed();
    return name.isEmpty() ? QObject::tr("mesh %1").arg(meshIndex + 1) : name;
}

} // namespace

QString SamplingFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.sampling");
}

QString SamplingFilterPlugin::name() const
{
    return QObject::tr("Sampling Filters");
}

MeshFilterRunResult SamplingFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    auto modifiedResult = [](QStringList messages = {}) {
        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = std::move(messages);
        return result;
    };

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return failResult(QObject::tr("No current mesh selected."));

    const Document::MeshEntry &entry = doc.mesh(meshIndex);
    const bool sourceHasColor = (entry.ioMask & Mask::IOM_VERTCOLOR) != 0;
    const bool sourceHasQuality = (entry.ioMask & Mask::IOM_VERTQUALITY) != 0;
    vcg::CallBackPos *cb = doc.progressCallback();

    if (filterId == QString::fromLatin1(kFilterElementSampling)) {
        const int sampleNum = params.getInt(QStringLiteral("SampleNum"));
        const QString sampling = params.getEnum(QStringLiteral("Sampling"));
        if (sampleNum <= 0)
            return failResult(QObject::tr("Number of samples must be greater than zero."));
        if ((sampling == QLatin1StringView("edge") || sampling == QLatin1StringView("face")) && entry.mesh.FN() <= 0) {
            return failResult(QObject::tr("Face or edge sampling requires a mesh with faces."));
        }

        std::unique_ptr<VCGMesh> prepared = makePreparedSurfaceMesh(entry.mesh);
        VCGMesh output;
        BaseSampler sampler(&output, sourceHasColor, sourceHasQuality);
        const RandomSeed seed = params.getRandomSeed();
        vcg::math::RandomGenerator &rng = SurfaceSampler::SamplingRandomGenerator();
        rng.initialize(seed.value);

        // All three modes are the same operation on a different element type: choose
        // sampleNum elements uniformly at random and emit one sample per chosen element.
        int elementCount = 0;
        if (sampling == QLatin1StringView("vertex")) {
            std::vector<VCGVertex *> elements;
            elements.reserve(prepared->vert.size());
            for (VCGVertex &v : prepared->vert)
                if (!v.IsD())
                    elements.push_back(&v);
            elementCount = int(elements.size());
            for (const int i : chooseElementSubset(elementCount, sampleNum, rng))
                sampler.AddVert(*elements[std::size_t(i)]);
        }
        else if (sampling == QLatin1StringView("edge")) {
            // Faux edges are the diagonals that triangulate a polygonal face. The
            // wireframe hides them, so sampling them would drop points onto edges the
            // user cannot see and that the polygonal mesh does not really have.
            using PEdge = vcg::tri::UpdateTopology<VCGMesh>::PEdge;
            std::vector<PEdge> elements;
            vcg::tri::UpdateTopology<VCGMesh>::FillUniqueEdgeVector(*prepared, elements, false);
            elementCount = int(elements.size());
            for (const int i : chooseElementSubset(elementCount, sampleNum, rng)) {
                const PEdge &e = elements[std::size_t(i)];
                sampler.AddFace(*e.f, e.EdgeBarycentricToFaceBarycentric(0.5f));
            }
        }
        else {
            std::vector<VCGFace *> elements;
            elements.reserve(prepared->face.size());
            for (VCGFace &f : prepared->face)
                if (!f.IsD())
                    elements.push_back(&f);
            elementCount = int(elements.size());
            const VCGMesh::CoordType centroid(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f);
            for (const int i : chooseElementSubset(elementCount, sampleNum, rng))
                sampler.AddFace(*elements[std::size_t(i)], centroid);
        }

        if (elementCount <= 0)
            return failResult(QObject::tr("The current mesh has no element of that kind to sample."));

        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Element Samples"),
            pointCloudIoMask(
                entry,
                sampling != QLatin1StringView("vertex"),
                sourceHasColor,
                sourceHasQuality));
        QStringList messages{
            QObject::tr("Generated %1 samples from %2 mesh elements.")
                .arg(output.VN())
                .arg(elementCount)
        };
        // Only a strict subset involves a random choice; taking every element does not.
        if (sampleNum < elementCount)
            messages << seed.message();
        return successResult(messages, { newIndex });
    }

    if (filterId == QString::fromLatin1(kFilterMontecarloSampling)) {
        const int sampleNum = params.getInt(QStringLiteral("SampleNum"));
        if (entry.mesh.FN() <= 0)
            return failResult(QObject::tr("Montecarlo sampling requires a mesh with faces."));
        if (sampleNum <= 0)
            return failResult(QObject::tr("Number of samples must be greater than zero."));
        if (params.getBool(QStringLiteral("Weighted")) && !sourceHasQuality) {
            return failResult(QObject::tr("Quality weighted sampling requires vertex quality on the current mesh."));
        }

        std::unique_ptr<VCGMesh> prepared = makePreparedSurfaceMesh(entry.mesh);
        VCGMesh output;
        BaseSampler sampler(&output, sourceHasColor, sourceHasQuality);
        sampler.perFaceNormal = params.getBool(QStringLiteral("PerFaceNormal"));
        const RandomSeed seed = params.getRandomSeed();
        SurfaceSampler::SamplingRandomGenerator().initialize(seed.value);

        if (params.getBool(QStringLiteral("EdgeSampling"))) {
            SurfaceSampler::EdgeMontecarlo(*prepared, sampler, sampleNum, false);
        } else if (params.getBool(QStringLiteral("Weighted"))) {
            SurfaceSampler::WeightedMontecarlo(
                *prepared,
                sampler,
                sampleNum,
                float(params.getDouble(QStringLiteral("RadiusVariance"))));
        } else if (params.getBool(QStringLiteral("ExactNum"))) {
            SurfaceSampler::Montecarlo(*prepared, sampler, sampleNum);
        } else {
            SurfaceSampler::MontecarloPoisson(*prepared, sampler, sampleNum);
        }

        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Montecarlo Samples"),
            pointCloudIoMask(entry, true, sourceHasColor, sourceHasQuality));
        return successResult(
            { QObject::tr("Generated %1 Montecarlo samples.").arg(output.VN()),
              seed.message() },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kFilterStratifiedSampling)) {
        const int sampleNum = params.getInt(QStringLiteral("SampleNum"));
        if (entry.mesh.FN() <= 0)
            return failResult(QObject::tr("Stratified sampling requires a mesh with faces."));
        if (sampleNum <= 0)
            return failResult(QObject::tr("Number of samples must be greater than zero."));

        std::unique_ptr<VCGMesh> prepared = makePreparedSurfaceMesh(entry.mesh);
        VCGMesh output;
        BaseSampler sampler(&output, sourceHasColor, sourceHasQuality);
        const QString strategy = params.getEnum(QStringLiteral("Sampling"));
        const bool randomSampling = params.getBool(QStringLiteral("Random"));
        // Randomness here is opt-in: without 'Random' the sample positions inside
        // each face are fixed, so there is nothing to seed.
        const RandomSeed seed = params.getRandomSeed();
        if (randomSampling)
            SurfaceSampler::SamplingRandomGenerator().initialize(seed.value);

        if (strategy == QLatin1StringView("similar_triangle"))
            SurfaceSampler::FaceSimilar(*prepared, sampler, sampleNum, false, randomSampling);
        else if (strategy == QLatin1StringView("dual_similar_triangle"))
            SurfaceSampler::FaceSimilar(*prepared, sampler, sampleNum, true, randomSampling);
        else if (strategy == QLatin1StringView("long_edge_subdiv"))
            SurfaceSampler::FaceSubdivision(*prepared, sampler, sampleNum, randomSampling);
        else if (strategy == QLatin1StringView("sample_edges"))
            SurfaceSampler::EdgeUniform(*prepared, sampler, sampleNum, true);
        else
            SurfaceSampler::EdgeUniform(*prepared, sampler, sampleNum, false);

        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Stratified Samples"),
            pointCloudIoMask(entry, true, sourceHasColor, sourceHasQuality));
        QStringList messages{
            QObject::tr("Generated %1 stratified samples.").arg(output.VN())
        };
        if (randomSampling)
            messages << seed.message();
        return successResult(messages, { newIndex });
    }

    if (filterId == QString::fromLatin1(kFilterClusteredSampling)) {
        const double threshold = params.getDouble(QStringLiteral("Threshold"));
        const bool selectedOnly = params.getBool(QStringLiteral("Selected"));
        if (!(threshold > 0.0))
            return failResult(QObject::tr("Cell size must be greater than zero."));
        if (selectedOnly && !hasSelection(entry.mesh))
            return failResult(QObject::tr("Only on Selection is enabled, but there is no current selection."));

        std::unique_ptr<VCGMesh> inputMesh = selectedOnly
            ? makeSelectedPointSet(entry.mesh)
            : makePreparedSurfaceMesh(entry.mesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(*inputMesh);

        VCGMesh output;
        const QString strategy = params.getEnum(QStringLiteral("Sampling"));
        if (strategy == QLatin1StringView("average")) {
            vcg::tri::Clustering<VCGMesh, vcg::tri::AverageColorCell<VCGMesh>> grid(
                inputMesh->bbox, 100000, float(threshold));
            grid.AddPointSet(*inputMesh, selectedOnly);
            grid.ExtractPointSet(output);
        } else {
            vcg::tri::Clustering<VCGMesh, vcg::tri::NearestToCenter<VCGMesh>> grid(
                inputMesh->bbox, 100000, float(threshold));
            grid.AddPointSet(*inputMesh, selectedOnly);
            grid.ExtractPointSet(output);
        }

        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Cluster Samples"),
            pointCloudIoMask(
                entry,
                (entry.ioMask & Mask::IOM_VERTNORMAL) != 0,
                sourceHasColor,
                sourceHasQuality));
        return successResult(
            { QObject::tr("Generated %1 clustered samples.").arg(output.VN()) },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kFilterPointCloudSimplification)) {
        const int requestedSampleNum = params.getInt(QStringLiteral("SampleNum"));
        float radius = float(params.getDouble(QStringLiteral("Radius")));
        const bool explicitRadius = radius > 0.0f;
        if (!explicitRadius && requestedSampleNum <= 0)
            return failResult(QObject::tr("Either the sample count or the explicit radius must be greater than zero."));

        int sampleNum = requestedSampleNum;
        if (!explicitRadius)
            radius =
                SurfaceSampler::ComputePoissonDiskRadius(const_cast<VCGMesh &>(entry.mesh), sampleNum);
        else
            sampleNum =
                SurfaceSampler::ComputePoissonSampleNum(const_cast<VCGMesh &>(entry.mesh), radius);

        VCGMesh output;
        BaseSampler sampler(&output, sourceHasColor, sourceHasQuality);
        SurfaceSampler::PoissonDiskParam pp;
        pp.bestSampleChoiceFlag = params.getBool(QStringLiteral("BestSampleFlag"));
        pp.bestSamplePoolSize = params.getInt(QStringLiteral("BestSamplePool"));
        // Pruning shuffles the occupied grid cells, so the surviving subset depends
        // on the generator state.
        const RandomSeed seed = params.getRandomSeed();
        pp.randomSeed = seed.value;

        if (params.getBool(QStringLiteral("ExactNumFlag")) && !explicitRadius) {
            SurfaceSampler::PoissonDiskPruningByNumber(
                sampler,
                const_cast<VCGMesh &>(entry.mesh),
                sampleNum,
                radius,
                pp,
                float(params.getDouble(QStringLiteral("ExactNumTolerance"))),
                20);
        } else {
            SurfaceSampler::PoissonDiskPruning(sampler, const_cast<VCGMesh &>(entry.mesh), radius, pp);
        }

        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Simplified Cloud"),
            pointCloudIoMask(
                entry,
                (entry.ioMask & Mask::IOM_VERTNORMAL) != 0,
                sourceHasColor,
                sourceHasQuality));
        return successResult(
            {
                QObject::tr("Generated %1 simplified samples.").arg(output.VN()),
                QObject::tr("Effective radius: %1").arg(radius, 0, 'f', 6),
                seed.message()
            },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kFilterPoissonDiskSampling)) {
        const int requestedSampleNum = params.getInt(QStringLiteral("SampleNum"));
        float radius = float(params.getDouble(QStringLiteral("Radius")));
        const bool explicitRadius = radius > 0.0f;
        if (!explicitRadius && requestedSampleNum <= 0)
            return failResult(QObject::tr("Either the sample count or the explicit radius must be greater than zero."));

        const bool subsample = params.getBool(QStringLiteral("Subsample"));
        if (!subsample && entry.mesh.FN() <= 0) {
            return failResult(QObject::tr(
                "Surface-based Poisson-disk sampling requires faces. Enable Base Mesh Subsampling to prune the original vertices instead."));
        }

        std::unique_ptr<VCGMesh> prepared = subsample ? nullptr : makePreparedSurfaceMesh(entry.mesh);
        const VCGMesh &samplingMesh = prepared ? *prepared : entry.mesh;

        int sampleNum = requestedSampleNum;
        if (!explicitRadius)
            radius = SurfaceSampler::ComputePoissonDiskRadius(const_cast<VCGMesh &>(samplingMesh), sampleNum);
        else
            sampleNum = SurfaceSampler::ComputePoissonSampleNum(const_cast<VCGMesh &>(samplingMesh), radius);

        SurfaceSampler::PoissonDiskParam pp;
        pp.radiusVariance = float(params.getDouble(QStringLiteral("RadiusVariance"), 1.0));
        pp.geodesicDistanceFlag = params.getBool(QStringLiteral("ApproximateGeodesicDistance"));
        pp.bestSampleChoiceFlag = params.getBool(QStringLiteral("BestSampleFlag"));
        pp.bestSamplePoolSize = params.getInt(QStringLiteral("BestSamplePool"));
        // Both stages draw from the generator — the Montecarlo pre-sampling and the
        // cell shuffle inside the pruning — so seed it up front and let the pruning
        // re-seed itself, the way vcg::tri::PoissonSampling does.
        const RandomSeed seed = params.getRandomSeed();
        SurfaceSampler::SamplingRandomGenerator().initialize(seed.value);
        pp.randomSeed = seed.value;
        if (pp.radiusVariance != 1.0f) {
            if (!sourceHasQuality) {
                return failResult(QObject::tr(
                    "Variable radius Poisson-disk sampling requires vertex quality on the current mesh."));
            }
            pp.adaptiveRadiusFlag = true;
        }

        VCGMesh montecarloMesh;
        QVector<int> newMeshIndices;
        QStringList infoMessages;
        const VCGMesh *presampledMesh = &samplingMesh;
        if (!subsample) {
            BaseSampler montecarloSampler(&montecarloMesh, sourceHasColor, sourceHasQuality);
            montecarloSampler.qualitySampling = true;
            if (pp.adaptiveRadiusFlag) {
                SurfaceSampler::WeightedMontecarlo(
                    const_cast<VCGMesh &>(samplingMesh),
                    montecarloSampler,
                    sampleNum * params.getInt(QStringLiteral("MontecarloRate")),
                    pp.radiusVariance);
            } else {
                SurfaceSampler::Montecarlo(
                    const_cast<VCGMesh &>(samplingMesh),
                    montecarloSampler,
                    sampleNum * params.getInt(QStringLiteral("MontecarloRate")));
            }
            montecarloMesh.bbox = samplingMesh.bbox;
            presampledMesh = &montecarloMesh;

            if (params.getBool(QStringLiteral("SaveMontecarlo"))) {
                const int montecarloIndex = addDerivedMesh(
                    doc,
                    meshIndex,
                    montecarloMesh,
                    QObject::tr("Montecarlo Samples"),
                    pointCloudIoMask(entry, true, sourceHasColor, sourceHasQuality));
                newMeshIndices.push_back(montecarloIndex);
            }
        }

        VCGMesh output;
        BaseSampler sampler(&output, sourceHasColor, sourceHasQuality);
        if (params.getBool(QStringLiteral("RefineFlag"))) {
            const int refineMeshIndex = meshIndexFromParam(params, QStringLiteral("RefineMesh"), doc);
            if (refineMeshIndex < 0 || refineMeshIndex >= doc.meshCount())
                return failResult(QObject::tr("Invalid refine mesh selection."));
            pp.preGenFlag = true;
            pp.preGenMesh = const_cast<VCGMesh *>(&doc.mesh(refineMeshIndex).mesh);
        }
        if (params.getBool(QStringLiteral("ExactNumFlag")) && !explicitRadius) {
            SurfaceSampler::PoissonDiskPruningByNumber(
                sampler,
                const_cast<VCGMesh &>(*presampledMesh),
                sampleNum,
                radius,
                pp,
                float(params.getDouble(QStringLiteral("ExactNumTolerance"))),
                20);
        } else {
            SurfaceSampler::PoissonDiskPruning(
                sampler,
                const_cast<VCGMesh &>(*presampledMesh),
                radius,
                pp);
        }

        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Poisson-disk Samples"),
            pointCloudIoMask(entry, true, sourceHasColor, sourceHasQuality));
        newMeshIndices.push_back(newIndex);
        infoMessages << QObject::tr("Generated %1 Poisson-disk samples.").arg(output.VN())
                     << QObject::tr("Effective radius: %1").arg(radius, 0, 'f', 6);
        if (!subsample)
            infoMessages << QObject::tr("Generated %1 Montecarlo candidates.").arg(presampledMesh->VN());
        infoMessages << seed.message();
        return successResult(infoMessages, newMeshIndices);
    }

    if (filterId == QString::fromLatin1(kFilterTexelSampling)) {
        if (!vcg::tri::HasPerWedgeTexCoord(entry.mesh))
            return failResult(QObject::tr("Texel sampling requires per-wedge texture coordinates."));

        const int requestedW = params.getInt(QStringLiteral("TextureW"));
        const int requestedH = params.getInt(QStringLiteral("TextureH"));
        const bool textureSpace = params.getBool(QStringLiteral("TextureSpace"));
        const bool recoverColor = params.getBool(QStringLiteral("RecoverColor"));
        const int requestedTextureSlot = params.getTextureRef(QStringLiteral("sourceTexture"), 0);

        std::vector<QImage> textureImages;
        if (recoverColor) {
            const int textureCount = Document::meshTextureAssociationCount(entry);
            if (textureCount <= 0)
                return failResult(QObject::tr("Recover Color is enabled, but the current mesh has no associated textures."));
            if (requestedTextureSlot < 0)
                return failResult(QObject::tr("Source Texture must be automatic or a positive selection."));
            if (requestedTextureSlot > textureCount) {
                return failResult(
                    QObject::tr("Source Texture %1 is out of range. The current mesh has %2 associated texture(s).")
                        .arg(requestedTextureSlot)
                        .arg(textureCount));
            }

            textureImages.resize(size_t(textureCount));
            for (int textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
                QString textureError;
                if (!Tex::loadAssociatedTextureImage(entry, textureIndex, textureImages[size_t(textureIndex)], textureError))
                    return failResult(textureError);
            }
        }

        int textureW = requestedW;
        int textureH = requestedH;
        if (recoverColor && textureW == 0 && !textureImages.empty())
            textureW = textureImages.front().width();
        if (recoverColor && textureH == 0 && !textureImages.empty())
            textureH = textureImages.front().height();
        if (textureW <= 0 || textureH <= 0)
            return failResult(QObject::tr("Texture width and height must be greater than zero."));

        VCGMesh output;
        BaseSampler sampler(&output, recoverColor, false);
        sampler.textureImages = recoverColor ? &textureImages : nullptr;
        sampler.forcedTextureSlot = requestedTextureSlot > 0 ? requestedTextureSlot - 1 : -1;
        sampler.texSamplingWidth = textureW;
        sampler.texSamplingHeight = textureH;
        sampler.uvSpaceFlag = textureSpace;

        std::unique_ptr<VCGMesh> prepared = makePreparedSurfaceMesh(entry.mesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceClearB(*prepared);
        SurfaceSampler::Texture(*prepared, sampler, textureW, textureH);
        vcg::tri::UpdateBounding<VCGMesh>::Box(output);

        const int ioMask = Mask::IOM_VERTNORMAL | (recoverColor ? Mask::IOM_VERTCOLOR : 0);
        const int newIndex = doc.addMesh(output, QObject::tr("Texel Samples"), ioMask);
        if (!textureSpace)
            doc.setMeshTransform(newIndex, doc.meshTransform(meshIndex), QString());
        return successResult(
            { QObject::tr("Generated %1 texel samples.").arg(output.VN()) },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kFilterHausdorffDistance)) {
        const int sampledMeshIndex = meshIndexFromParam(params, QStringLiteral("SampledMesh"), doc);
        const int targetMeshIndex = meshIndexFromParam(params, QStringLiteral("TargetMesh"), doc);
        if (sampledMeshIndex == targetMeshIndex)
            return failResult(QObject::tr("Hausdorff distance requires two different meshes."));

        const Document::MeshEntry &sampledEntry = doc.mesh(sampledMeshIndex);
        const Document::MeshEntry &targetEntry = doc.mesh(targetMeshIndex);
        std::unique_ptr<VCGMesh> sampledMesh = makeWorldMesh(sampledEntry, true);
        std::unique_ptr<VCGMesh> targetMesh = makeWorldMesh(targetEntry, true);
        targetMesh->face.EnableMark();
        const bool saveSamples = params.getBool(QStringLiteral("SaveSample"));
        const bool sampleVertices = params.getBool(QStringLiteral("SampleVert"));
        bool sampleEdges = params.getBool(QStringLiteral("SampleEdge"));
        const bool sampleFauxEdges = params.getBool(QStringLiteral("SampleFauxEdge"));
        bool sampleFaces = params.getBool(QStringLiteral("SampleFace"));
        const int sampleNum = params.getInt(QStringLiteral("SampleNum"));
        const float maxDist = float(params.getDouble(QStringLiteral("MaxDist")));

        if (sampleEdges && sampledMesh->FN() == 0)
            sampleEdges = false;
        if (sampleFaces && sampledMesh->FN() == 0)
            sampleFaces = false;

        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(*targetMesh);

        VCGMesh samplePointCloud;
        VCGMesh closestPointCloud;
        vcg::tri::HausdorffSampler<VCGMesh> hausdorffSampler(
            targetMesh.get(),
            saveSamples ? &samplePointCloud : nullptr,
            saveSamples ? &closestPointCloud : nullptr);
        hausdorffSampler.dist_upper_bound = maxDist;

        // Every SurfaceSampling instantiation owns its own static generator, so the
        // Hausdorff sampler has to be seeded separately from the plain BaseSampler
        // one used elsewhere in this plugin.
        using HausdorffSampling =
            vcg::tri::SurfaceSampling<VCGMesh, vcg::tri::HausdorffSampler<VCGMesh>>;
        const bool randomized = sampleVertices || sampleFaces;
        const RandomSeed seed = params.getRandomSeed();
        if (randomized)
            HausdorffSampling::SamplingRandomGenerator().initialize(seed.value);

        if (sampleVertices)
            vcg::tri::SurfaceSampling<VCGMesh, vcg::tri::HausdorffSampler<VCGMesh>>::VertexUniform(
                *sampledMesh, hausdorffSampler, sampleNum);
        if (sampleEdges)
            vcg::tri::SurfaceSampling<VCGMesh, vcg::tri::HausdorffSampler<VCGMesh>>::EdgeUniform(
                *sampledMesh, hausdorffSampler, sampleNum, sampleFauxEdges);
        if (sampleFaces)
            vcg::tri::SurfaceSampling<VCGMesh, vcg::tri::HausdorffSampler<VCGMesh>>::Montecarlo(
                *sampledMesh, hausdorffSampler, sampleNum);

        QStringList infoMessages;
        infoMessages << QObject::tr(
                            "Hausdorff distance between '%1' and '%2'.")
                            .arg(meshName(doc, sampledMeshIndex), meshName(doc, targetMeshIndex))
                     << QObject::tr("Samples: %1").arg(hausdorffSampler.n_total_samples)
                     << QObject::tr("Min: %1").arg(hausdorffSampler.getMinDist(), 0, 'f', 6)
                     << QObject::tr("Max: %1").arg(hausdorffSampler.getMaxDist(), 0, 'f', 6)
                     << QObject::tr("Mean: %1").arg(hausdorffSampler.getMeanDist(), 0, 'f', 6)
                     << QObject::tr("RMS: %1").arg(hausdorffSampler.getRMSDist(), 0, 'f', 6);
        if (randomized)
            infoMessages << seed.message();

        QVector<int> newMeshIndices;
        if (saveSamples) {
            vcg::tri::UpdateBounding<VCGMesh>::Box(samplePointCloud);
            vcg::tri::UpdateBounding<VCGMesh>::Box(closestPointCloud);
            const int sampleIdx = doc.addMesh(
                samplePointCloud,
                QObject::tr("Hausdorff Sample Points"),
                Mask::IOM_VERTQUALITY);
            const int closestIdx = doc.addMesh(
                closestPointCloud,
                QObject::tr("Hausdorff Closest Points"),
                Mask::IOM_VERTQUALITY);
            newMeshIndices << sampleIdx << closestIdx;
        }
        MeshFilterRunResult result = successResult(infoMessages, newMeshIndices);
        for (int newMeshIndex : newMeshIndices) {
            result.visualizationHints.push_back({
                newMeshIndex,
                MeshFilterVisualizationAttribute::VertexQuality
            });
        }
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterDistanceFromReference)) {
        const int measuredMeshIndex = meshIndexFromParam(params, QStringLiteral("MeasureMesh"), doc);
        const int referenceMeshIndex = meshIndexFromParam(params, QStringLiteral("RefMesh"), doc);
        if (measuredMeshIndex == referenceMeshIndex)
            return failResult(QObject::tr("Distance from reference requires two different meshes."));

        Document::MeshEntry &measuredEntry = doc.mesh(measuredMeshIndex);
        const Document::MeshEntry &referenceEntry = doc.mesh(referenceMeshIndex);
        std::unique_ptr<VCGMesh> measuredMesh = makeWorldMesh(measuredEntry, false);
        std::unique_ptr<VCGMesh> referenceMesh = makeWorldMesh(referenceEntry, true);
        referenceMesh->face.EnableMark();
        if (referenceMesh->FN() > 0) {
            vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(*referenceMesh);
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalized(*referenceMesh);
        }

        SimpleDistanceSampler distanceSampler(
            referenceMesh.get(),
            params.getBool(QStringLiteral("SignedDist")),
            params.getDouble(QStringLiteral("MaxDist")));
        vcg::tri::SurfaceSampling<VCGMesh, SimpleDistanceSampler>::AllVertex(*measuredMesh, distanceSampler);
        copyPerVertexAppearance(*measuredMesh, measuredEntry.mesh, false, true, false, false);
        measuredEntry.ioMask |= Mask::IOM_VERTQUALITY;
        doc.markMeshGeometryChanged(
            measuredMeshIndex,
            QObject::tr("Computed distance from '%1'").arg(referenceEntry.name));
        MeshFilterRunResult result = modifiedResult({
            QObject::tr("Computed per-vertex distances from '%1' to '%2'.")
                .arg(meshName(doc, measuredMeshIndex), meshName(doc, referenceMeshIndex)),
            QObject::tr("Min: %1").arg(distanceSampler.getMinDist(), 0, 'f', 6),
            QObject::tr("Max: %1").arg(distanceSampler.getMaxDist(), 0, 'f', 6),
            QObject::tr("Mean: %1").arg(distanceSampler.getMeanDist(), 0, 'f', 6),
            QObject::tr("RMS: %1").arg(distanceSampler.getRMSDist(), 0, 'f', 6)
        });
        result.visualizationHints.push_back({
            measuredMeshIndex,
            MeshFilterVisualizationAttribute::VertexQuality
        });
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertexResampling)) {
        const int sourceMeshIndex = meshIndexFromParam(params, QStringLiteral("SourceMesh"), doc);
        const int targetMeshIndex = meshIndexFromParam(params, QStringLiteral("TargetMesh"), doc);
        if (sourceMeshIndex == targetMeshIndex)
            return failResult(QObject::tr("Attribute transfer requires two different meshes."));

        const bool vertexSampling = params.getBool(QStringLiteral("VertexSampling"));
        const bool transferGeometry = params.getBool(QStringLiteral("GeomTransfer"));
        const bool transferNormals = params.getBool(QStringLiteral("NormalTransfer"));
        const bool transferColor = params.getBool(QStringLiteral("ColorTransfer"));
        const bool transferQuality = params.getBool(QStringLiteral("QualityTransfer"));
        const bool transferSelection = params.getBool(QStringLiteral("SelectionTransfer"));
        const bool qualityDistance = params.getBool(QStringLiteral("QualityDistance"));
        const bool saveBarycentric = params.getBool(QStringLiteral("SaveBarycentric"));
        const bool onlySelected = params.getBool(QStringLiteral("onSelected"));
        if (!transferGeometry && !transferNormals && !transferColor && !transferQuality
            && !transferSelection && !saveBarycentric) {
            return failResult(QObject::tr("Enable at least one attribute transfer option."));
        }

        Document::MeshEntry &targetEntry = doc.mesh(targetMeshIndex);
        const Document::MeshEntry &sourceEntry = doc.mesh(sourceMeshIndex);
        if (onlySelected && !hasSelection(targetEntry.mesh))
            return failResult(QObject::tr("Only on selection is enabled, but the target mesh has no selection."));

        std::unique_ptr<VCGMesh> sourceMesh = makeWorldMesh(sourceEntry, true);
        std::unique_ptr<VCGMesh> targetWorldMesh = makeWorldMesh(targetEntry, false);
        sourceMesh->face.EnableMark();
        if (onlySelected
            && vcg::tri::UpdateSelection<VCGMesh>::VertexCount(*targetWorldMesh) == 0
            && vcg::tri::UpdateSelection<VCGMesh>::FaceCount(*targetWorldMesh) > 0) {
            vcg::tri::UpdateSelection<VCGMesh>::VertexClear(*targetWorldMesh);
            vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceLoose(*targetWorldMesh);
        }

        LocalRedetailSampler sampler;
        sampler.useVertexSampling = vertexSampling;
        sampler.distUpperBound = float(params.getDouble(QStringLiteral("UpperBound")));
        sampler.colorFlag = transferColor;
        sampler.coordFlag = transferGeometry;
        sampler.normalFlag = transferNormals;
        sampler.qualityFlag = transferQuality;
        sampler.selectionFlag = transferSelection;
        sampler.storeDistanceAsQualityFlag = qualityDistance;
        sampler.storeBarycentricCoordsAsAttributesFlag = saveBarycentric;
        sampler.init(sourceMesh.get(), targetWorldMesh.get(), cb);

        if (transferColor)
            targetEntry.ioMask |= Mask::IOM_VERTCOLOR;
        if (transferQuality || qualityDistance)
            targetEntry.ioMask |= Mask::IOM_VERTQUALITY;
        if (transferNormals)
            targetEntry.ioMask |= Mask::IOM_VERTNORMAL;
        if (transferSelection)
            targetEntry.ioMask |= Mask::IOM_VERTFLAGS;

        vcg::tri::SurfaceSampling<VCGMesh, LocalRedetailSampler>::VertexUniform(
            *targetWorldMesh,
            sampler,
            targetWorldMesh->VN(),
            onlySelected);

        if (transferGeometry || transferNormals)
            copyWorldGeometryBackToLocal(
                *targetWorldMesh,
                targetEntry.mesh,
                targetEntry.transform,
                transferGeometry,
                transferNormals);
        copyPerVertexAppearance(
            *targetWorldMesh,
            targetEntry.mesh,
            transferColor,
            transferQuality || qualityDistance,
            false,
            transferSelection);

        if (saveBarycentric) {
            if (!vertexSampling) {
                auto srcBary =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Point>(
                        *targetWorldMesh,
                        std::string("BarycentricCoords"));
                auto srcFaceIndex =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Scalar>(
                        *targetWorldMesh,
                        std::string("NearestFaceIndex"));
                auto dstBary =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Point>(
                        targetEntry.mesh,
                        std::string("BarycentricCoords"));
                auto dstFaceIndex =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Scalar>(
                        targetEntry.mesh,
                        std::string("NearestFaceIndex"));
                for (int i = 0; i < std::min(targetWorldMesh->VN(), targetEntry.mesh.VN()); ++i) {
                    dstBary[targetEntry.mesh.vert[i]] = srcBary[targetWorldMesh->vert[i]];
                    dstFaceIndex[targetEntry.mesh.vert[i]] = srcFaceIndex[targetWorldMesh->vert[i]];
                }
            } else {
                auto srcVertexIndex =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Scalar>(
                        *targetWorldMesh,
                        std::string("NearestVertexIndex"));
                auto dstVertexIndex =
                    vcg::tri::Allocator<VCGMesh>::GetPerVertexAttribute<Scalar>(
                        targetEntry.mesh,
                        std::string("NearestVertexIndex"));
                for (int i = 0; i < std::min(targetWorldMesh->VN(), targetEntry.mesh.VN()); ++i)
                    dstVertexIndex[targetEntry.mesh.vert[i]] = srcVertexIndex[targetWorldMesh->vert[i]];
            }
        }

        if (transferGeometry)
            vcg::tri::UpdateBounding<VCGMesh>::Box(targetEntry.mesh);

        if (transferGeometry) {
            doc.markMeshGeometryChanged(
                targetMeshIndex,
                QObject::tr("Transferred vertex attributes from '%1'").arg(sourceEntry.name));
        } else if (transferSelection) {
            doc.markMeshSelectionChanged(
                targetMeshIndex,
                QObject::tr("Transferred vertex selection from '%1'").arg(sourceEntry.name));
        } else {
            doc.markMeshGeometryChanged(
                targetMeshIndex,
                QObject::tr("Transferred vertex attributes from '%1'").arg(sourceEntry.name));
        }
        return modifiedResult(
            { QObject::tr("Transferred attributes from '%1' to '%2'.")
                  .arg(meshName(doc, sourceMeshIndex), meshName(doc, targetMeshIndex)) });
    }

    if (filterId == QString::fromLatin1(kFilterUniformMeshResampling)) {
        if (entry.mesh.FN() <= 0)
            return failResult(QObject::tr("Uniform mesh resampling requires faces."));

        const double cellSize = params.getDouble(QStringLiteral("CellSize"));
        if (!(cellSize > 0.0))
            return failResult(QObject::tr("Precision must be greater than zero."));

        const double offset = params.getDouble(QStringLiteral("Offset"));
        std::unique_ptr<VCGMesh> baseMesh = makePreparedSurfaceMesh(entry.mesh);
        VCGMesh output;
        vcg::Point3i volumeDim;
        vcg::Box3f volumeBox = baseMesh->bbox;
        volumeBox.Offset(volumeBox.Diag() / 10.0f + std::fabs(float(offset)));
        vcg::BestDim(volumeBox, float(cellSize), volumeDim);

        vcg::tri::Resampler<VCGMesh, VCGMesh>::Resample(
            *baseMesh,
            output,
            volumeBox,
            volumeDim,
            float(cellSize) * 3.5f,
            float(offset),
            params.getBool(QStringLiteral("discretize")),
            params.getBool(QStringLiteral("multisample")),
            params.getBool(QStringLiteral("absDist")),
            cb);
        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        if (params.getBool(QStringLiteral("mergeCloseVert"))) {
            const float mergeThreshold = output.bbox.Diag() / 10000.0f;
            vcg::tri::Clean<VCGMesh>::MergeCloseVertex(output, mergeThreshold);
        }
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexPerFace(output);

        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Offset Mesh"),
            Mask::IOM_VERTNORMAL);
        return successResult(
            {
                QObject::tr("Generated uniform resampled mesh with %1 vertices and %2 faces.")
                    .arg(output.VN())
                    .arg(output.FN())
            },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kFilterVoronoiColoring)) {
        const int coloredMeshIndex = meshIndexFromParam(params, QStringLiteral("ColoredMesh"), doc);
        const int vertexMeshIndex = meshIndexFromParam(params, QStringLiteral("VertexMesh"), doc);

        Document::MeshEntry &coloredEntry = doc.mesh(coloredMeshIndex);
        const Document::MeshEntry &vertexEntry = doc.mesh(vertexMeshIndex);
        std::unique_ptr<VCGMesh> coloredMesh = makeWorldMesh(coloredEntry, true);
        std::unique_ptr<VCGMesh> vertexMesh = makeWorldMesh(vertexEntry, false);

        vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(*coloredMesh);
        vcg::tri::Allocator<VCGMesh>::CompactVertexVector(*coloredMesh);
        vcg::tri::Allocator<VCGMesh>::CompactFaceVector(*coloredMesh);
        coloredMesh->vert.EnableVFAdjacency();
        coloredMesh->face.EnableVFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::VertexFace(*coloredMesh);

        std::vector<Point> pointVector;
        pointVector.reserve(vertexMesh->VN());
        for (const VCGVertex &vertex : vertexMesh->vert) {
            
                pointVector.push_back(vertex.cP());
        }

        std::vector<VCGMesh::VertexPointer> seedVertices;
        vcg::tri::VoronoiProcessing<VCGMesh>::SeedToVertexConversion(*coloredMesh, pointVector, seedVertices);
        vcg::tri::EuclideanDistance<VCGMesh> distanceFunctor;
        vcg::tri::VoronoiProcessing<VCGMesh>::ComputePerVertexSources(
            *coloredMesh,
            seedVertices,
            distanceFunctor);
        for (VCGMesh::VertexPointer seed : seedVertices)
            seed->C() = vcg::Color4b::Red;
        vcg::tri::VoronoiProcessing<VCGMesh>::VoronoiColoring(
            *coloredMesh,
            params.getBool(QStringLiteral("backward")));

        copyPerVertexAppearance(*coloredMesh, coloredEntry.mesh, true, true, false, false);
        coloredEntry.ioMask |= (Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY);
        doc.markMeshGeometryChanged(
            coloredMeshIndex,
            QObject::tr("Computed Voronoi coloring from '%1'").arg(vertexEntry.name));
        return modifiedResult(
            { QObject::tr("Colored '%1' from %2 seed points.")
                  .arg(meshName(doc, coloredMeshIndex))
                  .arg(seedVertices.size()) });
    }

    if (filterId == QString::fromLatin1(kFilterDiskColoring)) {
        const int coloredMeshIndex = meshIndexFromParam(params, QStringLiteral("ColoredMesh"), doc);
        const int vertexMeshIndex = meshIndexFromParam(params, QStringLiteral("VertexMesh"), doc);

        Document::MeshEntry &coloredEntry = doc.mesh(coloredMeshIndex);
        const Document::MeshEntry &vertexEntry = doc.mesh(vertexMeshIndex);
        std::unique_ptr<VCGMesh> coloredMesh = makeWorldMesh(coloredEntry, true);
        std::unique_ptr<VCGMesh> vertexMesh = makeWorldMesh(vertexEntry, false);

        using SampleSHT = vcg::SpatialHashTable<VCGVertex, Scalar>;
        SampleSHT spatialHash;
        vcg::tri::EmptyTMark<VCGMesh> markerFunctor;
        vcg::tri::UpdateColor<VCGMesh>::PerVertexConstant(*coloredMesh, vcg::Color4b::LightGray);
        vcg::tri::UpdateQuality<VCGMesh>::VertexConstant(
            *coloredMesh,
            std::numeric_limits<float>::max());
        spatialHash.Set(coloredMesh->vert.begin(), coloredMesh->vert.end());
        std::vector<VCGVertex *> closestVertices;

        float radius = float(params.getDouble(QStringLiteral("Radius")));
        const bool useSampleRadius = params.getBool(QStringLiteral("SampleRadius"));
        const bool approximateGeodetic = params.getBool(QStringLiteral("ApproximateGeodetic"));
        for (VCGVertex &seed : vertexMesh->vert) {
            Point point = seed.cP();
            if (useSampleRadius)
                radius = seed.Q();
            vcg::Box3f box(point - Point(radius, radius, radius), point + Point(radius, radius, radius));
            vcg::GridGetInBox(spatialHash, markerFunctor, box, closestVertices);

            for (VCGVertex *candidate : closestVertices) {
                float dist = approximateGeodetic
                    ? vcg::ApproximateGeodesicDistance(seed.cP(), seed.cN(), candidate->cP(), candidate->cN())
                    : vcg::Distance(point, candidate->cP());
                if (dist < radius && candidate->Q() > dist) {
                    candidate->Q() = dist;
                    candidate->C().lerp(vcg::Color4b::White, vcg::Color4b::Red, dist / radius);
                }
            }
        }

        copyPerVertexAppearance(*coloredMesh, coloredEntry.mesh, true, true, false, false);
        coloredEntry.ioMask |= (Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY);
        doc.markMeshGeometryChanged(
            coloredMeshIndex,
            QObject::tr("Computed disk coloring from '%1'").arg(vertexEntry.name));
        return modifiedResult(
            { QObject::tr("Applied disk coloring to '%1'.").arg(meshName(doc, coloredMeshIndex)) });
    }

    if (filterId == QString::fromLatin1(kFilterRegularRecursiveSampling)) {
        if (entry.mesh.FN() <= 0)
            return failResult(QObject::tr("Regular recursive sampling requires faces."));

        const double cellSize = params.getDouble(QStringLiteral("CellSize"));
        if (!(cellSize > 0.0))
            return failResult(QObject::tr("Precision must be greater than zero."));

        std::unique_ptr<VCGMesh> prepared = makePreparedSurfaceMesh(entry.mesh);
        vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(*prepared);
        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(*prepared);
        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(*prepared);
        // RegularRecursiveOffset walks a face grid with closest-point queries, which
        // mark the faces they visit.
        prepared->face.EnableMark();
        std::vector<Point> pointVector;
        vcg::tri::SurfaceSampling<VCGMesh, LocalRedetailSampler>::RegularRecursiveOffset(
            *prepared,
            pointVector,
            float(params.getDouble(QStringLiteral("Offset"))),
            float(cellSize));

        VCGMesh output;
        vcg::tri::BuildMeshFromCoordVector(output, pointVector);
        vcg::tri::UpdateBounding<VCGMesh>::Box(output);
        const int newIndex = addDerivedMesh(
            doc,
            meshIndex,
            output,
            QObject::tr("Recursive Samples"),
            0);
        return successResult(
            { QObject::tr("Generated %1 recursive samples.").arg(output.VN()) },
            { newIndex });
    }

    return failResult(QObject::tr("Unknown filter id: %1").arg(filterId));
}

void registerSamplingFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<SamplingFilterPlugin>());
}
