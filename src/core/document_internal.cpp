#include "document_internal.h"

namespace DocumentInternal {

Document *g_callbackDocument = nullptr;

QString summarizeLoadMask(int mask)
{
    using Mask = vcg::tri::io::Mask;

    QStringList attrs;
    auto addIf = [&attrs, mask](int bit, const QString &name) {
        if ((mask & bit) != 0)
            attrs.append(name);
    };

    addIf(Mask::IOM_VERTCOLOR, QStringLiteral("vertex color"));
    addIf(Mask::IOM_EDGECOLOR, QStringLiteral("edge color"));
    addIf(Mask::IOM_FACECOLOR, QStringLiteral("face color"));
    addIf(Mask::IOM_VERTNORMAL, QStringLiteral("vertex normal"));
    addIf(Mask::IOM_FACENORMAL, QStringLiteral("face normal"));
    addIf(Mask::IOM_VERTTEXCOORD, QStringLiteral("vertex texcoord"));
    addIf(Mask::IOM_WEDGTEXCOORD, QStringLiteral("wedge texcoord"));
    addIf(Mask::IOM_WEDGTEXMULTI, QStringLiteral("multi texture index"));
    addIf(Mask::IOM_WEDGCOLOR, QStringLiteral("wedge color"));
    addIf(Mask::IOM_WEDGNORMAL, QStringLiteral("wedge normal"));
    addIf(Mask::IOM_VERTQUALITY, QStringLiteral("vertex quality"));
    addIf(Mask::IOM_FACEQUALITY, QStringLiteral("face quality"));
    addIf(Mask::IOM_VERTRADIUS, QStringLiteral("vertex radius"));
    addIf(Mask::IOM_EDGEINDEX, QStringLiteral("edge index"));
    addIf(Mask::IOM_BITPOLYGONAL, QStringLiteral("polygonal faces"));
    addIf(Mask::IOM_CAMERA, QStringLiteral("camera"));

    if (attrs.isEmpty())
        return QObject::tr("none");
    return attrs.join(QStringLiteral(", "));
}

bool isMeshLabProjectExtension(const QString &path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("mlp"), Qt::CaseInsensitive) == 0;
}

QString resolveProjectEntryPath(const QString &projectFilePath, const QString &entryPath)
{
    const QString normalized = QDir::fromNativeSeparators(entryPath.trimmed());
    if (normalized.isEmpty())
        return {};
    const QFileInfo info(normalized);
    if (info.isAbsolute())
        return info.absoluteFilePath();
    return QFileInfo(projectFilePath).dir().filePath(normalized);
}

bool parseFloatList(const QString &text, int expectedCount, std::vector<float> &values)
{
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < expectedCount)
        return false;
    values.clear();
    values.reserve(size_t(expectedCount));
    for (int i = 0; i < expectedCount; ++i) {
        bool ok = false;
        const float value = parts.at(i).toFloat(&ok);
        if (!ok)
            return false;
        values.push_back(value);
    }
    return true;
}

QMatrix4x4 meshLabProjectMatrixToQt(const std::vector<float> &values)
{
    return QMatrix4x4(
        values[0], values[1], values[2], values[3],
        values[4], values[5], values[6], values[7],
        values[8], values[9], values[10], values[11],
        values[12], values[13], values[14], values[15]);
}

bool parseIntPair(const QString &text, int &a, int &b)
{
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
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
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < 2)
        return false;
    bool ok0 = false;
    bool ok1 = false;
    a = parts.at(0).toFloat(&ok0);
    b = parts.at(1).toFloat(&ok1);
    return ok0 && ok1;
}

