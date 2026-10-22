#include "systeminfo.h"

#include "document.h"

#include <rhi/qrhi.h>

#include <QGuiApplication>
#include <QMetaObject>
#include <QMutex>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QSysInfo>

#if QT_CONFIG(opengl)
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#endif

#include <deque>

namespace SystemInfo {

namespace {

QPointer<Document> g_log;
QtMessageHandler g_previousHandler = nullptr;
QMutex g_mutex;
std::deque<QString> g_recent;   // newest last
QSet<QString> g_logged;         // first lines already in the log
constexpr std::size_t kRecentWarnings = 20;
constexpr int kLoggedLines = 4;   // a shader compile error carries the whole source after it

void forward(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    if (g_previousHandler)
        g_previousHandler(type, context, message);
    if (type != QtWarningMsg && type != QtCriticalMsg && type != QtFatalMsg)
        return;
    // Logging a line must not land back here, should something on that path warn.
    static thread_local bool inside = false;
    if (inside)
        return;
    inside = true;

    const QString category = QString::fromLatin1(context.category ? context.category : "default");
    const QString text = category == QLatin1String("default")
        ? message : QStringLiteral("[%1] %2").arg(category, message);
    QStringList lines = text.split(QLatin1Char('\n'));
    bool firstTime = false;
    {
        QMutexLocker lock(&g_mutex);
        g_recent.push_back(text.left(4000));
        while (g_recent.size() > kRecentWarnings)
            g_recent.pop_front();
        // A pipeline that fails is tried again every frame: once in the log is enough.
        firstTime = !g_logged.contains(lines.front());
        if (firstTime)
            g_logged.insert(lines.front());
    }
    if (firstTime && g_log) {
        if (lines.size() > kLoggedLines) {
            const int rest = int(lines.size()) - kLoggedLines;
            lines = lines.mid(0, kLoggedLines);
            lines << QObject::tr("(%n more line(s); see Help > About > System)", nullptr, rest);
        }
        const QString entry = QObject::tr("Qt: %1").arg(lines.join(QLatin1Char('\n')));
        const QPointer<Document> doc = g_log;
        // Warnings come from any thread; the log is the main thread's.
        QMetaObject::invokeMethod(doc, [doc, entry] {
            if (doc)
                doc->writeLog(entry, Document::LogSource::Application, Document::LogLevel::Warning);
        }, Qt::QueuedConnection);
    }
    inside = false;
}

QString deviceTypeName(QRhiDriverInfo::DeviceType type)
{
    switch (type) {
    case QRhiDriverInfo::IntegratedDevice: return QStringLiteral("integrated");
    case QRhiDriverInfo::DiscreteDevice: return QStringLiteral("discrete");
    case QRhiDriverInfo::ExternalDevice: return QStringLiteral("external");
    case QRhiDriverInfo::VirtualDevice: return QStringLiteral("virtual");
    case QRhiDriverInfo::CpuDevice: return QStringLiteral("CPU (software)");
    case QRhiDriverInfo::UnknownDevice: break;
    }
    return QStringLiteral("unknown type");
}

// The driver's own description, where the API has one to give: OpenGL's version and renderer
// strings name the driver (Mesa 24.0.5, NVIDIA 550.67, ...), which the device name alone does not.
QString openGlStrings(QRhi *rhi)
{
#if QT_CONFIG(opengl)
    if (rhi->backend() != QRhi::OpenGLES2 || !rhi->makeThreadLocalNativeContextCurrent())
        return {};
    QOpenGLContext *context = QOpenGLContext::currentContext();
    if (!context)
        return {};
    QOpenGLFunctions *gl = context->functions();
    const auto string = [gl](GLenum name) {
        const GLubyte *value = gl->glGetString(name);
        return value ? QString::fromLatin1(reinterpret_cast<const char *>(value)) : QString();
    };
    const QSurfaceFormat format = context->format();
    const QString profile = format.profile() == QSurfaceFormat::CoreProfile ? QStringLiteral("core")
        : format.profile() == QSurfaceFormat::CompatibilityProfile ? QStringLiteral("compatibility")
                                                                   : QStringLiteral("no profile");
    return QStringLiteral("OpenGL version: %1\nOpenGL renderer: %2\nOpenGL vendor: %3\nGLSL version: %4\n"
                          "Context: %5.%6 %7%8\n")
        .arg(string(GL_VERSION), string(GL_RENDERER), string(GL_VENDOR),
             string(0x8B8C /* GL_SHADING_LANGUAGE_VERSION */))
        .arg(format.majorVersion()).arg(format.minorVersion()).arg(profile)
        .arg(context->isOpenGLES() ? QStringLiteral(", OpenGL ES") : QString());
#else
    (void) rhi;
    return {};
#endif
}

} // namespace

void forwardQtWarnings(Document *doc)
{
    g_log = doc;
    if (!g_previousHandler)
        g_previousHandler = qInstallMessageHandler(forward);
}

QStringList recentWarnings()
{
    QMutexLocker lock(&g_mutex);
    return QStringList(g_recent.begin(), g_recent.end());
}

QString graphicsSummary(QRhi *rhi)
{
    if (!rhi)
        return QObject::tr("no graphics backend");
    const QRhiDriverInfo info = rhi->driverInfo();
    QString summary = QStringLiteral("%1 on %2 (%3)")
        .arg(QString::fromLatin1(rhi->backendName()), QString::fromUtf8(info.deviceName),
             deviceTypeName(info.deviceType));
    const QString gl = openGlStrings(rhi);
    if (!gl.isEmpty())
        summary += QStringLiteral(", ") + gl.section(QLatin1Char('\n'), 0, 0);
    return summary;
}

QString graphicsReport(QRhi *rhi)
{
    QString report;
    report += QStringLiteral("Windowing system: %1\n").arg(QGuiApplication::platformName());
    for (const char *variable : { "QT_QPA_PLATFORM", "QT_WIDGETS_RHI", "QT_WIDGETS_RHI_BACKEND",
                                  "QSG_RHI_BACKEND", "QSG_RHI_PROFILE", "LIBGL_ALWAYS_SOFTWARE",
                                  "WAYLAND_DISPLAY", "XDG_SESSION_TYPE" }) {
        if (qEnvironmentVariableIsSet(variable))
            report += QStringLiteral("%1=%2\n").arg(QString::fromLatin1(variable),
                                                     qEnvironmentVariable(variable));
    }
    for (const QScreen *screen : QGuiApplication::screens()) {
        report += QStringLiteral("Screen %1: %2 x %3, device pixel ratio %4\n")
                      .arg(screen->name()).arg(screen->size().width()).arg(screen->size().height())
                      .arg(screen->devicePixelRatio());
    }
    report += QStringLiteral("Kernel: %1 %2\n").arg(QSysInfo::kernelType(), QSysInfo::kernelVersion());

    if (!rhi)
        return report + QObject::tr("Graphics backend: not started yet (no view has drawn)\n");

    const QRhiDriverInfo info = rhi->driverInfo();
    report += QStringLiteral("Graphics API: %1\n").arg(QString::fromLatin1(rhi->backendName()));
    report += QStringLiteral("Device: %1 (%2, vendor 0x%3, device 0x%4)\n")
                  .arg(QString::fromUtf8(info.deviceName), deviceTypeName(info.deviceType))
                  .arg(info.vendorId, 0, 16).arg(info.deviceId, 0, 16);
    report += openGlStrings(rhi);

    const std::pair<QRhi::Feature, const char *> features[] = {
        { QRhi::WideLines, "wide lines" },
        { QRhi::VertexShaderPointSize, "point size from the vertex shader" },
        { QRhi::ScreenSpaceDerivatives, "screen-space derivatives" },
        { QRhi::ElementIndexUint, "32-bit indices" },
        { QRhi::BaseVertex, "base vertex" },
        { QRhi::Instancing, "instancing" },
        { QRhi::TexelFetch, "texel fetch" },
        { QRhi::NonFillPolygonMode, "line polygon mode" },
        { QRhi::MultisampleRenderBuffer, "multisample render buffers" },
        { QRhi::ReadBackAnyTextureFormat, "read back any texture format" },
        { QRhi::Timestamps, "GPU timestamps" },
        { QRhi::Compute, "compute" },
    };
    QStringList supported, missing;
    for (const auto &[feature, name] : features)
        (rhi->isFeatureSupported(feature) ? supported : missing) << QString::fromLatin1(name);
    report += QStringLiteral("Supported: %1\n").arg(supported.join(QStringLiteral(", ")));
    report += QStringLiteral("Not supported: %1\n")
                  .arg(missing.isEmpty() ? QStringLiteral("-") : missing.join(QStringLiteral(", ")));
    QStringList samples;
    for (int count : rhi->supportedSampleCounts())
        samples << QString::number(count);
    report += QStringLiteral("Max texture size: %1, max uniform buffer: %2 bytes, uniform buffer "
                             "alignment: %3, sample counts: %4, frames in flight: %5\n")
                  .arg(rhi->resourceLimit(QRhi::TextureSizeMax))
                  .arg(rhi->resourceLimit(QRhi::MaxUniformBufferRange))
                  .arg(rhi->ubufAlignment())
                  .arg(samples.join(QLatin1Char('/')))
                  .arg(rhi->resourceLimit(QRhi::FramesInFlight));
    report += QStringLiteral("Y up in NDC: %1, clip depth 0..1: %2\n")
                  .arg(rhi->isYUpInNDC() ? QStringLiteral("yes") : QStringLiteral("no"),
                       rhi->isClipDepthZeroToOne() ? QStringLiteral("yes") : QStringLiteral("no"));
    return report;
}

} // namespace SystemInfo
