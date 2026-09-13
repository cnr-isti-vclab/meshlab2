#include "layerfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMatrix4x4>
#include <QVector4D>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {
constexpr QLatin1StringView kSplitFaces("extract_selected_faces");
constexpr QLatin1StringView kSplitVertices("extract_selected_vertices");
constexpr QLatin1StringView kSplitConnected("split_into_connected_components");
constexpr QLatin1StringView kDuplicate("duplicate_current_layer");
constexpr QLatin1StringView kDeleteCurrent("remove_current_mesh_layer");
constexpr QLatin1StringView kDeleteHidden("remove_hidden_mesh_layers");
constexpr QLatin1StringView kDeleteCurrentRaster("remove_current_raster");
constexpr QLatin1StringView kDeleteHiddenRasters("remove_hidden_rasters");
constexpr QLatin1StringView kFlatten("merge_visible_layers");
constexpr QLatin1StringView kRenameMesh("rename_current_mesh_layer");
constexpr QLatin1StringView kRenameRaster("rename_current_raster");
constexpr QLatin1StringView kExportRasterCameras("export_cameras_from_visible_rasters");
constexpr QLatin1StringView kImportRasterCameras("import_cameras_to_visible_rasters");
constexpr QLatin1StringView kRenderFromRenderStateJson("render_from_render_state_json");
using Mask = vcg::tri::io::Mask;
using Sel = vcg::tri::UpdateSelection<VCGMesh>;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

MeshFilterRunResult successInfo(bool modified, const QStringList &info = {}, const QVector<int> &newMeshes = {})
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = modified;
    result.infoMessages = info;
    result.newMeshIndices = newMeshes;
    return result;
}