RasterPlaneSemantic rasterPlaneSemanticFromProject(const QString &semanticText)
{
    const QString semantic = semanticText.trimmed().toLower();
    if (semantic.isEmpty() || semantic == QStringLiteral("rgba") || semantic == QStringLiteral("rgb")
        || semantic == QStringLiteral("image") || semantic == QStringLiteral("color")) {
        return RasterPlaneSemantic::RGBA;
    }
    if (semantic == QStringLiteral("mask") || semantic == QStringLiteral("maskuint8"))
        return RasterPlaneSemantic::MaskUInt8;
    if (semantic == QStringLiteral("maskfloat"))
        return RasterPlaneSemantic::MaskFloat;
    if (semantic == QStringLiteral("depth") || semantic == QStringLiteral("depthfloat"))
        return RasterPlaneSemantic::DepthFloat;
    return RasterPlaneSemantic::RGBA;
}

bool parseMeshLabProjectCamera(
    const QXmlStreamAttributes &attrs,
    CameraShot &outShot,
    QString *errorMessage = nullptr)
{
    CameraShot::VcgShot shot;
    vcg::Camera<float> &cam = shot.Intrinsics;

    if (attrs.hasAttribute(QStringLiteral("CameraType"))) {
        bool ok = false;
        const int type = attrs.value(QStringLiteral("CameraType")).toInt(&ok);
        if (!ok) {
            if (errorMessage)
                *errorMessage = QObject::tr("Invalid CameraType attribute");
            return false;
        }
        cam.cameraType = static_cast<vcg::Camera<float>::CameraType>(type);
    }

    const bool binaryData =
        attrs.value(QStringLiteral("BinaryData")).trimmed() == QStringLiteral("1");

    if (binaryData) {
        const QByteArray traBytes = QByteArray::fromBase64(
            attrs.value(QStringLiteral("TranslationVector")).toLocal8Bit());
        const QByteArray rotBytes = QByteArray::fromBase64(
            attrs.value(QStringLiteral("RotationMatrix")).toLocal8Bit());
        const QByteArray focalBytes = QByteArray::fromBase64(
            attrs.value(QStringLiteral("FocalMm")).toLocal8Bit());
        const QByteArray viewportBytes = QByteArray::fromBase64(
            attrs.value(QStringLiteral("ViewportPx")).toLocal8Bit());
        const QByteArray centerBytes = QByteArray::fromBase64(
            attrs.value(QStringLiteral("CenterPx")).toLocal8Bit());
        const QByteArray pixelBytes = QByteArray::fromBase64(
            attrs.value(QStringLiteral("PixelSizeMm")).toLocal8Bit());
        const QByteArray lensBytes = QByteArray::fromBase64(
            attrs.value(QStringLiteral("LensDistortion")).toLocal8Bit());

        if (traBytes.size() < int(sizeof(float) * 3)
            || rotBytes.size() < int(sizeof(float) * 16)
            || focalBytes.size() < int(sizeof(float))
            || viewportBytes.size() < int(sizeof(int) * 2)
            || centerBytes.size() < int(sizeof(float) * 2)
            || pixelBytes.size() < int(sizeof(float) * 2)
            || lensBytes.size() < int(sizeof(float) * 2)) {
            if (errorMessage)
                *errorMessage = QObject::tr("Incomplete binary VCGCamera payload");
            return false;
        }

        vcg::Point3f tra;
        memcpy(tra.V(), traBytes.constData(), sizeof(float) * 3);
        shot.Extrinsics.SetTra(-tra);

        vcg::Matrix44f rot;
        memcpy(rot.V(), rotBytes.constData(), sizeof(float) * 16);
        shot.Extrinsics.SetRot(rot);

        memcpy(&cam.FocalMm, focalBytes.constData(), sizeof(float));
        memcpy(&cam.ViewportPx, viewportBytes.constData(), sizeof(int) * 2);
        memcpy(&cam.CenterPx, centerBytes.constData(), sizeof(float) * 2);
        memcpy(&cam.PixelSizeMm, pixelBytes.constData(), sizeof(float) * 2);
        memcpy(&cam.k[0], lensBytes.constData(), sizeof(float) * 2);
    } else {
        std::vector<float> translationValues;
        std::vector<float> rotationValues;
        if (!parseFloatList(attrs.value(QStringLiteral("TranslationVector")).toString(), 3, translationValues)
            || !parseFloatList(attrs.value(QStringLiteral("RotationMatrix")).toString(), 16, rotationValues)) {
            if (errorMessage)
                *errorMessage = QObject::tr("Invalid camera extrinsics");
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
            if (errorMessage)
                *errorMessage = QObject::tr("Invalid FocalMm attribute");
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
            if (errorMessage)
                *errorMessage = QObject::tr("Invalid camera intrinsics");
            return false;
        }
        cam.ViewportPx = vcg::Point2i(viewportW, viewportH);
        cam.CenterPx = vcg::Point2f(centerX, centerY);
        cam.PixelSizeMm = vcg::Point2f(pixelX, pixelY);
        cam.k[0] = lens0;
        cam.k[1] = lens1;
    }

    outShot = CameraShot::fromVcgShot(shot);
    if (!outShot.isValid()) {
        if (errorMessage)
            *errorMessage = QObject::tr("Parsed VCGCamera is not valid");
        return false;
    }
    return true;
}

