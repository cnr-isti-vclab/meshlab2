#include "rasteraligntool.h"

#include "document.h"
#include "filedialogdirectory.h"
#include "renderwidget.h"

#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QSaveFile>

namespace {
constexpr const char *kAlignKey =
    "meshlab2.filter.camera::align_current_raster_to_mesh_by_point_correspondences";
constexpr const char *kMutualInformationKey =
    "meshlab2.filter.camera::align_current_raster_to_mesh_by_mutual_information";

// The current raster, when it has a camera to align; null otherwise.
const Document::RasterEntry *alignableRaster(const RenderWidget *view)
{
    const Document *doc = view ? view->document() : nullptr;
    const int ri = doc ? doc->currentRasterIndex() : -1;
    if (ri < 0 || ri >= doc->rasterCount() || !doc->raster(ri).shot.isValid())
        return nullptr;
    return &doc->raster(ri);
}
} // namespace

QString RasterAlignTool::id() const
{
    return QStringLiteral("raster_align");
}

QString RasterAlignTool::name() const
{
    return QObject::tr("Raster Alignment Tool");
}

QString RasterAlignTool::statusHint() const
{
    return QObject::tr("Drag from a point of the mesh to where the photo shows it · middle drag pans");
}

QString RasterAlignTool::statusDetail() const
{
    const Document::RasterEntry *raster = alignableRaster(m_view);
    if (!raster)
        return QObject::tr("The current raster needs a camera; set one from a view first");
    if (m_dragging)
        return QObject::tr("Release on the photo, where it shows that point");
    const std::vector<Pair> *pairs = currentPairs();
    const int count = pairs ? int(pairs->size()) : 0;
    if (!m_lastFit.isEmpty())
        return m_lastFit;
    const int needed = m_estimateFocal ? 4 : 3;
    if (count < needed)
        return QObject::tr("%1 of the %2 pairs a fit needs").arg(count).arg(needed);
    return QObject::tr("%n pair(s) · Fit to align the camera", nullptr, count);
}

QString RasterAlignTool::iconPath() const
{
    return QStringLiteral(":/img/icon_raster_align.png");
}

QCursor RasterAlignTool::cursor() const
{
    return QCursor(QPixmap(QStringLiteral(":/img/cur_raster_align.png")), 1, 1);
}

std::vector<ToolControl> RasterAlignTool::controls() const
{
    using Kind = ToolControl::Kind;
    return {
        { QStringLiteral("fit"), Kind::Action, QObject::tr("Fit"),
          QObject::tr("Align the raster's camera to the pairs"), { Qt::Key_F, Qt::Key_Return },
          QStringLiteral("fit") },
        { QStringLiteral("mutual"), Kind::Action, QObject::tr("Mutual Information"),
          QObject::tr("Refine the camera by mutual information with the photo, kept close to the pairs if there are any"),
          { Qt::Key_M }, QStringLiteral("fit") },
        { QStringLiteral("focal"), Kind::Toggle, QObject::tr("Focal Length"),
          QObject::tr("Let the fit change the focal length too; it needs four pairs"), { Qt::Key_L },
          QStringLiteral("fit") },
        { QStringLiteral("undo"), Kind::Action, QObject::tr("Undo"),
          QObject::tr("Take back the last pair"), { Qt::Key_Backspace, Qt::Key_Delete },
          QStringLiteral("edit") },
        { QStringLiteral("clear"), Kind::Action, QObject::tr("Clear"),
          QObject::tr("Remove every pair of this raster"), { Qt::Key_C }, QStringLiteral("edit") },
        { QStringLiteral("save"), Kind::Action, QObject::tr("Save"),
          QObject::tr("Save this raster's pairs as JSON"), { Qt::Key_S }, QStringLiteral("file") },
        { QStringLiteral("load"), Kind::Action, QObject::tr("Load"),
          QObject::tr("Load pairs for this raster from JSON"), { Qt::Key_O }, QStringLiteral("file") },
    };
}

bool RasterAlignTool::isControlChecked(const QString &id) const
{
    return id == QLatin1String("focal") && m_estimateFocal;
}