template<typename Scalar>
vcg::Point3<Scalar> qMatrixMapPoint(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &point)
{
    const QVector4D mapped = matrix * QVector4D(point.X(), point.Y(), point.Z(), 1.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

bool isIdentityTransform(const QMatrix4x4 &matrix, float eps = 1e-6f)
{
    QMatrix4x4 identity;
    identity.setToIdentity();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (std::abs(matrix(r, c) - identity(r, c)) > eps)
                return false;
    return true;
}

void applyTransformToMesh(VCGMesh &mesh, const QMatrix4x4 &transform)
{
    if (isIdentityTransform(transform))
        return;
    for (VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        v.P() = qMatrixMapPoint<float>(transform, v.cP());
    }
}

void copyMaterialMetadata(const Document::MeshEntry &src, Document::MeshEntry &dst)
{
    dst.textureFileNames = src.textureFileNames;
    dst.textureFilePaths = src.textureFilePaths;
    dst.materialSet = src.materialSet;
    dst.ioMask |= (src.ioMask & (Mask::IOM_WEDGTEXCOORD | Mask::IOM_VERTTEXCOORD | Mask::IOM_VERTCOLOR | Mask::IOM_FACECOLOR | Mask::IOM_VERTQUALITY | Mask::IOM_FACEQUALITY));
}

void rebuildNormalsAndBounds(VCGMesh &mesh)
{
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    if (mesh.FN() > 0)
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

int addDerivedMesh(
    Document &doc,
    const VCGMesh &mesh,
    const QString &name,
    int ioMask,
    const Document::MeshEntry &sourceEntry,
    const QMatrix4x4 &transform)
{
    const int newIndex = doc.addMesh(mesh, name, ioMask);
    if (newIndex < 0)
        return newIndex;
    Document::MeshEntry &newEntry = doc.mesh(newIndex);
    newEntry.transform = transform;
    copyMaterialMetadata(sourceEntry, newEntry);
    return newIndex;
}

std::unique_ptr<VCGMesh> meshCopy(const VCGMesh &mesh)
{
    auto copy = std::make_unique<VCGMesh>();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(*copy, mesh);
    return copy;
}

int unionIoMask(const Document &doc, const std::vector<int> &indices)
{
    int mask = 0;
    for (int index : indices)
        mask |= doc.mesh(index).ioMask;
    return mask;
}

int findMeshIndexById(const Document &doc, std::uint64_t meshId)
{
    for (int i = 0; i < doc.meshCount(); ++i) {
        if (doc.mesh(i).meshId == meshId)
            return i;
    }
    return -1;
}

QString mergedLayerName()
{
    return QObject::tr("Merged Mesh");
}

bool mergeCameraStateIntoRenderState(
    const QString &cameraStateJson,
    const QString &renderStateJson,
    QString &outMergedRenderState,
    QString &errorMessage)
{
    QJsonParseError renderErr;
    const QJsonDocument renderDoc = QJsonDocument::fromJson(renderStateJson.toUtf8(), &renderErr);
    if (renderDoc.isNull() || !renderDoc.isObject()) {
        errorMessage = QObject::tr("Invalid render-state JSON: %1").arg(renderErr.errorString());
        return false;
    }
    QJsonObject renderRoot = renderDoc.object();

    QJsonParseError cameraErr;
    const QJsonDocument cameraDoc = QJsonDocument::fromJson(cameraStateJson.toUtf8(), &cameraErr);
    if (cameraDoc.isNull() || !cameraDoc.isObject()) {
        errorMessage = QObject::tr("Invalid camera-state JSON: %1").arg(cameraErr.errorString());
        return false;
    }
    const QJsonObject cameraRoot = cameraDoc.object();

    const QJsonObject cameraTrackball = cameraRoot.value(QStringLiteral("trackball")).toObject();
    if (!cameraTrackball.isEmpty()) {
        renderRoot.insert(QStringLiteral("trackball"), cameraTrackball);
    } else {
        renderRoot.insert(QStringLiteral("trackball"), cameraRoot);
    }

    outMergedRenderState = QString::fromUtf8(QJsonDocument(renderRoot).toJson(QJsonDocument::Compact));
    return true;
}

std::vector<int> visibleRasterIndices(const Document &doc)
{
    std::vector<int> indices;
    indices.reserve(size_t(doc.rasterCount()));
    for (int i = 0; i < doc.rasterCount(); ++i) {
        if (doc.raster(i).visible)
            indices.push_back(i);
    }
    return indices;
}

bool parseFloatList(const QString &text, int minCount, std::vector<float> &values)
{
    values.clear();
    const QStringList parts = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < minCount)
        return false;
    values.reserve(size_t(parts.size()));
    for (const QString &part : parts) {
        bool ok = false;
        const float value = part.toFloat(&ok);
        if (!ok)
            return false;
        values.push_back(value);
    }
    return true;
}

bool parseIntPair(const QString &text, int &a, int &b)
{
    const QStringList parts = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return false;
    bool ok0 = false;
    bool ok1 = false;
    a = parts.at(0).toInt(&ok0);
    b = parts.at(1).toInt(&ok1);
    return ok0 && ok1;
}

bool parseFloatPair(const QString &text, float &a, float &b)
{
    const QStringList parts = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return false;
    bool ok0 = false;
    bool ok1 = false;
    a = parts.at(0).toFloat(&ok0);
    b = parts.at(1).toFloat(&ok1);
    return ok0 && ok1;
}

bool parseVcgCamera(
    const QXmlStreamAttributes &attrs,
    CameraShot &outShot,
    QString &errorMessage)
{
    CameraShot::VcgShot shot;
    vcg::Camera<float> &cam = shot.Intrinsics;

    if (attrs.hasAttribute(QStringLiteral("CameraType"))) {
        bool ok = false;
        const int type = attrs.value(QStringLiteral("CameraType")).toInt(&ok);
        if (!ok) {
            errorMessage = QObject::tr("Invalid CameraType attribute.");
            return false;
        }
        cam.cameraType = static_cast<vcg::Camera<float>::CameraType>(type);
    }

    std::vector<float> translationValues;
    std::vector<float> rotationValues;
    if (!parseFloatList(attrs.value(QStringLiteral("TranslationVector")).toString(), 3, translationValues)
        || !parseFloatList(attrs.value(QStringLiteral("RotationMatrix")).toString(), 16, rotationValues)) {
        errorMessage = QObject::tr("Invalid camera extrinsics.");
        return false;
    }

    shot.Extrinsics.SetTra(vcg::Point3f(
        -translationValues[0],
        -translationValues[1],
        -translationValues[2]));
    vcg::Matrix44f rot;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            rot[row][col] = rotationValues[size_t(col + 4 * row)];
    shot.Extrinsics.SetRot(rot);

    bool ok = false;
    cam.FocalMm = attrs.value(QStringLiteral("FocalMm")).toFloat(&ok);
    if (!ok) {
        errorMessage = QObject::tr("Invalid FocalMm attribute.");
        return false;
    }

    int viewportW = 0;
    int viewportH = 0;
    float centerX = 0.0f;
    float centerY = 0.0f;
    float pixelX = 0.0f;
    float pixelY = 0.0f;
    float lens0 = 0.0f;
    float lens1 = 0.0f;
    if (!parseIntPair(attrs.value(QStringLiteral("ViewportPx")).toString(), viewportW, viewportH)
        || !parseFloatPair(attrs.value(QStringLiteral("CenterPx")).toString(), centerX, centerY)
        || !parseFloatPair(attrs.value(QStringLiteral("PixelSizeMm")).toString(), pixelX, pixelY)
        || !parseFloatPair(attrs.value(QStringLiteral("LensDistortion")).toString(), lens0, lens1)) {
        errorMessage = QObject::tr("Invalid camera intrinsics.");
        return false;
    }
    cam.ViewportPx = vcg::Point2i(viewportW, viewportH);
    cam.CenterPx = vcg::Point2f(centerX, centerY);
    cam.PixelSizeMm = vcg::Point2f(pixelX, pixelY);
    cam.k[0] = lens0;
    cam.k[1] = lens1;

    outShot = CameraShot::fromVcgShot(shot);
    if (!outShot.isValid()) {
        errorMessage = QObject::tr("Parsed VCGCamera is not valid.");
        return false;
    }
    return true;
}

void writeVcgCamera(QXmlStreamWriter &xml, const CameraShot &shot)
{
    xml.writeStartElement(QStringLiteral("VCGCamera"));
    if (shot.isValid()) {
        const CameraShot::VcgShot vcgShot = shot.toVcgShot();
        const vcg::Point3f tra = vcgShot.Extrinsics.Tra();
        const vcg::Matrix44f rot = vcgShot.Extrinsics.Rot();
        xml.writeAttribute(
            QStringLiteral("TranslationVector"),
            QStringLiteral("%1 %2 %3 1")
                .arg(-tra[0], 0, 'f', 6)
                .arg(-tra[1], 0, 'f', 6)
                .arg(-tra[2], 0, 'f', 6));
        xml.writeAttribute(
            QStringLiteral("RotationMatrix"),
            QStringLiteral("%1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11 %12 %13 %14 %15 %16")
                .arg(rot[0][0], 0, 'f', 6).arg(rot[0][1], 0, 'f', 6).arg(rot[0][2], 0, 'f', 6).arg(rot[0][3], 0, 'f', 6)
                .arg(rot[1][0], 0, 'f', 6).arg(rot[1][1], 0, 'f', 6).arg(rot[1][2], 0, 'f', 6).arg(rot[1][3], 0, 'f', 6)
                .arg(rot[2][0], 0, 'f', 6).arg(rot[2][1], 0, 'f', 6).arg(rot[2][2], 0, 'f', 6).arg(rot[2][3], 0, 'f', 6)
                .arg(rot[3][0], 0, 'f', 6).arg(rot[3][1], 0, 'f', 6).arg(rot[3][2], 0, 'f', 6).arg(rot[3][3], 0, 'f', 6));
        xml.writeAttribute(QStringLiteral("CameraType"), QString::number(int(vcgShot.Intrinsics.cameraType)));
        xml.writeAttribute(QStringLiteral("FocalMm"), QString::number(vcgShot.Intrinsics.FocalMm, 'f', 4));
        xml.writeAttribute(
            QStringLiteral("LensDistortion"),
            QStringLiteral("%1 %2").arg(vcgShot.Intrinsics.k[0], 0, 'f', 6).arg(vcgShot.Intrinsics.k[1], 0, 'f', 6));
        xml.writeAttribute(
            QStringLiteral("PixelSizeMm"),
            QStringLiteral("%1 %2")
                .arg(vcgShot.Intrinsics.PixelSizeMm[0], 0, 'f', 6)
                .arg(vcgShot.Intrinsics.PixelSizeMm[1], 0, 'f', 6));
        xml.writeAttribute(
            QStringLiteral("ViewportPx"),
            QStringLiteral("%1 %2")
                .arg(vcgShot.Intrinsics.ViewportPx[0])
                .arg(vcgShot.Intrinsics.ViewportPx[1]));
        xml.writeAttribute(
            QStringLiteral("CenterPx"),
            QStringLiteral("%1 %2")
                .arg(vcgShot.Intrinsics.CenterPx[0], 0, 'f', 2)
                .arg(vcgShot.Intrinsics.CenterPx[1], 0, 'f', 2));
    }
    xml.writeAttribute(QStringLiteral("BinaryData"), QStringLiteral("0"));
    xml.writeEndElement();
}

bool readVcgCameraFile(const QString &path, std::vector<CameraShot> &shots, QString &errorMessage)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMessage = QObject::tr("Cannot open camera file '%1'.").arg(path);
        return false;
    }

    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        if (xml.readNext() == QXmlStreamReader::StartElement
            && xml.name() == QStringLiteral("VCGCamera")) {
            CameraShot shot;
            QString cameraError;
            if (!parseVcgCamera(xml.attributes(), shot, cameraError)) {
                errorMessage = QObject::tr("Invalid VCGCamera in '%1': %2").arg(path, cameraError);
                return false;
            }
            shots.push_back(shot);
        }
    }

    if (xml.hasError()) {
        errorMessage = QObject::tr("XML parse error in '%1': %2").arg(path, xml.errorString());
        return false;
    }
    return true;
}

} // namespace