bool parseMeshLabProjectFile(
    const QString &filename,
    std::vector<MeshLabProjectMeshEntry> &meshes,
    std::vector<MeshLabProjectRasterEntry> &rasters,
    QString &errorMessage)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        errorMessage = QObject::tr("Cannot open project file '%1'").arg(filename);
        return false;
    }

    QXmlStreamReader xml(&file);
    MeshLabProjectMeshEntry *currentMesh = nullptr;
    MeshLabProjectRasterEntry *currentRaster = nullptr;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("MLMesh")) {
                MeshLabProjectMeshEntry mesh;
                const auto attrs = xml.attributes();
                mesh.label = attrs.value(QStringLiteral("label")).toString().trimmed();
                mesh.sourcePath = resolveProjectEntryPath(
                    filename,
                    attrs.value(QStringLiteral("filename")).toString());
                meshes.push_back(std::move(mesh));
                currentMesh = &meshes.back();
            } else if (name == QStringLiteral("MLMatrix44")) {
                if (currentMesh) {
                    std::vector<float> values;
                    const QString matrixText = xml.readElementText(QXmlStreamReader::SkipChildElements);
                    if (!parseFloatList(matrixText, 16, values)) {
                        errorMessage = QObject::tr("Invalid MLMatrix44 in project '%1'").arg(filename);
                        return false;
                    }
                    currentMesh->transform = meshLabProjectMatrixToQt(values);
                    currentMesh->hasTransform = true;
                }
            } else if (name == QStringLiteral("MLRaster")) {
                MeshLabProjectRasterEntry raster;
                raster.label = xml.attributes().value(QStringLiteral("label")).toString().trimmed();
                rasters.push_back(std::move(raster));
                currentRaster = &rasters.back();
            } else if (name == QStringLiteral("VCGCamera")) {
                if (currentRaster) {
                    QString cameraError;
                    if (!parseMeshLabProjectCamera(xml.attributes(), currentRaster->shot, &cameraError)) {
                        errorMessage =
                            QObject::tr("Invalid VCGCamera in project '%1': %2")
                                .arg(filename, cameraError);
                        return false;
                    }
                }
            } else if (name == QStringLiteral("Plane")) {
                if (currentRaster) {
                    MeshLabProjectPlaneEntry plane;
                    const auto attrs = xml.attributes();
                    plane.name = attrs.value(QStringLiteral("fileName")).toString().trimmed();
                    plane.sourcePath = resolveProjectEntryPath(
                        filename,
                        attrs.value(QStringLiteral("fileName")).toString());
                    plane.semantic =
                        rasterPlaneSemanticFromProject(attrs.value(QStringLiteral("semantic")).toString());
                    currentRaster->planes.push_back(std::move(plane));
                }
            }
        } else if (token == QXmlStreamReader::EndElement) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("MLMesh"))
                currentMesh = nullptr;
            else if (name == QStringLiteral("MLRaster"))
                currentRaster = nullptr;
        }
    }

    if (xml.hasError()) {
        errorMessage = QObject::tr("XML parse error in '%1': %2").arg(filename, xml.errorString());
        return false;
    }
    return true;
}

