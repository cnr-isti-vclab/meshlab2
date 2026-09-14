#pragma once

#include "document.h"
#include <QMainWindow>
#include <QImage>
#include <QStringList>
#include <QList>
#include <QMap>
#include <QPixmap>
#include <QPointer>
#include <QVector>
#include <QVariantMap>
#include <deque>
#include <memory>
#include <vector>

class InteractiveTool;
class RenderWidget;
class LayerWidget;
class MeshFilterPanel;
struct MeshFilterRunResult;
class PythonConsoleWidget;
class QMenu;
class QAction;
class QLabel;
class QProgressBar;
class QSplitter;
class QDockWidget;
class QToolButton;
class QListWidget;
class QTimer;
class UndoGraphWidget;
class MemoryPressureMonitor;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    // How the log panel prefixes each row with a time. Elapsed is measured from
    // application start, which reads more directly than a wall clock when the question is
    // "how long did that take".
    enum class LogTimestampMode {
        None,
        Elapsed,
        Clock
    };

    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void newDocument();
    void newInstance();
    void openFile();
    void openRasterImage();
    void reloadCurrentMesh();
    void reloadAllMeshes();
    void undo();
    void redo();
    void saveCurrentMesh();
    void saveProjectAs();
    void saveSnapshotPng();
    void addSnapshotRaster();
    void openFilterBrowser();
    void runFilterAction();
    void openLastMesh();
    void openRecentMesh();
    void showAbout();
    void showImportPlugins();
    void showFilterPlugins();
    void showMemoryInfo();
    void resetCamera();
    void centerCameraOnSelection();
    void copyCameraState();
    void pasteCameraState();
    void setCurrentViewSceneMode();
    void setCurrentViewParametrizationMode();
    void setCurrentViewRasterMode();
    // Toggles the current view between overlaying its visible layers and giving each one a
    // tile of its own.
    void toggleCurrentViewLayerGrid();
    void splitViewHorizontally();
    void splitViewVertically();

