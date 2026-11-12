#include "tsp_adapter.h"

// Misha reports some errors with MK_ERROR_OUT, which prints and calls exit(0): in the
// application that would close MeshLab. Redefined before anything includes Misha, it throws a
// MishaK::Exception instead, which runFilter's caller turns into a failed run. The helper
// programs of TextureConsolidation do the same.
#define MK_ERROR_OUT( ... ) MK_THROW( __VA_ARGS__ )
// Misha's warnings go to stderr, one line per event: on a 20k-face atlas, thousands of "Zero area
// polygon" lines for the cells a triangle only touches, which TSP then skips. They describe its own
// bookkeeping, not the user's data, so in the application they are dropped.
#define MK_WARN( ... ) ((void)0)

#include <Src/PreProcessing.h>

#include <Eigen/Sparse>
#include <Misha/Exceptions.h>
#include <Misha/Texels.h>
#include <Src/GradientDomain.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <tuple>

namespace TspAdapter {

namespace {

using namespace MishaK;
using namespace MishaK::TSP;

using Real = double;
using Texel = Point<Real, 4>;

// Quadrature points per triangle for the mass and stiffness integrals, SetExteriorTexels' default.
constexpr unsigned int kQuadratureSamples = 6;

// TSP's library needs every chart away from the image border: the cells around a chart's
// boundary must be inside the grid ("Could not factor node index" otherwise). On a 20k-face
// xatlas atlas whose charts come within half a texel of the edge, one texel of margin was enough
// and none failed; charts are kept at least two texels from the edge. An atlas that already is
// gets no margin, as in TSP's programs (Src/Padding.h pads only what the coordinates need):
// rescaling the coordinates into a larger grid rounds them, which moves the texels whose centers
// lie on an edge from one side to the other, and changes the result there (2,296 of 1M texels on
// that atlas at 1024², up to the full range).
constexpr double kClearance = 2;

int marginFor(const Atlas &atlas, int width, int height)
{
    double clearance = kClearance;
    for (const std::array<double, 2> &t : atlas.uv)
        clearance = std::min({ clearance, t[0] * width, (1 - t[0]) * width, t[1] * height, (1 - t[1]) * height });
    return int(std::ceil(kClearance - clearance));
}

Texel texelOf(const Image &image, int x, int y)
{
    const std::array<double, 4> &v = image.texels[size_t(y) * size_t(image.width) + size_t(x)];
    return Texel(v[0], v[1], v[2], v[3]);
}

void setTexel(Image &image, int x, int y, const Texel &t)
{
    image.texels[size_t(y) * size_t(image.width) + size_t(x)] = { t[0], t[1], t[2], t[3] };
}

// TSP finds the charts through the texture vertices the triangles share, so corners with the same
// surface vertex and the same texture coordinates must be one texture vertex: with one per corner,
// every triangle would be a chart of its own. This is the fusion TSP's own PLY reader makes
// (ReadTexturedMesh in Src/MeshIO.h), on the same key.
struct FusedTexture
{
    std::vector<std::array<double, 2>> vertices;
    std::vector<unsigned int> cornerVertex; // 3 per triangle, into vertices
};

FusedTexture fuseTextureCoordinates(const Atlas &atlas)
{
    using Key = std::tuple<unsigned int, double, double>;
    std::vector<std::pair<Key, size_t>> corners(atlas.uv.size());
    for (size_t c = 0; c < atlas.uv.size(); ++c)
        corners[c] = { Key(atlas.triangles[c / 3][c % 3], atlas.uv[c][0], atlas.uv[c][1]), c };
    std::sort(corners.begin(), corners.end());
    FusedTexture fused;
    fused.cornerVertex.resize(atlas.uv.size());
    for (size_t i = 0; i < corners.size(); ++i) {
        if (i == 0 || corners[i].first != corners[i - 1].first)
            fused.vertices.push_back(atlas.uv[corners[i].second]);
        fused.cornerVertex[corners[i].second] = unsigned(fused.vertices.size() - 1);
    }
    return fused;
}

template<typename Function>
auto rethrown(Function &&function)
{
    try {
        return function();
    } catch (const MishaK::Exception &e) {
        throw std::runtime_error(e.what());
    }
}

} // namespace

std::size_t repairSeams(const Atlas &atlas, Image &image)
{
    return rethrown([&]() -> std::size_t {
        using Solver = Eigen::SimplicialLDLT<Eigen::SparseMatrix<Real>>;

        // The image grows by `margin` texels on every side, its edge texels repeated, and the
        // texture coordinates move with it; GradientDomain flips v itself, and a margin equal on
        // all sides commutes with that flip. Only the texels of the original image are written back.
        const int margin = marginFor(atlas, image.width, image.height);
        const int paddedWidth = image.width + 2 * margin;
        const int paddedHeight = image.height + 2 * margin;
        const FusedTexture texture = fuseTextureCoordinates(atlas);

        GradientDomain<Real> gd(
            kQuadratureSamples,
            atlas.triangles.size(),
            atlas.positions.size(),
            texture.vertices.size(),
            [&](size_t t, unsigned int k) { return size_t(atlas.triangles[t][k]); },
            [&](size_t v) { return Point<Real, 3>(atlas.positions[v][0], atlas.positions[v][1], atlas.positions[v][2]); },
            [&](size_t t, unsigned int k) { return size_t(texture.cornerVertex[3 * t + k]); },
            [&](size_t v) {
                return Point<Real, 2>((texture.vertices[v][0] * image.width + margin) / paddedWidth,
                                      (texture.vertices[v][1] * image.height + margin) / paddedHeight);
            },
            unsigned(paddedWidth),
            unsigned(paddedHeight),
            true,   // normalize
            false,  // sanityCheck
            false); // computeDivergence: only the stiffness matrix is used, and it is most of the memory

        // As SetExteriorTexels: with P the prolongation from the exterior texels to all of them,
        // minimize (x + P y)^T S (x + P y) over the exterior values y, the covered ones in x held
        // fixed, which gives y = -(P^T S P)^{-1} P^T S x.
        std::vector<size_t> exterior;
        for (size_t n = 0; n < gd.numNodes(); ++n)
            if (!gd.isCovered(n))
                exterior.push_back(n);
        if (exterior.empty())
            return 0;
        Eigen::SparseMatrix<Real> P(Eigen::Index(gd.numNodes()), Eigen::Index(exterior.size()));
        {
            std::vector<Eigen::Triplet<Real>> triplets;
            triplets.reserve(exterior.size());
            for (size_t e = 0; e < exterior.size(); ++e)
                triplets.emplace_back(int(exterior[e]), int(e), Real(1));
            P.setFromTriplets(triplets.begin(), triplets.end());
        }
        const Eigen::SparseMatrix<Real> RS = Eigen::SparseMatrix<Real>(P.transpose()) * gd.stiffness();
        Eigen::SparseMatrix<Real> RSP = RS * P;

        auto paddedTexel = [&](int x, int y) {
            return texelOf(image,
                           std::clamp(x - margin, 0, image.width - 1),
                           std::clamp(y - margin, 0, image.height - 1));
        };
        std::vector<Texel> x(gd.numNodes()), current(gd.numNodes());
        for (size_t n = 0; n < gd.numNodes(); ++n) {
            const std::pair<unsigned int, unsigned int> node = gd.node(n);
            current[n] = paddedTexel(int(node.first), int(node.second));
            if (gd.isCovered(n))
                x[n] = current[n];
        }

        // The energy reaches some exterior texels only barely: one whose cells a triangle only
        // grazes, one beside an island too small to cover a texel center. Their values are then
        // hardly determined, and the solve takes them far out of range (on a 3M-face scan, 10% of
        // the texels coupled less than 1e-6 of the median came out beyond [-10, 265], clamped to
        // white or black dots along the islands), or, coupled not at all, leaves the system
        // singular (every island of a per-triangle layout). So each exterior texel is also pulled
        // towards its current value, with a weight of 1e-2 of the median coupling: a texel the
        // energy barely reaches keeps about its value, and the others move by little. This is the
        // one change to SetExteriorTexels' method. Measured against no pull, as texels more than
        // 64 levels from the median of their 3x3 neighbors: 22,220 -> 2,628 on the 3M-face scan
        // at 4096², 5,452 -> 1,041 on a 20k-face xatlas atlas at 1024²; and the seam discrepancy
        // on the gargoyle test comes out lower, 28.03 against 28.26 (1e-4 gives 28.17 and 9,955
        // dots on the scan, 1e-1 28.11 and 1,149).
        constexpr Real kPull = 1e-2;
        Eigen::VectorXd coupling = RSP.diagonal().cwiseAbs();
        std::nth_element(coupling.data(), coupling.data() + coupling.size() / 2, coupling.data() + coupling.size());
        Real regularization = kPull * coupling[coupling.size() / 2];
        if (!(regularization > 0))
            regularization = kPull * std::max(coupling.maxCoeff(), Real(1));
        {
            Eigen::SparseMatrix<Real> identity(RSP.rows(), RSP.cols());
            identity.setIdentity();
            RSP += regularization * identity;
        }
        Solver solver(RSP);
        if (solver.info() != Eigen::Success)
            throw std::runtime_error("The system for the exterior texels could not be factored.");

        Eigen::VectorXd channel(Eigen::Index(gd.numNodes()));
        Eigen::VectorXd pull(Eigen::Index(exterior.size()));
        for (unsigned int c = 0; c < 4; ++c) {
            for (size_t n = 0; n < gd.numNodes(); ++n)
                channel[Eigen::Index(n)] = x[n][c];
            for (size_t e = 0; e < exterior.size(); ++e)
                pull[Eigen::Index(e)] = regularization * current[exterior[e]][c];
            const Eigen::VectorXd y = solver.solve(pull - RS * channel);
            for (size_t e = 0; e < exterior.size(); ++e)
                x[exterior[e]][c] = y[Eigen::Index(e)];
        }

        // TSP numbers the texels per chart, so a texel can be a node of two charts: covered by one
        // and just outside the other, where charts sit closer than two texels. Such a texel keeps
        // its value, since the chart covering it reads it. (SetExteriorTexels writes every node back
        // in turn, so there the last chart wins, and can overwrite a covered texel.) A texel outside
        // two charts takes the value of the last.
        std::vector<char> coveredTexel(size_t(image.width) * size_t(image.height), 0);
        auto inImage = [&](size_t n, int &px, int &py) {
            const std::pair<unsigned int, unsigned int> node = gd.node(n);
            px = int(node.first) - margin;
            py = int(node.second) - margin;
            return px >= 0 && py >= 0 && px < image.width && py < image.height;
        };
        int px = 0, py = 0;
        for (size_t n = 0; n < gd.numNodes(); ++n)
            if (gd.isCovered(n) && inImage(n, px, py))
                coveredTexel[size_t(py) * size_t(image.width) + size_t(px)] = 1;
        std::vector<char> written(coveredTexel.size(), 0);
        for (const size_t n : exterior) {
            if (!inImage(n, px, py))
                continue;
            const size_t t = size_t(py) * size_t(image.width) + size_t(px);
            if (coveredTexel[t])
                continue;
            setTexel(image, px, py, x[n]);
            written[t] = 1;
        }
        return std::size_t(std::count(written.begin(), written.end(), 1));
    });
}

std::size_t dilate(const Atlas &atlas, Image &image, unsigned int radius)
{
    return rethrown([&]() -> std::size_t {
        constexpr unsigned int K = 2;
        constexpr unsigned int Dim = 3;
        using TexelGrid = Texels<true, unsigned int>; // texel centers at the nodes, as TextureDilation
        using TexelInfo = typename TexelGrid::template TexelInfo<K>;

        const unsigned int res[] = { unsigned(image.width), unsigned(image.height) };
        auto vertex = [&](size_t v) {
            return Point<Real, Dim>(atlas.positions[v][0], atlas.positions[v][1], atlas.positions[v][2]);
        };
        auto simplexIndex = [&](size_t s) {
            return SimplexIndex<K>(atlas.triangles[s][0], atlas.triangles[s][1], atlas.triangles[s][2]);
        };
        auto textureSimplex = [&](size_t s) {
            Simplex<Real, K, K> simplex;
            for (unsigned int k = 0; k <= K; ++k)
                simplex[k] = Point<Real, K>(atlas.uv[3 * s + k][0], atlas.uv[3 * s + k][1]);
            return simplex;
        };

        const RegularGrid<K, TexelInfo> covered = TexelGrid::template GetSupportedTexelInfo<Dim, false>(
            atlas.triangles.size(), vertex, simplexIndex, textureSimplex, res, 0, false);
        const RegularGrid<K, TexelInfo> dilated = TexelGrid::template GetSupportedTexelInfo<Dim, false>(
            atlas.triangles.size(), vertex, simplexIndex, textureSimplex, res, radius, false);

        // TSP's grid has its row 0 at the bottom, as v grows, and the image at the top.
        RegularGrid<K, Texel> input;
        input.resize(res);
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                input(x, image.height - 1 - y) = texelOf(image, x, y);

        // As TextureDilation, but sampling `input`, which nothing writes: the program samples the
        // image it is filling, so a sample can read a texel another thread has just set.
        auto samplePosition = [&](const TexelInfo &info) {
            Simplex<Real, K, K> s = textureSimplex(info.sIdx);
            for (unsigned int k = 0; k <= K; ++k)
                for (unsigned int d = 0; d < K; ++d)
                    s[k][d] *= res[d];
            return s(info.bc);
        };
        std::size_t filled = 0;
        for (size_t i = 0; i < dilated.resolution(); ++i) {
            if (dilated[i].sIdx == static_cast<unsigned int>(-1) || covered[i].sIdx != static_cast<unsigned int>(-1))
                continue;
            const typename RegularGrid<K>::Index I = RegularGrid<K>::FactorIndex(i, res);
            setTexel(image, int(I[0]), image.height - 1 - int(I[1]), input(samplePosition(dilated[i])));
            ++filled;
        }
        return filled;
    });
}

} // namespace TspAdapter
