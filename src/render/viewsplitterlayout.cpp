#include "viewsplitterlayout.h"

#include <QList>
#include <QSplitter>
#include <QWidget>

namespace ViewSplitterLayout {

void collapseSingleChildSplitters(QSplitter *from, QSplitter *root)
{
    QSplitter *current = from;
    while (current && current != root) {
        if (current->count() != 1) {
            current = qobject_cast<QSplitter *>(current->parentWidget());
            continue;
        }

        QWidget *onlyChild = current->widget(0);
        auto *parent = qobject_cast<QSplitter *>(current->parentWidget());
        if (!onlyChild || !parent)
            break;

        // The parent keeps the same number of panes across the swap -- one splitter out,
        // its survivor in, at the same index -- so its distribution carries over unchanged.
        // Without this the survivor inherits whatever QSplitter assigns a fresh widget and
        // the neighbouring panes shift for no reason the user can see.
        const QList<int> parentSizes = parent->sizes();
        const int index = parent->indexOf(current);

        // Order matters. Detaching the empty splitter first keeps the parent's child list
        // correct for the insertWidget below; leaving it attached until deleteLater() runs
        // left the parent laying out a pane for a widget that was on its way out, so two
        // views would share the space three ways and one third of the splitter was dead.
        current->setParent(nullptr);
        parent->insertWidget(index, onlyChild);
        current->deleteLater();

        if (parentSizes.size() == parent->count())
            parent->setSizes(parentSizes);

        current = parent;
    }
}

int countEmptySplitters(const QSplitter *root)
{
    if (!root)
        return 0;
    int empty = (root->count() == 0) ? 1 : 0;
    for (int i = 0; i < root->count(); ++i) {
        if (const auto *child = qobject_cast<const QSplitter *>(root->widget(i)))
            empty += countEmptySplitters(child);
    }
    return empty;
}

} // namespace ViewSplitterLayout
