#pragma once

#include "renderingsettings.h"

#include <QVector3D>
#include <QWidget>

class QVariantAnimation;

// The light-direction glyph shown while the headlight is dragged: a ball lit as the current
// layer's fill is, under its back-lighting mode; the light as a small ball 1.5 radii out, with
// the ray it comes along from the big ball's surface, occluded as 3D objects are -- faint where
// the big ball hides them, which a projected arrow alone could not show; and the azimuth and
// elevation. It stays a moment
// after the drag ends, then fades.
//
// A plain QWidget painted with QPainter, like ViewAxisGizmo: antialiased and outlined on any
// background at no GPU cost, and absent from offscreen captures, which never drag the light.
class LightGizmo : public QWidget
{
    Q_OBJECT
public:
    explicit LightGizmo(QWidget *parent = nullptr);

    // View-space light direction (x right, y up, z towards the viewer), and how the back of a
    // surface is lit; shows the glyph, at the size view.lightGizmoSize sets.
    void setLight(const QVector3D &direction, FillBackLighting backLighting);
    // Fades the glyph out after a hold; view.lightGizmoLinger sets how long the two take.
    void fadeOut();

protected:
    void paintEvent(QPaintEvent *e) override;

private:
    QVector3D m_direction{0.0f, 0.0f, 1.0f};
    FillBackLighting m_backLighting = FillBackLighting::Single;
    int m_ballRadius = 40;   // half of view.lightGizmoSize, read on every show
    qreal m_opacity = 1.0;
    QVariantAnimation *m_fade = nullptr;
};
