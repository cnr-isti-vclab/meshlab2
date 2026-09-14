#include "snapshotdialog.h"

#include "filedialogdirectory.h"
#include "renderwidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QDir>
#include <QMessageBox>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace {

// Floor, not a fixed size: the preview is what takes the room when the dialog is resized,
// so this is only the smallest it may shrink to.
constexpr int kMinPreviewWidth = 280;
constexpr int kMinPreviewHeight = 200;

// Drawn behind a transparent capture, so "transparent" reads as nothing rather than as
// black -- which is exactly what a plain dark panel behind it would look like.
void paintCheckerboard(QPainter &painter, const QRect &rect, int cell)
{
    const int kCell = qMax(2, cell);
    painter.fillRect(rect, QColor(0x9A, 0x9A, 0x9A));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xC8, 0xC8, 0xC8));
    for (int y = rect.top(); y < rect.bottom(); y += kCell) {
        for (int x = rect.left(); x < rect.right(); x += kCell) {
            if (((x / kCell) + (y / kCell)) % 2 == 0)
                painter.drawRect(QRect(x, y, kCell, kCell));
        }
    }
}

} // namespace

SnapshotDialog::SnapshotDialog(RenderWidget *view, const QString &suggestedPath, QWidget *parent)
    : QDialog(parent)
    , m_view(view)
{
    setWindowTitle(tr("Save Snapshot"));

    auto *layout = new QVBoxLayout(this);

    // --- where it goes. Its own full-width row above the columns: paths are long, and in
    // the form column it showed the tail of one and nothing else.
    m_pathEdit = new QLineEdit(suggestedPath, this);
    m_pathEdit->setToolTip(tr("Where the PNG is written"));
    auto *browse = new QToolButton(this);
    browse->setText(QStringLiteral("..."));
    browse->setToolTip(tr("Choose where to save"));
    connect(browse, &QToolButton::clicked, this, &SnapshotDialog::browseForPath);
    auto *pathRow = new QHBoxLayout();
    pathRow->addWidget(new QLabel(tr("File"), this), 0);
    pathRow->addWidget(m_pathEdit, 1);
    pathRow->addWidget(browse, 0);
    layout->addLayout(pathRow);

    auto *columns = new QHBoxLayout();
    layout->addLayout(columns, 1);

    auto *form = new QFormLayout();
    columns->addLayout(form, 0);

    // --- size
    const qreal dpr = view ? qMax(1.0, view->devicePixelRatioF()) : 1.0;
    const QSize base = view
        ? QSize(qMax(1, int(std::lround(double(view->width()) * dpr))),
                qMax(1, int(std::lround(double(view->height()) * dpr))))
        : QSize(1024, 768);
    m_aspect = (base.height() > 0) ? (double(base.width()) / double(base.height())) : 1.0;

    m_widthSpin = new QSpinBox(this);
    m_widthSpin->setRange(64, 16384);
    m_widthSpin->setValue(base.width());
    m_widthSpin->setSuffix(tr(" px"));
    m_heightSpin = new QSpinBox(this);
    m_heightSpin->setRange(64, 16384);
    m_heightSpin->setValue(base.height());
    m_heightSpin->setSuffix(tr(" px"));
    m_lockAspect = new QCheckBox(tr("Lock aspect ratio"), this);
    m_lockAspect->setChecked(true);
    form->addRow(tr("Width"), m_widthSpin);
    form->addRow(tr("Height"), m_heightSpin);
    form->addRow(QString(), m_lockAspect);

    // --- background
    m_backgroundCombo = new QComboBox(this);
    m_backgroundCombo->addItem(tr("Transparent"), int(Background::Transparent));
    m_backgroundCombo->addItem(tr("Current"), int(Background::Current));
    m_backgroundCombo->addItem(tr("White"), int(Background::White));
    m_backgroundCombo->addItem(tr("Black"), int(Background::Black));
    m_backgroundCombo->setCurrentIndex(0); // transparent by default
    m_backgroundCombo->setToolTip(
        tr("The viewport's gradient is what makes a screenshot hard to reuse, so a snapshot "
           "defaults to no background at all."));
    form->addRow(tr("Background"), m_backgroundCombo);

    // --- gizmos
    m_gizmosCheck = new QCheckBox(tr("Include gizmos and highlights"), this);
    m_gizmosCheck->setChecked(false);
    m_gizmosCheck->setToolTip(
        tr("The trackball sphere and the current-layer outline are there to work with, not "
           "to publish, so they are left out by default."));
    form->addRow(tr("Gizmos"), m_gizmosCheck);

    // --- preview
    auto *previewColumn = new QVBoxLayout();
    columns->addLayout(previewColumn, 1);
    previewColumn->addWidget(new QLabel(tr("Preview"), this), 0);
    m_previewLabel = new QLabel(this);
    m_previewLabel->setMinimumSize(kMinPreviewWidth, kMinPreviewHeight);
    m_previewLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_previewLabel->setFrameShape(QFrame::StyledPanel);
    m_previewLabel->setAlignment(Qt::AlignCenter);
    previewColumn->addWidget(m_previewLabel, 1);

    // The form is as tall as its rows; the preview takes everything else, so enlarging the
    // dialog enlarges the picture rather than the empty space around the controls.
    form->setSizeConstraint(QLayout::SetMinimumSize);

    auto *buttons =
        new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, Qt::Horizontal, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &SnapshotDialog::onSaveRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Aspect lock, which has to guard against the two spin boxes driving each other.
    connect(m_widthSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int w) {
        if (m_lockAspect->isChecked() && !m_resizingFromLock && m_aspect > 0.0) {
            m_resizingFromLock = true;
            m_heightSpin->setValue(qMax(64, int(std::lround(double(w) / m_aspect))));
            m_resizingFromLock = false;
        }
        schedulePreview();
    });
    connect(m_heightSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this](int h) {
        if (m_lockAspect->isChecked() && !m_resizingFromLock) {
            m_resizingFromLock = true;
            m_widthSpin->setValue(qMax(64, int(std::lround(double(h) * m_aspect))));
            m_resizingFromLock = false;
        }
        schedulePreview();
    });
    connect(m_backgroundCombo, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [this](int) { schedulePreview(); });
    connect(m_gizmosCheck, &QCheckBox::toggled, this, [this](bool) { schedulePreview(); });

    schedulePreview();
}