bool RasterAlignTool::isControlEnabled(const QString &id) const
{
    if (!alignableRaster(m_view))
        return false;
    const std::vector<Pair> *pairs = currentPairs();
    const size_t count = pairs ? pairs->size() : 0;
    if (id == QLatin1String("fit"))
        return count >= (m_estimateFocal ? 4u : 3u);
    if (id == QLatin1String("undo") || id == QLatin1String("clear") || id == QLatin1String("save"))
        return count > 0;
    return true;
}

void RasterAlignTool::triggerControl(const QString &id, Qt::KeyboardModifiers modifiers)
{
    (void)modifiers;
    std::vector<Pair> *pairs = currentPairs();
    if (id == QLatin1String("fit")) {
        fit();
    } else if (id == QLatin1String("mutual")) {
        refineByMutualInformation();
    } else if (id == QLatin1String("focal")) {
        m_estimateFocal = !m_estimateFocal;
    } else if (id == QLatin1String("undo") && pairs && !pairs->empty()) {
        pairs->pop_back();
        m_lastFit.clear();
    } else if (id == QLatin1String("clear") && pairs) {
        pairs->clear();
        m_lastFit.clear();
    } else if (id == QLatin1String("save")) {
        savePairs();
    } else if (id == QLatin1String("load")) {
        loadPairs();
    }
}

void RasterAlignTool::activate(RenderWidget &view)
{
    InteractiveTool::activate(view);
    m_pairs.clear();
    m_lastFit.clear();
    cancelGesture();
    // The pairs are made over the photo: go there, when the current raster can be shown
    if (view.viewMode() != RenderWidget::ViewMode::RasterImage && alignableRaster(&view))
        view.setViewMode(RenderWidget::ViewMode::RasterImage);
}

void RasterAlignTool::deactivate(bool commit)
{
    cancelGesture();
    m_pairs.clear();
    InteractiveTool::deactivate(commit);
}

std::vector<RasterAlignTool::Pair> *RasterAlignTool::currentPairs()
{
    const Document::RasterEntry *raster = alignableRaster(m_view);
    return raster ? &m_pairs[raster->rasterId] : nullptr;
}

const std::vector<RasterAlignTool::Pair> *RasterAlignTool::currentPairs() const
{
    const Document::RasterEntry *raster = alignableRaster(m_view);
    if (!raster)
        return nullptr;
    const auto it = m_pairs.find(raster->rasterId);
    return it == m_pairs.end() ? nullptr : &it->second;
}

bool RasterAlignTool::mousePress(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton)
        return false;   // the middle button pans, as in the raster view without a tool
    if (!alignableRaster(m_view))
        return true;
    m_dragging = true;
    m_released = m_pickReported = m_pickHit = false;
    m_pressPos = m_cursorPos = e->position();
    m_view->requestSurfacePick(e->position().toPoint());
    return true;
}

bool RasterAlignTool::mouseMove(QMouseEvent *e)
{
    if (!m_view || !e)
        return false;
    if (m_dragging && !m_released)
        m_cursorPos = e->position();
    return m_dragging;
}

bool RasterAlignTool::mouseRelease(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton || !m_dragging)
        return false;
    // A click, where the mesh already shows the point where the photo does, is a pair too:
    // it holds that point while a fit corrects the others
    m_released = true;
    m_releaseImage = m_view->widgetToRasterImage(e->position());
    addPairIfComplete();
    return true;
}

void RasterAlignTool::onSurfacePicked(const SurfacePick &result)
{
    if (!m_dragging)
        return;
    m_pickReported = true;
    m_pickHit = result.hit;
    m_pickPoint = result.worldPos;
    if (!m_pickHit) {
        if (m_view && m_view->document())
            m_view->document()->writeLog(QObject::tr("Raster alignment: no mesh under the press, so no pair."),
                                         Document::LogSource::Application);
        cancelGesture();
        return;
    }
    addPairIfComplete();
}

