#include "poissonrecon_backend.h"

#include "document.h"
#include "poissonrecon_adapter.h"

#include "Src/Geometry.h"
#include "Src/MultiThreading.h"
#include "Src/Reconstructors.h"

#include <QByteArray>

#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>

#include <algorithm>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using namespace PoissonRecon;
using Mask = vcg::tri::io::Mask;

QString sourceRootPath()
{
    return QStringLiteral("plugins/filter_screened_poisson/Src");
}

QStringList keyEntryPoints()
{
    return {
        QStringLiteral("PoissonRecon.cpp"),
        QStringLiteral("SSDRecon.cpp"),
        QStringLiteral("SurfaceTrimmer.cpp"),
        QStringLiteral("Reconstructors.h"),
        QStringLiteral("Reconstructors.streams.h"),
        QStringLiteral("FEMTree.h"),
        QStringLiteral("DataStream.h"),
        QStringLiteral("MultiThreading.h"),
        QStringLiteral("../poissonrecon_adapter.h"),
        QStringLiteral("../poissonrecon_adapter.cpp")
    };
}

int intParameter(const MeshFilterParameterValues &params, const QString &id, int fallback)
{
    const auto it = params.constFind(id);
    if (it == params.constEnd())
        return fallback;
    bool ok = false;
    const int value = it.value().toInt(&ok);
    return ok ? value : fallback;
}

double doubleParameter(const MeshFilterParameterValues &params, const QString &id, double fallback)
{
    const auto it = params.constFind(id);
    if (it == params.constEnd())
        return fallback;
    bool ok = false;
    const double value = it.value().toDouble(&ok);
    return ok ? value : fallback;
}

bool boolParameter(const MeshFilterParameterValues &params, const QString &id, bool fallback)
{
    const auto it = params.constFind(id);
    if (it == params.constEnd())
        return fallback;
    if (it.value().userType() == QMetaType::Bool)
        return it.value().toBool();
    const QString text = it.value().toString().trimmed().toLower();
    if (text == QStringLiteral("true") || text == QStringLiteral("1"))
        return true;
    if (text == QStringLiteral("false") || text == QStringLiteral("0"))
        return false;
    return fallback;
}

void reportProgress(vcg::CallBackPos *cb, int pos, const QString &msg, bool replaceLast = false)
{
    if (!cb)
        return;
    const QByteArray raw = replaceLast
        ? (msg + QStringLiteral("\r")).toLocal8Bit()
        : msg.toLocal8Bit();
    cb(pos, raw.constData());
}

void configureThreadPool(int requestedThreads)
{
    using ThreadPool = PoissonRecon::ThreadPool;
    const unsigned int threads = static_cast<unsigned int>(std::max(1, requestedThreads));
    ThreadPool::SetNumThreads(threads);
    ThreadPool::ChunkSize = 128;
    ThreadPool::Schedule = ThreadPool::DYNAMIC;

#if defined(__APPLE__)
    ThreadPool::ParallelizationType =
        threads > 1 ? ThreadPool::ASYNC : ThreadPool::NONE;
#elif defined(_OPENMP)
    ThreadPool::ParallelizationType =
        threads > 1 ? ThreadPool::OPEN_MP : ThreadPool::NONE;
#else
    ThreadPool::ParallelizationType =
        threads > 1 ? ThreadPool::ASYNC : ThreadPool::NONE;
#endif
}

template<typename ImplicitT, typename VertexStreamT>
void extractLevelSet(
    const ImplicitT &implicit,
    bool linearFit,
    bool forceManifold,
    bool preserveDensity,
    int requestedThreads,
    VertexStreamT &vertexStream,
    ScreenedPoisson::VectorFaceStream &faceStream)
{
    // Level-set extraction has proven to be the brittle stage in optimized macOS
    // builds. Keep the solve parallel, but force extraction to a single worker.
    configureThreadPool(1);
    PoissonRecon::Reconstructor::LevelSetExtractionParameters params;
    params.linearFit = linearFit;
    params.outputGradients = true;
    params.forceManifold = forceManifold;
    params.polygonMesh = false;
    params.gridCoordinates = false;
    params.verbose = false;
    params.outputDensity = preserveDensity;
    implicit.extractLevelSet(vertexStream, faceStream, params);
    configureThreadPool(requestedThreads);
}

