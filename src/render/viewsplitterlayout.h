#pragma once

class QSplitter;
class QWidget;

// Splitter-tree surgery for the multi-view layout.
//
// Views live in a tree of nested QSplitters: splitting along the current orientation adds a
// pane to the existing splitter, splitting across it replaces the pane with a new splitter
// holding both. Closing a view therefore has to tidy the tree afterwards, and getting that
// wrong is invisible in the widget hierarchy but very visible on screen -- panes at a third
// of their width, or a dead gap where a splitter used to be.
//
// Lives here, rather than in MainWindow, so it can be tested: these are plain QSplitters and
// need no document, no render context and no window.
namespace ViewSplitterLayout {

// Lifts the survivor out of every splitter left holding a single child, walking up from
// `from` and stopping at `root`, which is kept even when it holds one child because it is
// the container the views live in.
//
// Removal is immediate, not deferred: a splitter awaiting deleteLater() is still a child of
// its parent and still gets a share of the space, which is what produced the squeezed panes.
// Destruction itself stays deferred, since this can run from a signal handler.
void collapseSingleChildSplitters(QSplitter *from, QSplitter *root);

// Number of splitters below `root` (inclusive) that hold no children. Zero for a healthy
// tree; anything else means a collapse was missed or deferred.
int countEmptySplitters(const QSplitter *root);

} // namespace ViewSplitterLayout
