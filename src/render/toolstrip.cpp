#include "toolstrip.h"

#include <QFontMetrics>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>

#include <algorithm>

namespace {

// The strip is drawn over the scene like the view's other overlays, so it has their dark,
// translucent look in either theme rather than the palette's.
const QColor kText(246, 246, 250);
const QColor kSecondaryText(200, 200, 210);
const QColor kAccent(0, 174, 255);
const QColor kMuted(150, 150, 160);

constexpr int kPadding = 6;        // inside the strip's border
constexpr int kGroupGap = 12;      // between groups of buttons on one row
constexpr int kRowGap = 4;
constexpr int kButtonSpacing = 2;

// The key as the platform writes it: "↩", "⌫", "⇥" on macOS, "Return", "Backspace", "Tab" elsewhere.
QString keyName(int key)
{
    return QKeySequence(key).toString(QKeySequence::NativeText);
}

QFont keycapFont(const QFont &base)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() * 0.8);
    return font;
}

} // namespace

const QString ToolStrip::kCameraId = QStringLiteral("__camera");
const QString ToolStrip::kExitId = QStringLiteral("__exit");

ToolStripButton::ToolStripButton(const QString &text, const QString &keyText, QWidget *parent)
    : QAbstractButton(parent), m_keyText(keyText)
{
    setText(text);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover, true);
}

void ToolStripButton::setOn(bool on)
{
    if (m_on == on)
        return;
    m_on = on;
    update();
}

void ToolStripButton::flash()
{
    m_flashing = true;
    update();
    QTimer::singleShot(160, this, [this] {
        m_flashing = false;
        update();
    });
}

QSize ToolStripButton::sizeHint() const
{
    const QFontMetrics fm(font());
    int width = 8 + fm.horizontalAdvance(text()) + 8;
    if (!m_keyText.isEmpty()) {
        const QFontMetrics km(keycapFont(font()));
        width += km.horizontalAdvance(m_keyText) + 8 + 2;
    }
    return { width, fm.height() + 8 };
}

