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
#include <random>
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

// The atlas as TSP is given it: the image grows by `margin` texels on every side, its edge texels
// repeated, and the texture coordinates move with it. GradientDomain flips v itself, and a margin
// equal on all sides commutes with that flip. Only the texels of the original image are written
// back (writeBack).
struct PaddedAtlas
{
    const Atlas &atlas;
    const Image &image;
    const int margin;
    const int width;
    const int height;
    const FusedTexture texture;

    PaddedAtlas(const Atlas &a, const Image &i)
        : atlas(a), image(i), margin(marginFor(a, i.width, i.height)),
          width(i.width + 2 * margin), height(i.height + 2 * margin), texture(fuseTextureCoordinates(a))
    {
    }

    Point<Real, 2> padded(const std::array<double, 2> &t) const
    {
        return Point<Real, 2>((t[0] * image.width + margin) / width, (t[1] * image.height + margin) / height);
    }
    // The functors GradientDomain's constructors take.
    auto surfaceCorner() const { return [this](size_t t, unsigned int k) { return size_t(atlas.triangles[t][k]); }; }
    auto surfaceVertex() const
    {
        return [this](size_t v) { return Point<Real, 3>(atlas.positions[v][0], atlas.positions[v][1], atlas.positions[v][2]); };
    }
    auto textureCorner() const { return [this](size_t t, unsigned int k) { return size_t(texture.cornerVertex[3 * t + k]); }; }
    auto textureVertex() const { return [this](size_t v) { return padded(texture.vertices[v]); }; }
    // The value of a texel of the padded grid.
    Texel texel(int x, int y) const
    {
        return texelOf(image, std::clamp(x - margin, 0, image.width - 1), std::clamp(y - margin, 0, image.height - 1));
    }
    template<typename GD>
    Texel nodeTexel(const GD &gd, size_t n) const
    {
        const std::pair<unsigned int, unsigned int> node = gd.node(n);
        return texel(int(node.first), int(node.second));
    }
};

// Puts node values back into the image. TSP numbers the texels per chart, so a texel can be a node
// of two charts: covered by one and just outside the other, where charts sit closer than two
// texels. Such a texel never takes the value of the chart it lies outside, since the chart
// covering it reads it (SetExteriorTexels writes every node back in turn, so there the last chart
// wins, and can overwrite a covered texel); with `covered`, it takes the covering chart's. A texel
// outside two charts takes the value of the last. Only the nodes `allowed` lets through are
// written, renormalized with `normals` (see Image). Returns how many texels were.
template<typename GD, typename Allowed>
std::size_t writeBack(const GD &gd, const std::vector<Texel> &x, const PaddedAtlas &p, Image &image, bool covered,
                      bool normals, Allowed &&allowed)
{
    auto renormalized = [](Texel t) {
        Point<Real, 3> v(t[0] / 127.5 - 1, t[1] / 127.5 - 1, t[2] / 127.5 - 1);
        const Real length = Point<Real, 3>::Length(v);
        if (length > 0)
            for (unsigned int c = 0; c < 3; ++c)
                t[c] = (v[c] / length + 1) * 127.5;
        return t;
    };
    std::vector<char> coveredTexel(size_t(image.width) * size_t(image.height), 0);
    auto inImage = [&](size_t n, size_t &t) {
        const std::pair<unsigned int, unsigned int> node = gd.node(n);
        const int px = int(node.first) - p.margin, py = int(node.second) - p.margin;
        t = size_t(py) * size_t(image.width) + size_t(px);
        return px >= 0 && py >= 0 && px < image.width && py < image.height;
    };
    size_t t = 0;
    for (size_t n = 0; n < gd.numNodes(); ++n)
        if (gd.isCovered(n) && inImage(n, t))
            coveredTexel[t] = 1;
    std::vector<char> written(coveredTexel.size(), 0);
    auto write = [&](size_t n) {
        const std::pair<unsigned int, unsigned int> node = gd.node(n);
        setTexel(image, int(node.first) - p.margin, int(node.second) - p.margin, normals ? renormalized(x[n]) : x[n]);
        written[t] = 1;
    };
    for (size_t n = 0; n < gd.numNodes(); ++n)
        if (!gd.isCovered(n) && allowed(n) && inImage(n, t) && !coveredTexel[t])
            write(n);
    if (covered)
        for (size_t n = 0; n < gd.numNodes(); ++n)
            if (gd.isCovered(n) && allowed(n) && inImage(n, t))
                write(n);
    return std::size_t(std::count(written.begin(), written.end(), 1));
}

