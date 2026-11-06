#include "lightgizmo.h"

#include "preferences.h"

#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

namespace {

// Sizes in radii of the big ball, whose diameter is the view.lightGizmoSize preference.
constexpr qreal kRayLength = 1.5;     // from the centre to the light
constexpr qreal kLightRadius = 0.15;  // the small ball standing for the light
constexpr int kReadoutHeight = 20;
constexpr int kMinWidth = 170;   // the readout, at its longest, is wider than a small ball

// Room around the big ball for the ray and the light.
int margin(int ballRadius)
{
    return int(ballRadius * (kRayLength - 1.0 + kLightRadius)) + 3;
}
const QColor kLightColor(255, 196, 40);
const QColor kOutlineColor(20, 20, 20, 220);

// The ball as fill_smooth.frag lights a plain surface, its diffuse term sided as in
// two_sided_lighting.glsl, with an antialiased rim.
QImage litBall(int radius, const QVector3D &light, FillBackLighting backLighting, qreal dpr)
{
    const QVector3D frontTint(0.8f, 0.8f, 1.0f);   // kFancyFrontTint
    const QVector3D backTint(1.0f, 0.8f, 0.8f);    // kFancyBackTint
    const QVector3D halfway = (light + QVector3D(0.0f, 0.0f, 1.0f)).normalized();
    constexpr float kAmbient = 0.18f;
    constexpr float kAlbedo = 0.9f;
    const int size = int(std::ceil(2 * radius * dpr));
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const float r = float(size) * 0.5f;
    for (int y = 0; y < size; ++y) {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size; ++x) {
            const float nx = (float(x) + 0.5f - r) / r;
            const float ny = (r - float(y) - 0.5f) / r;
            const float q = nx * nx + ny * ny;
            if (q >= 1.0f)
                continue;
            const float nz = std::sqrt(1.0f - q);
            const QVector3D n(nx, ny, nz);
            const float ndl = QVector3D::dotProduct(n, light);
            const float diffuse = backLighting == FillBackLighting::Single ? std::max(ndl, 0.0f)
                                                                           : std::abs(ndl);
            const QVector3D tint = backLighting != FillBackLighting::Fancy ? QVector3D(1, 1, 1)
                                 : ndl >= 0.0f ? frontTint : backTint;
            // A plain material at full roughness: specular power 8, strength 0.02.
            const float specular =
                0.02f * std::pow(std::max(QVector3D::dotProduct(n, halfway), 0.0f), 8.0f);
            const QVector3D lit = kAlbedo * (QVector3D(kAmbient, kAmbient, kAmbient)
                                             + (1.0f - kAmbient) * diffuse * tint)
                                + QVector3D(specular, specular, specular);
            const float alpha = std::min(1.0f, (1.0f - std::sqrt(q)) * r);
            const auto channel = [alpha](float v) { return int(255 * std::min(v, 1.0f) * alpha); };
            line[x] = qRgba(channel(lit.x()), channel(lit.y()), channel(lit.z()), int(255 * alpha));
        }
    }
    image.setDevicePixelRatio(dpr);
    return image;
}

} // namespace

LightGizmo::LightGizmo(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);
    hide();

    // Held for the first half, so the result can be read, then faded.
    m_fade = new QVariantAnimation(this);
    m_fade->setStartValue(1.0);
    m_fade->setKeyValueAt(0.5, 1.0);
    m_fade->setEndValue(0.0);
    connect(m_fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_opacity = value.toReal();
        update();
    });
    connect(m_fade, &QVariantAnimation::finished, this, &QWidget::hide);
}

void LightGizmo::setLight(const QVector3D &direction, FillBackLighting backLighting)
{
    m_fade->stop();
    m_opacity = 1.0;
    m_direction = direction.normalized();
    m_backLighting = backLighting;
    // Read on every show, so a changed preference applies from the next drag on. The bottom
    // edge stays where the owner placed it, as the gizmo grows upwards from the corner.
    m_ballRadius = Preferences::instance().intValue(QStringLiteral("view.lightGizmoSize")) / 2;
    const int side = 2 * (m_ballRadius + margin(m_ballRadius));
    const QSize wanted(std::max(kMinWidth, side), side + kReadoutHeight);
    if (size() != wanted) {
        const int bottom = y() + height();
        setFixedSize(wanted);
        move(x(), bottom - height());
    }
    show();
    raise();
    update();
}

void LightGizmo::fadeOut()
{
    if (!isVisible())
        return;
    const int ms = qRound(1000 * Preferences::instance().doubleValue(QStringLiteral("view.lightGizmoLinger")));
    if (ms <= 0) {
        hide();
        return;
    }
    m_fade->setDuration(ms);
    m_fade->start();
}

