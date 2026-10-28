#include "filedialogdirectory.h"
#include "measuretool.h"

#include "document.h"
#include "parameterformbuilder.h"
#include "renderwidget.h"

#include <vcg/space/distance3.h>
#include <vcg/complex/allocate.h>

#include <QFileDialog>
#include <QFileInfo>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QSaveFile>
#include <QTextStream>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr const char *kScaleToLengthKey = "meshlab2.filter.meshing::scale_to_known_length";

// The id of the layer a pick landed on; 0, which names no layer, when it is not one.
std::uint64_t pickedLayerId(const Document &doc, const SurfacePick &pick)
{
    return pick.meshIndex >= 0 && pick.meshIndex < doc.meshCount() ? doc.mesh(pick.meshIndex).meshId : 0;
}
}

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
    return QObject::tr("Click two points · click an endpoint to go on from it, drag it to move it");
}

QString MeasureTool::statusDetail() const
{
    if (m_pendingPoint >= 0)
        return QObject::tr("Click the second point");
    if (m_measurements.empty())
        return {};
    return QObject::tr("%n measurement(s) · click the first point of the next", nullptr,
                       int(m_measurements.size()));
}

std::vector<ToolControl> MeasureTool::controls() const
{
    using Kind = ToolControl::Kind;
    return {
        { QStringLiteral("undo"), Kind::Action, QObject::tr("Undo"),
          QObject::tr("Take back the point just placed, or the last measurement"),
          { Qt::Key_Backspace, Qt::Key_Delete }, QStringLiteral("edit") },
        { QStringLiteral("clear"), Kind::Action, QObject::tr("Clear"),
          QObject::tr("Remove every measurement"), { Qt::Key_C }, QStringLiteral("edit") },
        { QStringLiteral("print"), Kind::Action, QObject::tr("Print"),
          QObject::tr("Write the measurements to the log"), { Qt::Key_P }, QStringLiteral("out") },
        { QStringLiteral("save"), Kind::Action, QObject::tr("Save"),
          QObject::tr("Save the measurements as a table"), { Qt::Key_S }, QStringLiteral("out") },
        { QStringLiteral("export"), Kind::Action, QObject::tr("Export Layer"),
          QObject::tr("Add the measurements to the document as a layer of segments"),
          { Qt::Key_X }, QStringLiteral("out") },
        { QStringLiteral("scale"), Kind::Action, QObject::tr("Scale to Length"),
          QObject::tr("Scale the layer so that the last measurement has the real length you give"),
          { Qt::Key_L }, QStringLiteral("scale") },
    };
}

bool MeasureTool::isControlEnabled(const QString &id) const
{
    if (id == QLatin1String("undo") || id == QLatin1String("clear"))
        return m_pendingPoint >= 0 || !m_measurements.empty();
    return !m_measurements.empty();
}

