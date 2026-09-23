#include "rendersettingsjson.h"

#include <QObject>

#include <type_traits>

namespace RenderSettingsJson {

bool fuzzyFloatEqual(float a, float b, float eps)
{
    return std::abs(a - b) <= eps;
}

bool parseFloatValue(const QJsonValue &value, float &outValue)
{
    if (!value.isDouble())
        return false;
    outValue = float(value.toDouble());
    return std::isfinite(outValue);
}

QJsonArray colorToJsonArray(const QColor &c)
{
    return QJsonArray{c.red(), c.green(), c.blue(), c.alpha()};
}

bool parseColorArray(const QJsonValue &value, QColor &outValue)
{
    if (!value.isArray())
        return false;
    const QJsonArray arr = value.toArray();
    if (arr.size() != 4)
        return false;
    int rgba[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (!arr[i].isDouble())
            return false;
        const int v = int(std::lround(arr[i].toDouble()));
        if (v < 0 || v > 255)
            return false;
        rgba[i] = v;
    }
    outValue = QColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    return true;
}

namespace {

// ---------------------------------------------------------------------------
// Per-type conversion. One overload set per direction, selected by the member's
// own type, so a field list needs to name only the key and the member: adding a
// field of an already-supported type needs nothing here.
// ---------------------------------------------------------------------------

QJsonValue valueToJson(bool v) { return v; }
QJsonValue valueToJson(int v) { return v; }
QJsonValue valueToJson(float v) { return double(v); }
QJsonValue valueToJson(const QString &v) { return v; }
QJsonValue valueToJson(const QColor &v) { return colorToJsonArray(v); }
QJsonValue valueToJson(const QVector3D &v)
{
    return QJsonArray{double(v.x()), double(v.y()), double(v.z())};
}

template <typename E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
QJsonValue valueToJson(E v)
{
    return static_cast<int>(v);
}

bool valueFromJson(const QJsonValue &j, bool &v)
{
    if (!j.isBool())
        return false;
    v = j.toBool();
    return true;
}

bool valueFromJson(const QJsonValue &j, int &v)
{
    if (!j.isDouble())
        return false;
    v = j.toInt();
    return true;
}

bool valueFromJson(const QJsonValue &j, float &v)
{
    return parseFloatValue(j, v);
}

bool valueFromJson(const QJsonValue &j, QString &v)
{
    if (!j.isString())
        return false;
    v = j.toString();
    return true;
}

bool valueFromJson(const QJsonValue &j, QColor &v)
{
    return parseColorArray(j, v);
}

bool valueFromJson(const QJsonValue &j, QVector3D &v)
{
    if (!j.isArray())
        return false;
    const QJsonArray arr = j.toArray();
    if (arr.size() != 3)
        return false;
    float xyz[3] = {};
    for (int i = 0; i < 3; ++i) {
        if (!parseFloatValue(arr[i], xyz[i]))
            return false;
    }
    v = QVector3D(xyz[0], xyz[1], xyz[2]);
    return true;
}

template <typename E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
bool valueFromJson(const QJsonValue &j, E &v)
{
    if (!j.isDouble())
        return false;
    v = static_cast<E>(j.toInt());
    return true;
}

// Floats compare with a tolerance; everything else exactly. This mirrors what the
// hand-written writer did when deciding whether a value differed from the default.
bool valuesEqual(float a, float b) { return fuzzyFloatEqual(a, b); }

template <typename T>
bool valuesEqual(const T &a, const T &b)
{
    return a == b;
}

// ---------------------------------------------------------------------------
// Field application. The member pointer carries the type, so these three are all
// a field list ever needs to expand into.
// ---------------------------------------------------------------------------

// A null `defaults` writes every field; otherwise only the ones that differ, which
// is what keeps a stored render state small.
template <typename S, typename T>
void writeField(QJsonObject &o, const char *key, const S &s, const S *defaults, T S::*member)
{
    if (!defaults || !valuesEqual(s.*member, defaults->*member))
        o.insert(QLatin1String(key), valueToJson(s.*member));
}

// A key that is absent leaves the member at its current value; a key of the wrong
// type is an error, named individually rather than as "one or more fields".
template <typename S, typename T>
bool readField(const QJsonObject &o, const char *key, S &out, T S::*member, QString *error)
{
    const QLatin1String k(key);
    if (!o.contains(k))
        return true;
    if (valueFromJson(o.value(k), out.*member))
        return true;
    if (error) {
        *error = QObject::tr("Invalid render-state JSON: '%1' has an invalid type.")
                     .arg(QString::fromLatin1(key));
    }
    return false;
}

template <typename S, typename T>
bool fieldsEqual(const S &a, const S &b, T S::*member)
{
    return valuesEqual(a.*member, b.*member);
}

// Expand a field list into each of the three operations. Declared as macros because
// the list itself is a macro; each takes the same F(key, member) shape.
#define MESHLAB2_WRITE_FIELD(key, member) \
    writeField(o, key, s, defaults, &SettingsType::member);
#define MESHLAB2_READ_FIELD(key, member) \
    if (!readField(obj, key, out, &SettingsType::member, error)) \
        return false;
#define MESHLAB2_EQUAL_FIELD(key, member) \
    if (!fieldsEqual(*this, o, &SettingsType::member)) \
        return false;

// 41 fields
#define MESHLAB2_GLOBAL_SETTINGS_FIELDS(F) \
    F("layer_arrangement", layerArrangement) \
    F("highlight_current_mesh", highlightCurrentMesh) \
    F("show_trackball_gizmo", showTrackballGizmo) \
    F("show_axis_gizmo", showAxisGizmo) \
    F("show_view_cameras", showViewCameras) \
    F("show_decorator_info", showDecoratorInfo) \
    F("fill_texture_nearest_sampling", fillTextureNearestSampling) \
    F("show_bounding_box_corners", showBoundingBoxCorners) \
    F("show_bounding_box_dimensions", showBoundingBoxDimensions) \
    F("current_mesh_outline_color", currentMeshOutlineColor) \
    F("current_mesh_outline_width", currentMeshOutlineWidth) \
    F("current_mesh_dilate_radius", currentMeshDilateRadius) \
    F("current_mesh_erode_radius", currentMeshErodeRadius) \
    F("current_mesh_debug_view", currentMeshDebugView) \
    F("settings_panel_visible", settingsPanelVisible) \
    F("current_pass", currentPass) \
    F("show_quality_histogram", showQualityHistogram) \
    F("uv_show_reference_frame", uvShowReferenceFrame) \
    F("uv_show_full_texture", uvShowFullTexture) \
    F("uv_texture_channel", uvTextureChannel) \
    F("uv_texture_nearest_sampling", uvTextureNearestSampling) \
    F("scene_background_top_color", sceneBackgroundTopColor) \
    F("scene_background_bottom_color", sceneBackgroundBottomColor) \
    F("quality_histogram_bins", qualityHistogramBins) \
    F("quality_histogram_source", qualityHistogramSource) \
    F("quality_histogram_fixed_range", qualityHistogramFixedRange) \
    F("quality_histogram_center_on_zero", qualityHistogramCenterOnZero) \
    F("quality_histogram_percentile_crop", qualityHistogramPercentileCrop) \
    F("quality_histogram_min", qualityHistogramMin) \
    F("quality_histogram_max", qualityHistogramMax) \
    F("quality_histogram_colormap_id", qualityHistogramColorMapId) \
    F("quality_histogram_invert_colormap", qualityHistogramInvertColorMap) \
    F("quality_isolines_enabled", qualityIsolinesEnabled) \
    F("quality_isoline_count", qualityIsolineCount) \
    F("clip_plane_enabled", clipPlaneEnabled) \
    F("clip_plane_axis", clipPlaneAxis) \
    F("clip_plane_custom_axis", clipPlaneCustomAxis) \
    F("clip_plane_relative_to", clipPlaneRelativeTo) \
    F("clip_plane_offset", clipPlaneOffset) \
    F("clip_plane_flipped", clipPlaneFlipped) \
    F("clip_plane_show_plane", clipPlaneShowPlane)

// 45 flat fields; the three fill_* sub-objects are listed separately
#define MESHLAB2_PER_MESH_SETTINGS_FIELDS(F) \
    F("show_bounding_box", showBoundingBox) \
    F("bounding_box_style", boundingBoxStyle) \
    F("show_points", showPoints) \
    F("show_edges", showEdges) \
    F("show_wire", showWire) \
    F("show_fill", showFill) \
    F("show_selection", showSelection) \
    F("show_selection_vertices", showSelectionVertices) \
    F("show_selection_edges", showSelectionEdges) \
    F("show_selection_faces", showSelectionFaces) \
    F("decorator_normals", decoratorNormals) \
    F("decorator_vertex_normals", decoratorVertexNormals) \
    F("decorator_face_normals", decoratorFaceNormals) \
    F("decorator_boundary", decoratorBoundary) \
    F("decorator_boundary_edges", decoratorBoundaryEdges) \
    F("decorator_texture_seams", decoratorTextureSeams) \
    F("decorator_non_manifold_edges", decoratorNonManifoldEdges) \
    F("decorator_non_manifold_vertices", decoratorNonManifoldVertices) \
    F("decorator_curvature_dir", decoratorCurvatureDir) \
    F("point_lighting", pointLighting) \
    F("wire_lighting", wireLighting) \
    F("wire_backface_culling", wireBackfaceCulling) \
    F("wire_respect_faux", wireRespectFaux) \
    F("fill_lighting", fillLighting) \
    F("fill_backface_culling", fillBackfaceCulling) \
    F("fill_material", fillMaterial) \
    F("point_color_source", pointColorSource) \
    F("edge_color_source", edgeColorSource) \
    F("decorator_vertex_normal_color", decoratorVertexNormalColor) \
    F("decorator_face_normal_color", decoratorFaceNormalColor) \
    F("decorator_boundary_edge_color", decoratorBoundaryEdgeColor) \
    F("decorator_texture_seam_color", decoratorTextureSeamColor) \
    F("decorator_non_manifold_edge_color", decoratorNonManifoldEdgeColor) \
    F("decorator_non_manifold_vertex_color", decoratorNonManifoldVertexColor) \
    F("decorator_curvature_dir_pd1_color", decoratorCurvatureDirPD1Color) \
    F("decorator_curvature_dir_pd2_color", decoratorCurvatureDirPD2Color) \
    F("decorator_boundary_width", decoratorBoundaryWidth) \
    F("bbox_wire_color", bboxWireColor) \
    F("point_color", pointColor) \
    F("point_size", pointSize) \
    F("edge_color", edgeColor) \
    F("edge_size", edgeSize) \
    F("wire_color", wireColor) \
    F("wire_size", wireSize) \
    F("fill_color", fillColor)

// PlainFillParams
#define MESHLAB2_FILL_PLAIN_FIELDS(F) \
    F("shading", shading) \
    F("color_source", colorSource) \
    F("texture_index", textureIndex)

// PbrFillParams
#define MESHLAB2_FILL_PBR_FIELDS(F) \
    F("shading", shading) \
    F("albedo_source", albedoSource) \
    F("albedo_index", albedoIndex) \
    F("normal_source", normalSource) \
    F("normal_index", normalIndex) \
    F("normal_map_space", normalMapSpace) \
    F("occlusion_source", occlusionSource) \
    F("occlusion_index", occlusionIndex) \
    F("roughness_source", roughnessSource) \
    F("roughness_index", roughnessIndex) \
    F("normal_scale", normalScale) \
    F("occlusion_strength", occlusionStrength) \
    F("roughness_factor", roughnessFactor)

// RsFillParams
#define MESHLAB2_FILL_RS_FIELDS(F) \
    F("shading", shading) \
    F("enhancement", enhancement) \
    F("display_mode", displayMode) \
    F("invert", invert)

// Each nested fill material is the same pattern one level down.
#define MESHLAB2_DEFINE_NESTED(Type, FieldList, jsonName) \
    QJsonObject Type##ToJson(const Type &s, const Type *defaults) \
    { \
        using SettingsType = Type; \
        QJsonObject o; \
        FieldList(MESHLAB2_WRITE_FIELD) \
        return o; \
    } \
    bool Type##FromJson(const QJsonObject &obj, Type &out, QString *error) \
    { \
        using SettingsType = Type; \
        FieldList(MESHLAB2_READ_FIELD) \
        return true; \
    }

MESHLAB2_DEFINE_NESTED(PlainFillParams, MESHLAB2_FILL_PLAIN_FIELDS, "fill_plain")
MESHLAB2_DEFINE_NESTED(PbrFillParams, MESHLAB2_FILL_PBR_FIELDS, "fill_pbr")
MESHLAB2_DEFINE_NESTED(RsFillParams, MESHLAB2_FILL_RS_FIELDS, "fill_rs")

#undef MESHLAB2_DEFINE_NESTED

// Read a nested sub-object: absent leaves the member alone, present-but-not-an-object
// is an error, and the contents go through the nested reader.
template <typename Nested, typename Reader>
bool readNested(
    const QJsonObject &obj,
    const char *key,
    Nested &out,
    Reader reader,
    QString *error)
{
    const QLatin1String k(key);
    if (!obj.contains(k))
        return true;
    const QJsonValue value = obj.value(k);
    if (!value.isObject()) {
        if (error) {
            *error = QObject::tr("Invalid render-state JSON: '%1' must be an object.")
                         .arg(QString::fromLatin1(key));
        }
        return false;
    }
    return reader(value.toObject(), out, error);
}

} // namespace

// ---------------------------------------------------------------------------
// The four public conversions, each the field list expanded once.
// ---------------------------------------------------------------------------

QJsonObject globalSettingsToJson(
    const GlobalRenderSettings &s,
    const GlobalRenderSettings *defaults)
{
    using SettingsType = GlobalRenderSettings;
    QJsonObject o;
    MESHLAB2_GLOBAL_SETTINGS_FIELDS(MESHLAB2_WRITE_FIELD)
    return o;
}

bool parseGlobalSettings(const QJsonObject &obj, GlobalRenderSettings &out, QString *error)
{
    using SettingsType = GlobalRenderSettings;
    MESHLAB2_GLOBAL_SETTINGS_FIELDS(MESHLAB2_READ_FIELD)
    return true;
}

QJsonObject perMeshSettingsToJson(
    const PerMeshRenderSettings &s,
    const PerMeshRenderSettings *defaults)
{
    QJsonObject o;
    {
        using SettingsType = PerMeshRenderSettings;
        MESHLAB2_PER_MESH_SETTINGS_FIELDS(MESHLAB2_WRITE_FIELD)
    }

    // An empty sub-object means every nested field matched the default, so it is
    // omitted rather than written as {}.
    auto insertNested = [&](const char *key, const QJsonObject &sub) {
        if (!defaults || !sub.isEmpty())
            o.insert(QLatin1String(key), sub);
    };
    insertNested(
        "fill_plain",
        PlainFillParamsToJson(s.fillPlain, defaults ? &defaults->fillPlain : nullptr));
    insertNested(
        "fill_pbr",
        PbrFillParamsToJson(s.fillPbr, defaults ? &defaults->fillPbr : nullptr));
    insertNested(
        "fill_rs",
        RsFillParamsToJson(s.fillRs, defaults ? &defaults->fillRs : nullptr));
    return o;
}

bool parsePerMeshSettings(const QJsonObject &obj, PerMeshRenderSettings &out, QString *error)
{
    {
        using SettingsType = PerMeshRenderSettings;
        MESHLAB2_PER_MESH_SETTINGS_FIELDS(MESHLAB2_READ_FIELD)
    }
    return readNested(obj, "fill_plain", out.fillPlain, PlainFillParamsFromJson, error)
        && readNested(obj, "fill_pbr", out.fillPbr, PbrFillParamsFromJson, error)
        && readNested(obj, "fill_rs", out.fillRs, RsFillParamsFromJson, error);
}

} // namespace RenderSettingsJson

