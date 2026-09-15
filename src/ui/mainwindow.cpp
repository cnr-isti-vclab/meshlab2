#include "mainwindow.h"

#include "aboutdialog.h"
#include "filterpluginsinfodialog.h"
#include "filterparam.h"
#include "document.h"
#include "helperprocess.h"
#include "meshfilterpanel.h"
#include "meshsaveoptionsdialog.h"
#include "filedialogdirectory.h"
#include "renderwidget.h"
#include "snapshotdialog.h"
#include "viewsplitterlayout.h"
#include "interactivetool.h"
#include "layerwidget.h"
#include "memorypressuremonitor.h"
#include "undographwidget.h"
#ifdef MESHLAB2_PYTHON_CONSOLE
#include "pythonconsole.h"
#include "PythonHost.h"
#endif
#include <wrap/io_trimesh/io_mask.h>
#include <QButtonGroup>
#include <QClipboard>
#include <QApplication>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImageWriter>
#include <QGuiApplication>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QTextBrowser>
#include <QScreen>
#include <QScrollBar>
#include <QSplitter>
#include <QDockWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QAction>
#include <QActionGroup>
#include <QIcon>
#include <QToolBar>
#include <QBrush>
#include <QCheckBox>
#include <QColor>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMouseEvent>
#include <QMimeData>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QDateTime>

#include "colormap.h"
#include "preferences.h"
#include "preferencesdialog.h"
#include "processmemoryinfo.h"

#include <QSettings>
#include <QSet>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
// QTreeWidget replaced by UndoGraphWidget
#include <QToolButton>
#include <QVector3D>
#include <QVBoxLayout>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace {
constexpr std::size_t kFrameStatsWindow = 100;
int recentMeshesLimit()
{
    return Preferences::instance().intValue(QStringLiteral("document.recentFileCount"));
}

QString normalizeRecentPath(const QString &path)
{
    const QFileInfo fi(path);
    const QString canonical = fi.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    return fi.absoluteFilePath();
}

bool sameRecentPath(const QString &a, const QString &b)
{
#if defined(Q_OS_WIN) || defined(Q_OS_DARWIN)
    return a.compare(b, Qt::CaseInsensitive) == 0;
#else
    return a == b;
#endif
}

QString fileExtensionLower(const QString &path)
{
    return QFileInfo(path).suffix().toLower();
}

QString ioCapabilityText(const MeshIOCapabilities &capabilities)
{
    using M = vcg::tri::io::Mask;
    const int m = capabilities.mask;
    QStringList out;
    if (m & M::IOM_VERTCOORD)    out << QObject::tr("vertices");
    if (m & M::IOM_EDGEINDEX)    out << QObject::tr("edges");
    if (m & M::IOM_FACEINDEX)    out << QObject::tr("faces");
    if (m & M::IOM_VERTNORMAL)   out << QObject::tr("vertex normals");
    if (m & M::IOM_FACENORMAL)   out << QObject::tr("face normals");
    if (m & M::IOM_WEDGNORMAL)   out << QObject::tr("corner normals");
    if (m & M::IOM_VERTCOLOR)    out << QObject::tr("vertex colors");
    if (m & M::IOM_EDGECOLOR)    out << QObject::tr("edge colors");
    if (m & M::IOM_FACECOLOR)    out << QObject::tr("face colors");
    if (m & M::IOM_WEDGCOLOR)    out << QObject::tr("corner colors");
    if (m & M::IOM_VERTQUALITY)  out << QObject::tr("vertex quality");
    if (m & M::IOM_FACEQUALITY)  out << QObject::tr("face quality");
    if (m & M::IOM_VERTTEXCOORD) out << QObject::tr("vertex UVs");
    if (m & M::IOM_WEDGTEXCOORD) out << QObject::tr("corner UVs");
    if (m & M::IOM_WEDGTEXMULTI) out << QObject::tr("texture groups");
    if (m & M::IOM_VERTRADIUS)   out << QObject::tr("vertex radius");
    if (m & M::IOM_CAMERA)       out << QObject::tr("camera");
    if (m & M::IOM_BITPOLYGONAL) out << QObject::tr("polygons");
    if (capabilities.materials)   out << QObject::tr("materials");
    if (capabilities.textureImages) out << QObject::tr("texture images");
    return out.isEmpty() ? QObject::tr("not declared") : out.join(QStringLiteral(", "));
}

QString extensionFromNameFilter(const QString &nameFilter)
{
    const int wildcardPos = nameFilter.indexOf(QStringLiteral("*."));
    if (wildcardPos < 0)
        return QString();
    int start = wildcardPos + 2;
    int end = start;
    while (end < nameFilter.size()) {
        const QChar c = nameFilter[end];
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-'))
            break;
        ++end;
    }
    if (end <= start)
        return QString();
    return nameFilter.mid(start, end - start).toLower();
}

QString appendSaveExtensionIfMissing(const QString &path, const QString &selectedFilter)
{
    if (!QFileInfo(path).suffix().isEmpty())
        return path;

    QString ext = extensionFromNameFilter(selectedFilter);
    if (ext.isEmpty())
        ext = QStringLiteral("ply");
    return QStringLiteral("%1.%2").arg(path, ext);
}

QString rasterImageOpenDialogFilter()
{
    QStringList patterns;
    const QList<QByteArray> formats = QImageReader::supportedImageFormats();
    patterns.reserve(formats.size());
    for (const QByteArray &format : formats) {
        const QString suffix = QString::fromLatin1(format).toLower();
        if (!suffix.isEmpty())
            patterns.push_back(QStringLiteral("*.%1").arg(suffix));
    }
    patterns.removeDuplicates();
    patterns.sort(Qt::CaseInsensitive);
    if (patterns.isEmpty())
        return QObject::tr("All Files (*)");
    return QObject::tr("Image Files (%1);;All Files (*)").arg(patterns.join(QLatin1Char(' ')));
}

bool canReadRasterImage(const QString &path)
{
    return !QImageReader::imageFormat(path).isEmpty();
}

bool saveFormatSupportsBinary(const QString &extension)
{
    return extension == QLatin1String("ply") || extension == QLatin1String("stl");
}

bool saveFormatSupportsEmbeddedTextures(const QString &extension)
{
    return extension == QLatin1String("gltf") || extension == QLatin1String("glb");
}

bool saveFormatSupportsCopyAssociatedTextures(const QString &extension)
{
    return extension == QLatin1String("ply");
}

bool saveFormatSupportsDracoCompression(const QString &extension)
{
    return extension == QLatin1String("gltf") || extension == QLatin1String("glb");
}

int availableSaveMaskForMesh(const Document::MeshEntry &entry)
{
    int mask = entry.ioMask;
    mask |= vcg::tri::io::Mask::IOM_VERTCOORD;
    if (entry.mesh.FN() > 0) {
        mask |= vcg::tri::io::Mask::IOM_FACEINDEX;
        // Polygonal export is an output choice, not an optional mesh component:
        // disabling it deliberately writes the underlying triangulation.
        mask |= vcg::tri::io::Mask::IOM_BITPOLYGONAL;
    }
    if (entry.mesh.EN() > 0)
        mask |= vcg::tri::io::Mask::IOM_EDGEINDEX;
    return mask;
}

int requiredSaveMaskForMesh(const Document::MeshEntry &entry, int capabilityMask)
{
    int requiredMask = 0;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_VERTCOORD) != 0 && entry.mesh.VN() > 0)
        requiredMask |= vcg::tri::io::Mask::IOM_VERTCOORD;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_FACEINDEX) != 0 && entry.mesh.FN() > 0)
        requiredMask |= vcg::tri::io::Mask::IOM_FACEINDEX;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_EDGEINDEX) != 0 && entry.mesh.EN() > 0)
        requiredMask |= vcg::tri::io::Mask::IOM_EDGEINDEX;
    return requiredMask;
}

int defaultSaveMaskForMesh(const Document::MeshEntry &entry, int capabilityMask)
{
    const int availableMask = availableSaveMaskForMesh(entry);
    int mask = availableMask & capabilityMask;
    mask |= requiredSaveMaskForMesh(entry, capabilityMask);
    if ((entry.ioMask & vcg::tri::io::Mask::IOM_BITPOLYGONAL) == 0)
        mask &= ~vcg::tri::io::Mask::IOM_BITPOLYGONAL;
    // Prefer wedge attributes over per-vertex ones when both are available.
    if ((mask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0)
        mask &= ~vcg::tri::io::Mask::IOM_VERTTEXCOORD;
    if ((mask & vcg::tri::io::Mask::IOM_WEDGNORMAL) != 0)
        mask &= ~vcg::tri::io::Mask::IOM_VERTNORMAL;
    return mask;
}



// The level decides the colour and the tag; the source only qualifies the tag, since
// which subsystem spoke matters far less than how loudly.
QColor logLevelColor(Document::LogLevel level)
{
    switch (level) {
    case Document::LogLevel::Error:
        return QColor(200, 30, 30);
    case Document::LogLevel::Warning:
        return QColor(170, 110, 0);
    case Document::LogLevel::Debug:
        return QColor(130, 130, 130);
    case Document::LogLevel::Info:
        break;
    }
    return QColor(50, 50, 50);
}

QString logEntryTag(Document::LogSource source, Document::LogLevel level)
{
    switch (level) {
    case Document::LogLevel::Error:
        return QObject::tr("err");
    case Document::LogLevel::Warning:
        return QObject::tr("wrn");
    case Document::LogLevel::Debug:
        return QObject::tr("dbg");
    case Document::LogLevel::Info:
        break;
    }
    return source == Document::LogSource::VCG ? QObject::tr("vcg") : QObject::tr("app");
}

// Index of the document log entry an item renders, so the panel can drop the right one
// even when quieter entries between them were filtered out.
constexpr int kLogEntryIndexRole = Qt::UserRole + 1;

QString formatLogTimestamp(MainWindow::LogTimestampMode mode, qint64 epochMs)
{
    if (mode == MainWindow::LogTimestampMode::None || epochMs <= 0)
        return {};
    if (mode == MainWindow::LogTimestampMode::Clock)
        return QDateTime::fromMSecsSinceEpoch(epochMs).toString(QStringLiteral("HH:mm:ss.zzz"));

    // Elapsed since startup. Formatted by hand rather than through QTime so a session
    // longer than a day keeps counting instead of wrapping around midnight.
    const qint64 ms = std::max<qint64>(0, epochMs - Document::applicationStartMSecsSinceEpoch());
    const qint64 seconds = ms / 1000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'))
        .arg(ms % 1000, 3, 10, QLatin1Char('0'));
}

Document::LogLevel logVerbosityPreference()
{
    const QString id = Preferences::instance().stringValue(QStringLiteral("log.verbosity"));
    if (id == QStringLiteral("error"))
        return Document::LogLevel::Error;
    if (id == QStringLiteral("warning"))
        return Document::LogLevel::Warning;
    if (id == QStringLiteral("debug"))
        return Document::LogLevel::Debug;
    return Document::LogLevel::Info;
}

MainWindow::LogTimestampMode logTimestampPreference()
{
    const QString id = Preferences::instance().stringValue(QStringLiteral("log.timestamp"));
    if (id == QStringLiteral("none"))
        return MainWindow::LogTimestampMode::None;
    if (id == QStringLiteral("clock"))
        return MainWindow::LogTimestampMode::Clock;
    return MainWindow::LogTimestampMode::Elapsed;
}
}

void MainWindow::appendLogItem(const Document::LogEntry &entry, int entryIndex, bool replaceLast)
{
    QListWidget *logWidget = m_logListWidget;
    QListWidgetItem *item = nullptr;
    // Reuse the last row only when it really renders the entry being replaced: at a low
    // verbosity the document's previous entry may have been filtered out, and overwriting
    // whatever happens to be at the bottom would corrupt an unrelated line.
    if (replaceLast && logWidget->count() > 0
        && logWidget->item(logWidget->count() - 1)->data(kLogEntryIndexRole).toInt() == entryIndex) {
        item = logWidget->item(logWidget->count() - 1);
    } else {
        item = new QListWidgetItem(logWidget);
    }

    const QString stamp = formatLogTimestamp(m_logTimestampMode, entry.epochMs);
    const QString tag = logEntryTag(entry.source, entry.level);
    item->setText(stamp.isEmpty()
        ? QStringLiteral("[%1] %2").arg(tag, entry.message)
        : QStringLiteral("%1 [%2] %3").arg(stamp, tag, entry.message));
    item->setForeground(QBrush(logLevelColor(entry.level)));
    item->setData(kLogEntryIndexRole, entryIndex);

    logWidget->scrollToBottom();
}

void MainWindow::rebuildLogPanel()
{
    if (!m_logListWidget || !m_doc)
        return;
    m_logListWidget->clear();
    const auto &entries = m_doc->logMessages();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].level > m_logVerbosity)
            continue;
        appendLogItem(entries[i], static_cast<int>(i), false);
    }
}

MainWindow::~MainWindow() = default;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("MeshLab"));
    setAcceptDrops(true);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect avail = screen->availableGeometry();
        resize(avail.width(), avail.height());
    } else {
        resize(800, 600);
    }

    m_doc = new Document(this);
    m_doc->setUndoMemoryLimitBytes(
        qint64(Preferences::instance().intValue(QStringLiteral("document.undoMemoryLimitMiB")))
        * 1024LL * 1024LL);
    m_purgeUndoOnMemoryPressure = Preferences::instance().boolValue(
        QStringLiteral("document.purgeUndoOnMemoryPressure"));
    m_memoryPressureMonitor = new MemoryPressureMonitor(this);
    connect(
        m_memoryPressureMonitor,
        &MemoryPressureMonitor::memoryPressure,
        this,
        [this](MemoryPressureLevel level) {
            if (!m_purgeUndoOnMemoryPressure || !m_doc)
                return;
            const qint64 beforeBytes = m_doc->undoMemoryStats().totalBytes();
            m_doc->handleUndoMemoryPressure(level == MemoryPressureLevel::Critical);
            const qint64 afterBytes = m_doc->undoMemoryStats().totalBytes();
            if (afterBytes < beforeBytes) {
                statusBar()->showMessage(
                    tr("Undo history reduced under system memory pressure"), 5000);
            }
        });

    m_viewSplitter = new QSplitter(Qt::Horizontal, this);
    m_viewSplitter->setChildrenCollapsible(false);
    setCentralWidget(m_viewSplitter);

    // Make the dock-area separators wide enough to grab comfortably.
    setStyleSheet(QStringLiteral("QMainWindow::separator { width: 5px; height: 5px; }"));

    RenderWidget *initialView = createRenderWidget(m_viewSplitter);
    setCurrentRenderWidget(initialView);

    // Capture/restore camera and render settings alongside mesh undo checkpoints.
    m_doc->setViewStateFunctions(
        [this]() -> ViewState {
            RenderWidget *v = m_currentRenderWidget;
            return v ? v->captureViewState() : ViewState{};
        },
        [this](const ViewState &vs, bool restoreCamera) {
            RenderWidget *v = m_currentRenderWidget;
            if (v)
                v->restoreViewState(vs, restoreCamera);
        });

    m_doc->setRenderStateSnapshotFunction(
        [this](const QString &renderStateJson,
               const QSize &pixelSize,
               QImage &outImage,
               CameraShot &outShot,
               QString &errorMessage) -> bool {
            RenderWidget *view = currentRenderWidget();
            if (!view) {
                errorMessage = tr("No active view");
                return false;
            }

            const QString previousRenderState = view->renderStateJson();
            QString applyError;
            if (!view->applyRenderStateJson(renderStateJson, &applyError)) {
                errorMessage = applyError;
                return false;
            }

            const qreal dpr = qMax(1.0, view->devicePixelRatioF());
            const QSize snapshotSize =
                pixelSize.isValid()
                ? pixelSize
                : QSize(
                    qMax(1, int(std::lround(double(view->width()) * dpr))),
                    qMax(1, int(std::lround(double(view->height()) * dpr))));

            QString captureError;
            outImage = view->renderOffscreenToImage(snapshotSize, false, &captureError);
            outShot = view->cameraShotForViewport(snapshotSize);

            QString restoreError;
            if (!view->applyRenderStateJson(previousRenderState, &restoreError)) {
                if (outImage.isNull()) {
                    errorMessage = tr("Failed to restore previous render state after snapshot: %1")
                                     .arg(restoreError);
                    return false;
                }
                m_doc->writeLog(
                    tr("Warning: failed to restore previous render state after snapshot: %1")
                        .arg(restoreError),
                    Document::LogSource::Application, Document::LogLevel::Warning);
            }

            if (outImage.isNull()) {
                errorMessage = captureError.isEmpty()
                    ? tr("Render target capture failed")
                    : captureError;
                return false;
            }

            errorMessage.clear();
            return true;
        });

#ifdef MESHLAB2_PYTHON_CONSOLE
    m_terminalButton = new QToolButton(this);
    m_terminalButton->setText(QStringLiteral(">_"));
    m_terminalButton->setToolTip(tr("Toggle Python console"));
    m_terminalButton->setCheckable(true);
    m_terminalButton->setChecked(false);
    m_terminalButton->setAutoRaise(true);
    connect(m_terminalButton, &QToolButton::toggled, this, [this](bool checked) {
        if (checked) {
            if (m_logDock)
                m_logDock->hide();
            if (m_pythonConsoleDock) {
                m_pythonConsoleDock->show();
                if (m_pythonConsole)
                    m_pythonConsole->setFocus();
            }
        } else {
            if (m_pythonConsoleDock)
                m_pythonConsoleDock->hide();
            if (m_logDock)
                m_logDock->show();
        }
    });
    statusBar()->addWidget(m_terminalButton, 0);
