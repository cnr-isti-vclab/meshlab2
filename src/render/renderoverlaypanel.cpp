#include "renderoverlaypanel.h"
#include "colormap.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace {

// Only these five passes have an apply-to-all: RenderWidget copies their fields and ignores
// the rest, so offering the gesture anywhere else would look like it had done something.
bool passSupportsApplyToAll(RenderPass pass)
{
    switch (pass) {
    case RenderPass::BoundingBox:
    case RenderPass::Points:
    case RenderPass::Edges:
    case RenderPass::Wireframe:
    case RenderPass::Fill:
        return true;
    default:
        return false;
    }
}
const QColor kAccentColor(36, 132, 210);
const QColor kNeutralArrowColor(90, 90, 90, 175);
const QColor kActiveArrowColor(36, 132, 210, 235);
constexpr int kPassButtonSize = 28;
constexpr int kPassIconSize = 24;
constexpr int kPassArrowWidth = 28;
constexpr int kPassArrowHeight = 10;
constexpr int kSettingsRowHeight = 20;
constexpr int kColorButtonSize = 16;
constexpr int kSettingsAutoCloseDelayMs = 5000;
constexpr Qt::Alignment kSettingsLabelAlignment = Qt::AlignRight | Qt::AlignVCenter;
constexpr int kPbrSourceRole = Qt::UserRole + 100;
constexpr int kPbrTextureIndexRole = Qt::UserRole + 101;

QWidget *makeCenteredFieldContainer(QWidget *fieldWidget, QWidget *parentWidget)
{
    auto *container = new QWidget(parentWidget);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    const QSize fieldHint = fieldWidget->sizeHint();
    if (auto *checkBox = qobject_cast<QCheckBox *>(fieldWidget)) {
        Q_UNUSED(checkBox);
        // Keep checkbox indicator fully visible even in narrow field columns.
        fieldWidget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        fieldWidget->setMinimumWidth(fieldHint.width() + 2);
        fieldWidget->setMaximumWidth(fieldHint.width() + 2);
    }

    layout->addWidget(fieldWidget, 0, Qt::AlignLeft | Qt::AlignVCenter);
    layout->addStretch(1);
    container->setMinimumWidth(fieldHint.width() + 2);
    container->setMinimumHeight(kSettingsRowHeight);
    return container;
}

void applyUniformFormRowHeights(QFormLayout *form)
{
    if (!form)
        return;

    for (int row = 0; row < form->rowCount(); ++row) {
        if (QLayoutItem *labelItem = form->itemAt(row, QFormLayout::LabelRole)) {
            if (QWidget *labelWidget = labelItem->widget()) {
                if (auto *label = qobject_cast<QLabel *>(labelWidget))
                    label->setAlignment(kSettingsLabelAlignment);
                QSizePolicy labelPolicy = labelWidget->sizePolicy();
                labelPolicy.setVerticalPolicy(QSizePolicy::Fixed);
                labelWidget->setSizePolicy(labelPolicy);
                labelWidget->setMinimumHeight(kSettingsRowHeight);
            }
        }

        if (QLayoutItem *fieldItem = form->itemAt(row, QFormLayout::FieldRole)) {
            if (QWidget *fieldWidget = fieldItem->widget()) {
                if (fieldWidget->minimumHeight() != fieldWidget->maximumHeight())
                    fieldWidget->setMinimumHeight(kSettingsRowHeight);
            }
        }
    }
}

void setHistogramRowVisible(QLabel *label, QWidget *field, bool visible)
{
    if (label)
        label->setVisible(visible);
    if (field)
        field->setVisible(visible);
}

