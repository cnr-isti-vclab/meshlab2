#include "viewgridlayout.h"

#include <cmath>

namespace ViewGridLayout {

namespace {

// What one empty cell costs, measured in the same units as the squareness term: a shape
// wasting one cell out of n is penalized by kEmptyCellPenalty/n, so a log-aspect advantage
// smaller than that loses to the tighter packing.
//
// At 1.0 the rule reproduces the whole of the originally sketched behaviour on a 4:3 view
// -- 3 and 4 layers in 2x2, 5 and 6 in 3x2, 7 and 8 in 4x2, 9 in 3x3 -- which is the frame
// that sketch had in mind. See the table pinned in test_viewgridlayout.cpp for what the
// same rule does on wider views.
constexpr double kEmptyCellPenalty = 1.0;

int ceilDiv(int a, int b)
{
    return (a + b - 1) / b;
}

// Exact cell boundaries: computing both edges from the same expression makes neighbouring
// cells share an edge and the last one land on the viewport edge, with no gap or overlap
// left by rounding each width independently.
int edge(int index, int count, int extent)
{
    return int((qint64(index) * qint64(extent)) / qint64(count));
}

} // namespace

Shape chooseShape(int tileCount, QSize viewport)
{
    if (tileCount <= 1)
        return Shape{ 1, 1 };

    const double width = double(qMax(1, viewport.width()));
    const double height = double(qMax(1, viewport.height()));
    const double viewportAspect = width / height;

    Shape best{ tileCount, 1 };
    double bestScore = 0.0;
    bool haveBest = false;

    // Enumerating by row count with columns = ceil(n/rows) visits every shape that is tight
    // in its columns; the guard then drops those with a wholly empty last row (2x3 for four
    // tiles, say, which 2x2 already holds).
    for (int rows = 1; rows <= tileCount; ++rows) {
        const int columns = ceilDiv(tileCount, rows);
        if (columns * (rows - 1) >= tileCount)
            continue;

        const double tileAspect = viewportAspect * double(rows) / double(columns);
        const double squareness = std::abs(std::log(tileAspect));
        const double waste = double(columns * rows - tileCount) / double(tileCount);
        const double score = squareness + kEmptyCellPenalty * waste;

        if (!haveBest || score < bestScore) {
            best = Shape{ columns, rows };
            bestScore = score;
            haveBest = true;
        }
    }

    return best;
}

std::vector<QRect> tileRects(int tileCount, Shape shape, QSize viewport, int inset)
{
    std::vector<QRect> rects;
    if (tileCount <= 0)
        return rects;

    const int columns = qMax(1, shape.columns);
    const int rows = qMax(1, shape.rows);
    const int width = qMax(0, viewport.width());
    const int height = qMax(0, viewport.height());
    const int clampedInset = qMax(0, inset);

    rects.reserve(size_t(tileCount));
    for (int index = 0; index < tileCount; ++index) {
        const int row = qMin(index / columns, rows - 1);
        const int column = index % columns;

        // A short last row is centred by shifting it half of the space its missing tiles
        // would have taken. Widths stay on the column grid, so every tile in the grid is
        // still the same size.
        const int tilesInRow = qMin(columns, tileCount - row * columns);
        const int rowShift = (columns - tilesInRow) * width / (2 * columns);

        const int left = edge(column, columns, width) + rowShift;
        const int right = edge(column + 1, columns, width) + rowShift;
        const int top = edge(row, rows, height);
        const int bottom = edge(row + 1, rows, height);

        // QRect's right/bottom are inclusive, so a cell spanning [left, right) is
        // right - left wide.
        QRect rect(left, top, right - left, bottom - top);
        if (rect.width() > 2 * clampedInset && rect.height() > 2 * clampedInset)
            rect.adjust(clampedInset, clampedInset, -clampedInset, -clampedInset);
        else if (rect.width() > 2 * clampedInset)
            rect.adjust(clampedInset, 0, -clampedInset, 0);
        else if (rect.height() > 2 * clampedInset)
            rect.adjust(0, clampedInset, 0, -clampedInset);

        rects.push_back(rect);
    }

    return rects;
}

std::vector<QRect> tileRects(int tileCount, QSize viewport, int inset)
{
    return tileRects(tileCount, chooseShape(tileCount, viewport), viewport, inset);
}

int tileAt(const std::vector<QRect> &rects, QPoint point)
{
    for (size_t i = 0; i < rects.size(); ++i) {
        if (rects[i].contains(point))
            return int(i);
    }
    return -1;
}

} // namespace ViewGridLayout
