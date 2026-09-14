#include "filedialogdirectory.h"
#include "measuretool.h"

#include "document.h"
#include "renderwidget.h"

#include <vcg/space/distance3.h>
#include <vcg/complex/allocate.h>

#include <QFileDialog>
#include <QFileInfo>
#include <QApplication>
#include <QKeyEvent>
#include <QMatrix4x4>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QSaveFile>
#include <QTextStream>
#include <QVector4D>

#include <algorithm>
#include <cmath>

namespace {

bool projectPoint(
    const QVector3D &world,
    const QMatrix4x4 &worldToClip,
    const QSize &viewportSize,
    QPointF &screen)
{
    const QVector4D clip = worldToClip * QVector4D(world, 1.0f);
    if (clip.w() <= 1e-6f)
        return false;
    const QVector3D ndc = clip.toVector3DAffine();
    screen = QPointF(
        (ndc.x() * 0.5f + 0.5f) * viewportSize.width(),
        (1.0f - (ndc.y() * 0.5f + 0.5f)) * viewportSize.height());
    return std::isfinite(screen.x()) && std::isfinite(screen.y());
}

} // namespace

QString MeasureTool::id() const
{
    return QStringLiteral("measure");
}

QString MeasureTool::name() const
{
    return QObject::tr("Measuring Tool");
}

QString MeasureTool::statusHint() const
{
    return QObject::tr(
        "Measure: click two points; click an endpoint to continue, or drag it to edit — C clear, Backspace undo, P print, S save, X export layer");
}

QString MeasureTool::badgeDetail() const
{
    return QObject::tr("%1 — select %2 point")
        .arg(m_measurements.size())
        .arg(m_pendingPoint >= 0 ? QObject::tr("second") : QObject::tr("first"));
}

QString MeasureTool::iconPath() const
{
    return QStringLiteral(":/img/icon_measure.png");
}

QCursor MeasureTool::cursor() const
{
    return QCursor(QPixmap(QStringLiteral(":/img/cur_measure.png")), 15, 15);
}

void MeasureTool::activate(RenderWidget &view)
{
    InteractiveTool::activate(view);
    resetSession();
}

void MeasureTool::resetSession()
{
    m_points.clear();
    m_measurements.clear();
    m_pendingPoint = -1;
    m_pendingPointIsNew = false;
    m_hoverPoint = m_pressedPoint = m_dragPoint = -1;
    m_dragging = false;
    m_hasPreviewPoint = false;
    m_pickAction = PickAction::None;
}

void MeasureTool::deactivate(bool commit)
{
    resetSession();
    InteractiveTool::deactivate(commit);
}

bool MeasureTool::mousePress(QMouseEvent *e)
{
    if (!m_view || !e || e->button() != Qt::LeftButton)
        return false;
    m_pressedPoint = pointAt(e->position());
    m_pressPos = e->position().toPoint();
    m_dragging = false;
    if (m_pressedPoint < 0) {
        m_pickAction = PickAction::AddPoint;
        m_view->requestSurfacePick(m_pressPos);
    }
    return true;
}

