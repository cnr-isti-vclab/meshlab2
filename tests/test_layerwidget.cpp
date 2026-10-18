#include <QtTest/QtTest>

#include "document.h"
#include "layerwidget.h"

#include <QImage>
#include <QScrollArea>
#include <QTreeWidget>
#include <QToolButton>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <wrap/io_trimesh/io_mask.h>

class LayerWidgetTests : public QObject
{
    Q_OBJECT

private slots:
    void visibilitySetChipsAreFullyVisibleAndNormallySized();
    void hoveringASetChipShadesItsLayersRows();
    void theEyeShowsAllOrHidesAllAndSaysWhatIsShown();
    void theTitleBarButtonsGoOverTheArrowsAndTheCheckMarks();
};

namespace {

int addTinyMesh(Document &doc, const QString &name)
{
    VCGMesh m;
    vcg::tri::Allocator<VCGMesh>::AddVertices(m, 3);
    m.vert[0].P() = vcg::Point3f(0, 0, 0);
    m.vert[1].P() = vcg::Point3f(1, 0, 0);
    m.vert[2].P() = vcg::Point3f(0, 1, 0);
    vcg::tri::Allocator<VCGMesh>::AddFace(m, 0, 1, 2);
    vcg::tri::UpdateBounding<VCGMesh>::Box(m);
    return doc.addMesh(m, name, vcg::tri::io::Mask::IOM_VERTCOORD);
}

} // namespace

// The chips under the layer list were first sized from the host widget before any chip was in
// it, which left the row a few pixels high and every chip cut off. They have to be as tall as a
// tool button needs, and wholly inside the row.
void LayerWidgetTests::visibilitySetChipsAreFullyVisibleAndNormallySized()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    doc.addVisibilitySet(QStringLiteral("front"), doc.visibleMeshIds());
    doc.addVisibilitySet(QStringLiteral("a long name that gets cut"), { doc.mesh(0).meshId });

    LayerWidget widget(&doc);
    widget.resize(320, 420);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);   // the chips are made when the queued rebuild runs

    const QString dir = qEnvironmentVariable("MESHLAB2_GRAB_DIR");
    if (!dir.isEmpty())
        widget.grab().save(dir + QStringLiteral("/layerwidget.png"));

    QList<QToolButton *> chips;
    for (QToolButton *button : widget.findChildren<QToolButton *>()) {
        if (button->isCheckable())
            chips << button;
    }
    QCOMPARE(chips.size(), 2);

    QToolButton reference;
    reference.setText(QStringLiteral("front"));
    reference.setAutoRaise(true);
    for (QToolButton *chip : chips) {
        QVERIFY2(chip->height() >= reference.sizeHint().height() - 2, qPrintable(chip->text()));
        QVERIFY2(chip->height() <= reference.sizeHint().height() + 12, qPrintable(chip->text()));
        const QWidget *viewport = chip->parentWidget()->parentWidget();   // host -> scroll viewport
        QVERIFY2(viewport->rect().contains(chip->geometry().adjusted(0, 0, -1, -1)),
                 qPrintable(chip->text()));
    }

}

// Hovering a chip previews its set: the rows of its layers are shaded, the others are not, and the
// shade goes when the pointer leaves.
void LayerWidgetTests::hoveringASetChipShadesItsLayersRows()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    doc.addVisibilitySet(QStringLiteral("only a"), { doc.mesh(0).meshId });

    LayerWidget widget(&doc);
    widget.resize(320, 420);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);

    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 2)
            tree = candidate;
    }
    QVERIFY(tree);
    QToolButton *chip = nullptr;
    for (QToolButton *button : widget.findChildren<QToolButton *>()) {
        if (button->isCheckable())
            chip = button;
    }
    QVERIFY(chip);

    // The right-hand end of each layer's row, clear of its text.
    const auto sample = [&](int row) {
        const QRect rect = tree->visualItemRect(tree->topLevelItem(row));
        return tree->viewport()->grab().toImage().pixelColor(rect.right() - 6, rect.center().y());
    };
    const QColor rowABefore = sample(0);
    const QColor rowBBefore = sample(1);

    QEvent enter(QEvent::Enter);
    QCoreApplication::sendEvent(chip, &enter);
    QTest::qWait(20);
    QVERIFY(sample(0) != rowABefore);
    QCOMPARE(sample(1), rowBBefore);
    // The detail rows under a layer are shaded with it, and the other layer's are not.
    const auto detailSample = [&](int top) {
        QTreeWidgetItem *child = tree->topLevelItem(top)->child(0);
        const QRect rect = tree->visualItemRect(child);
        return tree->viewport()->grab().toImage().pixelColor(rect.right() - 6, rect.center().y());
    };
    QVERIFY(tree->topLevelItem(0)->childCount() > 0);
    QVERIFY(detailSample(0) != detailSample(1));
    const QString dir = qEnvironmentVariable("MESHLAB2_GRAB_DIR");
    if (!dir.isEmpty())
        widget.grab().save(dir + QStringLiteral("/layerwidget_hover.png"));

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(chip, &leave);
    QTest::qWait(20);
    QCOMPARE(sample(0), rowABefore);
}

