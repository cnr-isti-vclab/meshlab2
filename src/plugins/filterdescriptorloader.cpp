#include "filterdescriptorloader.h"

#include "filtercategories.h"
#include <QDebug>

#include "document.h"
#include "vcgmesh.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/stat.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <QColor>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector3D>
#include <algorithm>
#include <cmath>
#include <thread>

namespace {

MeshFilterInputDomain parseInputDomain(const QString &s)
{
    if (s == QStringLiteral("None"))
        return MeshFilterInputDomain::None;
    if (s == QStringLiteral("WholeDocument"))
        return MeshFilterInputDomain::WholeDocument;
    return MeshFilterInputDomain::SingleMesh;
}

MeshFilterOutputDomain parseOutputDomain(const QString &s)
{
    if (s == QStringLiteral("NewMeshes"))
        return MeshFilterOutputDomain::NewMeshes;
    if (s == QStringLiteral("ModifyCurrentMesh"))
        return MeshFilterOutputDomain::ModifyCurrentMesh;
    return MeshFilterOutputDomain::Information;
}

MeshFilterParameterType parseParamType(const QString &s)
{
    if (s == QStringLiteral("bool"))   return MeshFilterParameterType::Bool;
    if (s == QStringLiteral("int"))    return MeshFilterParameterType::Int;
    if (s == QStringLiteral("double")) return MeshFilterParameterType::Double;
    if (s == QStringLiteral("absperc")) return MeshFilterParameterType::AbsPerc;
    if (s == QStringLiteral("mesh"))   return MeshFilterParameterType::Mesh;
    if (s == QStringLiteral("string")) return MeshFilterParameterType::String;
    if (s == QStringLiteral("fileopen") || s == QStringLiteral("file_open")) return MeshFilterParameterType::FileOpen;
    if (s == QStringLiteral("filesave") || s == QStringLiteral("file_save")) return MeshFilterParameterType::FileSave;
    if (s == QStringLiteral("textureref")) return MeshFilterParameterType::TextureRef;
    if (s == QStringLiteral("textureoutputref")) return MeshFilterParameterType::TextureOutputRef;
    if (s == QStringLiteral("enum"))   return MeshFilterParameterType::Enum;
    if (s == QStringLiteral("color"))  return MeshFilterParameterType::Color;
    if (s == QStringLiteral("point3f")) return MeshFilterParameterType::Point3f;
    if (s == QStringLiteral("camerastate")) return MeshFilterParameterType::CameraState;
    if (s == QStringLiteral("renderstate")) return MeshFilterParameterType::RenderState;
    return MeshFilterParameterType::String;
}

MeshFilterCleanupKind parseCleanupKind(const QString &s)
{
    if (s == QStringLiteral("removeUnreferencedVertices"))
        return MeshFilterCleanupKind::RemoveUnreferencedVertices;
    return MeshFilterCleanupKind::RemoveUnreferencedVertices;
}

MeshFilterProvenance parseProvenance(const QJsonObject &obj)
{
    MeshFilterProvenance provenance;
    provenance.project = obj.value(QStringLiteral("project")).toString();
    provenance.repository = obj.value(QStringLiteral("repository")).toString();
    provenance.license = obj.value(QStringLiteral("license")).toString();
    provenance.integration = obj.value(QStringLiteral("integration")).toString();
    return provenance;
}

MeshFilterReference parseReference(const QJsonObject &obj)
{
    MeshFilterReference reference;
    reference.id = obj.value(QStringLiteral("id")).toString();
    reference.type = obj.value(QStringLiteral("type")).toString();
    reference.title = obj.value(QStringLiteral("title")).toString();
    reference.containerTitle = obj.value(QStringLiteral("container-title")).toString();
    reference.publisher = obj.value(QStringLiteral("publisher")).toString();
    reference.volume = obj.value(QStringLiteral("volume")).toVariant().toString();
    reference.issue = obj.value(QStringLiteral("issue")).toVariant().toString();
    reference.page = obj.value(QStringLiteral("page")).toString();
    reference.doi = obj.value(QStringLiteral("DOI")).toString();
    reference.url = obj.value(QStringLiteral("URL")).toString();
    reference.isbn = obj.value(QStringLiteral("ISBN")).toString();
    reference.edition = obj.value(QStringLiteral("edition")).toVariant().toString();

    const QJsonArray authors = obj.value(QStringLiteral("author")).toArray();
    for (const QJsonValue &value : authors) {
        const QJsonObject authorObject = value.toObject();
        reference.authors.push_back({
            authorObject.value(QStringLiteral("family")).toString(),
            authorObject.value(QStringLiteral("given")).toString()
        });
    }

    const QJsonArray dateParts = obj.value(QStringLiteral("issued"))
                                     .toObject()
                                     .value(QStringLiteral("date-parts"))
                                     .toArray();
    if (!dateParts.isEmpty()) {
        const QJsonArray firstDate = dateParts.first().toArray();
        if (!firstDate.isEmpty())
            reference.year = firstDate.first().toInt();
    }
    return reference;
}

// A QVariant that is a QString starting with "@" is a symbolic token.
// Non-symbolic values are parsed from JSON numbers/booleans/strings.
QVariant parseJsonValue(const QJsonValue &v)
{
    if (v.isNull() || v.isUndefined())
        return QVariant();
    if (v.isBool())
        return v.toBool();
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        const QString s = v.toString();
        // Keep symbolic tokens as-is so resolveSymbolicBounds can handle them.
        return s;
    }
    return QVariant();
}

