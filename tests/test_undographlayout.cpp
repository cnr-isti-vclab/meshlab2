// The Action History's layout rules, driven with synthetic trees: which states fold into
// one row, which row the others hang from, which column each row sits in, and what tells
// the folded states apart. UndoGraphLayout is pure, so no widget or document is needed.
#include "undographlayout.h"

#include <QTest>

using namespace UndoGraphLayout;

namespace {

// The tree a DocumentUndoManager would report: node i has parent `parents[i]` (-1 for the
// root) and was made by `actions[i]` (empty for the root), and `current` is the current node.
// Serials are set apart from the ids, as they are once a history has been compacted, so a
// layout that confused the two would show.
QVector<UndoTreeNodeInfo> tree(const QVector<int> &parents, const QStringList &actions, int current)
{
    QSet<int> onPath;
    for (int id = current; id >= 0; id = parents[id])
        onPath.insert(id);
    QVector<UndoTreeNodeInfo> nodes;
    for (int i = 0; i < parents.size(); ++i) {
        UndoTreeNodeInfo n;
        n.nodeId = i;
        n.serial = quint64(1000 + i);
        n.parentId = parents[i];
        n.isCurrent = (i == current);
        n.isOnCurrentPath = onPath.contains(i);
        n.label = actions[i];
        if (!actions[i].isEmpty())
            n.filterKey = QStringLiteral("plugin::") + actions[i];
        nodes.append(n);
    }
    return nodes;
}

int rowIndex(const Layout &layout, int nodeId)
{
    for (int r = 0; r < layout.rows.size(); ++r)
        if (layout.rows[r].nodeId == nodeId)
            return r;
    return -1;
}

const Row &rowFor(const Layout &layout, int nodeId)
{
    static const Row missing;
    const int r = rowIndex(layout, nodeId);
    return r >= 0 ? layout.rows[r] : missing;
}

QVector<int> memberIds(const Row &row)
{
    QVector<int> ids;
    for (const Member &m : row.members)
        ids.append(m.nodeId);
    return ids;
}

MeshFilterParameterDescriptor parameter(const QString &id, const QString &label,
                                        MeshFilterParameterType type)
{
    MeshFilterParameterDescriptor p;
    p.id = id;
    p.label = label;
    p.type = type;
    return p;
}

} // namespace

class UndoGraphLayoutTests : public QObject
{
    Q_OBJECT
private slots:
    void parameterSweepFoldsIntoOneRow();
    void continuedTryKeepsItsRowAndColumn();
    void severalContinuedTriesKeepTheirRows();
    void branchFromAnEarlierStepKeepsTheRunWhole();
    void alternativeStepCarriesOnTheRun();
    void leafAlternativesOnARunAreTries();
    void expandedGroupGetsARowPerState();
    void oneVaryingParameterLabelsEachState();
    void cameraStateIsNotADifference();
};

// Four tries of one filter from the same state, the last one current: one row, holding all
// four, straight above the state they were tried from.
void UndoGraphLayoutTests::parameterSweepFoldsIntoOneRow()
{
    const Layout layout = build(
        tree({-1, 0, 1, 1, 1, 1}, {"", "open", "remesh", "remesh", "remesh", "remesh"}, 5), {});
    QCOMPARE(layout.rows.size(), 3);
    const Row &tries = layout.rows[0];
    QCOMPARE(tries.fold, Fold::Tries);
    QCOMPARE(memberIds(tries), QVector<int>({2, 3, 4, 5}));
    QVERIFY(tries.isCurrent);
    QCOMPARE(tries.displayNodeId, 5);
    QCOMPARE(tries.parentRow, rowIndex(layout, 1));
    QCOMPARE(tries.lane, 0);
    QCOMPARE(layout.laneCount, 1);
}

// Three tries, and the second was continued: the other two fold into its row, and what came
// after it stays in its column instead of the one its creation once opened.
void UndoGraphLayoutTests::continuedTryKeepsItsRowAndColumn()
{
    const Layout layout = build(
        tree({-1, 0, 1, 1, 1, 3}, {"", "open", "remesh", "remesh", "remesh", "smooth"}, 5), {});
    QCOMPARE(layout.rows.size(), 4);
    const Row &tries = rowFor(layout, 3);
    QCOMPARE(memberIds(tries), QVector<int>({2, 3, 4}));
    QVERIFY(!tries.isCurrent);
    QVERIFY(tries.isOnCurrentPath);
    QCOMPARE(tries.displayNodeId, 3);
    const Row &next = rowFor(layout, 5);
    QCOMPARE(next.parentRow, rowIndex(layout, 3));
    QCOMPARE(next.lane, tries.lane);
    QCOMPARE(layout.laneCount, 1);
}

