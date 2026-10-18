#include <QtTest/QtTest>

#include "document.h"
#include "layerwidget.h"

#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QMouseEvent>
#include <QTreeWidget>
#include <QToolButton>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <wrap/io_trimesh/io_mask.h>

class LayerWidgetTests : public QObject
{
    Q_OBJECT

    QTemporaryDir m_settings;

private slots:
    // The panel remembers whether the Sets section is folded; a test must not change the user's.
    void initTestCase()
    {
        QVERIFY(m_settings.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settings.path());
    }
    void theSetsSectionListsTheSetsWithTheirLayerCounts();
    void theSetsSectionFoldsAndRemembersIt();
    void clickingASetShowsItAndShiftAddsIt();
    void hoveringASetShadesItsLayersRows();
    void theEyeAboveTheLayersShowsThemAllOrHidesThemAll();
    void aDragOnTheSetListReordersTheSets();
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

// Shows a widget and waits for the queued rebuild that fills the sets and the eye.
void showAndSettle(LayerWidget &widget)
{
    widget.resize(320, 460);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));
    QTest::qWait(50);
}

QTreeWidget *setList(LayerWidget &widget)
{
    return widget.findChild<QTreeWidget *>(QStringLiteral("visibilitySetList"));
}

QTreeWidget *meshTree(LayerWidget &widget)
{
    for (QTreeWidget *candidate : widget.findChildren<QTreeWidget *>()) {
        if (candidate->objectName() != QLatin1String("visibilitySetList")
            && candidate->topLevelItemCount() > 0)
            return candidate;
    }
    return nullptr;
}

QPoint rowCenter(QTreeWidget *tree, int row)
{
    return tree->visualItemRect(tree->topLevelItem(row)).center();
}

} // namespace

// Each set is a row with its full name, how many layers it holds, and the key that shows it (the
// first nine); the set on screen is marked; the section says how to start while there are none.
void LayerWidgetTests::theSetsSectionListsTheSetsWithTheirLayerCounts()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    addTinyMesh(doc, QStringLiteral("c"));
    LayerWidget widget(&doc);
    showAndSettle(widget);
    QTreeWidget *list = setList(widget);
    QVERIFY(list);
    QVERIFY(list->isHidden());   // nothing to list yet; the hint is shown instead

    doc.addVisibilitySet(QStringLiteral("everything, and with a name that is long"), doc.visibleMeshIds());
    doc.addVisibilitySet(QStringLiteral("only a"), { doc.mesh(0).meshId });
    QTest::qWait(50);
    QVERIFY(!list->isHidden());
    QCOMPARE(list->topLevelItemCount(), 2);
    // The whole name, not cut to fit.
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("everything, and with a name that is long"));
    QCOMPARE(list->topLevelItem(0)->text(1), QStringLiteral("3"));
    QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("Alt+1"));
    QCOMPARE(list->topLevelItem(1)->text(1), QStringLiteral("1"));
    QCOMPARE(list->topLevelItem(1)->text(2), QStringLiteral("Alt+2"));
    // Everything is visible, so the first set is the one on screen.
    QVERIFY(list->topLevelItem(0)->font(0).bold());
    QVERIFY(!list->topLevelItem(1)->font(0).bold());

    // The "+" keeps what is visible as a set, under the name asked for; its button is there.
    QVERIFY(widget.findChild<QToolButton *>(QStringLiteral("visibilitySetAdd")));
}

void LayerWidgetTests::theSetsSectionFoldsAndRemembersIt()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    doc.addVisibilitySet(QStringLiteral("a"), { doc.mesh(0).meshId });
    {
        LayerWidget widget(&doc);
        showAndSettle(widget);
        QTreeWidget *list = setList(widget);
        QToolButton *fold = nullptr;
        for (QToolButton *button : widget.findChildren<QToolButton *>()) {
            if (button->text() == QStringLiteral("Sets"))
                fold = button;
        }
        QVERIFY(fold);
        QVERIFY(list->isVisible());
        fold->click();
        QVERIFY(!list->isVisible());
    }
    // A new panel opens as the last one was left.
    LayerWidget again(&doc);
    showAndSettle(again);
    QVERIFY(!setList(again)->isVisible());
    for (QToolButton *button : again.findChildren<QToolButton *>()) {
        if (button->text() == QStringLiteral("Sets"))
            button->click();
    }
    QVERIFY(setList(again)->isVisible());
}

void LayerWidgetTests::clickingASetShowsItAndShiftAddsIt()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    addTinyMesh(doc, QStringLiteral("c"));
    doc.addVisibilitySet(QStringLiteral("a"), { doc.mesh(0).meshId });
    doc.addVisibilitySet(QStringLiteral("b"), { doc.mesh(1).meshId });
    LayerWidget widget(&doc);
    showAndSettle(widget);
    QTreeWidget *list = setList(widget);

    QTest::mouseClick(list->viewport(), Qt::LeftButton, {}, rowCenter(list, 0));
    QVERIFY(doc.mesh(0).visible && !doc.mesh(1).visible && !doc.mesh(2).visible);
    QTest::qWait(50);   // the marked row follows
    QVERIFY(list->topLevelItem(0)->font(0).bold());

    // Shift adds the set to what is visible.
    QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::ShiftModifier, rowCenter(list, 1));
    QVERIFY(doc.mesh(0).visible && doc.mesh(1).visible && !doc.mesh(2).visible);
}