#endif

    m_loadProgressBar = new QProgressBar(this);
    m_loadProgressBar->setRange(0, 100);
    m_loadProgressBar->setTextVisible(false);
    m_loadProgressBar->setFixedWidth(160);
    m_loadProgressBar->setVisible(false);
    statusBar()->addPermanentWidget(m_loadProgressBar, 0);

    m_filterProgressBar = new QProgressBar(this);
    m_filterProgressBar->setRange(0, 100);
    m_filterProgressBar->setTextVisible(false);
    m_filterProgressBar->setFixedWidth(160);
    m_filterProgressBar->setVisible(false);
    statusBar()->addPermanentWidget(m_filterProgressBar, 0);

    m_filterCancelButton = new QToolButton(this);
    m_filterCancelButton->setText(tr("Cancel"));
    m_filterCancelButton->setAutoRaise(true);
    m_filterCancelButton->setVisible(false);
    connect(m_filterCancelButton, &QToolButton::clicked, this, [this]() {
        if (!m_doc || !m_filterCancelButton)
            return;
        m_doc->requestOperationCancel();
        m_filterCancelButton->setEnabled(false);
        statusBar()->showMessage(tr("Cancelling filter..."));
    });
    statusBar()->addPermanentWidget(m_filterCancelButton, 0);

    m_frameStatsLabel = new QLabel(this);
    QFont statsFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    statsFont.setStyleHint(QFont::TypeWriter);
    m_frameStatsLabel->setFont(statsFont);
    statusBar()->addPermanentWidget(m_frameStatsLabel, 1);

    m_layerWidget = new LayerWidget(m_doc, this);
    connect(m_layerWidget, &LayerWidget::filterActionRequested, this,
            &MainWindow::openFilterOrRunIfParameterless);
    m_layerDock = new QDockWidget(tr("Layers"), this);
    m_layerDock->setWidget(m_layerWidget);

    // Custom title bar with tree/table toggle
    auto *titleBar = new QWidget();
    auto *titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(8, 0, 4, 0);
    titleLayout->addWidget(new QLabel(tr("Layers")));
    titleLayout->addStretch();
    auto *toggleBtn = new QToolButton();
    toggleBtn->setText(tr("⬍ Table"));
    toggleBtn->setToolTip(tr("Toggle between tree and table view"));
    toggleBtn->setCheckable(true);
    toggleBtn->setChecked(false);
    toggleBtn->setAutoRaise(true);
    connect(toggleBtn, &QToolButton::toggled, this, [this, toggleBtn](bool checked) {
        m_layerWidget->setViewMode(checked ? LayerWidget::ViewMode::Table : LayerWidget::ViewMode::Tree);
        toggleBtn->setText(checked ? tr("⬍ Tree") : tr("⬍ Table"));
    });
    titleLayout->addWidget(toggleBtn);
    m_layerDock->setTitleBarWidget(titleBar);

    m_layerDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    addDockWidget(Qt::RightDockWidgetArea, m_layerDock);

    m_filterPanel = new MeshFilterPanel(m_doc, this);
    m_filterPanel->setViewContextProvider([this]() -> MeshFilterPanel::ViewContext {
        if (m_renderWidgets.isEmpty())
            return {};
        RenderWidget *v = m_renderWidgets.constFirst();
        return { v->trackballCenter(), v->cameraEyePosition(), v->cameraViewDirection() };
    });
    m_filterPanel->setCameraStateProvider([this]() -> QString {
        if (m_renderWidgets.isEmpty())
            return QString();
        RenderWidget *v = m_renderWidgets.constFirst();
        return v ? v->cameraStateJson() : QString();
    });
    m_filterPanel->setRenderStateProvider([this]() -> QString {
        if (m_renderWidgets.isEmpty())
            return QString();
        RenderWidget *v = m_renderWidgets.constFirst();
        return v ? v->renderStateJson() : QString();
    });
    m_filterDock = new QDockWidget(tr("Filters"), this);
    m_filterDock->setWidget(m_filterPanel);
    m_filterDock->setFeatures(QDockWidget::NoDockWidgetFeatures);
    addDockWidget(Qt::RightDockWidgetArea, m_filterDock);
    splitDockWidget(m_layerDock, m_filterDock, Qt::Vertical);
    const int rightColumnWidth = std::max(260, width() * 3 / 10);
    m_layerDock->setMinimumWidth(rightColumnWidth);
    m_filterDock->setMinimumWidth(rightColumnWidth);
    resizeDocks({ m_layerDock }, { rightColumnWidth }, Qt::Horizontal);
    resizeDocks({ m_layerDock, m_filterDock }, { 1, 1 }, Qt::Vertical);

    m_logListWidget = new QListWidget(this);
    m_logListWidget->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_logListWidget->setContextMenuPolicy(Qt::ActionsContextMenu);

    auto copyLogSelectionToClipboard = [this]() {
        if (!m_logListWidget)
            return;
        QStringList lines;
        const QList<QListWidgetItem *> selected = m_logListWidget->selectedItems();
        lines.reserve(selected.size());
        for (QListWidgetItem *item : selected) {
            if (item)
                lines.push_back(item->text());
        }
        if (lines.isEmpty()) {
            if (QListWidgetItem *current = m_logListWidget->currentItem())
                lines.push_back(current->text());
        }
        if (lines.isEmpty())
            return;

        const QString text = lines.join(QLatin1Char('\n'));
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(text, QClipboard::Clipboard);
            if (clipboard->supportsSelection())
                clipboard->setText(text, QClipboard::Selection);
        }
        statusBar()->showMessage(tr("Copied %1 log line(s)").arg(lines.size()), 1500);
    };

    auto *copyLogAction = new QAction(tr("Copy"), m_logListWidget);
    copyLogAction->setShortcut(QKeySequence::Copy);
    copyLogAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(copyLogAction, &QAction::triggered, this, copyLogSelectionToClipboard);
    m_logListWidget->addAction(copyLogAction);

    auto *selectAllLogAction = new QAction(tr("Select All"), m_logListWidget);
    selectAllLogAction->setShortcut(QKeySequence::SelectAll);
    selectAllLogAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    connect(selectAllLogAction, &QAction::triggered, m_logListWidget, &QListWidget::selectAll);
    m_logListWidget->addAction(selectAllLogAction);

    auto *copyAllLogAction = new QAction(tr("Copy All"), m_logListWidget);
    connect(copyAllLogAction, &QAction::triggered, this, [this]() {
        if (!m_logListWidget)
            return;
        m_logListWidget->selectAll();
        QStringList lines;
        lines.reserve(m_logListWidget->count());
        for (int i = 0; i < m_logListWidget->count(); ++i) {
            if (QListWidgetItem *item = m_logListWidget->item(i))
                lines.push_back(item->text());
        }
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            const QString text = lines.join(QLatin1Char('\n'));
            clipboard->setText(text, QClipboard::Clipboard);
            if (clipboard->supportsSelection())
                clipboard->setText(text, QClipboard::Selection);
        }
        statusBar()->showMessage(tr("Copied all log lines"), 1500);
    });
    m_logListWidget->addAction(copyAllLogAction);

    m_undoHistoryLaneWidget = new UndoGraphWidget(this);
    m_undoHistoryLaneWidget->setFocusPolicy(Qt::NoFocus);

    m_undoHistoryPreviewPopup = new QLabel(this);
    m_undoHistoryPreviewPopup->setWindowFlags(Qt::ToolTip);
    m_undoHistoryPreviewPopup->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background: rgba(24,24,24,235);"
        "  border: 1px solid rgba(180,180,180,180);"
        "  padding: 2px;"
        "}"));
    m_undoHistoryPreviewPopup->hide();
    m_undoHistoryPreviewTimer = new QTimer(this);
    m_undoHistoryPreviewTimer->setSingleShot(true);

    auto *historyPanel = new QWidget(this);
    auto *historyLayout = new QVBoxLayout(historyPanel);
    historyLayout->setContentsMargins(0, 0, 0, 0);
    historyLayout->setSpacing(4);
    auto *historyTitle = new QLabel(tr("Action History"), historyPanel);
    historyTitle->setStyleSheet(QStringLiteral("color: palette(mid);"));
    historyLayout->addWidget(historyTitle, 0);
    historyLayout->addWidget(m_undoHistoryLaneWidget, 1);

    auto *logSplit = new QSplitter(Qt::Horizontal, this);
    logSplit->setChildrenCollapsible(false);
    logSplit->addWidget(m_logListWidget);
    logSplit->addWidget(historyPanel);
    logSplit->setStretchFactor(0, 3);
    logSplit->setStretchFactor(1, 2);

    m_logDock = new QDockWidget(tr("Log"), this);
    m_logDock->setWidget(logSplit);
    // Hide the dock title bar; visibility is controlled via the status-bar button.
    m_logDock->setTitleBarWidget(new QWidget(m_logDock));
    addDockWidget(Qt::BottomDockWidgetArea, m_logDock);
    // Keep the right column (Layers + Filters) spanning full height.
    // This ensures the bottom Log dock does not extend under the right column.
    setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);

#ifdef MESHLAB2_PYTHON_CONSOLE
    m_pythonConsole = new PythonConsoleWidget(this);
    m_pythonConsoleDock = new QDockWidget(tr("Python Console"), this);
    m_pythonConsoleDock->setWidget(m_pythonConsole);
    // Hide the dock title bar; visibility is controlled via the status-bar button.
    m_pythonConsoleDock->setTitleBarWidget(new QWidget(m_pythonConsoleDock));
    addDockWidget(Qt::BottomDockWidgetArea, m_pythonConsoleDock);
    m_pythonConsoleDock->hide();