// Once both ends are known: the press's pick and the release on the photo.
void RasterAlignTool::addPairIfComplete()
{
    if (!m_released || !m_pickReported)
        return;
    const Document::RasterEntry *raster = alignableRaster(m_view);
    std::vector<Pair> *pairs = currentPairs();
    if (raster && pairs && m_pickHit) {
        const QSize size = raster->shot.viewportPx();
        pairs->push_back({ m_pickPoint, QPointF(m_releaseImage.x() * size.width(),
                                                m_releaseImage.y() * size.height()) });
        m_lastFit.clear();
    }
    cancelGesture();
}

QByteArray RasterAlignTool::toJson(const std::vector<Pair> &pairs, bool compact)
{
    QJsonArray json;
    for (const Pair &pair : pairs)
        json.append(QJsonObject{
            { QStringLiteral("point"), QJsonArray{ pair.point.x(), pair.point.y(), pair.point.z() } },
            { QStringLiteral("pixel"), QJsonArray{ pair.pixel.x(), pair.pixel.y() } } });
    return QJsonDocument(json).toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented);
}

void RasterAlignTool::cancelGesture()
{
    m_dragging = m_released = m_pickReported = m_pickHit = false;
}

void RasterAlignTool::fit()
{
    std::vector<Pair> *pairs = currentPairs();
    Document *doc = m_view ? m_view->document() : nullptr;
    if (!doc || !pairs)
        return;
    const MeshFilterRunResult result = doc->runFilter(QString::fromLatin1(kAlignKey), {
        { QStringLiteral("correspondences"), QString::fromUtf8(toJson(*pairs, true)) },
        { QStringLiteral("estimateFocal"), m_estimateFocal } });
    m_lastFit = result.success
        ? QObject::tr("Fitted: %1 px root mean square, at most %2 px")
              .arg(result.outputValues.value(QStringLiteral("rmsAfter")).toDouble(), 0, 'f', 2)
              .arg(result.outputValues.value(QStringLiteral("maxAfter")).toDouble(), 0, 'f', 2)
        : QObject::tr("Fit failed: %1").arg(result.errorMessage);
}

void RasterAlignTool::refineByMutualInformation()
{
    std::vector<Pair> *pairs = currentPairs();
    Document *doc = m_view ? m_view->document() : nullptr;
    if (!doc || !pairs)
        return;
    const MeshFilterRunResult result = doc->runFilter(QString::fromLatin1(kMutualInformationKey), {
        { QStringLiteral("correspondences"), QString::fromUtf8(toJson(*pairs, true)) },
        { QStringLiteral("estimateFocal"), m_estimateFocal } });
    if (!result.success) {
        m_lastFit = QObject::tr("Refinement failed: %1").arg(result.errorMessage);
        return;
    }
    m_lastFit = QObject::tr("Refined: mutual information %1 -> %2 bits")
                    .arg(result.outputValues.value(QStringLiteral("informationBefore")).toDouble(), 0, 'f', 3)
                    .arg(result.outputValues.value(QStringLiteral("informationAfter")).toDouble(), 0, 'f', 3);
    if (result.outputValues.contains(QStringLiteral("pairsAfter")))
        m_lastFit += QObject::tr(", pairs %1 px").arg(result.outputValues.value(QStringLiteral("pairsAfter")).toDouble(), 0, 'f', 2);
}

