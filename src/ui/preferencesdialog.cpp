#include "preferencesdialog.h"

#include "parameterformbuilder.h"
#include "preferences.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QVBoxLayout>

namespace {
// Whether the help under each row is shown. Remembered with the other UI state, not as a
// preference: it only changes how this dialog looks.
const QString kShowHelpKey = QStringLiteral("preferencesDialog/showHelp");
}

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));

    auto *rootLayout = new QVBoxLayout(this);

    // Typing narrows the list to the preferences whose name, help or options mention every
    // word typed. It has the focus on opening, so the search can start at once.
    auto *search = new QLineEdit(this);
    search->setPlaceholderText(tr("Search preferences"));
    search->setClearButtonEnabled(true);
    rootLayout->addWidget(search);
    search->setFocus();

    auto *intro = new QLabel(
        tr("Changes apply immediately and are remembered between sessions."),
        this);
    intro->setStyleSheet(QStringLiteral("color: palette(mid);"));
    intro->setWordWrap(true);
    rootLayout->addWidget(intro);

    auto *noMatch = new QLabel(this);
    noMatch->setStyleSheet(QStringLiteral("color: palette(mid);"));
    noMatch->hide();
    rootLayout->addWidget(noMatch);

    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    auto *content = new QWidget(scroll);
    m_formLayout = new QFormLayout(content);
    m_formLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    // Left-aligned, so each label starts where the help text under it does.
    m_formLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    scroll->setWidget(content);
    rootLayout->addWidget(scroll, 1);

    Preferences &preferences = Preferences::instance();
    m_form = new ParameterFormBuilder(m_formLayout, content, this);
    // Preferences are app-wide, so the document-coupled editor types never appear
    // here; leaving the context empty makes the builder skip them if one is declared.
    m_form->setAdvancedVisible(true);
    // Every preference explains itself under its row, and can be put back on its own.
    m_form->setShowsInlineHelp(true);
    m_form->setShowsResetButtons(true);
    // The sections are the dialog's top level, so they read as titles over the rows.
    m_form->setGroupHeadingScale(1.15);
    m_form->build(preferences.descriptors(), preferences.values());

    connect(
        m_form,
        &ParameterFormBuilder::valueChanged,
        this,
        [this](const QString &parameterId) {
            // A row back at its default forgets the stored value rather than storing a
            // copy of the default, so the preference follows a later change to it. The
            // editor may round, so the builder's tolerant comparison decides.
            Preferences &preferences = Preferences::instance();
            if (m_form->isDefault(parameterId))
                preferences.resetToDefault(parameterId);
            else
                preferences.setValue(parameterId, m_form->value(parameterId));
        });

    connect(search, &QLineEdit::textChanged, this, [this, noMatch](const QString &text) {
        const bool none = m_form->setFilter(text) == 0;
        noMatch->setText(tr("No preference matches \u201C%1\u201D.").arg(text.trimmed()));
        noMatch->setVisible(none);
    });

    auto *showHelp = new QCheckBox(tr("Show help"), this);
    showHelp->setChecked(QSettings().value(kShowHelpKey, true).toBool());
    m_form->setInlineHelpVisible(showHelp->isChecked());
    connect(showHelp, &QCheckBox::toggled, this, [this](bool shown) {
        m_form->setInlineHelpVisible(shown);
        QSettings().setValue(kShowHelpKey, shown);
    });

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto *resetButton =
        buttons->addButton(tr("Restore All Defaults"), QDialogButtonBox::ResetRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    // Enter in a dialog presses its next auto-default button, and with the focus in the
    // search field or an editor that could be Restore All Defaults. Enter does nothing here.
    for (QAbstractButton *button : buttons->buttons())
        if (auto *push = qobject_cast<QPushButton *>(button))
            push->setAutoDefault(false);
    connect(resetButton, &QPushButton::clicked, this, [this]() {
        Preferences &preferences = Preferences::instance();
        preferences.resetToDefaults();
        // Push the defaults back into the editors. The builder's own reset would do
        // the same, but going through the registry keeps it the single source of
        // truth and emits changed() for every consumer that is listening.
        m_form->setValues(preferences.values());
    });
    auto *bottomRow = new QHBoxLayout;
    bottomRow->addWidget(showHelp);
    bottomRow->addWidget(buttons, 1);
    rootLayout->addLayout(bottomRow);

    resize(600, 640);
}