#endif

    m_logVerbosity = logVerbosityPreference();
    m_logTimestampMode = logTimestampPreference();
    rebuildLogPanel();

    connect(m_doc, &Document::logCleared, m_logListWidget, &QListWidget::clear);
    connect(m_doc, &Document::logMessageAdded, m_logListWidget,
        [this](const QString &, Document::LogSource, Document::LogLevel level, bool replaceLast) {
            if (!m_logListWidget || level > m_logVerbosity)
                return;
            // Read the entry back rather than rebuilding it from the payload: it carries
            // the timestamp, and it is the row's single source of truth.
            const int index = static_cast<int>(m_doc->logMessages().size()) - 1;
            appendLogItem(m_doc->logMessages()[std::size_t(index)], index, replaceLast);
    });
    // The transient progress line is removed when its operation ends; see
    // Document::clearProgressLog(). Match on the entry index rather than assuming the
    // panel's last item is that line - at a low verbosity it may never have been shown.
    connect(m_doc, &Document::logLastEntryRemoved, m_logListWidget, [this](int index) {
        if (!m_logListWidget || m_logListWidget->count() == 0)
            return;
        QListWidgetItem *last = m_logListWidget->item(m_logListWidget->count() - 1);
        if (last->data(kLogEntryIndexRole).toInt() != index)
            return;
        delete m_logListWidget->takeItem(m_logListWidget->count() - 1);
    });
    connect(&Preferences::instance(), &Preferences::changed, this,
        [this](const QString &id, const QVariant &) {
            if (id == QStringLiteral("document.undoMemoryLimitMiB")) {
                m_doc->setUndoMemoryLimitBytes(
                    qint64(Preferences::instance().intValue(id)) * 1024LL * 1024LL);
                return;
            }
            if (id == QStringLiteral("document.purgeUndoOnMemoryPressure")) {
                m_purgeUndoOnMemoryPressure = Preferences::instance().boolValue(id);
                return;
            }
            if (id != QStringLiteral("log.verbosity") && id != QStringLiteral("log.timestamp"))
                return;
            m_logVerbosity = logVerbosityPreference();
            m_logTimestampMode = logTimestampPreference();
            rebuildLogPanel();
    });

    // Color maps load in a Core singleton that has no Document to write to, so their
    // failures are queued until here. Without this a color map the user dropped in the
    // folder themselves is ignored with no feedback anywhere in the UI.
    for (const ColorMapLoadIssue &issue : ColorMapRegistry::instance().takeLoadIssues()) {
        m_doc->writeLog(
            issue.message,
            Document::LogSource::Application,
            issue.bundled ? Document::LogLevel::Error : Document::LogLevel::Warning);
    }

    refreshUndoHistoryPanel();
    connect(
        m_doc,
        &Document::undoRedoStateChanged,
        this,
        [this](bool, bool, const QString &, const QString &) {
            refreshUndoHistoryPanel();
        });
    // An undo changes mesh contents, and the dynamic parameter bounds resolved from them
    // (a decimation target capped at the face count, a default derived from the bounding
    // box) go stale with it. Every other refresh trigger deliberately bails out mid-restore.
    connect(m_doc, &Document::undoRestoreCompleted, this, &MainWindow::refreshFilterUi);
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::nodeActivated, this, [this](int nodeId, bool withCamera) {
        jumpToUndoNode(nodeId, withCamera);
    });
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::nodeUpdateCameraRequested, this, [this](int nodeId) {
        if (!m_doc || !m_doc->updateUndoNodeCamera(nodeId))
            return;
        // Refresh the thumbnail and snapshot for this node with the current frame.
        RenderWidget *view = currentRenderWidget();
        if (view) {
            const QImage frame = view->grabFramebuffer();
            if (!frame.isNull()) {
                const int fw = frame.width(), fh = frame.height();
                int cropW, cropH;
                if (fw >= fh * 2) { cropH = fh; cropW = fh * 2; }
                else              { cropW = fw; cropH = fw / 2;   }
                const int ox = (fw - cropW) / 2, oy = (fh - cropH) / 2;
                const QSize thumbSize = UndoGraphWidget::thumbnailSize();
                m_undoNodeThumbnails[nodeId] = QPixmap::fromImage(
                    frame.copy(ox, oy, cropW, std::max(1, cropH))
                         .scaled(thumbSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
                m_undoNodeSnapshots[nodeId] = QPixmap::fromImage(
                    frame.scaled(std::max(1, fw / 2), std::max(1, fh / 2),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
        }
        refreshUndoHistoryPanel();
        statusBar()->showMessage(tr("Camera updated for history state %1").arg(nodeId), 1800);
    });
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::nodeMakeRootRequested, this, [this](int nodeId) {
        if (!m_doc)
            return;
        const auto answer = QMessageBox::question(
            this,
            tr("Make Root"),
            tr("This will permanently delete all ancestor states and any branches attached to them.\n"
               "This cannot be undone. Continue?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        m_doc->makeUndoRoot(nodeId);
        m_undoNodeThumbnails.clear();
        m_undoNodeSnapshots.clear();
        refreshUndoHistoryPanel();
        statusBar()->showMessage(tr("History root updated"), 2000);
    });
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::nodeReopenFilterRequested,
        this, &MainWindow::reopenFilterFromUndoNode);
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::nodePurgeBranchRequested, this, [this](int nodeId) {
        if (!m_doc)
            return;
        const auto answer = QMessageBox::question(
            this,
            tr("Purge Branch"),
            tr("This will permanently delete all states derived from this one.\n"
               "This cannot be undone. Continue?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        m_doc->purgeUndoBranch(nodeId);
        m_undoNodeThumbnails.clear();
        m_undoNodeSnapshots.clear();
        refreshUndoHistoryPanel();
        statusBar()->showMessage(tr("Branch purged"), 2000);
    });
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::linearizeHistoryRequested, this, [this]() {
        if (!m_doc)
            return;
        const auto answer = QMessageBox::question(
            this,
            tr("Linearize History"),
            tr("This will permanently delete all branches, keeping only the linear path "
               "from the root to the current state.\n"
               "This cannot be undone. Continue?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
        m_doc->linearizeUndoHistory();
        m_undoNodeThumbnails.clear();
        m_undoNodeSnapshots.clear();
        refreshUndoHistoryPanel();
        statusBar()->showMessage(tr("History linearized"), 2000);
    });
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::generatePythonScriptRequested, this, [this]() {
        if (!m_doc) return;

        QMessageBox exportModeBox(this);
        exportModeBox.setIcon(QMessageBox::Question);
        exportModeBox.setWindowTitle(tr("Generate Python Script"));
        exportModeBox.setText(tr("Choose how filter parameters should be written."));
        exportModeBox.setInformativeText(tr("Full keeps every recorded parameter for exact replay. Compact omits parameters that match the current descriptor defaults."));
        QPushButton *compactButton = exportModeBox.addButton(tr("Compact"), QMessageBox::AcceptRole);
        QPushButton *fullButton = exportModeBox.addButton(tr("Full"), QMessageBox::AcceptRole);
        exportModeBox.addButton(QMessageBox::Cancel);
        exportModeBox.setDefaultButton(compactButton);
        exportModeBox.exec();
        if (exportModeBox.clickedButton() != compactButton
            && exportModeBox.clickedButton() != fullButton) {
            return;
        }
        const bool includeDefaultParameters =
            (exportModeBox.clickedButton() == fullButton);

        // Collect file paths for common prefix detection
        QStringList allPaths;
        for (const auto &info : m_doc->undoTreeInfo()) {
            for (const ScriptAction &sa : m_doc->undoNodeScriptActions(info.nodeId)) {
                if (!sa.filePaths.isEmpty())
                    allPaths.append(sa.filePaths);
            }
        }
        QString commonDir;
        if (!allPaths.isEmpty()) {
            commonDir = allPaths.first();
            for (const QString &p : allPaths) {
                int i = 0;
                while (i < commonDir.length() && i < p.length()
                       && commonDir[i] == p[i]) ++i;
                commonDir.truncate(i);
            }
            int lastSep = commonDir.lastIndexOf(QLatin1Char('/'));
            if (lastSep >= 0)
                commonDir.truncate(lastSep);
        }

        QStringList lines;
        lines << QStringLiteral("# Uses the embedded MeshLab scripting session.");
        lines << QStringLiteral("# `ms` already refers to the current document MeshSet.");
        lines << QStringLiteral("");

        if (!commonDir.isEmpty()) {
            lines << QStringLiteral("# import os");
            lines << QStringLiteral("# os.chdir(%1)").arg(QString::fromUtf8(
                QJsonDocument(QJsonArray{commonDir}).toJson(QJsonDocument::Compact)));
            lines << QStringLiteral("");
        }

        // Walk current path from root to current
        const auto tree = m_doc->undoTreeInfo();
        QMap<int, int> depthMap;
        for (const auto &info : tree)
            depthMap[info.nodeId] = info.depth;
        struct PathNode { int nodeId; int depth; };
        std::vector<PathNode> path;
        for (const auto &info : tree) {
            if (info.isOnCurrentPath)
                path.push_back({info.nodeId, depthMap[info.nodeId]});
        }
        std::sort(path.begin(), path.end(),
                  [](const PathNode &a, const PathNode &b) { return a.depth < b.depth; });

        int exportedCurrentMeshIndex = -1;
        int exportedCurrentRasterIndex = -1;

        for (const auto &pn : path) {
            for (const ScriptAction &sa : m_doc->undoNodeScriptActions(pn.nodeId)) {
                if (sa.kind == QStringLiteral("filter")) {
                    if (sa.currentMeshIndex >= 0 && sa.currentMeshIndex != exportedCurrentMeshIndex) {
                        lines << QStringLiteral("ms.set_current_mesh(%1)").arg(sa.currentMeshIndex);
                        exportedCurrentMeshIndex = sa.currentMeshIndex;
                    }
                    if (sa.currentRasterIndex >= 0 && sa.currentRasterIndex != exportedCurrentRasterIndex) {
                        lines << QStringLiteral("ms.set_current_raster(%1)").arg(sa.currentRasterIndex);
                        exportedCurrentRasterIndex = sa.currentRasterIndex;
                    }
                    const QString recordedCall = includeDefaultParameters
                        ? sa.pythonCall.trimmed()
                        : sa.compactPythonCall.trimmed();
                    if (!recordedCall.isEmpty()) {
                        lines << recordedCall;
                        continue;
                    }
                    const MeshFilterDescriptor *desc = nullptr;
                    for (const auto &fi : m_doc->filterInfos()) {
                        if (fi.key == sa.filterKey) {
                            desc = &fi.descriptor;
                            break;
                        }
                    }
                    if (desc)
                        lines << filterCallToPython(*desc, sa.params, includeDefaultParameters);
                    else
                        lines << QStringLiteral("# unknown filter: %1").arg(sa.filterKey);
                } else if (sa.kind == QStringLiteral("load_mesh")) {
                    for (const QString &fp : sa.filePaths) {
                        QString rel = commonDir.isEmpty() ? fp : QDir(commonDir).relativeFilePath(fp);
                        lines << QStringLiteral("ms.load_new_mesh(\"%1\")").arg(rel.replace('\\', '/'));
                        exportedCurrentMeshIndex = -1;
                    }
                } else if (sa.kind == QStringLiteral("load_raster")) {
                    for (const QString &fp : sa.filePaths) {
                        QString rel = commonDir.isEmpty() ? fp : QDir(commonDir).relativeFilePath(fp);
                        lines << QStringLiteral("ms.load_new_raster(\"%1\")").arg(rel.replace('\\', '/'));
                        exportedCurrentRasterIndex = -1;
                    }
                } else if (sa.kind == QStringLiteral("load_project")) {
                    for (const QString &fp : sa.filePaths) {
                        QString rel = commonDir.isEmpty() ? fp : QDir(commonDir).relativeFilePath(fp);
                        lines << QStringLiteral("ms.load_project(\"%1\")").arg(rel.replace('\\', '/'));
                        exportedCurrentMeshIndex = -1;
                        exportedCurrentRasterIndex = -1;
                    }
                }
            }
        }
#ifdef MESHLAB2_PYTHON_CONSOLE
        if (m_pythonConsoleDock)
            m_pythonConsoleDock->show();
        if (m_pythonConsole)
            m_pythonConsole->setScriptText(lines.join(QStringLiteral("\n")));
        statusBar()->showMessage(tr("Python history script loaded into the script editor"), 4000);
#else
        Q_UNUSED(lines);
#endif
    });
    const auto showUndoHistoryPreview = [this](int nodeId, const QPoint &globalPos) {
        if (!m_undoHistoryPreviewPopup)
            return;
        // Use the 50%-size snapshot for the hover popup; fall back to the thumbnail.
        const QPixmap src = m_undoNodeSnapshots.contains(nodeId)
            ? m_undoNodeSnapshots.value(nodeId)
            : m_undoNodeThumbnails.value(nodeId);
        if (src.isNull()) return;
        QScreen *screenForPopup = this->screen();
        if (QScreen *cursorScreen = QGuiApplication::screenAt(globalPos))
            screenForPopup = cursorScreen;
        if (!screenForPopup && !QGuiApplication::screens().isEmpty())
            screenForPopup = QGuiApplication::screens().front();
        if (!screenForPopup) return;
        const QRect avail = screenForPopup->availableGeometry();
        // Show the snapshot as-is (already at 50% of the original frame);
        // only downscale further if it would not fit on screen.
        const QSize maxSize(std::max(64, avail.width() * 2 / 3), std::max(64, avail.height() * 2 / 3));
        const QPixmap display = (src.width() > maxSize.width() || src.height() > maxSize.height())
            ? src.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            : src;
        if (display.isNull()) return;
        m_undoHistoryPreviewPopup->setPixmap(display);
        m_undoHistoryPreviewPopup->adjustSize();
        const QRect historyRect = m_undoHistoryLaneWidget
            ? QRect(m_undoHistoryLaneWidget->mapToGlobal(QPoint(0, 0)), m_undoHistoryLaneWidget->size())
            : QRect(globalPos, QSize(1, 1));
        QPoint target;
        const int gap = 12;
        const bool fitsLeft = historyRect.left() - gap - m_undoHistoryPreviewPopup->width() >= avail.left();
        const bool fitsAbove = historyRect.top() - gap - m_undoHistoryPreviewPopup->height() >= avail.top();
        if (fitsLeft) {
            target = QPoint(
                historyRect.left() - gap - m_undoHistoryPreviewPopup->width(),
                historyRect.top());
        } else if (fitsAbove) {
            target = QPoint(
                historyRect.right() - m_undoHistoryPreviewPopup->width(),
                historyRect.top() - gap - m_undoHistoryPreviewPopup->height());
        } else {
            target = QPoint(
                historyRect.left(),
                historyRect.bottom() + gap);
        }
        const int maxX = avail.right() - m_undoHistoryPreviewPopup->width();
        const int maxY = avail.bottom() - m_undoHistoryPreviewPopup->height();
        if (maxX >= avail.left()) target.setX(std::clamp(target.x(), avail.left(), maxX));
        if (maxY >= avail.top())  target.setY(std::clamp(target.y(), avail.top(),  maxY));
        m_undoHistoryPreviewPopup->move(target);
        m_undoHistoryPreviewPopup->show();
        m_undoHistoryPreviewPopup->raise();
    };
    connect(m_undoHistoryPreviewTimer, &QTimer::timeout, this, [this, showUndoHistoryPreview]() {
        if (m_pendingUndoHistoryPreviewNodeId < 0)
            return;
        showUndoHistoryPreview(
            m_pendingUndoHistoryPreviewNodeId,
            m_pendingUndoHistoryPreviewGlobalPos);
    });
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::nodeHovered, this, [this, showUndoHistoryPreview](int nodeId, const QPoint &globalPos) {
        m_pendingUndoHistoryPreviewNodeId = nodeId;
        m_pendingUndoHistoryPreviewGlobalPos = globalPos;
        if (m_undoHistoryPreviewPopup && m_undoHistoryPreviewPopup->isVisible()) {
            showUndoHistoryPreview(nodeId, globalPos);
            return;
        }
        if (m_undoHistoryPreviewTimer)
            m_undoHistoryPreviewTimer->start(150);
    });
    connect(m_undoHistoryLaneWidget, &UndoGraphWidget::nodeUnhovered, this, [this]() {
        m_pendingUndoHistoryPreviewNodeId = -1;
        if (m_undoHistoryPreviewTimer)
            m_undoHistoryPreviewTimer->stop();
        if (m_undoHistoryPreviewPopup) m_undoHistoryPreviewPopup->hide();
    });

    connect(m_doc, &Document::loadProgressStarted, this, [this](const QString &filePath) {
        if (!m_loadProgressBar)
            return;
        m_loadProgressBar->setValue(0);
        m_loadProgressBar->setVisible(true);
        m_loadProgressBar->setToolTip(filePath);
        statusBar()->showMessage(tr("Loading %1...").arg(QFileInfo(filePath).fileName()));
    });
    connect(m_doc, &Document::loadProgressUpdated, this, [this](int percent, const QString &message) {
        if (!m_loadProgressBar)
            return;
        m_loadProgressBar->setVisible(true);
        m_loadProgressBar->setValue(std::clamp(percent, 0, 100));
        if (!message.isEmpty())
            statusBar()->showMessage(tr("Loading: %1% - %2").arg(percent).arg(message));
        else
            statusBar()->showMessage(tr("Loading: %1%").arg(percent));
    });
    connect(m_doc, &Document::loadProgressFinished, this, [this](bool success, const QString &message) {
        if (m_loadProgressBar)
            m_loadProgressBar->setVisible(false);
        statusBar()->showMessage(message.isEmpty()
                ? (success ? tr("Loading completed") : tr("Loading failed"))
                : message,
            success ? 2500 : 4000);
    });
    connect(m_doc, &Document::filterProgressStarted, this, [this](const QString &label) {
        setInteractionBlocked(true);
        if (m_filterProgressBar) {
            m_filterProgressBar->setValue(0);
            m_filterProgressBar->setVisible(true);
            m_filterProgressBar->setToolTip(label);
        }
        if (m_filterCancelButton) {
            m_filterCancelButton->setEnabled(true);
            m_filterCancelButton->setVisible(true);
        }
        statusBar()->showMessage(tr("Running %1...").arg(label));
    });
    connect(m_doc, &Document::filterProgressUpdated, this, [this](int percent, const QString &message) {
        if (m_filterProgressBar) {
            m_filterProgressBar->setVisible(true);
            m_filterProgressBar->setValue(std::clamp(percent, 0, 100));
        }
        if (!message.isEmpty())
            statusBar()->showMessage(tr("Filter: %1% - %2").arg(percent).arg(message));
        else
            statusBar()->showMessage(tr("Filter: %1%").arg(percent));
    });
    connect(m_doc, &Document::filterProgressFinished, this, [this](bool success, const QString &message) {
        setInteractionBlocked(false);
        if (m_filterProgressBar)
            m_filterProgressBar->setVisible(false);
        if (m_filterCancelButton)
            m_filterCancelButton->setVisible(false);
        if (m_closeWhenFilterStops) {
            // Queued, so the close runs from the main event loop rather than from
            // whatever nested pump delivered this.
            m_closeWhenFilterStops = false;
            QTimer::singleShot(0, this, [this] { close(); });
        }
        statusBar()->showMessage(
            message.isEmpty() ? (success ? tr("Filter completed") : tr("Filter failed")) : message,
            success ? 2500 : 4000);
    });
    connect(m_doc, &Document::meshVisibilityChanged, this, [this](int index, bool visible) {
        if (m_syncingVisibilityProxy)
            return;
        RenderWidget *view = currentRenderWidget();
        if (!view)
            return;
        view->setMeshVisible(index, visible);
    });

#ifdef MESHLAB2_PYTHON_CONSOLE
    // Initialize the embedded Python interpreter.  PyImport_AppendInittab was
    // already called from main() before QApplication was created.
    PythonHost::instance().initialize(m_doc, m_currentRenderWidget);
#endif

    QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(tr("&New"), QKeySequence::New, this, &MainWindow::newDocument);
    fileMenu->addAction(
        tr("New &Instance"),
        QKeySequence(QStringLiteral("Ctrl+Shift+N")),
        this,
        &MainWindow::newInstance);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::openFile);
    fileMenu->addAction(tr("Open &Raster Image..."), this, &MainWindow::openRasterImage);
    fileMenu->addAction(tr("Reload &Current Mesh"), this, &MainWindow::reloadCurrentMesh);
    fileMenu->addAction(tr("Reload &All Meshes"), this, &MainWindow::reloadAllMeshes);
    fileMenu->addSeparator();
    fileMenu->addAction(tr("&Save Mesh..."), QKeySequence::Save, this, &MainWindow::saveCurrentMesh);
    fileMenu->addAction(
        tr("Save Project &As..."),
        QKeySequence(QStringLiteral("Ctrl+Shift+P")),
        this,
        &MainWindow::saveProjectAs);
    fileMenu->addAction(
        tr("S&napshot PNG..."),
        QKeySequence(QStringLiteral("Ctrl+Shift+S")),
        this,
        &MainWindow::saveSnapshotPng);
    fileMenu->addAction(tr("Add Snapshot &Raster"), this, &MainWindow::addSnapshotRaster);
    m_openLastAction = fileMenu->addAction(tr("Open &Last Mesh"), this, &MainWindow::openLastMesh);
    m_openLastAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+L")));
    m_recentMenu = fileMenu->addMenu(tr("Open &Recent"));
    fileMenu->addSeparator();
    fileMenu->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu(tr("&Edit"));
    m_undoAction = editMenu->addAction(tr("&Undo"), this, &MainWindow::undo);
    m_undoAction->setShortcut(QKeySequence::Undo);
    m_redoAction = editMenu->addAction(tr("&Redo"), this, &MainWindow::redo);
    m_redoAction->setShortcut(QKeySequence::Redo);
    connect(m_doc, &Document::undoRedoStateChanged, this,
            [this](bool canUndo, bool canRedo, const QString &undoText, const QString &redoText) {
        if (m_undoAction) {
            m_undoAction->setEnabled(canUndo);
            m_undoAction->setText(
                canUndo ? tr("&Undo %1").arg(undoText) : tr("&Undo"));
        }
        if (m_redoAction) {
            m_redoAction->setEnabled(canRedo);
            m_redoAction->setText(
                canRedo ? tr("&Redo %1").arg(redoText) : tr("&Redo"));
        }
    });
    if (m_undoAction) {
        m_undoAction->setEnabled(m_doc->canUndo());
        m_undoAction->setText(
            m_doc->canUndo() ? tr("&Undo %1").arg(m_doc->undoText()) : tr("&Undo"));
    }
    if (m_redoAction) {
        m_redoAction->setEnabled(m_doc->canRedo());
        m_redoAction->setText(
            m_doc->canRedo() ? tr("&Redo %1").arg(m_doc->redoText()) : tr("&Redo"));
    }

    editMenu->addSeparator();
    QAction *preferencesAction = editMenu->addAction(tr("&Preferences..."), this, [this]() {
        PreferencesDialog dialog(this);
        dialog.exec();
    });
    // QKeySequence::Preferences maps to Cmd+, on macOS, which also lets the platform
    // move the entry into the application menu where users expect it.
    preferencesAction->setShortcut(QKeySequence::Preferences);
    preferencesAction->setMenuRole(QAction::PreferencesRole);

    m_filtersMenu = menuBar()->addMenu(tr("&Filters"));
    refreshFiltersMenu();
    if (m_filterPanel) {
        connect(m_filterPanel, &MeshFilterPanel::runRequested, this,
                [this](const QString &filterKey, const MeshFilterParameterValues &params, const QString &label) {
            executeFilter(filterKey, label, params);
        });
#ifdef MESHLAB2_PYTHON_CONSOLE
        connect(m_filterPanel, &MeshFilterPanel::copyToConsoleRequested, this,
                [this](const QString &code) {
            if (m_terminalButton && !m_terminalButton->isChecked())
                m_terminalButton->setChecked(true);
            if (m_pythonConsole)
                m_pythonConsole->setInputText(code);
        });
#endif
    }
    connect(m_doc, &Document::meshAdded, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        scheduleFilterUiRefresh();
    });
    connect(m_doc, &Document::meshRemoved, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        scheduleFilterUiRefresh();
    });
    connect(m_doc, &Document::currentMeshChanged, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        scheduleFilterUiRefresh();
    });
    connect(m_doc, &Document::meshDataChanged, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        scheduleFilterUiRefresh();
    });
    connect(m_doc, &Document::rasterAdded, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        refreshFiltersMenu();
    });
    connect(m_doc, &Document::rasterRemoved, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        refreshFiltersMenu();
    });
    connect(m_doc, &Document::currentRasterChanged, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        refreshFiltersMenu();
    });
    connect(m_doc, &Document::rasterDataChanged, this, [this](int) {
        if (m_doc->isRestoringUndoRedo()) return;
        refreshFiltersMenu();
    });

    QMenu *viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(tr("3D Scene Mode"), this, &MainWindow::setCurrentViewSceneMode);
    viewMenu->addAction(
        tr("Parametrization (UV) Mode"),
        this,
        &MainWindow::setCurrentViewParametrizationMode);
    viewMenu->addAction(tr("Raster Mode"), this, &MainWindow::setCurrentViewRasterMode);
    viewMenu->addSeparator();
    m_layerGridAction = viewMenu->addAction(
        tr("Arrange Layers in a Grid"),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G),
        this,
        &MainWindow::toggleCurrentViewLayerGrid);
    m_layerGridAction->setCheckable(true);
    m_layerGridAction->setShortcutContext(Qt::WindowShortcut);
    connect(viewMenu, &QMenu::aboutToShow, this, [this] {
        const RenderWidget *view = currentRenderWidget();
        m_layerGridAction->setChecked(
            view
            && view->renderSettings().layerArrangement == LayerArrangement::Grid);
    });
    viewMenu->addSeparator();
    viewMenu->addAction(tr("Split Horizontally"), this, &MainWindow::splitViewHorizontally);
    viewMenu->addAction(tr("Split Vertically"), this, &MainWindow::splitViewVertically);
    viewMenu->addSeparator();
    viewMenu->addAction(
        tr("Reset Camera"),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_H),
        this,
        &MainWindow::resetCamera);
    viewMenu->addAction(
        tr("Center on Selection"),
        QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_J),
        this,
        &MainWindow::centerCameraOnSelection);
    viewMenu->addSeparator();
    // Ctrl+C and Ctrl+V here are scoped to the 3D views rather than the whole window.
    // As window shortcuts they took the keys away from every other panel -- the log, the
    // Python console, the script editor -- so copy meant "camera JSON" wherever you were
    // typing, and paste overwrote the camera from whatever the clipboard held. Widget scope
    // hands them to whichever panel has focus. The menu entries still fire from the menu
    // whatever is focused, so the commands remain reachable with no view active.
    m_copyCameraAction = viewMenu->addAction(
        tr("Copy Camera/Trackball JSON"),
        QKeySequence::Copy,
        this,
        &MainWindow::copyCameraState);
    m_copyCameraAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    m_pasteCameraAction = viewMenu->addAction(
        tr("Paste Camera/Trackball JSON"),
        QKeySequence::Paste,
        this,
        &MainWindow::pasteCameraState);
    m_pasteCameraAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    // The first view is built before the menus, so it is picked up here; later splits get
    // them from createRenderWidget().
    for (RenderWidget *view : m_renderWidgets)
        attachViewShortcuts(view);

#ifdef MESHLAB2_PYTHON_CONSOLE
    viewMenu->addSeparator();
    if (m_pythonConsoleDock) {
        QAction *consoleAction = m_pythonConsoleDock->toggleViewAction();
        consoleAction->setText(tr("Python Console"));
        consoleAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Backslash));
        viewMenu->addAction(consoleAction);
    }
#endif

    setupToolsMenu(menuBar()->addMenu(tr("&Tools")));

    QMenu *helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(tr("&Filter Plugins..."), this, &MainWindow::showFilterPlugins);
    helpMenu->addAction(tr("I/O &Plugins..."), this, &MainWindow::showImportPlugins);
    helpMenu->addAction(tr("&Memory Info..."), this, &MainWindow::showMemoryInfo);
    helpMenu->addSeparator();
    helpMenu->addAction(
        tr("On screen &quick help"),
        QKeySequence(Qt::Key_F1),
        this,
        [this]() {
            if (RenderWidget *view = currentRenderWidget())
                view->toggleHelpOverlayVisible();
        });
    helpMenu->addSeparator();
    helpMenu->addAction(tr("&About"), this, &MainWindow::showAbout);

    // Ctrl+F steps aside while something editable has focus.
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        updateTextEditingShortcuts(now);
    });

    QSettings settings;
    m_recentMeshes = settings.value(QStringLiteral("recentMeshes")).toStringList();
    sanitizeRecentMeshes();
    refreshRecentMeshesMenu();
}

