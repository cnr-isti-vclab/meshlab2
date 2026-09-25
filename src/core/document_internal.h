#pragma once

#include "document.h"
#include "documentundomanager.h"
#include "meshfilterpluginmanager.h"
#include "meshiopluginmanager.h"
#include "plugins/filterpluginregistry.h"
#include "plugins/meshpluginregistry.h"

#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>

namespace DocumentInternal {

extern Document *g_callbackDocument;

QString summarizeLoadMask(int mask);
bool isMeshLabProjectExtension(const QString &path);
QString resolveProjectEntryPath(const QString &projectFilePath, const QString &entryPath);
bool parseFloatList(const QString &text, int expectedCount, std::vector<float> &values);
QMatrix4x4 meshLabProjectMatrixToQt(const std::vector<float> &values);
bool parseIntPair(const QString &text, int &a, int &b);
bool parseFloatPair(const QString &text, float &a, float &b);
RasterPlaneSemantic rasterPlaneSemanticFromProject(const QString &name);
bool parseMeshLabProjectCamera(
    const QXmlStreamAttributes &attrs,
    CameraShot &outShot,
    QString *errorMessage);

struct MeshLabProjectMeshEntry {
    QString label;
    QString sourcePath;
    QMatrix4x4 transform;
    bool hasTransform = false;
};

struct MeshLabProjectPlaneEntry {
    QString name;
    QString sourcePath;
    RasterPlaneSemantic semantic = RasterPlaneSemantic::RGBA;
};

struct MeshLabProjectRasterEntry {
    QString label;
    CameraShot shot;
    std::vector<MeshLabProjectPlaneEntry> planes;
};

bool parseMeshLabProjectFile(
    const QString &filename,
    std::vector<MeshLabProjectMeshEntry> &meshes,
    std::vector<MeshLabProjectRasterEntry> &rasters,
    QString &errorMessage);
QString resolveTexturePath(const QString &meshFilePath, const QString &declaredTextureName);
MeshIOMaterialTextureRef normalizeMaterialTextureRef(
    const QString &meshFilePath,
    const MeshIOMaterialTextureRef &src);
MeshIOMaterialSet normalizeMaterialSet(
    const QString &meshFilePath,
    const MeshIOMaterialSet &src,
    const VCGMesh &mesh);
std::vector<MeshIOTextureAsset> buildTextureAssetsFromLegacyAssociation(
    const QStringList &textureFileNames,
    const QStringList &textureFilePaths,
    const std::vector<std::string> &meshTextures,
    const std::vector<MeshIOTextureAsset> &importedAssets = {});
void syncTextureAssetsFromLegacyAssociation(Document::MeshEntry &entry);
QSize rasterPlaneStorageSize(const RasterPlane &plane);
QString rasterPlaneFallbackName(const RasterPlane &plane, int planeIndex);
QString rasterEntryDisplayName(const Document::RasterEntry &entry, int fallbackIndex);
void normalizeRasterEntry(Document::RasterEntry &entry, int fallbackIndex);
bool meshNeedsCompaction(const VCGMesh &mesh);
void compactMeshStorageInvariant(VCGMesh &mesh);
void copyMeshEntryMetadata(const Document::MeshEntry &src, Document::MeshEntry &dst);
void deepCopyMesh(const VCGMesh &src, VCGMesh &dst);

// Mesh memory accounting helpers (shared between document_memory.cpp and documentundomanager.cpp)
template <typename Vector>
qint64 vectorStorageBytes(const Vector &v)
{
    return qint64(v.capacity()) * qint64(sizeof(typename Vector::value_type));
}
qint64 vcgVertexOcfBytes(const VCGMesh &mesh);
qint64 vcgFaceOcfBytes(const VCGMesh &mesh);
qint64 vcgCustomAttributeBytes(const VCGMesh &mesh);
qint64 vcgMeshCpuBytes(const VCGMesh &mesh);

} // namespace DocumentInternal
