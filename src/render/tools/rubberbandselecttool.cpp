#include "rubberbandselecttool.h"

#include "document.h"
#include "renderwidget.h"
#include "selectionshortcuts.h"

#include <vcg/complex/algorithms/update/selection.h>

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
        return QObject::tr("Click the corners, double-click or Enter to close · Backspace takes "
                           "back a corner · Shift add · Ctrl subtract");
    return QObject::tr("Drag to select · Shift add · Ctrl subtract");
}

std::vector<ToolControl> RubberBandSelectTool::controls() const
{
    using Kind = ToolControl::Kind;
    return {
        { QStringLiteral("faces"), Kind::Toggle, QObject::tr("Faces"),
          QObject::tr("Select faces"), { Qt::Key_F }, QStringLiteral("element") },
        { QStringLiteral("vertices"), Kind::Toggle, QObject::tr("Vertices"),
          QObject::tr("Select vertices"), { Qt::Key_V }, QStringLiteral("element") },
        { QStringLiteral("visible"), Kind::Toggle, QObject::tr("Visible Only"),
          QObject::tr("Select only the faces in sight, not those hidden behind others"),
          { Qt::Key_B }, QStringLiteral("options") },
        { QStringLiteral("components"), Kind::Toggle, QObject::tr("Components"),
          QObject::tr("Grow the selection to whole connected components"),
          { Qt::Key_C }, QStringLiteral("options") },
        { QStringLiteral("islands"), Kind::Toggle, QObject::tr("UV Islands"),
          QObject::tr("Grow the selection to whole UV islands"),
          { Qt::Key_T }, QStringLiteral("options") },
        { QStringLiteral("lasso"), Kind::Toggle, QObject::tr("Lasso"),
          QObject::tr("Select inside a polygon clicked out corner by corner, instead of a rectangle"),
          { Qt::Key_P }, QStringLiteral("options") },
        { QStringLiteral("all"), Kind::Action, QObject::tr("All"),
          QObject::tr("Select all"), { Qt::Key_A }, QStringLiteral("edit") },
        { QStringLiteral("none"), Kind::Action, QObject::tr("None"),
          QObject::tr("Select none"), { Qt::Key_N }, QStringLiteral("edit") },
        { QStringLiteral("invert"), Kind::Action, QObject::tr("Invert"),
          QObject::tr("Invert the selection"), { Qt::Key_I }, QStringLiteral("edit") },
        { QStringLiteral("dilate"), Kind::Action, QObject::tr("Dilate"),
          QObject::tr("Grow the selection by one ring"), { Qt::Key_Plus, Qt::Key_Equal },
          QStringLiteral("edit") },
        { QStringLiteral("erode"), Kind::Action, QObject::tr("Erode"),
          QObject::tr("Shrink the selection by one ring"), { Qt::Key_Minus }, QStringLiteral("edit") },
        { QStringLiteral("remove"), Kind::Action, QObject::tr("Remove"),
          QObject::tr("Remove the selected elements from the layer"), { Qt::Key_X },
          QStringLiteral("edit") },
    };
}

bool RubberBandSelectTool::isControlChecked(const QString &id) const
{
    if (id == QLatin1String("faces"))
        return m_selectFaces;
    if (id == QLatin1String("vertices"))
        return !m_selectFaces;
    if (id == QLatin1String("visible"))
        return m_visibleOnly && m_selectFaces;
    if (id == QLatin1String("components"))
        return m_expansion == Expansion::Components;
    if (id == QLatin1String("islands"))
        return m_expansion == Expansion::UvIslands;
    if (id == QLatin1String("lasso"))
        return m_shape == Shape::Polygon;
    return false;
}

bool RubberBandSelectTool::isControlEnabled(const QString &id) const
{
    // Occlusion applies to faces only.
    if (id == QLatin1String("visible"))
        return m_selectFaces;
    // The selection must not change underneath an outline that is still being drawn.
    for (const char *edit : { "all", "none", "invert", "dilate", "erode", "remove" }) {
        if (id == QLatin1String(edit))
            return !gestureInFlight();
    }
    return true;
}