// Ctrl+F opens the Filter Browser from anywhere in the application, which means it also
// fires while you are typing in the script editor or at the console prompt -- jumping you
// out of the text you were writing.
//
// It is the only one of our standard-key shortcuts that needs help. Qt lets a focused widget
// claim a key ahead of a shortcut through QEvent::ShortcutOverride, and the text widgets do
// exactly that for the keys they implement, so Ctrl+C, Ctrl+V and Ctrl+Z already reach an
// editable QPlainTextEdit or QLineEdit untouched. Measured, per widget under focus:
//
//              QPlainTextEdit  read-only  QLineEdit  QListWidget
//   Ctrl+C     widget          SHORTCUT   widget     SHORTCUT
//   Ctrl+V     widget          SHORTCUT   widget     SHORTCUT
//   Ctrl+Z     widget          SHORTCUT   widget     SHORTCUT
//   Ctrl+F     SHORTCUT        SHORTCUT   SHORTCUT   SHORTCUT
//
// Nothing implements find, so Ctrl+F is claimed by nobody and the shortcut always wins. The
// read-only and list columns are why the camera keys had to move to the views: there is no
// text control there to claim them. Undo needs nothing -- where the shortcut does win there
// is no typing to undo, so document undo is the right answer.
//
// Qt resolves a shortcut before the key reaches the widget, so what steps aside is the
// binding rather than the action: the menu entry stays enabled and clickable throughout.
void MainWindow::updateTextEditingShortcuts(QWidget *focused)
{
    const auto editable = [](const QWidget *w) {
        if (auto *line = qobject_cast<const QLineEdit *>(w))
            return !line->isReadOnly();
        if (auto *plain = qobject_cast<const QPlainTextEdit *>(w))
            return !plain->isReadOnly();
        if (auto *rich = qobject_cast<const QTextEdit *>(w))
            return !rich->isReadOnly();
        return false;
    };

    const bool editing = focused && editable(focused);
    if (editing == m_textEditingHasFocus)
        return;
    m_textEditingHasFocus = editing;
    if (m_filterBrowserAction)
        m_filterBrowserAction->setShortcut(
            editing ? QKeySequence() : QKeySequence(QKeySequence::Find));
}


RenderWidget *MainWindow::currentRenderWidget() const
{
    if (m_currentRenderWidget)
        return m_currentRenderWidget;
    return m_renderWidgets.isEmpty() ? nullptr : m_renderWidgets.first();
}

// The view-scoped shortcuts every 3D view carries. Actions are owned by the View menu, so
// a view being closed does not take them with it.
void MainWindow::attachViewShortcuts(RenderWidget *view)
{
    if (!view)
        return;
    if (m_copyCameraAction)
        view->addAction(m_copyCameraAction);
    if (m_pasteCameraAction)
        view->addAction(m_pasteCameraAction);
}

RenderWidget *MainWindow::createRenderWidget(QSplitter *parentSplitter)
{
    if (!parentSplitter)
        return nullptr;

    auto *view = new RenderWidget(m_doc, parentSplitter);
    view->setAttribute(Qt::WA_StyledBackground, true);
    view->setAcceptDrops(true);
    view->installEventFilter(this);
    view->setContextMenuPolicy(Qt::CustomContextMenu);
    parentSplitter->addWidget(view);
    m_renderWidgets.append(view);
    attachViewShortcuts(view);

    connect(view, &RenderWidget::viewActivated, this, [this](RenderWidget *activatedView) {
        setCurrentRenderWidget(activatedView);
    });
    connect(view, &RenderWidget::toolExitRequested, this, &MainWindow::exitActiveTool);
    connect(view, &RenderWidget::cameraStateChanged, this, [this](RenderWidget *sourceView) {
        for (RenderWidget *peer : m_renderWidgets) {
            if (peer != sourceView && peer->showViewFrustumsEnabled())
                peer->update();
        }
        syncCameraViewsFrom(sourceView);
    });
    connect(view, &RenderWidget::frameRendered, this,
            [this, view](float cpuMs, float gpuMs, bool gpuTimingSupported, bool gpuSampleValid) {
        if (view != currentRenderWidget())
            return;
        updateFrameTimeStats(cpuMs, gpuMs, gpuTimingSupported, gpuSampleValid);
    });
    connect(view, &RenderWidget::trackballCenterPicked, this,
            [this, view](const QVector3D &worldPos) {
        setCurrentRenderWidget(view);
        const QString msg = tr("Trackball center: (%1, %2, %3)")
            .arg(worldPos.x(), 0, 'f', 6)
            .arg(worldPos.y(), 0, 'f', 6)
            .arg(worldPos.z(), 0, 'f', 6);
        statusBar()->showMessage(msg, 3500);
        m_doc->writeLog(msg, Document::LogSource::Application);
    });
    connect(view, &QWidget::customContextMenuRequested, this,
            [this, view](const QPoint &pos) {
        setCurrentRenderWidget(view);

        QMenu menu(view);
        QAction *sceneModeAction = menu.addAction(tr("3D Scene Mode"));
        QAction *uvModeAction = menu.addAction(tr("Parametrization (UV) Mode"));
        QAction *rasterModeAction = menu.addAction(tr("Raster Mode"));
        uvModeAction->setEnabled(view->canSwitchToViewMode(RenderWidget::ViewMode::ParametrizationUV));
        rasterModeAction->setEnabled(view->canSwitchToViewMode(RenderWidget::ViewMode::RasterImage));
        menu.addSeparator();
        QAction *layerGridAction = menu.addAction(tr("Arrange Layers in a Grid"));
        layerGridAction->setCheckable(true);
        layerGridAction->setChecked(
            view->renderSettings().layerArrangement == LayerArrangement::Grid);
        layerGridAction->setEnabled(view->viewMode() == RenderWidget::ViewMode::Scene3D);
        menu.addSeparator();
        QAction *syncCameraAction = menu.addAction(tr("Synchronize Camera"));
        syncCameraAction->setCheckable(true);
        syncCameraAction->setChecked(m_cameraSyncEnabled);
        syncCameraAction->setEnabled(m_renderWidgets.size() > 1);
        menu.addSeparator();
        QAction *splitHAction =
            menu.addAction(QIcon(QStringLiteral(":/img/splitV.png")), tr("Split Horizontally"));
        QAction *splitVAction =
            menu.addAction(QIcon(QStringLiteral(":/img/splitH.png")), tr("Split Vertically"));
        menu.addSeparator();
        QAction *closeAction = menu.addAction(tr("Close View"));
        closeAction->setEnabled(m_renderWidgets.size() > 1);

        QAction *chosen = menu.exec(view->mapToGlobal(pos));
        if (!chosen)
            return;
        if (chosen == sceneModeAction) {
            setCurrentViewSceneMode();
        } else if (chosen == uvModeAction) {
            setCurrentViewParametrizationMode();
        } else if (chosen == rasterModeAction) {
            setCurrentViewRasterMode();
        } else if (chosen == layerGridAction) {
            toggleCurrentViewLayerGrid();
        } else if (chosen == syncCameraAction) {
            m_cameraSyncEnabled = syncCameraAction->isChecked();
            if (m_cameraSyncEnabled)
                syncCameraViewsFrom(view);
        } else if (chosen == splitHAction) {
            splitViewHorizontally();
        } else if (chosen == splitVAction) {
            splitViewVertically();
        } else if (chosen == closeAction) {
            closeCurrentView();
        }
    });

    view->setPeerViewCameraProvider([this, view]() -> std::vector<PeerViewCamera> {
        std::vector<PeerViewCamera> result;
        for (const RenderWidget *peer : m_renderWidgets) {
            if (peer == view) continue;
            if (peer->viewMode() != RenderWidget::ViewMode::Scene3D) continue;
            PeerViewCamera pvc;
            pvc.view = peer->viewMatrix();
            pvc.proj = peer->projectionMatrix();
            pvc.viewportSize = peer->size();
            pvc.nearDist = peer->nearClipDistance();
            pvc.farDist = peer->farClipDistance();
            result.push_back(pvc);
        }
        return result;
    });

    return view;
}

void MainWindow::setCurrentRenderWidget(RenderWidget *view)
{
    if (!view)
        return;
    if (m_currentRenderWidget == view)
        return;
    m_currentRenderWidget = view;
    updateCurrentViewBorder();
    syncDocumentVisibilityFromCurrentView();
    // The tool stays pinned to its owner view; it's only live while that view is
    // the current one, so other views remain plain trackball inspectors.
    if (m_toolOwnerView)
        m_toolOwnerView->setToolOwnerIsCurrent(m_toolOwnerView == m_currentRenderWidget);
}

void MainWindow::setupToolsMenu(QMenu *toolsMenu)
{
    m_interactiveTools = createBuiltinInteractiveTools();
    auto *group = new QActionGroup(this);
    // Optional exclusivity: at most one tool checked, and it can be toggled off.
    group->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);

    auto *toolBar = new QToolBar(tr("Edit Tools"), this);
    toolBar->setObjectName(QStringLiteral("EditToolsToolBar"));
    toolBar->setAllowedAreas(Qt::LeftToolBarArea | Qt::RightToolBarArea);
    addToolBar(Qt::LeftToolBarArea, toolBar);

    for (int i = 0; i < static_cast<int>(m_interactiveTools.size()); ++i) {
        const InteractiveTool &tool = *m_interactiveTools[size_t(i)];
        QAction *action = new QAction(tool.name(), this);
        action->setCheckable(true);
        if (!tool.iconPath().isEmpty())
            action->setIcon(QIcon(tool.iconPath()));
        action->setToolTip(tool.statusHint().isEmpty() ? tool.name() : tool.statusHint());
        group->addAction(action);
        m_toolActions.append(action);
        toolsMenu->addAction(action); // menu and toolbar share the same QAction
        toolBar->addAction(action);
        connect(action, &QAction::triggered, this, [this, i](bool checked) {
            setActiveToolIndex(checked ? i : -1);
        });
    }

    // Snapshot lives here rather than on the horizontal bar, which is entirely rendering
    // state: this does something rather than changing how the view looks. It is not one of
    // the tools above either -- nothing is entered or exited, so it is not checkable and
    // stays out of their exclusive group -- hence the separator.
    toolBar->addSeparator();
    auto *snapshotAction = new QAction(QIcon(QStringLiteral(":/img/snapshot.png")),
                                       tr("Snapshot PNG"), this);
    snapshotAction->setToolTip(tr("Save a PNG of the current view (Ctrl+Shift+S)"));
    connect(snapshotAction, &QAction::triggered, this, &MainWindow::saveSnapshotPng);
    toolBar->addAction(snapshotAction);
}

void MainWindow::exitActiveTool()
{
    for (QAction *action : std::as_const(m_toolActions))
        action->setChecked(false);
    setActiveToolIndex(-1);
}

void MainWindow::setActiveToolIndex(int index)
{
    InteractiveTool *tool = (index >= 0 && index < static_cast<int>(m_interactiveTools.size()))
        ? m_interactiveTools[size_t(index)].get()
        : nullptr;

    // Detach from the previous owner (cancels any gesture there).
    if (m_toolOwnerView)
        m_toolOwnerView->setActiveTool(nullptr);
    m_toolOwnerView = nullptr;
    m_activeToolIndex = -1;

    // Pin the tool to the view that is current at activation time. It stays there
    // until exited; focusing another view only suspends it (see setCurrentRenderWidget).
    if (tool) {
        RenderWidget *owner = currentRenderWidget();
        if (owner) {
            m_toolOwnerView = owner;
            m_activeToolIndex = index;
            owner->setActiveTool(tool);
            owner->setToolOwnerIsCurrent(true);
        }
    }
}

void MainWindow::updateCurrentViewBorder()
{
    const bool showCurrentViewIndicator = (m_renderWidgets.size() > 1);
    for (RenderWidget *view : std::as_const(m_renderWidgets)) {
        if (!view)
            continue;
        const bool isCurrent = (view == m_currentRenderWidget);
        view->setCurrentViewHighlighted(showCurrentViewIndicator && isCurrent);
        view->update();
    }
}

void MainWindow::syncDocumentVisibilityFromCurrentView()
{
    RenderWidget *view = currentRenderWidget();
    if (!view || !m_doc)
        return;

    m_syncingVisibilityProxy = true;
    for (int i = 0; i < m_doc->meshCount(); ++i)
        m_doc->setMeshVisible(i, view->meshVisible(i));
    m_syncingVisibilityProxy = false;
}

void MainWindow::syncCameraViewsFrom(RenderWidget *sourceView)
{
    if (!m_cameraSyncEnabled || m_syncingCameraViews || !sourceView)
        return;
    if (m_renderWidgets.size() < 2)
        return;
    if (sourceView->viewMode() != RenderWidget::ViewMode::Scene3D)
        return;

    const ViewTrackball::State sourceState = sourceView->trackballState();
    m_syncingCameraViews = true;
    for (RenderWidget *targetView : std::as_const(m_renderWidgets)) {
        if (!targetView || targetView == sourceView)
            continue;
        if (targetView->viewMode() != RenderWidget::ViewMode::Scene3D)
            continue;
        targetView->applySynchronizedTrackballState(sourceState);
    }
    m_syncingCameraViews = false;
}

void MainWindow::splitCurrentView(Qt::Orientation orientation)
{
    RenderWidget *sourceView = currentRenderWidget();
    if (!sourceView)
        return;
    auto *parentSplitter = qobject_cast<QSplitter *>(sourceView->parentWidget());
    if (!parentSplitter)
        return;

    const int sourceIndex = parentSplitter->indexOf(sourceView);
    if (sourceIndex < 0)
        return;

    RenderWidget *newView = nullptr;
    if (parentSplitter->orientation() == orientation) {
        const QList<int> oldSizes = parentSplitter->sizes();
        newView = createRenderWidget(parentSplitter);
        if (!newView)
            return;
        parentSplitter->insertWidget(sourceIndex + 1, newView);

        // Split only the source pane into 50/50, keep all others unchanged.
        QList<int> newSizes = parentSplitter->sizes();
        if (oldSizes.size() == newSizes.size() - 1 && sourceIndex < oldSizes.size()) {
            const int sourceOldSize = qMax(2, oldSizes[sourceIndex]);
            const int firstHalf = sourceOldSize / 2;
            const int secondHalf = sourceOldSize - firstHalf;

            for (int i = 0; i < newSizes.size(); ++i) {
                if (i < sourceIndex) {
                    newSizes[i] = oldSizes[i];
                } else if (i == sourceIndex) {
                    newSizes[i] = firstHalf;
                } else if (i == sourceIndex + 1) {
                    newSizes[i] = secondHalf;
                } else {
                    newSizes[i] = oldSizes[i - 1];
                }
            }
            parentSplitter->setSizes(newSizes);
        }
    } else {
        const QList<int> oldParentSizes = parentSplitter->sizes();

        auto *nestedSplitter = new QSplitter(orientation, parentSplitter);
        nestedSplitter->setChildrenCollapsible(false);

        // Replace source view with a nested splitter in the parent.
        parentSplitter->insertWidget(sourceIndex, nestedSplitter);
        nestedSplitter->addWidget(sourceView);

        newView = createRenderWidget(nestedSplitter);
        if (!newView)
            return;
        nestedSplitter->setSizes(QList<int>{1, 1});

        // Preserve parent splitter allocation so other views are not affected.
        QList<int> parentSizes = parentSplitter->sizes();
        if (parentSizes.size() == oldParentSizes.size()) {
            for (int i = 0; i < parentSizes.size(); ++i)
                parentSizes[i] = oldParentSizes[i];
            parentSplitter->setSizes(parentSizes);
        }
    }

    newView->setRenderSettings(sourceView->renderSettings());
    newView->copyPerMeshRenderModesFrom(sourceView);
    newView->setMeshVisibilityState(sourceView->meshVisibilityState());
    // Both of these used to be collected and dropped. A failure here is invisible in the
    // result -- the new view just shows the default camera, or the 3D mode instead of the
    // one being split -- which reads as "splitting reset my camera" and leaves nothing to
    // go on. Copying the camera also clears the new view's pending frame-the-scene, so a
    // failure here is exactly what would make the split appear to reset it.
    QString viewModeError;
    if (!newView->setViewMode(sourceView->viewMode(), &viewModeError) && !viewModeError.isEmpty()) {
        m_doc->writeLog(tr("Split view: could not carry over the view mode: %1").arg(viewModeError),
                        Document::LogSource::Application, Document::LogLevel::Warning);
    }
    QString cameraError;
    if (!newView->applyCameraStateJson(sourceView->cameraStateJson(), &cameraError)
        && !cameraError.isEmpty()) {
        m_doc->writeLog(tr("Split view: could not carry over the camera: %1").arg(cameraError),
                        Document::LogSource::Application, Document::LogLevel::Warning);
    }

    setCurrentRenderWidget(newView);
    statusBar()->showMessage(tr("Created new view"), 1500);
}

bool MainWindow::closeRenderWidget(RenderWidget *view)
{
    if (!view || m_renderWidgets.size() <= 1)
        return false;

    // If the view hosting the active tool is closing, exit the tool first so the
    // owner pointer and toolbar state don't dangle.
    if (view == m_toolOwnerView)
        exitActiveTool();

    RenderWidget *nextCurrent = m_currentRenderWidget;
    if (nextCurrent == view) {
        nextCurrent = nullptr;
        for (RenderWidget *candidate : std::as_const(m_renderWidgets)) {
            if (candidate != view) {
                nextCurrent = candidate;
                break;
            }
        }
    }

    auto *parentSplitter = qobject_cast<QSplitter *>(view->parentWidget());
    m_renderWidgets.removeAll(view);

    view->setParent(nullptr);
    view->deleteLater();

    ViewSplitterLayout::collapseSingleChildSplitters(parentSplitter, m_viewSplitter);

    if (!nextCurrent && !m_renderWidgets.isEmpty())
        nextCurrent = m_renderWidgets.first();
    if (nextCurrent)
        setCurrentRenderWidget(nextCurrent);
    else
        updateCurrentViewBorder();

    return true;
}

