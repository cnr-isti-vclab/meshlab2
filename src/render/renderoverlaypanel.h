#pragma once

#include "renderingsettings.h"
#include <QWidget>
#include <functional>
#include <vector>

class QPushButton;
class QDoubleSpinBox;
class QComboBox;
class QCheckBox;
class QEvent;
class QLabel;
class QObject;
class QStackedWidget;
class QToolButton;
class QFormLayout;
class QTimer;

class RenderOverlayPanel : public QWidget
{
    Q_OBJECT
public:
    explicit RenderOverlayPanel(QWidget *parent = nullptr);
    ~RenderOverlayPanel() override;

    const RenderSettings &globalSettings() const { return m_globalSettings; }
    const PerMeshRenderSettings &meshSettings() const { return m_meshSettings; }
    void setGlobalSettings(const RenderSettings &settings);
    void setMeshSettings(const PerMeshRenderSettings &settings);
    void setViewerModeUv(bool uvMode);
    void setPointColorSourceAvailability(bool hasVertexColors, bool hasVertexQuality);
    void setPointLightingAvailability(bool hasVertexNormals);
    void setFillColorSourceAvailability(
        bool hasVertexColors,
        bool hasFaceColors,
        bool hasVertexQuality,
        bool hasFaceQuality,
        bool hasTextures);
    void setFillPbrMapAvailability(
        bool hasNormalMap,
        bool hasOcclusionMap,
        bool hasRoughnessMap);
    void setFillPbrTextureNames(const QStringList &textureNames);

signals:
    void globalSettingsChanged(const RenderSettings &settings);
    void meshSettingsChanged(const PerMeshRenderSettings &settings);
    void applyToAllMeshesRequested(const PerMeshRenderSettings &settings, RenderPass pass);
    void bakeQualityMappingToVertexColorRequested();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setCurrentRenderPass(RenderPass pass);
    void setSettingsVisible(bool visible);
    void startSettingsAutoCloseTimer();
    void stopSettingsAutoCloseTimer();
    void updateSettingsPanelGeometry();
    int renderPassPageIndex(RenderPass pass) const;
    void syncViewerSettingsModeUi();
    void syncRenderPassUiState();
    void syncFillPbrUiState();
    void syncQualityHistogramUiState();
    void rebuildFillPbrSourceCombos();
    void rebuildFillPbrSourceCombo(
        QComboBox *combo,
        FillPbrTextureSource currentSource,
        int currentTextureIndex);
    void syncFillPbrSourceCombo(
        QComboBox *combo,
        FillPbrTextureSource currentSource,
        int currentTextureIndex);
    void updateColorButtonStyle(QPushButton *button, const QColor &color);

    RenderSettings m_globalSettings;
    PerMeshRenderSettings m_meshSettings;

    // Reverse half of the bind* helpers: each bind registers a closure that writes
    // the current struct value back into its widget. setGlobalSettings/setMeshSettings
    // just run these, so a control cannot be bound one-way by accident — which is how
    // the "show view cameras" checkbox ended up never refreshing.
    std::vector<std::function<void()>> m_globalSyncers;
    std::vector<std::function<void()>> m_meshSyncers;
    bool m_viewerModeUv = false;

