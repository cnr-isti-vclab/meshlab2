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
#include <QPainterPath>
#include <QPixmap>
#include <QRegion>
#include <QSaveFile>
#include <QtMath>

#include <vcg/space/distance3.h>
#include <vcg/space/triangle3.h>

#include <algorithm>
#include <cmath>
#include <limits>

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

const QColor kPhotoColor(255, 140, 0);
const QColor kMeshColor(80, 220, 255);

// The normal of the face nearest to a world point, in world space: of layer meshIndex, or of
// any layer when it is -1. Null when there is no face. The pick gives a position only, and a
// scan of the faces, once per pick, costs less than an index kept up to date for it.
QVector3D surfaceNormal(const Document &doc, int meshIndex, const QVector3D &world)
{
    float best = std::numeric_limits<float>::max();
    QVector3D normal;
    for (int i = meshIndex < 0 ? 0 : meshIndex; i < (meshIndex < 0 ? doc.meshCount() : meshIndex + 1); ++i) {
        const Document::MeshEntry &entry = doc.mesh(i);
        bool invertible = false;
        const QVector3D local = entry.transform.inverted(&invertible).map(world);
        if (!invertible)
            continue;
        const vcg::Point3f q(local.x(), local.y(), local.z());
        for (const auto &f : entry.mesh.face) {
            if (f.IsD())
                continue;
            const vcg::Triangle3<float> t(f.cP(0), f.cP(1), f.cP(2));
            float distance = 0.0f;
            vcg::Point3f closest;
            vcg::TrianglePointDistance(t, q, distance, closest);
            if (distance < best) {
                best = distance;
                // Of the face as it stands in the world, which the layer's transform may shear
                const auto corner = [&](int k) {
                    return entry.transform.map(QVector3D(f.cP(k)[0], f.cP(k)[1], f.cP(k)[2]));
                };
                normal = QVector3D::crossProduct(corner(1) - corner(0), corner(2) - corner(0));
            }
        }
    }
    return normal.normalized();
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
    return QObject::tr("Drag from a point of the mesh to where the photo shows it · drag an end to move it · "
                       "middle drag pans");
}

