#include "filedialogdirectory.h"
#include "parameterformbuilder.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVector3D>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace {

QColor colorFromVariant(const QVariant &value, const QColor &fallback)
{
    if (value.userType() == QMetaType::QColor) {
        const QColor c = value.value<QColor>();
        return c.isValid() ? c : fallback;
    }
    const QString s = value.toString().trimmed();
    if (s.isEmpty())
        return fallback;
    const QColor c(s);
    return c.isValid() ? c : fallback;
}

void updateColorButtonStyle(QWidget *button, const QColor &color)
{
    if (!button)
        return;
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: %1; border: 1px solid palette(mid); min-height: 24px; }")
        .arg(color.name(QColor::HexRgb)));
}

class AbsPercEditor : public QWidget
{
public:
    explicit AbsPercEditor(
        double minValue,
        double maxValue,
        int decimals,
        QWidget *parent = nullptr)
        : QWidget(parent)
        , m_minValue(minValue)
        , m_maxValue(maxValue)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        m_absSpin = new QDoubleSpinBox(this);
        m_absSpin->setRange(minValue, maxValue);
        m_absSpin->setDecimals(std::clamp(decimals, 0, 10));
        m_absSpin->setAlignment(Qt::AlignRight);
        m_absSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        const double span = maxValue - minValue;
        if (std::isfinite(span) && std::fabs(span) > 1e-12)
            m_absSpin->setSingleStep(std::max(span / 100.0, 1e-12));

        auto *absLabel = new QLabel(tr("abs"), this);
        absLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));

        m_percentSpin = new QDoubleSpinBox(this);
        m_percentSpin->setRange(-200.0, 200.0);
        m_percentSpin->setDecimals(3);
        m_percentSpin->setSingleStep(0.5);
        m_percentSpin->setAlignment(Qt::AlignRight);
        m_percentSpin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto *percentLabel = new QLabel(tr("%"), this);
        percentLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));

        layout->addWidget(m_absSpin, 1);
        layout->addWidget(absLabel);
        layout->addSpacing(6);
        layout->addWidget(m_percentSpin, 1);
        layout->addWidget(percentLabel);

        const QString rangeText = tr("Percentage is mapped over the range %1 .. %2.")
                                      .arg(QLocale().toString(m_minValue))
                                      .arg(QLocale().toString(m_maxValue));
        m_percentSpin->setToolTip(rangeText);
        percentLabel->setToolTip(rangeText);

        connect(m_absSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            updatePercentFromAbsolute(value);
        });
        connect(m_percentSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double value) {
            updateAbsoluteFromPercent(value);
        });

        updatePercentFromAbsolute(m_absSpin->value());
    }

    void setAbsoluteValue(double value)
    {
        m_absSpin->setValue(value);
    }

    double absoluteValue() const
    {
        return m_absSpin->value();
    }

private:
    void updatePercentFromAbsolute(double value)
    {
        const double denom = m_maxValue - m_minValue;
        if (!std::isfinite(denom) || std::fabs(denom) <= 1e-12) {
            m_percentSpin->setEnabled(false);
            m_percentSpin->setValue(0.0);
            return;
        }

        const QSignalBlocker blocker(m_percentSpin);
        m_percentSpin->setEnabled(true);
        m_percentSpin->setValue((100.0 * (value - m_minValue)) / denom);
    }

    void updateAbsoluteFromPercent(double value)
    {
        const double denom = m_maxValue - m_minValue;
        if (!std::isfinite(denom) || std::fabs(denom) <= 1e-12)
            return;

        const QSignalBlocker blocker(m_absSpin);
        m_absSpin->setValue(m_minValue + (denom * value * 0.01));
    }

    double m_minValue = 0.0;
    double m_maxValue = 0.0;
    QDoubleSpinBox *m_absSpin = nullptr;
    QDoubleSpinBox *m_percentSpin = nullptr;
};

class FilePathEditor : public QWidget
{
public:
    enum class Mode {
        OpenFile,
        SaveFile
    };

    explicit FilePathEditor(
        Mode mode,
        const QString &dialogTitle,
        const QStringList &nameFilters,
        const QString &defaultSuffix,
        const Document *doc,
        QWidget *parent = nullptr)
        : QWidget(parent)
        , m_mode(mode)
        , m_dialogTitle(dialogTitle)
        , m_nameFilters(nameFilters)
        , m_defaultSuffix(defaultSuffix)
        , m_doc(doc)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        m_lineEdit = new QLineEdit(this);
        m_lineEdit->setClearButtonEnabled(true);
        m_lineEdit->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        auto *browseButton = new QToolButton(this);
        browseButton->setText(QStringLiteral("..."));
        browseButton->setToolTip(QObject::tr("Choose file"));

        layout->addWidget(m_lineEdit, 1);
        layout->addWidget(browseButton, 0);

        connect(browseButton, &QToolButton::clicked, this, [this]() {
            QString startPath = m_lineEdit->text().trimmed();
            if (startPath.isEmpty() && m_doc) {
                const int meshIndex = m_doc->currentMeshIndex();
                if (meshIndex >= 0 && meshIndex < m_doc->meshCount()) {
                    const QString sourcePath = m_doc->mesh(meshIndex).sourcePath;
                    if (!sourcePath.isEmpty())
                        startPath = QFileInfo(sourcePath).absolutePath();
                }
            }
            // Last resort, after the field's own text and the current layer's folder: the
            // directory a file dialog was last used in, rather than the working directory.
            if (startPath.isEmpty())
                startPath = FileDialogDirectory::startingDirectory(QStringLiteral("filter"));

            QString chosenPath;
            if (m_mode == Mode::SaveFile) {
                chosenPath = QFileDialog::getSaveFileName(
                    this,
                    m_dialogTitle.isEmpty() ? QObject::tr("Choose File") : m_dialogTitle,
                    startPath,
                    m_nameFilters.join(QStringLiteral(";;")));
                if (!chosenPath.isEmpty()
                    && QFileInfo(chosenPath).suffix().isEmpty()
                    && !m_defaultSuffix.trimmed().isEmpty()) {
                    chosenPath = QStringLiteral("%1.%2").arg(chosenPath, m_defaultSuffix);
                }
            } else {
                chosenPath = QFileDialog::getOpenFileName(
                    this,
                    m_dialogTitle.isEmpty() ? QObject::tr("Choose File") : m_dialogTitle,
                    startPath,
                    m_nameFilters.join(QStringLiteral(";;")));
            }
            if (!chosenPath.isEmpty()) {
                FileDialogDirectory::remember(QStringLiteral("filter"), chosenPath);
                m_lineEdit->setText(chosenPath);
            }
        });
    }

    void setValue(const QString &value)
    {
        m_lineEdit->setText(value);
    }

    QString value() const
    {
        return m_lineEdit->text();
    }

