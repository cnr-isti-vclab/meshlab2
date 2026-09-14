#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <vector>

// Tile arithmetic for the grid arrangement of the 3D view.
//
// In the grid arrangement one view shows each visible layer in its own tile, all tiles
// sharing a single camera. Deciding how many columns and rows to use, and where each tile
// lands, is the whole of the geometry -- the renderer then draws the same frame plan once
// per tile with the tile's rectangle as its viewport.
//
// Lives here, apart from RenderWidget, because it is arithmetic: no document, no GPU, no
// widget. That makes the one part of the feature with a debatable answer -- which grid
// shape is "best" -- something that can be pinned in a test and adjusted by looking at a
// table rather than by resizing a window.
namespace ViewGridLayout {

struct Shape {
    int columns = 1;
    int rows = 1;

    int cells() const { return columns * rows; }

    bool operator==(const Shape &o) const
    {
        return columns == o.columns && rows == o.rows;
    }
    bool operator!=(const Shape &o) const { return !(*this == o); }
};

// Number of columns and rows to show `tileCount` tiles in a viewport of `viewport`.
//
// The objective is square tiles, which is what suits the roughly isotropic things a mesh
// viewer shows: the shape minimizing |ln(tile width / tile height)| wins, with a mild
// penalty per empty cell to settle near-ties in favour of filling the grid. Both terms
// matter -- squareness alone puts three layers in a row on a wide display, and filling
// alone puts nine layers in a 9x1 strip.
//
// The shape therefore depends on the viewport aspect, deliberately: nine tiles are 3x3 on
// a 4:3 view and 5x2 on a 16:9 one, because those are the square-tiled answers in each
// frame. Always returns at least 1x1, and columns*rows >= tileCount with no wholly empty
// row.
Shape chooseShape(int tileCount, QSize viewport);

// Rectangles for `tileCount` tiles laid out row-major in `viewport`, index 0 top-left, in
// the caller's own coordinate system (y down, origin top-left) -- so pass a size in device
// pixels to place a GPU viewport and one in logical pixels to hit-test a mouse position.
//
// A short final row is centred, since a gap in the middle of the bottom edge reads as a
// layout accident while a centred short row reads as intended.
//
// `inset` shrinks every tile on all four sides, which puts a gap of twice `inset` between
// neighbours and `inset` around the outside. A gap is worth having: it is where a tile
// separator goes, and it absorbs the few pixels that wide lines and large points can spill
// past a viewport edge into the tile next door. Tiles narrower or shorter than the inset
// would allow keep their full extent in that dimension rather than collapsing to nothing.
//
// Returns exactly `tileCount` rectangles, or none when `tileCount` is not positive.
std::vector<QRect> tileRects(int tileCount, Shape shape, QSize viewport, int inset = 0);

// The same for a shape chosen by chooseShape().
std::vector<QRect> tileRects(int tileCount, QSize viewport, int inset = 0);

// Index of the tile containing `point`, or -1 when it lands in a gutter, in an empty cell
// of a short last row, or outside the viewport. Callers that map a click to a layer need
// that -1: a click between tiles belongs to no layer.
int tileAt(const std::vector<QRect> &rects, QPoint point);

} // namespace ViewGridLayout
