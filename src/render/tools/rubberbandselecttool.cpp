#include "rubberbandselecttool.h"

#include "document.h"
#include "renderwidget.h"

#include <QGuiApplication>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QImage>
#include <QPixmap>
#include <QPolygonF>
#include <QRect>
#include <QRubberBand>
#include <QJsonArray>
#include <QJsonDocument>
#include <QVariantMap>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

// Draws the polygon being clicked out: a translucent fill, the outline, the corners, and the
// edge from the last corner to the pointer. A child of the view that takes no mouse events, so
// it works in every view mode, like the rubber band.
class LassoOverlay final : public QWidget
{
public:
    explicit LassoOverlay(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setGeometry(parent->rect());
    }

    const QVector<QPoint> *polygon = nullptr;
    const QPoint *hover = nullptr;

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (!polygon || polygon->isEmpty())
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPolygonF closed;
        for (const QPoint &p : *polygon)
            closed << QPointF(p);
        if (hover)
            closed << QPointF(*hover);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(80, 160, 255, 40));
        painter.drawPolygon(closed);
        QPen pen(QColor(255, 255, 255, 230), 1.5, Qt::DashLine);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(closed);
        painter.setPen(QPen(QColor(80, 160, 255), 1.0));
        painter.setBrush(QColor(255, 255, 255));
        for (int i = 0; i < polygon->size(); ++i)
            painter.drawEllipse(QPointF((*polygon)[i]), i == 0 ? 4.5 : 3.0, i == 0 ? 4.5 : 3.0);
    }
};

// How close, in pixels, a click has to be to the first corner to close the polygon, and to the
// last one to count as the second press of a double click.
constexpr int kCloseDistance = 8;
constexpr int kRepeatDistance = 3;

}

QString RubberBandSelectTool::id() const
{
    return QStringLiteral("rubberband_select");
}

QString RubberBandSelectTool::name() const
{
    return QObject::tr("Rubber-band Select");
}

QString RubberBandSelectTool::statusHint() const
{
    if (m_shape == Shape::Polygon)
        return QObject::tr("Polygon lasso: click the corners, double-click or Enter to close — Backspace last corner, Esc drop it, Shift add, Ctrl subtract, F/V faces/vertices, B visible-only, C connected components, T UV islands, P rectangle; Tab: camera, Esc: exit");
    return QObject::tr("Rubber-band: drag to select — Shift add, Ctrl subtract, F/V faces/vertices, B visible-only, C connected components, T UV islands, P polygon lasso; Tab: camera, Esc: exit");
}

QString RubberBandSelectTool::badgeDetail() const
{
    QStringList parts;
    parts << (m_shape == Shape::Polygon ? QObject::tr("polygon") : QObject::tr("rectangle"));
    parts << (m_selectFaces ? QObject::tr("faces") : QObject::tr("vertices"));
    if (m_visibleOnly && m_selectFaces)
        parts << QObject::tr("visible-only");
    if (m_expansion == Expansion::Components)
        parts << QObject::tr("connected components");
    else if (m_expansion == Expansion::UvIslands)
        parts << QObject::tr("UV islands");
    return parts.join(QStringLiteral(" · "));
}

QString RubberBandSelectTool::iconPath() const
{
    return QStringLiteral(":/img/tool_select_rect.png");
}

// How far the badge tucks back under the artwork's corner, so it reads as part of the
// cursor rather than as something floating beside it.
static constexpr int kBadgeOverlap = 6;

