#include "selectlayertool.h"

#include "document.h"
#include "renderwidget.h"

#include <QMouseEvent>
#include <QPixmap>

QString SelectLayerTool::id() const
{
    return QStringLiteral("select_layer");
}

QString SelectLayerTool::name() const
{
    return QObject::tr("Select Layer");
}

QString SelectLayerTool::statusHint() const
{
    return QObject::tr("Click a layer to make it current");
}

QString SelectLayerTool::iconPath() const
{
    return QStringLiteral(":/img/tool_select_layer.png");
}

QCursor SelectLayerTool::cursor() const
{
    return QCursor(QPixmap(QStringLiteral(":/img/cur_pick.png")), 1, 1);
}

bool SelectLayerTool::mousePress(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton)
        return false;
    // Schedule the GPU pick; the result arrives in onSurfacePicked(). Consume the
    // click so it doesn't also start a camera rotation.
    m_view->requestSurfacePick(e->position().toPoint());
    return true;
}

void SelectLayerTool::onSurfacePicked(const SurfacePick &result)
{
    if (!m_view)
        return;
    Document *doc = m_view->document();
    if (!doc)
        return;

    // Guard against a pick that resolved to a now-stale index (e.g. a layer was
    // added/removed between the click and the async result).
    if (!result.hit || result.meshIndex < 0 || result.meshIndex >= doc->meshCount()) {
        doc->writeLog(QObject::tr("Select Layer: no layer under cursor"),
                      Document::LogSource::Application);
        return;
    }

    const QVector3D &p = result.worldPos;
    const QString name = doc->mesh(result.meshIndex).name;
    doc->writeLog(
        QObject::tr("Select Layer: layer %1 '%2' at (%3, %4, %5)")
            .arg(result.meshIndex)
            .arg(name)
            .arg(p.x(), 0, 'g', 4).arg(p.y(), 0, 'g', 4).arg(p.z(), 0, 'g', 4),
        Document::LogSource::Application);
    doc->setCurrentMeshIndex(result.meshIndex);
}