// Whether each node is selected: whether the triangle nearest to its texel center is. Through the
// lists of candidate triangles (UseVector): the other path rasterizes the triangles on several
// threads and keeps, for a texel several of them support, whichever was written last, which is
// neither the nearest nor the same from run to run, and at the edge of a selection decides which
// side a texel is on (20 texels of 31,000 changed between runs, inpainting a 20k-face atlas). The
// candidates' nearest is taken with ties broken by index, so the result is repeatable.
template<typename GD>
std::vector<char> selectedNodes(const GD &gd, const PaddedAtlas &p, const std::vector<char> &selectedTriangles)
{
    using TexelGrid = Texels<true, unsigned int>;
    const unsigned int res[] = { unsigned(p.width), unsigned(p.height) };
    const RegularGrid<2, TexelGrid::TexelInfo<2>> nearest = TexelGrid::GetSupportedTexelInfo<false, 2, true>(
        p.atlas.triangles.size(),
        [&](size_t s) {
            Simplex<Real, 2, 2> simplex;
            for (unsigned int k = 0; k < 3; ++k)
                simplex[k] = p.padded(p.atlas.uv[3 * s + k]);
            return simplex;
        },
        res, 2, false);
    std::vector<char> selected(gd.numNodes(), 0);
    for (size_t n = 0; n < gd.numNodes(); ++n) {
        // The texel grid has its row 0 at the bottom, GradientDomain's nodes at the top.
        const std::pair<unsigned int, unsigned int> node = gd.node(n);
        const unsigned int s = nearest(node.first, unsigned(p.height) - 1 - node.second).sIdx;
        selected[n] = s != static_cast<unsigned int>(-1) && selectedTriangles[s];
    }
    return selected;
}

