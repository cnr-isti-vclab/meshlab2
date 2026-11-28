#pragma once

#include "lengthunit.h"
#include "meshfilterplugin.h"
#include "vcgmesh.h"
#include <vcg/math/shot.h>
#include <QImage>
#include <QMatrix4x4>
#include <QString>
#include <QStringList>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
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
    // The document origin (zero when it has none), in double precision. A georeferenced
    // format (isGeoreferenced) adds it back, in double, to write true coordinates; every
    // other format writes the coordinates as the layer holds them, relative to it.
    // Document::saveMesh fills it in.
    vcg::Point3d origin = vcg::Point3d(0, 0, 0);
};

// What a load is told, and tells back, besides the mesh.
struct MeshIOLoadContext
{
    // In and out: the point the mesh's coordinates are relative to, in the file's coordinates
    // and in double precision -- the document origin (Document::origin). A georeferenced
    // format (isGeoreferenced) subtracts it in double, before converting to float, since
    // float keeps only multiples of 0.5 at 5,000,000. Given none, such a format sets one when
    // the file lies far from zero (isFarFromZero), and none otherwise. Other formats leave it
    // alone: their coordinates are taken as they are, relative to it.
    std::optional<vcg::Point3d> origin;
    // Out: the unit the file says its coordinates are in, for the formats that say one -- by
    // their definition (glTF and E57 are in meters) or in the file (3MF). Unknown when the
    // file does not say, which is what most formats do.
    LengthUnit unit = LengthUnit::Unknown;
    // In: the load options (loadParameters), every declared one, resolved and validated by
    // the Document; empty for a format that declares none.
    MeshFilterParameterValues options;
    // Out: what the load did that the log should keep -- the importer's progress callback
    // only shows a passing line.
    QStringList messages;
};

// Whether a box lies too far from zero for float coordinates: the float step at its centre is
// coarser than a millionth of its size (about 16 diagonals away, since float keeps 24 bits).
// A UTM survey is far by any measure -- at 5,000,000 m the step is 0.5 m -- while a scan in
// its scanner's frame, or an object a few sizes from zero, is not. A box with no size is
// never far: a single point loses nothing to rounding. The rule by which a georeferenced
// importer chooses an origin, and by which a layer is reported imprecise.
inline bool isFarFromZero(const vcg::Box3d &box)
{
    if (box.IsNull() || box.Diag() == 0)
        return false;
    const float centre = float(box.Center().Norm());
    return std::nextafter(centre, std::numeric_limits<float>::infinity()) - centre > 1e-6 * box.Diag();
}

// One layer of a scene file (MeshIOPlugin::holdsScenes), as loadScene gives it: its mesh in
// the coordinates of its own prim or node, and the matrix that places it, which becomes the
// layer matrix. Wedge texture coordinates are enabled on mesh; the Document disables them
// when loadMask says there are none.
struct MeshIOSceneLayer
{
    std::unique_ptr<VCGMesh> mesh = std::make_unique<VCGMesh>();
    int loadMask = 0;
    MeshIOMaterialSet materialSet;
    QMatrix4x4 transform;
    QString label;
    bool visible = true;
};

struct MeshIOTextureContext;

// One layer handed to saveScene: its mesh, what of it to write (mask), the matrix that places
// it, written as the prim's transform rather than baked, its label, and its textures.
struct MeshIOSceneLayerView
{
    VCGMesh *mesh = nullptr;
    int mask = 0;
    QMatrix4x4 transform;
    QString label;
    const MeshIOTextureContext *textures = nullptr;
};

// The load option of the formats that declare an up axis (USD, glTF): MeshLab keeps a file's
// own frame, so a Y-up file is read as it is unless the user asks to turn it Z-up. Not the
// default and never automatic; when applied, the log says so (docs/design/data_model.md,
// "Frames are kept").
namespace UpAxisOption {
inline constexpr const char *kId = "rotateYUpToZUp";
MeshFilterParameterDescriptor descriptor();
// Whether a load was asked to turn a Y-up file Z-up.
bool requested(const MeshIOLoadContext &context);
// Turns mesh Z-up, +90 degrees about X, positions and normals, when the file is Y-up and the
// option asks for it; either way, a Y-up file leaves a log line saying which.
void apply(VCGMesh &mesh, bool fileIsYUp, MeshIOLoadContext &context);
}

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

// An image a mesh file carries with the camera that took it, in the coordinates load() gives
// the mesh: E57 holds a scan's photos this way. Document::loadMesh makes each a raster.
struct MeshIORaster
{
    QString name;
    QImage image;
    vcg::Shot<float> shot;
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

    // Also takes and reports what MeshIOLoadContext holds: the origin and the unit. Document::loadMesh
    // and reloadMesh use this overload.
    virtual int load(
        const QString &filename,
        VCGMesh &mesh,
        vcg::CallBackPos *cb,
        int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet,
        MeshIOLoadContext &context) const
    {
        (void) context;
        return load(filename, mesh, cb, outLoadMask, outMaterialSet);
    }

    // The options a load of this file accepts, declared with the filter parameter schema:
    // what the format leaves to the user (whether STL's duplicated corners are merged).
    // Opening never asks: the Document fills them in from the declared defaults, the values
    // the user chose to remember for the format, and what the caller passes, and hands them to
    // load() in MeshIOLoadContext::options. Empty, the default, means the format has none.
    virtual std::vector<MeshFilterParameterDescriptor> loadParameters(const QString &filename) const
    {
        (void) filename;
        return {};
    }

    // Whether the format holds a scene: several layers, each with its matrix and label, which
    // loadScene reads as layers and saveScene writes from them (docs/design/proposals/
    // usd_support.md §5). load() and save() still read and write one mesh, the scene flattened.
    virtual bool holdsScenes(const QString &filename) const
    {
        (void) filename;
        return false;
    }
    // Reads every mesh, point cloud and polyline of the scene as a layer of its own. options and
    // messages travel in context as for load(). Returns 0 or an error code for errorString().
    virtual int loadScene(const QString &filename, std::vector<MeshIOSceneLayer> &layers,
        vcg::CallBackPos *cb, MeshIOLoadContext &context) const
    {
        (void) filename;
        (void) layers;
        (void) cb;
        (void) context;
        return -1;
    }
    // Writes the layers as one scene file; options give the unit and the origin, the masks
    // are per layer.
    virtual int saveScene(const QString &filename, const std::vector<MeshIOSceneLayerView> &layers,
        const MeshIOSaveOptions &options, vcg::CallBackPos *cb) const
    {
        (void) filename;
        (void) layers;
        (void) options;
        (void) cb;
        return -1;
    }

    // Whether the format's coordinates are true, georeferenced ones (E57): it subtracts the
    // document origin on load and adds it back on save (MeshIOLoadContext, MeshIOSaveOptions).
    // The others hold coordinates relative to it, both ways.
    virtual bool isGeoreferenced(const QString &filename) const
    {
        (void) filename;
        return false;
    }

    // The images the file holds with their cameras, read after a successful load(). Most
    // formats have none. The cameras are relative to the origin the load used (context, as
    // load() left it). What could not be read goes to warnings, for the log: it never fails the
    // mesh load.
    virtual std::vector<MeshIORaster> loadRasters(
        const QString &filename, const MeshIOLoadContext &context, QStringList &warnings) const
    {
        (void) filename;
        (void) context;
        (void) warnings;
        return {};
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