MeshFilterParameterDescriptor parseParameter(const QJsonObject &obj)
{
    MeshFilterParameterDescriptor p;
    p.id           = obj.value(QStringLiteral("id")).toString();
    p.label        = obj.value(QStringLiteral("label")).toString();
    p.helpMarkdown = obj.value(QStringLiteral("help")).toString();
    p.group        = obj.value(QStringLiteral("group")).toString(QStringLiteral("main"));
    p.type         = parseParamType(obj.value(QStringLiteral("type")).toString());
    p.decimals     = obj.value(QStringLiteral("decimals")).toInt(3);
    p.fileDialogTitle = obj.value(QStringLiteral("fileDialogTitle")).toString();
    p.enabledWhen  = obj.value(QStringLiteral("enabledWhen")).toString().trimmed();

    const QJsonArray nameFilters = obj.value(QStringLiteral("fileNameFilters")).toArray();
    for (const QJsonValue &fv : nameFilters)
        p.fileNameFilters.push_back(fv.toString());
    p.fileDefaultSuffix = obj.value(QStringLiteral("fileDefaultSuffix")).toString();
    p.textureSourceMeshParameter = obj.value(QStringLiteral("sourceMeshParameter")).toString();
    p.textureAllowAutomatic = obj.value(QStringLiteral("allowAutomatic")).toBool(true);

    const QJsonObject meshReq = obj.value(QStringLiteral("meshRequirements")).toObject();
    p.meshRequirements.requireVertices = meshReq.value(QStringLiteral("requireVertices")).toBool(false);
    p.meshRequirements.requireEdges = meshReq.value(QStringLiteral("requireEdges")).toBool(false);
    p.meshRequirements.requireFaces = meshReq.value(QStringLiteral("requireFaces")).toBool(false);
    p.meshRequirements.requireVertexColor = meshReq.value(QStringLiteral("requireVertexColor")).toBool(false);
    p.meshRequirements.requireFaceColor = meshReq.value(QStringLiteral("requireFaceColor")).toBool(false);
    p.meshRequirements.requireTextureCoordinates = meshReq.value(QStringLiteral("requireTextureCoordinates")).toBool(false);
    p.meshRequirements.requirePerVertexTexCoords = meshReq.value(QStringLiteral("requirePerVertexTexCoords")).toBool(false);
    p.meshRequirements.requirePerWedgeTexCoords = meshReq.value(QStringLiteral("requirePerWedgeTexCoords")).toBool(false);
    p.meshRequirements.requireTextures = meshReq.value(QStringLiteral("requireTextures")).toBool(false);
    p.meshRequirements.requireVertexQuality = meshReq.value(QStringLiteral("requireVertexQuality")).toBool(false);
    p.meshRequirements.requireEdgeQuality = meshReq.value(QStringLiteral("requireEdgeQuality")).toBool(false);
    p.meshRequirements.requireFaceQuality = meshReq.value(QStringLiteral("requireFaceQuality")).toBool(false);

    p.meshAllowsNone = obj.value(QStringLiteral("meshAllowsNone")).toBool(false);

    const QJsonArray meshPrep = obj.value(QStringLiteral("meshPrepare")).toArray();
    for (const QJsonValue &pv : meshPrep)
        p.meshPrepare << pv.toString();

    if (p.type == MeshFilterParameterType::Point3f) {
        // Default is a JSON array [x, y, z]; min/max are not applicable.
        const QJsonValue defVal = obj.value(QStringLiteral("default"));
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (defVal.isArray()) {
            const QJsonArray arr = defVal.toArray();
            if (arr.size() > 0) x = float(arr[0].toDouble());
            if (arr.size() > 1) y = float(arr[1].toDouble());
            if (arr.size() > 2) z = float(arr[2].toDouble());
        }
        p.defaultValue = QVariant::fromValue(QVector3D(x, y, z));
        p.point3fRole  = obj.value(QStringLiteral("point3fRole")).toString(QStringLiteral("point"));
        p.point3fDefaultPreset = obj.value(QStringLiteral("point3fDefaultPreset")).toString();
    } else {
        p.defaultValue = parseJsonValue(obj.value(QStringLiteral("default")));
        p.minValue     = parseJsonValue(obj.value(QStringLiteral("min")));
        p.maxValue     = parseJsonValue(obj.value(QStringLiteral("max")));
    }

    const QJsonArray opts = obj.value(QStringLiteral("enumOptions")).toArray();
    for (const QJsonValue &ov : opts) {
        const QJsonObject oo = ov.toObject();
        MeshFilterEnumOption opt;
        opt.id           = oo.value(QStringLiteral("id")).toString();
        opt.label        = oo.value(QStringLiteral("label")).toString();
        opt.helpMarkdown = oo.value(QStringLiteral("help")).toString();
        p.enumOptions.push_back(std::move(opt));
    }
    return p;
}

