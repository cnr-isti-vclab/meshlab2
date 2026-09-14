#include "meshfilterpanel.h"

#include "parameterformbuilder.h"

#include "filterpresentation.h"
#include "filterparam.h"
#include "mathmarkdownrenderer.h"

#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QTextBrowser>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector3D>
#include <QIcon>
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace {

QStringList tokenizeSearchTerms(const QString &text)
{
    return FilterPresentation::tokenize(text);
}

} // namespace

MeshFilterPanel::MeshFilterPanel(Document *doc, QWidget *parent)
    : QWidget(parent)
    , m_doc(doc)
{
    buildUi();
    updateParameterFormContext();
    reloadFilters();
}

void MeshFilterPanel::setViewContextProvider(std::function<ViewContext()> fn)
{
    m_viewContextProvider = std::move(fn);
    updateParameterFormContext();
}

void MeshFilterPanel::setTrackballCenterProvider(std::function<QVector3D()> fn)
{
    // Legacy wrapper: build a ViewContext provider from the old trackball-center-only provider
    m_viewContextProvider = [fn = std::move(fn)]() -> ViewContext {
        const QVector3D c = fn ? fn() : QVector3D(0, 0, 0);
        return ViewContext{ c, c, QVector3D(0, 0, -1) };
    };
    updateParameterFormContext();
}

void MeshFilterPanel::setCameraStateProvider(std::function<QString()> fn)
{
    m_cameraStateProvider = std::move(fn);
    updateParameterFormContext();
}

void MeshFilterPanel::setRenderStateProvider(std::function<QString()> fn)
{
    m_renderStateProvider = std::move(fn);
    updateParameterFormContext();
}

void MeshFilterPanel::updateParameterFormContext()
{
    if (!m_paramForm)
        return;
    ParameterFormBuilder::Context context;
    context.doc = m_doc;
    context.viewContextProvider = m_viewContextProvider;
    context.cameraStateProvider = m_cameraStateProvider;
    context.renderStateProvider = m_renderStateProvider;
    m_paramForm->setContext(std::move(context));
}


