#include <QtTest/QtTest>

#include "document.h"
#include "layerwidget.h"

#include <QScrollArea>
#include <QToolButton>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <wrap/io_trimesh/io_mask.h>

class LayerWidgetTests : public QObject
{
    Q_OBJECT

private slots:
    void visibilitySetChipsAreFullyVisibleAndNormallySized();
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

QTEST_MAIN(LayerWidgetTests)
#include "test_layerwidget.moc"