QString resolveTexturePath(const QString &meshFilePath, const QString &declaredTextureName)
{
    const QString normalizedName = QDir::fromNativeSeparators(declaredTextureName);
    QFileInfo textureInfo(normalizedName);
    if (textureInfo.isAbsolute())
        return textureInfo.absoluteFilePath();

    const QFileInfo meshInfo(meshFilePath);
    return meshInfo.dir().filePath(normalizedName);
}

MeshIOMaterialTextureRef normalizeMaterialTextureRef(
    const QString &meshFilePath,
    const MeshIOMaterialTextureRef &src)
{
    MeshIOMaterialTextureRef dst = src;
    const QString name = src.fileName.trimmed();
    const QString path = src.filePath.trimmed();
    if (src.assetIndex >= 0 && path.isEmpty()) {
        dst.fileName = name;
        dst.filePath.clear();
        return dst;
    }
    const QString source = !path.isEmpty() ? path : name;
    if (!source.isEmpty()) {
        if (meshFilePath.trimmed().isEmpty())
            dst.filePath = QDir::fromNativeSeparators(source);
        else
            dst.filePath = resolveTexturePath(meshFilePath, source);
        if (dst.fileName.trimmed().isEmpty())
            dst.fileName = QFileInfo(source).fileName();
    } else {
        dst.filePath.clear();
        if (dst.fileName.trimmed().isEmpty())
            dst.fileName.clear();
    }
    return dst;
}

MeshIOMaterialSet normalizeMaterialSet(
    const QString &meshFilePath,
    const MeshIOMaterialSet &src,
    const VCGMesh &mesh)
{
    MeshIOMaterialSet dst;
    dst.entries.reserve(src.entries.size());
    for (size_t i = 0; i < src.entries.size(); ++i) {
        MeshIOMaterialSlot slot = src.entries[i];
        if (slot.name.trimmed().isEmpty())
            slot.name = QObject::tr("Material %1").arg(i + 1);
        slot.baseColorTexture = normalizeMaterialTextureRef(meshFilePath, slot.baseColorTexture);
        slot.normalTexture = normalizeMaterialTextureRef(meshFilePath, slot.normalTexture);
        slot.occlusionTexture = normalizeMaterialTextureRef(meshFilePath, slot.occlusionTexture);
        slot.roughnessTexture = normalizeMaterialTextureRef(meshFilePath, slot.roughnessTexture);
        dst.entries.push_back(std::move(slot));
    }

    if (dst.entries.empty() && !mesh.textures.empty()) {
        dst.entries.reserve(mesh.textures.size());
        for (size_t i = 0; i < mesh.textures.size(); ++i) {
            const QString texName = QString::fromStdString(mesh.textures[i]).trimmed();
            if (texName.isEmpty())
                continue;
            MeshIOMaterialSlot slot;
            slot.name = QObject::tr("Material %1").arg(i + 1);
            slot.baseColorTexture.fileName = QFileInfo(texName).fileName();
            slot.baseColorTexture.filePath = resolveTexturePath(meshFilePath, texName);
            dst.entries.push_back(std::move(slot));
        }
    }

    return dst;
}

