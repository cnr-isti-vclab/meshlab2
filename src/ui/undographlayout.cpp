#include "undographlayout.h"

#include <QColor>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QVector3D>

#include <algorithm>

namespace UndoGraphLayout {

namespace {

struct Group {
    Fold fold = Fold::None;
    QVector<int> members; // oldest first
    int rep = -1;         // the state its children hang from
};

} // namespace

Layout build(const QVector<UndoTreeNodeInfo> &nodes, const QSet<quint64> &expandedGroups)
{
    Layout out;
    if (nodes.isEmpty())
        return out;

    // Node ids are handed out in creation order, so sorting by id sorts by age.
    QHash<int, int> at;
    for (int i = 0; i < nodes.size(); ++i)
        at.insert(nodes[i].nodeId, i);
    QHash<int, QVector<int>> children;
    for (const UndoTreeNodeInfo &n : nodes)
        if (n.parentId >= 0 && at.contains(n.parentId))
            children[n.parentId].append(n.nodeId);
    for (QVector<int> &c : children)
        std::sort(c.begin(), c.end());

    const auto info = [&](int id) -> const UndoTreeNodeInfo & { return nodes[at.value(id)]; };
    const auto childCount = [&](int id) { return children.value(id).size(); };
    // A filter is the same action whatever its parameters; anything else is named by its label.
    const auto actionKey = [&](int id) {
        const UndoTreeNodeInfo &n = info(id);
        return n.filterKey.isEmpty() ? QStringLiteral("label:") + n.label : n.filterKey;
    };

    QVector<Group> groups;
    QHash<int, int> groupOf;
    const auto addGroup = [&](Group g) {
        for (int m : g.members)
            groupOf.insert(m, int(groups.size()));
        groups.append(std::move(g));
    };

    // Tries: siblings made by the same action. When their parent was made by that action too
    // and the oldest of them was continued, that one carries on the parent's run instead, and
    // only the others can be tries: undoing two selections and selecting again branches off a
    // run, it is not a sweep. Parents are visited oldest first, so whether a parent is itself a
    // try is settled before its children are looked at.
    QVector<int> idsByAge;
    idsByAge.reserve(nodes.size());
    for (const UndoTreeNodeInfo &n : nodes)
        idsByAge.append(n.nodeId);
    std::sort(idsByAge.begin(), idsByAge.end());
    for (int parent : std::as_const(idsByAge)) {
        QMap<QString, QVector<int>> byAction;
        for (int c : children.value(parent))
            byAction[actionKey(c)].append(c);
        for (auto it = byAction.cbegin(); it != byAction.cend(); ++it) {
            QVector<int> same = it.value();
            if (info(parent).parentId >= 0 && !groupOf.contains(parent)
                && actionKey(parent) == it.key() && childCount(same.front()) > 0)
                same.removeFirst();
            QVector<int> leaves, continued;
            for (int c : same)
                (childCount(c) == 0 ? leaves : continued).append(c);
            Group g;
            g.fold = Fold::Tries;
            if (continued.size() == 1 && !leaves.isEmpty()) {
                g.members = leaves;
                g.members.append(continued.front());
                std::sort(g.members.begin(), g.members.end());
                g.rep = continued.front();
            } else if (leaves.size() >= 2) {
                g.members = leaves;
                g.rep = leaves.back();
            } else {
                continue;
            }
            addGroup(std::move(g));
        }
    }

    // Runs: a chain of one action, each step carried on by its oldest child made by the same
    // action that is not a try. A branch does not end the run: it hangs from the step it left.
    // A tried state is already folded, and the root is never part of a run.
    const auto nextInRun = [&](int id) {
        if (info(id).parentId < 0 || groupOf.contains(id))
            return -1;
        for (int c : children.value(id))
            if (!groupOf.contains(c) && actionKey(c) == actionKey(id))
                return c;
        return -1;
    };
    for (const UndoTreeNodeInfo &n : nodes) {
        if (n.parentId < 0 || groupOf.contains(n.nodeId)
            || (at.contains(n.parentId) && nextInRun(n.parentId) == n.nodeId))
            continue;
        QVector<int> chain{n.nodeId};
        for (int next = nextInRun(n.nodeId); next >= 0; next = nextInRun(next))
            chain.append(next);
        if (chain.size() >= 2)
            addGroup(Group{Fold::Run, chain, chain.back()});
    }

    const auto plainRow = [](const UndoTreeNodeInfo &n) {
        Row r;
        r.nodeId = n.nodeId;
        r.parentId = n.parentId;
        r.displayNodeId = n.nodeId;
        r.isCurrent = n.isCurrent;
        r.isOnCurrentPath = n.isOnCurrentPath;
        r.label = n.label;
        r.filterKey = n.filterKey;
        return r;
    };

    QVector<Row> rows;
    rows.reserve(nodes.size());
    for (const UndoTreeNodeInfo &n : nodes) {
        const int gi = groupOf.value(n.nodeId, -1);
        if (gi < 0) {
            rows.append(plainRow(n));
            continue;
        }
        const Group &g = groups[gi];
        const quint64 groupKey = info(g.members.front()).serial;
        const bool expanded = expandedGroups.contains(groupKey);
        if (n.nodeId != g.rep) {
            if (expanded)
                rows.append(plainRow(n));
            continue;
        }
        Row r = plainRow(n);
        r.fold = g.fold;
        r.expanded = expanded;
        r.groupKey = groupKey;
        for (int id : g.members) {
            const UndoTreeNodeInfo &m = info(id);
            r.members.append(Member{id, m.isCurrent, m.isOnCurrentPath});
        }
        if (!expanded) {
            // The row hangs where its first state does, and shows the current state if it
            // holds it, else the deepest one on the path to it.
            r.parentId = info(g.members.front()).parentId;
            for (const Member &m : r.members) {
                r.isCurrent = r.isCurrent || m.isCurrent;
                r.isOnCurrentPath = r.isOnCurrentPath || m.isOnCurrentPath;
                if (m.isOnCurrentPath)
                    r.displayNodeId = m.nodeId;
            }
            for (const Member &m : r.members)
                if (m.isCurrent)
                    r.displayNodeId = m.nodeId;
        }
        rows.append(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.nodeId > b.nodeId; });

    QHash<int, int> rowOf;
    for (int r = 0; r < rows.size(); ++r) {
        rowOf.insert(rows[r].nodeId, r);
        if (!rows[r].expanded)
            for (const Member &m : rows[r].members)
                rowOf.insert(m.nodeId, r);
    }
    for (Row &r : rows)
        r.parentRow = r.parentId >= 0 ? rowOf.value(r.parentId, -1) : -1;
    // A row that leaves a folded row from one of its earlier states hangs from that state, which
    // is marked so the strip can show where it left.
    for (int r = 0; r < rows.size(); ++r) {
        const int parentRow = rows[r].parentRow;
        const int parentId = rows[r].parentId;
        if (parentRow < 0 || parentId == rows[parentRow].nodeId)
            continue;
        for (Member &m : rows[parentRow].members)
            if (m.nodeId == parentId)
                m.branches = true;
    }

    // Columns, by the rule the history has always followed: a row's oldest child continues
    // its column, and each later one opens a new column, numbered in the order they were
    // made. A row that leaves a folded row from an earlier state always opens one, so it cannot
    // pass for what came after the last state. A folded row is as old as its first state -- a
    // run as its first step, though it is drawn where its last one is. Visiting rows in that
    // order also reaches every parent before its children, so a parent has its column already.
    const auto firstState = [](const Row &row) {
        return row.members.isEmpty() || row.expanded ? row.nodeId : row.members.front().nodeId;
    };
    QVector<int> byAge(rows.size());
    for (int r = 0; r < rows.size(); ++r)
        byAge[r] = r;
    std::sort(byAge.begin(), byAge.end(),
              [&](int a, int b) { return firstState(rows[a]) < firstState(rows[b]); });
    QSet<int> continuedRows;
    int nextLane = 1;
    for (int r : std::as_const(byAge)) {
        Row &row = rows[r];
        if (row.parentRow < 0) {
            row.lane = 0;
        } else if (row.parentId == rows[row.parentRow].nodeId
                   && !continuedRows.contains(row.parentRow)) {
            continuedRows.insert(row.parentRow);
            row.lane = rows[row.parentRow].lane;
        } else {
            row.lane = nextLane++;
        }
    }
    out.laneCount = nextLane;
    out.rows = std::move(rows);
    return out;
}

Differences differences(const MeshFilterDescriptor &descriptor,
                        const QVector<QVariantMap> &parameters)
{
    Differences out;
    if (parameters.size() < 2)
        return out;
    QVector<const MeshFilterParameterDescriptor *> varied;
    for (const MeshFilterParameterDescriptor &p : descriptor.parameters) {
        if (p.type == MeshFilterParameterType::CameraState
            || p.type == MeshFilterParameterType::RenderState)
            continue;
        const QVariant first = parameters.front().value(p.id);
        const bool differs = std::any_of(parameters.cbegin() + 1, parameters.cend(),
                                         [&](const QVariantMap &state) {
                                             return !sameParameterValue(state.value(p.id), first);
                                         });
        if (differs)
            varied.append(&p);
    }
    if (varied.isEmpty())
        return out;
    if (varied.size() == 1) {
        out.variedLabel = varied.front()->label;
        for (const QVariantMap &state : parameters)
            out.values.append(formatValue(*varied.front(), state.value(varied.front()->id)));
    }
    if (varied.size() <= kMaxListed) {
        for (const QVariantMap &state : parameters) {
            QStringList parts;
            for (const MeshFilterParameterDescriptor *p : std::as_const(varied))
                parts.append(p->label + QLatin1Char(' ') + formatValue(*p, state.value(p->id)));
            out.perState.append(parts.join(QStringLiteral(", ")));
        }
    }
    return out;
}

QString formatValue(const MeshFilterParameterDescriptor &parameter, const QVariant &value)
{
    switch (parameter.type) {
    case MeshFilterParameterType::Bool:
        return value.toBool() ? QObject::tr("on") : QObject::tr("off");
    case MeshFilterParameterType::Int:
    case MeshFilterParameterType::Mesh:
        return QString::number(value.toInt());
    case MeshFilterParameterType::Double:
    case MeshFilterParameterType::AbsPerc:
        return QString::number(value.toDouble(), 'g', 4);
    case MeshFilterParameterType::Enum:
        for (const MeshFilterEnumOption &option : parameter.enumOptions)
            if (option.id == value.toString())
                return option.label;
        break;
    case MeshFilterParameterType::FileOpen:
    case MeshFilterParameterType::FileSave:
        return QFileInfo(value.toString()).fileName();
    case MeshFilterParameterType::Point3f:
        if (value.userType() == QMetaType::QVector3D) {
            const QVector3D v = value.value<QVector3D>();
            return QStringLiteral("%1, %2, %3")
                .arg(double(v.x()), 0, 'g', 3)
                .arg(double(v.y()), 0, 'g', 3)
                .arg(double(v.z()), 0, 'g', 3);
        }
        break;
    case MeshFilterParameterType::Color:
        if (value.userType() == QMetaType::QColor)
            return value.value<QColor>().name();
        break;
    default:
        break;
    }
    return value.toString();
}

} // namespace UndoGraphLayout
