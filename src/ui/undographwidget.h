#pragma once

#include "document.h"
#include "undographlayout.h"
#include <QAbstractScrollArea>
#include <QMap>
#include <QPixmap>
#include <QSet>
#include <QVector>

// UndoGraphWidget renders the undo history as a git-style lane graph.
//
// Layout (UndoGraphLayout decides it; this widget paints it):
//   - Each row = one state of the undo tree, newest at top, oldest at bottom, except that
//     repeats fold: tries of one filter from the same state, and runs of one action, share a
//     row, drawn with a strip of dots under the label, one per state. Tries stand apart;
//     a run's dots are joined, since its states follow one another -- dashed, and muted,
//     where the current state does not come from them. When exactly one parameter tells the
//     states apart, each dot is labelled with its value.
//   - A row that left a run from an earlier step hangs from that step: its dot carries a
//     notch, the row takes a column of its own, and hovering either points at the other.
//   - Each lane (column) carries one branch: a row's oldest child continues its column and
//     later ones open new columns, in the order they were made.
//   - A filled circle is drawn on the row's lane; a ring marks the current state.
//   - Vertical lines connect parent↔child within the same lane; a branch runs down its own
//     lane and curves into its parent's row.
//
// Interaction:
//   - Double-click a row or a dot → jumpToNode(nodeId)
//   - Hover a thumbnail or a dot → large snapshot popup (via signal), with a caption saying
//     which try or step it is and what differs
//   - Click a folded row's count, or its "+N", to expand it into a row per state; click the
//     count again to fold it back

class UndoGraphWidget : public QAbstractScrollArea
{
    Q_OBJECT
public:
    explicit UndoGraphWidget(QWidget *parent = nullptr);
    static QSize thumbnailSize() { return QSize(kThumbW, kThumbH); }

    // Where the parameters of each state are read from, to label folded rows.
    void setDocument(const Document *doc) { m_doc = doc; }

    void setNodes(const QVector<UndoTreeNodeInfo> &nodes, const QMap<int, QPixmap> &thumbnails);

signals:
    // Emitted when the user requests to jump to a node.
    // withCamera=true  → restore data AND camera (Ctrl/Cmd+double-click or context menu)
    // withCamera=false → restore data only (plain double-click or context menu)
    void nodeActivated(int nodeId, bool withCamera);
    // Emitted when the user requests to store the current view into states and retake their
    // thumbnails: the one right-clicked, or every state of a folded row.
    void updateCameraRequested(const QVector<int> &nodeIds);
    // Emitted when the user requests to make a node the new history root.
    void nodeMakeRootRequested(int nodeId);
    // Emitted when the user requests to delete all descendants of a node.
    void nodePurgeBranchRequested(int nodeId);
    // Emitted when the user wants to vary the action that produced a node: return to the
    // state it was invoked from, with its filter reopened on the same parameters.
    void nodeReopenFilterRequested(int nodeId);
    // Emitted when the user requests to keep only the path root→current, removing all branches.
    void linearizeHistoryRequested();
    void generatePythonScriptRequested();
    // Emitted while hovering a thumbnail or a dot. `caption` is empty for a state with a
    // row of its own; for one on a folded row it says which try or step it is, and what
    // differs from the others.
    void nodeHovered(int nodeId, const QPoint &globalPos, const QString &caption);
    void nodeUnhovered();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    QSize sizeHint() const override;

private:
    // Where a folded row's strip puts things, shared by painting and hit-testing.
    struct StripItem {
        int nodeId = -1;
        QPoint dot;
        QRect text; // the value label, empty without one
        QRect hit;
    };
    struct Strip {
        QRect name;     // the varied parameter's label
        QRect overflow; // "+N" for the older states that do not fit
        int hidden = 0;
        QVector<StripItem> items;
        QRect toggle;   // the count and chevron
    };

    void rebuildRows();
    int  rowAt(int y) const;
    QRect rowRect(int row) const;
    QRect thumbnailRect(int row) const;
    int  laneX(int lane) const;
    void updateScrollBars();
    QFont stripFont() const;
    QString toggleText(const UndoGraphLayout::Row &row) const;
    Strip stripLayout(int row) const;
    void paintStrip(QPainter &p, int row) const;
    // The state a dot at pos on `row` stands for, or -1 when pos is on no dot.
    int stripNodeAt(int row, const QPoint &pos) const;
    // The state a click or hover at pos on `row` means: a dot's, else the row's own.
    int nodeAt(int row, const QPoint &pos) const;
    bool onStripToggle(int row, const QPoint &pos) const;
    void toggleGroup(int row);
    QString caption(int row, int nodeId) const;

    // ---- data ----
    const Document                     *m_doc = nullptr;
    QVector<UndoTreeNodeInfo>           m_nodes; // original tree info (DFS pre-order)
    QMap<int, QPixmap>                  m_thumbnails;
    QSet<quint64>                       m_expandedGroups; // by UndoGraphLayout::Row::groupKey

    // ---- layout ----
    UndoGraphLayout::Layout                   m_layout;      // row 0 = newest (top)
    QVector<UndoGraphLayout::Differences>     m_differences; // per row; empty for plain rows

    // ---- geometry constants ----
    static constexpr int kThumbW      = 80;  // thumbnail width  (2:1 aspect ratio)
    static constexpr int kThumbH      = 40;  // thumbnail height
    static constexpr int kRowHeight   = 40;  // keep rows visually compact
    static constexpr int kLaneWidth   = 20;
    static constexpr int kDotRadius   = 6;
    static constexpr int kTextLeft    = 8;   // gap between graph area and text
    // A row with a strip puts its label in the top line and the strip below it.
    static constexpr int kLabelLineH      = 22;
    static constexpr int kStripY          = 30; // strip centre, from the row's top
    static constexpr int kStripDotRadius  = 4;
    static constexpr int kStripDotTextGap = 3;
    static constexpr int kStripSpacing    = 8;
    static constexpr int kStripGap        = 6;
    static constexpr int kStripValueMaxW  = 64;
    // The mark on a step a branch left from: a tick above its dot, short enough to clear the
    // descenders of the label above.
    static constexpr int kNotchLength     = 3;

    int m_hoveredRow = -1;
    int m_hoveredMember = -1; // the state whose dot is under the mouse
};
