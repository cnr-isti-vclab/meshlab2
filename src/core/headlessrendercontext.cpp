#include "headlessrendercontext.h"
#include "document.h"
#include "../render/renderwidget.h"

#include <QEventLoop>
#include <QTimer>
#include <QWidget>

#include <cmath>

HeadlessRenderContext::HeadlessRenderContext(Document *doc)
    : m_doc(doc)
{
    ensureInitialized();
}

HeadlessRenderContext::~HeadlessRenderContext()
{
    if (m_doc && m_valid) {
        // Remove our render callback.
        m_doc->setRenderStateSnapshotFunction({});
    }
    if (m_renderWidget) {
        m_renderWidget->deleteLater();
        m_renderWidget = nullptr;
    }
}

void HeadlessRenderContext::ensureInitialized()
{
    if (m_valid)
        return;
    if (!m_doc)
        return;

    // Create a hidden top-level widget to host the RenderWidget.
    m_container = std::make_unique<QWidget>(nullptr);
    m_container->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_container->resize(1, 1);
    // The view goes in before the window is shown: a top-level window sets up its QRhi when it
    // is first shown, and only for the QRhiWidgets it holds then. Shown empty, it had none, and
    // every snapshot fell back to a QRhi of its own, made and torn down each time (and, on
    // OpenGL, made without the context format the view asks for).
    m_renderWidget = new RenderWidget(m_doc, m_container.get());
    m_container->show();

    // Wait for the first frame so the QRhi is fully initialised.
    {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);

        const QMetaObject::Connection frameConn =
            QObject::connect(m_renderWidget, &RenderWidget::frameRendered, &loop,
                [&loop](float, float, bool, bool) { loop.quit(); });

        timeout.start(5000);
        loop.exec();
        QObject::disconnect(frameConn);
    }

    // If the RenderWidget never produced a frame (e.g. no GPU), bail out.
    if (!m_renderWidget || !m_renderWidget->isVisible()) {
        return;
    }

    wireRenderCallback();
    m_valid = true;
}

void HeadlessRenderContext::wireRenderCallback()
{
    m_doc->setRenderStateSnapshotFunction(
        [this](const QString &renderStateJson,
               const QSize &pixelSize,
               QImage &outImage,
               CameraShot &outShot,
               QString &errorMessage) -> bool
        {
            if (!m_renderWidget) {
                errorMessage = QStringLiteral("Headless render widget not ready");
                return false;
            }

            const QString previousRenderState = m_renderWidget->renderStateJson();
            QString applyError;
            if (!m_renderWidget->applyRenderStateJson(renderStateJson, &applyError)) {
                errorMessage = applyError;
                return false;
            }

            QString captureError;
            outImage = m_renderWidget->renderOffscreenToImage(pixelSize, false, &captureError);
            outShot = m_renderWidget->cameraShotForViewport(pixelSize);

            QString restoreError;
            if (!m_renderWidget->applyRenderStateJson(previousRenderState, &restoreError)) {
                if (outImage.isNull()) {
                    errorMessage = QStringLiteral(
                        "Failed to restore previous render state after snapshot: %1")
                        .arg(restoreError);
                    return false;
                }
            }

            if (outImage.isNull()) {
                errorMessage = captureError.isEmpty()
                    ? QStringLiteral("Render target capture failed")
                    : captureError;
                return false;
            }

            errorMessage.clear();
            return true;
        });
}

QImage HeadlessRenderContext::snapshot(
    const QString &renderStateJson,
    const QSize &outputSize,
    bool transparentBackground,
    QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return QImage();
    };

    if (!m_valid)
        return fail(QStringLiteral("Headless render context not initialised"));

    if (outputSize.width() <= 0 || outputSize.height() <= 0)
        return fail(QStringLiteral("Invalid output size"));

    QImage outImage;
    CameraShot outShot;
    QString captureError;
    if (!m_doc->renderSnapshotFromStateJson(
            renderStateJson, outputSize, outImage, outShot, &captureError)) {
        return fail(captureError.isEmpty()
                    ? QStringLiteral("Offscreen render failed")
                    : captureError);
    }

    return outImage;
}

CameraShot HeadlessRenderContext::cameraShot() const
{
    if (m_renderWidget)
        return m_renderWidget->cameraShotForViewport(QSize(1, 1));

    return computeDefaultCameraShot();
}

CameraShot HeadlessRenderContext::computeDefaultCameraShot() const
{
    if (!m_doc)
        return {};

    float sceneMin[3] = {};
    float sceneMax[3] = {};
    bool bboxValid = false;
    for (int i = 0; i < m_doc->meshCount(); ++i) {
        const Document::MeshEntry &entry = m_doc->mesh(i);
        if (!entry.visible)
            continue;
        if (entry.mesh.bbox.IsNull())
            continue;
        const vcg::Box3f &b = entry.mesh.bbox;
        if (!bboxValid) {
            sceneMin[0] = b.min[0]; sceneMin[1] = b.min[1]; sceneMin[2] = b.min[2];
            sceneMax[0] = b.max[0]; sceneMax[1] = b.max[1]; sceneMax[2] = b.max[2];
            bboxValid = true;
        } else {
            sceneMin[0] = std::min(sceneMin[0], b.min[0]);
            sceneMin[1] = std::min(sceneMin[1], b.min[1]);
            sceneMin[2] = std::min(sceneMin[2], b.min[2]);
            sceneMax[0] = std::max(sceneMax[0], b.max[0]);
            sceneMax[1] = std::max(sceneMax[1], b.max[1]);
            sceneMax[2] = std::max(sceneMax[2], b.max[2]);
        }
    }
    if (!bboxValid)
        return {};

    const float center[3] = {
        (sceneMin[0] + sceneMax[0]) * 0.5f,
        (sceneMin[1] + sceneMax[1]) * 0.5f,
        (sceneMin[2] + sceneMax[2]) * 0.5f,
    };
    const float sizeX = sceneMax[0] - sceneMin[0];
    const float sizeY = sceneMax[1] - sceneMin[1];
    const float sizeZ = sceneMax[2] - sceneMin[2];
    const float diag = std::sqrt(sizeX * sizeX + sizeY * sizeY + sizeZ * sizeZ);
    const float dist = std::max(diag * 1.5f, 1.0f);

    CameraShot::VcgShot shot;
    shot.Intrinsics.cameraType = vcg::Camera<float>::PERSPECTIVE;
    shot.Intrinsics.ViewportPx = vcg::Point2i(1, 1);
    shot.Intrinsics.FocalMm = 50.0f;
    shot.Intrinsics.PixelSizeMm = vcg::Point2f(1.0f, 1.0f);
    shot.LookAt(center[0], center[1], center[2] + dist,
                center[0], center[1], center[2],
                0.0f, 1.0f, 0.0f);
    return CameraShot::fromVcgShot(shot);
}