void MeshFilterPanel::buildUi()
{
    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(6, 6, 6, 6);
    rootLayout->setSpacing(6);

    auto *searchLayout = new QHBoxLayout();
    searchLayout->setContentsMargins(0, 0, 0, 0);
    searchLayout->setSpacing(4);
    m_searchButton = new QToolButton(this);
    m_searchButton->setAutoRaise(true);
    const QIcon searchIcon = QIcon::fromTheme(QIcon::ThemeIcon::SystemSearch);
    if (!searchIcon.isNull())
        m_searchButton->setIcon(searchIcon);
    else
        m_searchButton->setIcon(style()->standardIcon(QStyle::SP_FileDialogContentsView));
    m_searchButton->setToolTip(tr("Search filters"));
    searchLayout->addWidget(m_searchButton, 0);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(tr("Search filters..."));
    m_searchEdit->installEventFilter(this);
    searchLayout->addWidget(m_searchEdit, 1);
    rootLayout->addLayout(searchLayout);

    m_stack = new QStackedWidget(this);
    rootLayout->addWidget(m_stack, 1);

    m_resultsPage = new QWidget(m_stack);
    auto *resultsLayout = new QVBoxLayout(m_resultsPage);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->setSpacing(4);
    m_resultsList = new QListWidget(m_resultsPage);
    m_resultsList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_resultsList->installEventFilter(this);
    resultsLayout->addWidget(m_resultsList, 1);
    m_stack->addWidget(m_resultsPage);

    m_parametersPage = new QWidget(m_stack);
    auto *paramsPageLayout = new QVBoxLayout(m_parametersPage);
    paramsPageLayout->setContentsMargins(0, 0, 0, 0);
    paramsPageLayout->setSpacing(6);

    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(6);

    m_filterTitleLabel = new QLabel(m_parametersPage);
    QFont titleFont = m_filterTitleLabel->font();
    titleFont.setBold(true);
    m_filterTitleLabel->setFont(titleFont);
    m_filterTitleLabel->setWordWrap(true);
    // Selectable so the name can be copied out -- handy for scripting and bug reports.
    // Mouse only: keyboard selection would put the label in the tab order ahead of the
    // parameter widgets.
    m_filterTitleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_filterTitleLabel->setCursor(Qt::IBeamCursor);
    headerLayout->addWidget(m_filterTitleLabel, 1);

    m_longDescriptionToggle = new QToolButton(m_parametersPage);
    m_longDescriptionToggle->setCheckable(true);
    m_longDescriptionToggle->setChecked(false);
    m_longDescriptionToggle->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_longDescriptionToggle->setText(QStringLiteral("?"));
    m_longDescriptionToggle->setToolTip(tr("Show details"));
    m_longDescriptionToggle->hide();
    auto makeReferenceButton = [this](const QString &text, const QString &tooltip) {
        auto *button = new QToolButton(m_parametersPage);
        button->setText(text);
        button->setToolTip(tooltip);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setAutoRaise(true);
        button->hide();
        return button;
    };
    m_bibButton = makeReferenceButton(QStringLiteral("[bib]"), tr("Show BibTeX"));
    m_doiButton = makeReferenceButton(QStringLiteral("[doi]"), tr("Open publication DOI"));
    m_webButton = makeReferenceButton(QStringLiteral("[web]"), tr("Open publication web page"));
#ifdef MESHLAB2_PYTHON_CONSOLE
    m_copyToConsoleButton = new QToolButton(m_parametersPage);
    m_copyToConsoleButton->setText(QStringLiteral(">_"));
    m_copyToConsoleButton->setToolTip(tr("Copy Python call to console"));
    m_copyToConsoleButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_copyToConsoleButton->setAutoRaise(true);
    m_copyToConsoleButton->hide();
    connect(m_copyToConsoleButton, &QToolButton::clicked, this, [this]() {
        if (m_currentFilterKey.isEmpty())
            return;
        const Document::FilterInfo *info = filterByKey(m_currentFilterKey);
        if (!info)
            return;
        const MeshFilterParameterValues vals = m_paramForm->values();
        emit copyToConsoleRequested(filterCallToPython(info->descriptor, vals, false));
    });
#endif
    m_resetParametersButton = new QToolButton(m_parametersPage);
    m_resetParametersButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_resetParametersButton->setToolTip(tr("Reset parameters to defaults"));
    m_resetParametersButton->setAutoRaise(true);
    m_applyButton = new QPushButton(tr("Apply"), m_parametersPage);
    headerLayout->addWidget(m_longDescriptionToggle, 0, Qt::AlignTop);
    headerLayout->addWidget(m_bibButton, 0, Qt::AlignTop);
    headerLayout->addWidget(m_doiButton, 0, Qt::AlignTop);
    headerLayout->addWidget(m_webButton, 0, Qt::AlignTop);
#ifdef MESHLAB2_PYTHON_CONSOLE
    headerLayout->addWidget(m_copyToConsoleButton, 0, Qt::AlignTop);
#endif
    headerLayout->addWidget(m_resetParametersButton, 0, Qt::AlignTop);
    headerLayout->addWidget(m_applyButton, 0, Qt::AlignTop);
    m_applyToAllVisible = new QCheckBox(tr("All"), m_parametersPage);
    m_applyToAllVisible->setToolTip(tr("Apply to all visible meshes"));
    m_applyToAllVisible->hide();
    headerLayout->addWidget(m_applyToAllVisible, 0, Qt::AlignTop);
    paramsPageLayout->addLayout(headerLayout);

    m_filterDescriptionLabel = new QLabel(m_parametersPage);
    m_filterDescriptionLabel->setWordWrap(true);
    m_filterDescriptionLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    paramsPageLayout->addWidget(m_filterDescriptionLabel);

    m_filterModifiesLabel = new QLabel(m_parametersPage);
    m_filterModifiesLabel->setVisible(false);
    {
        QFont f = m_filterModifiesLabel->font();
        f.setFamily(QStringLiteral("Courier New, Courier, monospace"));
        m_filterModifiesLabel->setFont(f);
    }
    paramsPageLayout->addWidget(m_filterModifiesLabel);

    m_longDescriptionView = new QTextBrowser(m_parametersPage);
    m_longDescriptionView->setOpenExternalLinks(true);
    m_longDescriptionView->setVisible(false);
    m_longDescriptionView->setMaximumHeight(180);
    paramsPageLayout->addWidget(m_longDescriptionView);

    m_showAdvancedCheck = new QCheckBox(tr("Show advanced parameters"), m_parametersPage);
    m_showAdvancedCheck->setChecked(false);
    paramsPageLayout->addWidget(m_showAdvancedCheck);

    m_parametersScroll = new QScrollArea(m_parametersPage);
    m_parametersScroll->setWidgetResizable(true);
    m_parametersWidget = new QWidget(m_parametersScroll);
    m_parametersLayout = new QFormLayout(m_parametersWidget);
    m_parametersLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    m_parametersLayout->setContentsMargins(0, 0, 0, 0);
    m_noParametersLabel = new QLabel(tr("This filter has no parameters."), m_parametersWidget);
    m_noParametersLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    m_parametersLayout->addRow(m_noParametersLabel);

    m_paramForm = new ParameterFormBuilder(m_parametersLayout, m_parametersWidget, this);
    connect(
        m_paramForm,
        &ParameterFormBuilder::valueChanged,
        this,
        [this](const QString &parameterId) {
            if (const auto *binding = m_paramForm->bindingById(parameterId)) {
                if (binding->descriptor.type == MeshFilterParameterType::Mesh)
                    m_paramForm->refreshDependentEditors();
            }
            refreshCurrentFilterApplicability();
        });

    m_parametersScroll->setWidget(m_parametersWidget);
    paramsPageLayout->addWidget(m_parametersScroll, 1);

    m_stack->addWidget(m_parametersPage);
    m_stack->setCurrentWidget(m_resultsPage);

    connect(m_searchEdit, &QLineEdit::textChanged, this, &MeshFilterPanel::onSearchTextChanged);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &MeshFilterPanel::onSearchReturnPressed);
    connect(m_resultsList, &QListWidget::itemClicked, this, &MeshFilterPanel::onResultItemClicked);
    connect(m_resultsList, &QListWidget::itemActivated, this, &MeshFilterPanel::onResultItemActivated);
    connect(m_searchButton, &QToolButton::clicked, this, [this]() { showSearchResultsFromUi(true); });
    connect(m_applyButton, &QPushButton::clicked, this, &MeshFilterPanel::onApplyClicked);
    connect(m_resetParametersButton, &QToolButton::clicked, this, &MeshFilterPanel::onResetParametersClicked);
    connect(m_showAdvancedCheck, &QCheckBox::toggled, this, &MeshFilterPanel::onShowAdvancedToggled);
    connect(m_longDescriptionToggle, &QToolButton::toggled, this, [this](bool checked) {
        if (m_longDescriptionView)
            m_longDescriptionView->setVisible(checked);
    });
    connect(m_bibButton, &QToolButton::clicked,
            this, &MeshFilterPanel::showCurrentReferencesBibTeX);
    connect(m_doiButton, &QToolButton::clicked,
            this, [this]() { openCurrentReferenceLink(true); });
    connect(m_webButton, &QToolButton::clicked,
            this, [this]() { openCurrentReferenceLink(false); });
}