QString SnapshotDialog::targetPath() const
{
    QString path = m_pathEdit->text().trimmed();
    if (!path.isEmpty() && !path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive))
        path += QStringLiteral(".png");
    return path;
}

QSize SnapshotDialog::snapshotSize() const
{
    return QSize(m_widthSpin->value(), m_heightSpin->value());
}

SnapshotDialog::Background SnapshotDialog::background() const
{
    return static_cast<Background>(m_backgroundCombo->currentData().toInt());
}

bool SnapshotDialog::includeGizmos() const
{
    return m_gizmosCheck->isChecked();
}

QImage SnapshotDialog::capture(const QSize &size, QString *errorMessage) const
{
    if (!m_view) {
        if (errorMessage)
            *errorMessage = tr("No view to capture.");
        return QImage();
    }
    if (m_capturing) {
        // Re-entered from the event loop the capture below is spinning. Refusing is right:
        // the settings save/restore around it is not re-entrant, and whoever asked is a
        // preview refresh that the one already running is about to make current anyway.
        if (errorMessage)
            *errorMessage = tr("A capture is already in progress.");
        return QImage();
    }
    m_capturing = true;
    const auto done = qScopeGuard([this] { m_capturing = false; });

    // White, black and "no gizmos" are all just render settings, so the capture is taken
    // with a temporarily modified copy and the view's own settings put back afterwards.
    // Transparency is not a setting -- it skips the background quad and clears to alpha 0 --
    // so it goes through renderOffscreenToImage's own flag.
    const RenderSettings saved = m_view->renderSettings();
    RenderSettings capture = saved;
    const Background mode = background();
    if (mode == Background::White) {
        capture.sceneBackgroundTopColor = Qt::white;
        capture.sceneBackgroundBottomColor = Qt::white;
    } else if (mode == Background::Black) {
        capture.sceneBackgroundTopColor = Qt::black;
        capture.sceneBackgroundBottomColor = Qt::black;
    }
    if (!includeGizmos()) {
        capture.showTrackballGizmo = false;
        capture.highlightCurrentMesh = false;
        capture.showViewCameras = false;
    }

    m_view->setRenderSettings(capture);
    const QImage image =
        m_view->renderOffscreenToImage(size, mode == Background::Transparent, errorMessage);
    m_view->setRenderSettings(saved);
    return image;
}