bool MeasureTool::mouseMove(QMouseEvent *e)
{
    if (!m_view || !e)
        return false;
    if (!(e->buttons() & Qt::LeftButton)) {
        m_hoverPoint = pointAt(e->position());
        if (m_pendingPoint >= 0) {
            if (m_hoverPoint >= 0) {
                m_previewPoint = m_points[std::size_t(m_hoverPoint)];
                m_hasPreviewPoint = true;
            } else {
                // Asynchronous GPU picking keeps the rubber-band endpoint on
                // the visible mesh surface without doing a CPU intersection.
                m_pickAction = PickAction::PreviewPoint;
                m_view->requestSurfacePick(e->position().toPoint());
            }
        }
        return true; // repaint when the hover highlight appears or disappears
    }
    if (m_pressedPoint < 0)
        return false;

    if (!m_dragging
        && (e->position().toPoint() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
        m_dragging = true;
        m_dragPoint = m_pressedPoint;
    }
    if (m_dragging) {
        // GPU picking constrains every drag sample to the rendered mesh surface.
        m_pickAction = PickAction::DragPoint;
        m_view->requestSurfacePick(e->position().toPoint());
    }
    return true;
}

bool MeasureTool::mouseRelease(QMouseEvent *e)
{
    if (!e || e->button() != Qt::LeftButton || m_pressedPoint < 0)
        return false;
    if (m_dragging) {
        // Request the release position too, since the final move event may have
        // been coalesced by Qt.
        m_pickAction = PickAction::DragPoint;
        m_view->requestSurfacePick(e->position().toPoint());
    } else if (m_pendingPoint >= 0) {
        finishSegment(std::size_t(m_pressedPoint));
    } else {
        // A click on an existing endpoint starts a connected segment.  Because
        // segments share point indices, later dragging keeps the junction joined.
        m_pendingPoint = m_pressedPoint;
        m_pendingPointIsNew = false;
    }
    m_pressedPoint = -1;
    m_dragging = false;
    return true;
}

void MeasureTool::onSurfacePicked(const SurfacePick &result)
{
    if (!m_view)
        return;
    if (!result.hit) {
        if (m_pickAction == PickAction::PreviewPoint)
            m_hasPreviewPoint = false;
        m_pickAction = PickAction::None;
        return;
    }
    if (m_pickAction == PickAction::DragPoint) {
        if (m_dragPoint >= 0 && std::size_t(m_dragPoint) < m_points.size()) {
            m_points[std::size_t(m_dragPoint)] = result.worldPos;
        }
        m_pickAction = PickAction::None;
        return;
    }
    if (m_pickAction != PickAction::AddPoint)
    {
        if (m_pickAction == PickAction::PreviewPoint) {
            m_previewPoint = result.worldPos;
            m_hasPreviewPoint = true;
            m_pickAction = PickAction::None;
        }
        return;
    }
    m_pickAction = PickAction::None;

    m_points.push_back(result.worldPos);
    const std::size_t point = m_points.size() - 1;
    if (m_pendingPoint < 0) {
        m_pendingPoint = int(point);
        m_pendingPointIsNew = true;
        return;
    }
    finishSegment(point);
}

int MeasureTool::pointAt(const QPointF &screenPos) const
{
    if (!m_view)
        return -1;
    const QMatrix4x4 worldToClip = m_view->projectionMatrix() * m_view->viewMatrix();
    const QSize viewportSize = m_view->size();
    int nearest = -1;
    qreal nearestSquared = 9.0 * 9.0;

    // Only referenced points are editable. This also makes points left unused by
    // deleting a segment harmless without a separate compaction data structure.
    for (const Measurement &m : m_measurements) {
        const std::size_t indices[2] = {m.a, m.b};
        for (const std::size_t index : indices) {
            QPointF projected;
            if (!projectPoint(m_points[index], worldToClip, viewportSize, projected))
                continue;
            const QPointF delta = projected - screenPos;
            const qreal squared = delta.x() * delta.x() + delta.y() * delta.y();
            if (squared <= nearestSquared) {
                nearestSquared = squared;
                nearest = int(index);
            }
        }
    }
    return nearest;
}

void MeasureTool::finishSegment(std::size_t point)
{
    if (m_pendingPoint < 0 || std::size_t(m_pendingPoint) >= m_points.size()
        || point >= m_points.size() || std::size_t(m_pendingPoint) == point)
        return;
    const std::size_t first = std::size_t(m_pendingPoint);
    m_measurements.push_back({first, point});
    m_pendingPoint = -1;
    m_pendingPointIsNew = false;
    m_hasPreviewPoint = false;
    if (Document *doc = m_view->document()) {
        const Measurement &m = m_measurements.back();
        doc->writeLog(
            QObject::tr("Distance M%1: %2")
                .arg(m_measurements.size() - 1)
                .arg(measurementLength(m), 0, 'g', 12),
            Document::LogSource::Application);
    }
}

double MeasureTool::measurementLength(const Measurement &measurement) const
{
    const QVector3D &a = m_points[measurement.a];
    const QVector3D &b = m_points[measurement.b];
    return vcg::Distance(
        vcg::Point3d(a.x(), a.y(), a.z()),
        vcg::Point3d(b.x(), b.y(), b.z()));
}

void MeasureTool::trimUnusedPoints()
{
    // Segments are removed in reverse creation order, so any point that becomes
    // unreferenced is at the end. Preserve the shared endpoints still in use.
    while (!m_points.empty()) {
        const std::size_t last = m_points.size() - 1;
        const bool referenced = std::any_of(
            m_measurements.begin(), m_measurements.end(),
            [last](const Measurement &m) { return m.a == last || m.b == last; });
        if (referenced || m_pendingPoint == int(last))
            break;
        m_points.pop_back();
    }
}

void MeasureTool::clearPendingPoint()
{
    // A point created by a surface click is unreferenced until its segment is
    // completed, so it is safe to remove. Existing endpoints are merely deselected.
    if (m_pendingPointIsNew && m_pendingPoint == int(m_points.size()) - 1)
        m_points.pop_back();
    m_pendingPoint = -1;
    m_pendingPointIsNew = false;
    m_hasPreviewPoint = false;
}

void MeasureTool::cancelGesture()
{
    clearPendingPoint();
    m_pressedPoint = m_dragPoint = -1;
    m_hasPreviewPoint = false;
    m_pickAction = PickAction::None;
}

bool MeasureTool::keyPress(QKeyEvent *e)
{
    if (!e)
        return false;
    if (e->key() == Qt::Key_C) {
        resetSession();
        return true;
    }
    if (e->key() == Qt::Key_Backspace || e->key() == Qt::Key_Delete) {
        if (m_pendingPoint >= 0)
            clearPendingPoint();
        else if (!m_measurements.empty()) {
            m_measurements.pop_back();
            trimUnusedPoints();
        }
        return true;
    }
    if (e->key() == Qt::Key_P) {
        printMeasurements();
        return true;
    }
    if (e->key() == Qt::Key_S) {
        saveMeasurements();
        return true;
    }
    if (e->key() == Qt::Key_X) {
        exportMeasurements();
        return true;
    }
    return false;
}

void MeasureTool::exportMeasurements()
{
    if (!m_view || !m_view->document() || m_measurements.empty())
        return;

    // A newly selected first point is not part of a completed measurement yet.
    const std::size_t vertexCount =
        m_points.size() - (m_pendingPointIsNew ? std::size_t(1) : std::size_t(0));
    VCGMesh mesh;
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        const QVector3D &p = m_points[i];
        mesh.vert[i].P() = vcg::Point3f(p.x(), p.y(), p.z());
    }
    vcg::tri::Allocator<VCGMesh>::AddEdges(mesh, m_measurements.size());
    for (std::size_t i = 0; i < m_measurements.size(); ++i) {
        const Measurement &m = m_measurements[i];
        mesh.edge[i].V(0) = &mesh.vert[m.a];
        mesh.edge[i].V(1) = &mesh.vert[m.b];
    }

    Document *doc = m_view->document();
    doc->beginUndoStep(QObject::tr("Export Measurements"));
    doc->addMesh(mesh, QObject::tr("Measurements"), vcg::tri::io::Mask::IOM_EDGEINDEX);
    doc->endUndoStep(true);
}

