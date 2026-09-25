#pragma once

#include <QColor>
#include <QMetaType>
#include <QString>
#include <QVector3D>

enum class RenderPass {
    CurrentMesh = 0,
    BoundingBox,
    Points,
    Edges,
    Wireframe,
    Fill,
    Selection,
    DecoratorNormals,
    DecoratorBoundary,
    QualityHistogram,
    // Appended, not inserted: the pass is stored as an integer in every saved render
    // state, so renumbering the ones above it would silently reinterpret them.
    ClipPlane
};

enum class FillShading {
    Smooth = 0,
    Flat
};

enum class FillColorSource {
    Constant = 0,
    PerVertex,
    PerFace,
    PerVertexQuality,
    PerFaceQuality,
    Texture,
    PerMesh
};

enum class FillMaterial {
    Plain = 0,
    Pbr,
    RadianceScaling
};

enum class FillPbrTextureSource {
    None = 0,
    Constant,
    Texture
};

enum class FillPbrNormalMapSpace {
    Tangent = 0,
    Object
};

// Per-material parameter sub-structs (Suggestion C).
// These group together all settings specific to one fill material so that
// render code, sync code and equality checks stay local to each material.

struct PbrFillParams {
    FillShading          shading          = FillShading::Smooth;
    FillPbrTextureSource albedoSource    = FillPbrTextureSource::Texture;
    int                  albedoIndex     = -1;
    FillPbrTextureSource normalSource    = FillPbrTextureSource::Texture;
    int                  normalIndex     = -1;
    FillPbrNormalMapSpace normalMapSpace = FillPbrNormalMapSpace::Tangent;
    FillPbrTextureSource occlusionSource = FillPbrTextureSource::Texture;
    int                  occlusionIndex  = -1;
    FillPbrTextureSource roughnessSource = FillPbrTextureSource::Texture;
    int                  roughnessIndex  = -1;
    float                normalScale       = 1.0f;
    float                occlusionStrength = 1.0f;
    float                roughnessFactor   = 1.0f;

    // Defined in rendersettingsjson.cpp, generated from the same field list as the
    // JSON conversions so the two cannot drift apart.
    bool operator==(const PbrFillParams &o) const;
    bool operator!=(const PbrFillParams &o) const { return !(*this == o); }
};

struct PlainFillParams {
    FillShading     shading      = FillShading::Smooth;
    FillColorSource colorSource  = FillColorSource::Constant;
    int             textureIndex = -1;

    // Defined in rendersettingsjson.cpp, generated from the same field list as the
    // JSON conversions so the two cannot drift apart.
    bool operator==(const PlainFillParams &o) const;
    bool operator!=(const PlainFillParams &o) const { return !(*this == o); }
};

struct RsFillParams {
    FillShading shading       = FillShading::Smooth;
    float enhancement = 0.5f;
    int   displayMode = 0;   // 0=Lambertian, 1=Colored Descriptor, 2=Grey Descriptor
    bool  invert      = false;

    // Defined in rendersettingsjson.cpp, generated from the same field list as the
    // JSON conversions so the two cannot drift apart.
    bool operator==(const RsFillParams &o) const;
    bool operator!=(const RsFillParams &o) const { return !(*this == o); }
};

// Which way the clipping plane faces. `View` follows the camera, which is how the near
// plane behaved when it was the only way to cut into an object; the rest hold still while
// you orbit, which is the point of having a real plane.
enum class ClipPlaneAxis {
    X = 0,
    Y,
    Z,
    View,
    Custom
};

// What the clipping plane's offset is measured from. The same four references
// `create_polyline_from_planar_section` offers, so the view control and the filter describe
// a plane the same way.
enum class ClipPlaneReference {
    Origin = 0,
    Center,
    Min,
    Max
};

// How a layer's bounding box is drawn.
enum class BoundingBoxStyle {
    Box = 0,        // all twelve edges
    CornerBrackets  // three short arms at each corner, the rest of each edge left open
};

enum class PointColorSource {
    Constant = 0,
    PerVertex,
    PerVertexQuality
};

enum class EdgeColorSource {
    Constant = 0,
    PerVertex,
    PerEdge
};

enum class QualityHistogramSource {
    Auto = 0,
    VertexQuality,
    FaceQuality
};