std::vector<MeshIOTextureAsset> buildTextureAssetsFromLegacyAssociation(
    const QStringList &textureFileNames,
    const QStringList &textureFilePaths,
    const std::vector<std::string> &meshTextures,
    const std::vector<MeshIOTextureAsset> &importedAssets)
{
    int count = std::max(textureFileNames.size(), textureFilePaths.size());
    count = std::max(count, int(meshTextures.size()));
    count = std::max(count, int(importedAssets.size()));

    std::vector<MeshIOTextureAsset> assets;
    assets.reserve(std::max(0, count));
    for (int i = 0; i < count; ++i) {
        MeshIOTextureAsset asset = i < int(importedAssets.size())
            ? importedAssets[size_t(i)]
            : MeshIOTextureAsset{};
        if (asset.name.trimmed().isEmpty() && i < textureFileNames.size())
            asset.name = textureFileNames.at(i).trimmed();
        if (!asset.hasImage() && asset.sourcePath.trimmed().isEmpty() && i < textureFilePaths.size())
            asset.sourcePath = QDir::toNativeSeparators(textureFilePaths.at(i).trimmed());
        if (!asset.hasImage() && asset.sourcePath.isEmpty() && i >= 0 && i < int(meshTextures.size()))
            asset.sourcePath = QDir::toNativeSeparators(QString::fromStdString(meshTextures[size_t(i)]).trimmed());
        if (asset.name.isEmpty() && !asset.sourcePath.isEmpty())
            asset.name = QFileInfo(asset.sourcePath).fileName();
        assets.push_back(std::move(asset));
    }
    return assets;
}

void syncTextureAssetsFromLegacyAssociation(Document::MeshEntry &entry)
{
    entry.textureAssets = buildTextureAssetsFromLegacyAssociation(
        entry.textureFileNames,
        entry.textureFilePaths,
        entry.mesh.textures);
}

QSize rasterPlaneStorageSize(const RasterPlane &plane)
{
    if (!plane.image.isNull())
        return plane.image.size();
    return plane.size;
}

QString rasterPlaneFallbackName(const RasterPlane &plane, int planeIndex)
{
    const QString explicitName = plane.name.trimmed();
    if (!explicitName.isEmpty())
        return explicitName;
    const QString sourcePath = plane.sourcePath.trimmed();
    if (!sourcePath.isEmpty())
        return QFileInfo(sourcePath).fileName();
    return QObject::tr("Plane %1").arg(planeIndex + 1);
}

QString rasterEntryDisplayName(const Document::RasterEntry &entry, int fallbackIndex)
{
    const QString explicitName = entry.name.trimmed();
    if (!explicitName.isEmpty())
        return explicitName;
    const QString sourcePath = entry.sourcePath.trimmed();
    if (!sourcePath.isEmpty())
        return QFileInfo(sourcePath).fileName();
    if (const RasterPlane *plane = entry.currentPlane()) {
        const QString planeName = rasterPlaneFallbackName(*plane, entry.currentPlaneIndex);
        if (!planeName.trimmed().isEmpty())
            return planeName;
    }
    return QObject::tr("Raster %1").arg(fallbackIndex + 1);
}

void normalizeRasterEntry(Document::RasterEntry &entry, int fallbackIndex)
{
    if (entry.currentPlaneIndex < 0 || entry.currentPlaneIndex >= int(entry.planes.size()))
        entry.currentPlaneIndex = entry.planes.empty() ? -1 : 0;

    entry.name = rasterEntryDisplayName(entry, fallbackIndex);
    if (entry.sourcePath.trimmed().isEmpty()) {
        if (const RasterPlane *plane = entry.currentPlane())
            entry.sourcePath = plane->sourcePath.trimmed();
    }

    for (int i = 0; i < int(entry.planes.size()); ++i) {
        RasterPlane &plane = entry.planes[size_t(i)];
        plane.name = rasterPlaneFallbackName(plane, i);
        plane.size = rasterPlaneStorageSize(plane);
    }
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
}