void RasterAlignTool::savePairs()
{
    const Document::RasterEntry *raster = alignableRaster(m_view);
    const std::vector<Pair> *pairs = currentPairs();
    if (!raster || !pairs || pairs->empty())
        return;
    const QString path = QFileDialog::getSaveFileName(
        m_view, QObject::tr("Save Correspondences"),
        FileDialogDirectory::startingPath(QStringLiteral("export"),
                                          QStringLiteral("%1_correspondences.json").arg(raster->name)),
        QObject::tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    FileDialogDirectory::remember(QStringLiteral("export"), path);
    QSaveFile file(path);
    Document *doc = m_view->document();
    if (!file.open(QIODevice::WriteOnly) || file.write(toJson(*pairs, false)) < 0 || !file.commit()) {
        doc->writeLog(QObject::tr("Cannot save correspondences to '%1'.").arg(path),
                      Document::LogSource::Application, Document::LogLevel::Error);
        return;
    }
    doc->writeLog(QObject::tr("Saved %1 correspondences of raster '%2' to '%3'.")
                      .arg(pairs->size()).arg(raster->name, path),
                  Document::LogSource::Application);
}

void RasterAlignTool::loadPairs()
{
    std::vector<Pair> *pairs = currentPairs();
    if (!pairs)
        return;
    const QString path = QFileDialog::getOpenFileName(
        m_view, QObject::tr("Load Correspondences"),
        FileDialogDirectory::startingPath(QStringLiteral("export"), QString()), QObject::tr("JSON (*.json)"));
    if (path.isEmpty())
        return;
    FileDialogDirectory::remember(QStringLiteral("export"), path);
    Document *doc = m_view->document();
    QFile file(path);
    const QJsonDocument json = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()) : QJsonDocument();
    std::vector<Pair> loaded;
    for (const QJsonValue &value : json.array()) {
        const QJsonArray point = value.toObject().value(QStringLiteral("point")).toArray();
        const QJsonArray pixel = value.toObject().value(QStringLiteral("pixel")).toArray();
        if (point.size() != 3 || pixel.size() != 2) {
            loaded.clear();
            break;
        }
        loaded.push_back({ QVector3D(float(point[0].toDouble()), float(point[1].toDouble()), float(point[2].toDouble())),
                           QPointF(pixel[0].toDouble(), pixel[1].toDouble()) });
    }
    if (loaded.empty()) {
        doc->writeLog(QObject::tr("'%1' holds no correspondences: a JSON array of "
                                  "{\"point\": [x, y, z], \"pixel\": [u, v]} is expected.").arg(path),
                      Document::LogSource::Application, Document::LogLevel::Error);
        return;
    }
    *pairs = std::move(loaded);
    m_lastFit.clear();
    doc->writeLog(QObject::tr("Loaded %1 correspondences from '%2'.").arg(pairs->size()).arg(path),
                  Document::LogSource::Application);
}

void RasterAlignTool::paintOverlay(QPainter &painter)
{
    const Document::RasterEntry *raster = alignableRaster(m_view);
    if (!raster || m_view->viewMode() != RenderWidget::ViewMode::RasterImage)
        return;
    const CameraShot &shot = raster->shot;
    const QSizeF size(shot.viewportPx());
    const auto toWidget = [&](const QPointF &pixel) {
        return m_view->rasterImageToWidget(QPointF(pixel.x() / size.width(), pixel.y() / size.height()));
    };
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor photoColor(255, 140, 0);
    const QColor meshColor(80, 220, 255);
    if (const std::vector<Pair> *pairs = currentPairs()) {
        for (const Pair &pair : *pairs) {
            // Where the camera puts the point now (vcg's pixels are y up), and its pixel: the
            // line between them is what a fit has left to correct
            const QVector2D projected = shot.project(pair.point);
            const QPointF onMesh = toWidget(QPointF(projected.x(), size.height() - projected.y()));
            const QPointF onPhoto = toWidget(pair.pixel);
            painter.setPen(QPen(Qt::white, 1.5));
            painter.drawLine(onMesh, onPhoto);
            painter.setPen(QPen(meshColor, 2));
            painter.drawLine(onMesh - QPointF(4, 4), onMesh + QPointF(4, 4));
            painter.drawLine(onMesh - QPointF(4, -4), onMesh + QPointF(4, -4));
            painter.setPen(QPen(Qt::black, 1));
            painter.setBrush(photoColor);
            painter.drawEllipse(onPhoto, 3.5, 3.5);
        }
    }
    if (m_dragging) {
        painter.setPen(QPen(Qt::white, 1.5, Qt::DashLine));
        painter.drawLine(m_pressPos, m_cursorPos);
        painter.setPen(QPen(meshColor, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(m_pressPos, 4, 4);
    }
}