    QWidget *m_settingsContainer = nullptr;
    QStackedWidget *m_settingsStack = nullptr;
    QStackedWidget *m_viewerSettingsStack = nullptr;
    QTimer *m_settingsAutoCloseTimer = nullptr;
    QPushButton *m_currentMeshOutlineColorButton = nullptr;
    QPushButton *m_sceneBackgroundTopColorButton = nullptr;
    QPushButton *m_sceneBackgroundBottomColorButton = nullptr;
    QPushButton *m_decoratorVertexNormalColorButton = nullptr;
    QPushButton *m_decoratorFaceNormalColorButton = nullptr;
    QPushButton *m_decoratorCurvatureDirPD1ColorButton = nullptr;
    QPushButton *m_decoratorCurvatureDirPD2ColorButton = nullptr;
    QPushButton *m_decoratorBoundaryEdgeColorButton = nullptr;
    QPushButton *m_decoratorTextureSeamColorButton = nullptr;
    QComboBox *m_bboxStyleCombo = nullptr;
    QPushButton *m_bboxColorButton = nullptr;
    QPushButton *m_pointsColorButton = nullptr;
    QPushButton *m_edgeColorButton = nullptr;
    QPushButton *m_wireColorButton = nullptr;
    QPushButton *m_fillColorButton = nullptr;
    QPushButton *m_fillPbrColorButton = nullptr;
    QDoubleSpinBox *m_currentMeshOutlineWidthSpin = nullptr;
    QDoubleSpinBox *m_currentMeshDilateRadiusSpin = nullptr;
    QDoubleSpinBox *m_currentMeshErodeRadiusSpin = nullptr;
    QComboBox *m_currentMeshDebugViewCombo = nullptr;
    QCheckBox *m_currentMeshHighlightCheck = nullptr;
    QCheckBox *m_showTrackballGizmoCheck = nullptr;
    QCheckBox *m_showAxisGizmoCheck = nullptr;
    QCheckBox *m_showViewCamerasCheck = nullptr;
    QCheckBox *m_bboxShowCornersCheck = nullptr;
    QCheckBox *m_bboxShowDimensionsCheck = nullptr;
    QCheckBox *m_decoratorVertexNormalsCheck = nullptr;
    QCheckBox *m_decoratorFaceNormalsCheck = nullptr;
    QCheckBox *m_decoratorCurvatureDirCheck = nullptr;
    QCheckBox *m_decoratorBoundaryEdgesCheck = nullptr;
    QCheckBox *m_decoratorTextureSeamsCheck = nullptr;
    QCheckBox *m_decoratorNonManifoldEdgesCheck = nullptr;
    QPushButton *m_decoratorNonManifoldEdgeColorButton = nullptr;
    QCheckBox *m_decoratorNonManifoldVerticesCheck = nullptr;
    QPushButton *m_decoratorNonManifoldVertexColorButton = nullptr;
    QCheckBox *m_decoratorInfoCheck = nullptr;
    QDoubleSpinBox *m_decoratorBoundaryWidthSpin = nullptr;
    QComboBox *m_qualityHistogramSourceCombo = nullptr;
    QComboBox *m_qualityHistogramColorMapCombo = nullptr;
    QCheckBox *m_qualityHistogramInvertCheck = nullptr;
    QCheckBox *m_qualityIsolinesCheck = nullptr;
    QDoubleSpinBox *m_qualityIsolineCountSpin = nullptr;
    QPushButton *m_qualityBakeVertexColorButton = nullptr;
    QCheckBox *m_qualityHistogramFixedRangeCheck = nullptr;
    QDoubleSpinBox *m_qualityHistogramBinsSpin = nullptr;
    QLabel *m_qualityHistogramCenterOnZeroLabel = nullptr;
    QCheckBox *m_qualityHistogramCenterOnZeroCheck = nullptr;
    QLabel *m_qualityHistogramPercentileCropLabel = nullptr;
    QDoubleSpinBox *m_qualityHistogramPercentileCropSpin = nullptr;
    QLabel *m_qualityHistogramMinLabel = nullptr;
    QDoubleSpinBox *m_qualityHistogramMinSpin = nullptr;
    QLabel *m_qualityHistogramMaxLabel = nullptr;
    QDoubleSpinBox *m_qualityHistogramMaxSpin = nullptr;
    QDoubleSpinBox *m_pointSizeSpin = nullptr;
    QDoubleSpinBox *m_edgeSizeSpin = nullptr;
    QDoubleSpinBox *m_wireSizeSpin = nullptr;
    QComboBox *m_pointColorSourceCombo = nullptr;
    QCheckBox *m_pointLightingCheck = nullptr;
    QCheckBox *m_wireLightingCheck = nullptr;
    QCheckBox *m_wireBackfaceCullingCheck = nullptr;
    QCheckBox *m_wireRespectFauxCheck = nullptr;
    QCheckBox *m_fillLightingCheck = nullptr;
    QCheckBox *m_fillBackfaceCullingCheck = nullptr;
    QDoubleSpinBox *m_fillNormalScaleSpin = nullptr;
    QDoubleSpinBox *m_fillOcclusionStrengthSpin = nullptr;
    QDoubleSpinBox *m_fillRoughnessFactorSpin = nullptr;
    QDoubleSpinBox *m_fillRsEnhancementSpin = nullptr;
    QComboBox *m_fillRsDisplayModeCombo = nullptr;
    QCheckBox *m_fillRsInvertCheck = nullptr;
    QCheckBox *m_fillRsFlatCheck = nullptr;
    QComboBox *m_fillMaterialCombo = nullptr;
    QComboBox *m_fillPbrAlbedoCombo = nullptr;
    QComboBox *m_fillPbrNormalCombo = nullptr;
    QComboBox *m_fillNormalSpaceCombo = nullptr;
    QComboBox *m_fillPbrOcclusionCombo = nullptr;
    QComboBox *m_fillPbrRoughnessCombo = nullptr;
    QCheckBox *m_selectionShowVerticesCheck = nullptr;
    QCheckBox *m_selectionShowFacesCheck = nullptr;
    QCheckBox *m_uvShowReferenceFrameCheck = nullptr;
    QCheckBox *m_uvShowFullTextureCheck = nullptr;
    QComboBox *m_uvTextureChannelCombo = nullptr;
    QCheckBox *m_uvTextureNearestCheck = nullptr;
    QCheckBox *m_fillTextureNearestCheck = nullptr;
    QPushButton *m_uvFillColorButton = nullptr;
    QComboBox *m_uvFillColorSourceCombo = nullptr;
    QComboBox *m_fillShadingCombo = nullptr;
    QComboBox *m_fillPbrShadingCombo = nullptr;
    QComboBox *m_fillPlainTextureCombo = nullptr;
    QFormLayout *m_fillPlainForm = nullptr;
    QComboBox *m_fillColorSourceCombo = nullptr;
    QStackedWidget *m_fillMaterialStack = nullptr;
    QToolButton *m_currentMeshButton = nullptr;
    QToolButton *m_normalsDecoratorsButton = nullptr;
    QToolButton *m_boundaryDecoratorsButton = nullptr;
    QToolButton *m_bboxButton = nullptr;
    QToolButton *m_pointsButton = nullptr;
    QToolButton *m_edgesButton = nullptr;
    QToolButton *m_wireButton = nullptr;
    QToolButton *m_fillButton = nullptr;
    QToolButton *m_selectionButton = nullptr;
    QToolButton *m_qualityHistogramButton = nullptr;
    QToolButton *m_currentMeshSettingsArrow = nullptr;
    QToolButton *m_normalsDecoratorsSettingsArrow = nullptr;
    QToolButton *m_boundaryDecoratorsSettingsArrow = nullptr;
    QToolButton *m_bboxSettingsArrow = nullptr;
    QToolButton *m_pointsSettingsArrow = nullptr;
    QToolButton *m_edgesSettingsArrow = nullptr;
    QToolButton *m_wireSettingsArrow = nullptr;
    QToolButton *m_fillSettingsArrow = nullptr;
    QToolButton *m_selectionSettingsArrow = nullptr;
    QToolButton *m_qualityHistogramSettingsArrow = nullptr;
    bool m_fillHasNormalMap = false;
    bool m_fillHasOcclusionMap = false;
    bool m_fillHasRoughnessMap = false;
    bool m_fillHasTextures = false;
    QStringList m_fillTextureNames;
};
