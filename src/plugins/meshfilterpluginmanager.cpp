#include "meshfilterpluginmanager.h"

#include "document.h"
#include "filterparam.h"
#include "vcgmesh.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <QColor>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QVector3D>
#include <algorithm>
#include <cmath>

namespace {
constexpr QLatin1StringView kKeySeparator("::");

bool validateStateJsonPayload(
    const QString &value,
    const QString &requiredKind,
    const QString &prefix,
    QString &errorMessage)
{
    if (value.trimmed().isEmpty()) {
        errorMessage = QObject::tr("%1 cannot be empty.").arg(prefix);
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(value.toUtf8(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        errorMessage = QObject::tr("%1 must be a valid JSON object (%2).")
                           .arg(prefix)
                           .arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QString kind = root.value(QStringLiteral("kind")).toString().trimmed();
    // State captured before the 2026-09 rename carries the "MeshLab." prefix.
    const QString legacyKind =
        QStringLiteral("Q") + requiredKind;
    if (kind != requiredKind && kind != legacyKind) {
        errorMessage = QObject::tr("%1 has invalid kind '%2' (expected '%3').")
                           .arg(prefix)
                           .arg(kind)
                           .arg(requiredKind);
        return false;
    }
    return true;
}

// Holds the OCF enable/disable state for one filter invocation.
// Constructed by prepareMeshForFilter(); must stay alive while the filter runs.
// The destructor disables whatever components were enabled, releasing memory.
struct MeshPreparationScope {
    VCGMesh *mesh = nullptr;
    bool disableFF   = false;
    bool disableVF   = false;
    bool disableMark = false;
    bool disableVertexMark = false;

    MeshPreparationScope() = default;
    // Movable so it can be returned by value and held in runFilter.
    MeshPreparationScope(MeshPreparationScope &&o) noexcept
        : mesh(o.mesh)
        , disableFF(o.disableFF)
        , disableVF(o.disableVF)
        , disableMark(o.disableMark)
        , disableVertexMark(o.disableVertexMark)
    {
        o.mesh = nullptr; // transfer ownership
    }
    MeshPreparationScope &operator=(MeshPreparationScope &&) = delete;
    MeshPreparationScope(const MeshPreparationScope &) = delete;
    MeshPreparationScope &operator=(const MeshPreparationScope &) = delete;

    ~MeshPreparationScope()
    {
        if (!mesh) return;
        if (disableFF)   mesh->face.DisableFFAdjacency();
        if (disableVF) {
            mesh->vert.DisableVFAdjacency();
            mesh->face.DisableVFAdjacency();
        }
        if (disableMark) mesh->face.DisableMark();
        if (disableVertexMark) mesh->vert.DisableMark();
    }
};

struct MultiMeshPreparationScope {
    std::vector<MeshPreparationScope> scopes;
};

struct CleanupApplicationResult {
    bool modified = false;
    QStringList infoMessages;
    QSet<int> modifiedMeshIndices;
};

bool meshHasAnyTextureAssociation(const Document::MeshEntry &entry)
{
    return Document::hasMeshTextureAssociation(entry);
}

int cleanupTargetMeshIndex(
    const MeshFilterCleanupAction &action,
    const FilterParams &params,
    const Document &doc,
    int fallbackCurrentMeshIndex)
{
    if (action.meshParameter.trimmed().isEmpty())
        return fallbackCurrentMeshIndex;
    return params.getMesh(action.meshParameter, fallbackCurrentMeshIndex);
}

bool meshNeedsCompaction(const VCGMesh &mesh)
{
    return mesh.VN() != int(mesh.vert.size())
        || mesh.EN() != int(mesh.edge.size())
        || mesh.FN() != int(mesh.face.size());
}

void compactMeshStorageInvariant(VCGMesh &mesh)
{
    if (!meshNeedsCompaction(mesh))
        return;

    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
    if (mesh.VN() > 0)
        vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    else
        mesh.bbox.SetNull();
}

void finalizeMeshesAfterSuccessfulFilter(
    const MeshFilterDescriptor &descriptor,
    const MeshFilterRunResult &result,
    Document &doc,
    int originalCurrentMeshIndex)
{
    if (!result.success || !result.documentModified || descriptor.outputDomain == MeshFilterOutputDomain::Information)
        return;

    QSet<int> targetMeshes;
    if (descriptor.inputDomain == MeshFilterInputDomain::SingleMesh
        && originalCurrentMeshIndex >= 0
        && originalCurrentMeshIndex < doc.meshCount()) {
        targetMeshes.insert(originalCurrentMeshIndex);
    }

    if (descriptor.inputDomain == MeshFilterInputDomain::WholeDocument) {
        for (int i = 0; i < doc.meshCount(); ++i)
            targetMeshes.insert(i);
    }

    for (int meshIndex : result.newMeshIndices) {
        if (meshIndex >= 0 && meshIndex < doc.meshCount())
            targetMeshes.insert(meshIndex);
    }

    const bool polygonStructureModified =
        descriptor.outputModifies.contains(QStringLiteral("FV"))
        || descriptor.outputModifies.contains(QStringLiteral("FP"));
    const bool fauxEdgesModified = descriptor.outputModifies.contains(QStringLiteral("FP"));

    for (int meshIndex : std::as_const(targetMeshes)) {
        compactMeshStorageInvariant(doc.mesh(meshIndex).mesh);
        // Avoid recounting a newly added polygon mesh: addMesh() initialized it.
        const bool cacheAlreadyInitialized =
            result.newMeshIndices.contains(meshIndex)
            && doc.mesh(meshIndex).polygonFaceCount >= 0;
        if (polygonStructureModified && !cacheAlreadyInitialized)
            doc.refreshMeshPolygonFaceCount(meshIndex, fauxEdgesModified);
    }
}

CleanupApplicationResult applyCleanupActions(
    const QVector<MeshFilterCleanupAction> &actions,
    const FilterParams &params,
    Document &doc,
    int fallbackCurrentMeshIndex,
    bool compactImmediately)
{
    CleanupApplicationResult result;

    for (const MeshFilterCleanupAction &action : actions) {
        if (!action.whenBoolParameter.trimmed().isEmpty()
            && !params.getBool(action.whenBoolParameter)) {
            continue;
        }

        const int meshIndex = cleanupTargetMeshIndex(action, params, doc, fallbackCurrentMeshIndex);
        if (meshIndex < 0 || meshIndex >= doc.meshCount())
            continue;

        VCGMesh &mesh = doc.mesh(meshIndex).mesh;
        switch (action.kind) {
        case MeshFilterCleanupKind::RemoveUnreferencedVertices: {
            const int removedVertices = vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(mesh);
            if (removedVertices <= 0)
                break;

            if (compactImmediately)
                vcg::tri::Allocator<VCGMesh>::CompactVertexVector(mesh);

            result.modified = true;
            result.modifiedMeshIndices.insert(meshIndex);
            result.infoMessages.push_back(
                QObject::tr("Framework cleanup removed %1 unreferenced vertices%2.")
                    .arg(removedVertices)
                    .arg(action.meshParameter.trimmed().isEmpty()
                             ? QString()
                             : QObject::tr(" on mesh '%1'").arg(doc.mesh(meshIndex).name)));
            break;
        }
        }
    }

    return result;
}

QString meshSubjectLabel(const QString &rawLabel)
{
    const QString label = rawLabel.trimmed();
    return label.isEmpty() ? QObject::tr("Selected Mesh") : label;
}

bool validateMeshRequirements(
    const QString &filterName,
    const QString &subjectLabel,
    const Document::MeshEntry &meshEntry,
    const MeshFilterMeshRequirements &req,
    QString &errorMessage)
{
    using Mask = vcg::tri::io::Mask;

    auto fail = [&](const QString &message) {
        errorMessage = message;
        return false;
    };

    const QString subject = meshSubjectLabel(subjectLabel);

    if (req.requireVertices && meshEntry.mesh.VN() <= 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have vertices.").arg(filterName, subject));
    if (req.requireEdges && meshEntry.mesh.EN() <= 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have edges.").arg(filterName, subject));
    if (req.requireFaces && meshEntry.mesh.FN() <= 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have faces.").arg(filterName, subject));
    if (req.requireVertexColor && (meshEntry.ioMask & Mask::IOM_VERTCOLOR) == 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have vertex color.").arg(filterName, subject));
    if (req.requireFaceColor && (meshEntry.ioMask & Mask::IOM_FACECOLOR) == 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have face color.").arg(filterName, subject));
    if (req.requireEdgeQuality && (meshEntry.ioMask & Mask::IOM_EDGEQUALITY) == 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have an edge scalar.").arg(filterName, subject));
    if (req.requirePerVertexTexCoords && (meshEntry.ioMask & Mask::IOM_VERTTEXCOORD) == 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have per-vertex texture coordinates.").arg(filterName, subject));
    if (req.requirePerWedgeTexCoords && (meshEntry.ioMask & Mask::IOM_WEDGTEXCOORD) == 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have per-wedge texture coordinates.").arg(filterName, subject));
    if (req.requireTextureCoordinates) {
        const bool hasTexCoords =
            (meshEntry.ioMask & (Mask::IOM_WEDGTEXCOORD | Mask::IOM_VERTTEXCOORD)) != 0;
        if (!hasTexCoords)
            return fail(QObject::tr("Filter '%1' requires %2 to have texture coordinates.").arg(filterName, subject));
    }
    if (req.requireTextures && !meshHasAnyTextureAssociation(meshEntry))
        return fail(QObject::tr("Filter '%1' requires %2 to have associated textures.").arg(filterName, subject));
    if (req.requireVertexQuality && (meshEntry.ioMask & Mask::IOM_VERTQUALITY) == 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have vertex quality.").arg(filterName, subject));
    if (req.requireFaceQuality && (meshEntry.ioMask & Mask::IOM_FACEQUALITY) == 0)
        return fail(QObject::tr("Filter '%1' requires %2 to have face quality.").arg(filterName, subject));

    return true;
}

int meshIndexFromVariant(const QVariant &value, int fallback)
{
    bool ok = false;
    const int meshIndex = value.toInt(&ok);
    return ok ? meshIndex : fallback;
}

// Enables OCF components, computes topology/normals/bbox as declared in
// descriptor.inputPrepare, and returns a scope guard that disables volatile
// components on destruction. Persistent output storage (texcoords/curvature
// directions) is intentionally left enabled because filters write results there.
MeshPreparationScope prepareMeshForPrepList(
    VCGMesh &mesh,
    const QStringList &prep)
{
    MeshPreparationScope scope;
    if (prep.isEmpty())
        return scope;
    scope.mesh = &mesh;

    // Apply in dependency order: enable OCF → topology → border flags → normals → bbox.
    const bool doFF       = prep.contains(QStringLiteral("FF"))       || prep.contains(QStringLiteral("BorderFF"));
    const bool doVF       = prep.contains(QStringLiteral("VF"))       || prep.contains(QStringLiteral("BorderVF"));
    const bool doBorderFF = prep.contains(QStringLiteral("BorderFF"));
    const bool doBorderVF = prep.contains(QStringLiteral("BorderVF"));
    const bool doFNorm    = prep.contains(QStringLiteral("FNorm"))    || prep.contains(QStringLiteral("VNorm"));
    const bool doVNorm    = prep.contains(QStringLiteral("VNorm"));
    const bool doBBox     = prep.contains(QStringLiteral("BBox"));
    const bool doFaceMark   = prep.contains(QStringLiteral("FMark")) || prep.contains(QStringLiteral("Mark"));
    const bool doVertexMark = prep.contains(QStringLiteral("VMark"));
    const bool doVertexTex =
        prep.contains(QStringLiteral("VTex")) || prep.contains(QStringLiteral("VertexTex"))
        || prep.contains(QStringLiteral("VT"));
    const bool doWedgeTex =
        prep.contains(QStringLiteral("WTex")) || prep.contains(QStringLiteral("WedgeTex"))
        || prep.contains(QStringLiteral("WT"));
    const bool doCurvatureDir =
        prep.contains(QStringLiteral("CurvDir")) || prep.contains(QStringLiteral("CurvatureDir"));

    if (doVertexTex)
        mesh.vert.EnableTexCoord();
    if (doWedgeTex)
        mesh.face.EnableWedgeTexCoord();
    if (doCurvatureDir)
        mesh.vert.EnableCurvatureDir();
    if (doFF) {
        mesh.face.EnableFFAdjacency();
        scope.disableFF = true;
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
    }
    if (doVF) {
        mesh.vert.EnableVFAdjacency();
        mesh.face.EnableVFAdjacency();
        scope.disableVF = true;
        vcg::tri::UpdateTopology<VCGMesh>::VertexFace(mesh);
    }
    if (doBorderFF) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::VertexBorderFromFaceBorder(mesh);
    }
    if (doBorderVF) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromVF(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::VertexBorderFromFaceBorder(mesh);
    }
    if (doFNorm) vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(mesh);
    if (doVNorm) {
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexFromCurrentFaceNormal(mesh);
        vcg::tri::UpdateNormal<VCGMesh>::NormalizePerVertex(mesh);
    }
    if (doBBox) vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (doFaceMark) {
        mesh.face.EnableMark();
        scope.disableMark = true;
    }
    if (doVertexMark) {
        mesh.vert.EnableMark();
        scope.disableVertexMark = true;
    }

    return scope;
}

MultiMeshPreparationScope prepareMeshesForFilter(
    const MeshFilterDescriptor &descriptor,
    const FilterParams &params,
    Document &doc)
{
    MultiMeshPreparationScope multiScope;
    QMap<int, QStringList> prepByMeshIndex;

    auto mergePrep = [&](int meshIndex, const QStringList &prep) {
        if (meshIndex < 0 || meshIndex >= doc.meshCount() || prep.isEmpty())
            return;
        QStringList &merged = prepByMeshIndex[meshIndex];
        for (const QString &item : prep) {
            if (!merged.contains(item))
                merged.push_back(item);
        }
    };

    if (descriptor.inputDomain == MeshFilterInputDomain::SingleMesh && !descriptor.inputPrepare.isEmpty())
        mergePrep(doc.currentMeshIndex(), descriptor.inputPrepare);

    for (const MeshFilterParameterDescriptor &parameter : descriptor.parameters) {
        if (parameter.type != MeshFilterParameterType::Mesh || parameter.meshPrepare.isEmpty())
            continue;
        mergePrep(params.getMesh(parameter.id, doc.currentMeshIndex()), parameter.meshPrepare);
    }

    multiScope.scopes.reserve(prepByMeshIndex.size());
    for (auto it = prepByMeshIndex.begin(); it != prepByMeshIndex.end(); ++it)
        multiScope.scopes.push_back(prepareMeshForPrepList(doc.mesh(it.key()).mesh, it.value()));

    return multiScope;
}



const MeshFilterDescriptor *findDescriptorById(
    const std::vector<MeshFilterDescriptor> &descriptors,
    const QString &filterId)
{
    for (const MeshFilterDescriptor &descriptor : descriptors) {
        if (descriptor.id == filterId)
            return &descriptor;
    }
    return nullptr;
}

QVariant defaultValueForParameter(const MeshFilterParameterDescriptor &parameter)
{
    if (parameter.defaultValue.isValid())
        return parameter.defaultValue;

    switch (parameter.type) {
    case MeshFilterParameterType::Bool:
        return false;
    case MeshFilterParameterType::Int:
    case MeshFilterParameterType::Mesh:
    case MeshFilterParameterType::TextureRef:
        return 0;
    case MeshFilterParameterType::TextureOutputRef:
        return QVariantMap{
            { QStringLiteral("mode"), QStringLiteral("new") },
            { QStringLiteral("path"), parameter.defaultValue.toString() }
        };
    case MeshFilterParameterType::Double:
    case MeshFilterParameterType::AbsPerc:
        return 0.0;
    case MeshFilterParameterType::String:
    case MeshFilterParameterType::FileOpen:
    case MeshFilterParameterType::FileSave:
    case MeshFilterParameterType::CameraState:
    case MeshFilterParameterType::RenderState:
        return QString();
    case MeshFilterParameterType::Enum:
        if (!parameter.enumOptions.empty())
            return parameter.enumOptions.front().id;
        return QString();
    case MeshFilterParameterType::Color:
        return QColor(Qt::white);
    case MeshFilterParameterType::Point3f:
        return QVariant::fromValue(QVector3D(0.0f, 0.0f, 0.0f));
    }
    return {};
}

bool parseBoolFromVariant(const QVariant &value, bool &out)
{
    if (value.userType() == QMetaType::Bool) {
        out = value.toBool();
        return true;
    }

    bool intOk = false;
    const int intValue = value.toInt(&intOk);
    if (intOk) {
        out = (intValue != 0);
        return true;
    }

    const QString text = value.toString().trimmed().toLower();
    if (text == QStringLiteral("true")
        || text == QStringLiteral("1")
        || text == QStringLiteral("yes")
        || text == QStringLiteral("on")) {
        out = true;
        return true;
    }
    if (text == QStringLiteral("false")
        || text == QStringLiteral("0")
        || text == QStringLiteral("no")
        || text == QStringLiteral("off")) {
        out = false;
        return true;
    }
    return false;
}

QString parameterErrorPrefix(const MeshFilterParameterDescriptor &parameter)
{
    return QObject::tr("Parameter '%1'").arg(parameter.id);
}

QString appendSuffixIfMissing(const QString &path, const QString &suffix)
{
    if (path.trimmed().isEmpty() || suffix.trimmed().isEmpty())
        return path;
    if (!QFileInfo(path).suffix().isEmpty())
        return path;
    return QStringLiteral("%1.%2").arg(path, suffix);
}

bool validateNamedMeshParameters(
    const MeshFilterDescriptor &descriptor,
    const MeshFilterParameterValues &parameterValues,
    const Document &doc,
    QString &errorMessage)
{
    for (const MeshFilterParameterDescriptor &parameter : descriptor.parameters) {
        if (parameter.type != MeshFilterParameterType::Mesh)
            continue;

        const int meshIndex = meshIndexFromVariant(
            parameterValues.value(parameter.id, parameter.defaultValue),
            doc.currentMeshIndex());
        if (meshIndex < 0 || meshIndex >= doc.meshCount()) {
            errorMessage = QObject::tr("Filter '%1' requires a valid mesh selection for '%2'.")
                               .arg(descriptor.name, meshSubjectLabel(parameter.label));
            return false;
        }

        if (parameter.meshRequirements.hasAnyRequirement()) {
            if (!validateMeshRequirements(
                    descriptor.name,
                    parameter.label,
                    doc.mesh(meshIndex),
                    parameter.meshRequirements,
                    errorMessage)) {
                return false;
            }
        }
    }

    return true;
}
}

void MeshFilterPluginManager::registerPlugin(std::unique_ptr<MeshFilterPlugin> plugin)
{
    if (!plugin)
        return;
    m_plugins.push_back(std::move(plugin));
}

std::vector<MeshFilterPluginManager::FilterInfo> MeshFilterPluginManager::filterInfos(
    const Document &doc) const
{
    std::vector<FilterInfo> infos;
    for (const auto &plugin : m_plugins) {
        const std::vector<MeshFilterDescriptor> descriptors = plugin->filters(doc);
        infos.reserve(infos.size() + descriptors.size());
        for (const MeshFilterDescriptor &descriptor : descriptors) {
            FilterInfo info;
            info.key = buildFilterKey(plugin->pluginId(), descriptor.id);
            info.pluginId = plugin->pluginId();
            info.pluginName = plugin->name();
            info.descriptor = descriptor;
            QString applicabilityError;
            info.applicable = validateDomain(descriptor, doc, applicabilityError);
            info.applicabilityError = applicabilityError;
            infos.push_back(std::move(info));
        }
    }
    return infos;
}

std::optional<MeshFilterPluginManager::FilterInfo> MeshFilterPluginManager::filterInfo(
    const QString &filterKey,
    const Document &doc) const
{
    QString pluginId;
    QString filterId;
    if (!splitFilterKey(filterKey, pluginId, filterId))
        return std::nullopt;

    for (const auto &plugin : m_plugins) {
        if (plugin->pluginId() != pluginId)
            continue;
        const std::vector<MeshFilterDescriptor> descriptors = plugin->filters(doc);
        const MeshFilterDescriptor *descriptor = findDescriptorById(descriptors, filterId);
        if (!descriptor)
            return std::nullopt;

        FilterInfo info;
        info.key = buildFilterKey(pluginId, filterId);
        info.pluginId = pluginId;
        info.pluginName = plugin->name();
        info.descriptor = *descriptor;
        QString applicabilityError;
        info.applicable = validateDomain(*descriptor, doc, applicabilityError);
        info.applicabilityError = applicabilityError;
        return info;
    }
    return std::nullopt;
}

bool MeshFilterPluginManager::validateFilterInvocation(
    const QString &filterKey,
    const MeshFilterParameterValues &parameters,
    const Document &doc,
    QString &errorMessage) const
{
    QString pluginId;
    QString filterId;
    if (!splitFilterKey(filterKey, pluginId, filterId)) {
        errorMessage = QObject::tr("Invalid filter key: %1").arg(filterKey);
        return false;
    }

    const MeshFilterPlugin *targetPlugin = nullptr;
    const MeshFilterDescriptor *targetDescriptor = nullptr;
    std::vector<MeshFilterDescriptor> descriptors;
    for (const auto &plugin : m_plugins) {
        if (plugin->pluginId() != pluginId)
            continue;
        targetPlugin = plugin.get();
        descriptors = plugin->filters(doc);
        targetDescriptor = findDescriptorById(descriptors, filterId);
        break;
    }

    if (!targetPlugin || !targetDescriptor) {
        errorMessage = QObject::tr("Filter not found: %1").arg(filterKey);
        return false;
    }

    if (!validateDomain(*targetDescriptor, doc, errorMessage))
        return false;

    MeshFilterParameterValues normalizedParameters;
    return normalizeAndValidateParameters(
        *targetDescriptor,
        parameters,
        doc,
        normalizedParameters,
        errorMessage);
}

QStringList MeshFilterPluginManager::loadedPluginSummaries() const
{
    QStringList summaries;
    summaries.reserve(static_cast<int>(m_plugins.size()));
    for (const auto &plugin : m_plugins) {
        summaries.push_back(QObject::tr("%1 (%2)").arg(plugin->name(), plugin->pluginId()));
    }
    return summaries;
}

MeshFilterRunResult MeshFilterPluginManager::runFilter(
    const QString &filterKey,
    const MeshFilterParameterValues &parameters,
    Document &doc) const
{
    QString pluginId;
    QString filterId;
    if (!splitFilterKey(filterKey, pluginId, filterId)) {
        return {
            false,
            false,
            QObject::tr("Invalid filter key: %1").arg(filterKey)
        };
    }

    const MeshFilterPlugin *targetPlugin = nullptr;
    const MeshFilterDescriptor *targetDescriptor = nullptr;
    std::vector<MeshFilterDescriptor> descriptors;
    for (const auto &plugin : m_plugins) {
        if (plugin->pluginId() != pluginId)
            continue;
        targetPlugin = plugin.get();
        descriptors = plugin->filters(doc);
        targetDescriptor = findDescriptorById(descriptors, filterId);
        break;
    }

    if (!targetPlugin || !targetDescriptor) {
        return {
            false,
            false,
            QObject::tr("Filter not found: %1").arg(filterKey)
        };
    }

    QString domainError;
    if (!validateDomain(*targetDescriptor, doc, domainError)) {
        return {
            false,
            false,
            domainError
        };
    }

    MeshFilterParameterValues normalizedParameters;
    QString parameterError;
    if (!normalizeAndValidateParameters(
            *targetDescriptor,
            parameters,
            doc,
            normalizedParameters,
            parameterError)) {
        return {
            false,
            false,
            parameterError
        };
    }

    // A filter that only touches selection bits (VS/FS) on a single mesh can use
    // the cheap bit-packed delta-undo path instead of a full geometry snapshot —
    // critical for large meshes, where snapshotting is seconds of deep-copy.
    const bool selectionOnlyUndo =
        targetDescriptor->inputDomain == MeshFilterInputDomain::SingleMesh
        && !targetDescriptor->outputModifies.isEmpty()
        && std::all_of(
               targetDescriptor->outputModifies.cbegin(),
               targetDescriptor->outputModifies.cend(),
               [](const QString &code) {
                   return code == QStringLiteral("VS") || code == QStringLiteral("FS");
               });
    const int originalCurrentMeshIndex = doc.currentMeshIndex();
    ScriptAction scriptAction;
    scriptAction.kind = QStringLiteral("filter");
    scriptAction.filterKey = filterKey;
    scriptAction.currentMeshIndex = doc.currentMeshIndex();
    scriptAction.currentRasterIndex = doc.currentRasterIndex();
    scriptAction.currentLayerKind = doc.currentLayerKind();
    // Preserve the original invocation values for history/script export so the
    // generated Python call matches what the user explicitly chose in the GUI.
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it)
        scriptAction.params[it.key()] = it.value();
    scriptAction.pythonCall = filterCallToPython(*targetDescriptor, parameters, true);
    scriptAction.compactPythonCall = filterCallToPython(*targetDescriptor, parameters, false);

    // Even an informational filter gets an outer transaction. If it creates
    // output conditionally, nested Document operations then collapse into this
    // single filter step. A non-mutating result discards the snapshot below but
    // retains its ScriptAction separately for Python history generation.
    //
    // Unless a step is already running: a caller applying this filter to several layers
    // brackets the whole sweep so that it lands as one undo entry. The same
    // own-the-step idiom guards the nested Document operations in document_layers.cpp.
    const bool ownUndoStep = !doc.isRestoringUndoRedo() && !doc.undoStepActive();
    if (ownUndoStep) {
        if (selectionOnlyUndo && originalCurrentMeshIndex >= 0)
            doc.beginUndoStep(targetDescriptor->name, scriptAction, originalCurrentMeshIndex);
        else
            doc.beginUndoStep(targetDescriptor->name, scriptAction);
    }

    const FilterParams typedParams(normalizedParameters);
    const CleanupApplicationResult preCleanup =
        applyCleanupActions(targetDescriptor->preRunCleanup, typedParams, doc, originalCurrentMeshIndex, true);

    // Framework-level incremental selection: save the current face/vertex selection
    // bits before running the filter, then OR them back afterwards.
    const bool saveSelection =
        targetDescriptor->incrementalSelection
        && FilterParams(normalizedParameters).getBool(
               QStringLiteral("incremental_selection"));

    std::vector<bool> savedFaceSel;
    std::vector<bool> savedVertSel;
    if (saveSelection) {
        const int meshIdx = doc.currentMeshIndex();
        if (meshIdx >= 0 && meshIdx < doc.meshCount()) {
            const VCGMesh &m = doc.mesh(meshIdx).mesh;
            savedFaceSel.reserve(m.face.size());
            for (const VCGFace &f : m.face)
                savedFaceSel.push_back(!f.IsD() && f.IsS());
            savedVertSel.reserve(m.vert.size());
            for (const VCGVertex &v : m.vert)
                savedVertSel.push_back(!v.IsD() && v.IsS());
        }
    }

    MeshFilterRunResult result;
    {
        // Enable OCF components, compute topology/normals, and keep them alive
        // until the filter returns (scope destructor disables them).
        const MultiMeshPreparationScope prepScope = prepareMeshesForFilter(*targetDescriptor, typedParams, doc);
        result = targetPlugin->runFilter(filterId, typedParams, doc);
        if (!result.success) {
            // Only roll back a step we opened ourselves: inside an outer transaction
            // this would discard the caller's entire step, not just this one run.
            if (ownUndoStep)
                doc.endUndoStep(false, true);
            return result;
        }

        // OR back the previously saved selection (if incremental was requested).
        if (saveSelection) {
            const int meshIdx = doc.currentMeshIndex();
            if (meshIdx >= 0 && meshIdx < doc.meshCount()) {
                VCGMesh &m = doc.mesh(meshIdx).mesh;
                for (size_t i = 0; i < savedFaceSel.size() && i < m.face.size(); ++i) {
                    if (savedFaceSel[i] && !m.face[i].IsD())
                        m.face[i].SetS();
                }
                for (size_t i = 0; i < savedVertSel.size() && i < m.vert.size(); ++i) {
                    if (savedVertSel[i] && !m.vert[i].IsD())
                        m.vert[i].SetS();
                }
            }
        }
    }

    const CleanupApplicationResult postCleanup =
        applyCleanupActions(targetDescriptor->postRunCleanup, typedParams, doc, originalCurrentMeshIndex, false);
    const bool pluginAlreadyMarkedModified = result.documentModified;
    const bool cleanupModifiedByFramework = preCleanup.modified || postCleanup.modified;
    if (cleanupModifiedByFramework)
        result.documentModified = true;
    result.infoMessages.append(preCleanup.infoMessages);
    result.infoMessages.append(postCleanup.infoMessages);

    if (cleanupModifiedByFramework && !pluginAlreadyMarkedModified) {
        QSet<int> cleanedMeshIndices = preCleanup.modifiedMeshIndices;
        cleanedMeshIndices.unite(postCleanup.modifiedMeshIndices);
        for (int meshIndex : std::as_const(cleanedMeshIndices)) {
            if (meshIndex >= 0 && meshIndex < doc.meshCount()) {
                doc.markMeshGeometryChanged(
                    meshIndex,
                    QObject::tr("Applied framework cleanup for '%1'").arg(targetDescriptor->name));
            }
        }
    }

    // Enforce the framework invariant that successful filters never leave
    // document meshes with deleted elements in storage.
    finalizeMeshesAfterSuccessfulFilter(*targetDescriptor, result, doc, originalCurrentMeshIndex);

    if (ownUndoStep)
        doc.endUndoStep(result.documentModified);
    // A run nested in someone else's step contributes no undo entry of its own, so its
    // call is kept as an informational record instead: the script history still lists
    // every individual invocation together with the layer it ran on.
    if (!ownUndoStep || !result.documentModified)
        doc.recordScriptAction(scriptAction);

    for (const QString &line : result.infoMessages) {
        if (!line.isEmpty()) {
            doc.writeLog(
                QObject::tr("Filter '%1': %2").arg(targetDescriptor->name, line),
                Document::LogSource::Application);
        }
    }

    return result;
}

QString MeshFilterPluginManager::buildFilterKey(const QString &pluginId, const QString &filterId)
{
    return pluginId + kKeySeparator + filterId;
}

bool MeshFilterPluginManager::splitFilterKey(
    const QString &filterKey,
    QString &pluginId,
    QString &filterId)
{
    const int sep = filterKey.indexOf(kKeySeparator);
    if (sep <= 0)
        return false;
    const int sepLen = int(kKeySeparator.size());
    if (sep + sepLen >= filterKey.size())
        return false;

    pluginId = filterKey.left(sep).trimmed();
    filterId = filterKey.mid(sep + sepLen).trimmed();
    return !pluginId.isEmpty() && !filterId.isEmpty();
}

bool MeshFilterPluginManager::validateDomain(
    const MeshFilterDescriptor &descriptor,
    const Document &doc,
    QString &errorMessage) const
{
    if (descriptor.inputDomain == MeshFilterInputDomain::None)
        return true;

    if (descriptor.inputDomain == MeshFilterInputDomain::WholeDocument) {
        if (doc.meshCount() <= 0) {
            errorMessage = QObject::tr("Filter '%1' requires a non-empty document.")
                               .arg(descriptor.name);
            return false;
        }
    }

    const int meshIndex = doc.currentMeshIndex();
    const bool mustValidateCurrentMesh =
        descriptor.inputDomain == MeshFilterInputDomain::SingleMesh
        || descriptor.inputRequirements.hasAnyRequirement();
    if (mustValidateCurrentMesh && (meshIndex < 0 || meshIndex >= doc.meshCount())) {
        errorMessage = QObject::tr("Filter '%1' requires a current mesh.")
                           .arg(descriptor.name);
        return false;
    }

    if (mustValidateCurrentMesh) {
        if (!validateMeshRequirements(
                descriptor.name,
                QObject::tr("Current Mesh"),
                doc.mesh(meshIndex),
                descriptor.inputRequirements,
                errorMessage)) {
            return false;
        }
    }

    return true;
}

bool MeshFilterPluginManager::normalizeAndValidateParameters(
    const MeshFilterDescriptor &descriptor,
    const MeshFilterParameterValues &inputParameters,
    const Document &doc,
    MeshFilterParameterValues &normalizedParameters,
    QString &errorMessage) const
{
    normalizedParameters.clear();
    QSet<QString> knownKeys;
    knownKeys.reserve(static_cast<qsizetype>(descriptor.parameters.size()));

    for (const MeshFilterParameterDescriptor &parameter : descriptor.parameters) {
        const QString parameterId = parameter.id.trimmed();
        if (parameterId.isEmpty())
            continue;
        knownKeys.insert(parameterId);

        const QVariant rawValue = inputParameters.contains(parameterId)
            ? inputParameters.value(parameterId)
            : defaultValueForParameter(parameter);

        QVariant convertedValue;
        if (!convertParameterValue(parameter, rawValue, convertedValue, errorMessage))
            return false;

        normalizedParameters.insert(parameterId, convertedValue);
    }

    for (auto it = inputParameters.constBegin(); it != inputParameters.constEnd(); ++it) {
        if (!knownKeys.contains(it.key())) {
            errorMessage = QObject::tr("Unknown parameter '%1' for filter '%2'.")
                               .arg(it.key(), descriptor.name);
            return false;
        }
    }

    return validateNamedMeshParameters(descriptor, normalizedParameters, doc, errorMessage);
}

bool MeshFilterPluginManager::convertParameterValue(
    const MeshFilterParameterDescriptor &parameter,
    const QVariant &inputValue,
    QVariant &outputValue,
    QString &errorMessage)
{
    const QString prefix = parameterErrorPrefix(parameter);

    switch (parameter.type) {
    case MeshFilterParameterType::Bool: {
        bool value = false;
        if (!parseBoolFromVariant(inputValue, value)) {
            errorMessage = QObject::tr("%1 must be a boolean value.").arg(prefix);
            return false;
        }
        outputValue = value;
        return true;
    }
    case MeshFilterParameterType::Int:
    case MeshFilterParameterType::Mesh:
    case MeshFilterParameterType::TextureRef: {
        bool ok = false;
        const qlonglong value = inputValue.toLongLong(&ok);
        if (!ok) {
            errorMessage = QObject::tr("%1 must be an integer value.").arg(prefix);
            return false;
        }
        if (parameter.minValue.isValid() && value < parameter.minValue.toLongLong()) {
            errorMessage = QObject::tr("%1 is below minimum (%2).")
                               .arg(prefix)
                               .arg(parameter.minValue.toLongLong());
            return false;
        }
        if (parameter.maxValue.isValid() && value > parameter.maxValue.toLongLong()) {
            errorMessage = QObject::tr("%1 is above maximum (%2).")
                               .arg(prefix)
                               .arg(parameter.maxValue.toLongLong());
            return false;
        }
        outputValue = static_cast<int>(value);
        return true;
    }
    case MeshFilterParameterType::TextureOutputRef: {
        QVariantMap map = inputValue.toMap();
        if (map.isEmpty()) {
            const QString path = appendSuffixIfMissing(
                inputValue.toString().trimmed(),
                parameter.fileDefaultSuffix.trimmed());
            if (path.isEmpty()) {
                errorMessage = QObject::tr("%1 requires either an existing texture choice or a new output file path.")
                                   .arg(prefix);
                return false;
            }
            map.insert(QStringLiteral("mode"), QStringLiteral("new"));
            map.insert(QStringLiteral("path"), path);
        }

        const QString mode = map.value(QStringLiteral("mode")).toString().trimmed().toLower();
        if (mode == QStringLiteral("existing")) {
            bool ok = false;
            const int oneBasedSlot = map.value(QStringLiteral("slot")).toInt(&ok);
            if (!ok || oneBasedSlot <= 0) {
                errorMessage = QObject::tr("%1 has an invalid existing texture selection.").arg(prefix);
                return false;
            }
            outputValue = QVariantMap{
                { QStringLiteral("mode"), QStringLiteral("existing") },
                { QStringLiteral("slot"), oneBasedSlot }
            };
            return true;
        }

        const QString path = appendSuffixIfMissing(
            map.value(QStringLiteral("path")).toString().trimmed(),
            parameter.fileDefaultSuffix.trimmed());
        if (path.isEmpty()) {
            errorMessage = QObject::tr("%1 requires a destination file path when creating a new texture.")
                               .arg(prefix);
            return false;
        }
        outputValue = QVariantMap{
            { QStringLiteral("mode"), QStringLiteral("new") },
            { QStringLiteral("path"), path }
        };
        return true;
    }
    case MeshFilterParameterType::Double:
    case MeshFilterParameterType::AbsPerc: {
        bool ok = false;
        const double value = inputValue.toDouble(&ok);
        if (!ok || !std::isfinite(value)) {
            errorMessage = QObject::tr("%1 must be a finite numeric value.").arg(prefix);
            return false;
        }
        if (parameter.minValue.isValid() && value < parameter.minValue.toDouble()) {
            errorMessage = QObject::tr("%1 is below minimum (%2).")
                               .arg(prefix)
                               .arg(parameter.minValue.toDouble());
            return false;
        }
        if (parameter.maxValue.isValid() && value > parameter.maxValue.toDouble()) {
            errorMessage = QObject::tr("%1 is above maximum (%2).")
                               .arg(prefix)
                               .arg(parameter.maxValue.toDouble());
            return false;
        }
        outputValue = value;
        return true;
    }
    case MeshFilterParameterType::String:
    case MeshFilterParameterType::FileOpen: {
        const QString value = inputValue.toString().trimmed();
        if (parameter.type == MeshFilterParameterType::FileOpen && !value.isEmpty()) {
            const QFileInfo info(value);
            if (!info.exists()) {
                errorMessage = QObject::tr("%1 does not exist.").arg(prefix);
                return false;
            }
            if (!info.isFile()) {
                errorMessage = QObject::tr("%1 must point to a file.").arg(prefix);
                return false;
            }
        }
        outputValue = value;
        return true;
    }
    case MeshFilterParameterType::CameraState: {
        const QString value = inputValue.toString().trimmed();
        if (!validateStateJsonPayload(value, QStringLiteral("MeshLab.CameraState"), prefix, errorMessage))
            return false;
        outputValue = value;
        return true;
    }
    case MeshFilterParameterType::RenderState: {
        const QString value = inputValue.toString().trimmed();
        if (!validateStateJsonPayload(value, QStringLiteral("MeshLab.RenderState"), prefix, errorMessage))
            return false;
        outputValue = value;
        return true;
    }
    case MeshFilterParameterType::FileSave:
        outputValue = appendSuffixIfMissing(
            inputValue.toString().trimmed(),
            parameter.fileDefaultSuffix.trimmed());
        return true;
    case MeshFilterParameterType::Enum: {
        const QString value = inputValue.toString();
        const bool valid =
            std::any_of(parameter.enumOptions.begin(),
                        parameter.enumOptions.end(),
                        [&value](const MeshFilterEnumOption &option) { return option.id == value; });
        if (!valid) {
            errorMessage = QObject::tr("%1 has invalid option '%2'.").arg(prefix, value);
            return false;
        }
        outputValue = value;
        return true;
    }
    case MeshFilterParameterType::Color: {
        QColor color;
        if (inputValue.userType() == QMetaType::QColor) {
            color = inputValue.value<QColor>();
        } else {
            color = QColor(inputValue.toString().trimmed());
        }
        if (!color.isValid()) {
            errorMessage = QObject::tr("%1 must be a valid color (e.g. #RRGGBB).").arg(prefix);
            return false;
        }
        outputValue = color;
        return true;
    }
    case MeshFilterParameterType::Point3f: {
        if (inputValue.userType() == QMetaType::QVector3D) {
            outputValue = inputValue;
            return true;
        }
        // Accept "x,y,z" string
        const QStringList parts = inputValue.toString().trimmed().split(QLatin1Char(','));
        if (parts.size() == 3) {
            bool okX = false, okY = false, okZ = false;
            const float x = parts[0].trimmed().toFloat(&okX);
            const float y = parts[1].trimmed().toFloat(&okY);
            const float z = parts[2].trimmed().toFloat(&okZ);
            if (okX && okY && okZ) {
                outputValue = QVariant::fromValue(QVector3D(x, y, z));
                return true;
            }
        }
        errorMessage = QObject::tr("%1 must be a 3D point (QVector3D or \"x,y,z\" string).").arg(prefix);
        return false;
    }
    }

    errorMessage = QObject::tr("%1 has unsupported parameter type.").arg(prefix);
    return false;
}
