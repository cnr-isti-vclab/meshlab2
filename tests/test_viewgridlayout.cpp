#include <QtTest/QtTest>

#include "viewgridlayout.h"

using ViewGridLayout::Shape;

// The grid arrangement shows each visible layer in its own tile. How many columns and rows
// that takes is the one part of the feature with no single right answer, so the chosen rule
// is pinned here as a table rather than left to be judged by resizing a window.
class ViewGridLayoutTests : public QObject
{
    Q_OBJECT

private slots:
    void shapesOnAFourThreeViewMatchTheSketchedLayout();
    void shapeFollowsTheViewportAspect();
    void shapeTransposesOnAPortraitViewport();
    void degenerateTileCountsGiveASingleTile();
    void everyShapeCoversTheTilesWithNoEmptyRow();

    void tilesAreRowMajorFromTheTopLeft();
    void aFullGridCoversTheViewportExactly();
    void aShortLastRowIsCentred();
    void insetLeavesTwiceTheGapBetweenNeighbours();
    void insetNeverCollapsesATileToNothing();
    void tileCountIsHonouredEvenWhenTheGridHasSpareCells();
    void theConvenienceOverloadUsesTheChosenShape();

    void tileAtFindsTheTileUnderAPoint();
    void tileAtRejectsGuttersAndOutsidePoints();
};

namespace {

const QSize kFourThree(800, 600);
const QSize kSixteenNine(1600, 900);

QString describe(Shape shape)
{
    return QStringLiteral("%1x%2").arg(shape.columns).arg(shape.rows);
}

// Fails with "4x2 != 3x3" rather than with two unrelated integer mismatches.
void compareShape(Shape actual, int columns, int rows, int tileCount)
{
    const Shape expected{ columns, rows };
    QVERIFY2(actual == expected,
             qPrintable(QStringLiteral("%1 tiles: got %2, expected %3")
                            .arg(tileCount)
                            .arg(describe(actual))
                            .arg(describe(expected))));
}

} // namespace

// The layout originally sketched for the feature -- 3 and 4 layers in 2x2, 5 and 6 in 3x2,
// 7 and 8 in 4x2, 9 in 3x3 -- is what the rule produces on a 4:3 view, which is the frame
// that sketch assumed. Everything about the scoring constants is answerable from this table.
void ViewGridLayoutTests::shapesOnAFourThreeViewMatchTheSketchedLayout()
{
    compareShape(ViewGridLayout::chooseShape(1, kFourThree), 1, 1, 1);
    compareShape(ViewGridLayout::chooseShape(2, kFourThree), 2, 1, 2);
    compareShape(ViewGridLayout::chooseShape(3, kFourThree), 2, 2, 3);
    compareShape(ViewGridLayout::chooseShape(4, kFourThree), 2, 2, 4);
    compareShape(ViewGridLayout::chooseShape(5, kFourThree), 3, 2, 5);
    compareShape(ViewGridLayout::chooseShape(6, kFourThree), 3, 2, 6);
    compareShape(ViewGridLayout::chooseShape(7, kFourThree), 4, 2, 7);
    compareShape(ViewGridLayout::chooseShape(8, kFourThree), 4, 2, 8);
    compareShape(ViewGridLayout::chooseShape(9, kFourThree), 3, 3, 9);
}

// Square tiles are the objective, so a wider frame wants wider grids. Two cases differ from
// the 4:3 table above and both are the deliberate consequence: on 16:9 three tiles side by
// side are squarer (0.59) than a 2x2 with an empty cell (1.78), and nine tiles in 5x2 are
// squarer (0.71) than in 3x3 (1.78).
void ViewGridLayoutTests::shapeFollowsTheViewportAspect()
{
    compareShape(ViewGridLayout::chooseShape(3, kSixteenNine), 3, 1, 3);
    compareShape(ViewGridLayout::chooseShape(4, kSixteenNine), 2, 2, 4);
    compareShape(ViewGridLayout::chooseShape(5, kSixteenNine), 3, 2, 5);
    compareShape(ViewGridLayout::chooseShape(6, kSixteenNine), 3, 2, 6);
    compareShape(ViewGridLayout::chooseShape(7, kSixteenNine), 4, 2, 7);
    compareShape(ViewGridLayout::chooseShape(8, kSixteenNine), 4, 2, 8);
    compareShape(ViewGridLayout::chooseShape(9, kSixteenNine), 5, 2, 9);
}

