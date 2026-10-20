#pragma once

#include "interactivetool.h"

#include <QAbstractButton>
#include <QWidget>

#include <vector>

class QLabel;

// One button of the tool strip: its label, and the key that does the same drawn as a keycap
// after it. Whether it is on comes from the tool (setOn), not from clicking it, since the tool
// owns that state and a key changes it as well. It never takes the keyboard focus.
class ToolStripButton final : public QAbstractButton
{
public:
    ToolStripButton(const QString &text, const QString &keyText, QWidget *parent);

    void setOn(bool on);
    bool isOn() const { return m_on; }
    QString keyText() const { return m_keyText; }
    // Lights the button for a moment: its key was pressed.
    void flash();

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QString m_keyText;
    bool m_on = false;
    bool m_flashing = false;
};

// The bar along the bottom of the view that owns an interactive tool. It names the tool, says
// what the mouse does (or, while it works, what it is doing), and has a button for each of the
// tool's options and actions (InteractiveTool::controls()) with the key that does the same, then
// the two every tool has: Tab hands the mouse to the camera and back, Esc cancels what is under
// way or leaves the tool. The border is the accent colour while the tool has the mouse and grey
// while it has not, so the strip is also what says that a tool is engaged.
//
// The groups of buttons wrap onto more rows when the view is narrow (fitWidth). The strip never
// takes the keyboard focus, so the keys keep going to the view.
class ToolStrip final : public QWidget
{
    Q_OBJECT
public:
    enum class State {
        Live,          // the tool has the mouse
        Camera,        // Tab gave the mouse to the camera
        NotCurrent,    // another view is current
        Unavailable,   // this view's mode or arrangement does not take the tool
    };

    // The ids of the two controls every tool has.
    static const QString kCameraId;
    static const QString kExitId;

    explicit ToolStrip(QWidget *parent = nullptr);

    // Brings the strip up to date with the tool: the buttons are made again when its controls
    // changed, and otherwise only their state and the text are. Call fitWidth afterwards.
    void sync(const InteractiveTool &tool, State state);
    // Lays the buttons out within `maxWidth` and sizes the strip to them.
    void fitWidth(int maxWidth);
    void flash(const QString &id);
    ToolStripButton *button(const QString &id) const;

signals:
    void controlClicked(const QString &id, Qt::KeyboardModifiers modifiers);
    // A press anywhere on the strip, buttons or not: it is a press in its view.
    void pressed();

protected:
    void paintEvent(QPaintEvent *event) override;
    // The strip keeps the presses, releases and moves on its background: passed on to the view
    // underneath, a click between two buttons would start a selection there. Wheel events go on,
    // so the scene still zooms under the strip.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    struct Group {
        QWidget *box = nullptr;
        std::vector<std::pair<QString, ToolStripButton *>> buttons;
    };
    Group makeGroup(const std::vector<ToolControl> &controls);
    void clearToolGroups();

    QWidget *m_header = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_status = nullptr;
    QString m_statusText;
    std::vector<Group> m_toolGroups;
    Group m_generic;
    QStringList m_controlIds;
    State m_state = State::Live;
};