void MeshFilterPanel::reloadFilters()
{
    reloadFilters(m_doc ? m_doc->filterInfos() : std::vector<Document::FilterInfo>{});
}

void MeshFilterPanel::reloadFilters(const std::vector<Document::FilterInfo> &filters)
{
    cacheCurrentFilterParameters();
    const QString previousKey = m_currentFilterKey;
    m_filters = filters;
    std::sort(
        m_filters.begin(),
        m_filters.end(),
        [](const Document::FilterInfo &a, const Document::FilterInfo &b) {
            const int menuCmp =
                a.descriptor.primaryCategory().compare(b.descriptor.primaryCategory(), Qt::CaseInsensitive);
            if (menuCmp != 0)
                return menuCmp < 0;
            return a.descriptor.name.compare(b.descriptor.name, Qt::CaseInsensitive) < 0;
        });

    rebuildResultsList();
    if (!previousKey.isEmpty()) {
        for (int i = 0; i < static_cast<int>(m_filters.size()); ++i) {
            if (m_filters[static_cast<size_t>(i)].key == previousKey) {
                openFilterAtIndex(i);
                return;
            }
        }
    }

    m_currentFilterKey.clear();
    m_stack->setCurrentWidget(m_resultsPage);
}

void MeshFilterPanel::showSearchResults()
{
    m_stack->setCurrentWidget(m_resultsPage);
}