// ---------------------------------------------------------------------------
// Equality, from the same field lists. Out-of-line rather than inline in the header
// so that the lists stay in one place; these run once per settings change, so the
// call is irrelevant.
// ---------------------------------------------------------------------------

#define MESHLAB2_DEFINE_EQUALITY(Type, FieldList) \
    bool Type::operator==(const Type &o) const \
    { \
        using namespace RenderSettingsJson; \
        using SettingsType = Type; \
        FieldList(MESHLAB2_EQUAL_FIELD) \
        return true; \
    }

MESHLAB2_DEFINE_EQUALITY(PlainFillParams, MESHLAB2_FILL_PLAIN_FIELDS)
MESHLAB2_DEFINE_EQUALITY(PbrFillParams, MESHLAB2_FILL_PBR_FIELDS)
MESHLAB2_DEFINE_EQUALITY(RsFillParams, MESHLAB2_FILL_RS_FIELDS)

bool PerMeshRenderSettings::operator==(const PerMeshRenderSettings &o) const
{
    using namespace RenderSettingsJson;
    using SettingsType = PerMeshRenderSettings;
    MESHLAB2_PER_MESH_SETTINGS_FIELDS(MESHLAB2_EQUAL_FIELD)
    return fillPbr == o.fillPbr && fillRs == o.fillRs && fillPlain == o.fillPlain;
}

bool GlobalRenderSettings::operator==(const GlobalRenderSettings &o) const
{
    using namespace RenderSettingsJson;
    using SettingsType = GlobalRenderSettings;
    MESHLAB2_GLOBAL_SETTINGS_FIELDS(MESHLAB2_EQUAL_FIELD)
    return true;
}
