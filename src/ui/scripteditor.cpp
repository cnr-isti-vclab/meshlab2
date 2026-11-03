#include "scripteditor.h"
#include "PythonHost.h"

#include <QFont>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QVBoxLayout>

#include <QRegularExpression>

// ---------------------------------------------------------------------------
// Simple Python syntax highlighter
// ---------------------------------------------------------------------------

class PythonHighlighter : public QSyntaxHighlighter
{
public:
    explicit PythonHighlighter(QTextDocument *parent)
        : QSyntaxHighlighter(parent)
    {
        // Keywords
        QTextCharFormat kwFmt;
        kwFmt.setForeground(QColor(QStringLiteral("#569cd6")));
        kwFmt.setFontWeight(QFont::Bold);
        const QStringList keywords = {
            QStringLiteral("and"),  QStringLiteral("as"),   QStringLiteral("assert"),
            QStringLiteral("break"),QStringLiteral("class"),QStringLiteral("continue"),
            QStringLiteral("def"),  QStringLiteral("del"),  QStringLiteral("elif"),
            QStringLiteral("else"), QStringLiteral("except"),QStringLiteral("False"),
            QStringLiteral("finally"),QStringLiteral("for"),QStringLiteral("from"),
            QStringLiteral("global"),QStringLiteral("if"),  QStringLiteral("import"),
            QStringLiteral("in"),   QStringLiteral("is"),   QStringLiteral("lambda"),
            QStringLiteral("None"), QStringLiteral("nonlocal"),QStringLiteral("not"),
            QStringLiteral("or"),   QStringLiteral("pass"), QStringLiteral("raise"),
            QStringLiteral("return"),QStringLiteral("True"),QStringLiteral("try"),
            QStringLiteral("while"),QStringLiteral("with"), QStringLiteral("yield"),
        };
        for (const QString &kw : keywords) {
            _keywordRules.append({
                QRegularExpression(QStringLiteral("\\b%1\\b").arg(kw)),
                kwFmt
            });
        }

        // Built-ins
        QTextCharFormat builtinFmt;
        builtinFmt.setForeground(QColor(QStringLiteral("#dcdcaa")));
        const QStringList builtins = {
            QStringLiteral("print"), QStringLiteral("range"),  QStringLiteral("len"),
            QStringLiteral("list"), QStringLiteral("dict"),   QStringLiteral("set"),
            QStringLiteral("tuple"),QStringLiteral("int"),   QStringLiteral("float"),
            QStringLiteral("str"),  QStringLiteral("bool"),  QStringLiteral("type"),
            QStringLiteral("open"), QStringLiteral("enumerate"),QStringLiteral("zip"),
            QStringLiteral("map"),  QStringLiteral("filter"),QStringLiteral("sorted"),
            QStringLiteral("ms"),   QStringLiteral("mlgui"),   QStringLiteral("pymeshlab2"),
            QStringLiteral("help"),
        };
        for (const QString &b : builtins) {
            _builtinRules.append({
                QRegularExpression(QStringLiteral("\\b%1\\b").arg(b)),
                builtinFmt
            });
        }

        // Numbers
        QTextCharFormat numFmt;
        numFmt.setForeground(QColor(QStringLiteral("#b5cea8")));
        _numberRule.pattern = QRegularExpression(QStringLiteral("\\b[0-9]+(\\.[0-9]+)?\\b"));
        _numberRule.format = numFmt;

        // Strings (single/double)
        QTextCharFormat strFmt;
        strFmt.setForeground(QColor(QStringLiteral("#ce9178")));
        _stringRules.append({ QRegularExpression(QStringLiteral("\"[^\"]*\"")), strFmt });
        _stringRules.append({ QRegularExpression(QStringLiteral("'[^']*'")),  strFmt });

        // Comments
        QTextCharFormat cmtFmt;
        cmtFmt.setForeground(QColor(QStringLiteral("#6a9955")));
        _commentRule.pattern = QRegularExpression(QStringLiteral("#[^\n]*"));
        _commentRule.format = cmtFmt;
    }

protected:
    void highlightBlock(const QString &text) override
    {
        for (const auto &rule : _keywordRules)
            applyRule(rule, text);
        for (const auto &rule : _builtinRules)
            applyRule(rule, text);
        applyRule(_numberRule, text);
        for (const auto &rule : _stringRules)
            applyRule(rule, text);
        applyRule(_commentRule, text);
    }

private:
    struct Rule {
        QRegularExpression pattern;
        QTextCharFormat   format;
    };

