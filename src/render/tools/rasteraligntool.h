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
// Either end of a pair can be dragged: the photo end to another pixel, the mesh end to another
// point of the surface; Delete removes the pair under the cursor.
// Each pair is drawn as a line from where the camera projects its point to its pixel, so after
// a fit what is left of each line is that pair's error. Mutual Information refines the camera
// further against the photo, with the pairs as a soft constraint.
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
    bool gestureInFlight() const override { return m_drag != Drag::None; }
    void paintOverlay(QPainter &painter) override;
    std::vector<Loupe> loupes() const override;

private:
    struct Pair {
        QVector3D point;   // world space
        QPointF pixel;     // in the camera's viewport, from the top-left corner, y down
        QVector3D normal;  // the surface's at the point, world space, unit; drawn only
    };

    // The pairs of the current raster, which needs a camera; null when there is none.
    std::vector<Pair> *currentPairs();
    const std::vector<Pair> *currentPairs() const;
    // An end of a pair under the cursor: its index, and whether it is the photo end
    struct Handle {
        int index = -1;
        bool photo = true;
    };
    enum class Drag { None, NewPair, MovePhoto, MoveMesh };

    // The pairs as the filter reads them
    static QByteArray toJson(const std::vector<Pair> &pairs, bool compact);
    // Where a pair's ends are drawn, in widget pixels: the projection of its point, and its pixel
    void ends(const Pair &pair, QPointF &onMesh, QPointF &onPhoto) const;
    QPointF toWidget(const QVector3D &world) const;   // through the raster's camera
    // The two ends as markers that say which is which: the photo's a cross in a square, flat on
    // the image; the mesh's a disc lying on the surface, its normal standing out of it
    void drawPhotoMarker(QPainter &painter, const QPointF &at, bool hovered) const;
    void drawMeshMarker(QPainter &painter, const Pair &pair, const QPointF &at, bool hovered) const;
    void drawLoupeFrame(QPainter &painter, const QRectF &rect, bool photo) const;
    Handle handleAt(const QPointF &widgetPos) const;
    QPointF toPixel(const QPointF &widgetPos) const;   // in the camera's viewport
    void addPairIfComplete();
    void fit();
    // Align Current Raster to Mesh by Mutual Information, with the pairs as its point pairs
    void refineByMutualInformation();
    void savePairs();
    void loadPairs();

    std::map<std::uint64_t, std::vector<Pair>> m_pairs;   // by raster id
    bool m_estimateFocal = false;
    QString m_lastFit;   // what the last fit said, until the pairs change

    // The drag in progress. A new pair is pressed on the mesh and released on the photo, and
    // the press's pick reports asynchronously, possibly after the release. Moving an end drags
    // the handle m_handle; its pixel or point before the drag is kept for a cancel.
    Drag m_drag = Drag::None;
    Handle m_handle;
    Pair m_before;
    Handle m_hover;
    bool m_released = false;
    bool m_pickReported = false;
    bool m_pickHit = false;
    QVector3D m_pickPoint;
    QVector3D m_pickNormal;
    QPointF m_pressPos;
    QPointF m_cursorPos;
    QPointF m_hoverPos;   // the pointer while no drag is under way
    QPointF m_releaseImage;   // in [0, 1] over the image, y down
};
