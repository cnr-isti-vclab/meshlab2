#pragma once

#include "lengthunit.h"
#include "vcgmesh.h"
#include <QImage>
#include <QString>
#include <QStringList>
#include <vector>

struct MeshIOTextureAsset
{
    QString name;
    QString sourcePath;
    QImage image;

    bool hasImage() const
    {
        return !image.isNull();
    }

    bool hasSourcePath() const
    {
        return !sourcePath.trimmed().isEmpty();
    }
};

struct MeshIOMaterialTextureRef
{
    QString fileName;
    QString filePath;
    int assetIndex = -1;

    bool isValid() const
    {
        return assetIndex >= 0 || !fileName.trimmed().isEmpty() || !filePath.trimmed().isEmpty();
    }
};

struct MeshIOMaterialSlot
{
    QString name;
    MeshIOMaterialTextureRef baseColorTexture;
    MeshIOMaterialTextureRef normalTexture;
    MeshIOMaterialTextureRef occlusionTexture;
    MeshIOMaterialTextureRef roughnessTexture;
    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float roughnessFactor = 1.0f;
};

struct MeshIOMaterialSet
{
    std::vector<MeshIOMaterialSlot> entries;
    // In-memory texture images.  Plugins can avoid writing embedded textures to
    // disk by providing QImage objects here alongside the material refs.
    // The GPU cache prefers these over on-disk file paths.
    std::vector<MeshIOTextureAsset> textureAssets;

    bool empty() const { return entries.empty(); }
    void clear() { entries.clear(); textureAssets.clear(); }
};

struct MeshIOSaveOptions
{
    // vcg::tri::io::Mask bits to export.
    int mask = 0;
    // Preferred binary/text mode when format supports it.
    bool binary = true;
    // Optional hint used by formats supporting embedded textures (e.g. glTF/glb).
    bool embedTextures = false;
    // For formats that refer to texture files (PLY TextureFile comments, OBJ map_Kd). A texture
    // with no file of its own is always written next to the mesh; this also copies there the
    // ones that have a file, which are otherwise referred to where they are.
    bool copyAssociatedTextures = false;
    // Optional hint for formats supporting Draco geometry compression (e.g. glTF/glb).
    bool dracoCompression = false;
    // Compression level in [0, 10], where 10 prefers smaller output over speed.
    int dracoCompressionLevel = 7;
    // The document's unit (Unknown when the user has not said), for formats that record one.
    // Document::saveMesh fills it in; a caller's value is replaced.
    LengthUnit unit = LengthUnit::Unknown;
};

// On save, an asset's sourcePath is the file holding exactly its image (Document::saveMesh
// resolves it), so a saver copies or refers to that file and writes the image only when the
// path is empty.
struct MeshIOTextureContext
{
    const QStringList *textureFileNames = nullptr;
    const QStringList *textureFilePaths = nullptr;
    const std::vector<MeshIOTextureAsset> *textureAssets = nullptr;
    const MeshIOMaterialSet *materialSet = nullptr;
    // Out, on save: one entry per asset, the absolute path of the file that now holds it --
    // written, copied or referred to -- or the mesh file itself when packed inside it; empty
    // when this save did not store it. A saver that stores no texture leaves it alone.
    QStringList *savedTexturePaths = nullptr;
};

struct MeshIOCapabilities
{
    int mask = 0;
    bool materials = false;
    bool textureImages = false;
};

// Abstract interface for a mesh I/O plugin.
// Implement this to add support for new file formats.
class MeshIOPlugin
{
public:
    virtual ~MeshIOPlugin() = default;

    // Stable plugin identifier used for preferences/settings (non-localized).
    virtual QString pluginId() const = 0;

    // Human-readable plugin name.
    virtual QString name() const = 0;

    // Supported file extensions (lowercase, without leading dot), e.g. {"ply", "obj"}.
    virtual QStringList supportedExtensions() const = 0;

    // Returns true if this plugin can handle the given filename (by extension).
    virtual bool canLoad(const QString &filename) const = 0;

    // Data categories that this format can import. Actual files may contain a subset.
    virtual MeshIOCapabilities loadCapabilities(const QString &filename) const
    {
        (void) filename;
        return {};
    }

    // Loads the mesh from filename into mesh.
    // The optional callback cb follows vcg::CallBackPos convention for progress reporting.
    // If outLoadMask is non-null, the plugin can store vcg::tri::io::Mask bits describing
    // which attributes were found in the input file.
    // Returns 0 on success, a non-zero error code otherwise.
    virtual int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask) const = 0;

    // Optional extended load path for material channel metadata (PBR-related texture channels).
    // Default behavior keeps backward compatibility: clear metadata and fallback to load().
    virtual int load(
        const QString &filename,
        VCGMesh &mesh,
        vcg::CallBackPos *cb,
        int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet) const
    {
        if (outMaterialSet)
            outMaterialSet->clear();
        return load(filename, mesh, cb, outLoadMask);
    }

    // Also reports the unit the file says its coordinates are in, for the formats that say one --
    // by their definition (glTF and E57 are in meters) or in the file (3MF). *outUnit is left
    // alone, Unknown, when the file does not say, which is what most formats do. Document::loadMesh
    // uses this overload.
    virtual int load(
        const QString &filename,
        VCGMesh &mesh,
        vcg::CallBackPos *cb,
        int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet,
        LengthUnit *outUnit) const
    {
        (void) outUnit;
        return load(filename, mesh, cb, outLoadMask, outMaterialSet);
    }

    // Qt file dialog filter string for this plugin's formats, e.g. "Mesh Files (*.ply *.obj)".
    virtual QString filterString() const = 0;

    // Human-readable error message for a given error code returned by load().
    virtual QString errorString(int errCode) const = 0;

    // Some formats can return a usable mesh together with a warning.
    virtual bool isLoadErrorCritical(const QString &filename, int errCode) const
    {
        (void) filename;
        return errCode != 0;
    }

    // Returns true if this plugin can save the given filename (typically by extension).
    virtual bool canSave(const QString &filename) const
    {
        (void) filename;
        return false;
    }

    // Saves the mesh to filename using the requested save options.
    // Returns 0 on success, a non-zero error code otherwise.
    virtual int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb) const
    {
        (void) filename;
        (void) mesh;
        (void) options;
        (void) cb;
        return -1;
    }

    // Optional extended save path for mesh-owned texture assets/materials.
    // Default behavior keeps backward compatibility with exporters that only
    // need the legacy VCGMesh fields.
    virtual int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb,
        const MeshIOTextureContext *textureContext) const
    {
        (void) textureContext;
        return save(filename, mesh, options, cb);
    }

    // Qt file dialog filter string for save operation, e.g. "PLY (*.ply);;OBJ (*.obj)".
    // Empty by default if plugin does not support export.
    virtual QString saveFilterString() const
    {
        return QString();
    }

    // Exportable attribute capability mask for filename's format (vcg::tri::io::Mask bits).
    // Returns 0 when unknown/unsupported.
    virtual int saveMaskCapability(const QString &filename) const
    {
        (void) filename;
        return 0;
    }

    virtual MeshIOCapabilities saveCapabilities(const QString &filename) const
    {
        return { saveMaskCapability(filename), false, false };
    }
};
