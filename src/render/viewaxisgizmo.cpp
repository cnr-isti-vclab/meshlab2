#include "viewaxisgizmo.h"

#include "preferences.h"

#include <QMouseEvent>
#include <QPainter>

#include <algorithm>

namespace {

constexpr qreal kHandleRadius = 9.5;
constexpr qreal kStemWidth = 2.0;
// Slack before a press becomes an orbit instead of a click, so a click with an
// unsteady hand still snaps.

const QColor kAxisColor[3] = {
    QColor(0xF7, 0x5C, 0x68),  // X
    QColor(0x9E, 0xD1, 0x3B),  // Y
    QColor(0x4B, 0x9B, 0xE8),  // Z
};

const char *const kAxisLabel[3] = {"X", "Y", "Z"};

QVector3D unitAxis(int axis, bool negative)
{
    const float s = negative ? -1.0f : 1.0f;
    switch (axis) {
    case 0: return QVector3D(s, 0.0f, 0.0f);
    case 1: return QVector3D(0.0f, s, 0.0f);
    default: return QVector3D(0.0f, 0.0f, s);
    }
}

} // namespace

ViewAxisGizmo::ViewAxisGizmo(QWidget *parent)
    : QWidget(parent)
{
    const int gizmoSize = Preferences::instance().intValue(QStringLiteral("view.axisGizmoSize"));
    setFixedSize(gizmoSize, gizmoSize);
    setMouseTracking(true);
    setToolTip(tr("Click an axis to view along it"));
}

void ViewAxisGizmo::setOrientation(const QQuaternion &rotation)
{
    if (qFuzzyCompare(m_rotation, rotation))
        return;
    m_rotation = rotation;
    update();
}

qreal ViewAxisGizmo::handleRadius()
{
    return kHandleRadius;
}

std::array<ViewAxisGizmo::Handle, 6> ViewAxisGizmo::projectedHandles() const
{
    const QPointF mid(width() / 2.0, height() / 2.0);
    const qreal reach = qMin(width(), height()) / 2.0 - kHandleRadius - 1.0;

    std::array<Handle, 6> out;
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign = 0; sign < 2; ++sign) {
            const bool negative = (sign == 1);
            // m_rotation maps world space to camera space, where x is right,
            // y is up and the camera looks down -z. Dropping z is therefore an
            // orthographic projection, and z itself is the draw-order depth.
            const QVector3D v = m_rotation.rotatedVector(unitAxis(axis, negative));
            Handle &h = out[axis * 2 + sign];
            h.id = axis * 2 + sign;
            h.axis = axis;
            h.negative = negative;
            h.center = mid + QPointF(v.x() * reach, -v.y() * reach);
            h.depth = v.z();
        }
    }
    return out;
}

int ViewAxisGizmo::handleAt(const QPointF &pos) const
{
    std::array<Handle, 6> h = projectedHandles();
    // Nearest first, so an overlapped handle never steals the click.
    std::sort(h.begin(), h.end(),
              [](const Handle &a, const Handle &b) { return a.depth > b.depth; });
    for (const Handle &handle : h) {
        const QPointF d = pos - handle.center;
        if (QPointF::dotProduct(d, d) <= kHandleRadius * kHandleRadius)
            return handle.id;
    }
    return -1;
}

void ViewAxisGizmo::setHovered(int id)
{
    if (m_hovered == id)
        return;
    m_hovered = id;
    // Unset rather than restore an arrow, so the parent view's tool cursor
    // still applies over the gizmo's empty corners.
    if (id >= 0)
        setCursor(Qt::PointingHandCursor);
    else
        unsetCursor();
    update();
}

