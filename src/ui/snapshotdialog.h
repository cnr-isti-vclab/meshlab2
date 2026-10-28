#pragma once

#include "viewtrackball.h"

#include <QDialog>
#include <QImage>
#include <QSize>
#include <QString>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QResizeEvent;
class QSpinBox;
class CameraShot;
class RenderWidget;

// Everything about a snapshot in one dialog: where it goes, how big, what is behind it and
// whether the gizmos come along, with a preview of the result. It used to be two dialogs in
// a row -- a file chooser, then size -- so the first thing you were asked was the one thing
// you could not judge without seeing the rest.
//
// The preview is a real capture through the same path as the saved image, just smaller, so
// what it shows is what gets written rather than an approximation of it.
class SnapshotDialog : public QDialog
{
    Q_OBJECT
public:
    // How the image is cleared before the scene is drawn.
    enum class Background {
        Transparent,  // alpha 0, the default: composites onto anything
        Current,      // the view's own gradient
        White,
        Black,
    };

    SnapshotDialog(RenderWidget *view, const QString &suggestedPath, QWidget *parent = nullptr);

    QString targetPath() const;
    QSize snapshotSize() const;
    Background background() const;
    bool includeGizmos() const;
    bool addAsRaster() const;
    // Whether the image is framed on the scene, as View > Frame Scene from Here does, and
    // given its shape.
    bool fitToScene() const;

    // Renders through the same path the preview uses, so the file matches what was shown.
    // Returns a null image and sets errorMessage on failure. `shot` and `cameraJson` get the
    // camera the image was taken with, which is not the view's when it is fitted to the scene.
    QImage capture(const QSize &size, QString *errorMessage, CameraShot *shot = nullptr,
                   QString *cameraJson = nullptr) const;

protected:
    void resizeEvent(QResizeEvent *e) override;

private slots:
    // Validates before closing: a path typed straight into the field never went through
    // QFileDialog, so nothing had asked about overwriting.
    void onSaveRequested();

private:
    void schedulePreview();
    void refreshPreview();
    void browseForPath();
    void incrementPath();
    void applySizePreset(int width, int height);

    RenderWidget *m_view = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QSpinBox *m_widthSpin = nullptr;
    QSpinBox *m_heightSpin = nullptr;
    QCheckBox *m_lockAspect = nullptr;
    QComboBox *m_backgroundCombo = nullptr;
    QCheckBox *m_gizmosCheck = nullptr;
    QCheckBox *m_rasterCheck = nullptr;
    QCheckBox *m_fitCheck = nullptr;
    QComboBox *m_gridTileShapeCombo = nullptr;
    QCheckBox *m_gridCaptionsCheck = nullptr;
    QLabel *m_previewLabel = nullptr;
    double m_aspect = 1.0;
    // The camera and the image shape that fit the scene, worked out once when first asked for.
    ViewTrackball::State m_fittedCamera;
    double m_fittedAspect = 1.0;
    bool m_fitted = false;
    double m_aspectBeforeFit = 1.0;
    bool m_resizingFromLock = false;
    bool m_previewPending = false;
    // renderOffscreenToImage spins an event loop, so a queued preview can fire while a
    // capture is already running. Nesting them would let the inner one restore the
    // outer one's temporary settings as if they were the view's own.
    mutable bool m_capturing = false;
};