void MainWindow::closeCurrentView()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;
    if (!closeRenderWidget(view)) {
        statusBar()->showMessage(tr("Cannot close the last view"), 1800);
        return;
    }
    statusBar()->showMessage(tr("View closed"), 1500);
}

void MainWindow::toggleCurrentViewLayerGrid()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;
    if (view->viewMode() != RenderWidget::ViewMode::Scene3D) {
        statusBar()->showMessage(
            tr("The grid arrangement applies to the 3D scene view."), 3000);
        return;
    }

    RenderSettings settings = view->renderSettings();
    const bool toGrid = (settings.layerArrangement != LayerArrangement::Grid);
    settings.layerArrangement = toGrid ? LayerArrangement::Grid : LayerArrangement::Overlay;
    view->setRenderSettings(settings);

    // A grid of one tile is the overlay arrangement, so say so rather than leave the user
    // wondering why nothing happened.
    if (toGrid && !view->isLayerGridActive()) {
        statusBar()->showMessage(
            tr("The grid needs at least two visible layers; showing the single one full size."),
            4000);
    }
}

void MainWindow::splitViewHorizontally()
{
    splitCurrentView(Qt::Horizontal);
}

void MainWindow::splitViewVertically()
{
    splitCurrentView(Qt::Vertical);
}

void MainWindow::newDocument()
{
    const bool hasContent = (m_doc->meshCount() > 0)
        || (m_doc->rasterCount() > 0)
        || !m_doc->undoStackLabels().isEmpty();
    if (hasContent) {
        const auto answer = QMessageBox::question(
            this,
            tr("New Document"),
            tr("This will close all layers and clear the undo history.\nContinue?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }
    m_doc->setSuppressUndo(true);
    m_doc->clearAllLayers();
    m_doc->setSuppressUndo(false);
    m_doc->clearUndoHistory();
    m_doc->clearLog();
    statusBar()->showMessage(tr("New document"), 2000);
}

void MainWindow::newInstance()
{
    const QString program = QCoreApplication::applicationFilePath();
    const bool started = QProcess::startDetached(program, QStringList());
    statusBar()->showMessage(
        started ? tr("Started new MeshLab instance")
                : tr("Failed to start a new MeshLab instance"),
        started ? 2000 : 4000);
}

void MainWindow::openFile()
{
    const QStringList fileNames = QFileDialog::getOpenFileNames(
        this,
        tr("Open Mesh"),
        FileDialogDirectory::startingDirectory(QStringLiteral("mesh")),
        m_doc->openDialogFilter());
    if (fileNames.isEmpty())
        return;

    FileDialogDirectory::remember(QStringLiteral("mesh"), fileNames.first());

    const bool groupUndoStep = (fileNames.size() > 1);
    if (groupUndoStep)
        m_doc->beginUndoStep(tr("Open Meshes"));

    int loadedCount = 0;
    QStringList failures;
    for (const QString &fileName : fileNames) {
        // Single file: let loadMeshFromPath put its own message in the status bar, which is
        // where one has always gone. A batch collects instead, so nothing is overwritten.
        QString reason;
        if (loadMeshFromPath(fileName, fileNames.size() > 1 ? &reason : nullptr))
            ++loadedCount;
        else
            failures << tr("%1 -- %2").arg(QFileInfo(fileName).fileName(), reason);
    }
    if (groupUndoStep)
        m_doc->endUndoStep(loadedCount > 0);

    if (fileNames.size() > 1) {
        statusBar()->showMessage(
            tr("Open complete: %1 loaded, %2 failed")
                .arg(loadedCount)
                .arg(failures.size()),
            3500);
    }

    if (failures.isEmpty())
        return;

    // Say which files did not make it and why. A count alone leaves the user to work out
    // which of the names they picked is missing from the layer list.
    for (const QString &failure : failures)
        m_doc->writeLog(tr("Open failed: %1").arg(failure), Document::LogSource::Application,
                        Document::LogLevel::Warning);
    if (fileNames.size() == 1)
        return; // already in the status bar, and one failure is not worth a dialog

    static constexpr int kMaxListed = 12;
    QStringList listed = failures.mid(0, kMaxListed);
    if (failures.size() > kMaxListed)
        listed << tr("... and %1 more (see the log)").arg(failures.size() - kMaxListed);
    QMessageBox::warning(
        this,
        tr("Some files did not open"),
        tr("%1 of %2 files did not open:\n\n%3")
            .arg(failures.size())
            .arg(fileNames.size())
            .arg(listed.join(QStringLiteral("\n"))));
}

void MainWindow::openRasterImage()
{
    const QStringList fileNames = QFileDialog::getOpenFileNames(
        this,
        tr("Open Raster Image"),
        FileDialogDirectory::startingDirectory(QStringLiteral("raster")),
        rasterImageOpenDialogFilter());
    if (fileNames.isEmpty())
        return;

    FileDialogDirectory::remember(QStringLiteral("raster"), fileNames.first());

    const bool groupUndoStep = (fileNames.size() > 1);
    if (groupUndoStep)
        m_doc->beginUndoStep(tr("Open Raster Images"));

    int loadedCount = 0;
    int failedCount = 0;
    for (const QString &fileName : fileNames) {
        if (loadRasterFromPath(fileName))
            ++loadedCount;
        else
            ++failedCount;
    }
    if (groupUndoStep)
        m_doc->endUndoStep(loadedCount > 0);

    if (fileNames.size() > 1) {
        statusBar()->showMessage(
            tr("Open rasters complete: %1 loaded, %2 failed")
                .arg(loadedCount)
                .arg(failedCount),
            3500);
    }
}

bool MainWindow::handleDragEnterOrMove(QDropEvent *event)
{
    // Disabling child widgets does not stop a drop: it is delivered to the window, which
    // stays enabled so its title bar still works.
    if (m_interactionBlocked) {
        if (event)
            event->ignore();
        return false;
    }
    if (!event || !event->mimeData() || !event->mimeData()->hasUrls()) {
        if (event)
            event->ignore();
        return false;
    }

    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (path.trimmed().isEmpty())
            continue;
        if (QFileInfo(path).isFile()) {
            event->setDropAction(Qt::CopyAction);
            event->accept();
            return true;
        }
    }

    event->ignore();
    return false;
}

void MainWindow::handleDroppedUrls(const QList<QUrl> &urls)
{
    QStringList filePaths;
    filePaths.reserve(urls.size());
    QSet<QString> seenPaths;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        const QFileInfo info(path);
        if (!info.isFile())
            continue;
        const QString normalized = normalizeRecentPath(path);
        if (normalized.isEmpty() || seenPaths.contains(normalized))
            continue;
        seenPaths.insert(normalized);
        filePaths.push_back(path);
    }

    if (filePaths.isEmpty()) {
        statusBar()->showMessage(tr("Drop ignored: no local files found"), 2500);
        return;
    }

    const bool groupUndoStep = (filePaths.size() > 1);
    if (groupUndoStep)
        m_doc->beginUndoStep(tr("Drop Layers"));

    int loadedCount = 0;
    int failedCount = 0;
    for (const QString &path : filePaths) {
        const bool loaded = canReadRasterImage(path)
            ? loadRasterFromPath(path)
            : loadMeshFromPath(path);
        if (loaded)
            ++loadedCount;
        else
            ++failedCount;
    }

    if (groupUndoStep)
        m_doc->endUndoStep(loadedCount > 0);

    if (filePaths.size() > 1 || failedCount > 0) {
        statusBar()->showMessage(
            tr("Drop complete: %1 loaded, %2 failed")
                .arg(loadedCount)
                .arg(failedCount),
            failedCount > 0 ? 4500 : 3000);
    }
}

void MainWindow::reloadCurrentMesh()
{
    if (!m_doc || m_doc->meshCount() <= 0) {
        statusBar()->showMessage(tr("No mesh to reload"), 2500);
        return;
    }

    const int currentIndex = m_doc->currentMeshIndex();
    if (currentIndex < 0 || currentIndex >= m_doc->meshCount()) {
        statusBar()->showMessage(tr("No current mesh to reload"), 2500);
        return;
    }

    const QString meshName = m_doc->mesh(currentIndex).name;
    const int err = m_doc->reloadMesh(currentIndex);
    if (err != 0) {
        statusBar()->showMessage(tr("Failed to reload %1").arg(meshName), 3500);
        return;
    }

    statusBar()->showMessage(tr("Reloaded %1").arg(meshName), 2500);
}

void MainWindow::reloadAllMeshes()
{
    if (!m_doc || m_doc->meshCount() <= 0) {
        statusBar()->showMessage(tr("No meshes to reload"), 2500);
        return;
    }

    const int total = m_doc->meshCount();
    m_doc->beginUndoStep(tr("Reload All Meshes"));

    int reloadedCount = 0;
    int failedCount = 0;
    for (int i = 0; i < total; ++i) {
        if (m_doc->reloadMesh(i) == 0)
            ++reloadedCount;
        else
            ++failedCount;
    }

    m_doc->endUndoStep(reloadedCount > 0);
    statusBar()->showMessage(
        tr("Reload complete: %1 reloaded, %2 failed")
            .arg(reloadedCount)
            .arg(failedCount),
        failedCount > 0 ? 4500 : 3000);
}

void MainWindow::refreshFiltersMenu()
{
    refreshFiltersMenu(m_doc ? m_doc->filterInfos() : std::vector<Document::FilterInfo>{});
}

void MainWindow::scheduleFilterUiRefresh()
{
    if (m_filterUiRefreshPending)
        return;
    m_filterUiRefreshPending = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_filterUiRefreshPending = false;
        refreshFilterUi();
    }, Qt::QueuedConnection);
}

void MainWindow::refreshFilterUi()
{
    const std::vector<Document::FilterInfo> infos =
        m_doc ? m_doc->filterInfos() : std::vector<Document::FilterInfo>{};
    refreshFiltersMenu(infos);
    if (m_filterPanel)
        m_filterPanel->reloadFilters(infos);
}

void MainWindow::refreshFiltersMenu(const std::vector<Document::FilterInfo> &filterInfos)
{
    if (!m_filtersMenu || !m_doc)
        return;

    m_filtersMenu->clear();
    m_filterBrowserAction = m_filtersMenu->addAction(
        tr("Filter Browser..."),
        this,
        &MainWindow::openFilterBrowser);
    // The menu is rebuilt from scratch on every filter reload, so the suspended state has
    // to be reapplied rather than assumed.
    m_filterBrowserAction->setShortcut(
        m_textEditingHasFocus ? QKeySequence() : QKeySequence(QKeySequence::Find));
    m_filterBrowserAction->setShortcutContext(Qt::ApplicationShortcut);
    m_filtersMenu->addSeparator();

    std::vector<Document::FilterInfo> infos = filterInfos;
    if (infos.empty()) {
        QAction *emptyAction = m_filtersMenu->addAction(tr("No filters available"));
        emptyAction->setEnabled(false);
        return;
    }

    std::sort(infos.begin(), infos.end(), [](const Document::FilterInfo &a, const Document::FilterInfo &b) {
        const int menuCmp = a.descriptor.primaryCategory().compare(b.descriptor.primaryCategory(), Qt::CaseInsensitive);
        if (menuCmp != 0)
            return menuCmp < 0;
        return a.descriptor.name.compare(b.descriptor.name, Qt::CaseInsensitive) < 0;
    });

    auto findOrCreateSubmenu = [](QMenu *parent, const QString &title) -> QMenu * {
        if (!parent)
            return nullptr;
        for (QAction *action : parent->actions()) {
            QMenu *submenu = action ? action->menu() : nullptr;
            if (submenu && submenu->title() == title)
                return submenu;
        }
        return parent->addMenu(title);
    };

    // A filter is listed under every one of its categories, so it can be found from
    // whichever concept the user thought of first. Cross-listing is intentional:
    // 27 filters legitimately belong to more than one category.
    auto addFilterAction = [this](QMenu *menu, const Document::FilterInfo &info) {
        QAction *action = menu->addAction(info.descriptor.name, this, &MainWindow::runFilterAction);
        action->setData(info.key);
        action->setEnabled(info.applicable);

        QString tip = info.descriptor.shortDescription.trimmed();
        if (!info.applicable && !info.applicabilityError.trimmed().isEmpty()) {
            if (!tip.isEmpty())
                tip += QStringLiteral("\n");
            tip += tr("Unavailable: %1").arg(info.applicabilityError);
        }
        if (!tip.isEmpty()) {
            action->setToolTip(tip);
            action->setStatusTip(tip);
        }
    };

    for (const Document::FilterInfo &info : infos) {
        const QStringList categories = info.descriptor.categories;
        if (categories.isEmpty()) {
            addFilterAction(m_filtersMenu, info);
            continue;
        }
        for (const QString &category : categories) {
            QMenu *menu = m_filtersMenu;
            for (const QString &group :
                 category.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
                menu = findOrCreateSubmenu(menu, group.trimmed());
                if (!menu)
                    break;
            }
            if (menu)
                addFilterAction(menu, info);
        }
    }
}

void MainWindow::openFilterBrowser()
{
    if (!m_doc || !m_filterPanel)
        return;

    if (m_filterDock) {
        if (m_filterDock->isFloating())
            m_filterDock->setFloating(false);
        m_filterDock->show();
        m_filterDock->raise();
    }
    m_filterPanel->showSearchResults();
    m_filterPanel->focusSearch();
}

// The layer context menu names one filter directly, so for a filter with nothing to ask
// -- Duplicate Current Layer, Remove Hidden Mesh Layers -- opening the panel only shows an
// empty form and an Apply button, and picking it from the menu already said Apply. The
// preference exists because the panel is also where a filter's description lives, so
// someone who wants to read before running can keep the old behaviour.
void MainWindow::openFilterOrRunIfParameterless(const QString &filterKey)
{
    if (!m_doc || !m_filterPanel || filterKey.isEmpty())
        return;

    if (Preferences::instance().boolValue(
            QStringLiteral("document.runParameterlessFiltersImmediately"))) {
        for (const Document::FilterInfo &info : m_doc->filterInfos()) {
            if (info.key != filterKey)
                continue;
            // Not applicable here: the menu greys those out, but the panel is the place
            // that explains why, so fall through to it rather than failing silently.
            if (info.applicable && info.descriptor.parameters.empty()) {
                executeFilter(filterKey, info.descriptor.name, {});
                return;
            }
            break;
        }
    }

    if (m_filterDock) {
        if (m_filterDock->isFloating())
            m_filterDock->setFloating(false);
        m_filterDock->show();
        m_filterDock->raise();
    }
    m_filterPanel->selectFilterByKey(filterKey, true);
}

void MainWindow::runFilterAction()
{
    QAction *action = qobject_cast<QAction *>(sender());
    if (!action || !m_doc || !m_filterPanel)
        return;

    const QString filterKey = action->data().toString();
    if (filterKey.isEmpty())
        return;

    if (m_filterDock) {
        if (m_filterDock->isFloating())
            m_filterDock->setFloating(false);
        m_filterDock->show();
        m_filterDock->raise();
    }
    m_filterPanel->selectFilterByKey(filterKey, true);
}

void MainWindow::executeFilter(
    const QString &filterKey,
    const QString &fallbackLabel,
    const QVariantMap &parameters)
{
    if (!m_doc || filterKey.trimmed().isEmpty())
        return;

    QString label = fallbackLabel;
    if (label.trimmed().isEmpty())
        label = tr("Filter");

    m_doc->beginFilterProgress(label);
    // Document::runFilter logs the run's duration for every entry point; no timing here.
    const MeshFilterRunResult result = m_doc->runFilter(filterKey, parameters);

    if (!result.success) {
        const QString errorText = result.errorMessage.trimmed().isEmpty()
            ? tr("Unknown filter error")
            : result.errorMessage.trimmed();
        const QString msg = errorText.contains(QStringLiteral("interrupt"), Qt::CaseInsensitive)
            ? errorText
            : tr("Filter failed: %1").arg(errorText);
        m_doc->finishFilterProgress(false, msg);
        m_doc->writeLog(msg, Document::LogSource::Application, Document::LogLevel::Error);
        return;
    }

    applyFilterVisualizationHints(result);

    QString status = tr("%1 executed").arg(label);
    if (!result.infoMessages.isEmpty())
        status = result.infoMessages.back();
    m_doc->finishFilterProgress(true, status);
}

void MainWindow::applyFilterVisualizationHints(const MeshFilterRunResult &result)
{
    if (!m_doc || result.visualizationHints.isEmpty())
        return;

    for (const MeshFilterVisualizationHint &hint : result.visualizationHints) {
        const int meshIndex = hint.meshIndex >= 0 ? hint.meshIndex : m_doc->currentMeshIndex();
        if (meshIndex < 0 || meshIndex >= m_doc->meshCount())
            continue;
        for (RenderWidget *view : std::as_const(m_renderWidgets)) {
            if (!view)
                continue;
            switch (hint.attribute) {
            case MeshFilterVisualizationAttribute::Texture:
                view->showTextureVisualization(meshIndex);
                break;
            case MeshFilterVisualizationAttribute::FaceQuality:
                view->showQualityVisualization(meshIndex, true);
                break;
            case MeshFilterVisualizationAttribute::VertexQuality:
                view->showQualityVisualization(meshIndex, false);
                break;
            }
        }
    }
}

void MainWindow::undo()
{
    if (!m_doc->undo())
        return;

    const QString label = m_doc->redoText();
    const QString msg = label.isEmpty() ? tr("Undo") : tr("Undo: %1").arg(label);
    statusBar()->showMessage(msg, 1800);
}

void MainWindow::redo()
{
    if (!m_doc->redo())
        return;

    const QString label = m_doc->undoText();
    const QString msg = label.isEmpty() ? tr("Redo") : tr("Redo: %1").arg(label);
    statusBar()->showMessage(msg, 1800);
}