// How the visible layers share the 3D view.
enum class LayerArrangement {
    // Every visible layer drawn in one shared space, which is what a 3D viewer normally
    // does and what you want whenever the layers belong together in world coordinates.
    Overlay = 0,
    // One tile per visible layer, all tiles showing the same camera from the same angle.
    // For comparing variants of one object -- decimations, parametrizations, repairs --
    // where overlaying them just produces one unreadable pile.
    Grid
};

enum class CurrentMeshDebugView {
    Outline = 0,
    FullMask,
    VisibleMask,
    OccludedMask,
    DilatedMask,
    ErodedMask
};

// Per-mesh rendering settings (one instance per mesh in the scene).
// Replaces the former private MeshRenderMode struct and is now a public,
// named type so that external code can manipulate per-mesh settings directly.
struct PerMeshRenderSettings {
    bool showBoundingBox = false;
    BoundingBoxStyle boundingBoxStyle = BoundingBoxStyle::Box;
    bool showPoints = false;
    bool showEdges = false;
    bool showWire = true;
    bool showFill = true;
    bool showSelection = true;
    bool showSelectionVertices = true;
    bool showSelectionFaces = true;
    // Covers both kinds of edge: the per-face edge bits of a triangle mesh, which
    // nothing else draws, and the selected VCGEdge elements of a polyline layer.
    bool showSelectionEdges = true;
    // Master switch for the normal decorator pass, driven by its toolbar button. As with
    // the boundary pass below, the button owns only this flag; the three sub-options are
    // the panel's own state.
    bool decoratorNormals = false;
    bool decoratorVertexNormals = true;
    bool decoratorFaceNormals = true;
    // Master switch for the boundary decorator pass, driven by its toolbar button. The
    // four flags below are the panel's own state and are never written by the button, so
    // toggling the pass off and on restores exactly what the user had selected.
    bool decoratorBoundary = false;
    bool decoratorBoundaryEdges = true;
    bool decoratorTextureSeams = true;
    bool decoratorNonManifoldEdges = false;
    bool decoratorNonManifoldVertices = false;
    // Unlike the two normal flags above this stays off by default: it needs per-vertex
    // principal-direction attributes, so it draws nothing until curvature is computed.
    bool decoratorCurvatureDir = false;
    bool pointLighting = false;
    bool wireLighting = false;
    bool wireBackfaceCulling = true;
    bool wireRespectFaux = true;
    bool fillLighting = true;
    bool fillBackfaceCulling = false;
    FillMaterial fillMaterial = FillMaterial::Plain;
    PbrFillParams fillPbr;
    RsFillParams  fillRs;
    PlainFillParams fillPlain;
    PointColorSource pointColorSource = PointColorSource::Constant;
    EdgeColorSource edgeColorSource = EdgeColorSource::Constant;
    QColor decoratorVertexNormalColor = QColor(70, 200, 255);
    QColor decoratorFaceNormalColor = QColor(70, 255, 120);
    QColor decoratorBoundaryEdgeColor = QColor(0, 255, 0);
    QColor decoratorTextureSeamColor = QColor(255, 80, 255);
    QColor decoratorNonManifoldEdgeColor = QColor(255, 50, 50);
    QColor decoratorNonManifoldVertexColor = QColor(255, 50, 255);
    QColor decoratorCurvatureDirPD1Color = QColor(50, 50, 220);   // max curvature direction
    QColor decoratorCurvatureDirPD2Color = QColor(220, 50, 50);   // min curvature direction
    float decoratorBoundaryWidth = 4.0f;
    QColor bboxWireColor = QColor(245, 190, 60);
    QColor pointColor = QColor(255, 191, 51);
    float pointSize = 4.0f;
    // The edges pass draws polyline layers, which usually sit against the dark end of the
    // background gradient; near-black made them all but invisible on arrival.
    QColor edgeColor = QColor(255, 255, 255);
    float edgeSize = 1.0f;
    QColor wireColor = QColor(15, 15, 20);
    float wireSize = 1.5f;
    QColor fillColor = QColor(230, 230, 230);

    // Defined in rendersettingsjson.cpp, generated from the same field list as the
    // JSON conversions so the two cannot drift apart.
    bool operator==(const PerMeshRenderSettings &o) const;
    bool operator!=(const PerMeshRenderSettings &o) const { return !(*this == o); }
};

Q_DECLARE_METATYPE(PerMeshRenderSettings)