// The cursor artwork shows the rectangle the tool draws, dotted black and white one pixel at a
// time, in the corner of the arrow. For the polygon lasso the same dotted style outlines a small
// irregular polygon in its place. The rectangle is found in the sprite rather than assumed -- the
// eye variants carry it a few pixels further right -- so retouching the artwork needs no change
// here: it is the box spanned by the sprite's opaque pixels on the first and last rows, from the
// 16th down, that have a long run of them.
static QPixmap polygonizeCursor(const QPixmap &sprite)
{
    QImage image = sprite.toImage().convertToFormat(QImage::Format_ARGB32);
    constexpr int kMinRow = 16, kMinColumn = 12, kLongRun = 8;
    const auto opaqueRun = [&](int y, int &first, int &last) {
        int count = 0;
        first = image.width();
        last = -1;
        for (int x = kMinColumn; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) > 128) {
                ++count;
                first = std::min(first, x);
                last = std::max(last, x);
            }
        }
        return count;
    };
    int top = -1, bottom = -1, left = 0, right = 0;
    for (int y = kMinRow; y < image.height(); ++y) {
        int first, last;
        if (opaqueRun(y, first, last) >= kLongRun) {
            if (top < 0) {
                top = y;
                left = first;
                right = last;
            }
            bottom = y;
        }
    }
    if (top < 0 || bottom - top < 4 || right - left < 4)
        return sprite;   // not the artwork this expects: leave it alone

    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x)
            image.setPixel(x, y, 0);

    // Corners of an irregular polygon, as fractions of the box the rectangle filled.
    // Five corners with a notch: few enough that the straight edges and the corners read at this
    // size, and not so regular that it passes for an ellipse.
    static const QPointF kCorners[] = {
        {0.00, 0.85}, {0.15, 0.00}, {0.55, 0.40}, {1.00, 0.05}, {0.90, 1.00}
    };
    const int count = int(sizeof(kCorners) / sizeof(kCorners[0]));
    std::vector<QPoint> points;
    for (int i = 0; i < count; ++i) {
        points.emplace_back(left + int(std::lround(kCorners[i].x() * (right - left))),
                            top + int(std::lround(kCorners[i].y() * (bottom - top))));
    }
    // Each edge as a run of pixels (Bresenham), black and white by turns along the whole outline.
    int parity = 0;
    const auto plot = [&](int x, int y) {
        image.setPixel(x, y, (parity++ & 1) ? qRgba(255, 255, 255, 255) : qRgba(0, 0, 0, 255));
    };
    for (int i = 0; i < count; ++i) {
        QPoint a = points[std::size_t(i)];
        const QPoint b = points[std::size_t((i + 1) % count)];
        const int dx = std::abs(b.x() - a.x()), dy = -std::abs(b.y() - a.y());
        const int sx = a.x() < b.x() ? 1 : -1, sy = a.y() < b.y() ? 1 : -1;
        int err = dx + dy;
        while (a != b) {   // the corner itself is the next edge's first pixel
            plot(a.x(), a.y());
            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; a.setX(a.x() + sx); }
            if (e2 <= dx) { err += dx; a.setY(a.y() + sy); }
        }
    }
    return QPixmap::fromImage(image);
}