MeshFilterDescriptor parseFilter(const QJsonObject &obj)
{
    MeshFilterDescriptor d;
    d.id                    = obj.value(QStringLiteral("id")).toString();
    // `categories` is the authored field. A legacy single `menuPath` string is still
    // accepted so an out-of-tree descriptor keeps loading; it is treated as one
    // category, so an out-of-tree descriptor written against the old schema keeps
    // loading.
    for (const QJsonValue &c : obj.value(QStringLiteral("categories")).toArray()) {
        const QString path = c.toString().trimmed();
        if (!path.isEmpty() && !d.categories.contains(path))
            d.categories << path;
    }
    if (d.categories.isEmpty()) {
        const QString legacy = obj.value(QStringLiteral("menuPath")).toString().trimmed();
        if (!legacy.isEmpty())
            d.categories << legacy;
    }
    d.name                  = obj.value(QStringLiteral("name")).toString();
    d.pythonName            = obj.value(QStringLiteral("pythonName")).toString();
    d.shortDescription      = obj.value(QStringLiteral("shortDescription")).toString();
    d.longDescriptionMarkdown = obj.value(QStringLiteral("longDescriptionMarkdown")).toString();

    const QJsonArray tags = obj.value(QStringLiteral("tags")).toArray();
    for (const QJsonValue &t : tags)
        d.tags << t.toString();

    d.inputDomain  = parseInputDomain(obj.value(QStringLiteral("inputDomain")).toString(QStringLiteral("SingleMesh")));
    d.outputDomain = parseOutputDomain(obj.value(QStringLiteral("outputDomain")).toString(QStringLiteral("Information")));

    const QJsonObject req = obj.value(QStringLiteral("inputRequirements")).toObject();
    d.inputRequirements.requireVertices           = req.value(QStringLiteral("requireVertices")).toBool(false);
    d.inputRequirements.requireEdges              = req.value(QStringLiteral("requireEdges")).toBool(false);
    d.inputRequirements.requireFaces              = req.value(QStringLiteral("requireFaces")).toBool(false);
    d.inputRequirements.requireVertexColor        = req.value(QStringLiteral("requireVertexColor")).toBool(false);
    d.inputRequirements.requireFaceColor          = req.value(QStringLiteral("requireFaceColor")).toBool(false);
    d.inputRequirements.requireTextureCoordinates = req.value(QStringLiteral("requireTextureCoordinates")).toBool(false);
    d.inputRequirements.requirePerVertexTexCoords = req.value(QStringLiteral("requirePerVertexTexCoords")).toBool(false);
    d.inputRequirements.requirePerWedgeTexCoords  = req.value(QStringLiteral("requirePerWedgeTexCoords")).toBool(false);
    d.inputRequirements.requireTextures           = req.value(QStringLiteral("requireTextures")).toBool(false);
    d.inputRequirements.requireVertexQuality      = req.value(QStringLiteral("requireVertexQuality")).toBool(false);
    d.inputRequirements.requireEdgeQuality      = req.value(QStringLiteral("requireEdgeQuality")).toBool(false);
    d.inputRequirements.requireFaceQuality        = req.value(QStringLiteral("requireFaceQuality")).toBool(false);

    auto parseCleanupArray = [](const QJsonArray &actions) {
        QVector<MeshFilterCleanupAction> cleanup;
        cleanup.reserve(actions.size());
        for (const QJsonValue &value : actions) {
            const QJsonObject actionObject = value.toObject();
            MeshFilterCleanupAction action;
            action.kind = parseCleanupKind(actionObject.value(QStringLiteral("kind")).toString());
            action.whenBoolParameter = actionObject.value(QStringLiteral("whenBoolParam")).toString();
            action.meshParameter = actionObject.value(QStringLiteral("meshParameter")).toString();
            cleanup.push_back(std::move(action));
        }
        return cleanup;
    };

    d.preRunCleanup = parseCleanupArray(obj.value(QStringLiteral("preRunCleanup")).toArray());
    d.postRunCleanup = parseCleanupArray(obj.value(QStringLiteral("postRunCleanup")).toArray());

    const QJsonArray params = obj.value(QStringLiteral("parameters")).toArray();
    for (const QJsonValue &pv : params) {
        d.parameters.push_back(parseParameter(pv.toObject()));
    }

    const QJsonArray mods = obj.value(QStringLiteral("outputModifies")).toArray();
    for (const QJsonValue &m : mods)
        d.outputModifies << m.toString();

    const QJsonArray prep = obj.value(QStringLiteral("inputPrepare")).toArray();
    for (const QJsonValue &p : prep)
        d.inputPrepare << p.toString();

    d.incrementalSelection = obj.value(QStringLiteral("incrementalSelection")).toBool(false);
    if (d.incrementalSelection) {
        // Inject the standard "incremental_selection" bool parameter at the end
        // (in the "main" group, so it always appears).  Plugins don't need to
        // declare it themselves — the framework manager handles the save/OR logic.
        MeshFilterParameterDescriptor p;
        p.id           = QStringLiteral("incremental_selection");
        p.label        = QObject::tr("Preserve Existing Selection");
        p.helpMarkdown = QObject::tr(
            "When enabled, the new selection is added (OR) to the existing one "
            "instead of replacing it.");
        p.group        = QStringLiteral("main");
        p.type         = MeshFilterParameterType::Bool;
        p.defaultValue = false;
        d.parameters.push_back(std::move(p));
    }

    return d;
}

