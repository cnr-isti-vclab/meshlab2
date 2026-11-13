#pragma once

// The one place MeshLab calls TextureSignalProcessing (TSP) and its Misha library. Only
// tsp_adapter.cpp includes their headers, so their templates, their namespace and the
// MK_ERROR_OUT redefinition stay out of the rest of the plugin. The interface is plain C++.

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace TspAdapter {

// A surface: positions, and triangles into them.
struct Surface
{
    std::vector<std::array<double, 3>> positions;
    std::vector<std::array<unsigned int, 3>> triangles; // into positions
};

// The faces of one texture, as TSP takes them: the surface, with coincident vertices merged so
// that the two sides of a seam share their vertices, and three texture coordinates per face, in
// the unit square with v up. Faces whose texture coordinates have no area are left out by the
// caller: they cover no texel, and TSP cannot process them.
struct Atlas : Surface
{
    std::vector<std::array<double, 2>> uv; // 3 per triangle
};

// A texture, row 0 at the top as QImage stores it, four channels on the 0-255 scale.
//
// The functions that solve for texel values take `normals` for object-space normal maps: the first
// three channels then encode a unit vector v as 127.5 (v + 1), and the texels written are
// renormalized. The solves need no decoding, since they commute with an affine change of the values
// (a constant has no gradient).
struct Image
{
    int width = 0;
    int height = 0;
    std::vector<std::array<double, 4>> texels; // width * height, row by row
};

// SetExteriorTexels: sets the texels just outside the charts to the values that minimize the
// Dirichlet energy of the texture on the surface, the covered texels held fixed, so that
// bilinear lookups on either side of a seam agree. Returns how many texels it set. Throws
// std::runtime_error with TSP's message if TSP fails.
std::size_t repairSeams(const Atlas &atlas, Image &image);

// TextureDilation: fills the texels up to `radius` outside the charts by following the surface
// across the seam from the nearest face and sampling the image there. The samples are read from
// the input image, never from texels being filled, so the result does not depend on threading.
// Returns how many texels it filled. Throws std::runtime_error with TSP's message if TSP fails.
std::size_t dilate(const Atlas &atlas, Image &image, unsigned int radius);

// SeamStitcher's mask mode: the texels whose nearest triangle is selected (one flag per triangle)
// take the values that minimize the Dirichlet energy of the texture on the surface, the other
// covered texels held, so the region is filled smoothly from around it, continuously across seams.
// Only the region is written, and its alpha is left as it was. Returns how many texels it set. Throws std::runtime_error with TSP's message if TSP fails.
std::size_t inpaint(const Atlas &atlas, Image &image, const std::vector<char> &selectedTriangles, bool normals);

// TextureFiltering: the texture whose values stay close to the input and whose gradients on the
// surface are the input's scaled by `gradientScale` (below 1 smooths, above 1 sharpens), solving
// (w M + S) x = w M x0 + gradientScale S x0, the mesh area normalized to 1, with w set so that
// `detailSize`, in the mesh's units, is the crossover: details smaller than it get the gradient
// scale, larger ones keep the input (weightForDetailSize in the source has the formula). Solved by
// multigrid until one more V-cycle changes no texel by more than a fiftieth of a level. With
// `detail` (an image of the same size), the gradients come from it instead, and the values are held
// to `image`'s: TextureFiltering's low-frequency input (--inLow) is `image`. Alpha is left as it was.
// With `selectedTriangles` (one flag per triangle), the gradients are scaled only around the
// selected triangles, and only the texels whose nearest triangle is selected are written; that
// needs the divergence operator, 3.5 times the memory. `onVCycle`, if given, is called after each
// V-cycle with how many have run, and stops the solve by returning false: the image is then left as
// it was and `canceled` set. Throws std::runtime_error with TSP's message if TSP fails.
struct FilterOutcome
{
    std::size_t texels = 0;  // written
    unsigned int vCycles = 0;
    bool converged = false;  // false if the cap on V-cycles was reached first
    bool canceled = false;
};
FilterOutcome filter(const Atlas &atlas, Image &image, double detailSize, double gradientScale,
                     const std::vector<char> *selectedTriangles, const Image *detail, bool normals,
                     const std::function<bool(unsigned int)> &onVCycle = {});

// LineIntegralConvolution: a texture of streaks along `field`, one vector per triangle on the surface,
// whose length drives the anisotropy with `anisotropyExponent` (0: the same everywhere it is not
// zero). Noise drawn from `seed`, one value per texel (a random hue, as the program draws, black or
// white, or a random gray), is diffused under a metric stretched across the field,
// (w M_a + S_a) x = w M_a n, solved directly, with the program's weight w set from `streakLength`, in
// the mesh's units (lineIntegralConvolution's comment says how), and the result sharpened by filter() with
// `sharpenDetailSize` and `sharpenScale`, whose outcome this returns. Writes every texel of the
// atlas into `image`, opaque. `onVCycle` is also called once, with 0, once the streak system is
// factored, so the run can be canceled there. Throws std::runtime_error with TSP's message if TSP
// fails.
enum class Noise { Hue, BlackAndWhite, Gray };
FilterOutcome lineIntegralConvolution(const Atlas &atlas, Image &image, const std::vector<std::array<double, 3>> &field,
                                      double streakLength, double sharpenDetailSize, double sharpenScale,
                                      double anisotropyExponent, Noise noise, unsigned int seed,
                                      const std::function<bool(unsigned int)> &onVCycle = {});

// TSP's principal curvature per triangle (InitializePrincipalCurvatureDirection): the shape operator
// of each triangle from the differences of its vertex normals, after LineIntegralConvolution's
// smoothing of those normals (`smoothingIterations` rounds, each a screened Poisson solve whose
// crossover is `smoothingSize`, in the surface's units, as `filter`'s `detailSize`). Directions are unit vectors, curvatures in the surface's
// units, the maximal first. Throws std::runtime_error with TSP's message if TSP fails.
struct TriangleCurvature
{
    std::array<double, 3> maxDirection, minDirection;
    double maxCurvature = 0, minCurvature = 0;
};
std::vector<TriangleCurvature> principalCurvature(const Surface &surface, unsigned int smoothingIterations,
                                                  double smoothingSize);

} // namespace TspAdapter