void MeshFilterPanel::focusSearch()
{
    if (!m_searchEdit)
        return;
    m_searchEdit->setFocus(Qt::OtherFocusReason);
    m_searchEdit->selectAll();
}

void MeshFilterPanel::selectFilterByKey(const QString &filterKey, bool openParameters)
{
    if (filterKey.trimmed().isEmpty())
        return;

    for (int i = 0; i < static_cast<int>(m_filters.size()); ++i) {
        if (m_filters[static_cast<size_t>(i)].key != filterKey)
            continue;

        for (int row = 0; row < m_resultsList->count(); ++row) {
            QListWidgetItem *item = m_resultsList->item(row);
            if (!item)
                continue;
            if (item->data(Qt::UserRole).toInt() == i) {
                m_resultsList->setCurrentRow(row);
                break;
            }
        }
        if (openParameters)
            openFilterAtIndex(i);
        return;
    }
}

void MeshFilterPanel::openFilterWithParameters(
    const QString &filterKey,
    const QVariantMap &parameters)
{
    selectFilterByKey(filterKey, true);
    // Applied after the form is built rather than seeded into m_filterParameterCache:
    // openFilterAtIndex() caches the outgoing filter's live values first, which would
    // overwrite a seed whenever the filter being reopened is the one already showing.
    if (m_currentFilterKey != filterKey)
        return;
    m_paramForm->setValues(parameters);
    // Leaving the filter and coming back should not silently revert to the older values.
    m_filterParameterCache.insert(filterKey, m_paramForm->values());
}

void MeshFilterPanel::onSearchTextChanged(const QString &)
{
    showSearchResultsFromUi(false);
    rebuildResultsList();
}

void MeshFilterPanel::onSearchReturnPressed()
{
    if (m_stack->currentWidget() == m_parametersPage) {
        onApplyClicked();
        return;
    }
    openSelectedResult(true);
}

void MeshFilterPanel::onResultItemClicked(QListWidgetItem *item)
{
    if (!item)
        return;
    const int filterIndex = item->data(Qt::UserRole).toInt();
    openFilterAtIndex(filterIndex);
}

void MeshFilterPanel::onResultItemActivated(QListWidgetItem *)
{
    openSelectedResult(true);
}

