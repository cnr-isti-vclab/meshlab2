#pragma once

#include "interactivetool.h"

#include <QPointF>
#include <QVector3D>

#include <cstdint>
#include <map>
#include <vector>

// Pairs points of the mesh with the pixels of the current raster's photo that show them, and
// fits the raster's camera to the pairs with Align Current Raster to Mesh by Point
// Correspondences. It works in the raster view, where the layers are drawn over the photo
// through the raster's camera: a drag goes from a feature where the mesh shows it to the same
// feature on the photo, and a click pairs a point the mesh already shows in the right place.
// Each pair is drawn as a line from where the camera projects its point to its pixel, so after
// a fit what is left of each line is that pair's error.
//
// The pairs are kept per raster for as long as the tool is engaged, and can be saved and
// loaded as the filter's JSON.
class RasterAlignTool final : public InteractiveTool
{
public:
    QString id() const override;
    QString name() const override;
    QString statusHint() const override;
    QString statusDetail() const override;
    QString iconPath() const override;
    QCursor cursor() const override;
    bool supportsRasterView() const override { return true; }

    void activate(RenderWidget &view) override;
    void deactivate(bool commit) override;
    bool mousePress(QMouseEvent *e) override;
    bool mouseMove(QMouseEvent *e) override;
    bool mouseRelease(QMouseEvent *e) override;
    std::vector<ToolControl> controls() const override;
    bool isControlChecked(const QString &id) const override;
    bool isControlEnabled(const QString &id) const override;
    void triggerControl(const QString &id, Qt::KeyboardModifiers modifiers) override;
    void onSurfacePicked(const SurfacePick &result) override;
    void cancelGesture() override;
    bool gestureInFlight() const override { return m_dragging; }
    void paintOverlay(QPainter &painter) override;

private:
    struct Pair {
        QVector3D point;   // world space
        QPointF pixel;     // in the camera's viewport, from the top-left corner, y down
    };

    // The pairs of the current raster, which needs a camera; null when there is none.
    std::vector<Pair> *currentPairs();
    const std::vector<Pair> *currentPairs() const;
    // The pairs as the filter reads them
    static QByteArray toJson(const std::vector<Pair> &pairs, bool compact);
    void addPairIfComplete();
    void fit();
    void savePairs();
    void loadPairs();

    std::map<std::uint64_t, std::vector<Pair>> m_pairs;   // by raster id
    bool m_estimateFocal = false;
    QString m_lastFit;   // what the last fit said, until the pairs change

    // The drag in progress: pressed on the mesh, released on the photo. The press's pick
    // reports asynchronously, possibly after the release.
    bool m_dragging = false;
    bool m_released = false;
    bool m_pickReported = false;
    bool m_pickHit = false;
    QVector3D m_pickPoint;
    QPointF m_pressPos;
    QPointF m_cursorPos;
    QPointF m_releaseImage;   // in [0, 1] over the image, y down
};
