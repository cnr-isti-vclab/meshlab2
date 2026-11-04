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
QString resolveProjectEntryPath(const QString &projectFilePath, const QString &entryPath);
bool parseFloatList(const QString &text, int expectedCount, std::vector<float> &values);
QMatrix4x4 meshLabProjectMatrixToQt(const std::vector<float> &values);
RasterPlaneSemantic rasterPlaneSemanticFromProject(const QString &name);

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
    bool undistorted = false; // the file is in the frame of the pinhole camera already
};

struct MeshLabProjectRasterEntry {
    QString label;
    CameraShot shot;
    CameraLens lens;
    std::vector<MeshLabProjectPlaneEntry> planes;
};

// A visibility set as the project file has it: the layers are the positions of the MLMesh
// entries in the file, which is all that outlives the session -- the ids do not.
struct MeshLabProjectVisibilitySetEntry {
    QString name;
    std::vector<int> meshOrdinals;
};

// A COLMAP sparse model (document_colmap.cpp), opened by its cameras.txt or cameras.bin: the
// images as rasters with their pinhole cameras and lenses, the points3D with their colors,
// and which pixel of which raster saw which point, to measure the registration by.
struct ColmapModel {
    struct Observation {
        int raster = 0;
        vcg::Point2d pixel; // in the photo as shot, COLMAP's pixels: y down, centres at k + 0.5
        int point = 0;
    };
    std::vector<MeshLabProjectRasterEntry> rasters;
    std::vector<vcg::Point3d> points;
    std::vector<vcg::Color4b> pointColors;
    std::vector<Observation> observations;
};
bool isColmapModelFile(const QString &path);
bool parseColmapModel(const QString &camerasPath, ColmapModel &model, QString &error);
// The distance, in pixels of the photo as shot, between each observation and its point
// projected through the raster's camera and lens
std::vector<double> colmapReprojectionErrors(const ColmapModel &model);

bool parseMeshLabProjectFile(
    const QString &filename,
    std::vector<MeshLabProjectMeshEntry> &meshes,
    std::vector<MeshLabProjectRasterEntry> &rasters,
    std::vector<MeshLabProjectVisibilitySetEntry> &visibilitySets,
    LengthUnit &unit,
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
void computeMissingNormals(VCGMesh &mesh, int loadMask);
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