// View-level (global) rendering settings shared across all meshes in the scene.
struct GlobalRenderSettings {
    LayerArrangement layerArrangement = LayerArrangement::Overlay;
    bool highlightCurrentMesh = true;
    bool showTrackballGizmo = true;
    // Separate from showTrackballGizmo on purpose: the orbit sphere and the corner
    // orientation gizmo are two different things, and wanting one without the other is the
    // common case -- the sphere sits over the mesh, the axis gizmo sits out of the way.
    bool showAxisGizmo = true;
    bool showViewCameras = true;
    bool showBoundingBoxCorners = false;
    bool showBoundingBoxDimensions = false;
    QColor currentMeshOutlineColor = QColor(42, 160, 240);
    float currentMeshOutlineWidth = 1.0f;
    float currentMeshDilateRadius = 2.5f;
    float currentMeshErodeRadius = 1.5f;
    CurrentMeshDebugView currentMeshDebugView = CurrentMeshDebugView::Outline;
    bool settingsPanelVisible = false;
    RenderPass currentPass = RenderPass::Fill;
    bool showQualityHistogram = false;
    // Shows an on-view panel with numeric counts for the enabled boundary/seam/
    // non-manifold decorators of the current mesh.
    bool showDecoratorInfo = false;
    bool uvShowReferenceFrame = true;
    bool uvShowFullTexture = false;
    int uvTextureChannel = 0;
    bool uvTextureNearestSampling = false;
    bool fillTextureNearestSampling = false;
    QColor sceneBackgroundTopColor = QColor(0, 0, 0);
    QColor sceneBackgroundBottomColor = QColor(128, 128, 255);
    int qualityHistogramBins = 32;
    QualityHistogramSource qualityHistogramSource = QualityHistogramSource::Auto;
    bool qualityHistogramFixedRange = false;
    bool qualityHistogramCenterOnZero = false;
    float qualityHistogramPercentileCrop = 0.01f;
    float qualityHistogramMin = 0.0f;
    float qualityHistogramMax = 1.0f;
    QString qualityHistogramColorMapId = QStringLiteral("rainbow");
    bool qualityHistogramInvertColorMap = false;
    bool qualityIsolinesEnabled = false;
    int qualityIsolineCount = 10;

    // The clipping plane. Everything on the negative side of it is cut away, in every
    // Scene3D pass including picking. Off by default; `view.nearClipRatio` is unaffected.
    bool clipPlaneEnabled = false;
    ClipPlaneAxis clipPlaneAxis = ClipPlaneAxis::View;
    QVector3D clipPlaneCustomAxis = QVector3D(0.0f, 1.0f, 0.0f);
    ClipPlaneReference clipPlaneRelativeTo = ClipPlaneReference::Center;
    // A fraction of the scene's bounding-box diagonal rather than a world distance, so the
    // slider covers a bunny and a building the same way and a saved view survives a rescale.
    float clipPlaneOffset = 0.0f;
    bool clipPlaneFlipped = false;
    // The plane's own grid is shown while it is being adjusted whatever this says; setting
    // it keeps the grid up afterwards too. Off by default, because once the cut is placed
    // the grid is in the way of the thing you cut open to look at.
    bool clipPlaneShowPlane = false;
    // Where the surface meets the plane, drawn in the fill shaders. Without it a cut
    // through a thin shell is just a hole, and it is hard to see where the plane is.
    QColor clipPlaneRimColor = QColor(255, 158, 51);
    // In pixels, so the rim reads the same at any zoom. Zero turns it off.
    float clipPlaneRimWidth = 2.0f;
    // Draw the cut as a solid face on the plane, found per pixel with a stencil winding
    // count rather than computed as geometry. The count is only exact for closed surfaces;
    // on an open one the cap is left out wherever a view ray leaves the object through a
    // hole, which is a missing face rather than a wrong one. Also decides whether the
    // viewport's Trim button closes the cut it makes.
    bool clipPlaneSolidCut = true;
    QColor clipPlaneSolidCutColor = QColor(200, 200, 205);

    // Defined in rendersettingsjson.cpp, generated from the same field list as the
    // JSON conversions so the two cannot drift apart.
    bool operator==(const GlobalRenderSettings &o) const;
    bool operator!=(const GlobalRenderSettings &o) const { return !(*this == o); }
};

// Backward-compatibility alias so callers that still use RenderSettings keep compiling.
using RenderSettings = GlobalRenderSettings;

Q_DECLARE_METATYPE(GlobalRenderSettings)