void RubberBandSelectTool::triggerControl(const QString &id, Qt::KeyboardModifiers modifiers)
{
    (void) modifiers;
    Document *doc = m_view ? m_view->document() : nullptr;
    const auto log = [doc](const QString &text) {
        if (doc)
            doc->writeLog(text, Document::LogSource::Application);
    };
    if (id == QLatin1String("faces") || id == QLatin1String("vertices")) {
        m_selectFaces = (id == QLatin1String("faces"));
        log(m_selectFaces ? QObject::tr("Rubber-band: selecting faces")
                          : QObject::tr("Rubber-band: selecting vertices"));
    } else if (id == QLatin1String("visible")) {
        m_visibleOnly = !m_visibleOnly;
        log(m_visibleOnly ? QObject::tr("Rubber-band: visible (non-occluded) faces only")
                          : QObject::tr("Rubber-band: all faces (ignore occlusion)"));
    } else if (id == QLatin1String("components") || id == QLatin1String("islands")) {
        const Expansion pressed =
            (id == QLatin1String("components")) ? Expansion::Components : Expansion::UvIslands;
        m_expansion = (m_expansion == pressed) ? Expansion::None : pressed;
        log(m_expansion == Expansion::Components
                ? QObject::tr("Rubber-band: expand to whole connected components")
            : m_expansion == Expansion::UvIslands
                ? QObject::tr("Rubber-band: expand to whole UV islands")
                : QObject::tr("Rubber-band: select only what the rectangle covers"));
    } else if (id == QLatin1String("lasso")) {
        m_shape = (m_shape == Shape::Polygon) ? Shape::Rectangle : Shape::Polygon;
        endDrag();
        clearPolygon();
        log(m_shape == Shape::Polygon
                ? QObject::tr("Rubber-band: polygon lasso (click the corners, double-click or Enter to close)")
                : QObject::tr("Rubber-band: rectangle"));
    } else {
        for (const ToolControl &control : controls()) {
            if (control.id == id && control.kind == ToolControl::Kind::Action)
                runEditingShortcut(control.keys.front());
        }
    }
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
    if (!doc || rect.width() < 2 || rect.height() < 2)
        return true; // ignore accidental clicks

    // Normalize to [0..1], y up (bottom-left origin) to match the filter's convention, over the
    // rect the scene is drawn in.
    const QPointF low = normalizedScenePoint(rect.bottomLeft());
    const QPointF high = normalizedScenePoint(rect.topRight());
    QVariantMap region;
    region[QStringLiteral("rect_min_x")] = low.x();
    region[QStringLiteral("rect_min_y")] = low.y();
    region[QStringLiteral("rect_max_x")] = high.x();
    region[QStringLiteral("rect_max_y")] = high.y();
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

QPointF RubberBandSelectTool::normalizedScenePoint(const QPoint &widgetPos) const
{
    const QRect scene = m_view->sceneRect();
    return { double(widgetPos.x() - scene.left()) / double(std::max(1, scene.width())),
             1.0 - double(widgetPos.y() - scene.top()) / double(std::max(1, scene.height())) };
}

void RubberBandSelectTool::commitPolygon(Qt::KeyboardModifiers modifiers)
{
    const QVector<QPoint> corners = m_polygon;
    clearPolygon();
    Document *doc = m_view ? m_view->document() : nullptr;
    if (!doc || corners.size() < 3)
        return;
    QJsonArray points;
    for (const QPoint &p : corners) {
        const QPointF n = normalizedScenePoint(p);
        points.append(QJsonArray{ n.x(), n.y() });
    }
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
    const QRect scene = m_view->sceneRect();
    const QString mode = (mods & Qt::ShiftModifier) ? QStringLiteral("add")
        : (mods & Qt::ControlModifier) ? QStringLiteral("subtract")
        : QStringLiteral("replace");

    params[QStringLiteral("aspect")] = double(std::max(1, scene.width())) / double(std::max(1, scene.height()));
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
    // The options and the editing actions are controls: the view runs them from their keys.
    return false;
}

// All, None, Invert, Dilate, Erode, Remove: one runs one selection filter on the kind of
// element the tool is picking (see selectionshortcuts.h), by the key the control lists.
void RubberBandSelectTool::runEditingShortcut(int key)
{
    const std::optional<SelectionShortcut> shortcut = selectionShortcutFor(key, m_selectFaces);
    if (!shortcut)
        return;

    Document *doc = m_view ? m_view->document() : nullptr;
    const int meshIndex = doc ? doc->currentMeshIndex() : -1;
    if (!doc || meshIndex < 0 || meshIndex >= doc->meshCount())
        return;

    // Clearing, growing or shrinking nothing would still leave an empty step in the undo
    // history, so those are skipped when there is nothing of this kind selected.
    const bool changesExisting = key == Qt::Key_N || key == Qt::Key_Plus
        || key == Qt::Key_Equal || key == Qt::Key_Minus;
    if (changesExisting) {
        const VCGMesh &mesh = doc->mesh(meshIndex).mesh;
        const size_t selected = m_selectFaces
            ? vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh)
            : vcg::tri::UpdateSelection<VCGMesh>::VertexCount(mesh);
        if (selected == 0)
            return;
    }

    const MeshFilterRunResult result = doc->runFilter(
        QStringLiteral("meshlab2.filter.select::") + shortcut->filterId, shortcut->params);
    if (!result.success)
        doc->writeLog(QObject::tr("Selection shortcut failed: %1").arg(result.errorMessage),
                      Document::LogSource::Application, Document::LogLevel::Error);
}
