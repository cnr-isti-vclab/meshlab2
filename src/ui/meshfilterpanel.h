#pragma once

#include "document.h"
#include "parameterformbuilder.h"
#include <QColor>
#include <QHash>
#include <QStringList>
#include <QVector3D>
#include <QWidget>
#include <functional>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QFormLayout;
class QTextBrowser;
class QToolButton;

class MeshFilterPanel : public QWidget
{
    Q_OBJECT
public:
    explicit MeshFilterPanel(Document *doc, QWidget *parent = nullptr);

    void reloadFilters();
    void reloadFilters(const std::vector<Document::FilterInfo> &filters);
    void showSearchResults();
    void focusSearch();
    void selectFilterByKey(const QString &filterKey, bool openParameters = true);
    // Opens a filter with a specific set of values rather than the remembered or default
    // ones — used to replay a recorded invocation from the undo history.
    void openFilterWithParameters(const QString &filterKey, const QVariantMap &parameters);

    // The editors live in ParameterFormBuilder now; the alias keeps existing
    // callers of setViewContextProvider() unchanged.
    using ViewContext = ParameterFormBuilder::ViewContext;

    // Provide a function that returns the current camera view context.
    // When set, the Point3f editor exposes context-fill shortcuts.
    void setViewContextProvider(std::function<ViewContext()> fn);

    // Legacy name kept for compatibility.
    void setTrackballCenterProvider(std::function<QVector3D()> fn);

    // Provide current-view JSON snapshots for typed camera/render-state parameters.
    void setCameraStateProvider(std::function<QString()> fn);
    void setRenderStateProvider(std::function<QString()> fn);

signals:
    void runRequested(
        const QString &filterKey,
        const MeshFilterParameterValues &parameters,
        const QString &filterLabel);
    void copyToConsoleRequested(const QString &code);

private slots:
    void onSearchTextChanged(const QString &text);
    void onSearchReturnPressed();
    void onResultItemClicked(QListWidgetItem *item);
    void onResultItemActivated(QListWidgetItem *item);
    void onApplyClicked();
    void onResetParametersClicked();
    void onShowAdvancedToggled(bool checked);

private:
    void buildUi();
    void rebuildResultsList();
    void openFilterAtIndex(int filterIndex);
    void clearParameterEditors();
    void buildParameterEditors(const Document::FilterInfo &filterInfo);
    void refreshCurrentFilterApplicability();
    void cacheCurrentFilterParameters();
    // The form's values plus `selectedOnly`, which the form does not hold: every caller
    // that runs, validates or exports the current filter goes through this.
    MeshFilterParameterValues currentParameterValues() const;
    // Re-reads the selection the current filter would be confined to. `reset` puts the
    // control back to its default -- on when there is a selection -- as opening a filter
    // does; otherwise an explicit choice survives while the selection stays non-empty.
    void refreshSelectionScopeControl(bool reset);
    int currentSelectionScopeMeshIndex() const;
    void updateParameterFormContext();
    bool matchesSearch(const Document::FilterInfo &filterInfo, const QStringList &terms) const;
    bool titleMatchesAllTerms(const Document::FilterInfo &filterInfo, const QStringList &terms) const;
    void openSelectedResult(bool focusApplyButton);
    const Document::FilterInfo *filterByKey(const QString &filterKey) const;
    void showSearchResultsFromUi(bool focusSearch);
    void updateReferenceButtons(const MeshFilterDescriptor &descriptor);
    void showCurrentReferencesBibTeX();
    void openCurrentReferenceLink(bool doi);
    bool eventFilter(QObject *watched, QEvent *event) override;

    Document *m_doc = nullptr;
    std::function<ViewContext()> m_viewContextProvider;
    std::function<QString()> m_cameraStateProvider;
    std::function<QString()> m_renderStateProvider;
    std::vector<Document::FilterInfo> m_filters;
    std::vector<int> m_visibleFilterIndices;
    ParameterFormBuilder *m_paramForm = nullptr;
    QHash<QString, MeshFilterParameterValues> m_filterParameterCache;
    QString m_currentFilterKey;

    QToolButton *m_searchButton = nullptr;
    QLineEdit *m_searchEdit = nullptr;
    QStackedWidget *m_stack = nullptr;
    QWidget *m_resultsPage = nullptr;
    QListWidget *m_resultsList = nullptr;
    QWidget *m_parametersPage = nullptr;
    QLabel *m_filterTitleLabel = nullptr;
    QLabel *m_filterDescriptionLabel = nullptr;
    QLabel *m_filterModifiesLabel = nullptr;
    QToolButton *m_longDescriptionToggle = nullptr;
    QToolButton *m_bibButton = nullptr;
    QToolButton *m_doiButton = nullptr;
    QToolButton *m_webButton = nullptr;
    QTextBrowser *m_longDescriptionView = nullptr;
    QCheckBox *m_showAdvancedCheck = nullptr;
    QCheckBox *m_applyToAllVisible = nullptr;
    // The selection scope of a filter that declares one: shown on its own line under the
    // header, never among the parameters, because it decides where the filter works.
    QCheckBox *m_selectedOnlyCheck = nullptr;
    SelectionScope m_currentScope = SelectionScope::None;
    QString m_currentScopeMeshParameter;
    int m_lastScopeCount = 0;
    QScrollArea *m_parametersScroll = nullptr;
    QWidget *m_parametersWidget = nullptr;
    QFormLayout *m_parametersLayout = nullptr;
    QLabel *m_noParametersLabel = nullptr;
    QPushButton *m_applyButton = nullptr;
    QToolButton *m_resetParametersButton = nullptr;
    QToolButton *m_copyToConsoleButton = nullptr;
    QString m_currentFilterUnavailableReason;
};