void MainWindow::saveCurrentMesh()
{
    const int currentIndex = m_doc->currentMeshIndex();
    if (currentIndex < 0 || currentIndex >= m_doc->meshCount()) {
        statusBar()->showMessage(tr("No current mesh to save"), 2500);
        return;
    }

    const Document::MeshEntry &entry = m_doc->mesh(currentIndex);
    const QString defaultPath = !entry.sourcePath.isEmpty()
        ? entry.sourcePath
        : FileDialogDirectory::startingPath(
              QStringLiteral("mesh"),
              QStringLiteral("%1.ply").arg(
                  entry.name.isEmpty() ? QStringLiteral("mesh") : entry.name));

    QString selectedFilter;
    QString targetPath = QFileDialog::getSaveFileName(
        this,
        tr("Save Mesh"),
        defaultPath,
        m_doc->saveDialogFilter(),
        &selectedFilter);
    if (targetPath.isEmpty())
        return;
    FileDialogDirectory::remember(QStringLiteral("mesh"), targetPath);
    targetPath = appendSaveExtensionIfMissing(targetPath, selectedFilter);

    const int capabilityMask = m_doc->saveMaskCapability(targetPath);
    if (capabilityMask == 0) {
        const QString msg =
            tr("No exporter is available for '.%1'").arg(fileExtensionLower(targetPath));
        statusBar()->showMessage(msg, 4000);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }
    const int availableMask = availableSaveMaskForMesh(entry);
    const int requiredMask = requiredSaveMaskForMesh(entry, capabilityMask);
    const QString extension = fileExtensionLower(targetPath);
    const bool binarySupported = saveFormatSupportsBinary(extension);
    const bool supportsEmbeddedTextures = saveFormatSupportsEmbeddedTextures(extension);
    const bool supportsCopyAssociatedTextures = saveFormatSupportsCopyAssociatedTextures(extension);
    const bool supportsDracoCompression = saveFormatSupportsDracoCompression(extension);

    MeshIOSaveOptions initialOptions;
    initialOptions.mask = defaultSaveMaskForMesh(entry, capabilityMask);
    initialOptions.binary = binarySupported;
    initialOptions.embedTextures = (extension == QLatin1String("glb"));
    initialOptions.copyAssociatedTextures = false;
    initialOptions.dracoCompression = false;
    initialOptions.dracoCompressionLevel = 7;

    MeshSaveOptionsDialog optionsDialog(
        targetPath,
        capabilityMask,
        availableMask,
        requiredMask,
        initialOptions,
        binarySupported,
        supportsEmbeddedTextures,
        supportsCopyAssociatedTextures,
        supportsDracoCompression,
        this);
    if (optionsDialog.exec() != QDialog::Accepted)
        return;

    MeshIOSaveOptions saveOptions = optionsDialog.selectedOptions();
    const int err = m_doc->saveCurrentMesh(targetPath, saveOptions);
    if (err != 0) {
        statusBar()->showMessage(tr("Save failed"), 4000);
        return;
    }

    statusBar()->showMessage(tr("Mesh saved to %1").arg(targetPath), 3000);
}

void MainWindow::saveProjectAs()
{
    using SaveOpts = Document::MeshLabProjectSaveOptions;

    QString selectedFilter;
    const QString defaultPath = FileDialogDirectory::startingPath(
        QStringLiteral("project"), QStringLiteral("project.mlp"));
    QString targetPath = QFileDialog::getSaveFileName(
        this,
        tr("Save MeshLab Project"),
        defaultPath,
        tr("MeshLab Project (*.mlp)"),
        &selectedFilter);
    if (targetPath.isEmpty()) return;
    FileDialogDirectory::remember(QStringLiteral("project"), targetPath);

    // Options dialog
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Save Project Options"));
    auto *layout = new QVBoxLayout(&dlg);

    auto *cbOnlyVisible = new QCheckBox(tr("Save only visible meshes"));
    auto *cbSaveModified = new QCheckBox(tr("Re-save modified meshes"));
    cbSaveModified->setChecked(true);
    auto *cbCopyFiles = new QCheckBox(tr("Copy files to project folder (if needed)"));

    // Check if files will be outside the project
    const QDir projectDir = QFileInfo(targetPath).absoluteDir();
    bool needsCopyCheck = false;
    for (int mi = 0; mi < m_doc->meshCount(); ++mi) {
        const QString src = m_doc->mesh(mi).sourcePath;
        if (src.isEmpty()) continue;
        if (!src.startsWith(projectDir.absolutePath() + QDir::separator())
            && QFileInfo(src).absolutePath() != projectDir.absolutePath()) {
            needsCopyCheck = true; break;
        }
    }
    if (!needsCopyCheck) {
        for (int ri = 0; ri < m_doc->rasterCount(); ++ri) {
            const auto &re = m_doc->raster(ri);
            if (re.planes.empty()) continue;
            const QString src = re.planes.front().sourcePath;
            if (src.isEmpty()) continue;
            if (!src.startsWith(projectDir.absolutePath() + QDir::separator())
                && QFileInfo(src).absolutePath() != projectDir.absolutePath()) {
                needsCopyCheck = true; break;
            }
        }
    }

    auto *warnLabel = new QLabel(
        tr("Some files are outside the project directory\nand need to be copied."));
    warnLabel->setStyleSheet(QStringLiteral("color: orange; font-weight: bold;"));
    warnLabel->setVisible(needsCopyCheck);
    cbCopyFiles->setVisible(needsCopyCheck);
    cbCopyFiles->setChecked(false);

    layout->addWidget(cbOnlyVisible);
    layout->addWidget(cbSaveModified);
    if (needsCopyCheck) layout->addWidget(warnLabel);
    if (needsCopyCheck) layout->addWidget(cbCopyFiles);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    layout->addWidget(buttonBox);

    if (dlg.exec() != QDialog::Accepted) return;

    SaveOpts opts;
    opts.onlyVisibleMeshes = cbOnlyVisible->isChecked();
    opts.saveModifiedMeshes = cbSaveModified->isChecked();
    opts.copyFiles = cbCopyFiles->isChecked();

    if (needsCopyCheck && !opts.copyFiles) {
        statusBar()->showMessage(
            tr("Some files are outside the project directory. Enable copy to save."), 4000);
        return;
    }

    QString error;
    if (!m_doc->saveMeshLabProject(targetPath, opts, &error)) {
        QMessageBox::critical(this, tr("Save Project Failed"), error);
    } else {
        statusBar()->showMessage(tr("Project saved: %1").arg(targetPath), 3000);
    }
}

void MainWindow::saveSnapshotPng()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;

    SnapshotDialog dialog(
        view,
        FileDialogDirectory::startingPath(QStringLiteral("snapshot"),
                                          QStringLiteral("snapshot.png")),
        this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString targetPath = dialog.targetPath();
    if (targetPath.isEmpty()) {
        statusBar()->showMessage(tr("Snapshot needs a file name"), 3000);
        return;
    }
    FileDialogDirectory::remember(QStringLiteral("snapshot"), targetPath);

    QString captureError;
    const QImage snapshot = dialog.capture(dialog.snapshotSize(), &captureError);
    if (snapshot.isNull()) {
        const QString msg = tr("Failed to capture snapshot: %1").arg(captureError);
        statusBar()->showMessage(msg, 3500);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }

    QImage outImage = snapshot.convertToFormat(QImage::Format_RGBA8888);
    outImage.setText(QStringLiteral("MeshLab.CameraTrackballState"), view->cameraStateJson());

    QImageWriter writer(targetPath, "png");
    if (!writer.write(outImage)) {
        const QString msg = tr("Failed to save snapshot: %1").arg(writer.errorString());
        statusBar()->showMessage(msg, 4500);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }

    const QString msg = tr("Snapshot saved to %1").arg(targetPath);
    statusBar()->showMessage(msg, 3000);
    m_doc->writeLog(msg, Document::LogSource::Application);
}

void MainWindow::addSnapshotRaster()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;

    const qreal dpr = qMax(1.0, view->devicePixelRatioF());
    const QSize snapshotSize(
        qMax(1, int(std::lround(double(view->width()) * dpr))),
        qMax(1, int(std::lround(double(view->height()) * dpr))));

    QString captureError;
    QImage snapshot = view->renderOffscreenToImage(snapshotSize, false, &captureError);
    if (snapshot.isNull()) {
        const QString msg = tr("Failed to capture snapshot raster: %1").arg(captureError);
        statusBar()->showMessage(msg, 3500);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }

    snapshot = snapshot.convertToFormat(QImage::Format_RGBA8888);
    snapshot.setText(QStringLiteral("MeshLab.CameraTrackballState"), view->cameraStateJson());

    const CameraShot shot = view->cameraShotForViewport(snapshotSize);
    const QString name = tr("Snapshot %1").arg(m_doc->rasterCount() + 1);
    const int index = m_doc->addRasterImage(snapshot, name, QString(), shot);
    if (index < 0) {
        statusBar()->showMessage(tr("Failed to add snapshot raster"), 3500);
        return;
    }

    const QString msg = tr("Added snapshot raster '%1'").arg(m_doc->raster(index).name);
    statusBar()->showMessage(msg, 2500);
    m_doc->writeLog(msg, Document::LogSource::Application);
}

void MainWindow::openLastMesh()
{
    sanitizeRecentMeshes();
    refreshRecentMeshesMenu();

    if (m_recentMeshes.isEmpty()) {
        statusBar()->showMessage(tr("No recent meshes"), 2000);
        return;
    }

    loadMeshFromPath(m_recentMeshes.constFirst());
}

void MainWindow::openRecentMesh()
{
    auto *action = qobject_cast<QAction *>(sender());
    if (!action)
        return;

    const QString filePath = action->data().toString();
    if (filePath.isEmpty())
        return;

    loadMeshFromPath(filePath);
}

void MainWindow::showMemoryInfo()
{
    auto formatBytes = [](qint64 bytes) -> QString {
        if (bytes < 0)
            return QStringLiteral("?");
        if (bytes < 1024LL)
            return QStringLiteral("%1 B").arg(bytes);
        if (bytes < 1024LL * 1024)
            return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 1);
        if (bytes < 1024LL * 1024 * 1024)
            return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 2);
        return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
    };

    const ProcessMemoryInfo processMemory = queryCurrentProcessMemoryInfo();
    const auto cpuStats = m_doc->cpuMeshMemoryStats();
    const auto imageStats = m_doc->cpuImageMemoryStats();
    const auto undoStats = m_doc->undoMemoryStats();
    const auto gpuStats = m_doc->gpuMemoryStats();

    qint64 cpuMeshTotal = 0;
    for (const auto &stats : cpuStats)
        cpuMeshTotal += stats.totalBytes();
    const qint64 trackedCpuTotal =
        cpuMeshTotal + imageStats.totalBytes() + undoStats.totalBytes();

    qint64 gpuTotal = 0;
    for (const auto &stats : gpuStats)
        gpuTotal += stats.totalBytes();

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Memory Info"));
    dialog.resize(760, 620);

    auto *layout = new QVBoxLayout(&dialog);

    auto *explanation = new QLabel(
        tr("OS process memory is the comparison value for Activity Monitor. "
           "Tracked CPU allocations are a lower-bound ownership estimate. "
           "Logical GPU resources use backend resource sizes and are not added to the CPU subtotal."),
        &dialog);
    explanation->setWordWrap(true);
    explanation->setStyleSheet(QStringLiteral("color: palette(mid);"));
    layout->addWidget(explanation);

    auto *tree = new QTreeWidget(&dialog);
    tree->setColumnCount(2);
    tree->setHeaderLabels({tr("Component"), tr("Size")});
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);

    auto makeCategory = [&](const QString &title, qint64 total) -> QTreeWidgetItem * {
        auto *item = new QTreeWidgetItem(tree);
        item->setText(0, title);
        item->setText(1, formatBytes(total));
        QFont f = item->font(0);
        f.setBold(true);
        item->setFont(0, f);
        item->setFont(1, f);
        item->setExpanded(true);
        return item;
    };

    auto addRow = [](QTreeWidgetItem *parent,
                     const QString &name,
                     qint64 bytes,
                     const QString &detail = {}) {
        auto *item = new QTreeWidgetItem(parent);
        item->setText(0, detail.isEmpty() ? name
                                          : QStringLiteral("%1  (%2)").arg(name, detail));
        auto formatB = [](qint64 b) -> QString {
            if (b < 0) return QStringLiteral("?");
            if (b < 1024LL) return QStringLiteral("%1 B").arg(b);
            if (b < 1024LL * 1024) return QStringLiteral("%1 KiB").arg(b / 1024.0, 0, 'f', 1);
            if (b < 1024LL * 1024 * 1024) return QStringLiteral("%1 MiB").arg(b / (1024.0 * 1024.0), 0, 'f', 2);
            return QStringLiteral("%1 GiB").arg(b / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
        };
        item->setText(1, formatB(bytes));
        if (bytes >= 0)
            item->setToolTip(1, QStringLiteral("%1 bytes").arg(bytes));
    };

    // --- OS process memory ---
    auto *processSection = makeCategory(tr("OS Process Memory"), processMemory.preferredBytes());
    if (processMemory.physicalFootprintBytes >= 0)
        addRow(processSection, tr("Physical footprint"), processMemory.physicalFootprintBytes,
               tr("Activity Monitor-compatible on macOS"));
    if (processMemory.privateBytes >= 0)
        addRow(processSection, tr("Private bytes"), processMemory.privateBytes);
    if (processMemory.residentBytes >= 0)
        addRow(processSection, tr("Resident set"), processMemory.residentBytes,
               tr("includes clean and reclaimable pages"));

    // --- CPU mesh data ---
    auto *cpuSection = makeCategory(
        tr("Tracked CPU Mesh Data  (%1 mesh%2)")
            .arg(cpuStats.size())
            .arg(cpuStats.size() == 1 ? "" : "es"),
        cpuMeshTotal);

    for (const auto &s : cpuStats) {
        auto *meshItem = new QTreeWidgetItem(cpuSection);
        meshItem->setText(0, s.name);
        meshItem->setText(1, formatBytes(s.totalBytes()));
        meshItem->setExpanded(true);
        addRow(meshItem, tr("Vertices"), s.vertexBytes,
               tr("%1 × %2 B").arg(s.vertexCapacity).arg(sizeof(VCGVertex)));
        if (s.vertexOcfBytes > 0)
            addRow(meshItem, tr("Vertex OCF side data"), s.vertexOcfBytes,
                   tr("optional-component vectors"));
        if (s.edgeCapacity > 0)
            addRow(meshItem, tr("Edges"), s.edgeBytes,
                   tr("%1 × %2 B").arg(s.edgeCapacity).arg(sizeof(VCGEdge)));
        if (s.faceCapacity > 0)
            addRow(meshItem, tr("Faces"), s.faceBytes,
                   tr("%1 × %2 B").arg(s.faceCapacity).arg(sizeof(VCGFace)));
        if (s.faceOcfBytes > 0)
            addRow(meshItem, tr("Face OCF side data"), s.faceOcfBytes,
                   tr("optional-component vectors"));
        if (s.customAttributeBytes > 0)
            addRow(meshItem, tr("Custom attributes"), s.customAttributeBytes,
                   tr("estimated from owning container capacities"));
        if (s.pluginDataBytes > 0)
            addRow(meshItem, tr("Plugin data"), s.pluginDataBytes,
                   tr("self-reported by the owning plugin"));
    }

    // --- CPU image storage ---
    auto *imageSection = makeCategory(tr("Tracked CPU Images"), imageStats.totalBytes());
    addRow(imageSection, tr("Mesh texture images"), imageStats.meshTextureBytes,
           tr("%1 unique backing store(s)").arg(imageStats.uniqueMeshTextureImages));
    addRow(imageSection, tr("Raster images"), imageStats.rasterImageBytes,
           tr("%1 unique backing store(s)").arg(imageStats.uniqueRasterImages));

    // --- CPU undo history ---
    auto *undoSection = makeCategory(
        tr("Tracked CPU Undo History  (%1 step%2, %3 node%4)")
            .arg(undoStats.steps.size())
            .arg(undoStats.steps.size() == 1 ? "" : "s")
            .arg(undoStats.nodeCount)
            .arg(undoStats.nodeCount == 1 ? "" : "s"),
        undoStats.totalBytes());

    addRow(undoSection, tr("Unique geometry snapshots"), undoStats.geometryBytes,
           tr("%1 allocation(s), shared snapshots counted once")
               .arg(undoStats.uniqueGeometryCount));
    addRow(undoSection, tr("Selection deltas"), undoStats.selectionBytes);
    addRow(undoSection, tr("History-only images"), undoStats.historyImageBytes,
           tr("%1 unique backing store(s); live images excluded")
               .arg(undoStats.uniqueHistoryImageCount));
    if (undoStats.pendingBytes() > 0) {
        auto *pendingItem = new QTreeWidgetItem(undoSection);
        pendingItem->setText(0, tr("Pending undo operation"));
        pendingItem->setText(1, formatBytes(undoStats.pendingBytes()));
        pendingItem->setExpanded(true);
        if (undoStats.pendingGeometryBytes > 0)
            addRow(pendingItem, tr("Unique geometry"), undoStats.pendingGeometryBytes);
        if (undoStats.pendingSelectionBytes > 0)
            addRow(pendingItem, tr("Selection delta"), undoStats.pendingSelectionBytes);
        if (undoStats.pendingImageBytes > 0)
            addRow(pendingItem, tr("History-only images"), undoStats.pendingImageBytes);
    }

    if (!undoStats.steps.empty()) {
        auto *pathItem = new QTreeWidgetItem(undoSection);
        pathItem->setText(0, tr("Current path references (not additive)"));
        pathItem->setText(1, QStringLiteral("-"));
        for (const auto &step : undoStats.steps) {
            const qint64 bytes = step.selectionDelta
                ? step.selectionBytes
                : step.referencedGeometryBytes;
            addRow(pathItem, step.label, bytes,
                   step.selectionDelta ? tr("owned selection storage")
                                       : tr("shared geometry referenced by state"));
        }
    }

    makeCategory(tr("TRACKED CPU SUBTOTAL (LOWER BOUND)"), trackedCpuTotal);

    // --- GPU buffers & textures ---
    auto *gpuSection = makeCategory(
        tr("Logical Mesh GPU Cache  (%1 mesh%2)")
            .arg(gpuStats.size())
            .arg(gpuStats.size() == 1 ? "" : "es"),
        gpuTotal);

    for (const auto &s : gpuStats) {
        QString meshName;
        for (int i = 0; i < m_doc->meshCount(); ++i) {
            if (m_doc->mesh(i).meshId == s.meshId) {
                meshName = m_doc->mesh(i).name;
                break;
            }
        }
        if (meshName.isEmpty())
            meshName = tr("Mesh [id %1]").arg(s.meshId);

        auto *meshItem = new QTreeWidgetItem(gpuSection);
        meshItem->setText(0, meshName);
        meshItem->setText(1, formatBytes(s.totalBytes()));
        meshItem->setExpanded(true);
        if (s.fillBufferBytes > 0)
            addRow(meshItem, tr("Fill vertex buffers"), s.fillBufferBytes);
        if (s.textureBytes > 0)
            addRow(meshItem, tr("Textures"),            s.textureBytes);
        if (s.wireBufferBytes > 0)
            addRow(meshItem, tr("Wire buffer"),         s.wireBufferBytes);
        if (s.edgeBufferBytes > 0)
            addRow(meshItem, tr("Edge buffers"),        s.edgeBufferBytes);
        if (s.pointsBufferBytes > 0)
            addRow(meshItem, tr("Points buffers"),      s.pointsBufferBytes);
        if (s.bboxBufferBytes > 0)
            addRow(meshItem, tr("BBox buffer"),         s.bboxBufferBytes);
        if (s.selectionBufferBytes > 0)
            addRow(meshItem, tr("Selection buffers"),   s.selectionBufferBytes);
        if (s.decoratorBufferBytes > 0)
            addRow(meshItem, tr("Decorator buffers"),   s.decoratorBufferBytes);
    }

    tree->resizeColumnToContents(1);
    layout->addWidget(tree);

    auto *buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    auto *copyJsonButton = buttonBox->addButton(tr("Copy JSON"), QDialogButtonBox::ActionRole);

    QJsonObject processJson;
    processJson.insert(QStringLiteral("physicalFootprintBytes"),
                       double(processMemory.physicalFootprintBytes));
    processJson.insert(QStringLiteral("privateBytes"), double(processMemory.privateBytes));
    processJson.insert(QStringLiteral("residentBytes"), double(processMemory.residentBytes));

    QJsonArray meshesJson;
    for (const auto &stats : cpuStats) {
        QJsonObject meshJson;
        meshJson.insert(QStringLiteral("meshId"), QString::number(stats.meshId));
        meshJson.insert(QStringLiteral("name"), stats.name);
        meshJson.insert(QStringLiteral("vertexBytes"), double(stats.vertexBytes));
        meshJson.insert(QStringLiteral("vertexOcfBytes"), double(stats.vertexOcfBytes));
        meshJson.insert(QStringLiteral("edgeBytes"), double(stats.edgeBytes));
        meshJson.insert(QStringLiteral("faceBytes"), double(stats.faceBytes));
        meshJson.insert(QStringLiteral("faceOcfBytes"), double(stats.faceOcfBytes));
        meshJson.insert(QStringLiteral("customAttributeBytes"), double(stats.customAttributeBytes));
        meshJson.insert(QStringLiteral("pluginDataBytes"), double(stats.pluginDataBytes));
        meshJson.insert(QStringLiteral("totalBytes"), double(stats.totalBytes()));
        meshesJson.append(meshJson);
    }

    QJsonObject imagesJson;
    imagesJson.insert(QStringLiteral("meshTextureBytes"), double(imageStats.meshTextureBytes));
    imagesJson.insert(QStringLiteral("rasterImageBytes"), double(imageStats.rasterImageBytes));
    imagesJson.insert(QStringLiteral("totalBytes"), double(imageStats.totalBytes()));

    QJsonObject undoJson;
    undoJson.insert(QStringLiteral("nodeCount"), undoStats.nodeCount);
    undoJson.insert(QStringLiteral("geometryBytes"), double(undoStats.geometryBytes));
    undoJson.insert(QStringLiteral("selectionBytes"), double(undoStats.selectionBytes));
    undoJson.insert(QStringLiteral("historyImageBytes"), double(undoStats.historyImageBytes));
    undoJson.insert(QStringLiteral("pendingBytes"), double(undoStats.pendingBytes()));
    undoJson.insert(QStringLiteral("totalBytes"), double(undoStats.totalBytes()));

    QJsonObject cpuJson;
    cpuJson.insert(QStringLiteral("meshes"), meshesJson);
    cpuJson.insert(QStringLiteral("meshTotalBytes"), double(cpuMeshTotal));
    cpuJson.insert(QStringLiteral("images"), imagesJson);
    cpuJson.insert(QStringLiteral("undo"), undoJson);
    cpuJson.insert(QStringLiteral("totalBytes"), double(trackedCpuTotal));

    QJsonObject gpuJson;
    gpuJson.insert(QStringLiteral("meshCacheTotalBytes"), double(gpuTotal));

    QJsonObject report;
    report.insert(QStringLiteral("schema"), QStringLiteral("org.meshlab.memory-report.v1"));
    report.insert(QStringLiteral("timestampUtc"),
                  QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    report.insert(QStringLiteral("pid"), double(QCoreApplication::applicationPid()));
    report.insert(QStringLiteral("process"), processJson);
    report.insert(QStringLiteral("trackedCpu"), cpuJson);
    report.insert(QStringLiteral("logicalGpu"), gpuJson);

    connect(copyJsonButton, &QPushButton::clicked, &dialog, [report]() {
        if (QClipboard *clipboard = QGuiApplication::clipboard()) {
            clipboard->setText(
                QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented)));
        }
    });
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::accept);
    layout->addWidget(buttonBox);

    dialog.exec();
}