QCursor RubberBandSelectTool::cursor() const
{
    // Reflect the composition modifier: Shift = add (+), Ctrl = subtract (−).
    // The eye variant marks visible-only mode (occlusion applies to faces only).
    const Qt::KeyboardModifiers mods = QGuiApplication::queryKeyboardModifiers();
    const bool eye = m_visibleOnly && m_selectFaces;
    const QString img = (mods & Qt::ShiftModifier)
        ? (eye ? QStringLiteral(":/img/cur_sel_rect_plus_eye.png")
               : QStringLiteral(":/img/cur_sel_rect_plus.png"))
        : (mods & Qt::ControlModifier)
            ? (eye ? QStringLiteral(":/img/cur_sel_rect_minus_eye.png")
                   : QStringLiteral(":/img/cur_sel_rect_minus.png"))
            : (eye ? QStringLiteral(":/img/cur_sel_rect_eye.png")
                   : QStringLiteral(":/img/cur_sel_rect.png"));
    const QPixmap base = (m_shape == Shape::Polygon)
        ? polygonizeCursor(QPixmap(img))
        : QPixmap(img);
    if (m_expansion == Expansion::None)
        return QCursor(base, 1, 1);

    // "cc" for growth to connected components, "uv" to UV islands. One sprite per kind
    // composited over the six base cursors, rather than twelve more files: the base
    // already varies by add/subtract and by the eye, and a badge baked into the artwork
    // would multiply that again.
    //
    // The 32x32 artwork fills its square -- arrow, dashed rectangle, sometimes an eye -- so
    // the badge gets room by growing the canvas instead of squeezing into a corner of it.
    // The added area is transparent and the hotspot stays on the arrow tip. Canvas and
    // placement are derived from the sprite, so retouching it, including at a different
    // size, needs no change here. Drawn at integer coordinates and never scaled, so the
    // pixels land exactly as authored.
    const QPixmap badge(m_expansion == Expansion::Components
                            ? QStringLiteral(":/img/cur_badge_cc.png")
                            : QStringLiteral(":/img/cur_badge_uv.png"));
    QPixmap pixmap(base.width() + badge.width() - kBadgeOverlap,
                   base.height() + badge.height() - kBadgeOverlap);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.drawPixmap(0, 0, base);
    painter.drawPixmap(pixmap.width() - badge.width(), pixmap.height() - badge.height(), badge);
    painter.end();
    return QCursor(pixmap, 1, 1);
}

void RubberBandSelectTool::deactivate(bool commit)
{
    endDrag();
    clearPolygon();
    delete m_band; // safe if null
    m_band = nullptr;
    delete m_lassoOverlay;
    m_lassoOverlay = nullptr;
    InteractiveTool::deactivate(commit);
}

void RubberBandSelectTool::endDrag()
{
    m_dragging = false;
    if (m_band)
        m_band->hide();
}

void RubberBandSelectTool::cancelGesture()
{
    endDrag();
    clearPolygon();
}

void RubberBandSelectTool::clearPolygon()
{
    m_polygon.clear();
    m_swallowRelease = false;
    updateLassoOverlay();
}

void RubberBandSelectTool::updateLassoOverlay()
{
    if (!m_view)
        return;
    if (m_polygon.isEmpty()) {
        if (m_lassoOverlay)
            m_lassoOverlay->hide();
        return;
    }
    if (!m_lassoOverlay) {
        auto *overlay = new LassoOverlay(m_view);
        overlay->polygon = &m_polygon;
        overlay->hover = &m_hover;
        m_lassoOverlay = overlay;
    }
    m_lassoOverlay->setGeometry(m_view->rect());
    m_lassoOverlay->show();
    m_lassoOverlay->raise();
    m_lassoOverlay->update();
}

bool RubberBandSelectTool::mousePress(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton)
        return false;
    if (m_shape == Shape::Polygon) {
        const QPoint p = e->position().toPoint();
        m_swallowRelease = true;
        // A click on the first corner closes the polygon, as does the second press of a double
        // click, which lands on the corner just added.
        if (m_polygon.size() >= 3
            && (p - m_polygon.first()).manhattanLength() <= kCloseDistance) {
            commitPolygon(e->modifiers());
            return true;
        }
        if (m_polygon.isEmpty()
            || (p - m_polygon.last()).manhattanLength() > kRepeatDistance)
            m_polygon.push_back(p);
        m_hover = p;
        updateLassoOverlay();
        return true;
    }
    m_origin = e->position().toPoint();
    if (!m_band)
        m_band = new QRubberBand(QRubberBand::Rectangle, m_view);
    m_band->setGeometry(QRect(m_origin, QSize()));
    m_band->show();
    m_dragging = true;
    return true;
}

bool RubberBandSelectTool::mouseMove(QMouseEvent *e)
{
    if (m_shape == Shape::Polygon && !m_polygon.isEmpty() && e) {
        m_hover = e->position().toPoint();
        updateLassoOverlay();
        return true;
    }
    if (!m_dragging || !e)
        return false;
    m_band->setGeometry(QRect(m_origin, e->position().toPoint()).normalized());
    return true;
}