void LightGizmo::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(m_opacity);

    const qreal r = m_ballRadius;
    const QPointF c(width() * 0.5, r + margin(m_ballRadius));
    p.drawImage(QPointF(c.x() - r, c.y() - r), litBall(m_ballRadius, m_direction, m_backLighting, devicePixelRatioF()));

    // Rim: dark then light, so the ball reads on dark and light backgrounds alike.
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(kOutlineColor, 1.5));
    p.drawEllipse(c, r + 0.5, r + 0.5);
    p.setPen(QPen(QColor(255, 255, 255, 90), 1.0));
    p.drawEllipse(c, r + 2.0, r + 2.0);

    // The light: a small ball at kRayLength radii along the direction, and the ray it comes
    // along, from the big ball's surface to the small ball's. The ray points at the small ball's
    // centre, so the part of it that overlaps the small ball on screen lies all behind it when the
    // light is in front (the small ball is drawn over the ray) and all in front of it when the
    // light is behind (the ray is drawn over the small ball).
    const bool front = m_direction.z() >= 0.0f;
    const QPointF axis(m_direction.x() * r, -m_direction.y() * r);
    const QPointF centre = c + kRayLength * axis;
    const QPointF base = c + axis;
    const QPointF end = c + (kRayLength - kLightRadius) * axis;
    const auto drawRay = [&](QPainter &q) {
        q.setPen(QPen(kOutlineColor, 3.0, Qt::SolidLine, Qt::RoundCap));
        q.drawLine(base, end);
        q.setPen(QPen(kLightColor, 1.5, Qt::SolidLine, Qt::RoundCap));
        q.drawLine(base, end);
    };
    const auto drawLight = [&](QPainter &q) {
        const qreal lr = kLightRadius * r;
        QRadialGradient shade(centre, lr, centre - QPointF(0.4, 0.4) * lr);
        shade.setColorAt(0.0, QColor(255, 248, 210));
        shade.setColorAt(0.55, kLightColor);
        shade.setColorAt(1.0, QColor(190, 120, 10));
        q.setBrush(shade);
        q.setPen(QPen(kOutlineColor, 1.0));
        q.drawEllipse(centre, lr, lr);
    };
    // Ray and light are composed in a layer first, so that the faint pass below fades them as
    // one object instead of letting the ray show through the light.
    const qreal dpr = devicePixelRatioF();
    QImage layer(size() * dpr, QImage::Format_ARGB32_Premultiplied);
    layer.setDevicePixelRatio(dpr);
    layer.fill(Qt::transparent);
    {
        QPainter q(&layer);
        q.setRenderHint(QPainter::Antialiasing);
        if (front) {
            drawRay(q);
            drawLight(q);
        } else {
            drawLight(q);
            drawRay(q);
        }
    }
    // Behind the big ball, every point of ray and light lies farther than it, so whatever
    // projects inside its outline is hidden: drawn there in a fainter shade, the rest in full.
    if (front) {
        p.drawImage(QPointF(0, 0), layer);
    } else {
        QPainterPath ball;
        ball.addEllipse(c, r, r);
        QPainterPath outside;
        outside.addRect(rect());
        p.setClipPath(outside.subtracted(ball));
        p.drawImage(QPointF(0, 0), layer);
        p.setClipPath(ball);
        p.setOpacity(m_opacity * 0.3);
        p.drawImage(QPointF(0, 0), layer);
        p.setClipping(false);
        p.setOpacity(m_opacity);
    }

    // Azimuth from the view axis, positive to the right; elevation positive up.
    const int azimuth = qRound(qRadiansToDegrees(std::atan2(m_direction.x(), m_direction.z())));
    const int elevation = qRound(qRadiansToDegrees(std::asin(std::clamp(m_direction.y(), -1.0f, 1.0f))));
    const QString text = tr("az %1°  el %2°").arg(azimuth).arg(elevation)
                         + (front ? QString() : tr("  behind"));
    QFont font = p.font();
    font.setPointSizeF(font.pointSizeF() * 0.85);
    p.setFont(font);
    const QRectF box(0, height() - kReadoutHeight, width(), kReadoutHeight - 2);
    QRectF pill = p.fontMetrics().boundingRect(text);
    pill.setWidth(pill.width() + 12);
    pill.setHeight(box.height());
    pill.moveCenter(box.center());
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 150));
    p.drawRoundedRect(pill, 6, 6);
    p.setPen(QColor(255, 255, 255, 230));
    p.drawText(box, Qt::AlignCenter, text);
}
