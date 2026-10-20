#include <QtTest/QtTest>

#include "toolstrip.h"

#include <QKeySequence>
#include <QLabel>
#include <QSet>
#include <QSignalSpy>

// The view under the strip, as far as presses go.
class PressCounter final : public QWidget
{
public:
    int presses = 0;

protected:
    void mousePressEvent(QMouseEvent *) override { ++presses; }
};

// A tool reduced to what the strip reads of it.
class FakeTool final : public InteractiveTool
{
public:
    QString id() const override { return QStringLiteral("fake"); }
    QString name() const override { return QStringLiteral("Fake Tool"); }
    QString statusHint() const override { return QStringLiteral("Drag to do things"); }
    QString statusDetail() const override { return detail; }
    std::vector<ToolControl> controls() const override { return list; }
    bool isControlChecked(const QString &id) const override { return checked.contains(id); }
    bool isControlEnabled(const QString &id) const override { return !disabled.contains(id); }
    bool gestureInFlight() const override { return inFlight; }

    std::vector<ToolControl> list;
    QSet<QString> checked;
    QSet<QString> disabled;
    QString detail;
    bool inFlight = false;
};

class ToolStripTests : public QObject
{
    Q_OBJECT

private slots:
    void eachControlIsAButtonWithItsKey();
    void aClickReportsTheControlAndLeavesTheFocusAlone();
    void theGroupsWrapToFitTheView();
    void theTextAndTheGenericButtonsFollowTheState();
    void theButtonsAreMadeAgainOnlyWhenTheControlsChange();
    void aPressBetweenTheButtonsStaysOffTheView();
};

namespace {

FakeTool selectionLikeTool()
{
    using Kind = ToolControl::Kind;
    FakeTool tool;
    tool.list = {
        { QStringLiteral("faces"), Kind::Toggle, QStringLiteral("Faces"), QStringLiteral("Select faces"),
          { Qt::Key_F }, QStringLiteral("element") },
        { QStringLiteral("vertices"), Kind::Toggle, QStringLiteral("Vertices"), QStringLiteral("Select vertices"),
          { Qt::Key_V }, QStringLiteral("element") },
        { QStringLiteral("visible"), Kind::Toggle, QStringLiteral("Visible Only"), QStringLiteral("Only in sight"),
          { Qt::Key_B }, QStringLiteral("options") },
        { QStringLiteral("all"), Kind::Action, QStringLiteral("All"), QStringLiteral("Select all"),
          { Qt::Key_A }, QStringLiteral("edit") },
        { QStringLiteral("dilate"), Kind::Action, QStringLiteral("Dilate"), QStringLiteral("Grow by a ring"),
          { Qt::Key_Plus, Qt::Key_Equal }, QStringLiteral("edit") },
        { QStringLiteral("remove"), Kind::Action, QStringLiteral("Remove"), QStringLiteral("Remove them"),
          { Qt::Key_X }, QStringLiteral("edit") },
    };
    tool.checked = { QStringLiteral("faces") };
    return tool;
}

void saveGrab(QWidget &widget, const QString &name)
{
    const QString dir = qEnvironmentVariable("MESHLAB2_GRAB_DIR");
    if (!dir.isEmpty())
        widget.grab().save(dir + QLatin1Char('/') + name);
}

} // namespace

// A button for each control, showing the first of its keys as the platform writes it and saying
// in its tooltip what it does and by which key; a toggle shows whether it is on, a control the tool
// does not allow now is greyed. The camera and exit buttons come after the tool's own.
void ToolStripTests::eachControlIsAButtonWithItsKey()
{
    FakeTool tool = selectionLikeTool();
    tool.disabled = { QStringLiteral("remove") };
    ToolStrip strip;
    strip.sync(tool, ToolStrip::State::Live);
    strip.fitWidth(1400);

    for (const ToolControl &control : tool.list) {
        ToolStripButton *button = strip.button(control.id);
        QVERIFY2(button, qPrintable(control.id));
        QCOMPARE(button->text(), control.label);
        QCOMPARE(button->keyText(), QKeySequence(control.keys.front()).toString(QKeySequence::NativeText));
        QVERIFY(button->toolTip().contains(control.help));
        QVERIFY(button->toolTip().contains(button->keyText()));
    }
    QVERIFY(strip.button(QStringLiteral("faces"))->isOn());
    QVERIFY(!strip.button(QStringLiteral("vertices"))->isOn());
    QVERIFY(!strip.button(QStringLiteral("remove"))->isEnabled());
    QVERIFY(strip.button(QStringLiteral("all"))->isEnabled());
    QCOMPARE(strip.button(QStringLiteral("dilate"))->keyText(), QStringLiteral("+"));
    QVERIFY(strip.button(ToolStrip::kCameraId));
    QVERIFY(strip.button(ToolStrip::kExitId));

    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));
    saveGrab(strip, QStringLiteral("toolstrip_wide.png"));
}

// Clicking a button says which control, and with the modifiers held; nothing in the strip takes
// the keyboard focus, so the keys keep going to the view.
void ToolStripTests::aClickReportsTheControlAndLeavesTheFocusAlone()
{
    FakeTool tool = selectionLikeTool();
    ToolStrip strip;
    strip.sync(tool, ToolStrip::State::Live);
    strip.fitWidth(1400);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));

    QSignalSpy clicked(&strip, &ToolStrip::controlClicked);
    QTest::mouseClick(strip.button(QStringLiteral("vertices")), Qt::LeftButton);
    QCOMPARE(clicked.count(), 1);
    QCOMPARE(clicked.first().at(0).toString(), QStringLiteral("vertices"));
    QTest::mouseClick(strip.button(ToolStrip::kExitId), Qt::LeftButton);
    QCOMPARE(clicked.last().at(0).toString(), ToolStrip::kExitId);
    // A button reports the click; whether it is on is the tool's to say, at the next sync.
    QVERIFY(!strip.button(QStringLiteral("vertices"))->isOn());

    QCOMPARE(strip.focusPolicy(), Qt::NoFocus);
    for (QWidget *child : strip.findChildren<QWidget *>())
        QCOMPARE(child->focusPolicy(), Qt::NoFocus);
}