// Two tries were continued: each keeps its own row for its children, and the two that went
// nowhere fold into a row of their own.
void UndoGraphLayoutTests::severalContinuedTriesKeepTheirRows()
{
    const Layout layout = build(tree({-1, 0, 1, 1, 1, 1, 3, 5},
                                     {"", "open", "remesh", "remesh", "remesh", "remesh",
                                      "smooth", "smooth"},
                                     7),
                                {});
    QCOMPARE(layout.rows.size(), 7);
    QCOMPARE(memberIds(rowFor(layout, 4)), QVector<int>({2, 4}));
    QCOMPARE(rowFor(layout, 3).fold, Fold::None);
    QCOMPARE(rowFor(layout, 5).fold, Fold::None);
    QCOMPARE(rowFor(layout, 6).lane, rowFor(layout, 3).lane);
    QCOMPARE(rowFor(layout, 7).lane, rowFor(layout, 5).lane);
    QCOMPARE(layout.laneCount, 3);
}

// Four selections, a smoothing that left from the second, and one that followed the fourth.
// The run stays one row: the branch hangs from the step it left, which is marked, and takes
// a column of its own even though it is the run's only branch, so it cannot pass for what came
// after the fourth step. What did follow the fourth continues the run's column.
void UndoGraphLayoutTests::branchFromAnEarlierStepKeepsTheRunWhole()
{
    const Layout layout = build(tree({-1, 0, 1, 2, 3, 4, 3, 5},
                                     {"", "open", "select", "select", "select", "select",
                                      "smooth", "smooth"},
                                     6),
                                {});
    QCOMPARE(layout.rows.size(), 5);
    const Row &run = rowFor(layout, 5);
    QCOMPARE(run.fold, Fold::Run);
    QCOMPARE(memberIds(run), QVector<int>({2, 3, 4, 5}));
    QVERIFY(run.members[1].branches);
    QVERIFY(!run.members[0].branches);
    QVERIFY(!run.members[3].branches); // what follows the last step is no branch
    // The current state left the run at its second step: that is the deepest on its path.
    QVERIFY(run.members[1].isOnCurrentPath);
    QVERIFY(!run.members[2].isOnCurrentPath);
    QCOMPARE(run.displayNodeId, 3);

    const Row &branch = rowFor(layout, 6);
    QCOMPARE(branch.parentId, 3);
    QCOMPARE(branch.parentRow, rowIndex(layout, 5));
    QCOMPARE(branch.lane, 1);
    QCOMPARE(rowFor(layout, 7).lane, run.lane);
    QCOMPARE(layout.laneCount, 2);
}

// Five selections, then back to the third and a different fourth: the original fourth was
// continued, so it carries on the run, and the new one is a branch off the third -- not a
// pair of tries splitting the run in three rows.
void UndoGraphLayoutTests::alternativeStepCarriesOnTheRun()
{
    const Layout layout = build(tree({-1, 0, 1, 2, 3, 4, 5, 4},
                                     {"", "open", "select", "select", "select", "select",
                                      "select", "select"},
                                     7),
                                {});
    QCOMPARE(layout.rows.size(), 4);
    const Row &run = rowFor(layout, 6);
    QCOMPARE(memberIds(run), QVector<int>({2, 3, 4, 5, 6}));
    QVERIFY(run.members[2].branches);
    QCOMPARE(run.displayNodeId, 4);
    const Row &branch = rowFor(layout, 7);
    QCOMPARE(branch.fold, Fold::None);
    QCOMPARE(branch.parentId, 4);
    QCOMPARE(branch.lane, 1);
}