private:
    Mode m_mode = Mode::OpenFile;
    QString m_dialogTitle;
    QStringList m_nameFilters;
    QString m_defaultSuffix;
    const Document *m_doc = nullptr;
    QLineEdit *m_lineEdit = nullptr;
};

class JsonStateEditor : public QWidget
{
public:
    explicit JsonStateEditor(
        const QString &fileDialogTitle,
        const QStringList &fileNameFilters,
        const Document *doc,
        std::function<QString()> currentViewProvider,
        QWidget *parent = nullptr)
        : QWidget(parent)
        , m_currentViewProvider(std::move(currentViewProvider))
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        m_sourceCombo = new QComboBox(this);
        m_sourceCombo->addItem(QObject::tr("Text"), QStringLiteral("text"));
        m_sourceCombo->addItem(QObject::tr("File"), QStringLiteral("file"));
        m_sourceCombo->addItem(QObject::tr("Current View"), QStringLiteral("current"));
        layout->addWidget(m_sourceCombo);

        m_stack = new QStackedWidget(this);

        auto *textPage = new QWidget(m_stack);
        auto *textLayout = new QVBoxLayout(textPage);
        textLayout->setContentsMargins(0, 0, 0, 0);
        textLayout->setSpacing(4);
        m_textEdit = new QPlainTextEdit(textPage);
        m_textEdit->setPlaceholderText(QObject::tr("Paste JSON payload here..."));
        m_textEdit->setMinimumHeight(110);
        textLayout->addWidget(m_textEdit);
        m_stack->addWidget(textPage);

        auto *filePage = new QWidget(m_stack);
        auto *fileLayout = new QVBoxLayout(filePage);
        fileLayout->setContentsMargins(0, 0, 0, 0);
        fileLayout->setSpacing(4);
        m_fileEditor = new FilePathEditor(
            FilePathEditor::Mode::OpenFile,
            fileDialogTitle,
            fileNameFilters,
            QString(),
            doc,
            filePage);
        fileLayout->addWidget(m_fileEditor);
        m_stack->addWidget(filePage);

        auto *currentPage = new QWidget(m_stack);
        auto *currentLayout = new QVBoxLayout(currentPage);
        currentLayout->setContentsMargins(0, 0, 0, 0);
        currentLayout->setSpacing(4);
        m_captureCurrentButton = new QToolButton(currentPage);
        m_captureCurrentButton->setText(QObject::tr("Capture Current View"));
        m_captureCurrentButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
        currentLayout->addWidget(m_captureCurrentButton, 0, Qt::AlignLeft);
        m_currentPreview = new QPlainTextEdit(currentPage);
        m_currentPreview->setReadOnly(true);
        m_currentPreview->setMinimumHeight(110);
        currentLayout->addWidget(m_currentPreview);
        m_stack->addWidget(currentPage);

        layout->addWidget(m_stack);

        connect(m_sourceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
            m_stack->setCurrentIndex(index);
            if (sourceMode() == QStringLiteral("current"))
                refreshCurrentPreview();
        });
        connect(m_captureCurrentButton, &QToolButton::clicked, this, [this]() {
            refreshCurrentPreview();
        });

        m_sourceCombo->setCurrentIndex(2);
        m_stack->setCurrentIndex(2);
        refreshCurrentPreview();
    }

    void setValue(const QString &value)
    {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) {
            m_sourceCombo->setCurrentIndex(0);
            m_stack->setCurrentIndex(0);
            m_textEdit->setPlainText(value);
            return;
        }
        m_sourceCombo->setCurrentIndex(2);
        m_stack->setCurrentIndex(2);
        refreshCurrentPreview();
    }

    QString value() const
    {
        const QString mode = sourceMode();
        if (mode == QStringLiteral("file")) {
            const QString path = m_fileEditor ? m_fileEditor->value().trimmed() : QString();
            if (path.isEmpty())
                return QString();
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
                return QString();
            return QString::fromUtf8(f.readAll()).trimmed();
        }
        if (mode == QStringLiteral("current")) {
            const QString payload = m_currentViewProvider ? m_currentViewProvider().trimmed() : QString();
            if (payload.isEmpty())
                return m_currentPreview ? m_currentPreview->toPlainText().trimmed() : QString();
            return payload;
        }
        return m_textEdit ? m_textEdit->toPlainText().trimmed() : QString();
    }

private:
    QString sourceMode() const
    {
        return m_sourceCombo ? m_sourceCombo->currentData().toString() : QStringLiteral("text");
    }

    void refreshCurrentPreview()
    {
        if (!m_currentPreview)
            return;
        const QString payload = m_currentViewProvider ? m_currentViewProvider().trimmed() : QString();
        if (payload.isEmpty()) {
            m_currentPreview->setPlainText(QObject::tr("Current view payload is unavailable."));
        } else {
            m_currentPreview->setPlainText(payload);
        }
    }

    std::function<QString()> m_currentViewProvider;
    QComboBox *m_sourceCombo = nullptr;
    QStackedWidget *m_stack = nullptr;
    QPlainTextEdit *m_textEdit = nullptr;
    FilePathEditor *m_fileEditor = nullptr;
    QToolButton *m_captureCurrentButton = nullptr;
    QPlainTextEdit *m_currentPreview = nullptr;
};

QString textureChoiceLabel(const Document::MeshEntry &entry, int slotIndex)
{
    const int oneBased = slotIndex + 1;
    const QString name = Document::meshTextureDisplayName(entry, slotIndex);
    return QObject::tr("%1: %2").arg(oneBased).arg(name);
}

class TextureRefEditor : public QWidget
{
public:
    explicit TextureRefEditor(
        Document *doc,
        bool allowAutomatic,
        QWidget *parent = nullptr)
        : QWidget(parent)
        , m_doc(doc)
        , m_allowAutomatic(allowAutomatic)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        m_combo = new QComboBox(this);
        m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(m_combo);
        repopulate(0);
    }

    void setSourceMeshIndex(int meshIndex)
    {
        if (m_sourceMeshIndex == meshIndex)
            return;
        const int preferred = value();
        m_sourceMeshIndex = meshIndex;
        repopulate(preferred);
    }

    void setValue(int value)
    {
        const int pos = m_combo->findData(value);
        if (pos >= 0) {
            m_combo->setCurrentIndex(pos);
            return;
        }
        repopulate(value);
    }

    int value() const
    {
        return m_combo->currentData().toInt();
    }