// Hovering a row previews its set: the rows of its layers are shaded, the others are not, the
// detail rows go with their layer, and the shade goes when the pointer leaves.
void LayerWidgetTests::hoveringASetShadesItsLayersRows()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    doc.addVisibilitySet(QStringLiteral("only a"), { doc.mesh(0).meshId });
    LayerWidget widget(&doc);
    showAndSettle(widget);

    QTreeWidget *layers = meshTree(widget);
    QTreeWidget *list = setList(widget);
    QVERIFY(layers && list);

    // The right-hand end of each layer's row, clear of its text.
    const auto sample = [&](int row) {
        const QRect rect = layers->visualItemRect(layers->topLevelItem(row));
        return layers->viewport()->grab().toImage().pixelColor(rect.right() - 6, rect.center().y());
    };
    const auto detailSample = [&](int top) {
        const QRect rect = layers->visualItemRect(layers->topLevelItem(top)->child(0));
        return layers->viewport()->grab().toImage().pixelColor(rect.right() - 6, rect.center().y());
    };
    const QColor rowABefore = sample(0);
    const QColor rowBBefore = sample(1);

    const QPointF over = rowCenter(list, 0);
    QMouseEvent move(QEvent::MouseMove, over, list->viewport()->mapToGlobal(over),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(list->viewport(), &move);
    QTest::qWait(20);
    QVERIFY(sample(0) != rowABefore);
    QCOMPARE(sample(1), rowBBefore);
    QVERIFY(layers->topLevelItem(0)->childCount() > 0);
    QVERIFY(detailSample(0) != detailSample(1));

    const QString dir = qEnvironmentVariable("MESHLAB2_GRAB_DIR");
    if (!dir.isEmpty())
        widget.grab().save(dir + QStringLiteral("/layerwidget_hover.png"));

    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(list->viewport(), &leave);
    QTest::qWait(20);
    QCOMPARE(sample(0), rowABefore);
}

// The one eye, above the check marks: it shows every layer, or hides every layer when they all
// are shown, and says which it will do.
void LayerWidgetTests::theEyeAboveTheLayersShowsThemAllOrHidesThemAll()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    addTinyMesh(doc, QStringLiteral("b"));
    LayerWidget widget(&doc);
    showAndSettle(widget);
    auto *eye = widget.findChild<QToolButton *>(QStringLiteral("layerEyeAll"));
    QVERIFY(eye && eye->isEnabled());
    QVERIFY(eye->toolTip().contains(QStringLiteral("Hide")));

    eye->click();
    QVERIFY(!doc.mesh(0).visible && !doc.mesh(1).visible);
    QTest::qWait(50);
    QVERIFY(eye->toolTip().contains(QStringLiteral("Show")));

    // Partly shown: the next click shows the rest, rather than hiding what is shown.
    doc.setMeshVisible(0, true);
    QTest::qWait(50);
    QVERIFY(eye->toolTip().contains(QStringLiteral("Show")));
    eye->click();
    QVERIFY(doc.mesh(0).visible && doc.mesh(1).visible);

    // With no layers there is nothing to toggle.
    Document empty;
    LayerWidget emptyWidget(&empty);
    showAndSettle(emptyWidget);
    QVERIFY(!emptyWidget.findChild<QToolButton *>(QStringLiteral("layerEyeAll"))->isEnabled());
}

// Dragging a row of the set list is a move of the set in the document, which is one undo step and
// is what the Alt+number keys follow.
void LayerWidgetTests::aDragOnTheSetListReordersTheSets()
{
    Document doc;
    addTinyMesh(doc, QStringLiteral("a"));
    doc.addVisibilitySet(QStringLiteral("first"), { doc.mesh(0).meshId });
    doc.addVisibilitySet(QStringLiteral("second"), { doc.mesh(0).meshId });
    doc.addVisibilitySet(QStringLiteral("third"), { doc.mesh(0).meshId });
    LayerWidget widget(&doc);
    showAndSettle(widget);

    doc.moveVisibilitySet(0, 2);   // what the drop asks for
    QTest::qWait(50);
    QTreeWidget *list = setList(widget);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("second"));
    QCOMPARE(list->topLevelItem(2)->text(0), QStringLiteral("first"));
    QCOMPARE(list->topLevelItem(0)->text(2), QStringLiteral("Alt+1"));
    QVERIFY(doc.undo());
    QTest::qWait(50);
    QCOMPARE(list->topLevelItem(0)->text(0), QStringLiteral("first"));
}

QTEST_MAIN(LayerWidgetTests)
#include "test_layerwidget.moc"
