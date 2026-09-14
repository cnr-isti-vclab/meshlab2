#include <QtTest/QtTest>

#include <QLabel>
#include <QSplitter>

#include "viewsplitterlayout.h"

// The multi-view layout is a tree of nested QSplitters, and closing a view has to tidy it.
// Getting that wrong shows up as panes at a fraction of their width, or a dead gap where a
// splitter used to be -- and the widget hierarchy looks perfectly reasonable meanwhile,
// which is why this is worth pinning rather than eyeballing.
class ViewSplitterLayoutTests : public QObject
{
    Q_OBJECT

private slots:
    void closingANestedPaneLeavesNoEmptySplitterBehind();
    void collapsingKeepsTheSurvivingPaneSizes();
    void collapsingCascadesThroughSeveralLevels();
    void rootIsKeptEvenWhenItHoldsOneChild();
};

namespace {

// A stand-in for a RenderWidget: the tree surgery only ever sees QWidget.
QWidget *pane(const QString &name)
{
    auto *w = new QLabel(name);
    w->setObjectName(name);
    return w;
}

QStringList paneOrder(const QSplitter *splitter)
{
    QStringList names;
    for (int i = 0; i < splitter->count(); ++i) {
        const QWidget *child = splitter->widget(i);
        if (const auto *nested = qobject_cast<const QSplitter *>(child))
            names << paneOrder(nested);
        else
            names << child->objectName();
    }
    return names;
}

} // namespace

void ViewSplitterLayoutTests::closingANestedPaneLeavesNoEmptySplitterBehind()
{
    // root[ A, nested[ B, C ] ] -- split once, then split the right pane across it. Closing
    // B is the case that went wrong: not a leaf of the deepest level from the parent's point
    // of view, because removing it empties the splitter that held it.
    QSplitter root(Qt::Horizontal);
    QWidget *a = pane(QStringLiteral("A"));
    auto *nested = new QSplitter(Qt::Vertical);
    QWidget *b = pane(QStringLiteral("B"));
    QWidget *c = pane(QStringLiteral("C"));
    root.addWidget(a);
    root.addWidget(nested);
    nested->addWidget(b);
    nested->addWidget(c);
    root.resize(600, 400);
    root.show();
    QVERIFY(QTest::qWaitForWindowExposed(&root));

    b->setParent(nullptr);
    b->deleteLater();
    ViewSplitterLayout::collapseSingleChildSplitters(nested, &root);

    // The heart of it: the emptied splitter must be gone from the parent's child list NOW,
    // not once the event loop gets round to deleteLater(). Left attached it still claims a
    // share of the width, so two surviving views shared the space three ways.
    QCOMPARE(root.count(), 2);
    QCOMPARE(ViewSplitterLayout::countEmptySplitters(&root), 0);
    QCOMPARE(paneOrder(&root), (QStringList{ QStringLiteral("A"), QStringLiteral("C") }));

    // And nothing has zero width, which is what the bug looked like on screen.
    for (int size : root.sizes())
        QVERIFY2(size > 0, "a surviving pane collapsed to zero width");
}

void ViewSplitterLayoutTests::collapsingKeepsTheSurvivingPaneSizes()
{
    QSplitter root(Qt::Horizontal);
    QWidget *a = pane(QStringLiteral("A"));
    auto *nested = new QSplitter(Qt::Vertical);
    root.addWidget(a);
    root.addWidget(nested);
    nested->addWidget(pane(QStringLiteral("B")));
    QWidget *c = pane(QStringLiteral("C"));
    nested->addWidget(c);
    root.resize(600, 400);
    root.show();
    QVERIFY(QTest::qWaitForWindowExposed(&root));

    // A deliberately lopsided split, so a distribution that is merely plausible cannot pass.
    root.setSizes(QList<int>{ 450, 150 });
    const QList<int> before = root.sizes();

    root.widget(1)->layout(); // no-op; keeps the compiler honest about the cast below
    nested->widget(0)->setParent(nullptr);
    ViewSplitterLayout::collapseSingleChildSplitters(nested, &root);

    QCOMPARE(root.count(), 2);
    QCOMPARE(root.sizes(), before);
    QCOMPARE(paneOrder(&root), (QStringList{ QStringLiteral("A"), QStringLiteral("C") }));
}

void ViewSplitterLayoutTests::collapsingCascadesThroughSeveralLevels()
{
    // root[ A, n1[ n2[ B ] ] ] -- what repeated splitting and closing leaves behind. One
    // call has to unwind the whole chain, or the next close starts from a bad tree.
    QSplitter root(Qt::Horizontal);
    root.addWidget(pane(QStringLiteral("A")));
    auto *n1 = new QSplitter(Qt::Vertical);
    auto *n2 = new QSplitter(Qt::Horizontal);
    root.addWidget(n1);
    n1->addWidget(n2);
    n2->addWidget(pane(QStringLiteral("B")));
    root.resize(600, 400);
    root.show();
    QVERIFY(QTest::qWaitForWindowExposed(&root));

    ViewSplitterLayout::collapseSingleChildSplitters(n2, &root);

    QCOMPARE(ViewSplitterLayout::countEmptySplitters(&root), 0);
    QCOMPARE(paneOrder(&root), (QStringList{ QStringLiteral("A"), QStringLiteral("B") }));
    // Both intermediate splitters are gone, not just the innermost.
    QCOMPARE(root.count(), 2);
    QVERIFY(qobject_cast<QSplitter *>(root.widget(1)) == nullptr);
}

void ViewSplitterLayoutTests::rootIsKeptEvenWhenItHoldsOneChild()
{
    // The root is the container the views live in; collapsing it away would leave the
    // window with nothing to put them in.
    QSplitter root(Qt::Horizontal);
    auto *nested = new QSplitter(Qt::Vertical);
    root.addWidget(nested);
    nested->addWidget(pane(QStringLiteral("A")));
    root.resize(400, 300);
    root.show();
    QVERIFY(QTest::qWaitForWindowExposed(&root));

    ViewSplitterLayout::collapseSingleChildSplitters(nested, &root);

    QCOMPARE(root.count(), 1);
    QCOMPARE(paneOrder(&root), (QStringList{ QStringLiteral("A") }));
    QCOMPARE(ViewSplitterLayout::countEmptySplitters(&root), 0);
}

QTEST_MAIN(ViewSplitterLayoutTests)
#include "test_viewsplitterlayout.moc"