private:
    void repopulate(int preferredValue)
    {
        const QSignalBlocker blocker(m_combo);
        m_combo->clear();

        bool hasTextures = false;
        if (m_allowAutomatic)
            m_combo->addItem(QObject::tr("Automatic (per-face assignment)"), 0);

        if (m_doc && m_sourceMeshIndex >= 0 && m_sourceMeshIndex < m_doc->meshCount()) {
            const Document::MeshEntry &entry = m_doc->mesh(m_sourceMeshIndex);
            const int textureCount = Document::meshTextureAssociationCount(entry);
            for (int i = 0; i < textureCount; ++i)
                m_combo->addItem(textureChoiceLabel(entry, i), i + 1);
            hasTextures = textureCount > 0;
        }

        if (!hasTextures && !m_allowAutomatic)
            m_combo->addItem(QObject::tr("No textures available"), -1);

        int targetValue = preferredValue;
        if (targetValue <= 0 && m_allowAutomatic)
            targetValue = 0;
        const int pos = m_combo->findData(targetValue);
        if (pos >= 0)
            m_combo->setCurrentIndex(pos);
        else if (m_allowAutomatic && m_combo->count() > 0)
            m_combo->setCurrentIndex(0);
        else if (m_combo->count() > 0)
            m_combo->setCurrentIndex(0);

        m_combo->setEnabled(m_combo->count() > 0);
    }

    Document *m_doc = nullptr;
    bool m_allowAutomatic = true;
    int m_sourceMeshIndex = -1;
    QComboBox *m_combo = nullptr;
};

class TextureOutputRefEditor : public QWidget
{
public:
    explicit TextureOutputRefEditor(
        Document *doc,
        const QString &dialogTitle,
        const QStringList &nameFilters,
        const QString &defaultSuffix,
        QWidget *parent = nullptr)
        : QWidget(parent)
        , m_doc(doc)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        m_combo = new QComboBox(this);
        m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        layout->addWidget(m_combo);

        m_fileEditor = new FilePathEditor(
            FilePathEditor::Mode::SaveFile,
            dialogTitle,
            nameFilters,
            defaultSuffix,
            doc,
            this);
        layout->addWidget(m_fileEditor);

        connect(m_combo, &QComboBox::currentIndexChanged, this, [this]() { updateModeUi(); });
        repopulate({});
    }

    void setSourceMeshIndex(int meshIndex)
    {
        if (m_sourceMeshIndex == meshIndex)
            return;
        const QVariantMap preferred = value();
        m_sourceMeshIndex = meshIndex;
        repopulate(preferred);
    }

    void setValue(const QVariant &rawValue)
    {
        QVariantMap preferred = rawValue.toMap();
        if (preferred.isEmpty()) {
            const QString path = rawValue.toString().trimmed();
            if (!path.isEmpty()) {
                preferred.insert(QStringLiteral("mode"), QStringLiteral("new"));
                preferred.insert(QStringLiteral("path"), path);
            }
        }
        repopulate(preferred);
    }

    QVariantMap value() const
    {
        const int currentData = m_combo->currentData().toInt();
        if (currentData > 0) {
            return QVariantMap{
                { QStringLiteral("mode"), QStringLiteral("existing") },
                { QStringLiteral("slot"), currentData }
            };
        }
        return QVariantMap{
            { QStringLiteral("mode"), QStringLiteral("new") },
            { QStringLiteral("path"), m_fileEditor->value().trimmed() }
        };
    }

private:
    void repopulate(const QVariantMap &preferred)
    {
        const QSignalBlocker blocker(m_combo);
        m_combo->clear();

        if (m_doc && m_sourceMeshIndex >= 0 && m_sourceMeshIndex < m_doc->meshCount()) {
            const Document::MeshEntry &entry = m_doc->mesh(m_sourceMeshIndex);
            const int textureCount = Document::meshTextureAssociationCount(entry);
            for (int i = 0; i < textureCount; ++i)
                m_combo->addItem(QObject::tr("Overwrite %1").arg(textureChoiceLabel(entry, i)), i + 1);
        }

        m_combo->addItem(QObject::tr("Create New Texture File..."), 0);

        const QString mode = preferred.value(QStringLiteral("mode")).toString().trimmed().toLower();
        const int preferredSlot = preferred.value(QStringLiteral("slot")).toInt();
        const QString preferredPath = preferred.value(QStringLiteral("path")).toString().trimmed();
        if (!preferredPath.isEmpty())
            m_fileEditor->setValue(preferredPath);

        int targetValue = 0;
        if (mode == QStringLiteral("existing") && preferredSlot > 0)
            targetValue = preferredSlot;
        const int pos = m_combo->findData(targetValue);
        if (pos >= 0)
            m_combo->setCurrentIndex(pos);
        else if (m_combo->count() > 0)
            m_combo->setCurrentIndex(m_combo->count() - 1);

        m_combo->setEnabled(m_combo->count() > 0);
        updateModeUi();
    }

    void updateModeUi()
    {
        const bool creatingNew = (m_combo->currentData().toInt() <= 0);
        m_fileEditor->setVisible(creatingNew);
    }

    Document *m_doc = nullptr;
    int m_sourceMeshIndex = -1;
    QComboBox *m_combo = nullptr;
    FilePathEditor *m_fileEditor = nullptr;
};

QString groupDisplayName(const QString &group)
{
    const QString trimmed = group.trimmed();
    if (trimmed.isEmpty())
        return QObject::tr("Main");
    QString name = trimmed;
    name.replace(QLatin1Char('_'), QLatin1Char(' '));
    name.replace(QLatin1Char('.'), QLatin1Char(' '));
    if (!name.isEmpty())
        name[0] = name[0].toUpper();
    return name;
}

QString meshComboLabel(const Document &doc, int meshIndex)
{
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return QObject::tr("Mesh %1").arg(meshIndex);

    const Document::MeshEntry &entry = doc.mesh(meshIndex);
    QString label = entry.name.trimmed();
    if (label.isEmpty())
        label = QObject::tr("Mesh %1").arg(meshIndex + 1);
    if (meshIndex == doc.currentMeshIndex())
        label += QObject::tr(" (current)");
    return label;
}