// On a wide view the strip is one row; on a narrow one its groups go onto more rows, and every
// button stays inside the strip, which stays inside the width it was given.
void ToolStripTests::theGroupsWrapToFitTheView()
{
    FakeTool tool = selectionLikeTool();
    ToolStrip strip;
    strip.sync(tool, ToolStrip::State::Live);
    strip.show();
    QVERIFY(QTest::qWaitForWindowExposed(&strip));

    strip.fitWidth(1400);
    const int oneRow = strip.height();
    QVERIFY(strip.width() <= 1400);

    strip.fitWidth(320);
    QVERIFY(strip.width() <= 320);
    QVERIFY2(strip.height() > oneRow, qPrintable(QStringLiteral("%1 vs %2").arg(strip.height()).arg(oneRow)));
    for (const ToolControl &control : tool.list) {
        ToolStripButton *button = strip.button(control.id);
        const QRect inStrip(button->mapTo(&strip, QPoint(0, 0)), button->size());
        QVERIFY2(strip.rect().contains(inStrip), qPrintable(control.id));
    }
    saveGrab(strip, QStringLiteral("toolstrip_narrow.png"));
}

// The text says what the mouse does, or what the tool is doing while it works; out of the tool's
// hands, what happened to it. Camera is on while Tab has the mouse; Exit reads Cancel while a
// gesture is under way, as Esc then cancels it; in a view that does not take the tool, nothing
// can be pressed.
void ToolStripTests::theTextAndTheGenericButtonsFollowTheState()
{
    FakeTool tool = selectionLikeTool();
    ToolStrip strip;
    const auto statusText = [&strip] {
        QString text;
        for (QLabel *label : strip.findChildren<QLabel *>())
            text += label->toolTip();
        return text;
    };

    strip.sync(tool, ToolStrip::State::Live);
    QVERIFY(statusText().contains(QStringLiteral("Drag to do things")));
    tool.detail = QStringLiteral("Move · axis X");
    strip.sync(tool, ToolStrip::State::Live);
    QVERIFY(statusText().contains(QStringLiteral("Move · axis X")));
    QCOMPARE(strip.button(ToolStrip::kExitId)->text(), QStringLiteral("Exit"));

    tool.inFlight = true;
    strip.sync(tool, ToolStrip::State::Live);
    QCOMPARE(strip.button(ToolStrip::kExitId)->text(), QStringLiteral("Cancel"));
    QVERIFY(!strip.button(ToolStrip::kCameraId)->isEnabled());
    tool.inFlight = false;

    strip.sync(tool, ToolStrip::State::Camera);
    QVERIFY(strip.button(ToolStrip::kCameraId)->isOn());
    QVERIFY(strip.button(QStringLiteral("all"))->isEnabled());

    strip.sync(tool, ToolStrip::State::Unavailable);
    QVERIFY(!strip.button(QStringLiteral("all"))->isEnabled());
    QVERIFY(!strip.button(ToolStrip::kCameraId)->isEnabled());
    QVERIFY(strip.button(ToolStrip::kExitId)->isEnabled());   // leaving is always possible
}

// The state changes on every key; the buttons are not made again for that, only when the tool
// offers different controls.
void ToolStripTests::theButtonsAreMadeAgainOnlyWhenTheControlsChange()
{
    FakeTool tool = selectionLikeTool();
    ToolStrip strip;
    strip.sync(tool, ToolStrip::State::Live);
    ToolStripButton *faces = strip.button(QStringLiteral("faces"));
    tool.checked = { QStringLiteral("vertices") };
    strip.sync(tool, ToolStrip::State::Live);
    QCOMPARE(strip.button(QStringLiteral("faces")), faces);
    QVERIFY(!faces->isOn());
    QVERIFY(strip.button(QStringLiteral("vertices"))->isOn());

    tool.list.pop_back();
    strip.sync(tool, ToolStrip::State::Live);
    QVERIFY(!strip.button(QStringLiteral("remove")));
    QVERIFY(strip.button(QStringLiteral("faces")));
}

// The strip sits on the view: a press on its background, between the buttons, must not reach the
// view and start a selection there. It is still a press in that view, which the strip reports.
void ToolStripTests::aPressBetweenTheButtonsStaysOffTheView()
{
    FakeTool tool = selectionLikeTool();
    PressCounter view;
    view.resize(1500, 300);
    auto *strip = new ToolStrip(&view);
    strip->sync(tool, ToolStrip::State::Live);
    strip->fitWidth(1400);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    QSignalSpy pressed(strip, &ToolStrip::pressed);
    QTest::mouseClick(strip, Qt::LeftButton, {}, QPoint(2, strip->height() / 2));
    QCOMPARE(view.presses, 0);
    QCOMPARE(pressed.count(), 1);
    QTest::mouseClick(strip->button(QStringLiteral("all")), Qt::LeftButton);
    QCOMPARE(view.presses, 0);
    QCOMPARE(pressed.count(), 2);
}

QTEST_MAIN(ToolStripTests)
#include "test_toolstrip.moc"