// Resolve a single QVariant that may be a symbolic "@token" string.
// Returns the original value if it is not a symbolic token.
QVariant resolveToken(const QVariant &v, double bboxDiag,
                      double qualityVMin, double qualityVMax,
                      double qualityFMin, double qualityFMax,
                      bool hasSelectedFaces, bool hasSelectedVerts,
                      int faceCount, int selFaces,
                      int hardwareThreads,
                      int currentMeshIndex,
                      int otherMeshIndex)
{
    if (v.userType() != QMetaType::QString)
        return v;
    const QString s = v.toString();
    if (!s.startsWith(QLatin1Char('@')))
        return v;

    const QString token = s.mid(1); // strip leading '@'

    if (token == QStringLiteral("bboxDiag"))           return bboxDiag;
    if (token == QStringLiteral("bboxDiag01"))         return bboxDiag * 0.01;
    if (token == QStringLiteral("bboxDiag001"))        return bboxDiag * 0.001;
    if (token == QStringLiteral("bboxDiag0001"))       return bboxDiag * 0.0001;
    if (token == QStringLiteral("bboxDiag00001"))      return bboxDiag * 0.00001;
    if (token == QStringLiteral("bboxDiag0005"))       return bboxDiag * 0.005;
    if (token == QStringLiteral("bboxDiag002"))        return bboxDiag * 0.02;
    if (token == QStringLiteral("bboxDiag003"))        return bboxDiag * 0.03;
    if (token == QStringLiteral("bboxDiagTenth"))      return bboxDiag * 0.1;
    if (token == QStringLiteral("bboxDiagHalf"))       return bboxDiag * 0.5;
    if (token == QStringLiteral("bboxDiag5x"))         return bboxDiag * 5.0;
    if (token == QStringLiteral("negBboxDiag"))        return -bboxDiag;
    if (token == QStringLiteral("negBboxDiag01"))      return -bboxDiag * 0.01;
    if (token == QStringLiteral("negBboxDiag001"))     return -bboxDiag * 0.001;
    if (token == QStringLiteral("negBboxDiagTenth"))   return -bboxDiag * 0.1;
    if (token == QStringLiteral("negBboxDiagHalf"))    return -bboxDiag * 0.5;
    if (token == QStringLiteral("qualityVMin"))        return qualityVMin;
    if (token == QStringLiteral("qualityVMax"))        return qualityVMax;
    if (token == QStringLiteral("qualityFMin"))        return qualityFMin;
    if (token == QStringLiteral("qualityFMax"))        return qualityFMax;
    if (token == QStringLiteral("hasSelectedFaces"))   return hasSelectedFaces;
    if (token == QStringLiteral("hasSelectedVerts"))   return hasSelectedVerts;
    if (token == QStringLiteral("faceCount"))          return std::max(1, faceCount);
    if (token == QStringLiteral("faceCountHalf"))      return std::max(1, faceCount / 2);
    if (token == QStringLiteral("selOrFaceCountHalf")) {
        const int base = (selFaces > 0 ? selFaces : faceCount);
        return std::max(1, base / 2);
    }
    if (token == QStringLiteral("hardwareThreads"))    return hardwareThreads;
    if (token == QStringLiteral("currentMeshIndex"))   return currentMeshIndex;
    if (token == QStringLiteral("otherMeshIndex"))     return otherMeshIndex;

    return v; // unknown token → leave as-is
}

} // namespace