// Editor widget for Point3f parameters.
// Shows a preset combo-box that auto-fills three X/Y/Z spinboxes when a named
// source is selected.  Manually editing a spinbox switches the combo to "Custom".
class Point3fEditor : public QWidget
{
public:
    explicit Point3fEditor(
        const QString &role, // "point" or "direction"
        Document *doc,
        std::function<ParameterFormBuilder::ViewContext()> viewContextProvider,
        QWidget *parent = nullptr)
        : QWidget(parent)
        , m_role(role)
        , m_doc(doc)
        , m_viewContextProvider(std::move(viewContextProvider))
    {
        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        outer->setSpacing(2);

        // --- Preset combo row ---
        m_combo = new QComboBox(this);
        m_combo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        populateCombo();

        auto *comboRow = new QHBoxLayout();
        comboRow->setContentsMargins(0, 0, 0, 0);
        comboRow->setSpacing(2);
        comboRow->addWidget(m_combo, 1);

        // Refresh button — re-fetches the dynamic preset (e.g. current view direction)
        m_refreshBtn = new QToolButton(this);
        m_refreshBtn->setText(QStringLiteral("\u21ba")); // ↺
        m_refreshBtn->setToolTip(QObject::tr("Re-fetch value from selected source"));
        m_refreshBtn->setVisible(false);
        comboRow->addWidget(m_refreshBtn, 0);
        outer->addLayout(comboRow);

        // --- Spinbox row ---
        auto *spinRow = new QHBoxLayout();
        spinRow->setContentsMargins(0, 0, 0, 0);
        spinRow->setSpacing(2);

        const QString labels[3] = { QStringLiteral("X"), QStringLiteral("Y"), QStringLiteral("Z") };
        for (int i = 0; i < 3; ++i) {
            auto *lbl = new QLabel(labels[i], this);
            lbl->setStyleSheet(QStringLiteral("color: palette(mid); padding: 0 1px;"));
            spinRow->addWidget(lbl, 0);
            m_spin[i] = new QDoubleSpinBox(this);
            m_spin[i]->setRange(-1e9, 1e9);
            m_spin[i]->setDecimals(4);
            m_spin[i]->setSingleStep(0.1);
            m_spin[i]->setAlignment(Qt::AlignRight);
            // Fixed compact width so three spinboxes fit side-by-side
            m_spin[i]->setFixedWidth(70);
            spinRow->addWidget(m_spin[i], 0);
        }
        spinRow->addStretch(1);
        outer->addLayout(spinRow);

        // Editing a spinbox switches combo to "Custom"
        for (int i = 0; i < 3; ++i) {
            connect(m_spin[i], qOverload<double>(&QDoubleSpinBox::valueChanged),
                    this, &Point3fEditor::onSpinEdited);
        }
        // Selecting a combo preset fills spinboxes
        connect(m_combo, qOverload<int>(&QComboBox::currentIndexChanged),
                this, &Point3fEditor::onPresetSelected);
        // Refresh button re-applies the current dynamic preset
        connect(m_refreshBtn, &QToolButton::clicked,
                this, [this]() { onPresetSelected(m_combo->currentIndex()); });
    }

    // Start on a named preset rather than a literal value, so a viewpoint parameter can
    // open already holding where the user is looking from. Returns false when the token
    // is unknown or its context provider is unavailable, leaving the caller to fall back
    // to the descriptor's literal default.
    bool selectPreset(const QString &token)
    {
        static const QHash<QString, const char *> kTokenLabels = {
            { QStringLiteral("cameraEye"), QT_TR_NOOP("Camera Eye Position") },
            { QStringLiteral("trackballCenter"), QT_TR_NOOP("Trackball Center") },
            { QStringLiteral("bboxCenter"), QT_TR_NOOP("Mesh BBox Center") },
            { QStringLiteral("viewDirection"), QT_TR_NOOP("View Direction") },
            { QStringLiteral("origin"), QT_TR_NOOP("Origin") },
        };
        const auto it = kTokenLabels.constFind(token);
        if (it == kTokenLabels.constEnd())
            return false;
        const QString label = QObject::tr(*it);
        for (int i = 1; i < int(m_presets.size()); ++i) {
            if (m_presets[i].label != label)
                continue;
            if (m_presets[i].dynamic && !m_viewContextProvider)
                return false;
            const QSignalBlocker b(m_combo);
            m_combo->setCurrentIndex(i);
            onPresetSelected(i);
            // A dynamic preset with no live view leaves the spinboxes at zero; treat that
            // as unavailable so the literal default still wins.
            return m_combo->currentIndex() == i;
        }
        return false;
    }

    void setValue(const QVector3D &v)
    {
        // Set spinboxes quietly, then select "Custom" so the combo reflects the explicit value
        setSpinsQuiet(v);
        selectCustom();
    }

    QVector3D value() const
    {
        return QVector3D(float(m_spin[0]->value()),
                         float(m_spin[1]->value()),
                         float(m_spin[2]->value()));
    }

private:
    // Preset entry: displayed name + optional fixed value (nullopt = dynamic/custom)
    struct Preset {
        QString label;
        QVector3D vec;    // only used when !dynamic
        bool dynamic = false; // requires context provider
    };

    static constexpr int kCustomIndex = 0; // "Custom" is always first

    void setSpinsReadOnly(bool ro)
    {
        for (int i = 0; i < 3; ++i) {
            m_spin[i]->setReadOnly(ro);
            m_spin[i]->setButtonSymbols(ro ? QAbstractSpinBox::NoButtons
                                           : QAbstractSpinBox::UpDownArrows);
            m_spin[i]->setStyleSheet(ro ? QStringLiteral("color: palette(mid);") : QString());
        }
    }

    void updatePresetUi(int idx)
    {
        const bool isCustom = (idx == kCustomIndex);
        const bool isDynamic = !isCustom && idx < int(m_presets.size()) && m_presets[idx].dynamic;
        setSpinsReadOnly(!isCustom);
        m_refreshBtn->setVisible(isDynamic);
    }