    void applyRule(const Rule &rule, const QString &text)
    {
        for (QRegularExpressionMatch m = rule.pattern.match(text);
             m.hasMatch();
             m = rule.pattern.match(text, m.capturedEnd())) {
            setFormat(m.capturedStart(), m.capturedLength(), rule.format);
        }
    }

    QList<Rule> _keywordRules;
    QList<Rule> _builtinRules;
    Rule        _numberRule;
    QList<Rule> _stringRules;
    Rule        _commentRule;
};

// ---------------------------------------------------------------------------
// Editor widget
// ---------------------------------------------------------------------------

static const char *kEditorStyle =
    "QPlainTextEdit {"
    "  background-color: #1e1e1e;"
    "  color: #d4d4d4;"
    "  border: 1px solid #3c3c3c;"
    "  selection-background-color: #264f78;"
    "}";

ScriptEditorWidget::ScriptEditorWidget(QWidget *parent)
    : QWidget(parent)
{
    const QFont monoFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    // --- Editor ----------
    m_editor = new QPlainTextEdit(this);
    m_editor->setFont(monoFont);
    m_editor->setStyleSheet(QString::fromLatin1(kEditorStyle));
    m_editor->setTabStopDistance(
        m_editor->fontMetrics().horizontalAdvance(QStringLiteral("    ")));
    m_editor->setPlaceholderText(
        QStringLiteral("# Python script — Ctrl+Enter or click Run to execute"));
    new PythonHighlighter(m_editor->document());
    layout->addWidget(m_editor, 1);

    // --- Toolbar ----------
    auto *toolbar = new QHBoxLayout();
    toolbar->setContentsMargins(2, 0, 2, 2);
    toolbar->setSpacing(4);

    m_runButton = new QPushButton(QStringLiteral("\u25b6 Run"), this);
    m_runButton->setToolTip(QStringLiteral("Ctrl+Enter / Cmd+Return"));
    connect(m_runButton, &QPushButton::clicked,
            this, &ScriptEditorWidget::executeCode);
    toolbar->addWidget(m_runButton);

    auto *clearButton = new QPushButton(QStringLiteral("Clear"), this);
    connect(clearButton, &QPushButton::clicked,
            this, &ScriptEditorWidget::clearEditor);
    toolbar->addWidget(clearButton);

    toolbar->addStretch();
    layout->addLayout(toolbar);

    // --- Run shortcut -----
    // Qt handles platform-aware modifiers: Qt::ControlModifier maps to
    // Cmd on macOS and Ctrl on other platforms.
    {
        auto *runShortcut = new QShortcut(
            QKeySequence(Qt::ControlModifier | Qt::Key_Return), this);
        connect(runShortcut, &QShortcut::activated,
                this, &ScriptEditorWidget::executeCode);
    }
}

void ScriptEditorWidget::setCode(const QString &code)
{
    if (!m_editor)
        return;
    m_editor->setPlainText(code);
}

void ScriptEditorWidget::focusEditor()
{
    if (m_editor)
        m_editor->setFocus();
}

void ScriptEditorWidget::executeCode()
{
    const QString code = m_editor->toPlainText().trimmed();
    if (code.isEmpty())
        return;

    PythonHost &host = PythonHost::instance();
    if (!host.isInitialized())
        return;

    // Echo a marker so the output area shows where the execution started.
    host.runCode(QStringLiteral("print(\"--- script ---\")"));
    host.runCode(code);
}

void ScriptEditorWidget::clearEditor()
{
    m_editor->clear();
}