void MeasureTool::triggerControl(const QString &id, Qt::KeyboardModifiers modifiers)
{
    (void) modifiers;
    if (id == QLatin1String("undo")) {
        if (m_pendingPoint >= 0)
            clearPendingPoint();
        else if (!m_measurements.empty()) {
            m_measurements.pop_back();
            trimUnusedPoints();
        }
    } else if (id == QLatin1String("clear")) {
        resetSession();
    } else if (id == QLatin1String("print")) {
        printMeasurements();
    } else if (id == QLatin1String("save")) {
        saveMeasurements();
    } else if (id == QLatin1String("export")) {
        exportMeasurements();
    } else if (id == QLatin1String("scale")) {
        scaleToKnownLength();
    }
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
    m_pointLayers.clear();
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
            m_pointLayers[std::size_t(m_dragPoint)] = pickedLayerId(*m_view->document(), result);
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
    m_pointLayers.push_back(pickedLayerId(*m_view->document(), result));
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
    int nearest = -1;
    qreal nearestSquared = 9.0 * 9.0;

    // Only referenced points are editable. This also makes points left unused by
    // deleting a segment harmless without a separate compaction data structure.
    for (const Measurement &m : m_measurements) {
        const std::size_t indices[2] = {m.a, m.b};
        for (const std::size_t index : indices) {
            QPointF projected;
            if (!m_view->projectToWidget(m_points[index], projected))
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
                .arg(lengthText(measurementLength(m), 12)),
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

QString MeasureTool::lengthText(double length, int precision) const
{
    QString text = QString::number(length, 'g', precision);
    const Document *doc = m_view ? m_view->document() : nullptr;
    if (doc && doc->unit() != LengthUnit::Unknown)
        text += QLatin1Char(' ') + LengthUnits::symbol(doc->unit());
    return text;
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
        m_pointLayers.pop_back();
    }
}

void MeasureTool::clearPendingPoint()
{
    // A point created by a surface click is unreferenced until its segment is
    // completed, so it is safe to remove. Existing endpoints are merely deselected.
    if (m_pendingPointIsNew && m_pendingPoint == int(m_points.size()) - 1) {
        m_points.pop_back();
        m_pointLayers.pop_back();
    }
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

void MeasureTool::scaleToKnownLength()
{
    Document *doc = m_view ? m_view->document() : nullptr;
    if (!doc || m_measurements.empty())
        return;
    const QString key = QString::fromLatin1(kScaleToLengthKey);
    const std::optional<MeshFilterDescriptor> descriptor = doc->filterDescriptor(key);
    if (!descriptor)
        return;

    // The layer to scale is the one the measurement was taken on; a segment from one layer to
    // another can only mean scaling them all.
    const Measurement m = m_measurements.back();
    const int layer = doc->indexOfMeshId(m_pointLayers[m.a]);
    const bool oneLayer = layer >= 0 && layer == doc->indexOfMeshId(m_pointLayers[m.b]);
    const double measured = measurementLength(m);

    // The filter's own form, less the two points, which are the measurement's.
    QDialog dialog(m_view);
    dialog.setWindowTitle(descriptor->name);
    auto *layout = new QVBoxLayout(&dialog);
    auto *intro = new QLabel(
        oneLayer ? QObject::tr("M%1 measures %2 on '%3'. Give its real length, and the layer is "
                               "scaled to it.")
                       .arg(m_measurements.size() - 1).arg(lengthText(measured, 8), doc->mesh(layer).name)
                 : QObject::tr("M%1 measures %2 from one layer to another. Give its real length, and "
                               "every layer is scaled to it.")
                       .arg(m_measurements.size() - 1).arg(lengthText(measured, 8)),
        &dialog);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *form = new QFormLayout;
    layout->addLayout(form);
    std::vector<MeshFilterParameterDescriptor> shown;
    for (const MeshFilterParameterDescriptor &parameter : descriptor->parameters) {
        if (parameter.id != QLatin1String("pointA") && parameter.id != QLatin1String("pointB"))
            shown.push_back(parameter);
    }
    ParameterFormBuilder builder(form, &dialog);
    builder.setContext({ doc });
    builder.build(shown, { { QStringLiteral("length"), measured },
                           { QStringLiteral("allLayers"), !oneLayer } });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;

    MeshFilterParameterValues values = builder.values();
    values.insert(QStringLiteral("pointA"), m_points[m.a]);
    values.insert(QStringLiteral("pointB"), m_points[m.b]);
    // The filter scales the current layer, so the measured one is made current for the run.
    const int previousCurrent = doc->currentMeshIndex();
    if (oneLayer)
        doc->setCurrentMeshIndex(layer);
    const int scaledLayer = doc->currentMeshIndex();
    const MeshFilterRunResult result = doc->runFilter(key, values);
    if (doc->currentMeshIndex() != previousCurrent)
        doc->setCurrentMeshIndex(previousCurrent);
    if (!result.success) {
        doc->writeLog(QObject::tr("Scale to Known Length failed: %1").arg(result.errorMessage),
                      Document::LogSource::Application, Document::LogLevel::Error);
        return;
    }

    // The points on the layers that were scaled move with them, so the labels show the lengths
    // as they are now: the one just given, first of all.
    const float factor = result.outputValues.value(QStringLiteral("scaleFactor")).toFloat();
    const QVector3D center = result.outputValues.value(QStringLiteral("center")).value<QVector3D>();
    const bool allLayers = values.value(QStringLiteral("allLayers")).toBool();
    for (std::size_t i = 0; i < m_points.size(); ++i) {
        if (allLayers || doc->indexOfMeshId(m_pointLayers[i]) == scaledLayer)
            m_points[i] = center + factor * (m_points[i] - center);
    }
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
                .arg(i).arg(lengthText(measurementLength(m), 12))
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
    // The columns are in the document's unit, which their names say once it is set.
    const QString suffix = doc->unit() == LengthUnit::Unknown
        ? QString() : QLatin1Char('_') + LengthUnits::symbol(doc->unit());
    out << "id";
    for (const char *column : { "length", "ax", "ay", "az", "bx", "by", "bz" })
        out << '\t' << column << suffix;
    out << '\n';
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

void MeasureTool::paintOverlay(QPainter &painter)
{
    if (!m_view)
        return;
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(QColor(85, 170, 255));
    for (std::size_t i = 0; i < m_measurements.size(); ++i) {
        const Measurement &m = m_measurements[i];
        const QVector3D &worldA = m_points[m.a];
        const QVector3D &worldB = m_points[m.b];
        QPointF a, b;
        if (!m_view->projectToWidget(worldA, a)
            || !m_view->projectToWidget(worldB, b))
            continue;
        QPen pen(QColor(85, 170, 255), 2.0);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.drawEllipse(a, 4.0, 4.0);
        painter.drawEllipse(b, 4.0, 4.0);
        const QString label = QStringLiteral("M%1: %2")
            .arg(i).arg(lengthText(measurementLength(m), 8));
        const QRectF textRect = painter.fontMetrics().boundingRect(label).adjusted(-5, -3, 5, 3);
        const QPointF midpoint = (a + b) * 0.5;
        const QRectF placed = textRect.translated(midpoint + QPointF(8, -8) - textRect.topLeft());
        painter.fillRect(placed, QColor(20, 20, 24, 190));
        painter.setPen(Qt::white);
        painter.drawText(placed, Qt::AlignCenter, label);
    }
    if (m_pendingPoint >= 0 && std::size_t(m_pendingPoint) < m_points.size()) {
        QPointF p;
        if (m_view->projectToWidget(m_points[std::size_t(m_pendingPoint)], p)) {
            painter.setBrush(QColor(255, 170, 85));
            painter.setPen(QPen(QColor(255, 220, 160), 2.0));
            painter.drawEllipse(p, 5.0, 5.0);
        }
    }
    if (m_hoverPoint >= 0 && std::size_t(m_hoverPoint) < m_points.size()) {
        QPointF p;
        if (m_view->projectToWidget(m_points[std::size_t(m_hoverPoint)], p)) {
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
