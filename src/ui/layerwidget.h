#pragma once

#include <QPair>
#include <cstdint>
#include <utility>
#include <vector>
#include <QVariantMap>
#include <QWidget>
#include <QTreeWidget>
#include <QTableWidget>

class Document;
class QStackedWidget;
class QSplitter;
class QHBoxLayout;
class QToolButton;
class QMenu;

class LayerWidget : public QWidget
{
    Q_OBJECT
public:
    explicit LayerWidget(Document *doc, QWidget *parent = nullptr);

    enum class ViewMode {
        Tree,
        Table
    };

    ViewMode viewMode() const { return m_viewMode; }
    void setViewMode(ViewMode mode);
    void toggleViewMode();

    // The recap shown above the panel: the totals over all layers. The layer count is the
    // document's, and the title bar shows it beside the title.
    QString summaryText() const { return m_summaryText; }
    // Whether any layer in the tree is expanded, which decides what toggleExpandAll() does.
    bool anyLayerExpanded() const;
    // Collapses every layer if any is open, and opens them all if none is.
    void toggleExpandAll();
    // How many layers (meshes and rasters) are shown, and how many there are: what the eye in the
    // title bar draws.
    std::pair<int, int> layerVisibility() const;
    // The eye's action: shows every layer, or hides every layer when they all are shown.
    void toggleAllVisible();
    // Where, from the panel's left edge, the tree's two narrow columns are centred -- the
    // disclosure arrows and the visibility check marks -- so that the title bar can put its
    // two buttons above them.
    int disclosureArrowCenter() const;
    int visibilityCheckCenter() const;

signals:
    // Run a filter now, with these parameters, rather than opening its panel: what a layer
    // rename typed into the table has to be, so that it is undoable, shows in the action
    // history and lands in a generated script like any other rename.
    void filterRunRequested(const QString &filterKey, const QString &label, const QVariantMap &parameters);
    void summaryChanged(const QString &text);
    // Emitted when the answer to anyLayerExpanded() may have changed.
    void expansionChanged(bool anyExpanded);
    // Emitted when the answer to layerVisibility() may have changed.
    void layerVisibilityChanged(int shown, int total);
    void filterActionRequested(const QString &filterKey);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    // Hovering a set's chip previews it: its layers' rows are shaded.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void rebuild();
    // The line above the panel: how many layers, and the totals over all of them.
    void updateSummary();
    // Posts a rebuild and drops any already posted. Every document signal below asks for
    // the same whole-panel rebuild, so a filter that adds forty layers was queueing forty
    // of them -- each one walking every layer, and each landing after the filter had
    // already reported itself finished.
    void scheduleRebuild();
    void rebuildTree();
    void rebuildTable();
    // Columns track their contents until the user drags one, after which they are the
    // user's and nothing moves them again.
    void makeColumnsResizable(QTableWidget *table, int firstResizable);
    // Lets the user hide and show the listed columns from a right-click on the header,
    // remembered under `settingsKey`; see the definition.
    void makeColumnsOptional(QTableWidget *table, const QString &settingsKey,
                             const QList<QPair<int, QString>> &optional,
                             const QList<int> &hiddenByDefault, const QList<int> &placedAfterName);
    void fitColumnsToContents();
    int kvRoleForColumn(int col) const { return Qt::UserRole + 100 + col; }

    enum class LayerItemKind {
        None,
        Mesh,
        Raster
    };
    struct LayerItemRef {
        LayerItemKind kind = LayerItemKind::None;
        int index = -1;
    };
    LayerItemRef layerRefForItem(QTreeWidgetItem *item) const;
    // The layer under a point given in this widget's own coordinates, in whichever view is
    // showing. Each view hit-tests in its viewport's coordinates, not the panel's, so the
    // point has to be mapped before it means anything.
    LayerItemRef layerRefAt(const QPoint &widgetPos) const;
    void updateCurrentItemVisuals();
    // The row of chips under the layers, one for each of the document's visibility sets and a
    // "+" that makes a new one from what is visible now. The chips are made again when the
    // sets' names change and otherwise only have their highlight brought up to date -- the
    // highlight says which set, if any, is what is on screen.
    void refreshVisibilitySetChips();
    void saveVisibilitySet();
    // The name asked of the user for a new set; empty if they cancel.
    QString askVisibilitySetName();
    // "Add to / Remove from Visibility Set" for a layer's right-click menu.
    void addVisibilitySetActions(QMenu &menu, int meshIndex);
    // Shades the rows of the layers of set `index` (-1 for none), without touching the items.
    void previewVisibilitySet(int index);
    bool isPreviewed(const QModelIndex &index) const;
    void showVisibilitySetMenu(int index, const QPoint &globalPos);
    void savePlaneImage(int rasterIndex, int planeIndex);

private slots:
    void onTreeItemChanged(QTreeWidgetItem *item, int column);
    void onTreeCurrentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *previous);
    void onMeshTableCellChanged(QTableWidgetItem *item);
    void onRasterTableCellChanged(QTableWidgetItem *item);
    void onMeshTableCurrentItemChanged(QTableWidgetItem *current, QTableWidgetItem *previous);
    void onRasterTableCurrentItemChanged(QTableWidgetItem *current, QTableWidgetItem *previous);

private:
    Document *m_doc;
    bool m_rebuilding = false;
    // Set once the user drags a column handle, which stops the automatic fitting; the
    // guard tells our own resizes apart from theirs, since the signal does not.
    bool m_columnsUserSized = false;
    bool m_adjustingColumns = false;
    bool m_rebuildPending = false;
    ViewMode m_viewMode = ViewMode::Tree;

    QString m_summaryText;
    QStackedWidget *m_stack = nullptr;
    QTreeWidget *m_meshTree = nullptr;
    QTreeWidget *m_rasterTree = nullptr;
    QTableWidget *m_meshTable = nullptr;
    QTableWidget *m_rasterTable = nullptr;
    QHBoxLayout *m_chipsLayout = nullptr;
    QList<QToolButton *> m_chips;
    QStringList m_chipNames;
    std::vector<std::uint64_t> m_previewMeshIds;
};