void MeshFilterPanel::onApplyClicked()
{
    if (m_currentFilterKey.trimmed().isEmpty())
        return;
    const Document::FilterInfo *info = filterByKey(m_currentFilterKey);
    if (!info)
        return;

    const MeshFilterParameterValues parameters = m_paramForm->values();

    if (m_applyToAllVisible && m_applyToAllVisible->isChecked()) {
        // The sweep, its single undo step and its reporting of skipped layers all live in
        // Document, so that the Python API and the tests get the same behaviour.
        m_filterParameterCache.insert(m_currentFilterKey, parameters);
        m_doc->runFilterOnVisibleMeshes(m_currentFilterKey, parameters);
        return;
    }

    QString applicabilityError;
    if (!m_doc->validateFilterInvocation(m_currentFilterKey, parameters, applicabilityError))
        return;
    m_filterParameterCache.insert(m_currentFilterKey, parameters);
    emit runRequested(m_currentFilterKey, parameters, info->descriptor.name);
}

void MeshFilterPanel::onResetParametersClicked()
{
    const QString filterKey = m_currentFilterKey.trimmed();
    if (filterKey.isEmpty())
        return;

    m_filterParameterCache.remove(filterKey);

    Document::FilterInfo resetInfo;
    bool foundInfo = false;
    if (m_doc) {
        const std::vector<Document::FilterInfo> freshInfos = m_doc->filterInfos();
        for (const Document::FilterInfo &info : freshInfos) {
            if (info.key != filterKey)
                continue;
            resetInfo = info;
            foundInfo = true;
            break;
        }
    }
    if (!foundInfo) {
        const Document::FilterInfo *currentInfo = filterByKey(filterKey);
        if (!currentInfo)
            return;
        resetInfo = *currentInfo;
        foundInfo = true;
    }

    for (Document::FilterInfo &info : m_filters) {
        if (info.key == filterKey) {
            info = resetInfo;
            break;
        }
    }

    buildParameterEditors(resetInfo);
    refreshCurrentFilterApplicability();
}

void MeshFilterPanel::onShowAdvancedToggled(bool checked)
{
    m_paramForm->setAdvancedVisible(checked);
}

void MeshFilterPanel::rebuildResultsList()
{
    m_resultsList->clear();
    m_visibleFilterIndices.clear();
    const QStringList terms = tokenizeSearchTerms(m_searchEdit->text());
    std::vector<int> titleFirst;
    std::vector<int> otherMatches;

    for (int i = 0; i < static_cast<int>(m_filters.size()); ++i) {
        const Document::FilterInfo &info = m_filters[static_cast<size_t>(i)];
        if (!matchesSearch(info, terms))
            continue;

        if (titleMatchesAllTerms(info, terms))
            titleFirst.push_back(i);
        else
            otherMatches.push_back(i);
    }

    auto appendResultItem = [this](int filterIndex) {
        const Document::FilterInfo &info = m_filters[static_cast<size_t>(filterIndex)];
        m_visibleFilterIndices.push_back(filterIndex);
        const QString text = info.descriptor.primaryCategory().trimmed().isEmpty()
            ? info.descriptor.name
            : QStringLiteral("%1 / %2").arg(info.descriptor.primaryCategory(), info.descriptor.name);
        auto *item = new QListWidgetItem(text, m_resultsList);
        item->setData(Qt::UserRole, filterIndex);
        QString tip = info.descriptor.shortDescription.trimmed();
        if (!info.applicable && !info.applicabilityError.trimmed().isEmpty()) {
            if (!tip.isEmpty())
                tip += QStringLiteral("\n");
            tip += tr("Unavailable: %1").arg(info.applicabilityError);
            item->setForeground(palette().brush(QPalette::Mid));
        }
        if (!tip.isEmpty())
            item->setToolTip(tip);
    };

    for (int filterIndex : titleFirst)
        appendResultItem(filterIndex);
    for (int filterIndex : otherMatches)
        appendResultItem(filterIndex);

    if (m_resultsList->count() > 0 && m_resultsList->currentRow() < 0)
        m_resultsList->setCurrentRow(0);
}