QString RasterAlignTool::statusDetail() const
{
    const Document::RasterEntry *raster = alignableRaster(m_view);
    if (!raster)
        return QObject::tr("The current raster needs a camera; set one from a view first");
    if (m_drag == Drag::NewPair)
        return QObject::tr("Release on the photo, where it shows that point");
    if (m_drag == Drag::MovePhoto)
        return QObject::tr("Release where the photo shows the pair's point");
    if (m_drag == Drag::MoveMesh)
        return QObject::tr("Release on the mesh, at the point the pair's pixel shows");
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
    if (m_hover.index >= 0 || m_drag == Drag::MovePhoto || m_drag == Drag::MoveMesh)
        return QCursor(Qt::SizeAllCursor);
    // A cross with "3D→2D" beside it: what a drag does, from a point of the mesh to the photo
    static const QCursor cursor = [] {
        QPixmap pixmap(128, 64);
        pixmap.setDevicePixelRatio(2);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        const QPointF c(9, 9);
        for (const auto &[pen, width] : { std::pair{ QColor(Qt::black), 3.0 }, std::pair{ QColor(Qt::white), 1.2 } }) {
            painter.setPen(QPen(pen, width, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(c + QPointF(-8, 0), c + QPointF(-3, 0));
            painter.drawLine(c + QPointF(3, 0), c + QPointF(8, 0));
            painter.drawLine(c + QPointF(0, -8), c + QPointF(0, -3));
            painter.drawLine(c + QPointF(0, 3), c + QPointF(0, 8));
        }
        QFont font = painter.font();
        font.setPixelSize(10);
        font.setBold(true);
        QPainterPath text;
        text.addText(QPointF(17, 28), font, QStringLiteral("3D\u21922D"));
        painter.strokePath(text, QPen(Qt::black, 2.5));
        painter.fillPath(text, Qt::white);
        painter.end();
        return QCursor(pixmap, 9, 9);
    }();
    return cursor;
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
        { QStringLiteral("delete"), Kind::Action, QObject::tr("Delete"),
          QObject::tr("Remove the pair under the cursor, or the last pair"), { Qt::Key_Backspace, Qt::Key_Delete },
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
    if (id == QLatin1String("delete") || id == QLatin1String("clear") || id == QLatin1String("save"))
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
    } else if (id == QLatin1String("delete") && pairs && !pairs->empty()) {
        if (m_hover.index >= 0 && m_hover.index < int(pairs->size()))
            pairs->erase(pairs->begin() + m_hover.index);
        else
            pairs->pop_back();
        m_hover = Handle();
        m_lastFit.clear();
    } else if (id == QLatin1String("clear") && pairs) {
        pairs->clear();
        m_hover = Handle();
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
    m_hover = Handle();
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

void RasterAlignTool::ends(const Pair &pair, QPointF &onMesh, QPointF &onPhoto) const
{
    const QSizeF size(alignableRaster(m_view)->shot.viewportPx());
    onMesh = toWidget(pair.point);
    onPhoto = m_view->rasterImageToWidget(QPointF(pair.pixel.x() / size.width(), pair.pixel.y() / size.height()));
}

QPointF RasterAlignTool::toWidget(const QVector3D &world) const
{
    const CameraShot &shot = alignableRaster(m_view)->shot;
    const QSizeF size(shot.viewportPx());
    // vcg's pixels are y up
    const QVector2D projected = shot.project(world);
    return m_view->rasterImageToWidget(QPointF(projected.x() / size.width(), 1.0 - projected.y() / size.height()));
}

// Dark under the colour, so the markers read on light and dark photos alike.
void RasterAlignTool::drawPhotoMarker(QPainter &painter, const QPointF &at, bool hovered) const
{
    // The cross stops short of the square, so the two read apart, as a sight
    const double half = hovered ? 12.75 : 9.75;
    const double arm = half - 4.5;
    const QRectF square(at - QPointF(half, half), at + QPointF(half, half));
    painter.setBrush(Qt::NoBrush);
    // The cross thinner than the square, as the hairline it aims with
    for (const auto &[color, width] : { std::pair{ QColor(0, 0, 0, 200), 2.0 },
                                        std::pair{ hovered ? QColor(Qt::white) : kPhotoColor, 0.0 } }) {
        painter.setPen(QPen(color, 1.5 + width));
        painter.drawRect(square);
        painter.setPen(QPen(color, 1.0 + width * 0.75));
        painter.drawLine(at - QPointF(arm, 0), at + QPointF(arm, 0));
        painter.drawLine(at - QPointF(0, arm), at + QPointF(0, arm));
    }
}

// A disc on the surface, projected through the raster's camera, so it shows as an ellipse that
// tilts with the surface, and a short normal from its centre. Its size is fixed on screen:
// the largest radius of the ellipse. Seen from behind, it is drawn faint.
void RasterAlignTool::drawMeshMarker(QPainter &painter, const Pair &pair, const QPointF &at, bool hovered) const
{
    const double radius = hovered ? 12.0 : 9.0;
    const QColor color = hovered ? QColor(Qt::white) : kMeshColor;
    QPolygonF disc;
    QPointF normalTip = at;
    const QVector3D &n = pair.normal;
    if (!n.isNull()) {
        // The screen images of a small step along two tangents and the normal: a linear map
        // from the disc's plane, scaled so that its longest axis is `radius` pixels.
        const CameraShot &shot = alignableRaster(m_view)->shot;
        const float step = 1e-3f * std::max(shot.depth(pair.point), 1e-6f);
        const QVector3D u = QVector3D::crossProduct(n, std::abs(n.x()) < 0.9f ? QVector3D(1, 0, 0)
                                                                              : QVector3D(0, 1, 0)).normalized();
        const QVector3D v = QVector3D::crossProduct(n, u);
        const QPointF du = toWidget(pair.point + step * u) - at;
        const QPointF dv = toWidget(pair.point + step * v) - at;
        // The largest singular value of [du dv]
        const double a = QPointF::dotProduct(du, du), b = QPointF::dotProduct(du, dv), c = QPointF::dotProduct(dv, dv);
        const double largest = std::sqrt(0.5 * (a + c + std::sqrt((a - c) * (a - c) + 4 * b * b)));
        if (largest > 1e-9) {
            const double scale = radius / largest;
            for (int i = 0; i < 32; ++i) {
                const double angle = 2 * M_PI * i / 32;
                disc << at + scale * (std::cos(angle) * du + std::sin(angle) * dv);
            }
            normalTip = at + 1.4 * scale * (toWidget(pair.point + step * n) - at);
        }
    }
    if (disc.isEmpty()) {   // no normal: a disc facing the camera
        for (int i = 0; i < 32; ++i) {
            const double angle = 2 * M_PI * i / 32;
            disc << at + radius * QPointF(std::cos(angle), std::sin(angle));
        }
    }
    const bool behind = !n.isNull()
        && QVector3D::dotProduct(n, alignableRaster(m_view)->shot.directionToCamera(pair.point)) < 0.0f;
    // Each part whole, outline and colour, before the next, so that a later one hides an earlier
    // one where they cross. The rim lies in the surface's plane and the normal wholly on one side
    // of it: towards the camera it is in front of the rim, away from it behind.
    const QColor outline(0, 0, 0, 200);
    const auto drawRim = [&] {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(outline, 3.5));
        painter.drawPolygon(disc);
        painter.setPen(QPen(color, 1.5));
        painter.drawPolygon(disc);
    };
    const auto drawNormal = [&] {   // thinner than the rim
        painter.setPen(QPen(outline, 2.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(at, normalTip);
        painter.setPen(QPen(color, 1.0, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(at, normalTip);
    };
    painter.save();
    painter.setOpacity(painter.opacity() * (behind ? 0.45 : 1.0));
    if (behind) {
        drawNormal();
        drawRim();
    } else {
        drawRim();
        drawNormal();
    }
    painter.restore();
}

// The photo ends first: after a fit the two ends of a pair coincide, and its pixel is what one
// corrects by hand
RasterAlignTool::Handle RasterAlignTool::handleAt(const QPointF &widgetPos) const
{
    const std::vector<Pair> *pairs = currentPairs();
    if (!pairs || !alignableRaster(m_view))
        return {};
    constexpr double kReach = 7.0;
    for (const bool photo : { true, false })
        for (int i = int(pairs->size()) - 1; i >= 0; --i) {
            QPointF onMesh, onPhoto;
            ends((*pairs)[size_t(i)], onMesh, onPhoto);
            const QPointF d = (photo ? onPhoto : onMesh) - widgetPos;
            if (QPointF::dotProduct(d, d) <= kReach * kReach)
                return { i, photo };
        }
    return {};
}

QPointF RasterAlignTool::toPixel(const QPointF &widgetPos) const
{
    const QSize size = alignableRaster(m_view)->shot.viewportPx();
    const QPointF image = m_view->widgetToRasterImage(widgetPos);
    return QPointF(image.x() * size.width(), image.y() * size.height());
}

bool RasterAlignTool::mousePress(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton)
        return false;   // the middle button pans, as in the raster view without a tool
    if (!alignableRaster(m_view))
        return true;
    m_released = m_pickReported = m_pickHit = false;
    m_pressPos = m_cursorPos = e->position();
    m_handle = handleAt(e->position());
    if (m_handle.index >= 0) {
        m_drag = m_handle.photo ? Drag::MovePhoto : Drag::MoveMesh;
        m_before = (*currentPairs())[size_t(m_handle.index)];
        return true;
    }
    m_drag = Drag::NewPair;
    m_view->requestSurfacePick(e->position().toPoint());
    return true;
}

bool RasterAlignTool::mouseMove(QMouseEvent *e)
{
    if (!m_view || !e)
        return false;
    if (m_drag == Drag::None) {
        // Hovering: the handle under the cursor is highlighted, and Delete removes its pair
        const Handle hover = handleAt(e->position());
        m_hover = hover;
        m_hoverPos = e->position();
        return true;   // the loupe follows the pointer
    }
    if (!m_released)
        m_cursorPos = e->position();
    std::vector<Pair> *pairs = currentPairs();
    if (m_drag == Drag::MovePhoto && pairs && m_handle.index < int(pairs->size()))
        (*pairs)[size_t(m_handle.index)].pixel = toPixel(e->position());
    return true;
}

bool RasterAlignTool::mouseRelease(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton || m_drag == Drag::None)
        return false;
    m_released = true;
    switch (m_drag) {
    case Drag::NewPair:
        // A click, where the mesh already shows the point where the photo does, is a pair too:
        // it holds that point while a fit corrects the others
        m_releaseImage = m_view->widgetToRasterImage(e->position());
        addPairIfComplete();
        break;
    case Drag::MovePhoto:
        m_lastFit.clear();
        m_drag = Drag::None;
        break;
    case Drag::MoveMesh:
        // The new point is the surface under the release
        m_view->requestSurfacePick(e->position().toPoint());
        break;
    case Drag::None:
        break;
    }
    return true;
}

void RasterAlignTool::onSurfacePicked(const SurfacePick &result)
{
    if (m_drag == Drag::MoveMesh && m_released) {
        std::vector<Pair> *pairs = currentPairs();
        if (result.hit && pairs && m_handle.index < int(pairs->size())) {
            Pair &pair = (*pairs)[size_t(m_handle.index)];
            pair.point = result.worldPos;
            pair.normal = surfaceNormal(*m_view->document(), result.meshIndex, result.worldPos);
            m_lastFit.clear();
        } else if (m_view && m_view->document()) {
            m_view->document()->writeLog(QObject::tr("Raster alignment: no mesh under the release, so the pair "
                                                     "keeps its point."),
                                         Document::LogSource::Application);
        }
        m_drag = Drag::None;
        return;
    }
    if (m_drag != Drag::NewPair)
        return;
    m_pickReported = true;
    m_pickHit = result.hit;
    m_pickPoint = result.worldPos;
    if (m_pickHit && m_view && m_view->document())
        m_pickNormal = surfaceNormal(*m_view->document(), result.meshIndex, result.worldPos);
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
                                                m_releaseImage.y() * size.height()), m_pickNormal });
        m_lastFit.clear();
    }
    m_drag = Drag::None;
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

// A moved end goes back where it was; a new pair is dropped.
void RasterAlignTool::cancelGesture()
{
    std::vector<Pair> *pairs = m_view ? currentPairs() : nullptr;
    if ((m_drag == Drag::MovePhoto || m_drag == Drag::MoveMesh) && pairs && m_handle.index < int(pairs->size()))
        (*pairs)[size_t(m_handle.index)] = m_before;
    m_drag = Drag::None;
    m_released = m_pickReported = m_pickHit = false;
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
        const QVector3D world(float(point[0].toDouble()), float(point[1].toDouble()), float(point[2].toDouble()));
        // The file has no normals, nor says which layer a point is on: the nearest face of any
        loaded.push_back({ world, QPointF(pixel[0].toDouble(), pixel[1].toDouble()), surfaceNormal(*doc, -1, world) });
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

// The mesh end's loupe first, the photo end's second: while hovering, where a press would pick
// the surface; during a drag, the end being placed under the pointer and the other end of the
// same pair beside it, to compare the two.
std::vector<InteractiveTool::Loupe> RasterAlignTool::loupes() const
{
    if (!m_view || !alignableRaster(m_view))
        return {};
    const std::vector<Pair> *pairs = currentPairs();
    QPointF onMesh, onPhoto;
    if ((m_drag == Drag::MovePhoto || m_drag == Drag::MoveMesh) && pairs && m_handle.index < int(pairs->size()))
        ends((*pairs)[size_t(m_handle.index)], onMesh, onPhoto);
    switch (m_drag) {
    case Drag::None:
        return { { m_hoverPos, false } };
    case Drag::NewPair:
        return { { m_pressPos, false }, { m_cursorPos, true } };
    case Drag::MovePhoto:
        return { { onMesh, false }, { m_cursorPos, true } };
    case Drag::MoveMesh:
        return { { m_cursorPos, false }, { onPhoto, true } };
    }
    return {};
}

void RasterAlignTool::paintOverlay(QPainter &painter)
{
    if (!alignableRaster(m_view) || m_view->viewMode() != RenderWidget::ViewMode::RasterImage)
        return;
    painter.setRenderHint(QPainter::Antialiasing);
    // The loupes are drawn by the view; the markers stay off them, their frames go on them
    const std::vector<QRect> loupeRects = m_view->toolLoupeRects();
    const std::vector<Loupe> loupeList = loupes();
    QRegion outsideLoupes(m_view->rect());
    for (const QRect &rect : loupeRects)
        outsideLoupes -= rect;
    painter.save();
    painter.setClipRegion(outsideLoupes);
    if (const std::vector<Pair> *pairs = currentPairs()) {
        for (int i = 0; i < int(pairs->size()); ++i) {
            // Where the camera puts the point now, and its pixel: the line between them is
            // what a fit has left to correct
            QPointF onMesh, onPhoto;
            ends((*pairs)[size_t(i)], onMesh, onPhoto);
            const bool hovered = m_hover.index == i && m_drag == Drag::None;
            const bool dragged = m_handle.index == i && (m_drag == Drag::MovePhoto || m_drag == Drag::MoveMesh);
            if (dragged && m_drag == Drag::MoveMesh) {
                // The point is chosen where the drag ends
                onMesh = m_cursorPos;
                painter.setPen(QPen(Qt::white, 1.5, Qt::DashLine));
            } else {
                painter.setPen(QPen(Qt::white, hovered ? 2.5 : 1.5));
            }
            // From marker edge to marker edge, so neither shows the line inside it
            const QPointF along = onPhoto - onMesh;
            const double length = std::hypot(along.x(), along.y());
            constexpr double kTrim = 10.0;
            if (length > 2 * kTrim)
                painter.drawLine(onMesh + along * (kTrim / length), onPhoto - along * (kTrim / length));
            drawMeshMarker(painter, (*pairs)[size_t(i)], onMesh, hovered && !m_hover.photo);
            drawPhotoMarker(painter, onPhoto, hovered && m_hover.photo);
        }
    }
    if (m_drag == Drag::NewPair) {
        painter.setPen(QPen(Qt::white, 1.5, Qt::DashLine));
        painter.drawLine(m_pressPos, m_cursorPos);
        painter.setPen(QPen(kMeshColor, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(m_pressPos, 4, 4);
    }
    painter.restore();
    for (size_t i = 0; i < std::min(loupeRects.size(), loupeList.size()); ++i)
        drawLoupeFrame(painter, QRectF(loupeRects[i]), loupeList[i].photo);
}

// A frame in the colour of the end it places, a label, and a sight at the centre: the point the
// loupe magnifies, which is what lands under the pointer. Open in the middle, so the pixel it
// aims at stays visible.
void RasterAlignTool::drawLoupeFrame(QPainter &painter, const QRectF &rect, bool photo) const
{
    const QColor color = photo ? kPhotoColor : kMeshColor;
    const QPointF c = rect.center();
    painter.setBrush(Qt::NoBrush);
    for (const auto &[pen, width] : { std::pair{ QColor(0, 0, 0, 200), 3.0 }, std::pair{ color, 1.0 } }) {
        painter.setPen(QPen(pen, width + 1.0));
        painter.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.setPen(QPen(pen, width));
        for (const QPointF &d : { QPointF(1, 0), QPointF(-1, 0), QPointF(0, 1), QPointF(0, -1) })
            painter.drawLine(c + 5 * d, c + 14 * d);
    }
    QFont font = painter.font();
    font.setPixelSize(11);
    font.setBold(true);
    QPainterPath text;
    text.addText(rect.topLeft() + QPointF(6, 15), font, photo ? QStringLiteral("2D") : QStringLiteral("3D"));
    painter.strokePath(text, QPen(QColor(0, 0, 0, 200), 2.5));
    painter.fillPath(text, color);
}