// The node values that minimize the Dirichlet energy x^T S x of the texture on the surface over the
// `free` nodes, the others held at their current values, as SetExteriorTexels and SeamStitcher do:
// with P the prolongation from the free nodes to all of them and x the held values (zero at the free
// nodes), minimize (x + P y)^T S (x + P y), which gives y = -(P^T S P)^{-1} P^T S x. Each free node n is
// also pulled towards its current value, with a weight of pull(n, c) times the median coupling, c
// being the node's own coupling relative to that median, which keeps the system regular where the
// energy barely reaches a node, or does not reach it at all.
template<typename GD, typename Pull>
std::vector<Texel> minimizeDirichlet(const GD &gd, const PaddedAtlas &p, const std::vector<size_t> &free, Pull &&pull)
{
    using Solver = Eigen::SimplicialLDLT<Eigen::SparseMatrix<Real>>;
    Eigen::SparseMatrix<Real> P(Eigen::Index(gd.numNodes()), Eigen::Index(free.size()));
    {
        std::vector<Eigen::Triplet<Real>> triplets;
        triplets.reserve(free.size());
        for (size_t f = 0; f < free.size(); ++f)
            triplets.emplace_back(int(free[f]), int(f), Real(1));
        P.setFromTriplets(triplets.begin(), triplets.end());
    }
    const Eigen::SparseMatrix<Real> RS = Eigen::SparseMatrix<Real>(P.transpose()) * gd.stiffness();
    Eigen::SparseMatrix<Real> RSP = RS * P;

    std::vector<Texel> x(gd.numNodes());
    for (size_t n = 0; n < gd.numNodes(); ++n)
        x[n] = p.nodeTexel(gd, n);
    const std::vector<Texel> current = x;
    for (size_t n : free)
        x[n] = Texel();

    const Eigen::VectorXd diagonal = RSP.diagonal().cwiseAbs();
    Eigen::VectorXd coupling = diagonal;
    std::nth_element(coupling.data(), coupling.data() + coupling.size() / 2, coupling.data() + coupling.size());
    Real median = coupling[coupling.size() / 2];
    if (!(median > 0))
        median = std::max(coupling.maxCoeff(), Real(1));
    Eigen::VectorXd weight(Eigen::Index(free.size()));
    for (size_t f = 0; f < free.size(); ++f)
        weight[Eigen::Index(f)] = pull(free[f], diagonal[Eigen::Index(f)] / median) * median;
    {
        Eigen::SparseMatrix<Real> diagonal(RSP.rows(), RSP.cols());
        std::vector<Eigen::Triplet<Real>> triplets;
        triplets.reserve(free.size());
        for (size_t f = 0; f < free.size(); ++f)
            triplets.emplace_back(int(f), int(f), weight[Eigen::Index(f)]);
        diagonal.setFromTriplets(triplets.begin(), triplets.end());
        RSP += diagonal;
    }
    Solver solver(RSP);
    if (solver.info() != Eigen::Success)
        throw std::runtime_error("The system for the texels to set could not be factored.");

    Eigen::VectorXd channel(Eigen::Index(gd.numNodes()));
    Eigen::VectorXd target(Eigen::Index(free.size()));
    std::vector<Texel> solved(free.size());
    for (unsigned int c = 0; c < 4; ++c) {
        for (size_t n = 0; n < gd.numNodes(); ++n)
            channel[Eigen::Index(n)] = x[n][c];
        for (size_t f = 0; f < free.size(); ++f)
            target[Eigen::Index(f)] = weight[Eigen::Index(f)] * current[free[f]][c];
        const Eigen::VectorXd y = solver.solve(target - RS * channel);
        for (size_t f = 0; f < free.size(); ++f)
            solved[f][c] = y[Eigen::Index(f)];
    }
    for (size_t f = 0; f < free.size(); ++f)
        x[free[f]] = solved[f];
    return x;
}

} // namespace

std::size_t repairSeams(const Atlas &atlas, Image &image)
{
    return rethrown([&]() -> std::size_t {
        const PaddedAtlas p(atlas, image);
        GradientDomain<Real> gd(
            kQuadratureSamples, atlas.triangles.size(), atlas.positions.size(), p.texture.vertices.size(),
            p.surfaceCorner(), p.surfaceVertex(), p.textureCorner(), p.textureVertex(),
            unsigned(p.width), unsigned(p.height),
            true,   // normalize
            false,  // sanityCheck
            false); // computeDivergence: only the stiffness matrix is used, and it is most of the memory

        // As SetExteriorTexels: the exterior texels are solved for, the covered ones held.
        std::vector<size_t> exterior;
        for (size_t n = 0; n < gd.numNodes(); ++n)
            if (!gd.isCovered(n))
                exterior.push_back(n);
        if (exterior.empty())
            return 0;

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
        const std::vector<Texel> x = minimizeDirichlet(gd, p, exterior, [](size_t, Real) { return kPull; });
        return writeBack(gd, x, p, image, false, false, [](size_t) { return true; });
    });
}