void ViewGridLayoutTests::shapeTransposesOnAPortraitViewport()
{
    const QSize portrait(600, 800);
    compareShape(ViewGridLayout::chooseShape(5, portrait), 2, 3, 5);
    compareShape(ViewGridLayout::chooseShape(6, portrait), 2, 3, 6);
    compareShape(ViewGridLayout::chooseShape(2, portrait), 1, 2, 2);
}

void ViewGridLayoutTests::degenerateTileCountsGiveASingleTile()
{
    // One tile is the overlay arrangement, so it has to be the whole viewport: the renderer
    // runs both arrangements through this same path.
    compareShape(ViewGridLayout::chooseShape(1, kFourThree), 1, 1, 1);
    compareShape(ViewGridLayout::chooseShape(0, kFourThree), 1, 1, 0);
    compareShape(ViewGridLayout::chooseShape(-3, kFourThree), 1, 1, -3);
    // A viewport with no area must not produce a shape from a division by zero.
    compareShape(ViewGridLayout::chooseShape(4, QSize(0, 0)), 2, 2, 4);
}

void ViewGridLayoutTests::everyShapeCoversTheTilesWithNoEmptyRow()
{
    const QSize viewports[] = { kFourThree, kSixteenNine, QSize(600, 800), QSize(1000, 1000) };
    for (const QSize &viewport : viewports) {
        for (int n = 1; n <= 40; ++n) {
            const Shape shape = ViewGridLayout::chooseShape(n, viewport);
            QVERIFY2(shape.columns >= 1 && shape.rows >= 1, qPrintable(describe(shape)));
            QVERIFY2(shape.cells() >= n,
                     qPrintable(QStringLiteral("%1 tiles do not fit in %2")
                                    .arg(n)
                                    .arg(describe(shape))));
            // A shape whose last row is entirely empty is never the right answer: the same
            // tiles fit in one row fewer, with larger tiles.
            QVERIFY2(shape.columns * (shape.rows - 1) < n,
                     qPrintable(QStringLiteral("%1 tiles in %2 leave an empty row")
                                    .arg(n)
                                    .arg(describe(shape))));
        }
    }
}

void ViewGridLayoutTests::tilesAreRowMajorFromTheTopLeft()
{
    const auto rects = ViewGridLayout::tileRects(4, Shape{ 2, 2 }, kFourThree);
    QCOMPARE(rects.size(), size_t(4));
    QCOMPARE(rects[0], QRect(0, 0, 400, 300));
    QCOMPARE(rects[1], QRect(400, 0, 400, 300));
    QCOMPARE(rects[2], QRect(0, 300, 400, 300));
    QCOMPARE(rects[3], QRect(400, 300, 400, 300));
}

// Rounding each tile independently would leave a seam column or a one-pixel overhang, which
// on screen is a stripe of background between tiles that moves as the window is resized.
void ViewGridLayoutTests::aFullGridCoversTheViewportExactly()
{
    // Sizes that divide badly by 3 and 7 are where independent rounding shows up.
    const QSize awkward(1001, 703);
    const Shape shape{ 7, 3 };
    const auto rects = ViewGridLayout::tileRects(shape.cells(), shape, awkward);

    int coveredArea = 0;
    for (size_t i = 0; i < rects.size(); ++i) {
        QVERIFY(!rects[i].isEmpty());
        QVERIFY(QRect(QPoint(0, 0), awkward).contains(rects[i]));
        coveredArea += rects[i].width() * rects[i].height();
        for (size_t j = i + 1; j < rects.size(); ++j)
            QVERIFY2(!rects[i].intersects(rects[j]), "tiles overlap");
    }
    QCOMPARE(coveredArea, awkward.width() * awkward.height());
}

void ViewGridLayoutTests::aShortLastRowIsCentred()
{
    // Three tiles in a 2x2: the lone tile on the bottom row sits in the middle rather than
    // hard left with a hole beside it.
    const auto rects = ViewGridLayout::tileRects(3, Shape{ 2, 2 }, kFourThree);
    QCOMPARE(rects.size(), size_t(3));
    QCOMPARE(rects[0], QRect(0, 0, 400, 300));
    QCOMPARE(rects[1], QRect(400, 0, 400, 300));
    QCOMPARE(rects[2], QRect(200, 300, 400, 300));

    // Same width as the tiles above it, and the same margin either side.
    QCOMPARE(rects[2].width(), rects[0].width());
    QCOMPARE(rects[2].left(), kFourThree.width() - rects[2].right() - 1);
}