void SnapshotDialog::schedulePreview()
{
    // Spin boxes emit per keystroke and the aspect lock makes each change emit twice, so
    // coalesce: one capture once the values settle, not one per edit.
    if (m_previewPending)
        return;
    m_previewPending = true;
    QTimer::singleShot(120, this, [this] {
        m_previewPending = false;
        refreshPreview();
    });
}

void SnapshotDialog::refreshPreview()
{
    if (!m_previewLabel)
        return;
    if (m_capturing) {
        schedulePreview(); // try again once the running capture finishes
        return;
    }

    // Everything here is in DEVICE pixels, with the ratio applied only when the finished
    // pixmap is handed to the label. Mixing the two is what left the preview drawn at half
    // size in a corner: grabFramebuffer() tags its result with the widget's device pixel
    // ratio, so drawImage() was placing a 2x image at 1x coordinates and halving it.
    const qreal dpr = qMax(1.0, devicePixelRatioF());
    const QSize logical = m_previewLabel->size();
    const QSize device = (QSizeF(logical) * dpr).toSize();
    if (device.isEmpty())
        return;

    QPixmap canvas(device);
    canvas.setDevicePixelRatio(1.0);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    const QRect area(QPoint(0, 0), device);

    if (background() == Background::Transparent)
        paintCheckerboard(painter, area, int(std::lround(8.0 * dpr)));
    else
        painter.fillRect(area, m_previewLabel->palette().window());

    // Capture at the preview's own size with the requested aspect, so what is shown is
    // framed exactly as the file will be.
    QSize captureSize = snapshotSize().scaled(device, Qt::KeepAspectRatio);
    captureSize = captureSize.expandedTo(QSize(64, 64));

    QString error;
    QImage shot = capture(captureSize, &error);
    if (shot.isNull()) {
        painter.setPen(m_previewLabel->palette().color(QPalette::Mid));
        QFont f = painter.font();
        f.setPointSizeF(f.pointSizeF() * dpr);
        painter.setFont(f);
        painter.drawText(area, Qt::AlignCenter, tr("Preview unavailable"));
    } else {
        shot.setDevicePixelRatio(1.0);
        QImage scaled = shot.scaled(device, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        scaled.setDevicePixelRatio(1.0);
        painter.drawImage(QPoint((device.width() - scaled.width()) / 2,
                                 (device.height() - scaled.height()) / 2),
                          scaled);
    }
    painter.end();

    canvas.setDevicePixelRatio(dpr);
    m_previewLabel->setPixmap(canvas);
}

void SnapshotDialog::resizeEvent(QResizeEvent *e)
{
    QDialog::resizeEvent(e);
    // The preview is a capture at the size of the area it is shown in, so growing the
    // dialog has to re-take it rather than upscale what was already there. Debounced, since
    // a drag on the window edge delivers a resize per pixel.
    schedulePreview();
}

void SnapshotDialog::onSaveRequested()
{
    const QString path = targetPath();
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Save Snapshot"),
                             tr("Enter a file name for the snapshot."));
        m_pathEdit->setFocus();
        return;
    }

    const QFileInfo info(path);
    if (info.exists()) {
        if (info.isDir()) {
            QMessageBox::warning(
                this, tr("Save Snapshot"),
                tr("'%1' is a folder. Give the snapshot a file name inside it.")
                    .arg(QDir::toNativeSeparators(path)));
            m_pathEdit->setFocus();
            return;
        }
        // QFileDialog asks this for you, but only when the path came from the browser: a
        // path typed into the field, or one left over from last time, never went near it.
        const auto answer = QMessageBox::question(
            this, tr("Overwrite?"),
            tr("'%1' already exists.\n\nReplace it?")
                .arg(QDir::toNativeSeparators(path)),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes)
            return;
    }

    const QDir parent = info.absoluteDir();
    if (!parent.exists()) {
        QMessageBox::warning(this, tr("Save Snapshot"),
                             tr("The folder '%1' does not exist.")
                                 .arg(QDir::toNativeSeparators(parent.path())));
        m_pathEdit->setFocus();
        return;
    }

    accept();
}

void SnapshotDialog::browseForPath()
{
    const QString chosen = QFileDialog::getSaveFileName(
        this,
        tr("Save Snapshot"),
        m_pathEdit->text().trimmed(),
        tr("PNG Image (*.png)"));
    if (chosen.isEmpty())
        return;
    m_pathEdit->setText(chosen);
}