bool RubberBandSelectTool::mouseRelease(QMouseEvent *e)
{
    if (m_shape == Shape::Polygon && e && e->button() == Qt::LeftButton && m_swallowRelease) {
        m_swallowRelease = false;   // the click that added a corner, or closed the polygon
        return true;
    }
    if (!m_dragging || !e || e->button() != Qt::LeftButton)
        return false;

    const QRect rect = QRect(m_origin, e->position().toPoint()).normalized();
    endDrag(); // hide the band before committing so the change signal is a no-op here

    Document *doc = m_view->document();
    const int w = std::max(1, m_view->width());
    const int h = std::max(1, m_view->height());
    if (!doc || rect.width() < 2 || rect.height() < 2)
        return true; // ignore accidental clicks

    // Normalize to [0..1], y up (bottom-left origin) to match the filter's convention.
    QVariantMap region;
    region[QStringLiteral("rect_min_x")] = double(rect.left()) / w;
    region[QStringLiteral("rect_min_y")] = 1.0 - double(rect.bottom()) / h;
    region[QStringLiteral("rect_max_x")] = double(rect.right()) / w;
    region[QStringLiteral("rect_max_y")] = 1.0 - double(rect.top()) / h;
    commitSelection(QStringLiteral("select_by_screen_rectangle"), region, e->modifiers());
    return true;
}

bool RubberBandSelectTool::mouseDoubleClick(QMouseEvent *e)
{
    // The polygon's closing gesture. The first press of the double click added the corner it
    // lands on, so there is nothing left to add.
    if (m_shape != Shape::Polygon || !e || e->button() != Qt::LeftButton)
        return false;
    m_swallowRelease = true;   // the release that follows belongs to this gesture too
    if (m_polygon.size() >= 3)
        commitPolygon(e->modifiers());
    return true;
}

void RubberBandSelectTool::commitPolygon(Qt::KeyboardModifiers modifiers)
{
    const QVector<QPoint> corners = m_polygon;
    clearPolygon();
    Document *doc = m_view ? m_view->document() : nullptr;
    if (!doc || corners.size() < 3)
        return;
    const int w = std::max(1, m_view->width());
    const int h = std::max(1, m_view->height());
    QJsonArray points;
    for (const QPoint &p : corners)
        points.append(QJsonArray{double(p.x()) / w, 1.0 - double(p.y()) / h});
    QVariantMap region;
    region[QStringLiteral("polygon")] =
        QString::fromUtf8(QJsonDocument(points).toJson(QJsonDocument::Compact));
    commitSelection(QStringLiteral("select_by_screen_polygon"), region, modifiers);
}

void RubberBandSelectTool::commitSelection(const QString &filterId, QVariantMap params,
                                           Qt::KeyboardModifiers mods)
{
    Document *doc = m_view->document();
    if (!doc)
        return;
    const int w = std::max(1, m_view->width());
    const int h = std::max(1, m_view->height());
    const QString mode = (mods & Qt::ShiftModifier) ? QStringLiteral("add")
        : (mods & Qt::ControlModifier) ? QStringLiteral("subtract")
        : QStringLiteral("replace");

    params[QStringLiteral("aspect")] = double(w) / double(h);
    params[QStringLiteral("element")] = m_selectFaces ? QStringLiteral("face") : QStringLiteral("vertex");
    params[QStringLiteral("mode")] = mode;
    params[QStringLiteral("visible_only")] = m_visibleOnly;
    params[QStringLiteral("expand_to")] =
        m_expansion == Expansion::Components ? QStringLiteral("connected_components")
        : m_expansion == Expansion::UvIslands ? QStringLiteral("uv_islands")
                                              : QStringLiteral("none");
    // Provide the parameters for BOTH spaces (the unused ones are ignored by the
    // filter). camera_state has no default, so it must always be present or the
    // filter's parameter validation rejects the call.
    params[QStringLiteral("space")] =
        (m_view->viewMode() == RenderWidget::ViewMode::ParametrizationUV)
            ? QStringLiteral("uv")
            : QStringLiteral("view3d");
    params[QStringLiteral("camera_state")] = m_view->cameraStateJson();
    params[QStringLiteral("uv_pan_x")] = double(m_view->uvPan().x());
    params[QStringLiteral("uv_pan_y")] = double(m_view->uvPan().y());
    params[QStringLiteral("uv_zoom")] = double(m_view->uvZoom());

    const MeshFilterRunResult result =
        doc->runFilter(QStringLiteral("meshlab2.filter.select::") + filterId, params);
    if (!result.success)
        doc->writeLog(QObject::tr("Rubber-band selection failed: %1").arg(result.errorMessage),
                      Document::LogSource::Application, Document::LogLevel::Error);
}