    void populateCombo()
    {
        m_presets.clear();
        m_combo->blockSignals(true);

        // Always first: Custom (user-edited)
        m_presets.push_back({ QObject::tr("Custom"), {}, false });
        m_combo->addItem(m_presets.back().label);

        if (m_role == QStringLiteral("direction")) {
            m_presets.push_back({ QObject::tr("+X Axis"),  QVector3D( 1, 0, 0), false });
            m_presets.push_back({ QObject::tr("-X Axis"),  QVector3D(-1, 0, 0), false });
            m_presets.push_back({ QObject::tr("+Y Axis"),  QVector3D( 0, 1, 0), false });
            m_presets.push_back({ QObject::tr("-Y Axis"),  QVector3D( 0,-1, 0), false });
            m_presets.push_back({ QObject::tr("+Z Axis"),  QVector3D( 0, 0, 1), false });
            m_presets.push_back({ QObject::tr("-Z Axis"),  QVector3D( 0, 0,-1), false });
            m_presets.push_back({ QObject::tr("View Direction"),     {}, true });
        } else {
            // "point" role
            m_presets.push_back({ QObject::tr("Origin"),         QVector3D(0, 0, 0), false });
            m_presets.push_back({ QObject::tr("Mesh BBox Center"),   {}, true });
            m_presets.push_back({ QObject::tr("Camera Eye Position"),{}, true });
            m_presets.push_back({ QObject::tr("Trackball Center"),   {}, true });
        }

        for (int i = 1; i < int(m_presets.size()); ++i)
            m_combo->addItem(m_presets[i].label);

        m_combo->blockSignals(false);
    }

    void setSpinsQuiet(const QVector3D &v)
    {
        for (int i = 0; i < 3; ++i) {
            const QSignalBlocker b(m_spin[i]);
            m_spin[i]->setValue(double(v[i]));
        }
    }

    void selectCustom()
    {
        const QSignalBlocker b(m_combo);
        m_combo->setCurrentIndex(kCustomIndex);
        updatePresetUi(kCustomIndex);
    }

    void onSpinEdited()
    {
        // User changed a spinbox manually — switch to Custom (spinboxes stay editable)
        const QSignalBlocker b(m_combo);
        if (m_combo->currentIndex() != kCustomIndex) {
            m_combo->setCurrentIndex(kCustomIndex);
            updatePresetUi(kCustomIndex);
        }
    }

    void onPresetSelected(int idx)
    {
        if (idx < 0 || idx >= int(m_presets.size()))
            return;

        updatePresetUi(idx);

        if (idx == kCustomIndex)
            return; // spinboxes already editable, nothing to fill

        const Preset &p = m_presets[idx];
        if (!p.dynamic) {
            setSpinsQuiet(p.vec);
            return;
        }

        // Dynamic — requires context
        const bool hasMesh = m_doc
            && m_doc->currentMeshIndex() >= 0
            && m_doc->currentMeshIndex() < m_doc->meshCount();

        if (m_role == QStringLiteral("direction")) {
            if (p.label == QObject::tr("View Direction") && m_viewContextProvider) {
                setSpinsQuiet(m_viewContextProvider().viewDirection);
                return;
            }
        } else {
            if (p.label == QObject::tr("Mesh BBox Center") && hasMesh) {
                const vcg::Point3f c = m_doc->mesh(m_doc->currentMeshIndex()).mesh.bbox.Center();
                setSpinsQuiet(QVector3D(c[0], c[1], c[2]));
                return;
            }
            if (p.label == QObject::tr("Camera Eye Position") && m_viewContextProvider) {
                setSpinsQuiet(m_viewContextProvider().eyePosition);
                return;
            }
            if (p.label == QObject::tr("Trackball Center") && m_viewContextProvider) {
                setSpinsQuiet(m_viewContextProvider().trackballCenter);
                return;
            }
        }

        // Provider unavailable — fall back to Custom
        selectCustom();
    }

    QString m_role;
    Document *m_doc = nullptr;
    std::function<ParameterFormBuilder::ViewContext()> m_viewContextProvider;
    std::vector<Preset> m_presets;
    QComboBox *m_combo = nullptr;
    QToolButton *m_refreshBtn = nullptr;
    QDoubleSpinBox *m_spin[3] = { nullptr, nullptr, nullptr };
};

} // namespace

ParameterFormBuilder::ParameterFormBuilder(
    QFormLayout *layout,
    QWidget *parentWidget,
    QObject *parent)
    : QObject(parent)
    , m_layout(layout)
    , m_parentWidget(parentWidget)
{
}

void ParameterFormBuilder::setContext(Context context)
{
    m_context = std::move(context);
}

void ParameterFormBuilder::clear()
{
    m_bindings.clear();
    m_groupHeadings.clear();
    m_hasAdvanced = false;
    if (!m_layout)
        return;
    while (m_layout->rowCount() > 0)
        m_layout->removeRow(0);
}

void ParameterFormBuilder::build(
    const std::vector<MeshFilterParameterDescriptor> &parameters,
    const MeshFilterParameterValues &initialValues)
{
    if (!m_layout || !m_parentWidget)
        return;

    // Collect the parameters by group, keeping the order in which the groups first
    // appear. Walking the descriptor order directly would emit a heading every time the
    // group changed, so a descriptor listing main/advanced/main/advanced produced four
    // headings for two groups.
    std::vector<QString> groupOrder;
    std::map<QString, std::vector<const MeshFilterParameterDescriptor *>> byGroup;
    for (const auto &param : parameters) {
        if (param.id.trimmed().isEmpty())
            continue;
        if (byGroup.find(param.group) == byGroup.end())
            groupOrder.push_back(param.group);
        byGroup[param.group].push_back(&param);
    }
    // A single group needs no header — the whole form is that group.
    const bool showGroupHeaders = groupOrder.size() > 1;

    for (const QString &group : groupOrder) {
    if (showGroupHeaders) {
        auto *groupLabel = new QLabel(groupDisplayName(group), m_parentWidget);
        QFont f = groupLabel->font();
        f.setBold(true);
        groupLabel->setFont(f);
        groupLabel->setStyleSheet(QStringLiteral("color: palette(mid); padding-top: 6px;"));
        m_layout->addRow(groupLabel);
        m_groupHeadings.push_back(
            { groupLabel, group.startsWith(QStringLiteral("advanced"), Qt::CaseInsensitive) });
    }
    for (const MeshFilterParameterDescriptor *paramPtr : byGroup[group]) {
        const MeshFilterParameterDescriptor &param = *paramPtr;

        QWidget *editor = createEditor(param);
        if (!editor)
            continue;

        Binding binding;
        binding.descriptor = param;
        binding.editor = editor;
        binding.advanced = param.isAdvancedGroup();
        m_hasAdvanced = m_hasAdvanced || binding.advanced;

        auto *labelWidget = new QLabel(param.label, m_parentWidget);
        binding.formLabel = labelWidget;
        if (!param.helpMarkdown.trimmed().isEmpty()) {
            labelWidget->setToolTip(param.helpMarkdown);
            editor->setToolTip(param.helpMarkdown);
        }

        m_layout->addRow(labelWidget, editor);

        if (binding.advanced && !m_advancedVisible) {
            labelWidget->hide();
            editor->hide();
        }

        // A stored value wins over the descriptor default, so a caller holding
        // settings can seed the form without rebuilding the editors.
        const auto it = initialValues.constFind(param.id);
        if (it != initialValues.constEnd())
            applyValue(binding, it.value());

        connectEditorSignals(binding);
        m_bindings.push_back(std::move(binding));
    }
    }
    refreshDependentEditors();
    refreshEnabledState();
    // Headings follow their parameters: with the advanced set hidden the "Advanced"
    // heading would otherwise sit above nothing.
    setAdvancedVisible(m_advancedVisible);
}

