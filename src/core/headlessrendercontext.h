#pragma once

#include "camerashot.h"

#include <QImage>
#include <QSize>
#include <QString>

#include <memory>

class Document;
class RenderWidget;
class QWidget;

// Manages a hidden RenderWidget + QRhi lifecycle for offscreen rendering.
// Designed for use in headless (no visible window) contexts such as
// pymeshlab2 scripts or automated batch rendering.
//
// Usage:
//   HeadlessRenderContext ctx(doc);
//   if (!ctx.isValid()) { ... handle error ... }
//   QImage img = ctx.snapshot(renderStateJson, QSize(1920, 1080));
//   img.save("output.png");
//
// The renderStateJson is the same JSON format used by the
// "Layer > Render from Render-State JSON" filter and by
// RenderWidget::applyRenderStateJson().
class HeadlessRenderContext
{
public:
    explicit HeadlessRenderContext(Document *doc);
    ~HeadlessRenderContext();

    HeadlessRenderContext(const HeadlessRenderContext &) = delete;
    HeadlessRenderContext &operator=(const HeadlessRenderContext &) = delete;

    bool isValid() const { return m_valid; }

    QImage snapshot(const QString &renderStateJson,
                    const QSize &outputSize,
                    bool transparentBackground = false,
                    QString *error = nullptr);

    // CameraShot corresponding to the last render, or
    // a frame-all-bounding-boxes default if nothing rendered yet.
    CameraShot cameraShot() const;

private:
    void ensureInitialized();
    void wireRenderCallback();
    CameraShot computeDefaultCameraShot() const;

    Document *m_doc = nullptr;
    std::unique_ptr<QWidget> m_container;
    RenderWidget *m_renderWidget = nullptr;
    bool m_valid = false;
};