private:
    RenderWidget *currentRenderWidget() const;
    RenderWidget *createRenderWidget(QSplitter *parentSplitter);
    void attachViewShortcuts(RenderWidget *view);
    void updateTextEditingShortcuts(QWidget *focused);
    void setCurrentRenderWidget(RenderWidget *view);
    void updateCurrentViewBorder();
    void splitCurrentView(Qt::Orientation orientation);
    bool closeRenderWidget(RenderWidget *view);
    void closeCurrentView();
    void syncDocumentVisibilityFromCurrentView();
    // errorMessage, when given, receives the reason instead of it going to the status bar,
    // so a batch open can report every failure together rather than each replacing the last.
    bool loadMeshFromPath(const QString &filePath, QString *errorMessage = nullptr);
    bool loadRasterFromPath(const QString &filePath);
    bool handleDragEnterOrMove(QDropEvent *event);
    void setInteractionBlocked(bool blocked);
    void openFilterOrRunIfParameterless(const QString &filterKey);
    void handleDroppedUrls(const QList<QUrl> &urls);
    void addRecentMesh(const QString &filePath);
    void sanitizeRecentMeshes();
    void refreshRecentMeshesMenu();
    void syncCameraViewsFrom(RenderWidget *sourceView);
    void refreshFilterUi();
    // Recomputes applicability for every registered filter, rebuilds the whole filters
    // menu and reloads the filter panel -- so it is posted once per burst rather than run
    // for each of the forty layers a split can add.
    void scheduleFilterUiRefresh();
    void refreshFiltersMenu();
    void refreshFiltersMenu(const std::vector<Document::FilterInfo> &infos);
    void setupToolsMenu(QMenu *toolsMenu);
    void setActiveToolIndex(int index);   // -1 clears the active tool
    void exitActiveTool();                // Esc from a view: uncheck + clear
    void executeFilter(
        const QString &filterKey,
        const QString &fallbackLabel,
        const QVariantMap &parameters = {});
    void applyFilterVisualizationHints(const MeshFilterRunResult &result);
    void updateFrameTimeStats(float cpuMs, float gpuMs, bool gpuTimingSupported, bool gpuSampleValid);
    void refreshUndoHistoryPanel();
    void rebuildLogPanel();
    void appendLogItem(const Document::LogEntry &entry, int entryIndex, bool replaceLast);
    void jumpToUndoNode(int nodeId, bool withCamera = true);
    void reopenFilterFromUndoNode(int nodeId);
    QPixmap captureUndoHistoryThumbnail() const;

    Document *m_doc;
    MemoryPressureMonitor *m_memoryPressureMonitor = nullptr;
    bool m_purgeUndoOnMemoryPressure = false;
    QSplitter *m_viewSplitter = nullptr;
    QList<RenderWidget *> m_renderWidgets;
    RenderWidget *m_currentRenderWidget = nullptr;
    bool m_filterUiRefreshPending = false;
    // Checked state follows the current view, refreshed when the View menu opens so it stays
    // right after an undo or a script changes the arrangement behind the menu's back.
    QAction *m_layerGridAction = nullptr;
    bool m_cameraSyncEnabled = false;
    bool m_syncingCameraViews = false;
    bool m_syncingVisibilityProxy = false;
    // Set when a close arrived while a helper process was still running; the close is
    // retried once the filter that owns it has unwound. See closeEvent().
    bool m_closeWhenFilterStops = false;
    bool m_interactionBlocked = false;
    // Disabling a widget clears its focus and re-enabling does not give it back, so the
    // render view would lose the keyboard after every interactive-tool commit.
    QPointer<QWidget> m_focusBeforeBlock;
    LayerWidget *m_layerWidget;
    MeshFilterPanel *m_filterPanel = nullptr;
    QDockWidget *m_layerDock = nullptr;
    QDockWidget *m_filterDock = nullptr;
    QDockWidget *m_logDock = nullptr;
    QDockWidget *m_pythonConsoleDock = nullptr;
    PythonConsoleWidget *m_pythonConsole = nullptr;
    QToolButton *m_terminalButton = nullptr;
    QMenu *m_recentMenu = nullptr;
    QMenu *m_filtersMenu = nullptr;
    std::vector<std::unique_ptr<InteractiveTool>> m_interactiveTools;
    QList<QAction *> m_toolActions;
    int m_activeToolIndex = -1;
    RenderWidget *m_toolOwnerView = nullptr; // view that hosts the active tool
    QAction *m_openLastAction = nullptr;
    QStringList m_recentMeshes;
    QProgressBar *m_loadProgressBar = nullptr;
    QProgressBar *m_filterProgressBar = nullptr;
    QToolButton *m_filterCancelButton = nullptr;
    QLabel *m_frameStatsLabel = nullptr;
    QAction *m_undoAction = nullptr;
    QAction *m_redoAction = nullptr;
    // Ctrl+C / Ctrl+V for the camera. Scoped to the 3D views, so the text panels keep
    // their own copy and paste; see attachViewShortcuts().
    QAction *m_copyCameraAction = nullptr;
    QAction *m_pasteCameraAction = nullptr;
    // Rebuilt with the Filters menu, so it is kept here to keep its Ctrl+F binding in step
    // with updateTextEditingShortcuts().
    QAction *m_filterBrowserAction = nullptr;
    bool m_textEditingHasFocus = false;
    QListWidget *m_logListWidget = nullptr;
    // Entries quieter than this are kept in the document but not shown; raising the
    // preference repopulates the panel, so past detail can be recovered after the fact.
    Document::LogLevel m_logVerbosity = Document::LogLevel::Info;
    LogTimestampMode m_logTimestampMode = LogTimestampMode::Elapsed;
    UndoGraphWidget *m_undoHistoryLaneWidget = nullptr;
    QLabel *m_undoHistoryPreviewPopup = nullptr;
    QTimer *m_undoHistoryPreviewTimer = nullptr;
    int m_pendingUndoHistoryPreviewNodeId = -1;
    QPoint m_pendingUndoHistoryPreviewGlobalPos;
    QMap<int, QPixmap> m_undoNodeThumbnails; // keyed by nodeId — 2:1 row icon
    QMap<int, QPixmap> m_undoNodeSnapshots;  // keyed by nodeId — 50% size hover image
    std::deque<float> m_lastCpuFrameTimes;
    std::deque<float> m_lastGpuFrameTimes;
};