QWidget *ParameterFormBuilder::createEditor(const MeshFilterParameterDescriptor &param)
{
    Document *doc = m_context.doc;
    switch (param.type) {
    case MeshFilterParameterType::Bool: {
        auto *w = new QCheckBox(m_parentWidget);
        w->setChecked(param.defaultValue.toBool());
        return w;
    }
    case MeshFilterParameterType::Int: {
        auto *w = new QSpinBox(m_parentWidget);
        const int minV = param.minValue.isValid() ? param.minValue.toInt() : std::numeric_limits<int>::lowest();
        const int maxV = param.maxValue.isValid() ? param.maxValue.toInt() : std::numeric_limits<int>::max();
        w->setRange(minV, maxV);
        w->setValue(param.defaultValue.isValid() ? param.defaultValue.toInt() : 0);
        return w;
    }
    case MeshFilterParameterType::Mesh: {
        if (!doc)
            return nullptr;
        auto *w = new QComboBox(m_parentWidget);
        for (int mi = 0; mi < doc->meshCount(); ++mi)
            w->addItem(meshComboLabel(*doc, mi), mi);
        int defaultIndex = param.defaultValue.isValid() ? param.defaultValue.toInt() : doc->currentMeshIndex();
        if (defaultIndex < 0 || defaultIndex >= doc->meshCount())
            defaultIndex = doc->currentMeshIndex();
        if (defaultIndex >= 0) {
            const int pos = w->findData(defaultIndex);
            if (pos >= 0)
                w->setCurrentIndex(pos);
        }
        return w;
    }
    case MeshFilterParameterType::Double: {
        auto *w = new QDoubleSpinBox(m_parentWidget);
        const double minV = param.minValue.isValid() ? param.minValue.toDouble() : -1e12;
        const double maxV = param.maxValue.isValid() ? param.maxValue.toDouble() : 1e12;
        w->setRange(minV, maxV);
        w->setDecimals(std::clamp(param.decimals, 0, 10));
        w->setValue(param.defaultValue.isValid() ? param.defaultValue.toDouble() : 0.0);
        return w;
    }
    case MeshFilterParameterType::AbsPerc: {
        const double minV = param.minValue.isValid() ? param.minValue.toDouble() : 0.0;
        const double maxV = param.maxValue.isValid() ? param.maxValue.toDouble() : 1.0;
        auto *w = new AbsPercEditor(minV, maxV, param.decimals, m_parentWidget);
        w->setAbsoluteValue(param.defaultValue.isValid() ? param.defaultValue.toDouble() : minV);
        return w;
    }
    case MeshFilterParameterType::String: {
        auto *w = new QLineEdit(m_parentWidget);
        w->setText(param.defaultValue.toString());
        return w;
    }
    case MeshFilterParameterType::FileOpen:
    case MeshFilterParameterType::FileSave: {
        auto *w = new FilePathEditor(
            param.type == MeshFilterParameterType::FileOpen
                ? FilePathEditor::Mode::OpenFile
                : FilePathEditor::Mode::SaveFile,
            param.fileDialogTitle,
            param.fileNameFilters,
            param.fileDefaultSuffix,
            doc,
            m_parentWidget);
        w->setValue(param.defaultValue.toString());
        return w;
    }
    case MeshFilterParameterType::TextureRef: {
        if (!doc)
            return nullptr;
        return new TextureRefEditor(doc, param.textureAllowAutomatic, m_parentWidget);
    }
    case MeshFilterParameterType::TextureOutputRef: {
        if (!doc)
            return nullptr;
        auto *w = new TextureOutputRefEditor(
            doc,
            param.fileDialogTitle,
            param.fileNameFilters,
            param.fileDefaultSuffix,
            m_parentWidget);
        w->setValue(param.defaultValue);
        return w;
    }
    case MeshFilterParameterType::Enum: {
        auto *w = new QComboBox(m_parentWidget);
        for (const auto &opt : param.enumOptions)
            w->addItem(opt.label, opt.id);
        const QString defaultId = param.defaultValue.toString();
        if (!defaultId.isEmpty()) {
            const int pos = w->findData(defaultId);
            if (pos >= 0)
                w->setCurrentIndex(pos);
        }
        return w;
    }
    case MeshFilterParameterType::Color: {
        auto *w = new QPushButton(m_parentWidget);
        const QColor c = colorFromVariant(param.defaultValue, QColor(Qt::white));
        w->setProperty("filterColor", c);
        updateColorButtonStyle(w, c);
        const QString parameterId = param.id;
        connect(w, &QPushButton::clicked, this, [this, w, parameterId]() {
            const QColor current = colorFromVariant(w->property("filterColor"), QColor(Qt::white));
            const QColor chosen = QColorDialog::getColor(current, m_parentWidget, tr("Choose Color"));
            if (!chosen.isValid())
                return;
            w->setProperty("filterColor", chosen);
            updateColorButtonStyle(w, chosen);
            emit valueChanged(parameterId);
        });
        return w;
    }
    case MeshFilterParameterType::Point3f: {
        auto *w = new Point3fEditor(param.point3fRole, doc, m_context.viewContextProvider, m_parentWidget);
        const QVector3D defVal = (param.defaultValue.userType() == QMetaType::QVector3D)
            ? param.defaultValue.value<QVector3D>()
            : QVector3D(0.0f, 0.0f, 0.0f);
        // A named preset wins when it can be resolved, so a viewpoint parameter opens
        // holding the current camera rather than a literal that is often unusable.
        if (param.point3fDefaultPreset.isEmpty() || !w->selectPreset(param.point3fDefaultPreset))
            w->setValue(defVal);
        return w;
    }
    case MeshFilterParameterType::CameraState:
    case MeshFilterParameterType::RenderState: {
        if (!doc)
            return nullptr;
        const bool camera = param.type == MeshFilterParameterType::CameraState;
        auto *w = new JsonStateEditor(
            param.fileDialogTitle.isEmpty()
                ? (camera ? tr("Open Camera-State JSON") : tr("Open Render-State JSON"))
                : param.fileDialogTitle,
            param.fileNameFilters.isEmpty()
                ? QStringList{ tr("JSON files (*.json)"), tr("All files (*)") }
                : param.fileNameFilters,
            doc,
            camera ? m_context.cameraStateProvider : m_context.renderStateProvider,
            m_parentWidget);
        w->setValue(param.defaultValue.toString());
        return w;
    }
    }
    return nullptr;
}

