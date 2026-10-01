#pragma once

#include "document_undo_types.h"
#include "meshfilterplugin.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

// How the Action History lays out the undo tree: which states share a row, which column
// each row sits in, and what tells the states of a row apart. Pure, so the rules can be
// tested without a widget; UndoGraphWidget paints the result.
//
// Two kinds of repeat fold into one row, drawn as a strip of dots, one per state:
//   - Tries: siblings made by the same action -- the parameter sweep left by undoing a
//     filter and running it again. Only tries that went nowhere fold. A try that was
//     continued keeps a row for its children to hang from; when exactly one was, the
//     others fold into its row. A run's own continuation is never a try.
//   - Runs: a chain of the same action, as an interactive tool leaves it, each step carried
//     on by its oldest child made by that action. A branch does not end the run: the row
//     that leaves it hangs from the step it left, and that step is marked.
// A folded group can be expanded back into one row per state.
namespace UndoGraphLayout {

enum class Fold { None, Tries, Run };

struct Member {
    int nodeId = -1;
    bool isCurrent = false;
    bool isOnCurrentPath = false;
    bool branches = false; // a row leaves the folded row from this state
};

struct Row {
    int nodeId = -1;        // the row's own state, which what follows it hangs from: a run's
                            // last step. A row leaving from an earlier state names that one in
                            // its parentId.
    int parentId = -1;      // the state the row hangs from
    int parentRow = -1;     // the row holding parentId; -1 for the root
    int displayNodeId = -1; // the state the row shows, and restores when activated
    int lane = 0;
    bool isCurrent = false;       // the current state is on this row
    bool isOnCurrentPath = false; // a state on this row lies on the path root -> current
    QString label;
    QString filterKey;
    // A folded row's states, oldest first. When the group is expanded its states get rows
    // of their own, and the one its children hang from keeps the list, with expanded set,
    // to offer folding it back.
    Fold fold = Fold::None;
    QVector<Member> members;
    bool expanded = false;
    // The serial of the group's first state, by which its expansion is remembered: unlike a
    // node id, it survives the history being compacted.
    quint64 groupKey = 0;
};

struct Layout {
    QVector<Row> rows; // newest first, the order they are drawn from the top
    int laneCount = 1;
};

// `expandedGroups` holds the keys of the groups to show one row per state.
Layout build(const QVector<UndoTreeNodeInfo> &nodes, const QSet<quint64> &expandedGroups);

// What tells a group's states apart, from the parameters each was made with, in member
// order. Camera and render states are left out: they say where a screen-space tool acted
// from, not what was chosen.
struct Differences {
    QString variedLabel;  // the one parameter that differs, when exactly one does
    QStringList values;   // its value for each state; empty unless variedLabel is set
    QStringList perState; // "Label value, ..." for each state; empty when nothing differs,
                          // or more than kMaxListed parameters do
};
inline constexpr int kMaxListed = 3;
Differences differences(const MeshFilterDescriptor &descriptor,
                        const QVector<QVariantMap> &parameters);

// A parameter value as a label shows it: an option's label, on/off, a file's name.
QString formatValue(const MeshFilterParameterDescriptor &parameter, const QVariant &value);

} // namespace UndoGraphLayout