std::size_t inpaint(const Atlas &atlas, Image &image, const std::vector<char> &selectedTriangles, bool normals)
{
    return rethrown([&]() -> std::size_t {
        const PaddedAtlas p(atlas, image);
        GradientDomain<Real> gd(
            kQuadratureSamples, atlas.triangles.size(), atlas.positions.size(), p.texture.vertices.size(),
            p.surfaceCorner(), p.surfaceVertex(), p.textureCorner(), p.textureVertex(),
            unsigned(p.width), unsigned(p.height),
            true, false, false); // normalize, no sanity check, no divergence operator

        // The region is freed, and so are the exterior texels outside it, which are not written:
        // held, the background of the gutter around the region would bleed into it as a boundary
        // value. Free, they take the values the surface gives them, as in the repair.
        const std::vector<char> selected = selectedNodes(gd, p, selectedTriangles);
        std::vector<size_t> free;
        for (size_t n = 0; n < gd.numNodes(); ++n)
            if (selected[n] || !gd.isCovered(n))
                free.push_back(n);
        if (std::find(selected.begin(), selected.end(), 1) == selected.end())
            return 0;

        // Nothing in the region may be held back towards what was there (the stain being removed):
        // the repair's pull, 1e-2 of the median coupling on every exterior texel, adds up along the
        // seams of a large region and kept 15,022 of 34,090 texels of a region painted magenta near
        // magenta on the gargoyle test. Without any pull, the texels the energy barely reaches come
        // out far out of range (up to 1,150 levels on a 3M-face scan at 4096²). So only the texels
        // coupled less than 1e-3 of the median take the repair's pull; the others are only
        // regularized, which lets a region with no held texel around it, a whole connected piece of
        // the surface selected, keep its mean. Measured, as threshold: magenta texels left on the
        // test, rms against the photo there (26.4 with no pull), values out of range on the scan:
        // 0: 0, 26.4, 45; 1e-4: 2, 26.4, 37; 1e-3: 89, 27.8, 31; 1e-2: 478, 38.1, 23.
        constexpr Real kRegularization = 1e-8;
        constexpr Real kWeak = 1e-3;
        constexpr Real kPull = 1e-2;
        std::vector<Texel> x = minimizeDirichlet(
            gd, p, free, [&](size_t, Real coupling) { return coupling < kWeak ? kPull : kRegularization; });
        // Alpha is coverage, not a signal to fill: the texels keep theirs, as in TSP's programs,
        // which read color only. Solved with the rest, the transparent gutter would bleed into it.
        for (size_t n = 0; n < gd.numNodes(); ++n)
            x[n][3] = p.nodeTexel(gd, n)[3];
        return writeBack(gd, x, p, image, true, normals, [&](size_t n) { return selected[n] != 0; });
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

namespace {

using Hierarchy = HierarchicalGradientDomain<Real, Eigen::SimplicialLDLT<Eigen::SparseMatrix<Real>>, Texel>;

// TextureFiltering's defaults: four levels, three Gauss-Seidel sweeps per level. The solve converges
// slowly on fragmented atlases, and the change per V-cycle understates the error left, by about 40
// times at 2048² (cycle 40 changes a texel by 0.05 levels at most and is still 2 from the direct
// solve). So the cycles run until one changes no texel by more than a fiftieth of a level, which
// agreed with the direct solve within one level at 1024² (61 cycles) and 2048² (89); a 3M-face scan
// at 4096² takes about 160.
constexpr unsigned int kLevels = 4;
constexpr unsigned int kSweeps = 3;
constexpr unsigned int kMaxVCycles = 300;
constexpr Real kConverged = 0.02;

// Runs V-cycles on the system set up in `gd` until converged, or until the cap. Returns false if
// `onVCycle` stopped it, with `outcome.canceled` set.
bool runVCycles(Hierarchy &gd, FilterOutcome &outcome, const std::function<bool(unsigned int)> &onVCycle)
{
    const size_t nodes = gd.numNodes();
    std::vector<Texel> previous(nodes);
    while (outcome.vCycles < kMaxVCycles && !outcome.converged) {
        std::copy(gd.x(), gd.x() + nodes, previous.begin());
        gd.vCycle(kSweeps);
        ++outcome.vCycles;
        Real change = 0;
        for (size_t n = 0; n < nodes; ++n)
            for (unsigned int c = 0; c < 3; ++c) // alpha is not written back
                change = std::max(change, std::abs(gd.x(n)[c] - previous[n][c]));
        outcome.converged = change < kConverged;
        if (onVCycle && !onVCycle(outcome.vCycles)) {
            outcome.canceled = true;
            return false;
        }
    }
    return true;
}

} // namespace

FilterOutcome filter(const Atlas &atlas, Image &image, double fidelity, double gradientScale,
                     const std::vector<char> *selectedTriangles, const Image *detail, bool normals,
                     const std::function<bool(unsigned int)> &onVCycle)
{
    return rethrown([&]() -> FilterOutcome {
        const bool localized = selectedTriangles != nullptr;
        const PaddedAtlas p(atlas, image);
        Hierarchy gd(
            kQuadratureSamples, atlas.triangles.size(), atlas.positions.size(), p.texture.vertices.size(),
            p.surfaceCorner(), p.surfaceVertex(), p.textureCorner(), p.textureVertex(),
            unsigned(p.width), unsigned(p.height), kLevels,
            true,       // normalize
            false,      // sanityCheck
            localized); // computeDivergence: only to scale the gradients texel by texel
        const size_t nodes = gd.numNodes();

        // held gives the values, x0 the gradients: the same texture unless a `detail` is given.
        std::vector<Texel> held(nodes), x0(nodes);
        for (size_t n = 0; n < nodes; ++n)
            held[n] = p.nodeTexel(gd, n);
        if (detail) {
            const PaddedAtlas d(atlas, *detail);
            for (size_t n = 0; n < nodes; ++n)
                x0[n] = d.nodeTexel(gd, n);
        } else {
            x0 = held;
        }

        const std::vector<char> selectedNode = localized ? selectedNodes(gd, p, *selectedTriangles) : std::vector<char>();

        // TextureFiltering's system: the values held to the input (`held`) with weight `fidelity`, the
        // gradients to the input's (x0's) scaled by `gradientScale`. Uniformly scaled, the divergence of
        // the target gradients is gradientScale S x0; scaled around the selection only, it is the
        // divergence of the edge differences, each scaled by the mean of its two ends' factors.
        std::vector<Texel> mass(nodes), gradients(nodes);
        gd.mass(&held[0], &mass[0]);
        if (!localized) {
            gd.stiffness(&x0[0], &gradients[0]);
            for (Texel &g : gradients)
                g *= gradientScale;
        } else {
            std::vector<Texel> differences(gd.numEdges());
            for (size_t e = 0; e < gd.numEdges(); ++e) {
                const std::pair<size_t, size_t> ends = gd.edge(e);
                const Real scale = 1 + (gradientScale - 1) * (selectedNode[ends.first] + selectedNode[ends.second]) / 2.0;
                differences[e] = (x0[ends.second] - x0[ends.first]) * scale;
            }
            gd.divergence(&differences[0], &gradients[0]);
        }
        for (size_t n = 0; n < nodes; ++n) {
            gd.x(n) = held[n];
            gd.b(n) = mass[n] * fidelity + gradients[n];
        }
        gd.updateSystem(fidelity, 1);

        FilterOutcome outcome;
        if (!runVCycles(gd, outcome, onVCycle))
            return outcome;

        // Alpha is coverage, not a signal to filter: the texels keep theirs, as in TextureFiltering,
        // which reads color only. Filtered with the rest, the transparent gutter of a projected
        // texture bled into it: smoothing left 162,929 of 162,932 opaque texels partly transparent.
        std::vector<Texel> x(gd.x(), gd.x() + nodes);
        for (size_t n = 0; n < nodes; ++n)
            x[n][3] = held[n][3];
        outcome.texels = writeBack(gd, x, p, image, true, normals,
                                   [&](size_t n) { return !localized || selectedNode[n]; });
        return outcome;
    });
}

FilterOutcome lineIntegralConvolution(const Atlas &atlas, Image &image, const std::vector<std::array<double, 3>> &field,
                                      double streakLength, double sharpenFidelity, double sharpenScale,
                                      double anisotropyExponent, Noise noise, unsigned int seed,
                                      const std::function<bool(unsigned int)> &onVCycle)
{
    return rethrown([&]() -> FilterOutcome {
        {
            const PaddedAtlas p(atlas, image);
            const size_t triangles = atlas.triangles.size();

            // As LineIntegralConvolution: each triangle's metric, the Gram matrix of its two edges from
            // its first corner on the surface scaled to unit area, and the field in that basis, scaled
            // to unit root-mean-square length.
            ExplicitIndexVector<AtlasMeshTriangleIndex, SquareMatrix<Real, 2>> metric(triangles), anisotropic;
            ExplicitIndexVector<AtlasMeshTriangleIndex, Point2D<Real>> vf(triangles);
            Real area = 0;
            for (size_t t = 0; t < triangles; ++t) {
                const AtlasMeshTriangleIndex i(static_cast<unsigned int>(t));
                Point3D<Real> v[3];
                for (unsigned int k = 0; k < 3; ++k) {
                    const std::array<double, 3> &q = atlas.positions[atlas.triangles[t][k]];
                    v[k] = Point3D<Real>(q[0], q[1], q[2]);
                }
                const Point3D<Real> d[] = { v[1] - v[0], v[2] - v[0] };
                const Point3D<Real> f(field[t][0], field[t][1], field[t][2]);
                for (unsigned int j = 0; j < 2; ++j)
                    for (unsigned int k = 0; k < 2; ++k)
                        metric[i](j, k) = Point3D<Real>::Dot(d[j], d[k]);
                vf[i] = metric[i].inverse() * Point2D<Real>(Point3D<Real>::Dot(d[0], f), Point3D<Real>::Dot(d[1], f));
                area += Point3D<Real>::Length(Point3D<Real>::CrossProduct(d[0], d[1])) / 2;
            }
            Real norm = 0, total = 0;
            for (size_t t = 0; t < triangles; ++t) {
                const AtlasMeshTriangleIndex i(static_cast<unsigned int>(t));
                metric[i] /= area;
                const Real a = std::sqrt(metric[i].determinant()) / 2;
                norm += Point2D<Real>::Dot(vf[i], metric[i] * vf[i]) * a;
                total += a;
            }
            norm = std::sqrt(norm / total);
            if (norm > 0)
                for (size_t t = 0; t < triangles; ++t)
                    vf[AtlasMeshTriangleIndex(static_cast<unsigned int>(t))] /= norm;
            // The metric stretched across the field, so that diffusion runs along it: the program's
            // anisotropy, |v|^aExp * 1e5 across the field, none where the field vanishes.
            InitializeVectorFieldMetric(metric, vf, [&](Real length) { return Real(std::pow(length, anisotropyExponent) * 1e5); },
                                        true, anisotropic);

            // The weight that gives streaks of `streakLength`. The anisotropic metric, rescaled to unit
            // area, shortens lengths along the field by s = (1 + 1e5)^(1/4), so the solve averages the
            // noise along the field over a decay length l = s sqrt(A) / sqrt(w), A the surface's area
            // in the mesh's units: the screened Poisson kernel falls as exp(-d / l). The correlation of
            // noise so averaged falls to half at 1.678 l on either side, (1 + x) e^(-x) = 1/2, and the
            // streak's length is taken as that whole width, 3.357 l. Measured on a cylinder whose
            // field runs around it, black and white noise, sharpening off: half the correlation at
            // 1.70 l at w = 1e4 and 1.88 l at w = 1e5, as the texels grow coarse against l; the
            // default sharpening shortens it to 1.30 l and 1.38 l. The program's default w = 1e4 is
            // a length of 0.60 sqrt(A). With an anisotropy exponent above 0, it holds where the field
            // has its root-mean-square length.
            if (!(streakLength > 0))
                throw std::runtime_error("The streak length must be positive.");
            const double decay = streakLength / (2 * 1.6783);
            const double streakFidelity = area * std::sqrt(1 + 1e5) / (decay * decay);

            GradientDomain<Real> gd(
                kQuadratureSamples, triangles, atlas.positions.size(), p.texture.vertices.size(),
                p.surfaceCorner(), [&](size_t t) { return anisotropic[AtlasMeshTriangleIndex(static_cast<unsigned int>(t))]; },
                p.textureCorner(), p.textureVertex(), unsigned(p.width), unsigned(p.height),
                true, false, false); // normalize, no sanity check, no divergence operator
            const size_t nodes = gd.numNodes();

            // The noise, one value per texel drawn from a generator of its own so that the seed decides
            // it: the program's random hue at full saturation and value, or black or white, or a random
            // gray, the classic noises of line-integral convolution. Then it is diffused along the field,
            // (w M_a + S_a) x = w M_a n, solved directly, as the program does with --useDirectSolver.
            // Multigrid does not converge under this anisotropy: on a 20k-face atlas at 1024², 300
            // V-cycles still changed texels by 9 levels and left them up to 862 from the direct
            // solution, which took less time (15 s with the sharpening, against 49 s).
            std::mt19937 generator(seed);
            std::vector<Texel> signal(nodes), mass(nodes);
            for (Texel &t : signal) {
                const double u = double(generator()) / double(std::mt19937::max());
                if (noise == Noise::Hue) {
                    const double hsv[] = { u * 2 * M_PI, 1, 1 };
                    double rgb[3];
                    Miscellany::HSVtoRGB(hsv, rgb);
                    t = Texel(rgb[0] * 255, rgb[1] * 255, rgb[2] * 255, 255);
                } else {
                    const double v = noise == Noise::BlackAndWhite ? (u < 0.5 ? 0 : 255) : u * 255;
                    t = Texel(v, v, v, 255);
                }
            }
            gd.mass(&signal[0], &mass[0]);
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<Real>> solver(gd.mass() * Real(streakFidelity) + gd.stiffness());
            if (solver.info() != Eigen::Success)
                throw std::runtime_error("The system for the streaks could not be factored.");
            if (onVCycle && !onVCycle(0)) {
                FilterOutcome canceled;
                canceled.canceled = true;
                return canceled;
            }
            std::vector<Texel> x(nodes);
            Eigen::VectorXd b(static_cast<Eigen::Index>(nodes));
            for (unsigned int c = 0; c < 3; ++c) {
                for (size_t n = 0; n < nodes; ++n)
                    b[Eigen::Index(n)] = mass[n][c] * streakFidelity;
                const Eigen::VectorXd y = solver.solve(b);
                for (size_t n = 0; n < nodes; ++n)
                    x[n][c] = y[Eigen::Index(n)];
            }
            for (Texel &t : x)
                t[3] = 255;
            writeBack(gd, x, p, image, true, false, [](size_t) { return true; });
        } // the anisotropic operators are released before the sharpening builds its own

        // The program's sharpening of the streaks, the stage-2 solve.
        return filter(atlas, image, sharpenFidelity, sharpenScale, nullptr, nullptr, false, onVCycle);
    });
}

std::vector<TriangleCurvature> principalCurvature(const Surface &surface, unsigned int smoothingIterations,
                                                  double smoothingWeight)
{
    return rethrown([&]() -> std::vector<TriangleCurvature> {
        TexturedTriangleMesh<Real> mesh;
        for (const std::array<double, 3> &p : surface.positions)
            mesh.surface.vertices.emplace_back(p[0], p[1], p[2]);
        for (const std::array<unsigned int, 3> &t : surface.triangles)
            mesh.surface.triangles.emplace_back(t[0], t[1], t[2]);
        const size_t vertices = mesh.surface.vertices.size();

        // As LineIntegralConvolution: the vertex normals, area weighted, then smoothed. Each round
        // moves every normal n_i by offsets o_i along two tangents T_i, solving the program's
        // (T^t (w M + S) T) o = -T^t S n on the surface of unit area, w the smoothing weight: the
        // offsets are held small by the mass term and make the normals vary less by the stiffness.
        std::vector<Point3D<Real>> normals(vertices);
        for (unsigned int t = 0; t < mesh.numTriangles(); ++t) {
            const Point3D<Real> n = mesh.surfaceTriangle(t).normal();
            for (unsigned int k = 0; k < 3; ++k)
                normals[mesh.surface.triangles[t][k]] += n;
        }
        auto normalize = [](Point3D<Real> &n) {
            const Real length = Point3D<Real>::Length(n);
            if (length > 0)
                n /= length;
        };
        for (Point3D<Real> &n : normals)
            normalize(n);
        if (smoothingIterations > 0) {
            FEM::RiemannianMesh<Real, unsigned int> rMesh(GetPointer(mesh.surface.triangles), mesh.surface.triangles.size());
            rMesh.setMetricFromEmbedding(GetPointer(mesh.surface.vertices));
            rMesh.makeUnitArea();
            const Eigen::SparseMatrix<Real> stiffness = rMesh.template stiffnessMatrix<FEM::BASIS_0_WHITNEY, true>();
            const Eigen::SparseMatrix<Real> system =
                rMesh.template massMatrix<FEM::BASIS_0_WHITNEY, true>() * Real(smoothingWeight) + stiffness;
            std::vector<Point3D<Real>> tangents(2 * vertices);
            for (unsigned int iteration = 0; iteration < smoothingIterations; ++iteration) {
                for (size_t i = 0; i < vertices; ++i) {
                    Point3D<Real> v(1, 0, 0);
                    if (std::abs(Point3D<Real>::Dot(v, normals[i])) > 0.99)
                        v = Point3D<Real>(0, 1, 0);
                    tangents[2 * i] = Point3D<Real>::CrossProduct(normals[i], v);
                    normalize(tangents[2 * i]);
                    tangents[2 * i + 1] = Point3D<Real>::CrossProduct(normals[i], tangents[2 * i]);
                    normalize(tangents[2 * i + 1]);
                }
                std::vector<Eigen::Triplet<Real>> triplets;
                for (int i = 0; i < system.outerSize(); ++i)
                    for (Eigen::SparseMatrix<Real>::InnerIterator it(system, i); it; ++it)
                        for (int a = 0; a < 2; ++a)
                            for (int b = 0; b < 2; ++b)
                                triplets.emplace_back(int(2 * it.row() + a), int(2 * it.col() + b),
                                                      it.value() * Point3D<Real>::Dot(tangents[2 * it.row() + a], tangents[2 * it.col() + b]));
                Eigen::SparseMatrix<Real> A(Eigen::Index(2 * vertices), Eigen::Index(2 * vertices));
                A.setFromTriplets(triplets.begin(), triplets.end());
                Eigen::VectorXd rhs = Eigen::VectorXd::Zero(Eigen::Index(2 * vertices));
                for (int i = 0; i < stiffness.outerSize(); ++i)
                    for (Eigen::SparseMatrix<Real>::InnerIterator it(stiffness, i); it; ++it)
                        for (int a = 0; a < 2; ++a)
                            rhs[2 * it.row() + a] -= it.value() * Point3D<Real>::Dot(normals[it.col()], tangents[2 * it.row() + a]);
                Eigen::SimplicialLDLT<Eigen::SparseMatrix<Real>> solver(A);
                if (solver.info() != Eigen::Success)
                    throw std::runtime_error("The normal smoothing system could not be factored.");
                const Eigen::VectorXd o = solver.solve(rhs);
                for (size_t i = 0; i < vertices; ++i) {
                    normals[i] += tangents[2 * i] * o[Eigen::Index(2 * i)] + tangents[2 * i + 1] * o[Eigen::Index(2 * i + 1)];
                    normalize(normals[i]);
                }
            }
        }

        std::vector<PrincipalCurvature<Real>> curvatures;
        InitializePrincipalCurvatureDirection(mesh, normals, curvatures);
        // TSP measures the triangles of the surface scaled to unit area (NORMALIZE_SURFACE_EMBEDDING
        // in Src/Metric.inl), so its curvatures are the surface's divided by the square root of its
        // area. Its directions are in the basis of each triangle's two edges from its first corner,
        // of unit length; its values ascend, so index 1 is the maximal.
        const Real scale = std::sqrt(mesh.surface.area());
        std::vector<TriangleCurvature> out(mesh.numTriangles());
        for (unsigned int t = 0; t < mesh.numTriangles(); ++t) {
            const Simplex<Real, 3, 2> s = mesh.surfaceTriangle(t);
            auto direction = [&](const Point2D<Real> &d) {
                Point3D<Real> v = (s[1] - s[0]) * d[0] + (s[2] - s[0]) * d[1];
                normalize(v);
                return std::array<double, 3>{ v[0], v[1], v[2] };
            };
            out[t].maxDirection = direction(curvatures[t].dirs[1]);
            out[t].minDirection = direction(curvatures[t].dirs[0]);
            out[t].maxCurvature = curvatures[t].values[1] * scale;
            out[t].minCurvature = curvatures[t].values[0] * scale;
        }
        return out;
    });
}

} // namespace TspAdapter
