#include "rubberbandselecttool.h"

#include "document.h"
#include "renderwidget.h"

#include <QGuiApplication>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPixmap>
#include <QRect>
#include <QRubberBand>
#include <QVariantMap>

#include <algorithm>

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
    return QObject::tr("Rubber-band: drag to select — Shift add, Ctrl subtract, F/V faces/vertices, B visible-only, C connected components; Tab: camera, Esc: exit");
}

QString RubberBandSelectTool::badgeDetail() const
{
    QStringList parts;
    parts << (m_selectFaces ? QObject::tr("faces") : QObject::tr("vertices"));
    if (m_visibleOnly && m_selectFaces)
        parts << QObject::tr("visible-only");
    if (m_connectedComponents)
        parts << QObject::tr("connected components");
    return parts.join(QStringLiteral(" · "));
}

QString RubberBandSelectTool::iconPath() const
{
    return QStringLiteral(":/img/tool_select_rect.png");
}

// How far the badge tucks back under the artwork's corner, so it reads as part of the
// cursor rather than as something floating beside it.
static constexpr int kBadgeOverlap = 6;

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
    const QPixmap base(img);
    if (!m_connectedComponents)
        return QCursor(base, 1, 1);

    // "cc" for connected-component growth. One sprite composited over the six base
    // cursors, rather than six more files: the base already varies by add/subtract and by
    // the eye, and a second badge baked into the artwork would multiply that again.
    //
    // The 32x32 artwork fills its square -- arrow, dashed rectangle, sometimes an eye -- so
    // the badge gets room by growing the canvas instead of squeezing into a corner of it.
    // The added area is transparent and the hotspot stays on the arrow tip. Canvas and
    // placement are derived from the sprite, so retouching it, including at a different
    // size, needs no change here. Drawn at integer coordinates and never scaled, so the
    // pixels land exactly as authored.
    const QPixmap badge(QStringLiteral(":/img/cur_badge_cc.png"));
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
    delete m_band; // safe if null
    m_band = nullptr;
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
}

bool RubberBandSelectTool::mousePress(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton)
        return false;
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
    if (!m_dragging || !e)
        return false;
    m_band->setGeometry(QRect(m_origin, e->position().toPoint()).normalized());
    return true;
}

bool RubberBandSelectTool::mouseRelease(QMouseEvent *e)
{
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
    const double xMin = double(rect.left()) / w;
    const double xMax = double(rect.right()) / w;
    const double yMin = 1.0 - double(rect.bottom()) / h;
    const double yMax = 1.0 - double(rect.top()) / h;

    const Qt::KeyboardModifiers mods = e->modifiers();
    const QString mode = (mods & Qt::ShiftModifier) ? QStringLiteral("add")
        : (mods & Qt::ControlModifier) ? QStringLiteral("subtract")
        : QStringLiteral("replace");

    QVariantMap params;
    params[QStringLiteral("aspect")] = double(w) / double(h);
    params[QStringLiteral("rect_min_x")] = xMin;
    params[QStringLiteral("rect_min_y")] = yMin;
    params[QStringLiteral("rect_max_x")] = xMax;
    params[QStringLiteral("rect_max_y")] = yMax;
    params[QStringLiteral("element")] = m_selectFaces ? QStringLiteral("face") : QStringLiteral("vertex");
    params[QStringLiteral("mode")] = mode;
    params[QStringLiteral("visible_only")] = m_visibleOnly;
    params[QStringLiteral("expand_to_components")] = m_connectedComponents;
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
        doc->runFilter(QStringLiteral("meshlab2.filter.select::select_by_screen_rectangle"), params);
    if (!result.success)
        doc->writeLog(QObject::tr("Rubber-band selection failed: %1").arg(result.errorMessage),
                      Document::LogSource::Application, Document::LogLevel::Error);
    return true;
}

bool RubberBandSelectTool::keyPress(QKeyEvent *e)
{
    if (!e)
        return false;
    // Esc is handled globally by the view (exits the tool); the tool owns F/V and B.
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
    if (e->key() == Qt::Key_C) {
        m_connectedComponents = !m_connectedComponents;
        if (m_view && m_view->document())
            m_view->document()->writeLog(
                m_connectedComponents
                    ? QObject::tr("Rubber-band: expand to whole connected components")
                    : QObject::tr("Rubber-band: select only what the rectangle covers"),
                Document::LogSource::Application);
        return true;
    }
    return false;
}