QString LayerFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.layer");
}

QString LayerFilterPlugin::name() const
{
    return QObject::tr("Layer Filters");
}

MeshFilterRunResult LayerFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kDeleteCurrentRaster)) {
        const int rasterIndex = doc.currentRasterIndex();
        if (rasterIndex < 0 || rasterIndex >= doc.rasterCount())
            return fail(QObject::tr("No current raster selected."));
        const QString name = doc.raster(rasterIndex).name;
        doc.removeRaster(rasterIndex);
        return successInfo(true, { QObject::tr("Deleted raster '%1'.").arg(name) });
    }

    if (filterId == QString::fromLatin1(kDeleteHiddenRasters)) {
        std::vector<int> toDelete;
        for (int i = 0; i < doc.rasterCount(); ++i) {
            if (!doc.raster(i).visible)
                toDelete.push_back(i);
        }
        if (toDelete.empty())
            return successInfo(false, { QObject::tr("No hidden raster layers to remove.") });

        std::sort(toDelete.rbegin(), toDelete.rend());
        for (int idx : toDelete)
            doc.removeRaster(idx);
        return successInfo(true, { QObject::tr("Removed %1 hidden raster layer(s).").arg(toDelete.size()) });
    }

    if (filterId == QString::fromLatin1(kRenameRaster)) {
        const int rasterIndex = doc.currentRasterIndex();
        if (rasterIndex < 0 || rasterIndex >= doc.rasterCount())
            return fail(QObject::tr("No current raster selected."));
        const QString newName = params.getString(QStringLiteral("newName")).trimmed();
        if (newName.isEmpty())
            return fail(QObject::tr("New raster name cannot be empty."));
        if (newName == doc.raster(rasterIndex).name)
            return successInfo(false, { QObject::tr("Raster is already named '%1'.").arg(newName) });
        doc.setRasterName(rasterIndex, newName);
        return successInfo(true, { QObject::tr("Renamed current raster to '%1'.").arg(newName) });
    }

    if (filterId == QString::fromLatin1(kExportRasterCameras)) {
        const QString path = params.getFileSave(QStringLiteral("camera_file")).trimmed();
        if (path.isEmpty())
            return fail(QObject::tr("No camera output file selected."));

        const std::vector<int> activeRasters = visibleRasterIndices(doc);
        if (activeRasters.empty())
            return fail(QObject::tr("No active raster layers to export."));

        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
            return fail(QObject::tr("Cannot write camera file '%1'.").arg(path));

        QXmlStreamWriter xml(&file);
        xml.setAutoFormatting(true);
        xml.writeStartDocument();
        xml.writeStartElement(QStringLiteral("QMeshLabRasterCameras"));
        for (int rasterIndex : activeRasters) {
            const Document::RasterEntry &raster = doc.raster(rasterIndex);
            xml.writeStartElement(QStringLiteral("RasterCamera"));
            xml.writeAttribute(QStringLiteral("label"), raster.name);
            xml.writeAttribute(QStringLiteral("index"), QString::number(rasterIndex));
            writeVcgCamera(xml, raster.shot);
            xml.writeEndElement();
        }
        xml.writeEndElement();
        xml.writeEndDocument();

        return successInfo(false, { QObject::tr("Exported %1 active raster camera(s) to '%2'.").arg(activeRasters.size()).arg(path) });
    }

    if (filterId == QString::fromLatin1(kImportRasterCameras)) {
        const QString path = params.getFileOpen(QStringLiteral("camera_file")).trimmed();
        if (path.isEmpty())
            return fail(QObject::tr("No camera input file selected."));

        std::vector<CameraShot> shots;
        QString parseError;
        if (!readVcgCameraFile(path, shots, parseError))
            return fail(parseError);
        if (shots.empty())
            return fail(QObject::tr("No VCGCamera entries found in '%1'.").arg(path));

        const std::vector<int> activeRasters = visibleRasterIndices(doc);
        if (activeRasters.empty())
            return fail(QObject::tr("No active raster layers to update."));
        if (shots.size() != activeRasters.size()) {
            return fail(QObject::tr("Camera count mismatch: file contains %1 camera(s), but %2 raster layer(s) are active.")
                .arg(shots.size())
                .arg(activeRasters.size()));
        }

        for (size_t i = 0; i < shots.size(); ++i) {
            doc.setRasterShot(
                activeRasters[i],
                shots[i],
                QObject::tr("Imported raster camera for '%1'").arg(doc.raster(activeRasters[i]).name));
        }
        return successInfo(true, { QObject::tr("Imported %1 raster camera(s) from '%2'.").arg(shots.size()).arg(path) });
    }

    if (filterId == QString::fromLatin1(kRenderFromRenderStateJson)) {
        QString renderStateJson = params.getRenderState(QStringLiteral("render_state")).trimmed();
        const QString cameraStateJson = params.getCameraState(QStringLiteral("camera_state")).trimmed();
        if (renderStateJson.isEmpty()) {
            return fail(QObject::tr("No render-state JSON provided."));
        }
        if (cameraStateJson.isEmpty()) {
            return fail(QObject::tr("No camera-state JSON provided."));
        }

        QString mergedRenderStateJson;
        QString mergeError;
        if (!mergeCameraStateIntoRenderState(cameraStateJson, renderStateJson, mergedRenderStateJson, mergeError)) {
            return fail(QObject::tr("Render-state preparation failed: %1").arg(mergeError));
        }

        const int w = std::max(0, params.getInt(QStringLiteral("output_width"), 0));
        const int h = std::max(0, params.getInt(QStringLiteral("output_height"), 0));
        const QSize targetSize = (w > 0 && h > 0) ? QSize(w, h) : QSize();

        QImage snapshot;
        CameraShot shot;
        QString captureError;
        if (!doc.renderSnapshotFromStateJson(mergedRenderStateJson, targetSize, snapshot, shot, &captureError)) {
            return fail(QObject::tr("Render from state JSON failed: %1").arg(captureError));
        }
        if (snapshot.isNull())
            return fail(QObject::tr("Render from state JSON produced an empty image."));

        bool modified = false;
        QStringList info;

        const QString savePath = params.getFileSave(QStringLiteral("save_png_path")).trimmed();
        if (!savePath.isEmpty()) {
            if (!snapshot.save(savePath, "PNG")) {
                return fail(QObject::tr("Failed to save PNG snapshot to '%1'.").arg(savePath));
            }
            info << QObject::tr("Saved PNG snapshot: %1").arg(savePath);
        }

        if (params.getBool(QStringLiteral("add_as_raster"), true)) {
            const QString requestedName = params.getString(QStringLiteral("raster_name")).trimmed();
            const QString rasterName = requestedName.isEmpty()
                ? QObject::tr("Programmatic Render")
                : requestedName;
            const QString rasterSourcePath = savePath.isEmpty() ? QString() : QFileInfo(savePath).absoluteFilePath();
            const int rasterIndex = doc.addRasterImage(snapshot, rasterName, rasterSourcePath, shot);
            if (rasterIndex < 0)
                return fail(QObject::tr("Failed to add rendered snapshot as raster layer."));
            modified = true;
            info << QObject::tr("Added raster layer '%1'.").arg(doc.raster(rasterIndex).name);
        }

        if (savePath.isEmpty() && !params.getBool(QStringLiteral("add_as_raster"), true)) {
            info << QObject::tr("Snapshot rendered successfully (no output target selected).");
        }

        return successInfo(modified, info);
    }

    const int currentIndex = doc.currentMeshIndex();

    if (filterId == QString::fromLatin1(kDeleteHidden)) {
        std::vector<int> toDelete;
        for (int i = 0; i < doc.meshCount(); ++i) {
            if (!doc.mesh(i).visible)
                toDelete.push_back(i);
        }
        if (toDelete.empty())
            return successInfo(false, { QObject::tr("No non-visible mesh layers to delete.") });

        std::sort(toDelete.rbegin(), toDelete.rend());
        for (int idx : toDelete)
            doc.removeMesh(idx);
        return successInfo(true, { QObject::tr("Deleted %1 non-visible mesh layer(s).").arg(toDelete.size()) });
    }

    if (currentIndex < 0 || currentIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &entry = doc.mesh(currentIndex);

    if (filterId == QString::fromLatin1(kRenameMesh)) {
        const QString newName = params.getString(QStringLiteral("newName")).trimmed();
        if (newName.isEmpty())
            return fail(QObject::tr("New mesh name cannot be empty."));
        if (newName == entry.name)
            return successInfo(false, { QObject::tr("Mesh is already named '%1'.").arg(newName) });
        doc.setMeshName(currentIndex, newName);
        return successInfo(true, { QObject::tr("Renamed current mesh to '%1'.").arg(newName) });
    }

    if (filterId == QString::fromLatin1(kDeleteCurrent)) {
        const QString name = entry.name;
        doc.removeMesh(currentIndex);
        return successInfo(true, { QObject::tr("Deleted mesh '%1'.").arg(name) });
    }

    if (filterId == QString::fromLatin1(kDuplicate)) {
        const int newIndex = doc.duplicateMesh(currentIndex);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to duplicate the current mesh."));
        return successInfo(true,
            { QObject::tr("Duplicated current mesh as '%1'.").arg(doc.mesh(newIndex).name) },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kSplitVertices)) {
        const int selectedVerts = Sel::VertexCount(entry.mesh);
        if (selectedVerts <= 0)
            return fail(QObject::tr("No selected vertices found on current mesh."));

        auto subsetSource = meshCopy(entry.mesh);
        Sel::FaceClear(*subsetSource);
        VCGMesh subsetMesh;
        vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(subsetMesh, *subsetSource, true);
        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(subsetMesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(subsetMesh);
        const int newIndex = addDerivedMesh(
            doc,
            subsetMesh,
            QObject::tr("SelectedVerticesSubset"),
            entry.ioMask,
            entry,
            entry.transform);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to create the destination layer."));

        const bool deleteOriginal = params.getBool(QStringLiteral("DeleteOriginal"), true);
        if (deleteOriginal) {
            Sel::FaceFromVertexLoose(entry.mesh);
            for (VCGFace &f : entry.mesh.face)
                if (!f.IsD() && f.IsS())
                    vcg::tri::Allocator<VCGMesh>::DeleteFace(entry.mesh, f);
            for (VCGVertex &v : entry.mesh.vert)
                if (!v.IsD() && v.IsS())
                    vcg::tri::Allocator<VCGMesh>::DeleteVertex(entry.mesh, v);
            Sel::VertexClear(entry.mesh);
            Sel::FaceClear(entry.mesh);
            vcg::tri::Allocator<VCGMesh>::CompactEveryVector(entry.mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(entry.mesh);
            if (entry.mesh.FN() > 0)
                vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry.mesh);
            doc.markMeshGeometryChanged(currentIndex, QObject::tr("Moved selected vertices from '%1' into a new layer").arg(entry.name));
        }

        return successInfo(true,
            {
                deleteOriginal
                    ? QObject::tr("Moved %1 selected vertices into '%2'.").arg(selectedVerts).arg(doc.mesh(newIndex).name)
                    : QObject::tr("Copied %1 selected vertices into '%2'.").arg(selectedVerts).arg(doc.mesh(newIndex).name)
            },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kSplitFaces)) {
        const int selectedFaces = Sel::FaceCount(entry.mesh);
        if (selectedFaces <= 0)
            return fail(QObject::tr("No selected faces found on current mesh."));

        auto subsetSource = meshCopy(entry.mesh);
        Sel::VertexFromFaceLoose(*subsetSource);
        const int selectedVerts = Sel::VertexCount(*subsetSource);
        VCGMesh subsetMesh;
        vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(subsetMesh, *subsetSource, true);
        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(subsetMesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(subsetMesh);
        if (subsetMesh.FN() > 0)
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(subsetMesh);
        const int newIndex = addDerivedMesh(
            doc,
            subsetMesh,
            QObject::tr("SelectedFacesSubset"),
            entry.ioMask,
            entry,
            entry.transform);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to create the destination layer."));

        const bool deleteOriginal = params.getBool(QStringLiteral("DeleteOriginal"), true);
        if (deleteOriginal) {
            Sel::VertexClear(entry.mesh);
            Sel::VertexFromFaceStrict(entry.mesh);
            for (VCGFace &f : entry.mesh.face)
                if (!f.IsD() && f.IsS())
                    vcg::tri::Allocator<VCGMesh>::DeleteFace(entry.mesh, f);
            for (VCGVertex &v : entry.mesh.vert)
                if (!v.IsD() && v.IsS())
                    vcg::tri::Allocator<VCGMesh>::DeleteVertex(entry.mesh, v);
            Sel::VertexClear(entry.mesh);
            Sel::FaceClear(entry.mesh);
            vcg::tri::Allocator<VCGMesh>::CompactEveryVector(entry.mesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(entry.mesh);
            if (entry.mesh.FN() > 0)
                vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(entry.mesh);
            doc.markMeshGeometryChanged(currentIndex, QObject::tr("Moved selected faces from '%1' into a new layer").arg(entry.name));
        }

        return successInfo(true,
            {
                deleteOriginal
                    ? QObject::tr("Moved %1 faces and %2 vertices into '%3'.").arg(selectedFaces).arg(selectedVerts).arg(doc.mesh(newIndex).name)
                    : QObject::tr("Copied %1 faces and %2 vertices into '%3'.").arg(selectedFaces).arg(selectedVerts).arg(doc.mesh(newIndex).name)
            },
            { newIndex });
    }

    if (filterId == QString::fromLatin1(kSplitConnected)) {
        if (entry.mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh has no faces."));

        // FF adjacency is already built by the framework via inputPrepare.
        std::vector<std::pair<int, VCGFace::FacePointer>> connectedCompVec;
        const int numCC = vcg::tri::Clean<VCGMesh>::ConnectedComponents(entry.mesh, connectedCompVec);
        Sel::FaceClear(entry.mesh);
        Sel::VertexClear(entry.mesh);

        QVector<int> newIndices;
        std::vector<std::uint64_t> newMeshIds;
        newIndices.reserve(int(connectedCompVec.size()));
        newMeshIds.reserve(connectedCompVec.size());
        for (size_t i = 0; i < connectedCompVec.size(); ++i) {
            connectedCompVec[i].second->SetS();
            vcg::tri::UpdateSelection<VCGMesh>::FaceConnectedFF(entry.mesh);
            vcg::tri::UpdateSelection<VCGMesh>::VertexFromFaceLoose(entry.mesh);
            VCGMesh componentMesh;
            if (entry.mesh.vert.IsTexCoordEnabled())
                componentMesh.vert.EnableTexCoord();
            if (entry.mesh.vert.IsCurvatureDirEnabled())
                componentMesh.vert.EnableCurvatureDir();
            if (entry.mesh.face.IsWedgeTexCoordEnabled())
                componentMesh.face.EnableWedgeTexCoord();
            vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(componentMesh, entry.mesh, true);
            vcg::tri::Allocator<VCGMesh>::CompactEveryVector(componentMesh);
            vcg::tri::UpdateBounding<VCGMesh>::Box(componentMesh);
            if (componentMesh.FN() > 0)
                vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(componentMesh);
            const int newIndex = addDerivedMesh(
                doc,
                componentMesh,
                QObject::tr("CC %1").arg(i),
                entry.ioMask,
                entry,
                entry.transform);
            if (newIndex < 0)
                return fail(QObject::tr("Failed to create connected component layer %1.").arg(i));
            newIndices.push_back(newIndex);
            newMeshIds.push_back(doc.mesh(newIndex).meshId);
            Sel::FaceClear(entry.mesh);
            Sel::VertexClear(entry.mesh);
        }

        const bool deleteSourceMesh = params.getBool(QStringLiteral("delete_source_mesh"), false);
        if (deleteSourceMesh)
            doc.removeMesh(currentIndex);

        if (deleteSourceMesh) {
            newIndices.clear();
            for (std::uint64_t meshId : newMeshIds) {
                const int idx = findMeshIndexById(doc, meshId);
                if (idx >= 0)
                    newIndices.push_back(idx);
            }
        }

        return successInfo(true,
            { QObject::tr("Split current mesh into %1 connected component layer(s).").arg(numCC) },
            newIndices);
    }

    if (filterId == QString::fromLatin1(kFlatten)) {
        const bool mergeVisible = params.getBool(QStringLiteral("MergeVisible"), true);
        const bool deleteLayer = params.getBool(QStringLiteral("DeleteLayer"), true);
        const bool mergeVertices = params.getBool(QStringLiteral("MergeVertices"), true);
        const bool alsoUnreferenced = params.getBool(QStringLiteral("AlsoUnreferenced"), true);

        std::vector<int> sourceIndices;
        for (int i = 0; i < doc.meshCount(); ++i) {
            if (doc.mesh(i).visible || !mergeVisible)
                sourceIndices.push_back(i);
        }
        if (sourceIndices.empty())
            return fail(QObject::tr("No source layers available for flattening."));

        VCGMesh mergedMesh;
        for (int index : sourceIndices) {
            auto part = meshCopy(doc.mesh(index).mesh);
            if (!alsoUnreferenced)
                vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(*part);
            applyTransformToMesh(*part, doc.mesh(index).transform);
            vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(mergedMesh, *part, false);
        }

        if (mergeVertices)
            vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(mergedMesh);
        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mergedMesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(mergedMesh);
        if (mergedMesh.FN() > 0)
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mergedMesh);

        const int mergedMask = unionIoMask(doc, sourceIndices);
        const int newIndex = doc.addMesh(mergedMesh, mergedLayerName(), mergedMask);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to create merged layer."));
        const std::uint64_t mergedMeshId = doc.mesh(newIndex).meshId;
        const QString mergedName = doc.mesh(newIndex).name;
        doc.mesh(newIndex).transform.setToIdentity();

        if (deleteLayer) {
            std::sort(sourceIndices.rbegin(), sourceIndices.rend());
            for (int index : sourceIndices)
                if (index >= 0 && index < doc.meshCount() && index != newIndex)
                    doc.removeMesh(index);
        }

        const int finalMergedIndex = findMeshIndexById(doc, mergedMeshId);
        QVector<int> resultNewIndices;
        if (finalMergedIndex >= 0)
            resultNewIndices.push_back(finalMergedIndex);

        return successInfo(true,
            {
                QObject::tr("Merged %1 layer(s) into '%2'.").arg(sourceIndices.size()).arg(mergedName),
                mergeVertices ? QObject::tr("Duplicate vertices were merged.") : QObject::tr("Duplicate vertices were kept.")
            },
            resultNewIndices);
    }

    return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
}

void registerLayerFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<LayerFilterPlugin>());
}