void MeshFilterPanel::openSelectedResult(bool focusApplyButton)
{
    if (!m_resultsList)
        return;

    QListWidgetItem *item = m_resultsList->currentItem();
    if (!item && m_resultsList->count() > 0) {
        item = m_resultsList->item(0);
        m_resultsList->setCurrentItem(item);
    }
    if (!item)
        return;

    onResultItemClicked(item);
    if (focusApplyButton && m_applyButton && m_stack->currentWidget() == m_parametersPage)
        m_applyButton->setFocus(Qt::OtherFocusReason);
}

void MeshFilterPanel::openFilterAtIndex(int filterIndex)
{
    if (filterIndex < 0 || filterIndex >= static_cast<int>(m_filters.size()))
        return;

    cacheCurrentFilterParameters();

    const Document::FilterInfo &info = m_filters[static_cast<size_t>(filterIndex)];
    m_currentFilterKey = info.key;
    m_filterTitleLabel->setText(info.descriptor.name);
    m_filterDescriptionLabel->setText(info.descriptor.shortDescription);
    const QStringList &mods = info.descriptor.outputModifies;
    if (!mods.isEmpty()) {
        // Words rather than two-letter codes, matching the info dialog.
        m_filterModifiesLabel->setText(
            tr("Modifies: ")
            + FilterPresentation::modifiedDataLabels(mods).join(QStringLiteral(", ")));
        m_filterModifiesLabel->setVisible(true);
    } else {
        m_filterModifiesLabel->setVisible(false);
    }
    QString longDescription = info.descriptor.longDescriptionMarkdown.trimmed();
    const MeshFilterProvenance &provenance = info.descriptor.provenance;
    if (!provenance.isEmpty()) {
        QStringList provenanceLines;
        if (!provenance.project.isEmpty()) {
            const QString project = provenance.repository.isEmpty()
                ? provenance.project
                : QStringLiteral("[%1](%2)").arg(provenance.project, provenance.repository);
            provenanceLines << tr("**Upstream:** %1").arg(project);
        }
        if (!provenance.license.isEmpty())
            provenanceLines << tr("**License:** %1").arg(provenance.license);
        if (!longDescription.isEmpty())
            longDescription += QStringLiteral("\n\n---\n\n");
        longDescription += provenanceLines.join(QStringLiteral("\n\n"));
    }
    if (!info.descriptor.references.empty()) {
        QStringList citations;
        for (const MeshFilterReference &reference : info.descriptor.references)
            citations << reference.markdownCitation();
        if (!longDescription.isEmpty())
            longDescription += QStringLiteral("\n\n---\n\n");
        longDescription += tr("**References**\n\n") + citations.join(QStringLiteral("\n\n"));
    }
    updateReferenceButtons(info.descriptor);
    const bool hasLongDescription = !longDescription.isEmpty();
    m_longDescriptionToggle->setVisible(hasLongDescription);
#ifdef MESHLAB2_PYTHON_CONSOLE
    if (m_copyToConsoleButton)
        m_copyToConsoleButton->setVisible(true);
#endif
    if (hasLongDescription) {
        MathMarkdownRenderer::setMarkdown(*m_longDescriptionView, longDescription);
    } else {
        m_longDescriptionView->clear();
    }
    if (m_longDescriptionToggle->isChecked())
        m_longDescriptionToggle->setChecked(false);
    else
        m_longDescriptionView->setVisible(false);
    if (info.applicable) {
        m_filterDescriptionLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
        m_applyButton->setToolTip(QString());
        m_currentFilterUnavailableReason.clear();
    } else {
        const QString reason = info.applicabilityError.trimmed().isEmpty()
            ? tr("This filter is not available in the current context.")
            : info.applicabilityError.trimmed();
        m_filterDescriptionLabel->setStyleSheet(QStringLiteral("color: #a13a3a;"));
        m_applyButton->setToolTip(tr("Unavailable: %1").arg(reason));
        m_currentFilterUnavailableReason = reason;
    }
    buildParameterEditors(info);
    const auto cacheIt = m_filterParameterCache.constFind(info.key);
    if (cacheIt != m_filterParameterCache.constEnd())
        m_paramForm->setValues(cacheIt.value());
    refreshCurrentFilterApplicability();
    m_stack->setCurrentWidget(m_parametersPage);
}

