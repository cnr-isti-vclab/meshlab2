#pragma once

#include <QMetaType>
#include <QVariant>
#include <QString>
#include <QStringList>
#include <QVector>
#include <vector>

class Document;

enum class MeshFilterInputDomain
{
    None,
    SingleMesh,
    WholeDocument
};
Q_DECLARE_METATYPE(MeshFilterInputDomain)

enum class MeshFilterOutputDomain
{
    Information,
    ModifyCurrentMesh,
    NewMeshes
};
Q_DECLARE_METATYPE(MeshFilterOutputDomain)

enum class MeshFilterParameterType
{
    Bool,
    Int,
    Double,
    AbsPerc,
    Mesh,
    String,
    FileOpen,
    FileSave,
    TextureRef,
    TextureOutputRef,
    Enum,
    Color,
    Point3f,    // 3D point or direction — editor shows X/Y/Z spinboxes with fill-from-context buttons
    CameraState,
    RenderState
};
Q_DECLARE_METATYPE(MeshFilterParameterType)

struct MeshFilterMeshRequirements
{
    bool requireVertices = false;
    bool requireEdges = false;
    bool requireFaces = false;
    bool requireVertexColor = false;
    bool requireFaceColor = false;
    bool requireTextureCoordinates = false;
    bool requirePerVertexTexCoords = false;
    bool requirePerWedgeTexCoords = false;
    bool requireTextures = false;
    bool requireVertexQuality = false;
    bool requireFaceQuality = false;

    bool hasAnyRequirement() const
    {
        return requireVertices
            || requireEdges
            || requireFaces
            || requireVertexColor
            || requireFaceColor
            || requireTextureCoordinates
            || requirePerVertexTexCoords
            || requirePerWedgeTexCoords
            || requireTextures
            || requireVertexQuality
            || requireFaceQuality;
    }
};

struct MeshFilterEnumOption
{
    QString id;
    QString label;
    QString helpMarkdown;
};

struct MeshFilterParameterDescriptor
{
    QString id;
    QString label;
    QString helpMarkdown;
    QString group;
    MeshFilterParameterType type = MeshFilterParameterType::String;
    QVariant defaultValue;
    QVariant minValue;
    QVariant maxValue;
    int decimals = 3;
    std::vector<MeshFilterEnumOption> enumOptions;
    // FileOpen/FileSave are raw external paths. Mesh-associated texture slots
    // should use TextureRef/TextureOutputRef instead.
    QString fileDialogTitle;
    QStringList fileNameFilters;
    QString fileDefaultSuffix;
    // TextureRef selects a mesh-associated input texture. TextureOutputRef selects
    // either an existing mesh-associated output texture or a new texture file.
    QString textureSourceMeshParameter;
    bool textureAllowAutomatic = true;
    // Only used when type == Mesh: requirements and volatile-data preparation
    // applied to the selected mesh parameter before runFilter().
    MeshFilterMeshRequirements meshRequirements;
    QStringList meshPrepare;
    // Only used when type == Point3f: "point" (position) or "direction" (unit vector).
    QString point3fRole = QStringLiteral("point");
    // Optional named preset the Point3f editor starts on, instead of the literal
    // defaultValue: "cameraEye", "trackballCenter", "bboxCenter", "viewDirection".
    // Ignored outside the UI, where defaultValue remains the fallback.
    QString point3fDefaultPreset;

    // Id of a bool parameter that gates this one: the editor is disabled while that
    // parameter is false. Prefix with '!' to invert. Empty means always enabled.
    // Lets the descriptor express what a lot of help text currently only says in
    // prose ("used only if 'custom axis' is chosen").
    QString enabledWhen;

    bool isAdvancedGroup() const
    {
        return group.startsWith(QStringLiteral("advanced"), Qt::CaseInsensitive);
    }
};

enum class MeshFilterCleanupKind
{
    RemoveUnreferencedVertices
};

struct MeshFilterCleanupAction
{
    MeshFilterCleanupKind kind = MeshFilterCleanupKind::RemoveUnreferencedVertices;
    // When non-empty, cleanup is applied only if this bool parameter is true.
    QString whenBoolParameter;
    // Empty means "current mesh". Otherwise names a Mesh parameter whose selected
    // mesh is the cleanup target.
    QString meshParameter;
};

