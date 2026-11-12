#pragma once

// The one place MeshLab calls TextureSignalProcessing (TSP) and its Misha library. Only
// tsp_adapter.cpp includes their headers, so their templates, their namespace and the
// MK_ERROR_OUT redefinition stay out of the rest of the plugin. The interface is plain C++.

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace TspAdapter {

// The faces of one texture, as TSP takes them: the surface, with coincident vertices merged so
// that the two sides of a seam share their vertices, and three texture coordinates per face, in
// the unit square with v up. Faces whose texture coordinates have no area are left out by the
// caller: they cover no texel, and TSP cannot process them.
struct Atlas
{
    std::vector<std::array<double, 3>> positions;
    std::vector<std::array<unsigned int, 3>> triangles; // into positions
    std::vector<std::array<double, 2>> uv;              // 3 per triangle
};

// A texture, row 0 at the top as QImage stores it, four channels on the 0-255 scale.
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

// TextureFiltering: the texture whose values stay close to the input and whose gradients on the
// surface are the input's scaled by `gradientScale` (below 1 smooths, above 1 sharpens), solving
// (w M + S) x = w M x0 + gradientScale S x0 with w = `fidelity`, the mesh area normalized to 1,
// by multigrid until one more V-cycle changes no texel by more than a fiftieth of a level.
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
FilterOutcome filter(const Atlas &atlas, Image &image, double fidelity, double gradientScale,
                     const std::vector<char> *selectedTriangles,
                     const std::function<bool(unsigned int)> &onVCycle = {});

} // namespace TspAdapter