void MeshFilterPanel::updateReferenceButtons(const MeshFilterDescriptor &descriptor)
{
    bool hasDoi = false;
    bool hasWeb = false;
    for (const MeshFilterReference &reference : descriptor.references) {
        hasDoi |= !reference.doiUrl().isEmpty();
        hasWeb |= !reference.webUrl().isEmpty();
    }
    m_bibButton->setVisible(!descriptor.references.empty());
    m_doiButton->setVisible(hasDoi);
    m_webButton->setVisible(hasWeb);
}

void MeshFilterPanel::showCurrentReferencesBibTeX()
{
    const Document::FilterInfo *info = filterByKey(m_currentFilterKey);
    if (!info || info->descriptor.references.empty())
        return;

    QStringList entries;
    for (const MeshFilterReference &reference : info->descriptor.references)
        entries << reference.bibTeX();
    const QString bibTeX = entries.join(QStringLiteral("\n\n"));

    QDialog dialog(this);
    dialog.setWindowTitle(tr("BibTeX — %1").arg(info->descriptor.name));
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QPlainTextEdit(bibTeX, &dialog);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(text);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    QPushButton *copyButton = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, &dialog,
            [bibTeX] { QGuiApplication::clipboard()->setText(bibTeX); });
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    layout->addWidget(buttons);

    dialog.resize(640, 360);
    dialog.exec();
}

void MeshFilterPanel::openCurrentReferenceLink(bool doi)
{
    const Document::FilterInfo *info = filterByKey(m_currentFilterKey);
    if (!info)
        return;

    std::vector<const MeshFilterReference *> links;
    for (const MeshFilterReference &reference : info->descriptor.references) {
        if (!(doi ? reference.doiUrl() : reference.webUrl()).isEmpty())
            links.push_back(&reference);
    }
    if (links.empty())
        return;

    const MeshFilterReference *selected = links.front();
    if (links.size() > 1) {
        QMenu menu(this);
        for (const MeshFilterReference *reference : links) {
            QAction *action = menu.addAction(reference->label());
            action->setData(doi ? reference->doiUrl() : reference->webUrl());
        }
        QToolButton *button = doi ? m_doiButton : m_webButton;
        QAction *action = menu.exec(button->mapToGlobal(QPoint(0, button->height())));
        if (!action)
            return;
        QDesktopServices::openUrl(QUrl(action->data().toString()));
        return;
    }
    QDesktopServices::openUrl(QUrl(doi ? selected->doiUrl() : selected->webUrl()));
}

bool MeshFilterPanel::matchesSearch(
    const Document::FilterInfo &filterInfo,
    const QStringList &terms) const
{
    // Shared with the Filter Plugins Info dialog; see src/ui/filterpresentation.h.
    return FilterPresentation::matches(filterInfo, terms);
}

bool MeshFilterPanel::titleMatchesAllTerms(
    const Document::FilterInfo &filterInfo,
    const QStringList &terms) const
{
    return FilterPresentation::titleMatchesAll(filterInfo, terms);
}

void MeshFilterPanel::clearParameterEditors()
{
    m_paramForm->clear();
    m_noParametersLabel = new QLabel(tr("This filter has no parameters."), m_parametersWidget);
    m_noParametersLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
    m_parametersLayout->addRow(m_noParametersLabel);
}