void copyMeshEntryMetadata(const Document::MeshEntry &src, Document::MeshEntry &dst)
{
    dst.meshId = src.meshId;
    dst.geometryRevision = src.geometryRevision;
    dst.materialRevision = src.materialRevision;
    dst.transform = src.transform;
    dst.name = src.name;
    dst.sourcePath = src.sourcePath;
    dst.textureFileNames = src.textureFileNames;
    dst.textureFilePaths = src.textureFilePaths;
    dst.textureAssets = src.textureAssets;
    dst.materialSet = src.materialSet;
    dst.visible = src.visible;
    dst.modified = src.modified;
    dst.ioMask = src.ioMask;
    dst.polygonFaceCount = src.polygonFaceCount;
    // Shared, not copied: the payloads are immutable, so a duplicate and its original
    // pointing at one instance cannot interfere. The copy is O(entries).
    dst.pluginData = src.pluginData;
}

void deepCopyMesh(const VCGMesh &src, VCGMesh &dst)
{
    dst.Clear();

    // Enable storable OCF fields in dst to match src.
    // Ancillary fields (FFAdj, VFAdj, Mark) are never copied — they are
    // transient and must be re-computed after use.
    const bool copyVertTexCoord   = src.vert.IsTexCoordEnabled();
    const bool copyVertCurvDir    = src.vert.IsCurvatureDirEnabled();
    const bool copyFaceWedgeTex   = src.face.IsWedgeTexCoordEnabled();
    if (copyVertTexCoord)  dst.vert.EnableTexCoord();
    if (copyVertCurvDir)   dst.vert.EnableCurvatureDir();
    if (copyFaceWedgeTex)  dst.face.EnableWedgeTexCoord();

    std::vector<int> vertexMap(src.vert.size(), -1);
    if (src.VN() > 0) {
        vcg::tri::Allocator<VCGMesh>::AddVertices(dst, src.VN());
        int dstVertexIndex = 0;
        for (size_t i = 0; i < src.vert.size(); ++i) {
            const VCGVertex &sv = src.vert[i];
            if (sv.IsD())
                continue;
            VCGVertex &dv = dst.vert[static_cast<size_t>(dstVertexIndex)];
            dv.P() = sv.cP();
            dv.N() = sv.cN();
            dv.C() = sv.cC();
            dv.Q() = sv.cQ();
            dv.Flags() = sv.Flags();
            if (copyVertTexCoord) dv.T()   = sv.cT();
            if (copyVertCurvDir)  { dv.PD1() = sv.cPD1(); dv.PD2() = sv.cPD2();
                                    dv.K1()  = sv.cK1();  dv.K2()  = sv.cK2(); }
            vertexMap[i] = dstVertexIndex;
            ++dstVertexIndex;
        }
    }

    const VCGVertex *srcVertexBase = src.vert.empty() ? nullptr : &src.vert.front();
    if (src.FN() > 0) {
        vcg::tri::Allocator<VCGMesh>::AddFaces(dst, src.FN());
        int dstFaceIndex = 0;
        for (const VCGFace &sf : src.face) {
            if (sf.IsD())
                continue;
            VCGFace &df = dst.face[static_cast<size_t>(dstFaceIndex)];
            for (int k = 0; k < 3; ++k) {
                const VCGVertex *sv = sf.cV(k);
                VCGVertex *dv = nullptr;
                if (sv && srcVertexBase) {
                    const ptrdiff_t srcIdx = sv - srcVertexBase;
                    if (srcIdx >= 0 && static_cast<size_t>(srcIdx) < vertexMap.size()) {
                        const int dstIdx = vertexMap[static_cast<size_t>(srcIdx)];
                        if (dstIdx >= 0)
                            dv = &dst.vert[static_cast<size_t>(dstIdx)];
                    }
                }
                df.V(k) = dv;
                if (copyFaceWedgeTex) df.WT(k) = sf.cWT(k);
            }
            df.N() = sf.cN();
            df.C() = sf.cC();
            df.Q() = sf.cQ();
            df.Flags() = sf.Flags();
            ++dstFaceIndex;
        }
    }

    if (src.EN() > 0) {
        vcg::tri::Allocator<VCGMesh>::AddEdges(dst, src.EN());
        int dstEdgeIndex = 0;
        for (const VCGEdge &se : src.edge) {
            if (se.IsD())
                continue;
            VCGEdge &de = dst.edge[static_cast<size_t>(dstEdgeIndex)];
            for (int k = 0; k < 2; ++k) {
                const VCGVertex *sv = se.cV(k);
                VCGVertex *dv = nullptr;
                if (sv && srcVertexBase) {
                    const ptrdiff_t srcIdx = sv - srcVertexBase;
                    if (srcIdx >= 0 && static_cast<size_t>(srcIdx) < vertexMap.size()) {
                        const int dstIdx = vertexMap[static_cast<size_t>(srcIdx)];
                        if (dstIdx >= 0)
                            dv = &dst.vert[static_cast<size_t>(dstIdx)];
                    }
                }
                de.V(k) = dv;
            }
            de.C() = se.cC();
            de.Flags() = se.Flags();
            ++dstEdgeIndex;
        }
    }

    dst.bbox = src.bbox;
    dst.textures = src.textures;
}