void MainWindow::showAbout()
{
    AboutDialog(this).exec();
}

void MainWindow::showImportPlugins()
{
    const std::vector<Document::ImportPluginInfo> importPlugins = m_doc->importPluginInfos();
    const QStringList importExtensions = m_doc->importSupportedExtensions();
    const std::vector<Document::ExportPluginInfo> exportPlugins = m_doc->exportPluginInfos();
    const QStringList exportExtensions = m_doc->exportSupportedExtensions();

    const bool hasImportMatrix = !importPlugins.empty() && !importExtensions.isEmpty();
    const bool hasExportMatrix = !exportPlugins.empty() && !exportExtensions.isEmpty();
    if (!hasImportMatrix && !hasExportMatrix) {
        QMessageBox::information(this, tr("I/O Plugins"), tr("No plugins are available."));
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("I/O Plugins"));

    auto *layout = new QVBoxLayout(&dialog);

    // A QTableWidget's sizeHint is its scroll area's, not its contents' -- about 256x192
    // whatever it holds -- so a dialog sized from size hints opens with the tables cropped:
    // elided plugin names down the side, the right-hand columns past the edge, and the last
    // row hidden. These matrices are small and fixed (a handful of plugins by a handful of
    // extensions), so measure what they actually need and let the dialog be that wide.
    const auto fitToContents = [](QTableWidget *table, int maxHeight) {
        table->resizeColumnsToContents();
        table->resizeRowsToContents();
        // The vertical header's sizeHint is also premature -- it has not measured its own
        // labels yet and reports far less than the plugin names need, which is what elided
        // them. Measure the text.
        // isHidden(), not isVisible(): nothing here has been shown yet, so isVisible() is
        // false for a header that is perfectly well configured to appear, and the whole
        // measurement silently came out as the columns alone.
        int width = 0;
        if (!table->verticalHeader()->isHidden()) {
            const QFontMetrics metrics(table->verticalHeader()->font());
            for (int row = 0; row < table->rowCount(); ++row) {
                if (const QTableWidgetItem *item = table->verticalHeaderItem(row))
                    width = std::max(width, metrics.horizontalAdvance(item->text()));
            }
            width += 16; // the header's own padding either side
        }
        for (int col = 0; col < table->columnCount(); ++col) {
            // resizeColumnsToContents measures items, and these cells hold WIDGETS -- the
            // radio buttons -- which have not been laid out yet and so measure near zero.
            // The header's own hint knows how wide "gltf" is; the floor covers a column
            // whose only content is a radio button. The header's ResizeToContents mode
            // sets the real widths once there is something to measure, so this is an upper
            // bound on what it will choose -- deliberately, since underestimating crops.
            static constexpr int kMinCellWidth = 34;
            width += std::max({ table->columnWidth(col),
                                table->horizontalHeader()->sectionSizeHint(col),
                                kMinCellWidth });
        }
        int height = table->horizontalHeader()->sizeHint().height();
        for (int row = 0; row < table->rowCount(); ++row)
            height += table->rowHeight(row);
        const int frame = 2 * table->frameWidth() + 2;
        table->setMinimumWidth(width + frame);
        // Also a maximum: stretched to the dialog width these matrices are mostly empty
        // frame, and a table exactly as wide as its columns reads as a table.
        table->setMaximumWidth(width + frame);
        // Capped, so an install with many plugins scrolls instead of filling the screen.
        table->setMinimumHeight(std::min(height + frame, maxHeight));
        table->setMaximumHeight(height + frame);
    };

    const auto sectionLabel = [&dialog](const QString &text) {
        auto *label = new QLabel(text, &dialog);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        return label;
    };
    const auto explanation = [&dialog](const QString &text) {
        auto *label = new QLabel(text, &dialog);
        label->setWordWrap(true);
        label->setStyleSheet(QStringLiteral("color: palette(mid);"));
        return label;
    };

    layout->addWidget(explanation(
        tr("Every format MeshLab opens or saves is handled by a plugin, and some formats "
           "are handled by more than one -- three plugins read .obj. This is where you "
           "choose which of them opens each kind of file, and see what each plugin can "
           "carry.")));
    layout->addSpacing(8);
    layout->addWidget(sectionLabel(tr("Import: which plugin opens each format")));

    QTableWidget *importTable = nullptr;
    std::vector<QButtonGroup *> groups;
    if (hasImportMatrix) {
        layout->addWidget(explanation(
            tr("Pick one plugin per extension; the choice is remembered between sessions. "
               "Where you have not chosen, the first plugin that accepts the file is used. "
               "Plugins differ in what they tolerate and what they preserve, so a file that "
               "one refuses may well open with another.")));

        importTable = new QTableWidget(
            static_cast<int>(importPlugins.size()),
            importExtensions.size(),
            &dialog);
        importTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        importTable->setSelectionMode(QAbstractItemView::NoSelection);
        importTable->setFocusPolicy(Qt::NoFocus);
        importTable->setHorizontalHeaderLabels(importExtensions);
        importTable->verticalHeader()->setVisible(true);
        importTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        importTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

        QStringList rowLabels;
        rowLabels.reserve(static_cast<int>(importPlugins.size()));
        for (const auto &plugin : importPlugins)
            rowLabels << plugin.name;
        importTable->setVerticalHeaderLabels(rowLabels);

        groups.assign(static_cast<size_t>(importExtensions.size()), nullptr);
        for (int col = 0; col < importExtensions.size(); ++col) {
            auto *group = new QButtonGroup(importTable);
            group->setExclusive(true);
            groups[static_cast<size_t>(col)] = group;
        }

        for (int col = 0; col < importExtensions.size(); ++col) {
            const QString extension = importExtensions[col];
            const QString preferredPluginId = m_doc->preferredImportPluginForExtension(extension);
            int preferredRow = -1;
            int firstSupportedRow = -1;

            for (int row = 0; row < static_cast<int>(importPlugins.size()); ++row) {
                const bool supports = importPlugins[static_cast<size_t>(row)].extensions.contains(extension);
                if (!supports)
                    continue;

                if (firstSupportedRow < 0)
                    firstSupportedRow = row;
                if (importPlugins[static_cast<size_t>(row)].id == preferredPluginId)
                    preferredRow = row;

                auto *radio = new QRadioButton(importTable);
                radio->setToolTip(
                    tr("Use \"%1\" for .%2 files")
                        .arg(importPlugins[static_cast<size_t>(row)].name, extension));
                groups[static_cast<size_t>(col)]->addButton(radio, row);

                auto *cell = new QWidget(importTable);
                auto *cellLayout = new QHBoxLayout(cell);
                cellLayout->setContentsMargins(0, 0, 0, 0);
                cellLayout->addWidget(radio, 0, Qt::AlignCenter);
                importTable->setCellWidget(row, col, cell);
            }

            const int rowToSelect = (preferredRow >= 0) ? preferredRow : firstSupportedRow;
            if (rowToSelect >= 0) {
                if (QAbstractButton *button = groups[static_cast<size_t>(col)]->button(rowToSelect))
                    button->setChecked(true);
            }
        }

        importTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        importTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        fitToContents(importTable, 260);
        layout->addWidget(importTable, 0);
    } else {
        auto *label = new QLabel(tr("No import plugins are available."), &dialog);
        label->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout->addWidget(label);
    }

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("Export: which plugin writes each format")));
    if (hasExportMatrix) {
        layout->addWidget(explanation(
            tr("Nothing to choose here: saving uses whichever plugin writes the format you "
               "save to. Savable formats: %1.")
                .arg(exportExtensions.join(QStringLiteral(", ")))));
    } else {
        layout->addWidget(explanation(tr("No savable formats available.")));
    }

    QTableWidget *exportTable = nullptr;
    if (hasExportMatrix) {
        exportTable = new QTableWidget(
            static_cast<int>(exportPlugins.size()),
            exportExtensions.size(),
            &dialog);
        exportTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
        exportTable->setSelectionMode(QAbstractItemView::NoSelection);
        exportTable->setFocusPolicy(Qt::NoFocus);
        exportTable->setHorizontalHeaderLabels(exportExtensions);
        exportTable->verticalHeader()->setVisible(true);
        exportTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        exportTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);

        QStringList rowLabels;
        rowLabels.reserve(static_cast<int>(exportPlugins.size()));
        for (const auto &plugin : exportPlugins)
            rowLabels << plugin.name;
        exportTable->setVerticalHeaderLabels(rowLabels);

        for (int col = 0; col < exportExtensions.size(); ++col) {
            const QString extension = exportExtensions[col];
            for (int row = 0; row < static_cast<int>(exportPlugins.size()); ++row) {
                const bool supports =
                    exportPlugins[static_cast<size_t>(row)].extensions.contains(extension);

                auto *item = new QTableWidgetItem();
                item->setFlags(Qt::ItemIsEnabled);
                item->setTextAlignment(Qt::AlignCenter);
                if (supports) {
                    item->setText(QStringLiteral("●"));
                    item->setToolTip(
                        tr("\"%1\" can save .%2 files")
                            .arg(exportPlugins[static_cast<size_t>(row)].name, extension));
                }
                exportTable->setItem(row, col, item);
            }
        }

        exportTable->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        exportTable->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        fitToContents(exportTable, 260);
        layout->addWidget(exportTable, 0);
    } else {
        auto *label = new QLabel(tr("No export plugins are available."), &dialog);
        label->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout->addWidget(label);
    }

    layout->addSpacing(12);
    layout->addWidget(sectionLabel(tr("What each plugin carries")));
    layout->addWidget(explanation(
        tr("The mesh data each plugin can read and write for a format. Anything not listed "
           "is dropped when a file goes through that plugin, which is the other reason the "
           "choice above matters.")));
    auto *capabilityTable = new QTableWidget(&dialog);
    capabilityTable->setColumnCount(4);
    capabilityTable->setHorizontalHeaderLabels(
        { tr("Format"), tr("Plugin"), tr("Import"), tr("Export") });
    capabilityTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    capabilityTable->setSelectionMode(QAbstractItemView::NoSelection);
    capabilityTable->setFocusPolicy(Qt::NoFocus);
    capabilityTable->verticalHeader()->hide();
    capabilityTable->setWordWrap(true);

    QHash<QString, int> capabilityRows;
    auto ensureCapabilityRow = [&](const QString &id, const QString &name, const QString &ext) {
        const QString key = id + QLatin1Char('\n') + ext;
        const auto existing = capabilityRows.constFind(key);
        if (existing != capabilityRows.constEnd())
            return existing.value();
        const int row = capabilityTable->rowCount();
        capabilityTable->insertRow(row);
        capabilityTable->setItem(row, 0, new QTableWidgetItem(QStringLiteral(".%1").arg(ext)));
        capabilityTable->setItem(row, 1, new QTableWidgetItem(name));
        capabilityTable->setItem(row, 2, new QTableWidgetItem(tr("-")));
        capabilityTable->setItem(row, 3, new QTableWidgetItem(tr("-")));
        capabilityRows.insert(key, row);
        return row;
    };
    for (const Document::ImportPluginInfo &plugin : importPlugins) {
        for (const QString &ext : plugin.extensions) {
            const int row = ensureCapabilityRow(plugin.id, plugin.name, ext);
            capabilityTable->item(row, 2)->setText(
                ioCapabilityText(plugin.capabilities.value(ext)));
        }
    }
    for (const Document::ExportPluginInfo &plugin : exportPlugins) {
        for (const QString &ext : plugin.extensions) {
            const int row = ensureCapabilityRow(plugin.id, plugin.name, ext);
            capabilityTable->item(row, 3)->setText(
                ioCapabilityText(plugin.capabilities.value(ext)));
        }
    }
    capabilityTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    capabilityTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    // Stretch shared the leftover width between these two, which with a narrow dialog left
    // each attribute list wrapping at about five characters a line. Give them a real width
    // and let the dialog carry it.
    capabilityTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    capabilityTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Interactive);
    capabilityTable->setColumnWidth(2, 260);
    capabilityTable->setColumnWidth(3, 260);
    capabilityTable->verticalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    capabilityTable->setMinimumHeight(200);
    layout->addWidget(capabilityTable, 1);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.adjustSize();

    // Widest thing the dialog has to show, rather than the size hint, which understates the
    // tables (see fitToContents above). The capability table is allowed to scroll sideways
    // if its two attribute columns are extravagant, so it only asks for its fixed columns.
    int contentWidth = 0;
    for (const QTableWidget *table : { importTable, exportTable })
        if (table)
            contentWidth = std::max(contentWidth, table->minimumWidth());
    const int capabilityColumns = capabilityTable->horizontalHeader()->sectionSize(0)
        + capabilityTable->horizontalHeader()->sectionSize(1) + 2 * 260;
    contentWidth = std::max(contentWidth,
                            capabilityColumns + 2 * capabilityTable->frameWidth()
                                + capabilityTable->verticalScrollBar()->sizeHint().width());
    const QMargins margins = layout->contentsMargins();
    contentWidth += margins.left() + margins.right();

    if (QScreen *screen = dialog.screen()) {
        const QRect available = screen->availableGeometry();
        const QSize maxDialogSize(available.width() * 9 / 10, available.height() * 9 / 10);
        const QSize wanted(std::max(contentWidth, dialog.sizeHint().width()),
                           dialog.sizeHint().height());
        dialog.resize(wanted.boundedTo(maxDialogSize));
    } else {
        dialog.resize(std::max(contentWidth, dialog.sizeHint().width()),
                      dialog.sizeHint().height());
    }

    if (dialog.exec() != QDialog::Accepted)
        return;

    for (int col = 0; col < importExtensions.size(); ++col) {
        QButtonGroup *group = groups[static_cast<size_t>(col)];
        if (!group)
            continue;
        const int selectedRow = group->checkedId();
        if (selectedRow < 0 || selectedRow >= static_cast<int>(importPlugins.size()))
            continue;

        m_doc->setPreferredImportPluginForExtension(
            importExtensions[col],
            importPlugins[static_cast<size_t>(selectedRow)].id);
    }

    if (hasImportMatrix)
        statusBar()->showMessage(tr("Plugin preferences updated"), 2000);
}