void MeasureTool::printMeasurements() const
{
    if (!m_view || !m_view->document())
        return;
    Document *doc = m_view->document();
    doc->writeLog(QObject::tr("Measurements: ID, distance, [point A], [point B]"),
                  Document::LogSource::Application);
    for (std::size_t i = 0; i < m_measurements.size(); ++i) {
        const Measurement &m = m_measurements[i];
        const QVector3D &a = m_points[m.a];
        const QVector3D &b = m_points[m.b];
        doc->writeLog(
            QStringLiteral("M%1: %2 [%3, %4, %5] [%6, %7, %8]")
                .arg(i).arg(measurementLength(m), 0, 'g', 12)
                .arg(a.x(), 0, 'g', 12).arg(a.y(), 0, 'g', 12).arg(a.z(), 0, 'g', 12)
                .arg(b.x(), 0, 'g', 12).arg(b.y(), 0, 'g', 12).arg(b.z(), 0, 'g', 12),
            Document::LogSource::Application);
    }
}

void MeasureTool::saveMeasurements() const
{
    if (!m_view || !m_view->document() || m_measurements.empty())
        return;
    Document *doc = m_view->document();
    QString base = QStringLiteral("measurements");
    const int meshIndex = doc->currentMeshIndex();
    if (meshIndex >= 0 && meshIndex < doc->meshCount()) {
        const Document::MeshEntry &entry = doc->mesh(meshIndex);
        base = QFileInfo(entry.sourcePath).completeBaseName();
        if (base.isEmpty())
            base = entry.name;
    }
    const QString path = QFileDialog::getSaveFileName(
        m_view,
        QObject::tr("Save Measurements"),
        FileDialogDirectory::startingPath(
            QStringLiteral("export"),
            QStringLiteral("%1_measurements.tsv").arg(base)),
        QObject::tr("Tab-separated values (*.tsv);;Text files (*.txt)"));
    if (path.isEmpty())
        return;
    FileDialogDirectory::remember(QStringLiteral("export"), path);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        doc->writeLog(QObject::tr("Cannot save measurements to '%1'.").arg(path),
                      Document::LogSource::Application, Document::LogLevel::Error);
        return;
    }
    QTextStream out(&file);
    out.setRealNumberNotation(QTextStream::SmartNotation);
    out.setRealNumberPrecision(17);
    out << "id\tlength\tax\tay\taz\tbx\tby\tbz\n";
    for (std::size_t i = 0; i < m_measurements.size(); ++i) {
        const Measurement &m = m_measurements[i];
        const QVector3D &a = m_points[m.a];
        const QVector3D &b = m_points[m.b];
        out << 'M' << i << '\t' << measurementLength(m)
            << '\t' << a.x() << '\t' << a.y() << '\t' << a.z()
            << '\t' << b.x() << '\t' << b.y() << '\t' << b.z() << '\n';
    }
    if (!file.commit()) {
        doc->writeLog(QObject::tr("Cannot finish writing measurements to '%1'.").arg(path),
                      Document::LogSource::Application, Document::LogLevel::Error);
        return;
    }
    doc->writeLog(QObject::tr("Measurements saved to '%1'.").arg(path),
                  Document::LogSource::Application);
}