qint64 vcgVertexOcfBytes(const VCGMesh &mesh)
{
    return vectorStorageBytes(mesh.vert.CV)
         + vectorStorageBytes(mesh.vert.CuV)
         + vectorStorageBytes(mesh.vert.CuDV)
         + vectorStorageBytes(mesh.vert.MV)
         + vectorStorageBytes(mesh.vert.NV)
         + vectorStorageBytes(mesh.vert.QV)
         + vectorStorageBytes(mesh.vert.RadiusV)
         + vectorStorageBytes(mesh.vert.TV)
         + vectorStorageBytes(mesh.vert.AV);
}

qint64 vcgFaceOcfBytes(const VCGMesh &mesh)
{
    return vectorStorageBytes(mesh.face.CV)
         + vectorStorageBytes(mesh.face.CDV)
         + vectorStorageBytes(mesh.face.MV)
         + vectorStorageBytes(mesh.face.NV)
         + vectorStorageBytes(mesh.face.QV)
         + vectorStorageBytes(mesh.face.WCV)
         + vectorStorageBytes(mesh.face.WNV)
         + vectorStorageBytes(mesh.face.WTV)
         + vectorStorageBytes(mesh.face.AV)
         + vectorStorageBytes(mesh.face.AF);
}

namespace {

template <typename AttributeSet>
qint64 attributeStorageBytes(const AttributeSet &attributes, std::size_t elementCapacity)
{
    qint64 total = 0;
    for (const auto &attribute : attributes) {
        if (attribute._handle)
            total += qint64(elementCapacity) * qint64(attribute._handle->SizeOf());
    }
    return total;
}

} // namespace

qint64 vcgCustomAttributeBytes(const VCGMesh &mesh)
{
    // SimpleTempData exposes its element size but not its vector capacity. It
    // reserves against the owning container when created, so container capacity
    // is the closest observable allocation estimate.
    return attributeStorageBytes(mesh.vert_attr, mesh.vert.capacity())
         + attributeStorageBytes(mesh.edge_attr, mesh.edge.capacity())
         + attributeStorageBytes(mesh.face_attr, mesh.face.capacity())
         + attributeStorageBytes(mesh.mesh_attr, std::size_t(1));
}

qint64 vcgMeshCpuBytes(const VCGMesh &mesh)
{
    return qint64(mesh.vert.capacity()) * sizeof(VCGVertex)
         + vcgVertexOcfBytes(mesh)
         + qint64(mesh.edge.capacity()) * sizeof(VCGEdge)
         + qint64(mesh.face.capacity()) * sizeof(VCGFace)
         + vcgFaceOcfBytes(mesh)
         + vcgCustomAttributeBytes(mesh);
}

} // namespace DocumentInternal