void ViewGridLayoutTests::insetLeavesTwiceTheGapBetweenNeighbours()
{
    constexpr int kInset = 4;
    const auto rects = ViewGridLayout::tileRects(4, Shape{ 2, 2 }, kFourThree, kInset);

    QCOMPARE(rects[0], QRect(4, 4, 392, 292));
    // The gap a separator line is drawn into, and the margin that absorbs the few pixels a
    // wide line or a large point can spill past a viewport edge.
    QCOMPARE(rects[1].left() - (rects[0].right() + 1), 2 * kInset);
    QCOMPARE(rects[2].top() - (rects[0].bottom() + 1), 2 * kInset);
    // Half as much around the outside, which is what makes the inner gaps look even.
    QCOMPARE(rects[0].left(), kInset);
    QCOMPARE(rects[0].top(), kInset);
    QCOMPARE(kFourThree.width() - rects[1].right() - 1, kInset);
}

void ViewGridLayoutTests::insetNeverCollapsesATileToNothing()
{
    // An inset wider than the cell would otherwise produce empty or negative rectangles,
    // and a zero-sized GPU viewport is not a drawing no-op everywhere.
    const auto rects = ViewGridLayout::tileRects(4, Shape{ 2, 2 }, QSize(10, 10), 8);
    for (const QRect &rect : rects) {
        QVERIFY(!rect.isEmpty());
        QVERIFY(rect.width() > 0);
        QVERIFY(rect.height() > 0);
    }

    // Only the dimension that cannot take the inset keeps its full extent.
    const auto narrow = ViewGridLayout::tileRects(2, Shape{ 2, 1 }, QSize(10, 400), 3);
    QCOMPARE(narrow[0].width(), 5);
    QCOMPARE(narrow[0].height(), 400 - 2 * 3);
}

void ViewGridLayoutTests::tileCountIsHonouredEvenWhenTheGridHasSpareCells()
{
    for (int n = 1; n <= 20; ++n) {
        const auto rects = ViewGridLayout::tileRects(n, kFourThree);
        QCOMPARE(rects.size(), size_t(n));
    }
    QVERIFY(ViewGridLayout::tileRects(0, kFourThree).empty());
    QVERIFY(ViewGridLayout::tileRects(-2, kFourThree).empty());
}

void ViewGridLayoutTests::theConvenienceOverloadUsesTheChosenShape()
{
    const Shape shape = ViewGridLayout::chooseShape(7, kSixteenNine);
    QCOMPARE(ViewGridLayout::tileRects(7, kSixteenNine, 2),
             ViewGridLayout::tileRects(7, shape, kSixteenNine, 2));
}

void ViewGridLayoutTests::tileAtFindsTheTileUnderAPoint()
{
    const auto rects = ViewGridLayout::tileRects(4, Shape{ 2, 2 }, kFourThree);
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(10, 10)), 0);
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(410, 10)), 1);
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(10, 310)), 2);
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(799, 599)), 3);
}

void ViewGridLayoutTests::tileAtRejectsGuttersAndOutsidePoints()
{
    const auto rects = ViewGridLayout::tileRects(4, Shape{ 2, 2 }, kFourThree, 4);
    // A click in the gap belongs to no layer, and saying so is the point: guessing the
    // nearest tile would make a miss look like a deliberate selection.
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(399, 100)), -1);
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(100, 299)), -1);
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(900, 100)), -1);
    QCOMPARE(ViewGridLayout::tileAt(rects, QPoint(-1, -1)), -1);

    // The empty cell of a short last row is not a tile either.
    const auto shortRow = ViewGridLayout::tileRects(3, Shape{ 2, 2 }, kFourThree);
    QCOMPARE(ViewGridLayout::tileAt(shortRow, QPoint(100, 400)), -1);
    QCOMPARE(ViewGridLayout::tileAt(shortRow, QPoint(400, 400)), 2);
}

QTEST_MAIN(ViewGridLayoutTests)
#include "test_viewgridlayout.moc"