MeshFilterRunResult finalizeMeshResult(
    Document &doc,
    VCGMesh &outputMesh,
    bool preserveColor,
    const QString &finishMessage,
    const QStringList &infoMessages)
{
    if (outputMesh.VN() <= 0 || outputMesh.FN() <= 0) {
        doc.finishFilterProgress(false, QObject::tr("The filter produced an empty mesh."));
        return { false, false, QObject::tr("The filter produced an empty mesh.") };
    }

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(outputMesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(outputMesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(outputMesh);

    int ioMask = Mask::IOM_VERTQUALITY | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    if (preserveColor)
        ioMask |= Mask::IOM_VERTCOLOR;

    const int newIndex = doc.addMesh(outputMesh, {}, ioMask);
    if (newIndex < 0) {
        const QString message = QObject::tr("Failed to add the generated mesh to the document.");
        doc.finishFilterProgress(false, message);
        return { false, false, message };
    }

    doc.finishFilterProgress(true, finishMessage);
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.newMeshIndices = { newIndex };
    result.infoMessages = infoMessages;
    return result;
}

template<typename RealT, unsigned int DimT, typename... AuxData>
using ValuedPointData = DirectSum<RealT, Point<RealT, DimT>, RealT, AuxData...>;

template<typename Index>
size_t boostHash(Index i1, Index i2)
{
    size_t hash = static_cast<size_t>(i1) + 0x9e3779b9;
    hash ^= static_cast<size_t>(i2) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
    return hash;
}

template<typename Index>
struct EdgeKey
{
    Index key1;
    Index key2;

    EdgeKey(Index k1 = 0, Index k2 = 0)
    {
        if (k1 < k2)
            key1 = k1, key2 = k2;
        else
            key1 = k2, key2 = k1;
    }

    bool operator==(const EdgeKey &key) const { return key1 == key.key1 && key2 == key.key2; }
    struct Hasher {
        size_t operator()(const EdgeKey &key) const { return boostHash(key.key1, key.key2); }
    };
};

template<typename Index>
struct HalfEdgeKey
{
    Index key1;
    Index key2;

    HalfEdgeKey(Index k1 = 0, Index k2 = 0)
        : key1(k1)
        , key2(k2)
    {
    }

    HalfEdgeKey opposite(void) const { return HalfEdgeKey(key2, key1); }
    bool operator==(const HalfEdgeKey &key) const { return key1 == key.key1 && key2 == key.key2; }
    struct Hasher {
        size_t operator()(const HalfEdgeKey &key) const { return boostHash(key.key1, key.key2); }
    };
};

template<typename Index>
struct ComponentGraph
{
    struct Node
    {
        double area = 0;
        std::vector<Node *> neighbors;
        std::list<Index> polygonIndices;

        void merge(void)
        {
            auto popBack = [&](std::vector<Node *> &nodes, size_t idx) {
                nodes[idx] = nodes.back();
                nodes.pop_back();
            };

            if (neighbors.empty())
                return;

            for (unsigned int i = 0; i < neighbors.size(); ++i) {
                for (int j = int(neighbors[i]->neighbors.size()) - 1; j >= 0; --j) {
                    if (neighbors[i]->neighbors[j] == this)
                        popBack(neighbors[i]->neighbors, size_t(j));
                }
            }

            Node *first = neighbors[0];
            first->area += area;
            first->polygonIndices.splice(first->polygonIndices.end(), polygonIndices);

            for (unsigned int i = 1; i < neighbors.size(); ++i) {
                first->area += neighbors[i]->area;
                first->polygonIndices.splice(first->polygonIndices.end(), neighbors[i]->polygonIndices);
                for (unsigned int j = 0; j < neighbors[i]->neighbors.size(); ++j) {
                    bool foundNeighbor = false;
                    for (int k = int(neighbors[i]->neighbors[j]->neighbors.size()) - 1; k >= 0; --k) {
                        if (neighbors[i]->neighbors[j]->neighbors[k] == neighbors[i])
                            popBack(neighbors[i]->neighbors[j]->neighbors, size_t(k));
                    }
                    for (unsigned int k = 0; k < first->neighbors.size(); ++k)
                        foundNeighbor |= neighbors[i]->neighbors[j] == first->neighbors[k];
                    if (!foundNeighbor) {
                        first->neighbors.push_back(neighbors[i]->neighbors[j]);
                        neighbors[i]->neighbors[j]->neighbors.push_back(first);
                    }
                }

                neighbors[i]->area = 0;
                neighbors[i]->neighbors.clear();
            }

            polygonIndices.clear();
            area = 0;
        }
    };
};

template<typename RealT, unsigned int DimT, typename... AuxData>
ValuedPointData<RealT, DimT, AuxData...> interpolateVertices(
    const ValuedPointData<RealT, DimT, AuxData...> &v1,
    const ValuedPointData<RealT, DimT, AuxData...> &v2,
    RealT value)
{
    if (v1.template get<1>() == v2.template get<1>())
        return (v1 + v2) / RealT(2);
    const RealT dx = (v1.template get<1>() - value) / (v1.template get<1>() - v2.template get<1>());
    return v1 * (RealT(1) - dx) + v2 * dx;
}

template<typename RealT, unsigned int DimT, typename Index, class Vertex>
void splitPolygon(
    const std::vector<Index> &polygon,
    std::vector<Vertex> &vertices,
    std::vector<std::vector<Index>> *ltPolygons,
    std::vector<std::vector<Index>> *gtPolygons,
    std::vector<bool> *ltFlags,
    std::vector<bool> *gtFlags,
    std::unordered_map<EdgeKey<Index>, Index, typename EdgeKey<Index>::Hasher> &vertexTable,
    RealT trimValue)
{
    const int sz = int(polygon.size());
    std::vector<bool> gt((size_t)sz);
    int gtCount = 0;
    for (int j = 0; j < sz; ++j) {
        gt[size_t(j)] = (vertices[size_t(polygon[size_t(j)])].template get<1>() > trimValue);
        if (gt[size_t(j)])
            ++gtCount;
    }

    if (gtCount == sz) {
        if (gtPolygons)
            gtPolygons->push_back(polygon);
        if (gtFlags)
            gtFlags->push_back(false);
    } else if (gtCount == 0) {
        if (ltPolygons)
            ltPolygons->push_back(polygon);
        if (ltFlags)
            ltFlags->push_back(false);
    } else {
        int start;
        for (start = 0; start < sz; ++start) {
            if (gt[size_t(start)] && !gt[size_t((start + sz - 1) % sz)])
                break;
        }

        bool gtFlag = true;
        std::vector<Index> poly;
        {
            const int j1 = (start + sz - 1) % sz;
            const int j2 = start;
            const Index v1 = polygon[size_t(j1)];
            const Index v2 = polygon[size_t(j2)];
            Index vIdx;
            const auto iter = vertexTable.find(EdgeKey<Index>(v1, v2));
            if (iter == vertexTable.end()) {
                vertexTable[EdgeKey<Index>(v1, v2)] = vIdx = Index(vertices.size());
                vertices.push_back(interpolateVertices(vertices[size_t(v1)], vertices[size_t(v2)], trimValue));
            } else {
                vIdx = iter->second;
            }
            poly.push_back(vIdx);
        }

        for (int _j = 0; _j <= sz; ++_j) {
            const int j1 = (_j + start + sz - 1) % sz;
            const int j2 = (_j + start) % sz;
            const Index v1 = polygon[size_t(j1)];
            const Index v2 = polygon[size_t(j2)];
            if (gt[size_t(j2)] == gtFlag) {
                poly.push_back(v2);
            } else {
                Index vIdx;
                const auto iter = vertexTable.find(EdgeKey<Index>(v1, v2));
                if (iter == vertexTable.end()) {
                    vertexTable[EdgeKey<Index>(v1, v2)] = vIdx = Index(vertices.size());
                    vertices.push_back(interpolateVertices(vertices[size_t(v1)], vertices[size_t(v2)], trimValue));
                } else {
                    vIdx = iter->second;
                }
                poly.push_back(vIdx);
                if (gtFlag) {
                    if (gtPolygons)
                        gtPolygons->push_back(poly);
                    if (ltFlags)
                        ltFlags->push_back(true);
                } else {
                    if (ltPolygons)
                        ltPolygons->push_back(poly);
                    if (gtFlags)
                        gtFlags->push_back(true);
                }
                poly.clear();
                poly.push_back(vIdx);
                poly.push_back(v2);
                gtFlag = !gtFlag;
            }
        }
    }
}

template<typename RealT, unsigned int DimT, typename Index, class Vertex>
void triangulate(
    const std::vector<Vertex> &vertices,
    const std::vector<std::vector<Index>> &polygons,
    std::vector<std::vector<Index>> &triangles)
{
    triangles.clear();
    for (size_t i = 0; i < polygons.size(); ++i) {
        if (polygons[i].size() > 3) {
            std::vector<Point<RealT, DimT>> polygonVertices(polygons[i].size());
            for (int j = 0; j < int(polygons[i].size()); ++j)
                polygonVertices[size_t(j)] = vertices[size_t(polygons[i][size_t(j)])].template get<0>();
            const std::vector<TriangleIndex<Index>> localTriangles = MinimalAreaTriangulation<Index, RealT, DimT>(
                reinterpret_cast<ConstPointer(Point<RealT, DimT>)>(GetPointer(polygonVertices)),
                polygonVertices.size());
            const size_t base = triangles.size();
            triangles.resize(base + localTriangles.size());
            for (size_t j = 0; j < localTriangles.size(); ++j) {
                triangles[base + j].resize(3);
                for (int k = 0; k < 3; ++k)
                    triangles[base + j][size_t(k)] = polygons[i][size_t(localTriangles[j].idx[k])];
            }
        } else if (polygons[i].size() == 3) {
            triangles.push_back(polygons[i]);
        }
    }
}

template<typename RealT, unsigned int DimT, typename Index, class Vertex>
double polygonArea(const std::vector<Vertex> &vertices, const std::vector<Index> &polygon)
{
    auto triangleArea = [](Point<RealT, DimT> v1, Point<RealT, DimT> v2, Point<RealT, DimT> v3) {
        Point<RealT, DimT> v[] = { v2 - v1, v3 - v1 };
        XForm<RealT, 2> mass;
        for (int i = 0; i < 2; ++i)
            for (int j = 0; j < 2; ++j)
                mass(i, j) = Point<RealT, DimT>::Dot(v[i], v[j]);
        const double det = mass.determinant();
        if (det < 0)
            return RealT(0);
        return RealT(std::sqrt(det) / 2.0);
    };

    if (polygon.size() < 3)
        return 0.0;
    if (polygon.size() == 3) {
        return triangleArea(
            vertices[size_t(polygon[0])].template get<0>(),
            vertices[size_t(polygon[1])].template get<0>(),
            vertices[size_t(polygon[2])].template get<0>());
    }

    Point<RealT, DimT> center;
    for (size_t i = 0; i < polygon.size(); ++i)
        center += vertices[size_t(polygon[i])].template get<0>();
    center /= RealT(polygon.size());

    double area = 0;
    for (size_t i = 0; i < polygon.size(); ++i) {
        area += triangleArea(
            center,
            vertices[size_t(polygon[i])].template get<0>(),
            vertices[size_t(polygon[(i + 1) % polygon.size()])].template get<0>());
    }
    return area;
}

template<typename Index, class Vertex>
void removeHangingVertices(std::vector<Vertex> &vertices, std::vector<std::vector<Index>> &polygons)
{
    std::unordered_map<Index, Index> vMap;
    std::vector<bool> vertexFlags(vertices.size(), false);
    for (size_t i = 0; i < polygons.size(); ++i)
        for (size_t j = 0; j < polygons[i].size(); ++j)
            vertexFlags[size_t(polygons[i][j])] = true;

    Index vCount = 0;
    for (Index i = 0; i < Index(vertices.size()); ++i)
        if (vertexFlags[size_t(i)])
            vMap[i] = vCount++;

    for (size_t i = 0; i < polygons.size(); ++i)
        for (size_t j = 0; j < polygons[i].size(); ++j)
            polygons[i][j] = vMap[polygons[i][j]];

    std::vector<Vertex> compactVertices((size_t)vCount);
    for (Index i = 0; i < Index(vertices.size()); ++i)
        if (vertexFlags[size_t(i)])
            compactVertices[size_t(vMap[i])] = vertices[size_t(i)];
    vertices = std::move(compactVertices);
}

template<typename Index>
void setConnectedComponents(const std::vector<std::vector<Index>> &polygons, std::vector<std::vector<Index>> &components)
{
    std::vector<Index> polygonRoots(polygons.size());
    for (size_t i = 0; i < polygons.size(); ++i)
        polygonRoots[i] = Index(i);

    std::unordered_map<EdgeKey<Index>, Index, typename EdgeKey<Index>::Hasher> edgeTable;
    for (size_t i = 0; i < polygons.size(); ++i) {
        const int sz = int(polygons[i].size());
        for (int j = 0; j < sz; ++j) {
            const Index v1 = polygons[i][size_t(j)];
            const Index v2 = polygons[i][size_t((j + 1) % sz)];
            const EdgeKey<Index> eKey(v1, v2);
            const auto iter = edgeTable.find(eKey);
            if (iter == edgeTable.end()) {
                edgeTable[eKey] = Index(i);
            } else {
                Index p = iter->second;
                while (polygonRoots[size_t(p)] != p) {
                    const Index temp = polygonRoots[size_t(p)];
                    polygonRoots[size_t(p)] = Index(i);
                    p = temp;
                }
                polygonRoots[size_t(p)] = Index(i);
            }
        }
    }

    for (size_t i = 0; i < polygonRoots.size(); ++i) {
        Index p = Index(i);
        while (polygonRoots[size_t(p)] != p)
            p = polygonRoots[size_t(p)];
        const Index root = p;
        p = Index(i);
        while (polygonRoots[size_t(p)] != p) {
            const Index temp = polygonRoots[size_t(p)];
            polygonRoots[size_t(p)] = root;
            p = temp;
        }
    }

    int cCount = 0;
    std::unordered_map<Index, Index> vMap;
    for (Index i = 0; i < Index(polygonRoots.size()); ++i)
        if (polygonRoots[size_t(i)] == i)
            vMap[i] = Index(cCount++);

    components.clear();
    components.resize(size_t(cCount));
    for (Index i = 0; i < Index(polygonRoots.size()); ++i)
        components[size_t(vMap[polygonRoots[size_t(i)]])].push_back(i);
}

template<bool PreserveColor, class Vertex>
void appendTrimmedVerticesToMesh(const std::vector<Vertex> &vertices, const std::vector<std::vector<int>> &polygons, VCGMesh &mesh)
{
    const int baseVertexIndex = mesh.VN();
    for (const Vertex &vertex : vertices) {
        const auto &p = vertex.template get<0>();
        vcg::tri::Allocator<VCGMesh>::AddVertex(mesh, vcg::Point3f(p[0], p[1], p[2]));
        mesh.vert.back().Q() = vertex.template get<1>();
        if constexpr (PreserveColor) {
            const auto &c = vertex.template get<2>();
            mesh.vert.back().C()[0] = static_cast<unsigned char>(std::clamp(c[0], 0.0f, 255.0f));
            mesh.vert.back().C()[1] = static_cast<unsigned char>(std::clamp(c[1], 0.0f, 255.0f));
            mesh.vert.back().C()[2] = static_cast<unsigned char>(std::clamp(c[2], 0.0f, 255.0f));
            mesh.vert.back().C()[3] = 255;
        }
    }

    for (const auto &polygon : polygons) {
        if (polygon.size() != 3)
            continue;
        vcg::tri::Allocator<VCGMesh>::AddFace(
            mesh,
            &mesh.vert[baseVertexIndex + polygon[0]],
            &mesh.vert[baseVertexIndex + polygon[1]],
            &mesh.vert[baseVertexIndex + polygon[2]]);
    }
}

template<typename Vertex>
void trimConnectedComponents(
    std::vector<Vertex> &vertices,
    std::vector<std::vector<int>> &ltPolygons,
    std::vector<std::vector<int>> &gtPolygons,
    double islandAreaRatio,
    bool removeIslands)
{
    if (islandAreaRatio <= 0.0)
        return;

    std::vector<std::vector<int>> allPolygons;
    std::vector<std::vector<int>> allComponents;
    std::vector<std::vector<int>> ltComponents, gtComponents;
    setConnectedComponents(ltPolygons, ltComponents);
    setConnectedComponents(gtPolygons, gtComponents);

    const size_t gtComponentStart = ltComponents.size();
    for (unsigned int i = 0; i < gtComponents.size(); ++i)
        for (unsigned int j = 0; j < gtComponents[i].size(); ++j)
            gtComponents[i][j] += int(ltPolygons.size());

    allPolygons.reserve(ltPolygons.size() + gtPolygons.size());
    allComponents.reserve(ltComponents.size() + gtComponents.size());
    allPolygons.insert(allPolygons.end(), ltPolygons.begin(), ltPolygons.end());
    allPolygons.insert(allPolygons.end(), gtPolygons.begin(), gtPolygons.end());
    allComponents.insert(allComponents.end(), ltComponents.begin(), ltComponents.end());
    allComponents.insert(allComponents.end(), gtComponents.begin(), gtComponents.end());

    std::vector<typename ComponentGraph<int>::Node> nodes(allComponents.size());
    for (unsigned int i = 0; i < allComponents.size(); ++i) {
        nodes[i].polygonIndices.insert(nodes[i].polygonIndices.end(), allComponents[i].begin(), allComponents[i].end());
        for (auto it = nodes[i].polygonIndices.begin(); it != nodes[i].polygonIndices.end(); ++it)
            nodes[i].area += polygonArea<float, 3, int, Vertex>(vertices, allPolygons[size_t(*it)]);
    }

    std::unordered_map<HalfEdgeKey<int>, int, typename HalfEdgeKey<int>::Hasher> componentBoundaryHalfEdges;
    for (unsigned int i = 0; i < allComponents.size(); ++i) {
        std::unordered_set<HalfEdgeKey<int>, typename HalfEdgeKey<int>::Hasher> componentHalfEdges;
        for (unsigned int j = 0; j < allComponents[i].size(); ++j) {
            const auto &poly = allPolygons[size_t(allComponents[i][j])];
            for (unsigned int k = 0; k < poly.size(); ++k)
                componentHalfEdges.insert(HalfEdgeKey<int>(poly[k], poly[(k + 1) % poly.size()]));
        }
        for (const auto &key : componentHalfEdges)
            if (componentHalfEdges.find(key.opposite()) == componentHalfEdges.end())
                componentBoundaryHalfEdges[key] = int(i);
    }

    std::unordered_set<EdgeKey<int>, typename EdgeKey<int>::Hasher> componentEdges;
    for (const auto &entry : componentBoundaryHalfEdges) {
        const auto opposite = componentBoundaryHalfEdges.find(entry.first.opposite());
        if (opposite != componentBoundaryHalfEdges.end())
            componentEdges.insert(EdgeKey<int>(entry.second, opposite->second));
    }
    for (const auto &edge : componentEdges) {
        nodes[size_t(edge.key1)].neighbors.push_back(&nodes[size_t(edge.key2)]);
        nodes[size_t(edge.key2)].neighbors.push_back(&nodes[size_t(edge.key1)]);
    }

    double totalArea = 0;
    for (const auto &node : nodes)
        totalArea += node.area;

    bool done = false;
    while (!done) {
        done = true;
        unsigned int idx = static_cast<unsigned int>(-1);
        for (unsigned int i = 0; i < nodes.size(); ++i) {
            if (!nodes[i].polygonIndices.empty() && !nodes[i].neighbors.empty()) {
                if (idx == static_cast<unsigned int>(-1) || nodes[i].area < nodes[idx].area)
                    idx = i;
            }
        }
        if (idx != static_cast<unsigned int>(-1) && nodes[idx].area < totalArea * islandAreaRatio) {
            nodes[idx].merge();
            done = false;
        }
    }

    ltPolygons.clear();
    gtPolygons.clear();
    for (unsigned int i = 0; i < gtComponentStart; ++i) {
        if (!nodes[i].neighbors.empty() || nodes[i].area >= totalArea * islandAreaRatio || !removeIslands) {
            for (auto it = nodes[i].polygonIndices.begin(); it != nodes[i].polygonIndices.end(); ++it)
                ltPolygons.push_back(allPolygons[size_t(*it)]);
        }
    }
    for (unsigned int i = static_cast<unsigned int>(gtComponentStart); i < nodes.size(); ++i) {
        if (!nodes[i].neighbors.empty() || nodes[i].area >= totalArea * islandAreaRatio || !removeIslands) {
            for (auto it = nodes[i].polygonIndices.begin(); it != nodes[i].polygonIndices.end(); ++it)
                gtPolygons.push_back(allPolygons[size_t(*it)]);
        }
    }
}

template<typename Vertex, bool PreserveColor>
MeshFilterRunResult runSurfaceTrimmerImpl(
    Document &doc,
    int meshIndex,
    const MeshFilterParameterValues &parameters)
{
    Document::MeshEntry &entry = doc.mesh(meshIndex);
    const auto &mesh = entry.mesh;
    if (mesh.FN() <= 0)
        return { false, false, QObject::tr("Surface Trimmer requires a mesh with faces.") };

    const float trimValue = float(doubleParameter(parameters, QStringLiteral("trim"), 0.0));
    const double islandAreaRatio = doubleParameter(parameters, QStringLiteral("islandAreaRatio"), 0.001);
    const bool removeIslands = boolParameter(parameters, QStringLiteral("removeIslands"), false);
    const bool polygonMeshRequested = boolParameter(parameters, QStringLiteral("polygonMesh"), false);

    vcg::CallBackPos *cb = doc.progressCallback();
    const QString progressLabel = QObject::tr("Trim Surface by Scalar Isovalue");
    doc.beginFilterProgress(progressLabel);
    reportProgress(cb, 0, QObject::tr("Preparing Surface Trimmer input..."), true);

    std::vector<Vertex> vertices;
    std::vector<int> vertexMap(mesh.vert.size(), -1);
    vertices.reserve(size_t(mesh.VN()));
    // Read now, while every vertex is still in the list: removeHangingVertices below
    // empties it when nothing survives, which is exactly when the range is worth saying.
    float scalarMin = std::numeric_limits<float>::max();
    float scalarMax = std::numeric_limits<float>::lowest();
    for (size_t i = 0; i < mesh.vert.size(); ++i) {
        const VCGVertex &v = mesh.vert[i];
        vertexMap[i] = int(vertices.size());
        Vertex outVertex;
        const auto pos = vcg::Point3f(v.cP());
        outVertex.template get<0>()[0] = pos[0];
        outVertex.template get<0>()[1] = pos[1];
        outVertex.template get<0>()[2] = pos[2];
        outVertex.template get<1>() = v.cQ();
        scalarMin = std::min(scalarMin, v.cQ());
        scalarMax = std::max(scalarMax, v.cQ());
        if constexpr (PreserveColor) {
            outVertex.template get<2>()[0] = float(v.C()[0]);
            outVertex.template get<2>()[1] = float(v.C()[1]);
            outVertex.template get<2>()[2] = float(v.C()[2]);
        }
        vertices.push_back(outVertex);
    }

    std::vector<std::vector<int>> polygons;
    polygons.reserve(size_t(mesh.FN()));
    for (const VCGFace &f : mesh.face) {
        const int v0 = vertexMap[vcg::tri::Index(mesh, f.cV(0))];
        const int v1 = vertexMap[vcg::tri::Index(mesh, f.cV(1))];
        const int v2 = vertexMap[vcg::tri::Index(mesh, f.cV(2))];
        if (v0 < 0 || v1 < 0 || v2 < 0)
            continue;
        polygons.push_back({ v0, v1, v2 });
    }

    if (polygons.empty()) {
        const QString message = QObject::tr("Surface Trimmer could not read any valid faces from the current mesh.");
        doc.finishFilterProgress(false, message);
        return { false, false, message };
    }

    reportProgress(cb, 25, QObject::tr("Splitting mesh polygons along trim threshold..."), true);
    std::unordered_map<EdgeKey<int>, int, typename EdgeKey<int>::Hasher> vertexTable;
    std::vector<std::vector<int>> ltPolygons, gtPolygons;
    std::vector<bool> ltFlags, gtFlags;
    for (const auto &polygon : polygons)
        splitPolygon<float, 3, int, Vertex>(polygon, vertices, &ltPolygons, &gtPolygons, &ltFlags, &gtFlags, vertexTable, trimValue);

    reportProgress(cb, 60, QObject::tr("Cleaning disconnected trimmed components..."), true);
    trimConnectedComponents(vertices, ltPolygons, gtPolygons, islandAreaRatio, removeIslands);

    reportProgress(cb, 80, QObject::tr("Triangulating trimmed output..."), true);
    std::vector<std::vector<int>> triangles;
    triangulate<float, 3, int, Vertex>(vertices, gtPolygons, triangles);
    gtPolygons = std::move(triangles);
    removeHangingVertices(vertices, gtPolygons);

    // Everything below the threshold is discarded, so a threshold above the scalar's
    // maximum discards the mesh -- and `entry.mesh.Clear()` a line down is unconditional,
    // so by the time anyone noticed, the layer was gone and reported as a success. Refuse
    // instead, and say what range the threshold has to fall in to do anything useful.
    if (gtPolygons.empty()) {
        const QString message = QObject::tr(
            "Trim Surface by Scalar Isovalue would remove the whole mesh: the threshold is "
            "%1 and the vertex scalar only spans %2 to %3. The mesh was left unchanged.")
            .arg(trimValue).arg(scalarMin).arg(scalarMax);
        doc.finishFilterProgress(false, message);
        return { false, false, message };
    }

    entry.mesh.Clear();
    appendTrimmedVerticesToMesh<PreserveColor>(vertices, gtPolygons, entry.mesh);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(entry.mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(entry.mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry.mesh);
    entry.ioMask |= Mask::IOM_VERTQUALITY | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
    if constexpr (PreserveColor)
        entry.ioMask |= Mask::IOM_VERTCOLOR;

    const QString contextMessage =
        QObject::tr("Trimmed mesh '%1' (%2 vertices, %3 faces).")
            .arg(entry.name)
            .arg(entry.mesh.VN())
            .arg(entry.mesh.FN());
    doc.markMeshGeometryChanged(meshIndex, contextMessage);

    QStringList infoMessages;
    infoMessages.push_back(contextMessage);
    if (polygonMeshRequested) {
        infoMessages.push_back(
            QObject::tr("The original SurfaceTrimmer can preserve polygon output. MeshLab stores triangle meshes, so the result was triangulated."));
    }
    doc.finishFilterProgress(true, QObject::tr("Trimmed current mesh."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = infoMessages;
    return result;
}

} // namespace

namespace ScreenedPoisson
{

BackendStatus inspectBackend()
{
    BackendStatus status;
    status.sourceRoot = sourceRootPath();
    status.keyEntryPoints = keyEntryPoints();
    status.vendoredSourcesPresent = true;
    status.summary = status.vendoredSourcesPresent
        ? QStringLiteral("Vendored PoissonRecon sources are available.")
        : QStringLiteral("Vendored PoissonRecon sources are not complete.");
    return status;
}

QString placeholderErrorMessage()
{
    return QStringLiteral("The PoissonRecon backend is not available.");
}

bool isEnabledByEnvironment()
{
    const QByteArray primary = qgetenv("MESHLAB2_POISSONRECON").trimmed();
    const QByteArray legacy = qgetenv("MESHLAB2_POISSON_UPSTREAM").trimmed();
    const QByteArray value = primary.isEmpty() ? legacy : primary;
    if (value.isEmpty())
        return true;
    const QByteArray normalized = value.toLower();
    return !(normalized == "0" || normalized == "false" || normalized == "no" || normalized == "off");
}

MeshFilterRunResult runScreenedPoissonFilter(
    Document &doc,
    const std::vector<int> &meshIndices,
    bool mergeVisible,
    const MeshFilterParameterValues &parameters)
{
    if (meshIndices.empty()) {
        return {
            false,
            false,
            mergeVisible
                ? QObject::tr("No visible meshes available for Screened Poisson reconstruction.")
                : QObject::tr("No current mesh selected.")
        };
    }

    const bool preserveColor = parameters.contains(QStringLiteral("preserveColor"))
        ? parameters.value(QStringLiteral("preserveColor")).toBool()
        : allSelectedMeshesHaveVertexColor(doc, meshIndices);
    const bool confidence = parameters.value(QStringLiteral("confidence"), false).toBool();
    const int requestedThreads = std::max(1, intParameter(parameters, QStringLiteral("threads"), 1));
    const qsizetype inputSampleCount = countInputSamples(doc, meshIndices);
    vcg::CallBackPos *cb = doc.progressCallback();
    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Screened Poisson"));
    reportProgress(
        cb,
        0,
        mergeVisible
            ? QObject::tr("Preparing Screened Poisson input from %1 visible layers (%2 samples)...")
                  .arg(meshIndices.size())
                  .arg(inputSampleCount)
            : QObject::tr("Preparing Screened Poisson input (%1 samples)...").arg(inputSampleCount),
        true);

    configureThreadPool(requestedThreads);

    namespace Reconstructor = PoissonRecon::Reconstructor;
    using Signature = PoissonRecon::IsotropicUIntPack<
        Dim,
        PoissonRecon::FEMDegreeAndBType<
            Reconstructor::Poisson::DefaultFEMDegree,
            Reconstructor::Poisson::DefaultFEMBoundary>::Signature>;

    Reconstructor::Poisson::SolutionParameters<Real> solveParams;
    solveParams.verbose = false;
    solveParams.dirichletErode = false;
    solveParams.exactInterpolation = false;
    solveParams.showResidual = false;
    solveParams.confidence = confidence;
    solveParams.scale = Real(std::max(0.1, doubleParameter(parameters, QStringLiteral("scale"), 1.1)));
    solveParams.width = Real(0);
    solveParams.lowDepthCutOff = Real(0);
    solveParams.samplesPerNode = Real(std::max(0.01, doubleParameter(parameters, QStringLiteral("samplesPerNode"), 1.5)));
    solveParams.cgSolverAccuracy = Real(1e-3);
    solveParams.perLevelDataScaleFactor = Real(32);
    solveParams.depth = static_cast<unsigned int>(std::max(1, intParameter(parameters, QStringLiteral("depth"), 8)));
    solveParams.solveDepth = solveParams.depth;
    solveParams.baseDepth = static_cast<unsigned int>(std::max(0, intParameter(parameters, QStringLiteral("cgDepth"), 0)));
    solveParams.fullDepth = static_cast<unsigned int>(std::clamp(intParameter(parameters, QStringLiteral("fullDepth"), 5), 1, int(solveParams.depth)));
    solveParams.kernelDepth = static_cast<unsigned int>(-1);
    solveParams.baseVCycles = 1;
    solveParams.iters = static_cast<unsigned int>(std::max(1, intParameter(parameters, QStringLiteral("iters"), 8)));
    solveParams.alignDir = 0;
    solveParams.pointWeight = Real(std::max(0.0, doubleParameter(parameters, QStringLiteral("pointWeight"), 4.0)));
    solveParams.valueInterpolationWeight = Real(0);

    SelectionOptions selection;
    selection.mergeVisible = mergeVisible;
    selection.confidenceFromQuality = confidence;

    VCGMesh outputMesh;
    try {
        reportProgress(
            cb,
            10,
            QObject::tr("Screened Poisson: solving implicit field (%1 thread%2)...")
                .arg(requestedThreads)
                .arg(requestedThreads == 1 ? QString() : QStringLiteral("s")),
            true);

        if (preserveColor) {
            using Solver = Reconstructor::Poisson::Solver<Real, Dim, Signature, Color>;
            DocumentOrientedPointColorStream pointStream(doc, meshIndices, selection);
            std::unique_ptr<Reconstructor::Implicit<Real, Dim, Signature, Color>> implicit(
                Solver::Solve(pointStream, solveParams, Color()));
            reportProgress(cb, 75, QObject::tr("Screened Poisson: extracting iso-surface (forcing 1 thread for stability)..."), true);
            VectorLevelSetVertexColorStream vertexStream;
            VectorFaceStream faceStream;
            extractLevelSet(*implicit, false, true, true, requestedThreads, vertexStream, faceStream);
            reportProgress(cb, 92, QObject::tr("Screened Poisson: assembling mesh..."), true);
            appendToMesh(vertexStream.vertices(), faceStream.faces(), outputMesh);
        } else {
            using Solver = Reconstructor::Poisson::Solver<Real, Dim, Signature>;
            DocumentOrientedPointStream pointStream(doc, meshIndices, selection);
            std::unique_ptr<Reconstructor::Implicit<Real, Dim, Signature>> implicit(
                Solver::Solve(pointStream, solveParams));
            reportProgress(cb, 75, QObject::tr("Screened Poisson: extracting iso-surface (forcing 1 thread for stability)..."), true);
            VectorLevelSetVertexStream vertexStream;
            VectorFaceStream faceStream;
            extractLevelSet(*implicit, false, true, true, requestedThreads, vertexStream, faceStream);
            reportProgress(cb, 92, QObject::tr("Screened Poisson: assembling mesh..."), true);
            appendToMesh(vertexStream.vertices(), faceStream.faces(), outputMesh);
        }
    } catch (const std::exception &ex) {
        const QString message = QObject::tr("Screened Poisson failed: %1").arg(QString::fromUtf8(ex.what()));
        doc.finishFilterProgress(false, message);
        return { false, false, message };
    }

    reportProgress(cb, 97, QObject::tr("Screened Poisson: finalizing mesh..."), true);
    return finalizeMeshResult(
        doc,
        outputMesh,
        preserveColor,
        mergeVisible
            ? QObject::tr("Reconstructed from %1 visible layers").arg(meshIndices.size())
            : QObject::tr("Reconstructed from the current layer"),
        {
            // The layer is named by the framework, which reports it separately; this says
            // what the backend did, not what the result is called.
            mergeVisible
                ? QObject::tr("PoissonRecon over %1 visible layers: %2 input samples, "
                              "%3 vertices, %4 faces.")
                      .arg(meshIndices.size())
                      .arg(inputSampleCount)
                      .arg(outputMesh.VN())
                      .arg(outputMesh.FN())
                : QObject::tr("PoissonRecon: %1 input samples, %2 vertices, %3 faces.")
                      .arg(inputSampleCount)
                      .arg(outputMesh.VN())
                      .arg(outputMesh.FN()),
            QObject::tr("Screened Poisson used %1 thread%2 for the solve and forced 1 thread "
                        "for iso-surface extraction stability.")
                .arg(requestedThreads)
                .arg(requestedThreads == 1 ? QString() : QStringLiteral("s"))
        });
}

MeshFilterRunResult runSSDReconFilter(
    Document &doc,
    const std::vector<int> &meshIndices,
    bool mergeVisible,
    const MeshFilterParameterValues &parameters)
{
    if (meshIndices.empty()) {
        return {
            false,
            false,
            mergeVisible
                ? QObject::tr("No visible meshes available for SSD reconstruction.")
                : QObject::tr("No current mesh selected.")
        };
    }

    const bool preserveColor = parameters.contains(QStringLiteral("preserveColor"))
        ? boolParameter(parameters, QStringLiteral("preserveColor"), true)
        : allSelectedMeshesHaveVertexColor(doc, meshIndices);
    const bool confidence = boolParameter(parameters, QStringLiteral("confidence"), false);
    const bool exactInterpolation = boolParameter(parameters, QStringLiteral("exactInterpolation"), false);
    const bool nonLinearFit = boolParameter(parameters, QStringLiteral("nonLinearFit"), false);
    const bool nonManifold = boolParameter(parameters, QStringLiteral("nonManifold"), false);
    const int requestedThreads = std::max(1, intParameter(parameters, QStringLiteral("threads"), 1));
    const qsizetype inputSampleCount = countInputSamples(doc, meshIndices);
    vcg::CallBackPos *cb = doc.progressCallback();
    doc.beginFilterProgress(QObject::tr("Reconstruct Surface by Smooth Signed Distance"));
    reportProgress(
        cb,
        0,
        mergeVisible
            ? QObject::tr("Preparing SSD input from %1 visible layers (%2 samples)...")
                  .arg(meshIndices.size())
                  .arg(inputSampleCount)
            : QObject::tr("Preparing SSD input (%1 samples)...").arg(inputSampleCount),
        true);

    configureThreadPool(requestedThreads);

    namespace Reconstructor = PoissonRecon::Reconstructor;
    using Signature = PoissonRecon::IsotropicUIntPack<
        Dim,
        PoissonRecon::FEMDegreeAndBType<
            Reconstructor::SSD::DefaultFEMDegree,
            Reconstructor::SSD::DefaultFEMBoundary>::Signature>;

    Reconstructor::SSD::SolutionParameters<Real> solveParams;
    solveParams.verbose = false;
    solveParams.exactInterpolation = exactInterpolation;
    solveParams.showResidual = false;
    solveParams.confidence = confidence;
    solveParams.scale = Real(std::max(0.1, doubleParameter(parameters, QStringLiteral("scale"), 1.1)));
    solveParams.lowDepthCutOff = Real(0);
    solveParams.width = Real(0);
    solveParams.pointWeight = Real(std::max(0.0, doubleParameter(parameters, QStringLiteral("valueWeight"), 1.0)) * Reconstructor::SSD::WeightMultipliers[0]);
    solveParams.gradientWeight = Real(std::max(1e-6, doubleParameter(parameters, QStringLiteral("gradientWeight"), 1.0)) * Reconstructor::SSD::WeightMultipliers[1]);
    solveParams.biLapWeight = Real(std::max(1e-6, doubleParameter(parameters, QStringLiteral("biLapWeight"), 1.0)) * Reconstructor::SSD::WeightMultipliers[2]);
    solveParams.samplesPerNode = Real(std::max(0.01, doubleParameter(parameters, QStringLiteral("samplesPerNode"), 1.5)));
    solveParams.cgSolverAccuracy = Real(std::clamp(doubleParameter(parameters, QStringLiteral("cgAccuracy"), 1e-3), 1e-8, 1.0));
    solveParams.perLevelDataScaleFactor = Real(std::max(0.0, doubleParameter(parameters, QStringLiteral("dataScale"), 32.0)));
    solveParams.depth = static_cast<unsigned int>(std::max(1, intParameter(parameters, QStringLiteral("depth"), 8)));
    solveParams.solveDepth = solveParams.depth;
    solveParams.baseDepth = static_cast<unsigned int>(std::max(0, intParameter(parameters, QStringLiteral("baseDepth"), 0)));
    solveParams.fullDepth = static_cast<unsigned int>(std::clamp(intParameter(parameters, QStringLiteral("fullDepth"), 5), 1, int(solveParams.depth)));
    solveParams.kernelDepth = static_cast<unsigned int>(-1);
    solveParams.baseVCycles = 1;
    solveParams.iters = static_cast<unsigned int>(std::max(1, intParameter(parameters, QStringLiteral("iters"), 8)));
    solveParams.alignDir = 0;

    SelectionOptions selection;
    selection.mergeVisible = mergeVisible;
    selection.confidenceFromQuality = confidence;

    VCGMesh outputMesh;
    try {
        reportProgress(
            cb,
            10,
            QObject::tr("SSD reconstruction: solving implicit field (%1 thread%2)...")
                .arg(requestedThreads)
                .arg(requestedThreads == 1 ? QString() : QStringLiteral("s")),
            true);

        if (preserveColor) {
            using Solver = Reconstructor::SSD::Solver<Real, Dim, Signature, Color>;
            DocumentOrientedPointColorStream pointStream(doc, meshIndices, selection);
            std::unique_ptr<Reconstructor::Implicit<Real, Dim, Signature, Color>> implicit(
                Solver::Solve(pointStream, solveParams, Color()));
            reportProgress(cb, 75, QObject::tr("SSD reconstruction: extracting iso-surface (forcing 1 thread for stability)..."), true);
            VectorLevelSetVertexColorStream vertexStream;
            VectorFaceStream faceStream;
            extractLevelSet(*implicit, !nonLinearFit, !nonManifold, true, requestedThreads, vertexStream, faceStream);
            reportProgress(cb, 92, QObject::tr("SSD reconstruction: assembling mesh..."), true);
            appendToMesh(vertexStream.vertices(), faceStream.faces(), outputMesh);
        } else {
            using Solver = Reconstructor::SSD::Solver<Real, Dim, Signature>;
            DocumentOrientedPointStream pointStream(doc, meshIndices, selection);
            std::unique_ptr<Reconstructor::Implicit<Real, Dim, Signature>> implicit(
                Solver::Solve(pointStream, solveParams));
            reportProgress(cb, 75, QObject::tr("SSD reconstruction: extracting iso-surface (forcing 1 thread for stability)..."), true);
            VectorLevelSetVertexStream vertexStream;
            VectorFaceStream faceStream;
            extractLevelSet(*implicit, !nonLinearFit, !nonManifold, true, requestedThreads, vertexStream, faceStream);
            reportProgress(cb, 92, QObject::tr("SSD reconstruction: assembling mesh..."), true);
            appendToMesh(vertexStream.vertices(), faceStream.faces(), outputMesh);
        }
    } catch (const std::exception &ex) {
        const QString message = QObject::tr("SSD reconstruction failed: %1").arg(QString::fromUtf8(ex.what()));
        doc.finishFilterProgress(false, message);
        return { false, false, message };
    }

    reportProgress(cb, 97, QObject::tr("SSD reconstruction: finalizing mesh..."), true);
    return finalizeMeshResult(
        doc,
        outputMesh,
        preserveColor,
        mergeVisible
            ? QObject::tr("Reconstructed from %1 visible layers").arg(meshIndices.size())
            : QObject::tr("Reconstructed from the current layer"),
        {
            mergeVisible
                ? QObject::tr("SSDRecon over %1 visible layers: %2 input samples, "
                              "%3 vertices, %4 faces.")
                      .arg(meshIndices.size())
                      .arg(inputSampleCount)
                      .arg(outputMesh.VN())
                      .arg(outputMesh.FN())
                : QObject::tr("SSDRecon: %1 input samples, %2 vertices, %3 faces.")
                      .arg(inputSampleCount)
                      .arg(outputMesh.VN())
                      .arg(outputMesh.FN()),
            QObject::tr("SSDRecon used %1 thread%2 for the solve and forced 1 thread "
                        "for iso-surface extraction stability.")
                .arg(requestedThreads)
                .arg(requestedThreads == 1 ? QString() : QStringLiteral("s"))
        });
}

MeshFilterRunResult runSurfaceTrimmerFilter(
    Document &doc,
    int meshIndex,
    const MeshFilterParameterValues &parameters)
{
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return { false, false, QObject::tr("No current mesh selected.") };

    const bool preserveColor = parameters.contains(QStringLiteral("preserveColor"))
        ? boolParameter(parameters, QStringLiteral("preserveColor"), true)
        : (doc.mesh(meshIndex).ioMask & Mask::IOM_VERTCOLOR) != 0;
    try {
        if (preserveColor) {
            using Vertex = ValuedPointData<float, 3, Point<float, 3>>;
            return runSurfaceTrimmerImpl<Vertex, true>(doc, meshIndex, parameters);
        }
        using Vertex = ValuedPointData<float, 3>;
        return runSurfaceTrimmerImpl<Vertex, false>(doc, meshIndex, parameters);
    } catch (const std::exception &ex) {
        const QString message = QObject::tr("Surface Trimmer failed: %1").arg(QString::fromUtf8(ex.what()));
        doc.finishFilterProgress(false, message);
        return { false, false, message };
    }
}

} // namespace ScreenedPoisson