void ToolStripButton::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF frame = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor fill = Qt::transparent;
    if (m_on)
        fill = QColor(kAccent.red(), kAccent.green(), kAccent.blue(), 150);
    if (isEnabled() && (m_flashing || isDown()))
        fill = QColor(255, 255, 255, 80);
    else if (isEnabled() && underMouse() && !m_on)
        fill = QColor(255, 255, 255, 30);
    if (fill.alpha() > 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawRoundedRect(frame, 5, 5);
    }

    QColor text = kText;
    if (!isEnabled())
        text.setAlpha(90);
    const QFontMetrics fm(font());
    const int textWidth = fm.horizontalAdvance(this->text());
    painter.setPen(text);
    painter.drawText(QRect(8, 0, textWidth, height()), Qt::AlignVCenter | Qt::AlignLeft, this->text());

    if (m_keyText.isEmpty())
        return;
    const QFont keyFont = keycapFont(font());
    const QFontMetrics km(keyFont);
    const QRectF cap(8 + textWidth + 6, (height() - km.height() - 2) / 2.0,
                     km.horizontalAdvance(m_keyText) + 8, km.height() + 2);
    QColor capLine = m_on ? QColor(255, 255, 255, 200) : QColor(255, 255, 255, 110);
    if (!isEnabled())
        capLine.setAlpha(45);
    painter.setPen(QPen(capLine, 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(cap, 3, 3);
    painter.setFont(keyFont);
    QColor capText = m_on ? kText : QColor(225, 225, 232);
    if (!isEnabled())
        capText.setAlpha(80);
    painter.setPen(capText);
    painter.drawText(cap, Qt::AlignCenter, m_keyText);
}

ToolStrip::ToolStrip(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setFocusPolicy(Qt::NoFocus);
    // A child of the view inherits its cursor, which over the strip would promise a selection.
    setCursor(Qt::ArrowCursor);

    m_header = new QWidget(this);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(4, 0, 2, 0);
    headerLayout->setSpacing(8);
    m_title = new QLabel(m_header);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    m_status = new QLabel(m_header);
    for (QLabel *label : { m_title, m_status })
        label->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    QPalette palette = m_title->palette();
    palette.setColor(QPalette::WindowText, kText);
    m_title->setPalette(palette);
    palette.setColor(QPalette::WindowText, kSecondaryText);
    m_status->setPalette(palette);
    headerLayout->addWidget(m_title);
    headerLayout->addWidget(m_status);

    ToolControl camera;
    camera.id = kCameraId;
    camera.kind = ToolControl::Kind::Toggle;
    camera.label = tr("Camera");
    camera.help = tr("Hand the mouse to the camera, and back to the tool");
    camera.keys = { Qt::Key_Tab };
    ToolControl exit;
    exit.id = kExitId;
    exit.label = tr("Exit");
    exit.help = tr("Leave the tool, or cancel what it is in the middle of");
    exit.keys = { Qt::Key_Escape };
    m_generic = makeGroup({ camera, exit });
}

ToolStrip::Group ToolStrip::makeGroup(const std::vector<ToolControl> &controls)
{
    Group group;
    group.box = new QWidget(this);
    auto *layout = new QHBoxLayout(group.box);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(kButtonSpacing);
    for (const ToolControl &control : controls) {
        const QString key = control.keys.isEmpty() ? QString() : keyName(control.keys.front());
        auto *button = new ToolStripButton(control.label, key, group.box);
        button->setToolTip(key.isEmpty() ? control.help
                                         : tr("%1 (%2)").arg(control.help, key));
        button->setAccessibleName(control.label);
        connect(button, &QAbstractButton::pressed, this, &ToolStrip::pressed);
        connect(button, &QAbstractButton::clicked, this, [this, id = control.id] {
            emit controlClicked(id, QGuiApplication::keyboardModifiers());
        });
        layout->addWidget(button);
        group.buttons.emplace_back(control.id, button);
    }
    group.box->show();
    return group;
}

void ToolStrip::clearToolGroups()
{
    for (Group &group : m_toolGroups)
        delete group.box;
    m_toolGroups.clear();
}

void ToolStrip::sync(const InteractiveTool &tool, State state)
{
    m_state = state;
    const std::vector<ToolControl> controls = tool.controls();
    QStringList ids;
    for (const ToolControl &control : controls)
        ids << control.id;
    if (ids != m_controlIds) {
        clearToolGroups();
        // Consecutive controls of one group make one box of buttons.
        std::vector<ToolControl> run;
        for (const ToolControl &control : controls) {
            if (!run.empty() && run.back().group != control.group) {
                m_toolGroups.push_back(makeGroup(run));
                run.clear();
            }
            run.push_back(control);
        }
        if (!run.empty())
            m_toolGroups.push_back(makeGroup(run));
        m_controlIds = ids;
    }

    const bool usable = state != State::Unavailable;
    for (const Group &group : m_toolGroups) {
        for (const auto &[id, button] : group.buttons) {
            button->setOn(tool.isControlChecked(id));
            button->setEnabled(usable && tool.isControlEnabled(id));
        }
    }
    ToolStripButton *camera = button(kCameraId);
    camera->setOn(state == State::Camera);
    camera->setEnabled(usable && !tool.gestureInFlight());
    // Esc does what the button says: it cancels a gesture under way before it leaves the tool.
    button(kExitId)->setText(tool.gestureInFlight() ? tr("Cancel") : tr("Exit"));

    m_title->setText(tool.name());
    switch (state) {
    case State::Live: {
        const QString detail = tool.statusDetail();
        m_statusText = detail.isEmpty() ? tool.statusHint() : detail;
        break;
    }
    case State::Camera:
        m_statusText = tr("The mouse moves the camera");
        break;
    case State::NotCurrent:
        m_statusText = tr("Suspended: click this view to resume");
        break;
    case State::Unavailable:
        m_statusText = tr("Not available in this view");
        break;
    }
    m_status->setToolTip(m_statusText);
    update();
}

void ToolStrip::fitWidth(int maxWidth)
{
    maxWidth = std::max(maxWidth, 120);
    // The status text gives way first: it is cut short, whole in its tooltip, rather than
    // pushing the strip past the view.
    const int titleWidth = m_title->sizeHint().width();
    const int statusRoom = std::max(40, maxWidth - 2 * kPadding - titleWidth - 14);
    m_status->setText(m_status->fontMetrics().elidedText(m_statusText, Qt::ElideRight, statusRoom));
    m_header->adjustSize();

    std::vector<QWidget *> boxes{ m_header };
    for (const Group &group : m_toolGroups)
        boxes.push_back(group.box);
    boxes.push_back(m_generic.box);

    // Left to right, a group at a time, starting a new row when the next group does not fit.
    int x = kPadding, y = kPadding, rowHeight = 0, widest = 0;
    std::vector<std::pair<QWidget *, int>> row;   // the boxes of the current row, and their x
    const auto finishRow = [&] {
        for (const auto &[box, left] : row)
            box->move(left, y + (rowHeight - box->height()) / 2);
        row.clear();
    };
    for (QWidget *box : boxes) {
        box->adjustSize();
        const QSize size = box->size();
        if (!row.empty() && x + size.width() + kPadding > maxWidth) {
            finishRow();
            y += rowHeight + kRowGap;
            x = kPadding;
            rowHeight = 0;
        }
        row.emplace_back(box, x);
        rowHeight = std::max(rowHeight, size.height());
        widest = std::max(widest, x + size.width());
        x += size.width() + kGroupGap;
    }
    finishRow();
    setFixedSize(std::min(maxWidth, widest + kPadding), y + rowHeight + kPadding);
}

void ToolStrip::flash(const QString &id)
{
    if (ToolStripButton *b = button(id))
        b->flash();
}

ToolStripButton *ToolStrip::button(const QString &id) const
{
    const auto find = [&id](const Group &group) -> ToolStripButton * {
        for (const auto &[buttonId, b] : group.buttons) {
            if (buttonId == id)
                return b;
        }
        return nullptr;
    };
    if (ToolStripButton *b = find(m_generic))
        return b;
    for (const Group &group : m_toolGroups) {
        if (ToolStripButton *b = find(group))
            return b;
    }
    return nullptr;
}

void ToolStrip::mousePressEvent(QMouseEvent *event)
{
    emit pressed();
    event->accept();
}

void ToolStrip::mouseReleaseEvent(QMouseEvent *event)
{
    event->accept();
}

void ToolStrip::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
}

void ToolStrip::mouseMoveEvent(QMouseEvent *event)
{
    event->accept();
}

void ToolStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor border = m_state == State::Live ? kAccent : kMuted;
    painter.setPen(QPen(QColor(border.red(), border.green(), border.blue(), 220), 1.0));
    painter.setBrush(QColor(20, 20, 24, 200));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}
