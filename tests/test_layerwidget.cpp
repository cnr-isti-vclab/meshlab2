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
        if (button->isCheckable() && button->text().contains(QRegularExpression(QStringLiteral("^[12] "))))
            chips << button;
    }
    QCOMPARE(chips.size(), 2);

    QToolButton reference;
    reference.setText(QStringLiteral("1 front"));
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

QTEST_MAIN(LayerWidgetTests)
#include "test_layerwidget.moc"
