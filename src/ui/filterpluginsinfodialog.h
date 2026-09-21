#pragma once

#include "document.h"

#include <QDialog>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

// Inspector for the filter registry: which plugins are loaded, what each one
// contributes, what every descriptor declares, and why a filter is unavailable.
//
// Deliberately *not* a filter browser — MeshFilterPanel already searches and runs
// filters. This dialog answers questions about the registry itself, so it shows
// declared contracts, provenance and ontology coverage rather than offering "run".
//
// Layout: search + grouping controls, a tree (grouped by category or by plugin)
// beside a detail panel, and a summary footer. Grouping by category mirrors the
// real Filters menu, including the cross-listing of multi-category filters, and
// also reveals declared-but-empty categories.
class FilterPluginsInfoDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FilterPluginsInfoDialog(std::vector<Document::FilterInfo> filters,
                                     QWidget *parent = nullptr);

protected:
    // Down in the search box moves into the results, as it does in the Filters panel.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void rebuildTree();
    QTreeWidgetItem *firstFilterItem() const;
    void updateDetail();
    void updateSummary(int shownFilters);

    // Detail renderers, one per kind of selected node.
    QString filterDetailHtml(const Document::FilterInfo &info) const;
    QString pluginDetailHtml(const QString &pluginId) const;
    QString categoryDetailHtml(const QString &category) const;

    bool matches(const Document::FilterInfo &info) const;

    std::vector<Document::FilterInfo> m_filters;
    QLineEdit *m_search = nullptr;
    QComboBox *m_groupBy = nullptr;
    QCheckBox *m_onlyUnavailable = nullptr;
    QTreeWidget *m_tree = nullptr;
    QTextBrowser *m_detail = nullptr;
    QTextBrowser *m_description = nullptr;
    QLabel *m_summary = nullptr;
};