void MainWindow::showFilterPlugins()
{
    std::vector<Document::FilterInfo> filters = m_doc->filterInfos();
    if (filters.empty()) {
        QMessageBox::information(this, tr("Filter Plugins Info"),
                                 tr("No filter plugins are available."));
        return;
    }
    FilterPluginsInfoDialog dialog(std::move(filters), this);
    dialog.exec();
}

void MainWindow::resetCamera()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;
    view->resetCameraToScene();
    statusBar()->showMessage(tr("Camera reset"), 1500);
}

void MainWindow::centerCameraOnSelection()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;
    if (view->centerCameraOnSelection())
        statusBar()->showMessage(tr("Centered on selection"), 1500);
    else
        statusBar()->showMessage(tr("No selection to center on"), 2000);
}

void MainWindow::setCurrentViewSceneMode()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;
    QString error;
    if (!view->setViewMode(RenderWidget::ViewMode::Scene3D, &error)) {
        const QString msg = tr("Cannot switch to 3D mode: %1").arg(error);
        statusBar()->showMessage(msg, 3000);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }
    statusBar()->showMessage(tr("View mode: 3D scene"), 2000);
}

void MainWindow::setCurrentViewParametrizationMode()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;
    QString error;
    if (!view->setViewMode(RenderWidget::ViewMode::ParametrizationUV, &error)) {
        const QString msg = tr("Cannot switch to parametrization mode: %1").arg(error);
        statusBar()->showMessage(msg, 3500);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }
    statusBar()->showMessage(tr("View mode: parametrization (UV)"), 2000);
}

void MainWindow::setCurrentViewRasterMode()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;
    QString error;
    if (!view->setViewMode(RenderWidget::ViewMode::RasterImage, &error)) {
        const QString msg = tr("Cannot switch to raster mode: %1").arg(error);
        statusBar()->showMessage(msg, 3500);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }
    statusBar()->showMessage(tr("View mode: raster"), 2000);
}

void MainWindow::copyCameraState()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;

    const QString json = view->cameraStateJson();
    if (QClipboard *clipboard = QGuiApplication::clipboard()) {
        clipboard->setText(json, QClipboard::Clipboard);
        if (clipboard->supportsSelection())
            clipboard->setText(json, QClipboard::Selection);
    }

    const QString msg = tr("Camera/trackball JSON copied to clipboard");
    statusBar()->showMessage(msg, 2000);
    m_doc->writeLog(msg, Document::LogSource::Application);
}

void MainWindow::pasteCameraState()
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return;

    QString jsonText;
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        jsonText = clipboard->text(QClipboard::Clipboard);

    QString error;
    if (!view->applyCameraStateJson(jsonText, &error)) {
        const QString msg = tr("Cannot paste camera/trackball JSON: %1").arg(error);
        statusBar()->showMessage(msg, 3500);
        m_doc->writeLog(msg, Document::LogSource::Application);
        return;
    }

    const QString msg = tr("Camera/trackball state restored from clipboard JSON");
    statusBar()->showMessage(msg, 2500);
    m_doc->writeLog(msg, Document::LogSource::Application);
}

bool MainWindow::loadMeshFromPath(const QString &filePath, QString *errorMessage)
{
    const bool isProject =
        QFileInfo(filePath).suffix().compare(QStringLiteral("mlp"), Qt::CaseInsensitive) == 0;
    QString reason;
    const int err = isProject
        ? m_doc->loadMeshLabProject(filePath)
        : m_doc->loadMesh(filePath, &reason);
    if (err != 0) {
        if (reason.isEmpty()) {
            reason = isProject ? tr("the project could not be opened")
                               : tr("the importer reported error %1").arg(err);
        }
        // The status bar is the wrong place to leave this when several files are opening:
        // the next file's message replaces it and the summary replaces them all, which is
        // how a four-file open could drop two of them with nothing to show for it. The
        // caller collects the reason and reports the set.
        if (errorMessage)
            *errorMessage = reason;
        else
            statusBar()->showMessage(tr("Failed to load %1: %2").arg(filePath, reason), 5000);
        return false;
    }

    statusBar()->showMessage(tr("Loaded %1").arg(filePath), 3000);
    addRecentMesh(filePath);
    return true;
}

bool MainWindow::loadRasterFromPath(const QString &filePath)
{
    const int index = m_doc->loadRasterImage(filePath);
    if (index < 0) {
        statusBar()->showMessage(tr("Failed to load raster %1").arg(filePath), 3000);
        return false;
    }

    statusBar()->showMessage(tr("Loaded raster %1").arg(filePath), 3000);
    return true;
}

void MainWindow::addRecentMesh(const QString &filePath)
{
    const QString normalizedPath = normalizeRecentPath(filePath);
    if (!normalizedPath.isEmpty())
        m_recentMeshes.prepend(normalizedPath);

    sanitizeRecentMeshes();
    refreshRecentMeshesMenu();
}

void MainWindow::sanitizeRecentMeshes()
{
    QStringList cleaned;
    cleaned.reserve(recentMeshesLimit());
    for (const QString &path : std::as_const(m_recentMeshes)) {
        const QString normalizedPath = normalizeRecentPath(path);
        if (normalizedPath.isEmpty())
            continue;
        if (!QFileInfo::exists(normalizedPath))
            continue;

        bool alreadyInList = false;
        for (const QString &existingPath : std::as_const(cleaned)) {
            if (sameRecentPath(existingPath, normalizedPath)) {
                alreadyInList = true;
                break;
            }
        }
        if (alreadyInList)
            continue;

        if (cleaned.size() >= recentMeshesLimit())
            break;
        cleaned.append(normalizedPath);
    }

    if (cleaned != m_recentMeshes)
        m_recentMeshes = cleaned;

    QSettings settings;
    settings.setValue(QStringLiteral("recentMeshes"), m_recentMeshes);
}

void MainWindow::refreshRecentMeshesMenu()
{
    m_recentMenu->clear();

    for (int i = 0; i < m_recentMeshes.size() && i < recentMeshesLimit(); ++i) {
        const QString &path = m_recentMeshes[i];
        QAction *action = m_recentMenu->addAction(QFileInfo(path).fileName());
        action->setData(path);
        action->setToolTip(path);
        if (i < 8)
            action->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)));
        connect(action, &QAction::triggered, this, &MainWindow::openRecentMesh);
    }

    m_recentMenu->setEnabled(!m_recentMeshes.isEmpty());
    m_openLastAction->setEnabled(!m_recentMeshes.isEmpty());
}

void MainWindow::updateFrameTimeStats(float cpuMs, float gpuMs, bool gpuTimingSupported, bool gpuSampleValid)
{
    m_lastCpuFrameTimes.push_back(cpuMs);
    if (m_lastCpuFrameTimes.size() > kFrameStatsWindow)
        m_lastCpuFrameTimes.pop_front();

    const auto [cpuMinIt, cpuMaxIt] = std::minmax_element(m_lastCpuFrameTimes.begin(), m_lastCpuFrameTimes.end());
    const float cpuSum = std::accumulate(m_lastCpuFrameTimes.begin(), m_lastCpuFrameTimes.end(), 0.0f);
    const float cpuAvg = cpuSum / static_cast<float>(m_lastCpuFrameTimes.size());
    const float cpuMinMs = (cpuMinIt != m_lastCpuFrameTimes.end()) ? *cpuMinIt : cpuMs;
    const float cpuMaxMs = (cpuMaxIt != m_lastCpuFrameTimes.end()) ? *cpuMaxIt : cpuMs;

    QString gpuText;
    if (!gpuTimingSupported) {
        m_lastGpuFrameTimes.clear();
        gpuText = tr("GPU: n/a");
    } else {
        if (gpuSampleValid) {
            m_lastGpuFrameTimes.push_back(gpuMs);
            if (m_lastGpuFrameTimes.size() > kFrameStatsWindow)
                m_lastGpuFrameTimes.pop_front();
        }

        if (m_lastGpuFrameTimes.empty()) {
            gpuText = tr("GPU: waiting...");
        } else {
            const auto [gpuMinIt, gpuMaxIt] =
                std::minmax_element(m_lastGpuFrameTimes.begin(), m_lastGpuFrameTimes.end());
            const float gpuSum = std::accumulate(m_lastGpuFrameTimes.begin(), m_lastGpuFrameTimes.end(), 0.0f);
            const float gpuAvg = gpuSum / static_cast<float>(m_lastGpuFrameTimes.size());
            const float gpuMinMs = (gpuMinIt != m_lastGpuFrameTimes.end()) ? *gpuMinIt : m_lastGpuFrameTimes.back();
            const float gpuMaxMs = (gpuMaxIt != m_lastGpuFrameTimes.end()) ? *gpuMaxIt : m_lastGpuFrameTimes.back();
            const float gpuCurrentMs = gpuSampleValid ? gpuMs : m_lastGpuFrameTimes.back();

            gpuText = tr("GPU: %1 ms | Last %2 avg %3 ms (min %4, max %5)")
                .arg(gpuCurrentMs, 0, 'f', 3)
                .arg(m_lastGpuFrameTimes.size())
                .arg(gpuAvg, 0, 'f', 3)
                .arg(gpuMinMs, 0, 'f', 3)
                .arg(gpuMaxMs, 0, 'f', 3);
        }
    }

    const QString statsText = tr("CPU: %1 ms | Last %2 avg %3 ms (min %4, max %5) | %6")
        .arg(cpuMs, 0, 'f', 3)
        .arg(m_lastCpuFrameTimes.size())
        .arg(cpuAvg, 0, 'f', 3)
        .arg(cpuMinMs, 0, 'f', 3)
        .arg(cpuMaxMs, 0, 'f', 3)
        .arg(gpuText);

    if (m_frameStatsLabel)
        m_frameStatsLabel->setText(statsText);
    else
        statusBar()->showMessage(statsText);
}

QPixmap MainWindow::captureUndoHistoryThumbnail() const
{
    RenderWidget *view = currentRenderWidget();
    if (!view)
        return QPixmap();
    const QImage frame = view->grabFramebuffer();
    if (frame.isNull())
        return QPixmap();
    // Centre-crop to 2:1 aspect ratio, then scale to kThumbW × kThumbH.
    return QPixmap::fromImage(
        [&]{
            const int fw = frame.width();
            const int fh = frame.height();
            // Crop to 2:1: pick the dimension that fits without stretching.
            int cropW, cropH;
            if (fw * 1 >= fh * 2) {   // frame is wider than 2:1 → crop width
                cropH = fh;
                cropW = fh * 2;
            } else {                   // frame is taller than 2:1 → crop height
                cropW = fw;
                cropH = fw / 2;
            }
            const int ox = (fw - cropW) / 2;
            const int oy = (fh - cropH) / 2;
            const QImage cropped = frame.copy(ox, oy, cropW, std::max(1, cropH));
            return cropped.scaled(96, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }());
}

void MainWindow::jumpToUndoNode(int nodeId, bool withCamera)
{
    if (!m_doc)
        return;
    if (m_doc->jumpToUndoNode(nodeId, withCamera)) {
        const auto infos = m_doc->undoTreeInfo();
        for (const auto &info : infos) {
            if (info.nodeId == nodeId && !info.label.isEmpty()) {
                statusBar()->showMessage(tr("History: %1").arg(info.label), 1800);
                break;
            }
        }
    }
}

// Varying a recorded action means standing where it was invoked from, not where it landed:
// we jump to the node's parent and reopen its filter on the same values, so Apply produces
// a sibling branch rather than stacking on top of the original result.
void MainWindow::reopenFilterFromUndoNode(int nodeId)
{
    if (!m_doc || !m_filterPanel)
        return;

    // undoNodeAction(), not undoNodeScriptActions(): the latter also returns the
    // informational filter calls recorded around this state, which did not produce it.
    const std::optional<ScriptAction> action = m_doc->undoNodeAction(nodeId);
    if (!action || action->kind != QStringLiteral("filter") || action->filterKey.isEmpty())
        return;

    int parentId = -1;
    QString actionLabel;
    for (const auto &info : m_doc->undoTreeInfo()) {
        if (info.nodeId == nodeId) {
            parentId = info.parentId;
            actionLabel = info.label;
            break;
        }
    }
    if (parentId < 0)
        return;

    // Data only: the point is to redo the work, so leave the user's viewpoint alone.
    jumpToUndoNode(parentId, false);

    if (m_filterDock) {
        if (m_filterDock->isFloating())
            m_filterDock->setFloating(false);
        m_filterDock->show();
        m_filterDock->raise();
    }
    // After the jump, so the restore's own filter-list refresh cannot overwrite the values.
    m_filterPanel->openFilterWithParameters(action->filterKey, action->params);
    statusBar()->showMessage(
        tr("Reopened '%1' with its recorded parameters")
            .arg(actionLabel.isEmpty() ? action->filterKey : actionLabel),
        2500);
}

void MainWindow::refreshUndoHistoryPanel()
{
    if (!m_doc || !m_undoHistoryLaneWidget)
        return;

    const auto treeInfo = m_doc->undoTreeInfo();
    const int currentNodeId = m_doc->undoCurrentNodeId();

    // Capture thumbnail (2:1 row icon) and hover snapshot (50% of frame) for the
    // current node if we don't have them yet.
    if (currentNodeId >= 0 && !m_undoNodeThumbnails.contains(currentNodeId)) {
        RenderWidget *view = currentRenderWidget();
        if (view) {
            const QImage frame = view->grabFramebuffer();
            if (!frame.isNull()) {
                // 2:1 thumbnail: centre-crop then scale to 96×48.
                {
                    const int fw = frame.width();
                    const int fh = frame.height();
                    const QSize thumbSize = UndoGraphWidget::thumbnailSize();
                    int cropW, cropH;
                    if (fw >= fh * 2) { cropH = fh; cropW = fh * 2; }
                    else              { cropW = fw; cropH = fw / 2;   }
                    const int ox = (fw - cropW) / 2;
                    const int oy = (fh - cropH) / 2;
                    const QImage cropped = frame.copy(ox, oy, cropW, std::max(1, cropH));
                    m_undoNodeThumbnails[currentNodeId] = QPixmap::fromImage(
                        cropped.scaled(thumbSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
                }
                // 50%-size snapshot for the hover popup.
                m_undoNodeSnapshots[currentNodeId] = QPixmap::fromImage(
                    frame.scaled(std::max(1, frame.width() / 2),
                                 std::max(1, frame.height() / 2),
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
            }
        }
    }

    // Remove thumbnail entries for nodes that no longer exist.
    {
        QSet<int> liveIds;
        for (const auto &info : treeInfo)
            liveIds.insert(info.nodeId);
        for (auto it = m_undoNodeThumbnails.begin(); it != m_undoNodeThumbnails.end(); ) {
            if (!liveIds.contains(it.key()))
                it = m_undoNodeThumbnails.erase(it);
            else
                ++it;
        }
        for (auto it = m_undoNodeSnapshots.begin(); it != m_undoNodeSnapshots.end(); ) {
            if (!liveIds.contains(it.key()))
                it = m_undoNodeSnapshots.erase(it);
            else
                ++it;
        }
    }

    // Convert to QVector and hand off to the lane widget.
    QVector<UndoTreeNodeInfo> vec;
    vec.reserve(static_cast<int>(treeInfo.size()));
    for (const auto &info : treeInfo)
        vec.append(info);
    m_undoHistoryLaneWidget->setNodes(vec, currentNodeId, m_undoNodeThumbnails);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (qobject_cast<RenderWidget *>(watched)) {
        switch (event->type()) {
        case QEvent::DragEnter:
        case QEvent::DragMove:
            return handleDragEnterOrMove(static_cast<QDropEvent *>(event));
        case QEvent::Drop: {
            auto *dropEvent = static_cast<QDropEvent *>(event);
            if (!handleDragEnterOrMove(dropEvent))
                return true;
            handleDroppedUrls(dropEvent->mimeData()->urls());
            return true;
        }
        default:
            break;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

// A running filter pumps the event loop so its Cancel button stays clickable, and it
// pumps AllEvents -- so every other control stays clickable too, while the filter is
// still on the stack holding a live MeshEntry reference. Most filters get away with it
// because their pump is throttled to a progress change every 80 ms; one that shells out
// to a helper pumps every 100 ms for minutes, and the application is plainly live. Either
// way a second filter, or a layer deletion, can run underneath the first one.
//
// The status bar is deliberately left alone: it carries the progress bar and the Cancel
// button, the one control that must keep working.
void MainWindow::setInteractionBlocked(bool blocked)
{
    if (m_interactionBlocked == blocked)
        return;
    m_interactionBlocked = blocked;

    if (blocked) {
        m_focusBeforeBlock = QApplication::focusWidget();
        // Greyed-out controls say "not now" only once the pointer is already over one.
        // The cursor says it everywhere, including over the 3D view, which greys out
        // less visibly than the panels do.
        QApplication::setOverrideCursor(Qt::WaitCursor);
    }

    // Disabling the containers rather than their contents: Qt leaves each child's own
    // enabled flag untouched, so re-enabling restores whatever was greyed out on its
    // own account.
    if (QWidget *bar = menuBar())
        bar->setEnabled(!blocked);
    if (QWidget *central = centralWidget())
        central->setEnabled(!blocked);
    for (QDockWidget *dock : findChildren<QDockWidget *>())
        dock->setEnabled(!blocked);
    for (QToolBar *toolBar : findChildren<QToolBar *>())
        toolBar->setEnabled(!blocked);

    if (!blocked) {
        QApplication::restoreOverrideCursor();
        // Held as a QPointer: the filter may well have deleted whatever had focus.
        if (m_focusBeforeBlock)
            m_focusBeforeBlock->setFocus(Qt::OtherFocusReason);
        m_focusBeforeBlock = nullptr;
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // A filter that shells out to a helper pumps events while it waits, so this close
    // arrives with that filter still on the stack: the window cannot go away underneath
    // it, and a helper that never finishes would keep the application alive with no
    // window to close. Stop the helpers instead and retry the close once the filter has
    // returned -- filterProgressFinished, wired in the constructor, does that.
    if (HelperProcess::anyRunning()) {
        const QString stopping = HelperProcess::runningLabels().join(QStringLiteral(", "));
        m_closeWhenFilterStops = true;
        if (m_doc)
            m_doc->requestOperationCancel();
        HelperProcess::terminateAll();
        statusBar()->showMessage(tr("Stopping %1...").arg(stopping));
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    handleDragEnterOrMove(event);
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    handleDragEnterOrMove(event);
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (!handleDragEnterOrMove(event))
        return;
    handleDroppedUrls(event->mimeData()->urls());
}