// Every editor kind funnels into the single valueChanged signal. The compound
// editors have no signal of their own, so we hang off the child widgets they are
// built from.
void ParameterFormBuilder::connectEditorSignals(const Binding &binding)
{
    QWidget *editor = binding.editor;
    if (!editor)
        return;
    const QString id = binding.descriptor.id;
    auto notify = [this, id]() {
        refreshEnabledState();
        emit valueChanged(id);
    };

    if (auto *w = qobject_cast<QCheckBox *>(editor)) {
        connect(w, &QCheckBox::toggled, this, notify);
    } else if (auto *w = qobject_cast<QSpinBox *>(editor)) {
        connect(w, qOverload<int>(&QSpinBox::valueChanged), this, notify);
    } else if (auto *w = qobject_cast<QDoubleSpinBox *>(editor)) {
        connect(w, qOverload<double>(&QDoubleSpinBox::valueChanged), this, notify);
    } else if (auto *w = qobject_cast<QComboBox *>(editor)) {
        connect(w, qOverload<int>(&QComboBox::currentIndexChanged), this, notify);
    } else if (auto *w = qobject_cast<QLineEdit *>(editor)) {
        connect(w, &QLineEdit::textChanged, this, notify);
    } else if (dynamic_cast<AbsPercEditor *>(editor)
               || dynamic_cast<Point3fEditor *>(editor)) {
        for (QDoubleSpinBox *spin : editor->findChildren<QDoubleSpinBox *>())
            connect(spin, qOverload<double>(&QDoubleSpinBox::valueChanged), this, notify);
    } else if (dynamic_cast<FilePathEditor *>(editor)) {
        for (QLineEdit *lineEdit : editor->findChildren<QLineEdit *>())
            connect(lineEdit, &QLineEdit::textChanged, this, notify);
    } else if (dynamic_cast<TextureRefEditor *>(editor)
               || dynamic_cast<TextureOutputRefEditor *>(editor)) {
        for (QComboBox *combo : editor->findChildren<QComboBox *>())
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, notify);
        for (QLineEdit *lineEdit : editor->findChildren<QLineEdit *>())
            connect(lineEdit, &QLineEdit::textChanged, this, notify);
    } else if (dynamic_cast<JsonStateEditor *>(editor)) {
        for (QComboBox *combo : editor->findChildren<QComboBox *>())
            connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this, notify);
        for (QLineEdit *lineEdit : editor->findChildren<QLineEdit *>())
            connect(lineEdit, &QLineEdit::textChanged, this, notify);
        for (QPlainTextEdit *textEdit : editor->findChildren<QPlainTextEdit *>())
            connect(textEdit, &QPlainTextEdit::textChanged, this, notify);
        for (QToolButton *btn : editor->findChildren<QToolButton *>())
            connect(btn, &QToolButton::clicked, this, notify);
    }
}

QVariant ParameterFormBuilder::readValue(const Binding &binding) const
{
    QWidget *editor = binding.editor;
    if (!editor)
        return {};
    switch (binding.descriptor.type) {
    case MeshFilterParameterType::Bool:
        return qobject_cast<QCheckBox *>(editor)->isChecked();
    case MeshFilterParameterType::Int:
        return qobject_cast<QSpinBox *>(editor)->value();
    case MeshFilterParameterType::Mesh:
        return qobject_cast<QComboBox *>(editor)->currentData().toInt();
    case MeshFilterParameterType::Double:
        return qobject_cast<QDoubleSpinBox *>(editor)->value();
    case MeshFilterParameterType::AbsPerc:
        return dynamic_cast<AbsPercEditor *>(editor)->absoluteValue();
    case MeshFilterParameterType::String:
        return qobject_cast<QLineEdit *>(editor)->text();
    case MeshFilterParameterType::FileOpen:
    case MeshFilterParameterType::FileSave:
        return dynamic_cast<FilePathEditor *>(editor)->value();
    case MeshFilterParameterType::TextureRef:
        return dynamic_cast<TextureRefEditor *>(editor)->value();
    case MeshFilterParameterType::TextureOutputRef:
        return dynamic_cast<TextureOutputRefEditor *>(editor)->value();
    case MeshFilterParameterType::Enum:
        return qobject_cast<QComboBox *>(editor)->currentData().toString();
    case MeshFilterParameterType::Color:
        return colorFromVariant(editor->property("filterColor"), QColor(Qt::white));
    case MeshFilterParameterType::Point3f:
        return QVariant::fromValue(dynamic_cast<Point3fEditor *>(editor)->value());
    case MeshFilterParameterType::CameraState:
    case MeshFilterParameterType::RenderState:
        return dynamic_cast<JsonStateEditor *>(editor)->value();
    }
    return {};
}