std::vector<MeshFilterParameterDescriptor> FilterDescriptorLoader::loadParameters(
    const QString &resourcePath,
    QString &errorMessage)
{
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("Cannot open parameter file: %1").arg(resourcePath);
        return {};
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull()) {
        errorMessage = QStringLiteral("JSON parse error in %1: %2")
                           .arg(resourcePath, err.errorString());
        return {};
    }

    const QJsonArray parameters = doc.object().value(QStringLiteral("parameters")).toArray();
    std::vector<MeshFilterParameterDescriptor> result;
    result.reserve(static_cast<size_t>(parameters.size()));
    for (const QJsonValue &pv : parameters) {
        MeshFilterParameterDescriptor p = parseParameter(pv.toObject());
        if (p.id.trimmed().isEmpty()) {
            errorMessage = QStringLiteral("Parameter without id in %1").arg(resourcePath);
            return {};
        }
        result.push_back(std::move(p));
    }
    return result;
}

std::vector<MeshFilterDescriptor> FilterDescriptorLoader::load(
    const QString &resourcePath,
    QString &errorMessage)
{
    QFile f(resourcePath);
    if (!f.open(QIODevice::ReadOnly)) {
        errorMessage = QStringLiteral("Cannot open filter descriptor file: %1").arg(resourcePath);
        return {};
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (doc.isNull()) {
        errorMessage = QStringLiteral("JSON parse error in %1: %2")
                           .arg(resourcePath, err.errorString());
        return {};
    }

    const QJsonObject root = doc.object();
    const QJsonArray filters = root.value(QStringLiteral("filters")).toArray();
    const MeshFilterProvenance provenance =
        parseProvenance(root.value(QStringLiteral("provenance")).toObject());
    QHash<QString, MeshFilterReference> references;
    for (const QJsonValue &value : root.value(QStringLiteral("references")).toArray()) {
        MeshFilterReference reference = parseReference(value.toObject());
        if (reference.id.isEmpty()) {
            errorMessage = QStringLiteral("Reference without id in %1").arg(resourcePath);
            return {};
        }
        if (references.contains(reference.id)) {
            errorMessage = QStringLiteral("Duplicate reference id '%1' in %2")
                               .arg(reference.id, resourcePath);
            return {};
        }
        references.insert(reference.id, std::move(reference));
    }

    std::vector<MeshFilterDescriptor> result;
    result.reserve(static_cast<size_t>(filters.size()));
    for (const QJsonValue &fv : filters) {
        MeshFilterDescriptor descriptor = parseFilter(fv.toObject());
        descriptor.provenance = provenance;
        for (const QJsonValue &idValue :
             fv.toObject().value(QStringLiteral("referenceIds")).toArray()) {
            const QString id = idValue.toString();
            const auto it = references.constFind(id);
            if (it == references.constEnd()) {
                errorMessage = QStringLiteral("Unknown reference id '%1' in %2")
                                   .arg(id, resourcePath);
                return {};
            }
            descriptor.references.push_back(it.value());
        }
        result.push_back(std::move(descriptor));
    }

    validateCategories(result, resourcePath);
    validateAbsPercBounds(result, resourcePath);
    return result;
}

void FilterDescriptorLoader::validateAbsPercBounds(
    const std::vector<MeshFilterDescriptor> &descriptors,
    const QString &resourcePath)
{
    // An absperc parameter is a percentage *of its max*, so a missing max silently makes
    // 100% mean 1.0 in model units — the slider then has no relation to the mesh, which
    // is impossible to spot from the UI alone. Warn rather than fail: the parameter still
    // works as an absolute value.
    QStringList offenders;
    for (const MeshFilterDescriptor &d : descriptors) {
        for (const MeshFilterParameterDescriptor &p : d.parameters) {
            if (p.type != MeshFilterParameterType::AbsPerc)
                continue;
            if (p.minValue.isValid() && p.maxValue.isValid())
                continue;
            offenders << QStringLiteral("%1.%2").arg(d.id, p.id);
        }
    }
    if (offenders.isEmpty())
        return;

    qWarning().noquote() << QStringLiteral(
        "[filter parameters] %1: absperc parameter(s) without min/max, so percentages are "
        "relative to 1.0 instead of the mesh: %2")
        .arg(resourcePath, offenders.join(QStringLiteral(", ")));
}

void FilterDescriptorLoader::validateCategories(
    const std::vector<MeshFilterDescriptor> &descriptors,
    const QString &resourcePath)
{
    // The in-tree descriptors are fully migrated, so anything reported here is a typo
    // or an out-of-tree descriptor using a stale name. Kept at warning level so a bad
    // third-party plugin degrades rather than blocks startup.
    //
    // Reported once per descriptor file, listing the distinct offenders rather than
    // one line per filter — otherwise the pre-migration state produces 272 lines.
    QStringList unknown;
    for (const MeshFilterDescriptor &d : descriptors) {
        for (const QString &path : d.categories) {
            if (path.isEmpty() || FilterCategories::isValid(path))
                continue;
            if (!unknown.contains(path))
                unknown << path;
        }
    }
    if (unknown.isEmpty())
        return;

    unknown.sort();
    qWarning().noquote() << QStringLiteral(
        "[filter categories] %1: %2 value(s) outside the ontology: %3")
        .arg(resourcePath)
        .arg(unknown.size())
        .arg(unknown.join(QStringLiteral(", ")));
}

void FilterDescriptorLoader::resolveSymbolicBounds(
    std::vector<MeshFilterDescriptor> &descriptors,
    const Document &doc)
{
    auto usesToken = [&descriptors](const QString &token) {
        for (const MeshFilterDescriptor &fd : descriptors) {
            for (const MeshFilterParameterDescriptor &p : fd.parameters) {
                if (p.defaultValue.toString() == token
                    || p.minValue.toString() == token
                    || p.maxValue.toString() == token)
                    return true;
            }
        }
        return false;
    };
    const bool needSelectedFaces =
        usesToken(QStringLiteral("@hasSelectedFaces"))
        || usesToken(QStringLiteral("@selOrFaceCountHalf"));
    const bool needSelectedVerts = usesToken(QStringLiteral("@hasSelectedVerts"));
    const bool needVertexQuality =
        usesToken(QStringLiteral("@qualityVMin")) || usesToken(QStringLiteral("@qualityVMax"));
    const bool needFaceQuality =
        usesToken(QStringLiteral("@qualityFMin")) || usesToken(QStringLiteral("@qualityFMax"));

    double bboxDiag = 1.0;
    double qualityVMin = 0.0, qualityVMax = 1.0;
    double qualityFMin = 0.0, qualityFMax = 1.0;
    bool hasSelectedFaces = false;
    bool hasSelectedVerts = false;
    int faceCount = 0;
    int selFaces = 0;

    const int ci = doc.currentMeshIndex();
    if (ci >= 0 && ci < doc.meshCount()) {
        const VCGMesh &mesh = doc.mesh(ci).mesh;
        bboxDiag = std::max(1e-9, double(mesh.bbox.Diag()));
        faceCount = mesh.FN();
        if (needSelectedFaces)
            selFaces = vcg::tri::UpdateSelection<VCGMesh>::FaceCount(mesh);
        const int selVerts = needSelectedVerts
            ? int(vcg::tri::UpdateSelection<VCGMesh>::VertexCount(mesh))
            : 0;
        hasSelectedFaces = (selFaces > 0);
        hasSelectedVerts = (selVerts > 0);
        const int ioMask = doc.mesh(ci).ioMask;
        if (needVertexQuality && (ioMask & vcg::tri::io::Mask::IOM_VERTQUALITY)
            && mesh.VN() > 0) {
            const auto vr = vcg::tri::Stat<VCGMesh>::ComputePerVertexQualityMinMax(mesh);
            qualityVMin = std::min(double(vr.first), double(vr.second));
            qualityVMax = std::max(double(vr.first), double(vr.second));
        }
        if (needFaceQuality && (ioMask & vcg::tri::io::Mask::IOM_FACEQUALITY)
            && mesh.FN() > 0) {
            const auto fr = vcg::tri::Stat<VCGMesh>::ComputePerFaceQualityMinMax(mesh);
            qualityFMin = std::min(double(fr.first), double(fr.second));
            qualityFMax = std::max(double(fr.first), double(fr.second));
        }
    }

    const unsigned int hw = std::thread::hardware_concurrency();
    const int hardwareThreads = static_cast<int>(hw > 0 ? hw : 8);
    const int currentMeshIndex = ci;
    int otherMeshIndex = currentMeshIndex;
    for (int i = 0; i < doc.meshCount(); ++i) {
        if (i != currentMeshIndex) {
            otherMeshIndex = i;
            break;
        }
    }

    for (MeshFilterDescriptor &fd : descriptors) {
        for (MeshFilterParameterDescriptor &p : fd.parameters) {
            p.defaultValue = resolveToken(p.defaultValue, bboxDiag,
                qualityVMin, qualityVMax, qualityFMin, qualityFMax,
                hasSelectedFaces, hasSelectedVerts, faceCount, selFaces, hardwareThreads,
                currentMeshIndex, otherMeshIndex);
            p.minValue = resolveToken(p.minValue, bboxDiag,
                qualityVMin, qualityVMax, qualityFMin, qualityFMax,
                hasSelectedFaces, hasSelectedVerts, faceCount, selFaces, hardwareThreads,
                currentMeshIndex, otherMeshIndex);
            p.maxValue = resolveToken(p.maxValue, bboxDiag,
                qualityVMin, qualityVMax, qualityFMin, qualityFMax,
                hasSelectedFaces, hasSelectedVerts, faceCount, selFaces, hardwareThreads,
                currentMeshIndex, otherMeshIndex);
        }
    }
}