void MeasureTool::paintOverlay(
    QPainter &painter,
    const QMatrix4x4 &worldToClip,
    const QSize &viewportSize)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(QColor(85, 170, 255));
    for (std::size_t i = 0; i < m_measurements.size(); ++i) {
        const Measurement &m = m_measurements[i];
        const QVector3D &worldA = m_points[m.a];
        const QVector3D &worldB = m_points[m.b];
        QPointF a, b;
        if (!projectPoint(worldA, worldToClip, viewportSize, a)
            || !projectPoint(worldB, worldToClip, viewportSize, b))
            continue;
        QPen pen(QColor(85, 170, 255), 2.0);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawEllipse(a, 4.0, 4.0);
        painter.drawEllipse(b, 4.0, 4.0);
        const QString label = QStringLiteral("M%1: %2")
            .arg(i).arg(measurementLength(m), 0, 'g', 8);
        const QRectF textRect = painter.fontMetrics().boundingRect(label).adjusted(-5, -3, 5, 3);
        const QPointF midpoint = (a + b) * 0.5;
        const QRectF placed = textRect.translated(midpoint + QPointF(8, -8) - textRect.topLeft());
        painter.fillRect(placed, QColor(20, 20, 24, 190));
        painter.setPen(Qt::white);
        painter.drawText(placed, Qt::AlignCenter, label);
    }
    if (m_pendingPoint >= 0 && std::size_t(m_pendingPoint) < m_points.size()) {
        QPointF p;
        if (projectPoint(m_points[std::size_t(m_pendingPoint)], worldToClip, viewportSize, p)) {
            painter.setBrush(QColor(255, 170, 85));
            painter.setPen(QPen(QColor(255, 220, 160), 2.0));
            painter.drawEllipse(p, 5.0, 5.0);
        }
    }
    if (m_hoverPoint >= 0 && std::size_t(m_hoverPoint) < m_points.size()) {
        QPointF p;
        if (projectPoint(m_points[std::size_t(m_hoverPoint)], worldToClip, viewportSize, p)) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 230, 120), 2.5));
            painter.drawEllipse(p, 8.0, 8.0);

            const QString hint = QObject::tr("Click to continue · Drag to move");
            const QRectF textRect = painter.fontMetrics().boundingRect(hint).adjusted(-5, -3, 5, 3);
            const QRectF placed = textRect.translated(p + QPointF(12, 12) - textRect.topLeft());
            painter.fillRect(placed, QColor(20, 20, 24, 210));
            painter.setPen(QColor(255, 240, 170));
            painter.drawText(placed, Qt::AlignCenter, hint);
        }
    }
}

std::vector<ToolLineSegment> MeasureTool::depthCuedLines() const
{
    std::vector<ToolLineSegment> lines;
    lines.reserve(m_measurements.size() + 1);
    for (const Measurement &m : m_measurements)
        lines.push_back({m_points[m.a], m_points[m.b]});
    if (m_pendingPoint >= 0 && std::size_t(m_pendingPoint) < m_points.size()
        && m_hasPreviewPoint)
        lines.push_back({m_points[std::size_t(m_pendingPoint)], m_previewPoint});
    return lines;
}