bool RubberBandSelectTool::keyPress(QKeyEvent *e)
{
    if (!e)
        return false;
    // A polygon being clicked out has its own keys. Esc reaches the tool first while one is open
    // (gestureInFlight), and drops it rather than leaving the tool; Tab is held back so the
    // camera does not take the mouse in the middle of an outline.
    if (!m_polygon.isEmpty()) {
        if (e->key() == Qt::Key_Escape) {
            clearPolygon();
            return true;
        }
        if (e->key() == Qt::Key_Tab)
            return true;
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (m_polygon.size() >= 3)
                commitPolygon(e->modifiers());
            return true;
        }
        if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) {
            m_polygon.removeLast();
            updateLassoOverlay();
            return true;
        }
    }
    if (e->key() == Qt::Key_P) {
        m_shape = (m_shape == Shape::Polygon) ? Shape::Rectangle : Shape::Polygon;
        endDrag();
        clearPolygon();
        if (m_view && m_view->document())
            m_view->document()->writeLog(
                m_shape == Shape::Polygon
                    ? QObject::tr("Rubber-band: polygon lasso (click the corners, double-click or Enter to close)")
                    : QObject::tr("Rubber-band: rectangle"),
                Document::LogSource::Application);
        return true;
    }
    // Esc is handled globally by the view (exits the tool); the tool owns F/V, B, C and T.
    if (e->key() == Qt::Key_F || e->key() == Qt::Key_V) {
        m_selectFaces = (e->key() == Qt::Key_F);
        if (m_view && m_view->document())
            m_view->document()->writeLog(
                m_selectFaces ? QObject::tr("Rubber-band: selecting faces")
                              : QObject::tr("Rubber-band: selecting vertices"),
                Document::LogSource::Application);
        return true;
    }
    if (e->key() == Qt::Key_B) {
        m_visibleOnly = !m_visibleOnly;
        if (m_view && m_view->document())
            m_view->document()->writeLog(
                m_visibleOnly ? QObject::tr("Rubber-band: visible (non-occluded) faces only")
                              : QObject::tr("Rubber-band: all faces (ignore occlusion)"),
                Document::LogSource::Application);
        return true;
    }
    if (e->key() == Qt::Key_C || e->key() == Qt::Key_T) {
        const Expansion pressed =
            (e->key() == Qt::Key_C) ? Expansion::Components : Expansion::UvIslands;
        m_expansion = (m_expansion == pressed) ? Expansion::None : pressed;
        if (m_view && m_view->document())
            m_view->document()->writeLog(
                m_expansion == Expansion::Components
                    ? QObject::tr("Rubber-band: expand to whole connected components")
                : m_expansion == Expansion::UvIslands
                    ? QObject::tr("Rubber-band: expand to whole UV islands")
                    : QObject::tr("Rubber-band: select only what the rectangle covers"),
                Document::LogSource::Application);
        return true;
    }
    return false;
}
