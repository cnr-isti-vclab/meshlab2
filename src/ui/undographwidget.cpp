#include "undographwidget.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QHash>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <algorithm>
#include <cmath>

using UndoGraphLayout::Fold;
using UndoGraphLayout::Member;
using UndoGraphLayout::Row;

// Single colour for all lanes – use the palette's highlight colour.
static QColor laneColor(int /*lane*/, const QPalette &pal)
{
    return pal.color(QPalette::Highlight);
}

// ─────────────────────────────────────────────────────────────────────────────
UndoGraphWidget::UndoGraphWidget(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setMouseTracking(true);
    if (viewport())
        viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void UndoGraphWidget::setNodes(const QVector<UndoTreeNodeInfo> &nodes,
                               const QMap<int, QPixmap> &thumbnails)
{
    m_nodes         = nodes;
    m_thumbnails    = thumbnails;
    m_hoveredRow    = -1;
    m_hoveredMember = -1;
    rebuildRows();
    updateScrollBars();
    viewport()->update();
}

// ─────────────────────────────────────────────────────────────────────────────
// rebuildRows – the rows and lanes come from UndoGraphLayout; what is left here is
// reading the parameters each folded state was made with, to tell them apart.
// ─────────────────────────────────────────────────────────────────────────────
void UndoGraphWidget::rebuildRows()
{
    m_layout = UndoGraphLayout::build(m_nodes, m_expandedGroups);
    m_differences = QVector<UndoGraphLayout::Differences>(m_layout.rows.size());
    if (!m_doc)
        return;
    for (int r = 0; r < m_layout.rows.size(); ++r) {
        const Row &row = m_layout.rows[r];
        if (row.members.isEmpty() || row.filterKey.isEmpty())
            continue;
        const std::optional<MeshFilterDescriptor> descriptor = m_doc->filterDescriptor(row.filterKey);
        if (!descriptor)
            continue;
        QVector<QVariantMap> parameters;
        for (const Member &m : row.members) {
            const std::optional<ScriptAction> action = m_doc->undoNodeAction(m.nodeId);
            parameters.append(action ? action->params : QVariantMap());
        }
        m_differences[r] = UndoGraphLayout::differences(*descriptor, parameters);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void UndoGraphWidget::updateScrollBars()
{
    const int contentH = m_layout.rows.size() * kRowHeight;
    const int graphW   = m_layout.laneCount * kLaneWidth + kLaneWidth / 2;
    // Text column gets the remaining width.
    const int textW    = std::max(200, viewport()->width() - graphW - kThumbW - kTextLeft * 2);
    const int contentW = graphW + kTextLeft + textW + kThumbW;

    verticalScrollBar()->setRange(0, std::max(0, contentH - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(kRowHeight);
    horizontalScrollBar()->setRange(0, std::max(0, contentW - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(20);
}

int UndoGraphWidget::laneX(int lane) const
{
    // Centre of the lane circle in content coordinates (before scroll offset).
    return kLaneWidth / 2 + lane * kLaneWidth;
}

int UndoGraphWidget::rowAt(int y) const
{
    const int cy = y + verticalScrollBar()->value();
    const int r  = cy / kRowHeight;
    return (cy >= 0 && r < m_layout.rows.size()) ? r : -1;
}

QRect UndoGraphWidget::rowRect(int row) const
{
    const int y = row * kRowHeight - verticalScrollBar()->value();
    return QRect(0, y, viewport()->width(), kRowHeight);
}

QRect UndoGraphWidget::thumbnailRect(int row) const
{
    const int vw = viewport()->width();
    const int x = vw - kThumbW - 4;
    const int y = row * kRowHeight - verticalScrollBar()->value();
    return QRect(x, y, kThumbW, kThumbH);
}

QSize UndoGraphWidget::sizeHint() const
{
    return QSize(280, 200);
}

// ─────────────────────────────────────────────────────────────────────────────
// The strip of a folded row: its count and chevron at the right end, the varied
// parameter's label and as many of the newest states as fit before it, and "+N" for
// the older ones that do not.
// ─────────────────────────────────────────────────────────────────────────────
QFont UndoGraphWidget::stripFont() const
{
    QFont f = font();
    if (f.pointSizeF() > 0)
        f.setPointSizeF(f.pointSizeF() * 0.85);
    return f;
}

QString UndoGraphWidget::toggleText(const Row &row) const
{
    const int n = int(row.members.size());
    const QString count = row.fold == Fold::Tries ? tr("%n tries", nullptr, n)
                                                  : tr("%n steps", nullptr, n);
    return count + (row.expanded ? QStringLiteral(" ▾") : QStringLiteral(" ▸"));
}

UndoGraphWidget::Strip UndoGraphWidget::stripLayout(int ri) const
{
    Strip s;
    if (ri < 0 || ri >= m_layout.rows.size())
        return s;
    const Row &row = m_layout.rows[ri];
    if (row.members.isEmpty())
        return s;
    const QFontMetrics fm(stripFont());
    // Rounded up from the fractional advance: eliding measures the text that finely, and a
    // label a fraction of a pixel wider than its slot would lose its last character.
    const QFontMetricsF fmf(stripFont());
    const auto textWidth = [&](const QString &text) {
        return int(std::ceil(fmf.horizontalAdvance(text)));
    };
    const int scrollX = horizontalScrollBar()->value();
    const int left  = m_layout.laneCount * kLaneWidth + kLaneWidth / 2 + kTextLeft - scrollX;
    const int right = viewport()->width() - kThumbW - 4 - kTextLeft;
    const int cy    = ri * kRowHeight - verticalScrollBar()->value() + kStripY;
    const int h     = fm.height();
    const auto lineRect = [&](int x, int w) { return QRect(x, cy - h / 2, w, h); };

    // The control sits at the right end, so however many states there are, it stays in reach.
    const int toggleW = textWidth(toggleText(row));
    s.toggle = lineRect(right - toggleW, toggleW);
    if (row.expanded)
        return s;

    int x = left;
    const UndoGraphLayout::Differences &d = m_differences.at(ri);
    if (!d.variedLabel.isEmpty()) {
        const int w = std::min(textWidth(d.variedLabel + QLatin1Char(':')),
                               std::max(0, right - left) / 3);
        s.name = lineRect(x, w);
        x += w + kStripGap;
    }
    const int n = int(row.members.size());
    QVector<int> widths(n);
    for (int i = 0; i < n; ++i) {
        widths[i] = 2 * kStripDotRadius;
        if (i < d.values.size())
            widths[i] += kStripDotTextGap + std::min(textWidth(d.values[i]), kStripValueMaxW);
    }
    const auto spacingAfter = [&](int i) { return i < n - 1 ? kStripSpacing : 0; };
    const int limit = s.toggle.left() - kStripGap;
    int first = n;
    int used = 0;
    for (int i = n - 1; i >= 0; --i) {
        const int need = widths[i] + spacingAfter(i);
        if (x + used + need > limit)
            break;
        used += need;
        first = i;
    }
    // Room for the "+N" marker, which can push out one more state, and then grows by one.
    const auto markerWidth = [&](int hidden) {
        return textWidth(QStringLiteral("+%1").arg(hidden)) + kStripSpacing;
    };
    while (first > 0 && first < n && x + markerWidth(first) + used > limit) {
        used -= widths[first] + spacingAfter(first);
        ++first;
    }
    s.hidden = first;
    if (first > 0) {
        const int w = markerWidth(first) - kStripSpacing;
        s.overflow = lineRect(x, w);
        x += w + kStripSpacing;
    }
    for (int i = first; i < n; ++i) {
        StripItem item;
        item.nodeId = row.members[i].nodeId;
        item.dot = QPoint(x + kStripDotRadius, cy);
        if (i < d.values.size()) {
            const int textX = x + 2 * kStripDotRadius + kStripDotTextGap;
            item.text = lineRect(textX, x + widths[i] - textX);
        }
        item.hit = QRect(x - kStripSpacing / 2, cy - h / 2 - 2, widths[i] + kStripSpacing, h + 4);
        s.items.append(item);
        x += widths[i] + kStripSpacing;
    }
    return s;
}

void UndoGraphWidget::paintStrip(QPainter &p, int ri) const
{
    const Row &row = m_layout.rows[ri];
    const Strip s = stripLayout(ri);
    const UndoGraphLayout::Differences &d = m_differences.at(ri);
    const QFont font = stripFont();
    const QFontMetrics fm(font);
    const QColor lane = laneColor(row.lane, palette());
    const QColor muted = palette().color(QPalette::PlaceholderText);
    const QColor offPath = palette().color(QPalette::Mid);
    const auto memberIndex = [&row](int nodeId) {
        int mi = 0;
        while (row.members[mi].nodeId != nodeId)
            ++mi;
        return mi;
    };
    // The state the hovered row left from, when it left this row from an earlier one.
    int pointedAt = -1;
    if (m_hoveredRow >= 0 && m_hoveredRow < m_layout.rows.size()) {
        const Row &hovered = m_layout.rows[m_hoveredRow];
        if (hovered.parentRow == ri && hovered.parentId != row.nodeId)
            pointedAt = hovered.parentId;
    }

    p.save();
    p.setFont(font);
    p.setPen(muted);
    p.drawText(s.toggle, Qt::AlignVCenter | Qt::AlignRight | Qt::TextSingleLine, toggleText(row));
    if (!row.expanded) {
        if (!s.name.isEmpty())
            p.drawText(s.name, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                       fm.elidedText(d.variedLabel + QLatin1Char(':'), Qt::ElideRight, s.name.width()));
        if (!s.overflow.isEmpty())
            p.drawText(s.overflow, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                       QStringLiteral("+%1").arg(s.hidden));
        // A run's states follow one another, so lines join them: solid along the path to the
        // current state, dashed where it does not come from. Tries are alternatives and stand
        // apart.
        if (row.fold == Fold::Run) {
            for (int i = 0; i + 1 < s.items.size(); ++i) {
                const bool onPath = row.members[memberIndex(s.items[i].nodeId)].isOnCurrentPath
                    && row.members[memberIndex(s.items[i + 1].nodeId)].isOnCurrentPath;
                QPen join(onPath ? lane : offPath, 1.5);
                if (!onPath)
                    join.setDashPattern({2.0, 2.0});
                p.setPen(join);
                p.drawLine(s.items[i].dot, s.items[i + 1].dot);
            }
        }
        for (const StripItem &item : s.items) {
            const int mi = memberIndex(item.nodeId);
            const Member &m = row.members[mi];
            const bool pointed = item.nodeId == m_hoveredMember || item.nodeId == pointedAt;
            const int radius = kStripDotRadius + (pointed ? 2 : 0);
            if (m.isCurrent) {
                p.setPen(QPen(lane.lighter(150), 1.5));
                p.setBrush(lane);
            } else {
                p.setPen(QPen(m.isOnCurrentPath ? lane : offPath, 1.5));
                p.setBrush(palette().color(QPalette::Base));
            }
            // A row left from this state: a notch on top of its dot points up at it. Placed for
            // the dot at rest, so enlarging it on hover covers the notch's foot instead of
            // pushing the notch into the label.
            if (m.branches) {
                const int foot = item.dot.y() - kStripDotRadius - 1;
                p.save();
                p.setPen(QPen(lane, 2.0, Qt::SolidLine, Qt::FlatCap));
                p.drawLine(item.dot.x(), foot, item.dot.x(), foot - kNotchLength);
                p.restore();
            }
            p.drawEllipse(item.dot, radius, radius);
            if (!item.text.isEmpty()) {
                // The state on the way to the current one reads as kept, the others as set aside.
                p.setPen(m.isOnCurrentPath ? palette().color(QPalette::Text) : muted);
                p.drawText(item.text, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextSingleLine,
                           fm.elidedText(d.values[mi], Qt::ElideRight, item.text.width()));
            }
        }
    }
    p.restore();
}

int UndoGraphWidget::stripNodeAt(int ri, const QPoint &pos) const
{
    const Strip s = stripLayout(ri);
    for (const StripItem &item : s.items)
        if (item.hit.contains(pos))
            return item.nodeId;
    return -1;
}

int UndoGraphWidget::nodeAt(int ri, const QPoint &pos) const
{
    const int member = stripNodeAt(ri, pos);
    return member >= 0 ? member : m_layout.rows[ri].displayNodeId;
}

bool UndoGraphWidget::onStripToggle(int ri, const QPoint &pos) const
{
    const Strip s = stripLayout(ri);
    return s.toggle.contains(pos) || s.overflow.contains(pos);
}

void UndoGraphWidget::toggleGroup(int ri)
{
    const int key = m_layout.rows[ri].groupKey;
    if (!m_expandedGroups.remove(key))
        m_expandedGroups.insert(key);
    m_hoveredMember = -1;
    rebuildRows();
    updateScrollBars();
    viewport()->update();
}

QString UndoGraphWidget::caption(int ri, int nodeId) const
{
    const Row &row = m_layout.rows[ri];
    if (row.members.isEmpty() || row.expanded)
        return {};
    int index = -1;
    for (int i = 0; i < row.members.size(); ++i)
        if (row.members[i].nodeId == nodeId)
            index = i;
    if (index < 0)
        return {};
    const int n = int(row.members.size());
    const QString position = row.fold == Fold::Tries ? tr("Try %1 of %2").arg(index + 1).arg(n)
                                                     : tr("Step %1 of %2").arg(index + 1).arg(n);
    QStringList parts{position};
    const QString differs = m_differences.at(ri).perState.value(index);
    if (!differs.isEmpty())
        parts << differs;
    if (row.members[index].branches) {
        QStringList left;
        for (const Row &other : m_layout.rows)
            if (other.parentRow == ri && other.parentId == nodeId)
                left << other.label;
        parts << (left.size() == 1 ? tr("%1 branches here").arg(left.front())
                                   : tr("%1 branch here").arg(left.join(QStringLiteral(", "))));
    }
    return parts.join(QStringLiteral(" · "));
}

// ─────────────────────────────────────────────────────────────────────────────
// paintEvent
// ─────────────────────────────────────────────────────────────────────────────
void UndoGraphWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter p(viewport());
    p.setRenderHint(QPainter::Antialiasing);

    const int scrollY = verticalScrollBar()->value();
    const int scrollX = horizontalScrollBar()->value();
    const int vw      = viewport()->width();
    const int vh      = viewport()->height();
    const QVector<Row> &rows = m_layout.rows;

    // Background
    p.fillRect(viewport()->rect(), palette().color(QPalette::Base));

    if (rows.isEmpty()) {
        p.setPen(palette().color(QPalette::Mid));
        p.drawText(viewport()->rect(), Qt::AlignCenter, tr("No undo history"));
        return;
    }

    const int graphAreaW = m_layout.laneCount * kLaneWidth + kLaneWidth / 2;
    const int thumbColX  = vw - kThumbW - 4 + scrollX; // right-aligned

    // ── pass 1: row backgrounds (hover / current) ────────────────────────
    for (int ri = 0; ri < rows.size(); ++ri) {
        const Row &row = rows[ri];
        const int  top = ri * kRowHeight - scrollY;
        if (top > vh || top + kRowHeight < 0)
            continue;
        // Hovering a state that rows left from lights those rows up too.
        const bool leftHoveredState = m_hoveredMember >= 0 && row.parentId == m_hoveredMember
            && row.parentRow >= 0 && rows[row.parentRow].nodeId != m_hoveredMember;
        if (row.isCurrent)
            p.fillRect(QRect(0, top, vw, kRowHeight),
                       QColor(173, 216, 230)); // light blue
        else if (ri == m_hoveredRow || leftHoveredState)
            p.fillRect(QRect(0, top, vw, kRowHeight),
                       palette().color(QPalette::AlternateBase));
    }

    // ── pass 2: connector lines ───────────────────────────────────────────
    for (int ri = 0; ri < rows.size(); ++ri) {
        const Row &row = rows[ri];
        if (row.parentRow < 0)
            continue;
        const Row &parentRow = rows[row.parentRow];

        const int x1 = laneX(row.lane)       - scrollX;
        const int y1 = ri * kRowHeight + kRowHeight / 2 - scrollY;
        const int x2 = laneX(parentRow.lane) - scrollX;
        const int y2 = row.parentRow * kRowHeight + kRowHeight / 2 - scrollY;

        // Skip entirely off-screen connectors.
        if ((y1 < -kRowHeight && y2 < -kRowHeight) || (y1 > vh + kRowHeight && y2 > vh + kRowHeight))
            continue;

        QPen linePen(laneColor(row.lane, palette()), 2.0f);
        linePen.setCapStyle(Qt::RoundCap);
        p.setPen(linePen);

        if (row.lane == parentRow.lane) {
            p.drawLine(x1, y1, x2, y2);
        } else {
            // Draw straight on the child's lane all the way down to just above
            // the parent's row, then a short curve into the parent.
            // This places the visible kink at the parent row (= the actual branch
            // point), not halfway between child and parent.
            const int cornerY = y2 - kRowHeight / 2;
            QPainterPath path;
            path.moveTo(x1, y1);
            path.lineTo(x1, cornerY);
            path.quadTo(x1, y2, x2, y2);
            p.strokePath(path, linePen);
        }
    }

    // ── pass 3: dots, thumbnails, labels and strips ──────────────────────
    for (int ri = 0; ri < rows.size(); ++ri) {
        const Row  &row = rows[ri];
        const int   cy  = ri * kRowHeight + kRowHeight / 2 - scrollY;
        const int   top = ri * kRowHeight - scrollY;

        if (cy < -kRowHeight || cy > vh + kRowHeight)
            continue;

        const int cx = laneX(row.lane) - scrollX;
        const QColor col = laneColor(row.lane, palette());

        // Node dot
        if (row.isCurrent) {
            // Filled circle + outer ring
            p.setPen(QPen(col.lighter(150), 2));
            p.setBrush(col);
            p.drawEllipse(QPoint(cx, cy), kDotRadius, kDotRadius);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(col.lighter(180), 1.5));
            p.drawEllipse(QPoint(cx, cy), kDotRadius + 3, kDotRadius + 3);
        } else {
            p.setPen(QPen(col, 2));
            p.setBrush(palette().color(QPalette::Base));
            p.drawEllipse(QPoint(cx, cy), kDotRadius, kDotRadius);
        }

        // Thumbnail (right-aligned): the dot under the mouse, else the state the row shows.
        const bool hoveringStrip = !row.expanded
            && std::any_of(row.members.cbegin(), row.members.cend(),
                           [this](const Member &m) { return m.nodeId == m_hoveredMember; });
        const QPixmap thumb = m_thumbnails.value(hoveringStrip ? m_hoveredMember : row.displayNodeId);
        if (!thumb.isNull()) {
            const int tx = thumbColX - scrollX;
            const int ty = top;
            p.drawPixmap(tx, ty, kThumbW, kThumbH, thumb);
        }

        // Text: centred, or on the top line when a strip takes the bottom one.
        const int textX = graphAreaW + kTextLeft - scrollX;
        const int textW = std::max(10, thumbColX - scrollX - graphAreaW - kTextLeft * 2);
        const QRect textRect = row.members.isEmpty() ? QRect(textX, top, textW, kRowHeight)
                                                     : QRect(textX, top, textW, kLabelLineH);

        QFont f = p.font();
        const bool bold = row.isCurrent;
        if (f.bold() != bold) { f.setBold(bold); p.setFont(f); }

        QColor textColor = palette().color(QPalette::Text);
        if (!row.isOnCurrentPath)
            textColor = palette().color(QPalette::Mid);
        p.setPen(textColor);
        const QString label = row.label.isEmpty() ? tr("Initial state") : row.label;
        p.drawText(textRect, Qt::AlignVCenter | Qt::TextSingleLine,
                   p.fontMetrics().elidedText(label, Qt::ElideRight, textW));

        if (!row.members.isEmpty())
            paintStrip(p, ri);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void UndoGraphWidget::mousePressEvent(QMouseEvent *event)
{
    const int ri = rowAt(event->pos().y());
    if (event->button() == Qt::LeftButton && ri >= 0 && onStripToggle(ri, event->pos())) {
        toggleGroup(ri);
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void UndoGraphWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int ri = rowAt(event->pos().y());
    if (ri >= 0) {
        // The second click of a double-click on the count arrives here instead of as a
        // press; it toggles again, as two single clicks would.
        if (event->button() == Qt::LeftButton && onStripToggle(ri, event->pos())) {
            toggleGroup(ri);
            event->accept();
            return;
        }
        const bool withCamera =
            event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier);
        emit nodeActivated(nodeAt(ri, event->pos()), withCamera);
    }
    QAbstractScrollArea::mouseDoubleClickEvent(event);
}

void UndoGraphWidget::contextMenuEvent(QContextMenuEvent *event)
{
    const int ri = rowAt(event->pos().y());
    if (ri < 0) {
        QAbstractScrollArea::contextMenuEvent(event);
        return;
    }
    // The menu acts on the dot under the mouse, or on the state the row shows -- except that
    // a camera update on a folded row, anywhere but on a dot, goes to every state it holds.
    const int nodeId = nodeAt(ri, event->pos());
    const Row &row = m_layout.rows[ri];
    QVector<int> cameraTargets{nodeId};
    if (!row.members.isEmpty() && !row.expanded && stripNodeAt(ri, event->pos()) < 0) {
        cameraTargets.clear();
        for (const Member &m : row.members)
            cameraTargets.append(m.nodeId);
    }

    const auto infoFor = [this](int id) -> const UndoTreeNodeInfo * {
        for (const UndoTreeNodeInfo &n : m_nodes)
            if (n.nodeId == id)
                return &n;
        return nullptr;
    };
    QHash<int, int> childCounts;
    for (const UndoTreeNodeInfo &n : m_nodes)
        if (n.parentId >= 0)
            ++childCounts[n.parentId];

    // Non-empty only for nodes a filter produced, and only meaningful when there is a
    // parent state to stand in while varying it.
    const UndoTreeNodeInfo *node = infoFor(nodeId);
    const bool canReopenFilter = node && !node->filterKey.isEmpty() && node->parentId >= 0;

    QMenu menu(this);
    QAction *restoreAct      = menu.addAction(tr("Restore state"));
    restoreAct->setToolTip(tr("Restore the document to this state (data only)"));
    QAction *restoreCamAct   = menu.addAction(tr("Restore state and camera"));
    restoreCamAct->setToolTip(tr("Restore the document to this state including the camera"));
    QAction *reopenFilterAct = menu.addAction(tr("Undo this action and reopen its filter"));
    reopenFilterAct->setToolTip(
        tr("Return to the state this action started from and reopen its filter with the "
           "same parameters, ready to try a variation."));
    reopenFilterAct->setEnabled(canReopenFilter);
    menu.addSeparator();
    const int targets = int(cameraTargets.size());
    QAction *updateCamAct = menu.addAction(
        targets == 1 ? tr("Update camera")
        : row.fold == Fold::Tries ? tr("Update camera for all %n tries", nullptr, targets)
                                  : tr("Update camera for all %n steps", nullptr, targets));
    updateCamAct->setToolTip(
        targets == 1 ? tr("Store the current view into this state and retake its thumbnail")
                     : tr("Store the current view into each of these states and retake their "
                          "thumbnails"));
    menu.addSeparator();
    QAction *makeRootAct     = menu.addAction(tr("Make Root"));
    makeRootAct->setToolTip(
        tr("Make this state the new root of the history tree. "
           "All ancestor states and any branches attached to them will be permanently deleted."));
    QAction *purgeBranchAct  = menu.addAction(tr("Purge Branch"));
    purgeBranchAct->setToolTip(
        tr("Delete all states derived from this one. "
           "This state is preserved; all its descendants are permanently removed."));
    // Disable actions that would be no-ops.
    makeRootAct->setEnabled(node && node->parentId >= 0);
    purgeBranchAct->setEnabled(childCounts.value(nodeId) > 0);

    // Linearize is useful only when some state has more than one child; folded tries can
    // hide that from the lanes, so it is counted on the tree itself.
    const bool hasAnyBranch = std::any_of(childCounts.cbegin(), childCounts.cend(),
                                          [](int count) { return count > 1; });
    QAction *linearizeAct = menu.addAction(tr("Linearize History"));
    linearizeAct->setToolTip(
        tr("Remove all branches from the history tree, keeping only the linear path "
           "from the root to the current state. All branching states are permanently deleted."));
    linearizeAct->setEnabled(hasAnyBranch);
    menu.addSeparator();
    QAction *genPyAct = menu.addAction(tr("Generate Python Script"));
    genPyAct->setToolTip(tr("Export the current undo path as a runnable Python script"));

    QAction *chosen = menu.exec(event->globalPos());
    if (chosen == restoreAct)
        emit nodeActivated(nodeId, false);
    else if (chosen == restoreCamAct)
        emit nodeActivated(nodeId, true);
    else if (chosen == reopenFilterAct)
        emit nodeReopenFilterRequested(nodeId);
    else if (chosen == updateCamAct)
        emit updateCameraRequested(cameraTargets);
    else if (chosen == makeRootAct)
        emit nodeMakeRootRequested(nodeId);
    else if (chosen == purgeBranchAct)
        emit nodePurgeBranchRequested(nodeId);
    else if (chosen == linearizeAct)
        emit linearizeHistoryRequested();
    else if (chosen == genPyAct)
        emit generatePythonScriptRequested();
}

void UndoGraphWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->pos();
    const int ri = rowAt(pos.y());
    const int member = ri >= 0 ? stripNodeAt(ri, pos) : -1;
    if (ri != m_hoveredRow || member != m_hoveredMember) {
        m_hoveredRow = ri;
        m_hoveredMember = member;
        viewport()->update();
    }
    if (ri >= 0 && onStripToggle(ri, pos))
        viewport()->setCursor(Qt::PointingHandCursor);
    else
        viewport()->unsetCursor();
    // A dot shows its own state; a thumbnail the state its row shows.
    if (member >= 0 || (ri >= 0 && thumbnailRect(ri).contains(pos))) {
        const int nodeId = member >= 0 ? member : m_layout.rows[ri].displayNodeId;
        emit nodeHovered(nodeId, viewport()->mapToGlobal(pos), caption(ri, nodeId));
    } else {
        emit nodeUnhovered();
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void UndoGraphWidget::leaveEvent(QEvent *event)
{
    if (m_hoveredRow >= 0 || m_hoveredMember >= 0) {
        m_hoveredRow = -1;
        m_hoveredMember = -1;
        viewport()->update();
    }
    viewport()->unsetCursor();
    emit nodeUnhovered();
    QAbstractScrollArea::leaveEvent(event);
}

void UndoGraphWidget::resizeEvent(QResizeEvent *event)
{
    updateScrollBars();
    QAbstractScrollArea::resizeEvent(event);
}