struct MeshFilterProvenance
{
    QString project;
    QString repository;
    QString license;
    QString integration;

    bool isEmpty() const
    {
        return project.isEmpty() && repository.isEmpty() && license.isEmpty()
            && integration.isEmpty();
    }
};

struct MeshFilterReferenceAuthor
{
    QString family;
    QString given;
};

// CSL-JSON-compatible publication metadata shared by filter descriptors, the
// in-app help, and generated documentation.
struct MeshFilterReference
{
    QString id;
    QString type;
    QString title;
    std::vector<MeshFilterReferenceAuthor> authors;
    QString containerTitle;
    QString publisher;
    QString volume;
    QString issue;
    QString page;
    int year = 0;
    QString doi;
    QString url;
    // A book has no DOI; the ISBN is its identifier. Edition matters for books whose
    // content differs between printings.
    QString isbn;
    QString edition;

    QString label() const;
    QString doiUrl() const;
    QString webUrl() const;
    QString markdownCitation() const;
    // Same bibliography as markdownCitation(), rendered as HTML with live links.
    QString htmlCitation() const;
    QString bibTeX() const;
};

struct MeshFilterDescriptor
{
    QString id;
    // Ordered set of ontology categories; the first is primary (canonical home),
    // the rest are cross-listings. Validated against FilterCategories.
    // See docs/design/vocabulary.md §1.
    QStringList categories;
    QString name;
    QString pythonName;          // explicit Python method name; auto-computed from name if empty
    QString shortDescription;
    QString longDescriptionMarkdown;
    MeshFilterProvenance provenance;
    std::vector<MeshFilterReference> references;
    QStringList tags;
    MeshFilterInputDomain inputDomain = MeshFilterInputDomain::SingleMesh;
    MeshFilterMeshRequirements inputRequirements;
    MeshFilterOutputDomain outputDomain = MeshFilterOutputDomain::Information;
    // When true the framework automatically appends an "incremental_selection" bool
    // parameter and, when enabled, ORs the pre-run selection back after the filter.
    bool incrementalSelection = false;
    std::vector<MeshFilterParameterDescriptor> parameters;

    // The canonical home of this filter: first category, or empty if unclassified.
    // Use for single-placement contexts (sorting, one-line labels). Contexts that
    // should reflect every category — the Filters menu, search — iterate `categories`.
    QString primaryCategory() const
    {
        return categories.isEmpty() ? QString() : categories.first();
    }
    // Distinct top-level roots this filter belongs to, order preserved.
    QStringList categoryRoots() const
    {
        QStringList out;
        for (const QString &c : categories) {
            const QString root = c.section(QLatin1Char('/'), 0, 0).trimmed();
            if (!root.isEmpty() && !out.contains(root))
                out << root;
        }
        return out;
    }

    // Two-letter codes describing which mesh attributes this filter modifies.
    // Recognised codes: VG VN VC VQ VT VA VS FV FN FC FQ FA FS FP WT TX TM
    // Used only when outputDomain == ModifyCurrentMesh.
    // VG=vertex geometry  VN=vertex normals  VC=vertex color  VQ=vertex quality
    // VT=vertex texcoords VA=vertex attributes VS=vertex selection
    // FV=face-vertex connectivity  FN=face normals  FC=face color  FQ=face quality
    // FA=face attributes  FS=face selection  FP=face polygon (faux-edge) bits
    // WT=wedge texcoords  TX=texture images  TM=per-mesh transform matrix
    QStringList outputModifies;