void ParameterFormBuilder::applyValue(const Binding &binding, const QVariant &value)
{
    QWidget *editor = binding.editor;
    if (!editor)
        return;
    switch (binding.descriptor.type) {
    case MeshFilterParameterType::Bool:
        if (auto *w = qobject_cast<QCheckBox *>(editor))
            w->setChecked(value.toBool());
        break;
    case MeshFilterParameterType::Int:
        if (auto *w = qobject_cast<QSpinBox *>(editor))
            w->setValue(value.toInt());
        break;
    case MeshFilterParameterType::Mesh:
        if (auto *w = qobject_cast<QComboBox *>(editor)) {
            const int pos = w->findData(value.toInt());
            if (pos >= 0)
                w->setCurrentIndex(pos);
        }
        break;
    case MeshFilterParameterType::Double:
        if (auto *w = qobject_cast<QDoubleSpinBox *>(editor))
            w->setValue(value.toDouble());
        break;
    case MeshFilterParameterType::AbsPerc:
        if (auto *w = dynamic_cast<AbsPercEditor *>(editor))
            w->setAbsoluteValue(value.toDouble());
        break;
    case MeshFilterParameterType::String:
        if (auto *w = qobject_cast<QLineEdit *>(editor))
            w->setText(value.toString());
        break;
    case MeshFilterParameterType::FileOpen:
    case MeshFilterParameterType::FileSave:
        if (auto *w = dynamic_cast<FilePathEditor *>(editor))
            w->setValue(value.toString());
        break;
    case MeshFilterParameterType::TextureRef:
        if (auto *w = dynamic_cast<TextureRefEditor *>(editor))
            w->setValue(value.toInt());
        break;
    case MeshFilterParameterType::TextureOutputRef:
        if (auto *w = dynamic_cast<TextureOutputRefEditor *>(editor))
            w->setValue(value);
        break;
    case MeshFilterParameterType::Enum:
        if (auto *w = qobject_cast<QComboBox *>(editor)) {
            const int pos = w->findData(value.toString());
            if (pos >= 0)
                w->setCurrentIndex(pos);
        }
        break;
    case MeshFilterParameterType::Color: {
        const QColor fallback = colorFromVariant(editor->property("filterColor"), QColor(Qt::white));
        const QColor c = colorFromVariant(value, fallback);
        editor->setProperty("filterColor", c);
        updateColorButtonStyle(editor, c);
        break;
    }
    case MeshFilterParameterType::Point3f:
        if (auto *w = dynamic_cast<Point3fEditor *>(editor)) {
            w->setValue(
                (value.userType() == QMetaType::QVector3D)
                    ? value.value<QVector3D>()
                    : QVector3D(0.0f, 0.0f, 0.0f));
        }
        break;
    case MeshFilterParameterType::CameraState:
    case MeshFilterParameterType::RenderState:
        if (auto *w = dynamic_cast<JsonStateEditor *>(editor))
            w->setValue(value.toString());
        break;
    }
}

MeshFilterParameterValues ParameterFormBuilder::values() const
{
    MeshFilterParameterValues out;
    for (const Binding &binding : m_bindings) {
        const QVariant v = readValue(binding);
        if (v.isValid())
            out.insert(binding.descriptor.id, v);
    }
    return out;
}

QVariant ParameterFormBuilder::value(const QString &parameterId) const
{
    if (const Binding *binding = bindingById(parameterId))
        return readValue(*binding);
    return {};
}

void ParameterFormBuilder::setValues(const MeshFilterParameterValues &values)
{
    for (const Binding &binding : m_bindings) {
        const auto it = values.constFind(binding.descriptor.id);
        if (it != values.constEnd())
            applyValue(binding, it.value());
    }
    refreshDependentEditors();
    refreshEnabledState();
}

void ParameterFormBuilder::resetToDefaults()
{
    for (const Binding &binding : m_bindings)
        applyValue(binding, binding.descriptor.defaultValue);
}

void ParameterFormBuilder::setAdvancedVisible(bool visible)
{
    m_advancedVisible = visible;
    for (const Binding &binding : m_bindings) {
        if (!binding.advanced)
            continue;
        if (binding.formLabel)
            binding.formLabel->setVisible(visible);
        if (binding.editor)
            binding.editor->setVisible(visible);
    }

    // Count the groups that still have something in them. Hiding the advanced
    // parameters can leave a single visible group, and then its heading says nothing
    // the form does not already say -- the same reason a one-group form has no heading
    // at all.
    int visibleGroups = 0;
    for (const GroupHeading &heading : m_groupHeadings)
        if (visible || !heading.advanced)
            ++visibleGroups;

    for (const GroupHeading &heading : m_groupHeadings) {
        if (!heading.label)
            continue;
        const bool wanted = (visible || !heading.advanced) && visibleGroups > 1;
        heading.label->setVisible(wanted);
    }
}

const ParameterFormBuilder::Binding *ParameterFormBuilder::bindingById(
    const QString &parameterId) const
{
    const QString trimmed = parameterId.trimmed();
    if (trimmed.isEmpty())
        return nullptr;
    for (const Binding &binding : m_bindings) {
        if (binding.descriptor.id == trimmed)
            return &binding;
    }
    return nullptr;
}

void ParameterFormBuilder::refreshDependentEditors()
{
    for (const Binding &binding : m_bindings) {
        if (binding.descriptor.type != MeshFilterParameterType::TextureRef
            && binding.descriptor.type != MeshFilterParameterType::TextureOutputRef)
            continue;

        auto applySourceMeshIndex = [&](auto *editor) {
            if (!editor)
                return;

            int sourceMeshIndex = m_context.doc ? m_context.doc->currentMeshIndex() : -1;
            if (!binding.descriptor.textureSourceMeshParameter.trimmed().isEmpty()) {
                if (const Binding *sourceBinding =
                        bindingById(binding.descriptor.textureSourceMeshParameter)) {
                    if (auto *combo = qobject_cast<QComboBox *>(sourceBinding->editor))
                        sourceMeshIndex = combo->currentData().toInt();
                }
            }
            editor->setSourceMeshIndex(sourceMeshIndex);
        };

        if (binding.descriptor.type == MeshFilterParameterType::TextureRef)
            applySourceMeshIndex(dynamic_cast<TextureRefEditor *>(binding.editor));
        else
            applySourceMeshIndex(dynamic_cast<TextureOutputRefEditor *>(binding.editor));
    }
}

// "paramId" enables the row while that bool is true; "!paramId" inverts it.
// An unknown or non-bool id leaves the row enabled, so a typo degrades to the
// previous behaviour rather than greying out a control with no way back.
bool ParameterFormBuilder::evaluateEnabledWhen(const QString &expression) const
{
    QString id = expression.trimmed();
    if (id.isEmpty())
        return true;
    bool negate = false;
    if (id.startsWith(QLatin1Char('!'))) {
        negate = true;
        id = id.mid(1).trimmed();
    }
    const Binding *gate = bindingById(id);
    if (!gate || gate->descriptor.type != MeshFilterParameterType::Bool)
        return true;
    const bool gateValue = readValue(*gate).toBool();
    return negate ? !gateValue : gateValue;
}

void ParameterFormBuilder::refreshEnabledState()
{
    for (const Binding &binding : m_bindings) {
        if (binding.descriptor.enabledWhen.trimmed().isEmpty())
            continue;
        const bool enabled = evaluateEnabledWhen(binding.descriptor.enabledWhen);
        if (binding.formLabel)
            binding.formLabel->setEnabled(enabled);
        if (binding.editor)
            binding.editor->setEnabled(enabled);
    }
}