class PassArrowButton final : public QToolButton
{
public:
    explicit PassArrowButton(QWidget *parent = nullptr)
        : QToolButton(parent)
    {
        setCheckable(true);
        setAutoRaise(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(kPassArrowWidth, kPassArrowHeight);
        setToolTip(QObject::tr("Show settings for this pass"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        QColor arrowColor = isChecked() ? kActiveArrowColor : kNeutralArrowColor;
        if (underMouse() || isDown())
            arrowColor = kActiveArrowColor;
        p.setBrush(arrowColor);

        const int triW = 14;
        const int triH = 8;
        const int cx = width() / 2;
        const int top = (height() - triH) / 2;
        QPolygon poly;
        poly << QPoint(cx - triW / 2, top)
             << QPoint(cx + triW / 2, top)
             << QPoint(cx, top + triH);
        p.drawPolygon(poly);
    }
};

class CurrentPageStackedWidget final : public QStackedWidget
{
public:
    explicit CurrentPageStackedWidget(QWidget *parent = nullptr)
        : QStackedWidget(parent)
    {
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        connect(this, &QStackedWidget::currentChanged, this, [this]() {
            updateGeometry();
        });
    }

    QSize sizeHint() const override
    {
        if (QWidget *page = currentWidget())
            return page->sizeHint();
        return QStackedWidget::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        if (QWidget *page = currentWidget())
            return page->minimumSizeHint();
        return QStackedWidget::minimumSizeHint();
    }
};
}

RenderOverlayPanel::RenderOverlayPanel(QWidget *parent)
    : QWidget(parent)
{
    m_settingsAutoCloseTimer = new QTimer(this);
    m_settingsAutoCloseTimer->setSingleShot(true);
    m_settingsAutoCloseTimer->setInterval(kSettingsAutoCloseDelayMs);
    connect(m_settingsAutoCloseTimer, &QTimer::timeout, this, [this]() {
        if (m_settingsContainer && m_settingsContainer->isVisible())
            setSettingsVisible(false);
    });
    if (qApp)
        qApp->installEventFilter(this);

    auto *panelLayout = new QVBoxLayout(this);
    panelLayout->setContentsMargins(0, 0, 0, 0);
    panelLayout->setSpacing(1);

    auto *buttonRow = new QWidget(this);
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(3);
    panelLayout->addWidget(buttonRow);

    const QString passButtonStyle = QStringLiteral(
        "QToolButton { background: rgba(250,250,250,165); border: 1px solid rgba(40,40,40,115); border-radius: 4px; }"
        "QToolButton:checked { background: rgba(%1,%2,%3,195); border-color: rgba(%1,%2,%3,220); }"
        "QToolButton:hover { background: rgba(220,230,245,185); }"
        "QToolButton[settingsTarget=\"true\"] { border: 2px solid rgba(%1,%2,%3,210); }")
            .arg(kAccentColor.red()).arg(kAccentColor.green()).arg(kAccentColor.blue());

    auto makeButton = [this, &passButtonStyle](const QString &iconPath, const QString &tooltip) {
        auto *btn = new QToolButton(this);
        btn->setIcon(QIcon(iconPath));
        btn->setToolTip(tooltip);
        btn->setCheckable(true);
        btn->setAutoRaise(false);
        btn->setIconSize(QSize(kPassIconSize, kPassIconSize));
        btn->setFixedSize(kPassButtonSize, kPassButtonSize);
        btn->setStyleSheet(passButtonStyle);
        return btn;
    };

    m_currentMeshButton = makeButton(QStringLiteral(":/img/global.png"), tr("Viewer Settings"));
    m_currentMeshButton->setCheckable(false);
    m_normalsDecoratorsButton = makeButton(QStringLiteral(":/img/normals.png"), tr("Normal Decorators"));
    m_boundaryDecoratorsButton = makeButton(QStringLiteral(":/img/boundary.png"), tr("Boundary Decorators"));
    m_bboxButton = makeButton(QStringLiteral(":/img/box.png"), tr("Bounding Box"));
    m_pointsButton = makeButton(QStringLiteral(":/img/points.png"), tr("Points"));
    m_edgesButton = makeButton(QStringLiteral(":/img/edge-mesh.png"), tr("Edges pass"));
    m_wireButton = makeButton(QStringLiteral(":/img/wire.png"), tr("Wireframe pass"));
    m_fillButton = makeButton(QStringLiteral(":/img/flat.png"), tr("Fill pass"));
    m_selectionButton =
        makeButton(QStringLiteral(":/img/selected.png"), tr("Selected elements overlay"));
    m_qualityHistogramButton =
        makeButton(QStringLiteral(":/img/histogram.png"), tr("Quality Histogram"));

    buttonLayout->addWidget(m_currentMeshButton);
    buttonLayout->addWidget(m_bboxButton);
    buttonLayout->addWidget(m_pointsButton);
    buttonLayout->addWidget(m_edgesButton);
    buttonLayout->addWidget(m_wireButton);
    buttonLayout->addWidget(m_fillButton);
    buttonLayout->addWidget(m_selectionButton);
    buttonLayout->addWidget(m_normalsDecoratorsButton);
    buttonLayout->addWidget(m_boundaryDecoratorsButton);
    buttonLayout->addWidget(m_qualityHistogramButton);

    m_normalsDecoratorsButton->setChecked(false);
    m_boundaryDecoratorsButton->setChecked(false);
    m_bboxButton->setChecked(false);
    m_pointsButton->setChecked(false);
    m_edgesButton->setChecked(false);
    m_wireButton->setChecked(true);
    m_fillButton->setChecked(true);
    m_selectionButton->setChecked(false);
    m_qualityHistogramButton->setChecked(false);

    auto *arrowRow = new QWidget(this);
    auto *arrowLayout = new QHBoxLayout(arrowRow);
    arrowLayout->setContentsMargins(0, 0, 0, 0);
    arrowLayout->setSpacing(3);
    panelLayout->addWidget(arrowRow);

    auto makeArrowButton = [this, arrowRow](const QString &tooltip) {
        auto *btn = new PassArrowButton(arrowRow);
        btn->setToolTip(tooltip);
        return btn;
    };
    auto makeColorButton = [this](QWidget *parentWidget) {
        auto *btn = new QPushButton(parentWidget);
        btn->setText(QString());
        btn->setToolTip(tr("Click to choose a new color"));
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFixedSize(kColorButtonSize, kColorButtonSize);
        return btn;
    };
    m_currentMeshSettingsArrow = makeArrowButton(tr("Settings: Viewer"));
    arrowLayout->addWidget(m_currentMeshSettingsArrow);

    m_bboxSettingsArrow = makeArrowButton(tr("Settings: Bounding Box"));
    m_pointsSettingsArrow = makeArrowButton(tr("Settings: Points"));
    m_edgesSettingsArrow = makeArrowButton(tr("Settings: Edges"));
    m_wireSettingsArrow = makeArrowButton(tr("Settings: Wireframe"));
    m_fillSettingsArrow = makeArrowButton(tr("Settings: Fill"));
    m_selectionSettingsArrow = makeArrowButton(tr("Settings: Selection"));
    m_qualityHistogramSettingsArrow = makeArrowButton(tr("Settings: Quality Histogram"));
    arrowLayout->addWidget(m_bboxSettingsArrow);
    arrowLayout->addWidget(m_pointsSettingsArrow);
    arrowLayout->addWidget(m_edgesSettingsArrow);
    arrowLayout->addWidget(m_wireSettingsArrow);
    arrowLayout->addWidget(m_fillSettingsArrow);
    arrowLayout->addWidget(m_selectionSettingsArrow);
    m_normalsDecoratorsSettingsArrow = makeArrowButton(tr("Settings: Normal Decorators"));
    arrowLayout->addWidget(m_normalsDecoratorsSettingsArrow);
    m_boundaryDecoratorsSettingsArrow = makeArrowButton(tr("Settings: Boundary Decorators"));
    arrowLayout->addWidget(m_boundaryDecoratorsSettingsArrow);
    arrowLayout->addWidget(m_qualityHistogramSettingsArrow);

    m_settingsContainer = new QFrame(this);
    m_settingsContainer->setVisible(false);
    m_settingsContainer->setObjectName(QStringLiteral("settingsContainer"));
    m_settingsContainer->setStyleSheet(QStringLiteral(
        "#settingsContainer { background: rgba(250,250,250,170); border: 1px solid rgba(40,40,40,110); border-radius: 4px; }"
        "#settingsContainer QLabel { border: none; background: transparent; }"
        "#settingsContainer, #settingsContainer QLabel, #settingsContainer QCheckBox, #settingsContainer QComboBox, #settingsContainer QDoubleSpinBox { font-size: 11px; }"));

    auto *settingsContainerLayout = new QVBoxLayout(m_settingsContainer);
    settingsContainerLayout->setContentsMargins(4, 4, 4, 4);
    settingsContainerLayout->setSpacing(2);
    m_settingsStack = new CurrentPageStackedWidget(m_settingsContainer);
    settingsContainerLayout->addWidget(m_settingsStack);

    auto *currentMeshPage = new QWidget(m_settingsStack);
    auto *currentMeshLayout = new QVBoxLayout(currentMeshPage);
    currentMeshLayout->setContentsMargins(0, 0, 0, 0);
    currentMeshLayout->setSpacing(2);
    m_viewerSettingsStack = new CurrentPageStackedWidget(currentMeshPage);
    currentMeshLayout->addWidget(m_viewerSettingsStack);

    auto *viewer3dPage = new QWidget(m_viewerSettingsStack);
    auto *viewer3dLayout = new QVBoxLayout(viewer3dPage);
    viewer3dLayout->setContentsMargins(0, 0, 0, 0);
    viewer3dLayout->setSpacing(2);
    auto *currentMeshForm = new QFormLayout();
    currentMeshForm->setContentsMargins(0, 0, 0, 0);
    currentMeshForm->setHorizontalSpacing(6);
    currentMeshForm->setVerticalSpacing(1);
    currentMeshForm->setLabelAlignment(kSettingsLabelAlignment);
    m_currentMeshHighlightCheck = new QCheckBox(viewer3dPage);
    m_currentMeshHighlightCheck->setChecked(m_globalSettings.highlightCurrentMesh);
    m_showTrackballGizmoCheck = new QCheckBox(viewer3dPage);
    m_showTrackballGizmoCheck->setChecked(m_globalSettings.showTrackballGizmo);
    m_showAxisGizmoCheck = new QCheckBox(viewer3dPage);
    m_showAxisGizmoCheck->setChecked(m_globalSettings.showAxisGizmo);
    m_currentMeshOutlineColorButton = makeColorButton(viewer3dPage);
    m_sceneBackgroundTopColorButton = makeColorButton(viewer3dPage);
    m_sceneBackgroundBottomColorButton = makeColorButton(viewer3dPage);
    m_currentMeshOutlineWidthSpin = new QDoubleSpinBox(viewer3dPage);
    m_currentMeshDilateRadiusSpin = new QDoubleSpinBox(viewer3dPage);
    m_currentMeshErodeRadiusSpin = new QDoubleSpinBox(viewer3dPage);
    m_currentMeshDebugViewCombo = new QComboBox(viewer3dPage);
    m_currentMeshOutlineWidthSpin->setRange(1.0, 8.0);
    m_currentMeshOutlineWidthSpin->setSingleStep(0.5);
    m_currentMeshOutlineWidthSpin->setDecimals(1);
    m_currentMeshOutlineWidthSpin->setSuffix(tr(" px"));
    m_currentMeshOutlineWidthSpin->setValue(m_globalSettings.currentMeshOutlineWidth);
    m_currentMeshDilateRadiusSpin->setRange(0.0, 16.0);
    m_currentMeshDilateRadiusSpin->setSingleStep(0.5);
    m_currentMeshDilateRadiusSpin->setDecimals(1);
    m_currentMeshDilateRadiusSpin->setSuffix(tr(" px"));
    m_currentMeshDilateRadiusSpin->setValue(m_globalSettings.currentMeshDilateRadius);
    m_currentMeshErodeRadiusSpin->setRange(0.0, 16.0);
    m_currentMeshErodeRadiusSpin->setSingleStep(0.5);
    m_currentMeshErodeRadiusSpin->setDecimals(1);
    m_currentMeshErodeRadiusSpin->setSuffix(tr(" px"));
    m_currentMeshErodeRadiusSpin->setValue(m_globalSettings.currentMeshErodeRadius);
    m_currentMeshDebugViewCombo->addItem(
        tr("Outline"),
        static_cast<int>(CurrentMeshDebugView::Outline));
    m_currentMeshDebugViewCombo->addItem(
        tr("Full Mask"),
        static_cast<int>(CurrentMeshDebugView::FullMask));
    m_currentMeshDebugViewCombo->addItem(
        tr("Visible Mask"),
        static_cast<int>(CurrentMeshDebugView::VisibleMask));
    m_currentMeshDebugViewCombo->addItem(
        tr("Occluded Mask"),
        static_cast<int>(CurrentMeshDebugView::OccludedMask));
    m_currentMeshDebugViewCombo->addItem(
        tr("Dilated"),
        static_cast<int>(CurrentMeshDebugView::DilatedMask));
    m_currentMeshDebugViewCombo->addItem(
        tr("Eroded"),
        static_cast<int>(CurrentMeshDebugView::ErodedMask));
    // Names the thing it switches on. "Highlight" named neither what is highlighted nor
    // how, and sat directly above the outline's own colour and width.
    currentMeshForm->addRow(
        tr("Current layer outline"),
        makeCenteredFieldContainer(m_currentMeshHighlightCheck, viewer3dPage));
    currentMeshForm->addRow(
        tr("Trackball gizmo"),
        makeCenteredFieldContainer(m_showTrackballGizmoCheck, viewer3dPage));
    currentMeshForm->addRow(
        tr("Axis gizmo"),
        makeCenteredFieldContainer(m_showAxisGizmoCheck, viewer3dPage));
    m_showViewCamerasCheck = new QCheckBox(viewer3dPage);
    m_showViewCamerasCheck->setChecked(m_globalSettings.showViewCameras);
    currentMeshForm->addRow(
        tr("Show View Cameras"),
        makeCenteredFieldContainer(m_showViewCamerasCheck, viewer3dPage));
    currentMeshForm->addRow(
        tr("Outline color"),
        makeCenteredFieldContainer(m_currentMeshOutlineColorButton, viewer3dPage));
    currentMeshForm->addRow(tr("Outline width"), m_currentMeshOutlineWidthSpin);
    currentMeshForm->addRow(tr("Dilate"), m_currentMeshDilateRadiusSpin);
    currentMeshForm->addRow(tr("Erode"), m_currentMeshErodeRadiusSpin);
    currentMeshForm->addRow(tr("Debug view"), m_currentMeshDebugViewCombo);
    currentMeshForm->addRow(
        tr("Bg top"),
        makeCenteredFieldContainer(m_sceneBackgroundTopColorButton, viewer3dPage));
    currentMeshForm->addRow(
        tr("Bg bottom"),
        makeCenteredFieldContainer(m_sceneBackgroundBottomColorButton, viewer3dPage));
    applyUniformFormRowHeights(currentMeshForm);
    viewer3dLayout->addLayout(currentMeshForm);
    m_viewerSettingsStack->addWidget(viewer3dPage);

    // Page 1 of m_viewerSettingsStack is no longer used for UV options;
    // UV fill options live in the dedicated UV fill page of m_settingsStack instead.
    m_viewerSettingsStack->addWidget(new QWidget(m_viewerSettingsStack));

    m_settingsStack->addWidget(currentMeshPage);

    auto *normalDecoratorsPage = new QWidget(m_settingsStack);
    auto *normalDecoratorsLayout = new QVBoxLayout(normalDecoratorsPage);
    normalDecoratorsLayout->setContentsMargins(0, 0, 0, 0);
    normalDecoratorsLayout->setSpacing(2);
    auto *normalDecoratorsForm = new QFormLayout();
    normalDecoratorsForm->setContentsMargins(0, 0, 0, 0);
    normalDecoratorsForm->setHorizontalSpacing(6);
    normalDecoratorsForm->setVerticalSpacing(1);
    normalDecoratorsForm->setLabelAlignment(kSettingsLabelAlignment);
    m_decoratorVertexNormalsCheck = new QCheckBox(normalDecoratorsPage);
    m_decoratorFaceNormalsCheck = new QCheckBox(normalDecoratorsPage);
    m_decoratorCurvatureDirCheck = new QCheckBox(normalDecoratorsPage);
    m_decoratorBoundaryEdgesCheck = new QCheckBox(normalDecoratorsPage);
    m_decoratorTextureSeamsCheck = new QCheckBox(normalDecoratorsPage);
    m_decoratorVertexNormalsCheck->setChecked(m_meshSettings.decoratorVertexNormals);
    m_decoratorFaceNormalsCheck->setChecked(m_meshSettings.decoratorFaceNormals);
    m_decoratorCurvatureDirCheck->setChecked(m_meshSettings.decoratorCurvatureDir);
    m_decoratorBoundaryEdgesCheck->setChecked(m_meshSettings.decoratorBoundaryEdges);
    m_decoratorTextureSeamsCheck->setChecked(m_meshSettings.decoratorTextureSeams);
    m_decoratorVertexNormalColorButton = makeColorButton(normalDecoratorsPage);
    m_decoratorFaceNormalColorButton = makeColorButton(normalDecoratorsPage);
    m_decoratorCurvatureDirPD1ColorButton = makeColorButton(normalDecoratorsPage);
    m_decoratorCurvatureDirPD2ColorButton = makeColorButton(normalDecoratorsPage);
    m_decoratorBoundaryEdgeColorButton = makeColorButton(normalDecoratorsPage);
    m_decoratorTextureSeamColorButton = makeColorButton(normalDecoratorsPage);
    normalDecoratorsForm->addRow(
        tr("Vertex normals"),
        makeCenteredFieldContainer(m_decoratorVertexNormalsCheck, normalDecoratorsPage));
    normalDecoratorsForm->addRow(
        tr("Vertex normal color"),
        makeCenteredFieldContainer(m_decoratorVertexNormalColorButton, normalDecoratorsPage));
    normalDecoratorsForm->addRow(
        tr("Face normals"),
        makeCenteredFieldContainer(m_decoratorFaceNormalsCheck, normalDecoratorsPage));
    normalDecoratorsForm->addRow(
        tr("Face normal color"),
        makeCenteredFieldContainer(m_decoratorFaceNormalColorButton, normalDecoratorsPage));
    normalDecoratorsForm->addRow(
        tr("Curvature directions"),
        makeCenteredFieldContainer(m_decoratorCurvatureDirCheck, normalDecoratorsPage));
    normalDecoratorsForm->addRow(
        tr("Curv. max dir color"),
        makeCenteredFieldContainer(m_decoratorCurvatureDirPD1ColorButton, normalDecoratorsPage));
    normalDecoratorsForm->addRow(
        tr("Curv. min dir color"),
        makeCenteredFieldContainer(m_decoratorCurvatureDirPD2ColorButton, normalDecoratorsPage));
    applyUniformFormRowHeights(normalDecoratorsForm);
    normalDecoratorsLayout->addLayout(normalDecoratorsForm);
    m_settingsStack->addWidget(normalDecoratorsPage);

    auto *boundaryDecoratorsPage = new QWidget(m_settingsStack);
    auto *boundaryDecoratorsLayout = new QVBoxLayout(boundaryDecoratorsPage);
    boundaryDecoratorsLayout->setContentsMargins(0, 0, 0, 0);
    boundaryDecoratorsLayout->setSpacing(2);
    auto *boundaryDecoratorsForm = new QFormLayout();
    boundaryDecoratorsForm->setContentsMargins(0, 0, 0, 0);
    boundaryDecoratorsForm->setHorizontalSpacing(6);
    boundaryDecoratorsForm->setVerticalSpacing(1);
    boundaryDecoratorsForm->setLabelAlignment(kSettingsLabelAlignment);
    m_decoratorBoundaryWidthSpin = new QDoubleSpinBox(boundaryDecoratorsPage);
    m_decoratorBoundaryWidthSpin->setRange(0.5, 64.0);
    m_decoratorBoundaryWidthSpin->setSingleStep(0.5);
    m_decoratorBoundaryWidthSpin->setDecimals(1);
    m_decoratorBoundaryWidthSpin->setSuffix(tr(" px"));
    m_decoratorBoundaryWidthSpin->setValue(m_meshSettings.decoratorBoundaryWidth);
    m_decoratorNonManifoldEdgesCheck = new QCheckBox(boundaryDecoratorsPage);
    m_decoratorNonManifoldEdgesCheck->setChecked(m_meshSettings.decoratorNonManifoldEdges);
    m_decoratorNonManifoldEdgeColorButton = makeColorButton(boundaryDecoratorsPage);
    m_decoratorNonManifoldVerticesCheck = new QCheckBox(boundaryDecoratorsPage);
    m_decoratorNonManifoldVerticesCheck->setChecked(m_meshSettings.decoratorNonManifoldVertices);
    m_decoratorNonManifoldVertexColorButton = makeColorButton(boundaryDecoratorsPage);
    m_decoratorInfoCheck = new QCheckBox(boundaryDecoratorsPage);
    m_decoratorInfoCheck->setChecked(m_globalSettings.showDecoratorInfo);
    boundaryDecoratorsForm->addRow(
        tr("Boundary edges"),
        makeCenteredFieldContainer(m_decoratorBoundaryEdgesCheck, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(
        tr("Boundary edge color"),
        makeCenteredFieldContainer(m_decoratorBoundaryEdgeColorButton, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(
        tr("Texture seams"),
        makeCenteredFieldContainer(m_decoratorTextureSeamsCheck, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(
        tr("Texture seam color"),
        makeCenteredFieldContainer(m_decoratorTextureSeamColorButton, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(
        tr("Non-manifold edges"),
        makeCenteredFieldContainer(m_decoratorNonManifoldEdgesCheck, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(
        tr("Non-manifold edge color"),
        makeCenteredFieldContainer(m_decoratorNonManifoldEdgeColorButton, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(
        tr("Non-manifold vertices"),
        makeCenteredFieldContainer(m_decoratorNonManifoldVerticesCheck, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(
        tr("Non-manifold vertex color"),
        makeCenteredFieldContainer(m_decoratorNonManifoldVertexColorButton, boundaryDecoratorsPage));
    boundaryDecoratorsForm->addRow(tr("Line width"), m_decoratorBoundaryWidthSpin);
    boundaryDecoratorsForm->addRow(
        tr("Show info panel"),
        makeCenteredFieldContainer(m_decoratorInfoCheck, boundaryDecoratorsPage));
    applyUniformFormRowHeights(boundaryDecoratorsForm);
    boundaryDecoratorsLayout->addLayout(boundaryDecoratorsForm);
    m_settingsStack->addWidget(boundaryDecoratorsPage);

    auto addApplyToAllButton = [this](QLayout *layout, RenderPass pass) {
        // "to All" left the reader to guess all of what -- all passes? all views? It is
        // all layers, and the label now says which.
        auto *btn = new QPushButton(tr("Apply to All Layers"));
        btn->setToolTip(tr("Apply current %1 settings to all visible layers")
            .arg(pass == RenderPass::BoundingBox ? tr("box") :
                 pass == RenderPass::Points ? tr("points") :
                 pass == RenderPass::Edges ? tr("edges") :
                 pass == RenderPass::Wireframe ? tr("wire") :
                 tr("fill")));
        layout->addWidget(btn);
        connect(btn, &QPushButton::clicked, this, [this, pass]() {
            emit applyToAllMeshesRequested(m_meshSettings, pass);
        });

        // The shortcut is invisible by nature, so it is written down exactly where someone
        // who wanted it would be looking -- next to the long way round.
        auto *hint = new QLabel(tr("Shift-click a pass button to do this without opening it."));
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("QLabel { color: rgba(90,90,96,205); }"));
        layout->addWidget(hint);
    };

    auto *bboxPage = new QWidget(m_settingsStack);
    auto *bboxLayout = new QVBoxLayout(bboxPage);
    bboxLayout->setContentsMargins(0, 0, 0, 0);
    bboxLayout->setSpacing(2);
    auto *bboxForm = new QFormLayout();
    bboxForm->setContentsMargins(0, 0, 0, 0);
    bboxForm->setHorizontalSpacing(6);
    bboxForm->setVerticalSpacing(1);
    bboxForm->setLabelAlignment(kSettingsLabelAlignment);
    m_bboxStyleCombo = new QComboBox(bboxPage);
    m_bboxStyleCombo->addItem(tr("Box"), static_cast<int>(BoundingBoxStyle::Box));
    m_bboxStyleCombo->addItem(
        tr("Corner brackets"), static_cast<int>(BoundingBoxStyle::CornerBrackets));
    m_bboxColorButton = makeColorButton(bboxPage);
    m_bboxShowCornersCheck = new QCheckBox(bboxPage);
    m_bboxShowCornersCheck->setChecked(m_globalSettings.showBoundingBoxCorners);
    m_bboxShowDimensionsCheck = new QCheckBox(bboxPage);
    m_bboxShowDimensionsCheck->setChecked(m_globalSettings.showBoundingBoxDimensions);
    bboxForm->addRow(tr("Style"), m_bboxStyleCombo);
    bboxForm->addRow(
        tr("Wire color"),
        makeCenteredFieldContainer(m_bboxColorButton, bboxPage));
    bboxForm->addRow(
        tr("Show min/max corners"),
        makeCenteredFieldContainer(m_bboxShowCornersCheck, bboxPage));
    bboxForm->addRow(
        tr("Show X/Y/Z size"),
        makeCenteredFieldContainer(m_bboxShowDimensionsCheck, bboxPage));
    applyUniformFormRowHeights(bboxForm);
    bboxLayout->addLayout(bboxForm);
    addApplyToAllButton(bboxLayout, RenderPass::BoundingBox);
    m_settingsStack->addWidget(bboxPage);
    auto *pointsPage = new QWidget(m_settingsStack);
    auto *pointsLayout = new QVBoxLayout(pointsPage);
    pointsLayout->setContentsMargins(0, 0, 0, 0);
    pointsLayout->setSpacing(2);
    auto *pointsForm = new QFormLayout();
    pointsForm->setContentsMargins(0, 0, 0, 0);
    pointsForm->setHorizontalSpacing(6);
    pointsForm->setVerticalSpacing(1);
    pointsForm->setLabelAlignment(kSettingsLabelAlignment);
    m_pointsColorButton = makeColorButton(pointsPage);
    m_pointColorSourceCombo = new QComboBox(pointsPage);
    m_pointColorSourceCombo->addItem(tr("Constant"), static_cast<int>(PointColorSource::Constant));
    m_pointColorSourceCombo->addItem(tr("Per-Vertex"), static_cast<int>(PointColorSource::PerVertex));
    m_pointColorSourceCombo->addItem(
        tr("Per-Vertex Quality"),
        static_cast<int>(PointColorSource::PerVertexQuality));
    m_pointSizeSpin = new QDoubleSpinBox(pointsPage);
    m_pointSizeSpin->setRange(1.0, 32.0);
    m_pointSizeSpin->setSingleStep(0.5);
    m_pointSizeSpin->setDecimals(1);
    m_pointSizeSpin->setSuffix(tr(" px"));
    m_pointSizeSpin->setValue(m_meshSettings.pointSize);
    m_pointLightingCheck = new QCheckBox(pointsPage);
    m_pointLightingCheck->setChecked(m_meshSettings.pointLighting);
    pointsForm->addRow(tr("Color source"), m_pointColorSourceCombo);
    pointsForm->addRow(
        tr("Point color"),
        makeCenteredFieldContainer(m_pointsColorButton, pointsPage));
    pointsForm->addRow(tr("Point size"), m_pointSizeSpin);
    pointsForm->addRow(
        tr("Lighting"),
        makeCenteredFieldContainer(m_pointLightingCheck, pointsPage));
    applyUniformFormRowHeights(pointsForm);
    pointsLayout->addLayout(pointsForm);
    addApplyToAllButton(pointsLayout, RenderPass::Points);
    m_settingsStack->addWidget(pointsPage);
    auto *edgesPage = new QWidget(m_settingsStack);
    auto *edgesLayout = new QVBoxLayout(edgesPage);
    edgesLayout->setContentsMargins(0, 0, 0, 0);
    edgesLayout->setSpacing(2);
    auto *edgesForm = new QFormLayout();
    edgesForm->setContentsMargins(0, 0, 0, 0);
    edgesForm->setHorizontalSpacing(6);
    edgesForm->setVerticalSpacing(1);
    edgesForm->setLabelAlignment(kSettingsLabelAlignment);
    m_edgeSizeSpin = new QDoubleSpinBox(edgesPage);
    m_edgeSizeSpin->setRange(0.0, 100.0);
    m_edgeSizeSpin->setDecimals(1);
    m_edgeSizeSpin->setSingleStep(0.5);
    m_edgeSizeSpin->setPrefix(QStringLiteral("px "));
    m_edgeSizeSpin->setSuffix(QString());
    m_edgeSizeSpin->setValue(m_meshSettings.edgeSize);
    m_edgeColorButton = makeColorButton(edgesPage);
    edgesForm->addRow(
        tr("Edge color"),
        makeCenteredFieldContainer(m_edgeColorButton, edgesPage));
    edgesForm->addRow(tr("Edge size"), m_edgeSizeSpin);
    applyUniformFormRowHeights(edgesForm);
    edgesLayout->addLayout(edgesForm);
    addApplyToAllButton(edgesLayout, RenderPass::Edges);
    m_settingsStack->addWidget(edgesPage);
    auto *wirePage = new QWidget(m_settingsStack);
    auto *wireLayout = new QVBoxLayout(wirePage);
    wireLayout->setContentsMargins(0, 0, 0, 0);
    wireLayout->setSpacing(2);
    auto *wireForm = new QFormLayout();
    wireForm->setContentsMargins(0, 0, 0, 0);
    wireForm->setHorizontalSpacing(6);
    wireForm->setVerticalSpacing(1);
    wireForm->setLabelAlignment(kSettingsLabelAlignment);
    m_wireColorButton = makeColorButton(wirePage);
    m_wireSizeSpin = new QDoubleSpinBox(wirePage);
    m_wireSizeSpin->setRange(0.5, 8.0);
    m_wireSizeSpin->setSingleStep(0.1);
    m_wireSizeSpin->setDecimals(1);
    m_wireSizeSpin->setSuffix(tr(" px"));
    m_wireSizeSpin->setValue(m_meshSettings.wireSize);
    m_wireBackfaceCullingCheck = new QCheckBox(wirePage);
    m_wireBackfaceCullingCheck->setChecked(m_meshSettings.wireBackfaceCulling);
    m_wireLightingCheck = new QCheckBox(wirePage);
    m_wireLightingCheck->setChecked(m_meshSettings.wireLighting);
    m_wireRespectFauxCheck = new QCheckBox(wirePage);
    m_wireRespectFauxCheck->setChecked(m_meshSettings.wireRespectFaux);
    wireForm->addRow(
        tr("Wire color"),
        makeCenteredFieldContainer(m_wireColorButton, wirePage));
    wireForm->addRow(tr("Wire width"), m_wireSizeSpin);
    wireForm->addRow(
        tr("Backface culling"),
        makeCenteredFieldContainer(m_wireBackfaceCullingCheck, wirePage));
    wireForm->addRow(
        tr("Lighting"),
        makeCenteredFieldContainer(m_wireLightingCheck, wirePage));
    wireForm->addRow(
        tr("Respect polygon edges"),
        makeCenteredFieldContainer(m_wireRespectFauxCheck, wirePage));
    applyUniformFormRowHeights(wireForm);
    wireLayout->addLayout(wireForm);
    addApplyToAllButton(wireLayout, RenderPass::Wireframe);
    m_settingsStack->addWidget(wirePage);
    auto *fillPage = new QWidget(m_settingsStack);
    auto *fillLayout = new QVBoxLayout(fillPage);
    fillLayout->setContentsMargins(0, 0, 0, 0);
    fillLayout->setSpacing(2);
    auto *fillForm = new QFormLayout();
    fillForm->setContentsMargins(0, 0, 0, 0);
    fillForm->setHorizontalSpacing(6);
    fillForm->setVerticalSpacing(1);
    fillForm->setLabelAlignment(kSettingsLabelAlignment);
    m_fillMaterialCombo = new QComboBox(fillPage);
    m_fillMaterialCombo->addItem(tr("Plain"), static_cast<int>(FillMaterial::Plain));
    m_fillMaterialCombo->addItem(tr("PBR"), static_cast<int>(FillMaterial::Pbr));
    m_fillMaterialCombo->addItem(tr("Radiance Scaling"), static_cast<int>(FillMaterial::RadianceScaling));
    fillForm->addRow(tr("Material"), m_fillMaterialCombo);
    m_fillColorButton = makeColorButton(fillPage);
    m_fillColorSourceCombo = new QComboBox(fillPage);
    m_fillColorSourceCombo->addItem(tr("Constant"), static_cast<int>(FillColorSource::Constant));
    m_fillColorSourceCombo->addItem(tr("Per-Vertex"), static_cast<int>(FillColorSource::PerVertex));
    m_fillColorSourceCombo->addItem(tr("Per-Face"), static_cast<int>(FillColorSource::PerFace));
    m_fillColorSourceCombo->addItem(tr("Per-Mesh"), static_cast<int>(FillColorSource::PerMesh));
    m_fillColorSourceCombo->addItem(
        tr("Per-Vertex Quality"),
        static_cast<int>(FillColorSource::PerVertexQuality));
    m_fillColorSourceCombo->addItem(
        tr("Per-Face Quality"),
        static_cast<int>(FillColorSource::PerFaceQuality));
    m_fillColorSourceCombo->addItem(tr("Texture"), static_cast<int>(FillColorSource::Texture));
    m_fillShadingCombo = new QComboBox(fillPage);
    m_fillShadingCombo->addItem(tr("Smooth"), static_cast<int>(FillShading::Smooth));
    m_fillShadingCombo->addItem(tr("Flat"), static_cast<int>(FillShading::Flat));
    m_fillBackfaceCullingCheck = new QCheckBox(fillPage);
    m_fillBackfaceCullingCheck->setChecked(m_meshSettings.fillBackfaceCulling);
    m_fillLightingCheck = new QCheckBox(fillPage);
    m_fillLightingCheck->setChecked(m_meshSettings.fillLighting);
    m_fillNormalScaleSpin = new QDoubleSpinBox(fillPage);
    m_fillNormalScaleSpin->setRange(-2.0, 2.0);
    m_fillNormalScaleSpin->setSingleStep(0.05);
    m_fillNormalScaleSpin->setDecimals(2);
    m_fillNormalScaleSpin->setValue(m_meshSettings.fillPbr.normalScale);
    m_fillOcclusionStrengthSpin = new QDoubleSpinBox(fillPage);
    m_fillOcclusionStrengthSpin->setRange(-2.0, 2.0);
    m_fillOcclusionStrengthSpin->setSingleStep(0.05);
    m_fillOcclusionStrengthSpin->setDecimals(2);
    m_fillOcclusionStrengthSpin->setValue(m_meshSettings.fillPbr.occlusionStrength);
    m_fillRoughnessFactorSpin = new QDoubleSpinBox(fillPage);
    m_fillRoughnessFactorSpin->setRange(0.0, 2.0);
    m_fillRoughnessFactorSpin->setSingleStep(0.05);
    m_fillRoughnessFactorSpin->setDecimals(2);
    m_fillRoughnessFactorSpin->setValue(m_meshSettings.fillPbr.roughnessFactor);

    m_fillMaterialStack = new CurrentPageStackedWidget(fillPage);
    auto *fillPlainPage = new QWidget(m_fillMaterialStack);
    auto *fillPlainLayout = new QVBoxLayout(fillPlainPage);
    fillPlainLayout->setContentsMargins(0, 0, 0, 0);
    fillPlainLayout->setSpacing(2);
    m_fillPlainForm = new QFormLayout();
    m_fillPlainForm->setContentsMargins(0, 0, 0, 0);
    m_fillPlainForm->setHorizontalSpacing(6);
    m_fillPlainForm->setVerticalSpacing(1);
    m_fillPlainForm->setLabelAlignment(kSettingsLabelAlignment);
    m_fillPlainForm->addRow(tr("Color source"), m_fillColorSourceCombo);
    m_fillPlainTextureCombo = new QComboBox(fillPage);
    m_fillPlainForm->addRow(tr("Texture"), m_fillPlainTextureCombo);
    m_fillTextureNearestCheck = new QCheckBox(fillPage);
    m_fillTextureNearestCheck->setChecked(m_globalSettings.fillTextureNearestSampling);
    m_fillPlainForm->addRow(
        tr("Nearest sampling"),
        makeCenteredFieldContainer(m_fillTextureNearestCheck, fillPage));
    m_fillPlainForm->addRow(
        tr("Fill color"),
        makeCenteredFieldContainer(m_fillColorButton, fillPage));
    m_fillPlainForm->addRow(tr("Shading"), m_fillShadingCombo);
    m_fillPlainForm->addRow(
        tr("Backface culling"),
        makeCenteredFieldContainer(m_fillBackfaceCullingCheck, fillPage));
    m_fillPlainForm->addRow(
        tr("Lighting"),
        makeCenteredFieldContainer(m_fillLightingCheck, fillPage));
    applyUniformFormRowHeights(m_fillPlainForm);
    fillPlainLayout->addLayout(m_fillPlainForm);
    m_fillMaterialStack->addWidget(fillPlainPage);

    auto *fillPbrPage = new QWidget(m_fillMaterialStack);
    auto *fillPbrLayout = new QVBoxLayout(fillPbrPage);
    fillPbrLayout->setContentsMargins(0, 0, 0, 0);
    fillPbrLayout->setSpacing(2);
    auto *fillPbrForm = new QFormLayout();
    fillPbrForm->setContentsMargins(0, 0, 0, 0);
    fillPbrForm->setHorizontalSpacing(6);
    fillPbrForm->setVerticalSpacing(1);
    fillPbrForm->setLabelAlignment(kSettingsLabelAlignment);
    m_fillPbrShadingCombo = new QComboBox(fillPage);
    m_fillPbrShadingCombo->addItem(tr("Smooth"), static_cast<int>(FillShading::Smooth));
    m_fillPbrShadingCombo->addItem(tr("Flat"), static_cast<int>(FillShading::Flat));
    m_fillPbrAlbedoCombo = new QComboBox(fillPage);
    m_fillPbrNormalCombo = new QComboBox(fillPage);
    m_fillNormalSpaceCombo = new QComboBox(fillPage);
    m_fillNormalSpaceCombo->addItem(tr("Tangent"), static_cast<int>(FillPbrNormalMapSpace::Tangent));
    m_fillNormalSpaceCombo->addItem(tr("Object"), static_cast<int>(FillPbrNormalMapSpace::Object));
    m_fillPbrOcclusionCombo = new QComboBox(fillPage);
    m_fillPbrRoughnessCombo = new QComboBox(fillPage);
    rebuildFillPbrSourceCombos();
    m_fillPbrColorButton = makeColorButton(fillPage);
    fillPbrForm->addRow(tr("Shading"), m_fillPbrShadingCombo);
    fillPbrForm->addRow(tr("Albedo"), m_fillPbrAlbedoCombo);
    fillPbrForm->addRow(tr("Normal"), m_fillPbrNormalCombo);
    fillPbrForm->addRow(tr("Normal space"), m_fillNormalSpaceCombo);
    fillPbrForm->addRow(tr("AO Map"), m_fillPbrOcclusionCombo);
    fillPbrForm->addRow(tr("Roughness Map"), m_fillPbrRoughnessCombo);
    fillPbrForm->addRow(
        tr("Albedo color"),
        makeCenteredFieldContainer(m_fillPbrColorButton, fillPage));
    fillPbrForm->addRow(tr("Normal scale"), m_fillNormalScaleSpin);
    fillPbrForm->addRow(tr("AO strength"), m_fillOcclusionStrengthSpin);
    fillPbrForm->addRow(tr("Rough fac"), m_fillRoughnessFactorSpin);
    applyUniformFormRowHeights(fillPbrForm);
    fillPbrLayout->addLayout(fillPbrForm);
    m_fillMaterialStack->addWidget(fillPbrPage);

    auto *fillRsPage = new QWidget(m_fillMaterialStack);
    auto *fillRsLayout = new QVBoxLayout(fillRsPage);
    fillRsLayout->setContentsMargins(0, 0, 0, 0);
    fillRsLayout->setSpacing(2);
    auto *fillRsForm = new QFormLayout();
    fillRsForm->setContentsMargins(0, 0, 0, 0);
    fillRsForm->setHorizontalSpacing(6);
    fillRsForm->setVerticalSpacing(1);
    fillRsForm->setLabelAlignment(kSettingsLabelAlignment);
    m_fillRsEnhancementSpin = new QDoubleSpinBox(fillPage);
    m_fillRsEnhancementSpin->setRange(0.0, 1.0);
    m_fillRsEnhancementSpin->setSingleStep(0.05);
    m_fillRsEnhancementSpin->setDecimals(2);
    m_fillRsEnhancementSpin->setValue(m_meshSettings.fillRs.enhancement);
    fillRsForm->addRow(tr("Enhancement"), m_fillRsEnhancementSpin);
    m_fillRsDisplayModeCombo = new QComboBox(fillPage);
    m_fillRsDisplayModeCombo->addItem(tr("Lambertian RS"), 0);
    m_fillRsDisplayModeCombo->addItem(tr("Colored Descriptor"), 1);
    m_fillRsDisplayModeCombo->addItem(tr("Grey Descriptor"), 2);
    fillRsForm->addRow(tr("Display"), m_fillRsDisplayModeCombo);
    m_fillRsInvertCheck = new QCheckBox(fillPage);
    m_fillRsInvertCheck->setChecked(m_meshSettings.fillRs.invert);
    fillRsForm->addRow(tr("Invert"), makeCenteredFieldContainer(m_fillRsInvertCheck, fillPage));
    m_fillRsFlatCheck = new QCheckBox(fillPage);
    m_fillRsFlatCheck->setChecked(m_meshSettings.fillRs.shading == FillShading::Flat);
    fillRsForm->addRow(tr("Flat shading"), makeCenteredFieldContainer(m_fillRsFlatCheck, fillPage));
    applyUniformFormRowHeights(fillRsForm);
    fillRsLayout->addLayout(fillRsForm);
    m_fillMaterialStack->addWidget(fillRsPage);

    applyUniformFormRowHeights(fillForm);
    fillLayout->addLayout(fillForm);
    fillLayout->addWidget(m_fillMaterialStack);
    addApplyToAllButton(fillLayout, RenderPass::Fill);
    m_settingsStack->addWidget(fillPage);

    auto *selectionPage = new QWidget(m_settingsStack);
    auto *selectionLayout = new QVBoxLayout(selectionPage);
    selectionLayout->setContentsMargins(0, 0, 0, 0);
    selectionLayout->setSpacing(2);
    auto *selectionForm = new QFormLayout();
    selectionForm->setContentsMargins(0, 0, 0, 0);
    selectionForm->setHorizontalSpacing(6);
    selectionForm->setVerticalSpacing(1);
    selectionForm->setLabelAlignment(kSettingsLabelAlignment);
    m_selectionShowVerticesCheck = new QCheckBox(selectionPage);
    m_selectionShowVerticesCheck->setChecked(m_meshSettings.showSelectionVertices);
    m_selectionShowFacesCheck = new QCheckBox(selectionPage);
    m_selectionShowFacesCheck->setChecked(m_meshSettings.showSelectionFaces);
    selectionForm->addRow(
        tr("Vertices"),
        makeCenteredFieldContainer(m_selectionShowVerticesCheck, selectionPage));
    selectionForm->addRow(
        tr("Faces"),
        makeCenteredFieldContainer(m_selectionShowFacesCheck, selectionPage));
    applyUniformFormRowHeights(selectionForm);
    selectionLayout->addLayout(selectionForm);
    m_settingsStack->addWidget(selectionPage);

    auto *histogramPage = new QWidget(m_settingsStack);
    auto *histogramLayout = new QVBoxLayout(histogramPage);
    histogramLayout->setContentsMargins(0, 0, 0, 0);
    histogramLayout->setSpacing(2);
    auto *histogramForm = new QFormLayout();
    histogramForm->setContentsMargins(0, 0, 0, 0);
    histogramForm->setHorizontalSpacing(6);
    histogramForm->setVerticalSpacing(1);
    histogramForm->setLabelAlignment(kSettingsLabelAlignment);
    m_qualityHistogramSourceCombo = new QComboBox(histogramPage);
    m_qualityHistogramSourceCombo->addItem(
        tr("Auto"), static_cast<int>(QualityHistogramSource::Auto));
    m_qualityHistogramSourceCombo->addItem(
        tr("Vertex Q"), static_cast<int>(QualityHistogramSource::VertexQuality));
    m_qualityHistogramSourceCombo->addItem(
        tr("Face Q"), static_cast<int>(QualityHistogramSource::FaceQuality));
    histogramForm->addRow(tr("Source"), m_qualityHistogramSourceCombo);
    m_qualityHistogramColorMapCombo = new QComboBox(histogramPage);
    const ColorMapRegistry &colorMapRegistry = ColorMapRegistry::instance();
    const QStringList colorMapIds = colorMapRegistry.mapIds();
    for (const QString &id : colorMapIds)
        m_qualityHistogramColorMapCombo->addItem(colorMapRegistry.displayName(id), id);
    m_qualityHistogramColorMapCombo->addItem(tr("Constant"), QStringLiteral("constant"));
    if (m_qualityHistogramColorMapCombo->count() == 0)
        m_qualityHistogramColorMapCombo->addItem(tr("Rainbow"), QStringLiteral("rainbow"));
    int initialColorMapIndex = m_qualityHistogramColorMapCombo->findData(
        m_globalSettings.qualityHistogramColorMapId.trimmed().toLower());
    if (initialColorMapIndex < 0)
        initialColorMapIndex = m_qualityHistogramColorMapCombo->findData(
            colorMapRegistry.fallbackMapId());
    if (initialColorMapIndex < 0 && m_qualityHistogramColorMapCombo->count() > 0)
        initialColorMapIndex = 0;
    if (initialColorMapIndex >= 0) {
        m_qualityHistogramColorMapCombo->setCurrentIndex(initialColorMapIndex);
        m_globalSettings.qualityHistogramColorMapId =
            m_qualityHistogramColorMapCombo->itemData(initialColorMapIndex).toString();
    }
    histogramForm->addRow(tr("Color map"), m_qualityHistogramColorMapCombo);
    m_qualityHistogramInvertCheck = new QCheckBox(histogramPage);
    m_qualityHistogramInvertCheck->setChecked(m_globalSettings.qualityHistogramInvertColorMap);
    histogramForm->addRow(
        tr("Invert"),
        makeCenteredFieldContainer(m_qualityHistogramInvertCheck, histogramPage));
    m_qualityIsolinesCheck = new QCheckBox(histogramPage);
    m_qualityIsolinesCheck->setChecked(m_globalSettings.qualityIsolinesEnabled);
    histogramForm->addRow(
        tr("Isolines"),
        makeCenteredFieldContainer(m_qualityIsolinesCheck, histogramPage));
    m_qualityIsolineCountSpin = new QDoubleSpinBox(histogramPage);
    m_qualityIsolineCountSpin->setRange(1.0, 100.0);
    m_qualityIsolineCountSpin->setSingleStep(1.0);
    m_qualityIsolineCountSpin->setDecimals(0);
    m_qualityIsolineCountSpin->setValue(m_globalSettings.qualityIsolineCount);
    m_qualityIsolineCountSpin->setEnabled(m_globalSettings.qualityIsolinesEnabled);
    histogramForm->addRow(tr("Isoline count"), m_qualityIsolineCountSpin);
    m_qualityHistogramBinsSpin = new QDoubleSpinBox(histogramPage);
    m_qualityHistogramBinsSpin->setRange(4.0, 512.0);
    m_qualityHistogramBinsSpin->setSingleStep(1.0);
    m_qualityHistogramBinsSpin->setDecimals(0);
    m_qualityHistogramBinsSpin->setValue(m_globalSettings.qualityHistogramBins);
    histogramForm->addRow(tr("Bins"), m_qualityHistogramBinsSpin);
    m_qualityHistogramFixedRangeCheck = new QCheckBox(histogramPage);
    m_qualityHistogramFixedRangeCheck->setChecked(m_globalSettings.qualityHistogramFixedRange);
    histogramForm->addRow(
        tr("Fixed range"),
        makeCenteredFieldContainer(m_qualityHistogramFixedRangeCheck, histogramPage));
    m_qualityHistogramCenterOnZeroCheck = new QCheckBox(histogramPage);
    m_qualityHistogramCenterOnZeroCheck->setChecked(m_globalSettings.qualityHistogramCenterOnZero);
    m_qualityHistogramCenterOnZeroLabel = new QLabel(tr("Center on zero"), histogramPage);
    histogramForm->addRow(
        m_qualityHistogramCenterOnZeroLabel,
        makeCenteredFieldContainer(m_qualityHistogramCenterOnZeroCheck, histogramPage));
    m_qualityHistogramPercentileCropSpin = new QDoubleSpinBox(histogramPage);
    m_qualityHistogramPercentileCropSpin->setRange(0.0, 0.5);
    m_qualityHistogramPercentileCropSpin->setDecimals(4);
    m_qualityHistogramPercentileCropSpin->setSingleStep(0.001);
    m_qualityHistogramPercentileCropSpin->setValue(m_globalSettings.qualityHistogramPercentileCrop);
    m_qualityHistogramPercentileCropLabel = new QLabel(tr("Percentile crop"), histogramPage);
    histogramForm->addRow(m_qualityHistogramPercentileCropLabel, m_qualityHistogramPercentileCropSpin);
    m_qualityHistogramMinSpin = new QDoubleSpinBox(histogramPage);
    m_qualityHistogramMinSpin->setRange(-1e12, 1e12);
    m_qualityHistogramMinSpin->setDecimals(6);
    m_qualityHistogramMinSpin->setSingleStep(0.1);
    m_qualityHistogramMinSpin->setValue(m_globalSettings.qualityHistogramMin);
    m_qualityHistogramMinLabel = new QLabel(tr("Min"), histogramPage);
    histogramForm->addRow(m_qualityHistogramMinLabel, m_qualityHistogramMinSpin);
    m_qualityHistogramMaxSpin = new QDoubleSpinBox(histogramPage);
    m_qualityHistogramMaxSpin->setRange(-1e12, 1e12);
    m_qualityHistogramMaxSpin->setDecimals(6);
    m_qualityHistogramMaxSpin->setSingleStep(0.1);
    m_qualityHistogramMaxSpin->setValue(m_globalSettings.qualityHistogramMax);
    m_qualityHistogramMaxLabel = new QLabel(tr("Max"), histogramPage);
    histogramForm->addRow(m_qualityHistogramMaxLabel, m_qualityHistogramMaxSpin);
    m_qualityBakeVertexColorButton = new QPushButton(tr("Bake to Vertex Color"), histogramPage);
    m_qualityBakeVertexColorButton->setToolTip(
        tr("Bake the current vertex-quality color mapping into per-vertex colors. Isolines are ignored."));
    histogramForm->addRow(QString(), m_qualityBakeVertexColorButton);
    applyUniformFormRowHeights(histogramForm);
    histogramLayout->addLayout(histogramForm);
    m_settingsStack->addWidget(histogramPage);
    syncQualityHistogramUiState();

    // UV fill page (index 10): shown instead of the regular fill page when in UV mode.
    auto *uvFillPage = new QWidget(m_settingsStack);
    auto *uvFillLayout = new QVBoxLayout(uvFillPage);
    uvFillLayout->setContentsMargins(0, 0, 0, 0);
    uvFillLayout->setSpacing(2);
    auto *uvFillForm = new QFormLayout();
    uvFillForm->setContentsMargins(0, 0, 0, 0);
    uvFillForm->setHorizontalSpacing(6);
    uvFillForm->setVerticalSpacing(1);
    uvFillForm->setLabelAlignment(kSettingsLabelAlignment);
    m_uvFillColorSourceCombo = new QComboBox(uvFillPage);
    m_uvFillColorSourceCombo->addItem(tr("Constant"), static_cast<int>(FillColorSource::Constant));
    m_uvFillColorSourceCombo->addItem(tr("Per-Vertex"), static_cast<int>(FillColorSource::PerVertex));
    m_uvFillColorSourceCombo->addItem(tr("Per-Face"), static_cast<int>(FillColorSource::PerFace));
    m_uvFillColorSourceCombo->addItem(tr("Per-Mesh"), static_cast<int>(FillColorSource::PerMesh));
    m_uvFillColorSourceCombo->addItem(
        tr("Per-Vertex Quality"),
        static_cast<int>(FillColorSource::PerVertexQuality));
    m_uvFillColorSourceCombo->addItem(
        tr("Per-Face Quality"),
        static_cast<int>(FillColorSource::PerFaceQuality));
    m_uvFillColorSourceCombo->addItem(tr("Texture"), static_cast<int>(FillColorSource::Texture));
    uvFillForm->addRow(tr("Color source"), m_uvFillColorSourceCombo);
    m_uvFillColorButton = makeColorButton(uvFillPage);
    uvFillForm->addRow(tr("Fill color"), makeCenteredFieldContainer(m_uvFillColorButton, uvFillPage));
    m_uvTextureChannelCombo = new QComboBox(uvFillPage);
    m_uvTextureChannelCombo->addItems(
        { tr("Base color"), tr("Normal"), tr("Occlusion"), tr("Roughness") });
    uvFillForm->addRow(tr("Channel"), m_uvTextureChannelCombo);
    m_uvTextureNearestCheck = new QCheckBox(uvFillPage);
    m_uvTextureNearestCheck->setChecked(m_globalSettings.uvTextureNearestSampling);
    uvFillForm->addRow(
        tr("Nearest sampling"),
        makeCenteredFieldContainer(m_uvTextureNearestCheck, uvFillPage));
    m_uvShowFullTextureCheck = new QCheckBox(uvFillPage);
    m_uvShowFullTextureCheck->setChecked(m_globalSettings.uvShowFullTexture);
    uvFillForm->addRow(
        tr("Full texture"),
        makeCenteredFieldContainer(m_uvShowFullTextureCheck, uvFillPage));
    m_uvShowReferenceFrameCheck = new QCheckBox(uvFillPage);
    m_uvShowReferenceFrameCheck->setChecked(m_globalSettings.uvShowReferenceFrame);
    uvFillForm->addRow(
        tr("UV axis"),
        makeCenteredFieldContainer(m_uvShowReferenceFrameCheck, uvFillPage));
    applyUniformFormRowHeights(uvFillForm);
    uvFillLayout->addLayout(uvFillForm);
    m_settingsStack->addWidget(uvFillPage);  // index 10

    panelLayout->addWidget(m_settingsContainer);

    auto setGlobalField = [this](auto member, const auto &value, bool syncUi = false) {
        if (m_globalSettings.*member == value)
            return false;
        m_globalSettings.*member = value;
        if (syncUi)
            setGlobalSettings(m_globalSettings);
        emit globalSettingsChanged(m_globalSettings);
        return true;
    };
    auto setMeshField = [this](auto member, const auto &value, bool syncUi = false) {
        if (m_meshSettings.*member == value)
            return false;
        m_meshSettings.*member = value;
        if (syncUi)
            setMeshSettings(m_meshSettings);
        emit meshSettingsChanged(m_meshSettings);
        return true;
    };

    auto bindGlobalCheckBox = [this, setGlobalField](
                                QCheckBox *checkBox,
                                bool GlobalRenderSettings::*member,
                                bool syncUi = false) {
        connect(checkBox, &QCheckBox::toggled, this, [this, setGlobalField, member, syncUi](bool checked) {
            setGlobalField(member, checked, syncUi);
        });
        m_globalSyncers.push_back([this, checkBox, member] {
            QSignalBlocker blocker(checkBox);
            checkBox->setChecked(m_globalSettings.*member);
        });
    };
    auto bindMeshCheckBox = [this, setMeshField](
                                QCheckBox *checkBox,
                                bool PerMeshRenderSettings::*member,
                                bool syncUi = false) {
        connect(checkBox, &QCheckBox::toggled, this, [this, setMeshField, member, syncUi](bool checked) {
            setMeshField(member, checked, syncUi);
        });
        m_meshSyncers.push_back([this, checkBox, member] {
            QSignalBlocker blocker(checkBox);
            checkBox->setChecked(m_meshSettings.*member);
        });
    };

    auto bindGlobalToolToggle = [this, setGlobalField](
                                  QToolButton *button,
                                  bool GlobalRenderSettings::*member,
                                  bool syncUi = false) {
        connect(button, &QToolButton::toggled, this, [this, setGlobalField, member, syncUi](bool checked) {
            setGlobalField(member, checked, syncUi);
        });
        m_globalSyncers.push_back([this, button, member] {
            QSignalBlocker blocker(button);
            button->setChecked(m_globalSettings.*member);
        });
    };
    auto bindMeshToolToggle = [this, setMeshField](
                                  QToolButton *button,
                                  bool PerMeshRenderSettings::*member,
                                  bool syncUi = false) {
        connect(button, &QToolButton::toggled, this, [this, setMeshField, member, syncUi](bool checked) {
            setMeshField(member, checked, syncUi);
        });
        m_meshSyncers.push_back([this, button, member] {
            QSignalBlocker blocker(button);
            button->setChecked(m_meshSettings.*member);
        });
    };

    auto bindGlobalFloatSpin = [this, setGlobalField](
                                   QDoubleSpinBox *spin,
                                   float GlobalRenderSettings::*member) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, setGlobalField, member](double value) {
            setGlobalField(member, static_cast<float>(value));
        });
        m_globalSyncers.push_back([this, spin, member] {
            QSignalBlocker blocker(spin);
            spin->setValue(m_globalSettings.*member);
        });
    };
    auto bindMeshFloatSpin = [this, setMeshField](
                                 QDoubleSpinBox *spin,
                                 float PerMeshRenderSettings::*member) {
        connect(spin, &QDoubleSpinBox::valueChanged, this, [this, setMeshField, member](double value) {
            setMeshField(member, static_cast<float>(value));
        });
        m_meshSyncers.push_back([this, spin, member] {
            QSignalBlocker blocker(spin);
            spin->setValue(m_meshSettings.*member);
        });
    };

    auto bindGlobalEnumCombo = [this, setGlobalField](QComboBox *combo, auto member) {
        using EnumType = std::decay_t<decltype(m_globalSettings.*member)>;
        connect(
            combo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this, setGlobalField, combo, member](int idx) {
                const QVariant data = combo->itemData(idx);
                if (!data.isValid())
                    return;
                setGlobalField(member, static_cast<EnumType>(data.toInt()));
            });
        m_globalSyncers.push_back([this, combo, member] {
            const int wanted = static_cast<int>(m_globalSettings.*member);
            QSignalBlocker blocker(combo);
            for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemData(i).toInt() == wanted) {
                    combo->setCurrentIndex(i);
                    break;
                }
            }
        });
    };
    auto bindMeshEnumCombo = [this, setMeshField](QComboBox *combo, auto member) {
        using EnumType = std::decay_t<decltype(m_meshSettings.*member)>;
        connect(
            combo,
            qOverload<int>(&QComboBox::currentIndexChanged),
            this,
            [this, setMeshField, combo, member](int idx) {
                const QVariant data = combo->itemData(idx);
                if (!data.isValid())
                    return;
                setMeshField(member, static_cast<EnumType>(data.toInt()));
            });
        m_meshSyncers.push_back([this, combo, member] {
            const int wanted = static_cast<int>(m_meshSettings.*member);
            QSignalBlocker blocker(combo);
            for (int i = 0; i < combo->count(); ++i) {
                if (combo->itemData(i).toInt() == wanted) {
                    combo->setCurrentIndex(i);
                    break;
                }
            }
        });
    };

    auto bindGlobalColorButton = [this, setGlobalField](
                                   QPushButton *button,
                                   QColor GlobalRenderSettings::*member,
                                   const QString &dialogTitle) {
        connect(button, &QPushButton::clicked, this, [this, setGlobalField, button, member, dialogTitle]() {
            const QColor picked = QColorDialog::getColor(m_globalSettings.*member, this, dialogTitle);
            if (!picked.isValid())
                return;
            if (setGlobalField(member, picked))
                updateColorButtonStyle(button, picked);
        });
        m_globalSyncers.push_back([this, button, member] {
            updateColorButtonStyle(button, m_globalSettings.*member);
        });
    };
    auto bindMeshColorButton = [this, setMeshField](
                                   QPushButton *button,
                                   QColor PerMeshRenderSettings::*member,
                                   const QString &dialogTitle) {
        connect(button, &QPushButton::clicked, this, [this, setMeshField, button, member, dialogTitle]() {
            const QColor picked = QColorDialog::getColor(m_meshSettings.*member, this, dialogTitle);
            if (!picked.isValid())
                return;
            if (setMeshField(member, picked))
                updateColorButtonStyle(button, picked);
        });
        m_meshSyncers.push_back([this, button, member] {
            updateColorButtonStyle(button, m_meshSettings.*member);
        });
    };

    auto bindPassButton = [this](QToolButton *button, RenderPass pass, bool showSettings) {
        connect(button, &QToolButton::clicked, this, [this, pass, showSettings]() {
            // Shift turns the click into "and make every layer look like this". The button
            // has already emitted toggled() by the time clicked() arrives, so the pass it
            // pushes out includes the on/off state this very click just set.
            const bool applyToAll =
                (QApplication::keyboardModifiers() & Qt::ShiftModifier)
                && passSupportsApplyToAll(pass);
            setCurrentRenderPass(pass);
            if (showSettings)
                setSettingsVisible(true);
            if (applyToAll)
                emit applyToAllMeshesRequested(m_meshSettings, pass);
        });
    };

    // An arrow opens its own page and closes the panel again when that page is already the
    // one showing. That is what the dropped panel button used to be for, except it now sits
    // under the pass it belongs to instead of standing apart from all of them.
    auto bindSettingsArrow = [this](QToolButton *button, RenderPass pass) {
        connect(button, &QToolButton::clicked, this, [this, pass]() {
            const bool alreadyOpen =
                m_globalSettings.settingsPanelVisible && m_globalSettings.currentPass == pass;
            setCurrentRenderPass(pass);
            setSettingsVisible(!alreadyOpen);
            syncRenderPassUiState();
        });
    };

    bindGlobalCheckBox(m_currentMeshHighlightCheck, &GlobalRenderSettings::highlightCurrentMesh);
    bindGlobalCheckBox(m_showTrackballGizmoCheck, &GlobalRenderSettings::showTrackballGizmo);
    bindGlobalCheckBox(m_showAxisGizmoCheck, &GlobalRenderSettings::showAxisGizmo);
    bindGlobalCheckBox(m_showViewCamerasCheck, &GlobalRenderSettings::showViewCameras);
    bindGlobalCheckBox(m_fillTextureNearestCheck, &GlobalRenderSettings::fillTextureNearestSampling);
    bindGlobalColorButton(
        m_currentMeshOutlineColorButton,
        &GlobalRenderSettings::currentMeshOutlineColor,
        tr("Current Mesh Outline Color"));
    bindGlobalColorButton(
        m_sceneBackgroundTopColorButton,
        &GlobalRenderSettings::sceneBackgroundTopColor,
        tr("Scene Background Top Color"));
    bindGlobalColorButton(
        m_sceneBackgroundBottomColorButton,
        &GlobalRenderSettings::sceneBackgroundBottomColor,
        tr("Scene Background Bottom Color"));
    bindGlobalFloatSpin(m_currentMeshOutlineWidthSpin, &GlobalRenderSettings::currentMeshOutlineWidth);
    bindGlobalFloatSpin(m_currentMeshDilateRadiusSpin, &GlobalRenderSettings::currentMeshDilateRadius);
    bindGlobalFloatSpin(m_currentMeshErodeRadiusSpin, &GlobalRenderSettings::currentMeshErodeRadius);
    bindGlobalEnumCombo(m_currentMeshDebugViewCombo, &GlobalRenderSettings::currentMeshDebugView);

    bindMeshCheckBox(m_decoratorVertexNormalsCheck, &PerMeshRenderSettings::decoratorVertexNormals, true);
    bindMeshCheckBox(m_decoratorFaceNormalsCheck, &PerMeshRenderSettings::decoratorFaceNormals, true);
    bindMeshCheckBox(m_decoratorCurvatureDirCheck, &PerMeshRenderSettings::decoratorCurvatureDir, true);
    bindMeshCheckBox(m_decoratorBoundaryEdgesCheck, &PerMeshRenderSettings::decoratorBoundaryEdges, true);
    bindMeshCheckBox(m_decoratorTextureSeamsCheck, &PerMeshRenderSettings::decoratorTextureSeams, true);
    bindGlobalCheckBox(m_decoratorInfoCheck, &GlobalRenderSettings::showDecoratorInfo);
    bindMeshColorButton(
        m_decoratorVertexNormalColorButton,
        &PerMeshRenderSettings::decoratorVertexNormalColor,
        tr("Decorator Vertex Normal Color"));
    bindMeshColorButton(
        m_decoratorFaceNormalColorButton,
        &PerMeshRenderSettings::decoratorFaceNormalColor,
        tr("Decorator Face Normal Color"));
    bindMeshColorButton(
        m_decoratorCurvatureDirPD1ColorButton,
        &PerMeshRenderSettings::decoratorCurvatureDirPD1Color,
        tr("Curvature Max Direction Color"));
    bindMeshColorButton(
        m_decoratorCurvatureDirPD2ColorButton,
        &PerMeshRenderSettings::decoratorCurvatureDirPD2Color,
        tr("Curvature Min Direction Color"));
    bindMeshColorButton(
        m_decoratorBoundaryEdgeColorButton,
        &PerMeshRenderSettings::decoratorBoundaryEdgeColor,
        tr("Decorator Boundary Edge Color"));
    bindMeshColorButton(
        m_decoratorTextureSeamColorButton,
        &PerMeshRenderSettings::decoratorTextureSeamColor,
        tr("Decorator Texture Seam Color"));
    bindMeshCheckBox(
        m_decoratorNonManifoldEdgesCheck, &PerMeshRenderSettings::decoratorNonManifoldEdges, true);
    bindMeshColorButton(
        m_decoratorNonManifoldEdgeColorButton,
        &PerMeshRenderSettings::decoratorNonManifoldEdgeColor,
        tr("Non-Manifold Edge Color"));
    bindMeshCheckBox(
        m_decoratorNonManifoldVerticesCheck,
        &PerMeshRenderSettings::decoratorNonManifoldVertices,
        true);
    bindMeshColorButton(
        m_decoratorNonManifoldVertexColorButton,
        &PerMeshRenderSettings::decoratorNonManifoldVertexColor,
        tr("Non-Manifold Vertex Color"));
    bindMeshFloatSpin(m_decoratorBoundaryWidthSpin, &PerMeshRenderSettings::decoratorBoundaryWidth);

    bindMeshColorButton(m_bboxColorButton, &PerMeshRenderSettings::bboxWireColor, tr("Bounding Box Wire Color"));
    bindGlobalCheckBox(m_bboxShowCornersCheck, &GlobalRenderSettings::showBoundingBoxCorners);
    bindGlobalCheckBox(m_bboxShowDimensionsCheck, &GlobalRenderSettings::showBoundingBoxDimensions);

    bindMeshEnumCombo(m_bboxStyleCombo, &PerMeshRenderSettings::boundingBoxStyle);
    bindMeshEnumCombo(m_pointColorSourceCombo, &PerMeshRenderSettings::pointColorSource);
    bindMeshColorButton(m_pointsColorButton, &PerMeshRenderSettings::pointColor, tr("Point Color"));
    bindMeshFloatSpin(m_pointSizeSpin, &PerMeshRenderSettings::pointSize);
    bindMeshCheckBox(m_pointLightingCheck, &PerMeshRenderSettings::pointLighting);

    bindMeshColorButton(m_edgeColorButton, &PerMeshRenderSettings::edgeColor, tr("Edge Color"));
    bindMeshFloatSpin(m_edgeSizeSpin, &PerMeshRenderSettings::edgeSize);

    bindMeshColorButton(m_wireColorButton, &PerMeshRenderSettings::wireColor, tr("Wire Color"));
    bindMeshFloatSpin(m_wireSizeSpin, &PerMeshRenderSettings::wireSize);
    bindMeshCheckBox(m_wireBackfaceCullingCheck, &PerMeshRenderSettings::wireBackfaceCulling);
    bindMeshCheckBox(m_wireLightingCheck, &PerMeshRenderSettings::wireLighting);
    bindMeshCheckBox(m_wireRespectFauxCheck, &PerMeshRenderSettings::wireRespectFaux);

    bindMeshEnumCombo(m_fillMaterialCombo, &PerMeshRenderSettings::fillMaterial);

    // PBR source combos: use reference-returning lambdas so each combo writes
    // directly into the PbrFillParams sub-struct fields.
    auto bindPbrSourceCombo =
        [this](QComboBox *combo,
               std::function<FillPbrTextureSource &(PerMeshRenderSettings &)> getSource,
               std::function<int &(PerMeshRenderSettings &)> getIndex) {
            connect(
                combo,
                qOverload<int>(&QComboBox::currentIndexChanged),
                this,
                [this, combo, getSource, getIndex](int idx) {
                    if (!combo || idx < 0 || idx >= combo->count())
                        return;
                    const QVariant sourceData = combo->itemData(idx, kPbrSourceRole);
                    const QVariant textureData = combo->itemData(idx, kPbrTextureIndexRole);
                    if (!sourceData.isValid())
                        return;
                    bool changed = false;
                    const FillPbrTextureSource source =
                        static_cast<FillPbrTextureSource>(sourceData.toInt());
                    const int textureIndex = textureData.isValid() ? textureData.toInt() : -1;
                    if (getSource(m_meshSettings) != source) {
                        getSource(m_meshSettings) = source;
                        changed = true;
                    }
                    if (getIndex(m_meshSettings) != textureIndex) {
                        getIndex(m_meshSettings) = textureIndex;
                        changed = true;
                    }
                    if (!changed)
                        return;
                    syncFillPbrUiState();
                    emit meshSettingsChanged(m_meshSettings);
                });
        };
    bindPbrSourceCombo(
        m_fillPbrAlbedoCombo,
        [](PerMeshRenderSettings &s) -> FillPbrTextureSource & { return s.fillPbr.albedoSource; },
        [](PerMeshRenderSettings &s) -> int & { return s.fillPbr.albedoIndex; });
    bindPbrSourceCombo(
        m_fillPbrNormalCombo,
        [](PerMeshRenderSettings &s) -> FillPbrTextureSource & { return s.fillPbr.normalSource; },
        [](PerMeshRenderSettings &s) -> int & { return s.fillPbr.normalIndex; });
    bindPbrSourceCombo(
        m_fillPbrOcclusionCombo,
        [](PerMeshRenderSettings &s) -> FillPbrTextureSource & { return s.fillPbr.occlusionSource; },
        [](PerMeshRenderSettings &s) -> int & { return s.fillPbr.occlusionIndex; });
    bindPbrSourceCombo(
        m_fillPbrRoughnessCombo,
        [](PerMeshRenderSettings &s) -> FillPbrTextureSource & { return s.fillPbr.roughnessSource; },
        [](PerMeshRenderSettings &s) -> int & { return s.fillPbr.roughnessIndex; });
    connect(m_fillColorSourceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_fillColorSourceCombo || idx < 0 || idx >= m_fillColorSourceCombo->count()) return;
        const QVariant data = m_fillColorSourceCombo->itemData(idx);
        if (!data.isValid()) return;
        const auto value = static_cast<FillColorSource>(data.toInt());
        if (m_meshSettings.fillPlain.colorSource == value) return;
        m_meshSettings.fillPlain.colorSource = value;
        syncFillPbrUiState();
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(m_uvFillColorSourceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_uvFillColorSourceCombo || idx < 0 || idx >= m_uvFillColorSourceCombo->count()) return;
        const QVariant data = m_uvFillColorSourceCombo->itemData(idx);
        if (!data.isValid()) return;
        const auto value = static_cast<FillColorSource>(data.toInt());
        if (m_meshSettings.fillPlain.colorSource == value) return;
        m_meshSettings.fillPlain.colorSource = value;
        emit meshSettingsChanged(m_meshSettings);
    });
    bindMeshColorButton(m_fillColorButton, &PerMeshRenderSettings::fillColor, tr("Fill Color"));
    bindMeshColorButton(m_fillPbrColorButton, &PerMeshRenderSettings::fillColor, tr("Fill Color"));
    bindMeshColorButton(m_uvFillColorButton, &PerMeshRenderSettings::fillColor, tr("Fill Color"));
    connect(m_fillShadingCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_fillShadingCombo || idx < 0 || idx >= m_fillShadingCombo->count()) return;
        const QVariant data = m_fillShadingCombo->itemData(idx);
        if (!data.isValid()) return;
        const auto value = static_cast<FillShading>(data.toInt());
        if (m_meshSettings.fillPlain.shading == value) return;
        m_meshSettings.fillPlain.shading = value;
        emit meshSettingsChanged(m_meshSettings);
    });
    bindMeshCheckBox(m_fillBackfaceCullingCheck, &PerMeshRenderSettings::fillBackfaceCulling);
    bindMeshCheckBox(m_fillLightingCheck, &PerMeshRenderSettings::fillLighting);
    connect(m_fillPlainTextureCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_fillPlainTextureCombo) return;
        const int texIndex = m_fillPlainTextureCombo->itemData(idx).toInt();
        if (m_meshSettings.fillPlain.textureIndex == texIndex) return;
        m_meshSettings.fillPlain.textureIndex = texIndex;
        emit meshSettingsChanged(m_meshSettings);
    });
    // Sub-struct fields: use direct connects instead of member-pointer helpers.
    connect(m_fillPbrShadingCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_fillPbrShadingCombo) return;
        const auto value = static_cast<FillShading>(m_fillPbrShadingCombo->itemData(idx).toInt());
        if (m_meshSettings.fillPbr.shading == value) return;
        m_meshSettings.fillPbr.shading = value;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(m_fillNormalScaleSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const float fv = static_cast<float>(v);
        if (m_meshSettings.fillPbr.normalScale == fv) return;
        m_meshSettings.fillPbr.normalScale = fv;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(m_fillNormalSpaceCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
        if (!m_fillNormalSpaceCombo || idx < 0 || idx >= m_fillNormalSpaceCombo->count()) return;
        const QVariant data = m_fillNormalSpaceCombo->itemData(idx);
        if (!data.isValid()) return;
        const auto value = static_cast<FillPbrNormalMapSpace>(data.toInt());
        if (m_meshSettings.fillPbr.normalMapSpace == value) return;
        m_meshSettings.fillPbr.normalMapSpace = value;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(m_fillRsEnhancementSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const float fv = static_cast<float>(v);
        if (m_meshSettings.fillRs.enhancement == fv) return;
        m_meshSettings.fillRs.enhancement = fv;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(
        m_fillRsDisplayModeCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int idx) {
            if (!m_fillRsDisplayModeCombo) return;
            const QVariant data = m_fillRsDisplayModeCombo->itemData(idx);
            if (!data.isValid()) return;
            const int value = data.toInt();
            if (m_meshSettings.fillRs.displayMode == value) return;
            m_meshSettings.fillRs.displayMode = value;
            emit meshSettingsChanged(m_meshSettings);
        });
    connect(m_fillRsInvertCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (m_meshSettings.fillRs.invert == checked) return;
        m_meshSettings.fillRs.invert = checked;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(m_fillRsFlatCheck, &QCheckBox::toggled, this, [this](bool checked) {
        const FillShading s = checked ? FillShading::Flat : FillShading::Smooth;
        if (m_meshSettings.fillRs.shading == s) return;
        m_meshSettings.fillRs.shading = s;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(m_fillOcclusionStrengthSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const float fv = static_cast<float>(v);
        if (m_meshSettings.fillPbr.occlusionStrength == fv) return;
        m_meshSettings.fillPbr.occlusionStrength = fv;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(m_fillRoughnessFactorSpin, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        const float fv = static_cast<float>(v);
        if (m_meshSettings.fillPbr.roughnessFactor == fv) return;
        m_meshSettings.fillPbr.roughnessFactor = fv;
        emit meshSettingsChanged(m_meshSettings);
    });
    connect(
        m_fillMaterialCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { syncFillPbrUiState(); });
    connect(
        m_fillPbrAlbedoCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { syncFillPbrUiState(); });
    connect(
        m_fillPbrNormalCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { syncFillPbrUiState(); });
    connect(
        m_fillPbrOcclusionCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { syncFillPbrUiState(); });
    connect(
        m_fillPbrRoughnessCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { syncFillPbrUiState(); });
    connect(
        m_fillColorSourceCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int) { syncFillPbrUiState(); });
    bindMeshCheckBox(m_selectionShowVerticesCheck, &PerMeshRenderSettings::showSelectionVertices);
    bindMeshCheckBox(m_selectionShowFacesCheck, &PerMeshRenderSettings::showSelectionFaces);
    bindGlobalCheckBox(m_uvShowReferenceFrameCheck, &GlobalRenderSettings::uvShowReferenceFrame);
    bindGlobalCheckBox(m_uvShowFullTextureCheck, &GlobalRenderSettings::uvShowFullTexture);
    bindGlobalCheckBox(m_uvTextureNearestCheck, &GlobalRenderSettings::uvTextureNearestSampling);
    connect(
        m_uvTextureChannelCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int channel) {
            if (m_globalSettings.uvTextureChannel == channel)
                return;
            m_globalSettings.uvTextureChannel = channel;
            emit globalSettingsChanged(m_globalSettings);
        });
    bindGlobalEnumCombo(m_qualityHistogramSourceCombo, &GlobalRenderSettings::qualityHistogramSource);
    connect(
        m_qualityHistogramColorMapCombo,
        qOverload<int>(&QComboBox::currentIndexChanged),
        this,
        [this](int idx) {
            const QString colorMapId =
                m_qualityHistogramColorMapCombo->itemData(idx).toString().trimmed().toLower();
            if (colorMapId.isEmpty())
                return;
            if (m_globalSettings.qualityHistogramColorMapId == colorMapId)
                return;
            m_globalSettings.qualityHistogramColorMapId = colorMapId;
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityHistogramInvertCheck,
        &QCheckBox::toggled,
        this,
        [this](bool checked) {
            if (m_globalSettings.qualityHistogramInvertColorMap == checked)
                return;
            m_globalSettings.qualityHistogramInvertColorMap = checked;
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityIsolinesCheck,
        &QCheckBox::toggled,
        this,
        [this](bool checked) {
            if (m_globalSettings.qualityIsolinesEnabled == checked)
                return;
            m_globalSettings.qualityIsolinesEnabled = checked;
            if (m_qualityIsolineCountSpin)
                m_qualityIsolineCountSpin->setEnabled(checked);
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityIsolineCountSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double value) {
            const int count = std::clamp(int(std::lround(value)), 1, 100);
            if (m_globalSettings.qualityIsolineCount == count)
                return;
            m_globalSettings.qualityIsolineCount = count;
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityHistogramFixedRangeCheck,
        &QCheckBox::toggled,
        this,
        [this](bool checked) {
            if (m_globalSettings.qualityHistogramFixedRange == checked)
                return;
            m_globalSettings.qualityHistogramFixedRange = checked;
            syncQualityHistogramUiState();
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityHistogramCenterOnZeroCheck,
        &QCheckBox::toggled,
        this,
        [this](bool checked) {
            if (m_globalSettings.qualityHistogramCenterOnZero == checked)
                return;
            m_globalSettings.qualityHistogramCenterOnZero = checked;
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityHistogramPercentileCropSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double value) {
            const float crop = std::clamp(static_cast<float>(value), 0.0f, 0.5f);
            if (m_globalSettings.qualityHistogramPercentileCrop == crop)
                return;
            m_globalSettings.qualityHistogramPercentileCrop = crop;
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityHistogramMinSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double value) {
            const float v = static_cast<float>(value);
            if (m_globalSettings.qualityHistogramMin == v)
                return;
            m_globalSettings.qualityHistogramMin = v;
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityHistogramMaxSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double value) {
            const float v = static_cast<float>(value);
            if (m_globalSettings.qualityHistogramMax == v)
                return;
            m_globalSettings.qualityHistogramMax = v;
            emit globalSettingsChanged(m_globalSettings);
        });
    connect(
        m_qualityBakeVertexColorButton,
        &QPushButton::clicked,
        this,
        [this]() {
            emit bakeQualityMappingToVertexColorRequested();
        });
    connect(
        m_qualityHistogramBinsSpin,
        &QDoubleSpinBox::valueChanged,
        this,
        [this](double value) {
            const int bins = std::clamp(int(std::lround(value)), 4, 512);
            if (m_globalSettings.qualityHistogramBins == bins)
                return;
            m_globalSettings.qualityHistogramBins = bins;
            emit globalSettingsChanged(m_globalSettings);
        });

    bindPassButton(m_currentMeshButton, RenderPass::CurrentMesh, true);
    bindPassButton(m_normalsDecoratorsButton, RenderPass::DecoratorNormals, false);
    bindPassButton(m_boundaryDecoratorsButton, RenderPass::DecoratorBoundary, false);
    bindPassButton(m_bboxButton, RenderPass::BoundingBox, false);
    bindPassButton(m_pointsButton, RenderPass::Points, false);
    bindPassButton(m_edgesButton, RenderPass::Edges, false);
    bindPassButton(m_wireButton, RenderPass::Wireframe, false);
    bindPassButton(m_fillButton, RenderPass::Fill, false);
    bindPassButton(m_selectionButton, RenderPass::Selection, false);
    bindPassButton(m_qualityHistogramButton, RenderPass::QualityHistogram, false);

    bindSettingsArrow(m_currentMeshSettingsArrow, RenderPass::CurrentMesh);
    bindSettingsArrow(m_normalsDecoratorsSettingsArrow, RenderPass::DecoratorNormals);
    bindSettingsArrow(m_boundaryDecoratorsSettingsArrow, RenderPass::DecoratorBoundary);
    bindSettingsArrow(m_bboxSettingsArrow, RenderPass::BoundingBox);
    bindSettingsArrow(m_pointsSettingsArrow, RenderPass::Points);
    bindSettingsArrow(m_edgesSettingsArrow, RenderPass::Edges);
    bindSettingsArrow(m_wireSettingsArrow, RenderPass::Wireframe);
    bindSettingsArrow(m_fillSettingsArrow, RenderPass::Fill);
    bindSettingsArrow(m_selectionSettingsArrow, RenderPass::Selection);
    bindSettingsArrow(m_qualityHistogramSettingsArrow, RenderPass::QualityHistogram);

    bindMeshToolToggle(m_bboxButton, &PerMeshRenderSettings::showBoundingBox);
    // Plain pass toggles, like Points/Edges/Wire/Fill: each owns only its master flag and
    // leaves the checkboxes alone, so switching a pass off and on is lossless.
    bindMeshToolToggle(m_normalsDecoratorsButton, &PerMeshRenderSettings::decoratorNormals);
    bindMeshToolToggle(m_boundaryDecoratorsButton, &PerMeshRenderSettings::decoratorBoundary);
    bindMeshToolToggle(m_pointsButton, &PerMeshRenderSettings::showPoints);
    bindMeshToolToggle(m_edgesButton, &PerMeshRenderSettings::showEdges);
    bindMeshToolToggle(m_wireButton, &PerMeshRenderSettings::showWire);
    bindMeshToolToggle(m_fillButton, &PerMeshRenderSettings::showFill);
    bindMeshToolToggle(m_selectionButton, &PerMeshRenderSettings::showSelection);
    bindGlobalToolToggle(m_qualityHistogramButton, &GlobalRenderSettings::showQualityHistogram);

    updateColorButtonStyle(m_currentMeshOutlineColorButton, m_globalSettings.currentMeshOutlineColor);
    updateColorButtonStyle(m_sceneBackgroundTopColorButton, m_globalSettings.sceneBackgroundTopColor);
    updateColorButtonStyle(
        m_sceneBackgroundBottomColorButton,
        m_globalSettings.sceneBackgroundBottomColor);
    updateColorButtonStyle(m_decoratorVertexNormalColorButton, m_meshSettings.decoratorVertexNormalColor);
    updateColorButtonStyle(m_decoratorFaceNormalColorButton, m_meshSettings.decoratorFaceNormalColor);
    updateColorButtonStyle(m_decoratorCurvatureDirPD1ColorButton, m_meshSettings.decoratorCurvatureDirPD1Color);
    updateColorButtonStyle(m_decoratorCurvatureDirPD2ColorButton, m_meshSettings.decoratorCurvatureDirPD2Color);
    updateColorButtonStyle(m_decoratorBoundaryEdgeColorButton, m_meshSettings.decoratorBoundaryEdgeColor);
    updateColorButtonStyle(m_decoratorTextureSeamColorButton, m_meshSettings.decoratorTextureSeamColor);
    updateColorButtonStyle(
        m_decoratorNonManifoldEdgeColorButton, m_meshSettings.decoratorNonManifoldEdgeColor);
    updateColorButtonStyle(
        m_decoratorNonManifoldVertexColorButton, m_meshSettings.decoratorNonManifoldVertexColor);
    updateColorButtonStyle(m_bboxColorButton, m_meshSettings.bboxWireColor);
    updateColorButtonStyle(m_pointsColorButton, m_meshSettings.pointColor);
    updateColorButtonStyle(m_edgeColorButton, m_meshSettings.edgeColor);
    updateColorButtonStyle(m_wireColorButton, m_meshSettings.wireColor);
    updateColorButtonStyle(m_fillColorButton, m_meshSettings.fillColor);
    updateColorButtonStyle(m_fillPbrColorButton, m_meshSettings.fillColor);
    setPointColorSourceAvailability(false, false);
    setPointLightingAvailability(false);
    setFillColorSourceAvailability(false, false, false, false, false);
    setFillPbrMapAvailability(false, false, false);
    if (m_settingsStack)
        m_settingsStack->setCurrentIndex(renderPassPageIndex(m_globalSettings.currentPass));
    syncViewerSettingsModeUi();
    if (m_settingsContainer)
        m_settingsContainer->setVisible(m_globalSettings.settingsPanelVisible);
    if (!m_globalSettings.settingsPanelVisible)
        stopSettingsAutoCloseTimer();
    updateSettingsPanelGeometry();
    syncRenderPassUiState();
}

RenderOverlayPanel::~RenderOverlayPanel()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

bool RenderOverlayPanel::eventFilter(QObject *watched, QEvent *event)
{
    if (m_settingsContainer && m_settingsContainer->isVisible()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            const QPoint localPos = mapFromGlobal(mouseEvent->globalPosition().toPoint());
            if (rect().contains(localPos))
                stopSettingsAutoCloseTimer();
            else
                startSettingsAutoCloseTimer();
        } else if (event->type() == QEvent::WindowDeactivate) {
            startSettingsAutoCloseTimer();
        }
    }
    return QWidget::eventFilter(watched, event);
}

int RenderOverlayPanel::renderPassPageIndex(RenderPass pass) const
{
    if (m_viewerModeUv && pass == RenderPass::Fill)
        return 10;
    switch (pass) {
    case RenderPass::CurrentMesh: return 0;
    case RenderPass::DecoratorNormals: return 1;
    case RenderPass::DecoratorBoundary: return 2;
    case RenderPass::BoundingBox: return 3;
    case RenderPass::Points: return 4;
    case RenderPass::Edges: return 5;
    case RenderPass::Wireframe: return 6;
    case RenderPass::Fill: return 7;
    case RenderPass::Selection: return 8;
    case RenderPass::QualityHistogram: return 9;
    }
    return 0;
}

void RenderOverlayPanel::setCurrentRenderPass(RenderPass pass)
{
    if (m_globalSettings.currentPass == pass)
        return;
    m_globalSettings.currentPass = pass;
    if (m_settingsStack)
        m_settingsStack->setCurrentIndex(renderPassPageIndex(pass));
    updateSettingsPanelGeometry();
    syncRenderPassUiState();
    emit globalSettingsChanged(m_globalSettings);
}

void RenderOverlayPanel::setSettingsVisible(bool visible)
{
    stopSettingsAutoCloseTimer();
    if (m_globalSettings.settingsPanelVisible == visible)
        return;
    m_globalSettings.settingsPanelVisible = visible;
    if (m_settingsContainer)
        m_settingsContainer->setVisible(visible);
    updateSettingsPanelGeometry();
    adjustSize();
    syncRenderPassUiState();
    emit globalSettingsChanged(m_globalSettings);
}

void RenderOverlayPanel::startSettingsAutoCloseTimer()
{
    if (m_settingsAutoCloseTimer && m_settingsContainer && m_settingsContainer->isVisible())
        m_settingsAutoCloseTimer->start();
}

void RenderOverlayPanel::stopSettingsAutoCloseTimer()
{
    if (m_settingsAutoCloseTimer)
        m_settingsAutoCloseTimer->stop();
}

void RenderOverlayPanel::updateSettingsPanelGeometry()
{
    if (m_fillMaterialStack)
        m_fillMaterialStack->updateGeometry();
    if (m_viewerSettingsStack)
        m_viewerSettingsStack->updateGeometry();
    if (m_settingsStack)
        m_settingsStack->updateGeometry();
    if (m_settingsContainer)
        m_settingsContainer->updateGeometry();
    updateGeometry();
    adjustSize();
}

void RenderOverlayPanel::setViewerModeUv(bool uvMode)
{
    if (m_viewerModeUv == uvMode)
        return;
    m_viewerModeUv = uvMode;
    syncViewerSettingsModeUi();
    // If currently showing the fill pass, switch between the 3D and UV fill pages.
    if (m_globalSettings.currentPass == RenderPass::Fill && m_settingsStack) {
        m_settingsStack->setCurrentIndex(renderPassPageIndex(RenderPass::Fill));
        updateSettingsPanelGeometry();
    }
}

void RenderOverlayPanel::syncViewerSettingsModeUi()
{
    if (!m_viewerSettingsStack)
        return;
    m_viewerSettingsStack->setCurrentIndex(0);
}

void RenderOverlayPanel::setGlobalSettings(const RenderSettings &settings)
{
    m_globalSettings = settings;

    // Reverse half of every bindGlobal* call; see m_globalSyncers.
    for (const auto &sync : m_globalSyncers)
        sync();

    if (m_showTrackballGizmoCheck) {
        QSignalBlocker blocker(m_showTrackballGizmoCheck);
        m_showTrackballGizmoCheck->setChecked(m_globalSettings.showTrackballGizmo);
    }
    if (m_showAxisGizmoCheck) {
        QSignalBlocker blocker(m_showAxisGizmoCheck);
        m_showAxisGizmoCheck->setChecked(m_globalSettings.showAxisGizmo);
    }
    if (m_uvShowFullTextureCheck) {
        QSignalBlocker blocker(m_uvShowFullTextureCheck);
        m_uvShowFullTextureCheck->setChecked(m_globalSettings.uvShowFullTexture);
    }
    if (m_uvTextureChannelCombo) {
        QSignalBlocker blocker(m_uvTextureChannelCombo);
        m_uvTextureChannelCombo->setCurrentIndex(
            std::clamp(m_globalSettings.uvTextureChannel, 0, 3));
    }
    if (m_bboxShowCornersCheck) {
        QSignalBlocker blocker(m_bboxShowCornersCheck);
        m_bboxShowCornersCheck->setChecked(m_globalSettings.showBoundingBoxCorners);
    }
    if (m_bboxShowDimensionsCheck) {
        QSignalBlocker blocker(m_bboxShowDimensionsCheck);
        m_bboxShowDimensionsCheck->setChecked(m_globalSettings.showBoundingBoxDimensions);
    }
    if (m_settingsContainer
        && m_settingsContainer->isVisible() != m_globalSettings.settingsPanelVisible) {
        m_settingsContainer->setVisible(m_globalSettings.settingsPanelVisible);
        updateSettingsPanelGeometry();
        adjustSize();
    }
    if (m_currentMeshDilateRadiusSpin) {
        QSignalBlocker blocker(m_currentMeshDilateRadiusSpin);
        m_currentMeshDilateRadiusSpin->setValue(m_globalSettings.currentMeshDilateRadius);
    }
    if (m_currentMeshDebugViewCombo) {
        QSignalBlocker blocker(m_currentMeshDebugViewCombo);
        const int value = static_cast<int>(m_globalSettings.currentMeshDebugView);
        for (int i = 0; i < m_currentMeshDebugViewCombo->count(); ++i) {
            if (m_currentMeshDebugViewCombo->itemData(i).toInt() == value) {
                m_currentMeshDebugViewCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_qualityHistogramBinsSpin) {
        QSignalBlocker blocker(m_qualityHistogramBinsSpin);
        m_qualityHistogramBinsSpin->setValue(m_globalSettings.qualityHistogramBins);
    }
    if (m_qualityHistogramFixedRangeCheck) {
        QSignalBlocker blocker(m_qualityHistogramFixedRangeCheck);
        m_qualityHistogramFixedRangeCheck->setChecked(m_globalSettings.qualityHistogramFixedRange);
    }
    if (m_qualityHistogramCenterOnZeroCheck) {
        QSignalBlocker blocker(m_qualityHistogramCenterOnZeroCheck);
        m_qualityHistogramCenterOnZeroCheck->setChecked(m_globalSettings.qualityHistogramCenterOnZero);
    }
    if (m_qualityHistogramPercentileCropSpin) {
        QSignalBlocker blocker(m_qualityHistogramPercentileCropSpin);
        m_qualityHistogramPercentileCropSpin->setValue(m_globalSettings.qualityHistogramPercentileCrop);
    }
    if (m_qualityHistogramMinSpin) {
        QSignalBlocker blocker(m_qualityHistogramMinSpin);
        m_qualityHistogramMinSpin->setValue(m_globalSettings.qualityHistogramMin);
    }
    if (m_qualityHistogramMaxSpin) {
        QSignalBlocker blocker(m_qualityHistogramMaxSpin);
        m_qualityHistogramMaxSpin->setValue(m_globalSettings.qualityHistogramMax);
    }
    if (m_qualityHistogramColorMapCombo) {
        QSignalBlocker blocker(m_qualityHistogramColorMapCombo);
        QString value = m_globalSettings.qualityHistogramColorMapId.trimmed().toLower();
        int idx = m_qualityHistogramColorMapCombo->findData(value);
        if (idx < 0) {
            idx = m_qualityHistogramColorMapCombo->findData(
                ColorMapRegistry::instance().fallbackMapId());
        }
        if (idx < 0 && m_qualityHistogramColorMapCombo->count() > 0)
            idx = 0;
        if (idx >= 0) {
            m_qualityHistogramColorMapCombo->setCurrentIndex(idx);
            m_globalSettings.qualityHistogramColorMapId =
                m_qualityHistogramColorMapCombo->itemData(idx).toString();
        }
    }
    if (m_qualityHistogramInvertCheck) {
        QSignalBlocker blocker(m_qualityHistogramInvertCheck);
        m_qualityHistogramInvertCheck->setChecked(m_globalSettings.qualityHistogramInvertColorMap);
    }
    syncQualityHistogramUiState();
    if (m_settingsContainer)
        m_settingsContainer->setVisible(m_globalSettings.settingsPanelVisible);
    if (!m_globalSettings.settingsPanelVisible)
        stopSettingsAutoCloseTimer();
    if (m_settingsStack)
        m_settingsStack->setCurrentIndex(renderPassPageIndex(m_globalSettings.currentPass));
    syncViewerSettingsModeUi();
    updateSettingsPanelGeometry();

    syncRenderPassUiState();
}

void RenderOverlayPanel::setMeshSettings(const PerMeshRenderSettings &settings)
{
    m_meshSettings = settings;

    // Reverse half of every bindMesh* call; see m_meshSyncers.
    for (const auto &sync : m_meshSyncers)
        sync();

    if (m_normalsDecoratorsButton) {
        QSignalBlocker blocker(m_normalsDecoratorsButton);
        m_normalsDecoratorsButton->setChecked(m_meshSettings.decoratorNormals);
    }
    if (m_pointsButton) {
        QSignalBlocker blocker(m_pointsButton);
        m_pointsButton->setChecked(m_meshSettings.showPoints);
    }
    if (m_wireButton) {
        QSignalBlocker blocker(m_wireButton);
        m_wireButton->setChecked(m_meshSettings.showWire);
    }
    if (m_selectionButton) {
        QSignalBlocker blocker(m_selectionButton);
        m_selectionButton->setChecked(m_meshSettings.showSelection);
    }
    if (m_selectionShowFacesCheck) {
        QSignalBlocker blocker(m_selectionShowFacesCheck);
        m_selectionShowFacesCheck->setChecked(m_meshSettings.showSelectionFaces);
    }
    if (m_decoratorFaceNormalsCheck) {
        QSignalBlocker blocker(m_decoratorFaceNormalsCheck);
        m_decoratorFaceNormalsCheck->setChecked(m_meshSettings.decoratorFaceNormals);
    }
    if (m_decoratorBoundaryEdgesCheck) {
        QSignalBlocker blocker(m_decoratorBoundaryEdgesCheck);
        m_decoratorBoundaryEdgesCheck->setChecked(m_meshSettings.decoratorBoundaryEdges);
    }
    if (m_decoratorNonManifoldEdgesCheck) {
        QSignalBlocker blocker(m_decoratorNonManifoldEdgesCheck);
        m_decoratorNonManifoldEdgesCheck->setChecked(m_meshSettings.decoratorNonManifoldEdges);
    }
    if (m_pointLightingCheck) {
        QSignalBlocker blocker(m_pointLightingCheck);
        m_pointLightingCheck->setChecked(m_meshSettings.pointLighting);
    }
    if (m_wireBackfaceCullingCheck) {
        QSignalBlocker blocker(m_wireBackfaceCullingCheck);
        m_wireBackfaceCullingCheck->setChecked(m_meshSettings.wireBackfaceCulling);
    }
    if (m_fillLightingCheck) {
        QSignalBlocker blocker(m_fillLightingCheck);
        m_fillLightingCheck->setChecked(m_meshSettings.fillLighting);
    }
    if (m_fillNormalScaleSpin) {
        QSignalBlocker blocker(m_fillNormalScaleSpin);
        m_fillNormalScaleSpin->setValue(m_meshSettings.fillPbr.normalScale);
    }
    if (m_fillNormalSpaceCombo) {
        QSignalBlocker blocker(m_fillNormalSpaceCombo);
        const int value = static_cast<int>(m_meshSettings.fillPbr.normalMapSpace);
        for (int i = 0; i < m_fillNormalSpaceCombo->count(); ++i) {
            if (m_fillNormalSpaceCombo->itemData(i).toInt() == value) {
                m_fillNormalSpaceCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_fillRsEnhancementSpin) {
        QSignalBlocker blocker(m_fillRsEnhancementSpin);
        m_fillRsEnhancementSpin->setValue(m_meshSettings.fillRs.enhancement);
    }
    if (m_fillRsDisplayModeCombo) {
        QSignalBlocker blocker(m_fillRsDisplayModeCombo);
        for (int i = 0; i < m_fillRsDisplayModeCombo->count(); ++i) {
            if (m_fillRsDisplayModeCombo->itemData(i).toInt() == m_meshSettings.fillRs.displayMode) {
                m_fillRsDisplayModeCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_fillRsInvertCheck) {
        QSignalBlocker blocker(m_fillRsInvertCheck);
        m_fillRsInvertCheck->setChecked(m_meshSettings.fillRs.invert);
    }
    if (m_fillRsFlatCheck) {
        QSignalBlocker blocker(m_fillRsFlatCheck);
        m_fillRsFlatCheck->setChecked(m_meshSettings.fillRs.shading == FillShading::Flat);
    }
    if (m_fillOcclusionStrengthSpin) {
        QSignalBlocker blocker(m_fillOcclusionStrengthSpin);
        m_fillOcclusionStrengthSpin->setValue(m_meshSettings.fillPbr.occlusionStrength);
    }
    if (m_fillRoughnessFactorSpin) {
        QSignalBlocker blocker(m_fillRoughnessFactorSpin);
        m_fillRoughnessFactorSpin->setValue(m_meshSettings.fillPbr.roughnessFactor);
    }
    if (m_pointSizeSpin) {
        QSignalBlocker blocker(m_pointSizeSpin);
        m_pointSizeSpin->setValue(m_meshSettings.pointSize);
    }
    if (m_edgeSizeSpin) {
        QSignalBlocker blocker(m_edgeSizeSpin);
        m_edgeSizeSpin->setValue(m_meshSettings.edgeSize);
    }
    if (m_pointColorSourceCombo) {
        QSignalBlocker blocker(m_pointColorSourceCombo);
        const int value = static_cast<int>(m_meshSettings.pointColorSource);
        for (int i = 0; i < m_pointColorSourceCombo->count(); ++i) {
            if (m_pointColorSourceCombo->itemData(i).toInt() == value) {
                m_pointColorSourceCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_fillShadingCombo) {
        QSignalBlocker blocker(m_fillShadingCombo);
        const int value = static_cast<int>(m_meshSettings.fillPlain.shading);
        for (int i = 0; i < m_fillShadingCombo->count(); ++i) {
            if (m_fillShadingCombo->itemData(i).toInt() == value) {
                m_fillShadingCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_fillPbrShadingCombo) {
        QSignalBlocker blocker(m_fillPbrShadingCombo);
        const int value = static_cast<int>(m_meshSettings.fillPbr.shading);
        for (int i = 0; i < m_fillPbrShadingCombo->count(); ++i) {
            if (m_fillPbrShadingCombo->itemData(i).toInt() == value) {
                m_fillPbrShadingCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    syncFillPbrSourceCombo(
        m_fillPbrAlbedoCombo,
        m_meshSettings.fillPbr.albedoSource,
        m_meshSettings.fillPbr.albedoIndex);
    syncFillPbrSourceCombo(
        m_fillPbrNormalCombo,
        m_meshSettings.fillPbr.normalSource,
        m_meshSettings.fillPbr.normalIndex);
    syncFillPbrSourceCombo(
        m_fillPbrOcclusionCombo,
        m_meshSettings.fillPbr.occlusionSource,
        m_meshSettings.fillPbr.occlusionIndex);
    syncFillPbrSourceCombo(
        m_fillPbrRoughnessCombo,
        m_meshSettings.fillPbr.roughnessSource,
        m_meshSettings.fillPbr.roughnessIndex);
    if (m_fillColorSourceCombo) {
        QSignalBlocker blocker(m_fillColorSourceCombo);
        const int value = static_cast<int>(m_meshSettings.fillPlain.colorSource);
        for (int i = 0; i < m_fillColorSourceCombo->count(); ++i) {
            if (m_fillColorSourceCombo->itemData(i).toInt() == value) {
                m_fillColorSourceCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_uvFillColorSourceCombo) {
        QSignalBlocker blocker(m_uvFillColorSourceCombo);
        const int value = static_cast<int>(m_meshSettings.fillPlain.colorSource);
        for (int i = 0; i < m_uvFillColorSourceCombo->count(); ++i) {
            if (m_uvFillColorSourceCombo->itemData(i).toInt() == value) {
                m_uvFillColorSourceCombo->setCurrentIndex(i);
                break;
            }
        }
    }
    if (m_fillPlainTextureCombo) {
        QSignalBlocker blocker(m_fillPlainTextureCombo);
        int selectIdx = 0;
        for (int i = 0; i < m_fillPlainTextureCombo->count(); ++i) {
            if (m_fillPlainTextureCombo->itemData(i).toInt() == m_meshSettings.fillPlain.textureIndex) {
                selectIdx = i;
                break;
            }
        }
        m_fillPlainTextureCombo->setCurrentIndex(selectIdx);
    }
    syncFillPbrUiState();

}

void RenderOverlayPanel::setPointColorSourceAvailability(
    bool hasVertexColors, bool hasVertexQuality)
{
    if (!m_pointColorSourceCombo)
        return;

    const int vertexIndex =
        m_pointColorSourceCombo->findData(static_cast<int>(PointColorSource::PerVertex));
    const int qualityIndex =
        m_pointColorSourceCombo->findData(static_cast<int>(PointColorSource::PerVertexQuality));
    if (vertexIndex >= 0) {
        m_pointColorSourceCombo->setItemData(
            vertexIndex,
            hasVertexColors ? QVariant() : QVariant(0),
            Qt::UserRole - 1);
    }
    if (qualityIndex >= 0) {
        m_pointColorSourceCombo->setItemData(
            qualityIndex,
            hasVertexQuality ? QVariant() : QVariant(0),
            Qt::UserRole - 1);
    }
}

void RenderOverlayPanel::setPointLightingAvailability(bool hasVertexNormals)
{
    if (!m_pointLightingCheck)
        return;
    m_pointLightingCheck->setEnabled(hasVertexNormals);
}

void RenderOverlayPanel::setFillColorSourceAvailability(
    bool hasVertexColors,
    bool hasFaceColors,
    bool hasVertexQuality,
    bool hasFaceQuality,
    bool hasTextures)
{
    if (!m_fillColorSourceCombo)
        return;
    m_fillHasTextures = hasTextures;

    const int vertexIndex =
        m_fillColorSourceCombo->findData(static_cast<int>(FillColorSource::PerVertex));
    const int faceIndex =
        m_fillColorSourceCombo->findData(static_cast<int>(FillColorSource::PerFace));
    const int vertexQualityIndex =
        m_fillColorSourceCombo->findData(static_cast<int>(FillColorSource::PerVertexQuality));
    const int faceQualityIndex =
        m_fillColorSourceCombo->findData(static_cast<int>(FillColorSource::PerFaceQuality));
    const int textureIndex =
        m_fillColorSourceCombo->findData(static_cast<int>(FillColorSource::Texture));

    if (vertexIndex >= 0) {
        m_fillColorSourceCombo->setItemData(
            vertexIndex,
            hasVertexColors ? QVariant() : QVariant(0),
            Qt::UserRole - 1);
    }
    if (faceIndex >= 0) {
        m_fillColorSourceCombo->setItemData(
            faceIndex,
            hasFaceColors ? QVariant() : QVariant(0),
            Qt::UserRole - 1);
    }
    if (vertexQualityIndex >= 0) {
        m_fillColorSourceCombo->setItemData(
            vertexQualityIndex,
            hasVertexQuality ? QVariant() : QVariant(0),
            Qt::UserRole - 1);
    }
    if (faceQualityIndex >= 0) {
        m_fillColorSourceCombo->setItemData(
            faceQualityIndex,
            hasFaceQuality ? QVariant() : QVariant(0),
            Qt::UserRole - 1);
    }
    if (textureIndex >= 0) {
        m_fillColorSourceCombo->setItemData(
            textureIndex,
            hasTextures ? QVariant() : QVariant(0),
            Qt::UserRole - 1);
    }
    rebuildFillPbrSourceCombos();
    if (m_uvShowFullTextureCheck)
        m_uvShowFullTextureCheck->setEnabled(hasTextures);
    syncFillPbrUiState();
}

void RenderOverlayPanel::setFillPbrMapAvailability(
    bool hasNormalMap,
    bool hasOcclusionMap,
    bool hasRoughnessMap)
{
    m_fillHasNormalMap = hasNormalMap;
    m_fillHasOcclusionMap = hasOcclusionMap;
    m_fillHasRoughnessMap = hasRoughnessMap;
    syncFillPbrUiState();
}

void RenderOverlayPanel::setFillPbrTextureNames(const QStringList &textureNames)
{
    if (m_fillTextureNames == textureNames)
        return;
    m_fillTextureNames = textureNames;
    rebuildFillPbrSourceCombos();
    // Also rebuild the plain material texture picker.
    if (m_fillPlainTextureCombo) {
        QSignalBlocker blocker(m_fillPlainTextureCombo);
        const int prevIndex = m_meshSettings.fillPlain.textureIndex;
        m_fillPlainTextureCombo->clear();
        m_fillPlainTextureCombo->addItem(tr("Automatic (per-face assignment)"), -1);
        for (int i = 0; i < m_fillTextureNames.size(); ++i)
            m_fillPlainTextureCombo->addItem(m_fillTextureNames.at(i), i);
        int selectIdx = 0;
        for (int i = 0; i < m_fillPlainTextureCombo->count(); ++i) {
            if (m_fillPlainTextureCombo->itemData(i).toInt() == prevIndex) {
                selectIdx = i;
                break;
            }
        }
        m_fillPlainTextureCombo->setCurrentIndex(selectIdx);
    }
}

void RenderOverlayPanel::rebuildFillPbrSourceCombos()
{
    rebuildFillPbrSourceCombo(
        m_fillPbrAlbedoCombo,
        m_meshSettings.fillPbr.albedoSource,
        m_meshSettings.fillPbr.albedoIndex);
    rebuildFillPbrSourceCombo(
        m_fillPbrNormalCombo,
        m_meshSettings.fillPbr.normalSource,
        m_meshSettings.fillPbr.normalIndex);
    rebuildFillPbrSourceCombo(
        m_fillPbrOcclusionCombo,
        m_meshSettings.fillPbr.occlusionSource,
        m_meshSettings.fillPbr.occlusionIndex);
    rebuildFillPbrSourceCombo(
        m_fillPbrRoughnessCombo,
        m_meshSettings.fillPbr.roughnessSource,
        m_meshSettings.fillPbr.roughnessIndex);
}

void RenderOverlayPanel::rebuildFillPbrSourceCombo(
    QComboBox *combo,
    FillPbrTextureSource currentSource,
    int currentTextureIndex)
{
    if (!combo)
        return;

    QSignalBlocker blocker(combo);
    combo->clear();

    auto addItem = [this, combo](const QString &label, FillPbrTextureSource source, int textureIndex) {
        combo->addItem(label);
        const int idx = combo->count() - 1;
        combo->setItemData(idx, static_cast<int>(source), kPbrSourceRole);
        combo->setItemData(idx, textureIndex, kPbrTextureIndexRole);
        if (source == FillPbrTextureSource::Texture && !m_fillHasTextures)
            combo->setItemData(idx, QVariant(0), Qt::UserRole - 1);
    };

    addItem(tr("None"), FillPbrTextureSource::None, -1);
    addItem(tr("Constant"), FillPbrTextureSource::Constant, -1);
    addItem(tr("Automatic (per-face assignment)"), FillPbrTextureSource::Texture, -1);
    for (int i = 0; i < m_fillTextureNames.size(); ++i)
        addItem(m_fillTextureNames.at(i), FillPbrTextureSource::Texture, i);

    syncFillPbrSourceCombo(combo, currentSource, currentTextureIndex);
}

void RenderOverlayPanel::syncFillPbrSourceCombo(
    QComboBox *combo,
    FillPbrTextureSource currentSource,
    int currentTextureIndex)
{
    if (!combo)
        return;

    QSignalBlocker blocker(combo);
    int selected = -1;
    const int sourceValue = static_cast<int>(currentSource);
    for (int i = 0; i < combo->count(); ++i) {
        const int itemSource = combo->itemData(i, kPbrSourceRole).toInt();
        const int itemTexIndex = combo->itemData(i, kPbrTextureIndexRole).toInt();
        if (itemSource == sourceValue && itemTexIndex == currentTextureIndex) {
            selected = i;
            break;
        }
    }
    if (selected < 0) {
        for (int i = 0; i < combo->count(); ++i) {
            const int itemSource = combo->itemData(i, kPbrSourceRole).toInt();
            const int itemTexIndex = combo->itemData(i, kPbrTextureIndexRole).toInt();
            if (itemSource == sourceValue && itemTexIndex == -1) {
                selected = i;
                break;
            }
        }
    }
    if (selected < 0)
        selected = 0;
    combo->setCurrentIndex(selected);
}

void RenderOverlayPanel::syncFillPbrUiState()
{
    const bool usePbr = (m_meshSettings.fillMaterial == FillMaterial::Pbr);
    const bool useRs  = (m_meshSettings.fillMaterial == FillMaterial::RadianceScaling);
    const bool usePlain = !usePbr && !useRs;
    const bool textureColorSource =
        (m_meshSettings.fillPlain.colorSource == FillColorSource::Texture);
    if (m_fillPlainForm)
        m_fillPlainForm->setRowVisible(m_fillPlainTextureCombo, usePlain && textureColorSource);

    if (m_fillMaterialStack) {
        int stackIdx = 0;
        if (usePbr)  stackIdx = 1;
        if (useRs)   stackIdx = 2;
        m_fillMaterialStack->setCurrentIndex(stackIdx);
    }
    updateSettingsPanelGeometry();

    if (m_fillRsEnhancementSpin)
        m_fillRsEnhancementSpin->setEnabled(useRs);
    if (m_fillRsDisplayModeCombo)
        m_fillRsDisplayModeCombo->setEnabled(useRs);
    if (m_fillRsInvertCheck)
        m_fillRsInvertCheck->setEnabled(useRs);
    if (m_fillRsFlatCheck)
        m_fillRsFlatCheck->setEnabled(useRs);

    const bool constantAlbedo = (m_meshSettings.fillPbr.albedoSource == FillPbrTextureSource::Constant);
    if (m_fillPbrAlbedoCombo)
        m_fillPbrAlbedoCombo->setEnabled(usePbr);
    if (m_fillPbrNormalCombo)
        m_fillPbrNormalCombo->setEnabled(usePbr);
    if (m_fillPbrOcclusionCombo)
        m_fillPbrOcclusionCombo->setEnabled(usePbr);
    if (m_fillPbrRoughnessCombo)
        m_fillPbrRoughnessCombo->setEnabled(usePbr);
    if (m_fillPbrColorButton)
        m_fillPbrColorButton->setEnabled(usePbr && constantAlbedo);

    const bool normalEnabled =
        usePbr && m_meshSettings.fillPbr.normalSource == FillPbrTextureSource::Texture;
    if (m_fillNormalSpaceCombo)
        m_fillNormalSpaceCombo->setEnabled(normalEnabled);
    if (m_fillNormalScaleSpin)
        m_fillNormalScaleSpin->setEnabled(normalEnabled);

    const bool occlusionEnabled = usePbr && m_meshSettings.fillPbr.occlusionSource != FillPbrTextureSource::None;
    if (m_fillOcclusionStrengthSpin)
        m_fillOcclusionStrengthSpin->setEnabled(occlusionEnabled);

    const bool roughnessEnabled = usePbr && m_meshSettings.fillPbr.roughnessSource != FillPbrTextureSource::None;
    if (m_fillRoughnessFactorSpin)
        m_fillRoughnessFactorSpin->setEnabled(roughnessEnabled);
}

void RenderOverlayPanel::updateColorButtonStyle(QPushButton *button, const QColor &color)
{
    if (!button)
        return;
    button->setText(QString());
    button->setStyleSheet(QStringLiteral(
        "QPushButton { background: %1; border: 1px solid rgba(40,40,40,160); border-radius: 3px; padding: 0px; min-width: %2px; min-height: %2px; max-width: %2px; max-height: %2px; }"
        "QPushButton:hover { border-color: rgba(36,132,210,220); }")
            .arg(color.name())
            .arg(kColorButtonSize));
}

void RenderOverlayPanel::syncQualityHistogramUiState()
{
    const bool fixedRange = m_globalSettings.qualityHistogramFixedRange;

    setHistogramRowVisible(
        m_qualityHistogramCenterOnZeroLabel,
        m_qualityHistogramCenterOnZeroCheck ? m_qualityHistogramCenterOnZeroCheck->parentWidget() : nullptr,
        !fixedRange);
    setHistogramRowVisible(
        m_qualityHistogramPercentileCropLabel,
        m_qualityHistogramPercentileCropSpin,
        !fixedRange);
    setHistogramRowVisible(m_qualityHistogramMinLabel, m_qualityHistogramMinSpin, fixedRange);
    setHistogramRowVisible(m_qualityHistogramMaxLabel, m_qualityHistogramMaxSpin, fixedRange);
    updateSettingsPanelGeometry();
}

void RenderOverlayPanel::syncRenderPassUiState()
{
    auto setPassMarker = [this](QToolButton *btn, RenderPass pass) {
        if (!btn)
            return;
        const bool isTarget = (m_globalSettings.currentPass == pass);
        if (btn->property("settingsTarget").toBool() == isTarget)
            return;
        btn->setProperty("settingsTarget", isTarget);
        btn->style()->unpolish(btn);
        btn->style()->polish(btn);
        btn->update();
    };

    auto setArrowChecked = [this](QToolButton *btn, RenderPass pass) {
        if (!btn)
            return;
        const bool isTarget =
            m_globalSettings.settingsPanelVisible && (m_globalSettings.currentPass == pass);
        QSignalBlocker blocker(btn);
        btn->setChecked(isTarget);
        btn->update();
    };

    setPassMarker(m_currentMeshButton, RenderPass::CurrentMesh);
    setPassMarker(m_normalsDecoratorsButton, RenderPass::DecoratorNormals);
    setPassMarker(m_boundaryDecoratorsButton, RenderPass::DecoratorBoundary);
    setPassMarker(m_bboxButton, RenderPass::BoundingBox);
    setPassMarker(m_pointsButton, RenderPass::Points);
    setPassMarker(m_edgesButton, RenderPass::Edges);
    setPassMarker(m_wireButton, RenderPass::Wireframe);
    setPassMarker(m_fillButton, RenderPass::Fill);
    setPassMarker(m_selectionButton, RenderPass::Selection);
    setPassMarker(m_qualityHistogramButton, RenderPass::QualityHistogram);

    setArrowChecked(m_currentMeshSettingsArrow, RenderPass::CurrentMesh);
    setArrowChecked(m_normalsDecoratorsSettingsArrow, RenderPass::DecoratorNormals);
    setArrowChecked(m_boundaryDecoratorsSettingsArrow, RenderPass::DecoratorBoundary);
    setArrowChecked(m_bboxSettingsArrow, RenderPass::BoundingBox);
    setArrowChecked(m_pointsSettingsArrow, RenderPass::Points);
    setArrowChecked(m_edgesSettingsArrow, RenderPass::Edges);
    setArrowChecked(m_wireSettingsArrow, RenderPass::Wireframe);
    setArrowChecked(m_fillSettingsArrow, RenderPass::Fill);
    setArrowChecked(m_selectionSettingsArrow, RenderPass::Selection);
    setArrowChecked(m_qualityHistogramSettingsArrow, RenderPass::QualityHistogram);
}
