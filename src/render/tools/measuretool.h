#pragma once

#include "interactivetool.h"

#include <QVector3D>
#include <QPoint>

#include <vector>

class MeasureTool final : public InteractiveTool
{
public:
    QString id() const override;
    QString name() const override;
    QString statusHint() const override;
    QString statusDetail() const override;
    QString iconPath() const override;
    QCursor cursor() const override;

    void activate(RenderWidget &view) override;
    void deactivate(bool commit) override;
    bool mousePress(QMouseEvent *e) override;
    bool mouseMove(QMouseEvent *e) override;
    bool mouseRelease(QMouseEvent *e) override;
    std::vector<ToolControl> controls() const override;
    bool isControlEnabled(const QString &id) const override;
    void triggerControl(const QString &id, Qt::KeyboardModifiers modifiers) override;
    void onSurfacePicked(const SurfacePick &result) override;
    void cancelGesture() override;
    void paintOverlay(QPainter &painter) override;
    std::vector<ToolLineSegment> depthCuedLines() const override;

private:
    struct Measurement {
        std::size_t a = 0;
        std::size_t b = 0;
    };

    enum class PickAction { None, AddPoint, DragPoint, PreviewPoint };

    int pointAt(const QPointF &screenPos) const;
    void resetSession();
    void finishSegment(std::size_t point);
    double measurementLength(const Measurement &measurement) const;
    // A length as text, with the document's unit after it once the user has set one.
    QString lengthText(double length, int precision) const;
    void trimUnusedPoints();
    void clearPendingPoint();
    void exportMeasurements();
    void printMeasurements() const;
    void saveMeasurements() const;

    std::vector<QVector3D> m_points;
    std::vector<Measurement> m_measurements;
    int m_pendingPoint = -1;
    bool m_pendingPointIsNew = false;
    int m_hoverPoint = -1;
    int m_pressedPoint = -1;
    int m_dragPoint = -1;
    QPoint m_pressPos;
    bool m_dragging = false;
    QVector3D m_previewPoint;
    bool m_hasPreviewPoint = false;
    PickAction m_pickAction = PickAction::None;
};