// Three tries of a second smoothing on top of a first, none of them continued: that is a sweep,
// and stays one, though its parent was made by the same filter.
void UndoGraphLayoutTests::leafAlternativesOnARunAreTries()
{
    const Layout layout = build(
        tree({-1, 0, 1, 2, 2, 2}, {"", "open", "smooth", "smooth", "smooth", "smooth"}, 5), {});
    QCOMPARE(layout.rows.size(), 4);
    const Row &tries = rowFor(layout, 5);
    QCOMPARE(tries.fold, Fold::Tries);
    QCOMPARE(memberIds(tries), QVector<int>({3, 4, 5}));
    QCOMPARE(rowFor(layout, 2).fold, Fold::None);
    QCOMPARE(tries.parentRow, rowIndex(layout, 2));
    QCOMPARE(tries.lane, rowFor(layout, 2).lane);
}

// Expanding the sweep gives every try its row back, in the columns it would have had, and
// leaves the control to fold it on the row the group hangs from.
void UndoGraphLayoutTests::expandedGroupGetsARowPerState()
{
    const Layout layout = build(
        tree({-1, 0, 1, 1, 1, 1}, {"", "open", "remesh", "remesh", "remesh", "remesh"}, 5), {1002});
    QCOMPARE(layout.rows.size(), 6);
    const Row &head = rowFor(layout, 5);
    QVERIFY(head.expanded);
    QCOMPARE(head.groupKey, quint64(1002)); // the first try's serial, not its id
    QCOMPARE(memberIds(head), QVector<int>({2, 3, 4, 5}));
    QVERIFY(rowFor(layout, 4).members.isEmpty());
    QCOMPARE(rowFor(layout, 2).lane, 0);
    QCOMPARE(rowFor(layout, 5).lane, 3);
    QCOMPARE(layout.laneCount, 4);
}

// When one parameter is all that changed, each state is labelled with its value, an option
// by its label. A parameter equal up to int-versus-double does not count as changed.
void UndoGraphLayoutTests::oneVaryingParameterLabelsEachState()
{
    MeshFilterDescriptor descriptor;
    descriptor.parameters.push_back(
        parameter(QStringLiteral("length"), QStringLiteral("Target length"), MeshFilterParameterType::Double));
    descriptor.parameters.push_back(
        parameter(QStringLiteral("iterations"), QStringLiteral("Iterations"), MeshFilterParameterType::Int));
    MeshFilterParameterDescriptor mode =
        parameter(QStringLiteral("mode"), QStringLiteral("Mode"), MeshFilterParameterType::Enum);
    mode.enumOptions = {{QStringLiteral("a"), QStringLiteral("Alpha"), {}},
                        {QStringLiteral("b"), QStringLiteral("Beta"), {}}};
    descriptor.parameters.push_back(mode);

    const auto state = [](double length, const QVariant &iterations, const QString &option) {
        return QVariantMap{{QStringLiteral("length"), length},
                           {QStringLiteral("iterations"), iterations},
                           {QStringLiteral("mode"), option}};
    };
    const Differences one = differences(
        descriptor, {state(0.3, 5, "a"), state(0.5, 5.0, "a"), state(0.8, 5, "a")});
    QCOMPARE(one.variedLabel, QStringLiteral("Target length"));
    QCOMPARE(one.values, QStringList({"0.3", "0.5", "0.8"}));
    QCOMPARE(one.perState.value(1), QStringLiteral("Target length 0.5"));

    const Differences two = differences(
        descriptor, {state(0.3, 5, "a"), state(0.5, 5, "b"), state(0.8, 5, "a")});
    QVERIFY(two.variedLabel.isEmpty());
    QVERIFY(two.values.isEmpty());
    QCOMPARE(two.perState.value(1), QStringLiteral("Target length 0.5, Mode Beta"));
}

// A screen-space tool records the camera it acted from; that changes with every drag and is
// not a choice, so on its own it is no difference at all.
void UndoGraphLayoutTests::cameraStateIsNotADifference()
{
    MeshFilterDescriptor descriptor;
    descriptor.parameters.push_back(
        parameter(QStringLiteral("camera"), QStringLiteral("Camera"), MeshFilterParameterType::CameraState));
    const Differences d = differences(descriptor,
                                      {{{QStringLiteral("camera"), QStringLiteral("{\"a\":1}")}},
                                       {{QStringLiteral("camera"), QStringLiteral("{\"a\":2}")}}});
    QVERIFY(d.variedLabel.isEmpty());
    QVERIFY(d.perState.isEmpty());
}

QTEST_GUILESS_MAIN(UndoGraphLayoutTests)
#include "test_undographlayout.moc"