    // Codes declaring volatile mesh data the framework must prepare before
    // calling runFilter().  The framework executes the corresponding VCG
    // algorithms automatically, so filter code need not repeat them.
    //
    // Recognised codes (applied in dependency order):
    //   FF        — Enable face-face adjacency + UpdateTopology::FaceFace
    //   VF        — Enable vertex-face adjacency + UpdateTopology::VertexFace
    //   BorderFF  — FF + FaceBorderFromFF + VertexBorderFromFaceBorder
    //   BorderVF  — VF + FaceBorderFromVF + VertexBorderFromFaceBorder
    //   FNorm     — UpdateNormal::PerFaceNormalized
    //   VNorm     — UpdateNormal::PerVertexNormalizedPerFaceNormalized
    //   BBox      — UpdateBounding::Box
    //   FMark     — Enable per-face OCF mark
    //   VMark     — Enable per-vertex OCF mark
    //   Mark      — Backward-compatible alias for FMark
    //   VTex/VT   — Enable per-vertex OCF texture coordinates
    //   WTex/WT   — Enable per-wedge OCF texture coordinates
    //   CurvDir   — Enable per-vertex OCF curvature directions
    QStringList inputPrepare;

    // Declarative semantic cleanup hooks executed by the framework.
    //
    // Recognised kinds:
    //   RemoveUnreferencedVertices
    //
    // preRunCleanup runs after parameter normalization/validation and before
    // inputPrepare, so filters can rely on a cleaned mesh when building
    // adjacency or other volatile structures.
    //
    // postRunCleanup runs after the filter returns successfully and before the
    // framework compaction pass, so filters can delegate common cleanup without
    // duplicating bookkeeping code.
    QVector<MeshFilterCleanupAction> preRunCleanup;
    QVector<MeshFilterCleanupAction> postRunCleanup;

    // Returns pythonName if explicitly set, otherwise auto-computes a
    // snake_case identifier from the display name.
    QString effectivePythonName() const
    {
        if (!pythonName.isEmpty())
            return pythonName;
        QString result;
        result.reserve(name.size());
        for (const QChar &c : name) {
            if (c.isLetterOrNumber()) {
                result += c.toLower();
            } else if (c == u' ' || c == u'/' || c == u'-') {
                if (!result.isEmpty() && result.back() != u'_')
                    result += u'_';
            }
        }
        while (!result.isEmpty() && result.back() == u'_')
            result.chop(1);
        return result;
    }
};

using MeshFilterParameterValues = QVariantMap;

// FilterParams is a typed wrapper around MeshFilterParameterValues.
// Declared in filterparam.h; forward-declared here to avoid circular inclusion.
class FilterParams;

enum class MeshFilterVisualizationAttribute
{
    VertexQuality,
    FaceQuality,
    Texture,
    // Per-edge colour on a polyline layer. Needed because the automatic choice in
    // defaultRenderModeForMesh only runs for newly created layers -- a filter that
    // colours the edges of an existing one would otherwise change nothing on screen.
    EdgeColor
};

struct MeshFilterVisualizationHint
{
    int meshIndex = -1;
    MeshFilterVisualizationAttribute attribute = MeshFilterVisualizationAttribute::VertexQuality;
};

struct MeshFilterRunResult
{
    bool success = false;
    bool documentModified = false;
    QString errorMessage;
    QStringList infoMessages;
    QVector<int> newMeshIndices;
    QVector<MeshFilterVisualizationHint> visualizationHints;
    QVariantMap outputValues;
};

class MeshFilterPlugin
{
public:
    virtual ~MeshFilterPlugin() = default;

    virtual QString pluginId() const = 0;
    virtual QString name() const = 0;

    // Returns all filter descriptors exposed by this plugin.
    // The default implementation loads from the Qt resource path
    //   :/filters/<pluginId>/filters.json
    // and resolves symbolic bound tokens against doc.
    // Override for plugins that need completely custom descriptor logic.
    virtual std::vector<MeshFilterDescriptor> filters(const Document &doc) const;

    // Runs one filter declared by filters().
    // filterId is the plugin-local id (MeshFilterDescriptor::id).
    // params contains pre-normalized, validated values for all declared parameters.
    //
    // Framework invariant:
    // - imported meshes and meshes added through Document::addMesh() are compact
    // - after any successful non-information filter, the framework compacts the
    //   original current mesh (for SingleMesh filters), every mesh reported in
    //   MeshFilterRunResult::newMeshIndices, and all document meshes for
    //   WholeDocument filters
    // Therefore filter code should not leave deleted elements behind or rely on
    // them surviving past runFilter() return.
    virtual MeshFilterRunResult runFilter(
        const QString &filterId,
        const FilterParams &params,
        Document &doc) const = 0;
};