void ViewAxisGizmo::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    std::array<Handle, 6> h = projectedHandles();
    // Painter's algorithm: farthest first, so nearer handles cover them.
    std::sort(h.begin(), h.end(),
              [](const Handle &a, const Handle &b) { return a.depth < b.depth; });

    const QPointF mid(width() / 2.0, height() / 2.0);

    QFont f = p.font();
    f.setPixelSize(10);
    f.setBold(true);
    p.setFont(f);

    // One back-to-front pass, stem then disc per handle. Two passes -- every stem, then
    // every disc -- put all six discs on top of all three stems whatever the sort said, so
    // a handle behind an axis still covered it. The stem is drawn at its own handle's
    // depth, which is an approximation near the centre where every stem meets, but it is
    // what makes the near/far relationship read correctly out at the handles where it shows.
    for (const Handle &handle : h) {
        if (!handle.negative) {
            p.setPen(QPen(kAxisColor[handle.axis], kStemWidth, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(mid, handle.center);
        }

        const QColor base = kAxisColor[handle.axis];
        const bool hot = (handle.id == m_hovered);

        if (handle.negative && !hot) {
            // Opaque, not translucent: when an axis points straight at the
            // camera its two handles coincide, and a see-through fill would let
            // the positive disc show through the one in front of it.
            p.setPen(QPen(base, 1.6));
            p.setBrush(base.darker(260));
            p.drawEllipse(handle.center, kHandleRadius - 1.5, kHandleRadius - 1.5);
            continue;
        }

        p.setPen(hot ? QPen(Qt::white, 1.5) : QPen(Qt::NoPen));
        p.setBrush(base);
        p.drawEllipse(handle.center, kHandleRadius, kHandleRadius);

        QString label = QString::fromLatin1(kAxisLabel[handle.axis]);
        if (handle.negative)
            label.prepend(u'-');

        p.setPen(QColor(25, 25, 25));
        p.drawText(QRectF(handle.center.x() - kHandleRadius,
                          handle.center.y() - kHandleRadius,
                          2.0 * kHandleRadius,
                          2.0 * kHandleRadius),
                   Qt::AlignCenter,
                   label);
    }
}

void ViewAxisGizmo::mouseMoveEvent(QMouseEvent *e)
{
    if (!(e->buttons() & Qt::LeftButton)) {
        setHovered(handleAt(e->position()));
        e->ignore();
        return;
    }

    if (!m_dragging && (e->position() - m_pressPos).manhattanLength()
            > Preferences::instance().intValue(QStringLiteral("input.dragThreshold"))) {
        m_dragging = true;
        // The pointer is driving the camera now, not pointing at a handle.
        setHovered(-1);
        // Begin from the press, not from here, so the arcball is anchored where
        // the gesture actually started and the view does not jump.
        emit orbitBegan(m_pressPos);
    }
    if (m_dragging)
        emit orbitMoved(e->position());
    e->accept();
}

void ViewAxisGizmo::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        e->ignore();
        return;
    }
    // The whole rect is captured, empty area included: a drag has to be able to
    // start anywhere in the gizmo, and Qt only routes the rest of the gesture
    // here if this press is accepted.
    m_pressedHandle = handleAt(e->position());
    m_pressPos = e->position();
    m_dragging = false;
    e->accept();
}

void ViewAxisGizmo::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        e->ignore();
        return;
    }

    if (m_dragging) {
        emit orbitEnded();
    } else if (m_pressedHandle >= 0 && handleAt(e->position()) == m_pressedHandle) {
        // A click, and it ended on the handle it started on.
        QVector3D direction = unitAxis(m_pressedHandle / 2, (m_pressedHandle % 2) == 1);

        // Clicking the handle you are already looking down flips to the far side instead of
        // doing nothing, so the same letter walks front/back the way it does in Blender.
        // A handle pointing at the viewer has camera-space z of +1, which is what "already
        // looking down this axis" means here; the threshold is about two and a half
        // degrees, tight enough that a click after orbiting away re-snaps rather than
        // flipping, since that is the other thing a second click could reasonably mean.
        constexpr float kAlreadyFacing = 0.999f;
        if (projectedHandles()[std::size_t(m_pressedHandle)].depth > kAlreadyFacing)
            direction = -direction;

        emit axisPicked(direction);
    }

    m_dragging = false;
    m_pressedHandle = -1;
    setHovered(handleAt(e->position()));
    e->accept();
}

void ViewAxisGizmo::leaveEvent(QEvent *e)
{
    setHovered(-1);
    QWidget::leaveEvent(e);
}