// The eye in the title bar: its action shows every layer, or hides every layer when they all are
// shown, and the panel says how many are shown so that the eye can be drawn open, closed or half.
void LayerWidgetTests::theEyeShowsAllOrHidesAllAndSaysWhatIsShown()
{
    Document doc;
    LayerWidget widget(&doc);
    QCOMPARE(widget.layerVisibility(), std::make_pair(0, 0));
    widget.toggleAllVisible();   // no layers: nothing to do, and no harm

    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    QSignalSpy spy(&widget, &LayerWidget::layerVisibilityChanged);
    QCOMPARE(widget.layerVisibility(), std::make_pair(2, 2));

    widget.toggleAllVisible();
    QVERIFY(!doc.mesh(0).visible && !doc.mesh(1).visible);
    QCOMPARE(widget.layerVisibility(), std::make_pair(0, 2));
    QTRY_VERIFY(!spy.isEmpty());   // the rebuild that follows reports it
    QCOMPARE(spy.last().at(0).toInt(), 0);
    QCOMPARE(spy.last().at(1).toInt(), 2);

    // Partly shown: the next action shows the rest, rather than hiding what is shown.
    doc.setMeshVisible(0, true);
    QCOMPARE(widget.layerVisibility(), std::make_pair(1, 2));
    widget.toggleAllVisible();
    QVERIFY(doc.mesh(0).visible && doc.mesh(1).visible);
    widget.toggleAllVisible();
    QVERIFY(!doc.mesh(0).visible && !doc.mesh(1).visible);
}

// The title bar puts its collapse-all button over the disclosure arrows and its eye over the check
// marks, at the places the panel reports; they have to be where the style really draws them.
void LayerWidgetTests::theTitleBarButtonsGoOverTheArrowsAndTheCheckMarks()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    LayerWidget widget(&doc);
    widget.resize(320, 420);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);

    QTreeWidget *tree = nullptr;
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->topLevelItemCount() == 1)
            tree = candidate;
    }
    QVERIFY(tree);
    QCOMPARE(tree->mapTo(&widget, QPoint(0, 0)).x(), 0);   // the places are counted from this edge

    // The first run of columns with ink in a stretch of the row: the arrow, the check mark, and
    // not the text that follows it.
    const QImage image = tree->viewport()->grab().toImage();
    const QRect rowRect = tree->visualItemRect(tree->topLevelItem(0));
    const QColor background = image.pixelColor(image.width() - 2, rowRect.center().y());
    const auto inkCenter = [&](int from, int to) {
        int first = -1, last = -1;
        for (int x = from; x < to; ++x) {
            bool ink = false;
            for (int y = rowRect.top() + 2; y < rowRect.bottom() - 1 && !ink; ++y)
                ink = image.pixelColor(x, y) != background;
            if (ink) {
                first = first < 0 ? x : first;
                last = x;
            } else if (first >= 0) {
                break;
            }
        }
        return first < 0 ? -1.0 : (first + last) / 2.0;
    };
    const int indentation = tree->indentation();
    const int offset = tree->frameWidth();
    const double arrow = inkCenter(0, indentation);
    const double check = inkCenter(indentation, indentation + tree->columnWidth(0));
    QVERIFY(arrow >= 0 && check >= 0);
    QVERIFY2(qAbs(arrow + offset - widget.disclosureArrowCenter()) <= 3,
             qPrintable(QStringLiteral("arrow ink %1, reported %2").arg(arrow + offset).arg(widget.disclosureArrowCenter())));
    QVERIFY2(qAbs(check + offset - widget.visibilityCheckCenter()) <= 3,
             qPrintable(QStringLiteral("check ink %1, reported %2").arg(check + offset).arg(widget.visibilityCheckCenter())));
}

QTEST_MAIN(LayerWidgetTests)
#include "test_layerwidget.moc"