void MeshFilterPanel::buildParameterEditors(const Document::FilterInfo &filterInfo)
{
    clearParameterEditors();
    if (filterInfo.descriptor.parameters.empty()) {
        m_noParametersLabel->show();
        if (m_showAdvancedCheck) {
            m_showAdvancedCheck->setChecked(false);
            m_showAdvancedCheck->hide();
        }
        return;
    }
    if (m_noParametersLabel)
        m_noParametersLabel->hide();

    m_paramForm->setAdvancedVisible(m_showAdvancedCheck && m_showAdvancedCheck->isChecked());
    m_paramForm->build(filterInfo.descriptor.parameters);
    refreshCurrentFilterApplicability();

    if (m_showAdvancedCheck) {
        if (m_paramForm->hasAdvanced()) {
            m_showAdvancedCheck->show();
        } else {
            m_showAdvancedCheck->setChecked(false);
            m_showAdvancedCheck->hide();
        }
    }
}


void MeshFilterPanel::refreshCurrentFilterApplicability()
{
    const Document::FilterInfo *info = filterByKey(m_currentFilterKey);
    if (!info || !m_applyButton || !m_filterDescriptionLabel)
        return;

    const MeshFilterParameterValues parameters = m_paramForm->values();
    QString errorMessage;
    const bool applicable = m_doc->validateFilterInvocation(m_currentFilterKey, parameters, errorMessage);

    if (applicable) {
        m_filterDescriptionLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));
        m_applyButton->setToolTip(QString());
    } else {
        const QString reason = errorMessage.trimmed().isEmpty()
            ? (!m_currentFilterUnavailableReason.trimmed().isEmpty()
                   ? m_currentFilterUnavailableReason
                   : tr("This filter is not available in the current context."))
            : errorMessage.trimmed();
        m_filterDescriptionLabel->setStyleSheet(QStringLiteral("color: #a13a3a;"));
        m_applyButton->setToolTip(tr("Unavailable: %1").arg(reason));
    }
    m_applyButton->setEnabled(applicable);
    if (m_applyToAllVisible)
        m_applyToAllVisible->setVisible(
            applicable && info->descriptor.inputDomain == MeshFilterInputDomain::SingleMesh
                         && m_doc->meshCount() > 1);
}



void MeshFilterPanel::cacheCurrentFilterParameters()
{
    if (m_currentFilterKey.trimmed().isEmpty())
        return;
    m_filterParameterCache.insert(m_currentFilterKey, m_paramForm->values());
}



const Document::FilterInfo *MeshFilterPanel::filterByKey(const QString &filterKey) const
{
    if (filterKey.trimmed().isEmpty())
        return nullptr;
    for (const auto &info : m_filters) {
        if (info.key == filterKey)
            return &info;
    }
    return nullptr;
}

void MeshFilterPanel::showSearchResultsFromUi(bool focusSearch)
{
    if (m_stack && m_stack->currentWidget() == m_parametersPage)
        cacheCurrentFilterParameters();
    showSearchResults();
    if (focusSearch && m_searchEdit)
        m_searchEdit->setFocus(Qt::OtherFocusReason);
}

bool MeshFilterPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_searchEdit && event) {
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::FocusIn) {
            showSearchResultsFromUi(false);
        }
    }
    if (watched == m_searchEdit && event && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Down && keyEvent->modifiers() == Qt::NoModifier) {
            showSearchResultsFromUi(false);
            if (m_resultsList && m_resultsList->count() > 0) {
                if (m_resultsList->currentRow() < 0)
                    m_resultsList->setCurrentRow(0);
                m_resultsList->setFocus(Qt::OtherFocusReason);
            }
            return true;
        }
    }
    if (watched == m_resultsList && event && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        const int key = keyEvent->key();
        if ((key == Qt::Key_Return || key == Qt::Key_Enter)
            && keyEvent->modifiers() == Qt::NoModifier) {
            openSelectedResult(true);
            return true;
        }
        if (key == Qt::Key_Up && keyEvent->modifiers() == Qt::NoModifier
            && m_resultsList->currentRow() <= 0) {
            if (m_searchEdit)
                m_searchEdit->setFocus(Qt::OtherFocusReason);
            return true;
        }
        if (key == Qt::Key_Escape) {
            showSearchResultsFromUi(true);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
