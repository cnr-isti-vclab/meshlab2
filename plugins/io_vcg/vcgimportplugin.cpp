#include "plugins/io_vcg/vcgimportplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"

#include <wrap/io_trimesh/export_obj.h>
#include <wrap/io_trimesh/export_off.h>
#include <wrap/io_trimesh/export_ply.h>
#include <wrap/io_trimesh/export_stl.h>
#include <wrap/io_trimesh/import.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <algorithm>
#include <memory>

namespace {
constexpr int kErrSaveUnsupportedFormat = -1000;
constexpr int kErrSaveTextureCopyFailed = -1100;
constexpr int kErrSavePlyBase = -2000;
constexpr int kErrSaveObjBase = -3000;
constexpr int kErrSaveStlBase = -4000;
constexpr int kErrSaveOffBase = -5000;

QString normalizedExtension(const QString &filename)
{
    return QFileInfo(filename).suffix().trimmed().toLower();
}

bool isSupportedLoadExtension(const QString &ext)
{
    return ext == QLatin1String("ply")
        || ext == QLatin1String("obj")
        || ext == QLatin1String("stl")
        || ext == QLatin1String("off")
        || ext == QLatin1String("vmi");
}

bool isSupportedSaveExtension(const QString &ext)
{
    return ext == QLatin1String("ply")
        || ext == QLatin1String("obj")
        || ext == QLatin1String("stl")
        || ext == QLatin1String("off");
}

int encodeExporterError(int base, int exporterErr)
{
    if (exporterErr == 0)
        return 0;
    return base - exporterErr;
}

bool decodeExporterError(int errCode, int base, int *outExporterErr)
{
    if (errCode >= base || errCode <= base - 1000)
        return false;
    if (outExporterErr)
        *outExporterErr = base - errCode;
    return true;
}

int requiredGeometryMask(const VCGMesh &mesh, int capabilityMask)
{
    int mask = 0;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_VERTCOORD) != 0 && mesh.VN() > 0)
        mask |= vcg::tri::io::Mask::IOM_VERTCOORD;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_FACEINDEX) != 0 && mesh.FN() > 0)
        mask |= vcg::tri::io::Mask::IOM_FACEINDEX;
    if ((capabilityMask & vcg::tri::io::Mask::IOM_EDGEINDEX) != 0 && mesh.EN() > 0)
        mask |= vcg::tri::io::Mask::IOM_EDGEINDEX;
    return mask;
}

int effectiveSaveMask(const VCGMesh &mesh, int capabilityMask, int requestedMask)
{
    int mask = (requestedMask != 0) ? requestedMask : capabilityMask;
    mask &= capabilityMask;
    mask |= requiredGeometryMask(mesh, capabilityMask);

    // Prefer wedge attributes when both corner and per-vertex variants are selected.
    if ((mask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0)
        mask &= ~vcg::tri::io::Mask::IOM_VERTTEXCOORD;
    if ((mask & vcg::tri::io::Mask::IOM_WEDGNORMAL) != 0)
        mask &= ~vcg::tri::io::Mask::IOM_VERTNORMAL;
    if ((mask & vcg::tri::io::Mask::IOM_WEDGCOLOR) != 0)
        mask &= ~vcg::tri::io::Mask::IOM_VERTCOLOR;
    return mask;
}

int saveMaskCapabilityForExtension(const QString &ext)
{
    if (ext == QLatin1String("ply"))
        return vcg::tri::io::ExporterPLY<VCGMesh>::GetExportMaskCapability();
    if (ext == QLatin1String("obj"))
        return vcg::tri::io::ExporterOBJ<VCGMesh>::GetExportMaskCapability();
    if (ext == QLatin1String("stl"))
        return vcg::tri::io::ExporterSTL<VCGMesh>::GetExportMaskCapability();
    if (ext == QLatin1String("off"))
        return vcg::tri::io::ExporterOFF<VCGMesh>::GetExportMaskCapability();
    return 0;
}

int loadMaskCapabilityForExtension(const QString &ext)
{
    using M = vcg::tri::io::Mask;
    if (ext == QLatin1String("ply"))
        return M::IOM_VERTCOORD | M::IOM_VERTFLAGS | M::IOM_VERTCOLOR
            | M::IOM_VERTQUALITY | M::IOM_VERTNORMAL | M::IOM_VERTTEXCOORD
            | M::IOM_VERTRADIUS | M::IOM_EDGEINDEX | M::IOM_EDGECOLOR
            | M::IOM_EDGEQUALITY | M::IOM_FACEINDEX
            | M::IOM_FACEFLAGS | M::IOM_FACECOLOR | M::IOM_FACEQUALITY
            | M::IOM_FACENORMAL | M::IOM_WEDGCOLOR | M::IOM_WEDGTEXCOORD
            | M::IOM_CAMERA | M::IOM_BITPOLYGONAL;
    if (ext == QLatin1String("obj"))
        return M::IOM_VERTCOORD | M::IOM_VERTCOLOR | M::IOM_VERTNORMAL
            | M::IOM_VERTTEXCOORD | M::IOM_EDGEINDEX | M::IOM_FACEINDEX
            | M::IOM_FACECOLOR | M::IOM_FACENORMAL | M::IOM_WEDGTEXCOORD
            | M::IOM_WEDGNORMAL | M::IOM_BITPOLYGONAL;
    if (ext == QLatin1String("stl"))
        return M::IOM_VERTCOORD | M::IOM_FACEINDEX | M::IOM_FACECOLOR
            | M::IOM_FACENORMAL;
    if (ext == QLatin1String("off"))
        return M::IOM_VERTCOORD | M::IOM_VERTCOLOR | M::IOM_VERTNORMAL
            | M::IOM_FACEINDEX | M::IOM_FACECOLOR | M::IOM_FACENORMAL
            | M::IOM_BITPOLYGONAL;
    if (ext == QLatin1String("vmi"))
        return M::IOM_ALL;
    return 0;
}

bool saveWritesTextureReferences(int saveMask)
{
    return (saveMask & vcg::tri::io::Mask::IOM_WEDGTEXCOORD) != 0
        || (saveMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0;
}

QString normalizeExistingPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    return info.absoluteFilePath();
}

QString uniqueTextureFileName(
    const QDir &destinationDir,
    const QString &baseName,
    QHash<QString, QString> &reservedNames)
{
    QFileInfo info(baseName);
    QString stem = info.completeBaseName();
    QString suffix = info.suffix();
    // vcglib's readers take a texture name as one token of printable ASCII: PLY drops the
    // spaces from "TextureFile a b.png", OBJ ignores "map_Kd a b.png". A file this save
    // writes keeps to [A-Za-z0-9_.-], as project save does for mesh files.
    static const QRegularExpression unsafe(QStringLiteral("[^A-Za-z0-9_.-]"));
    stem.replace(unsafe, QStringLiteral("_"));
    suffix.replace(unsafe, QStringLiteral("_"));
    if (stem.isEmpty())
        stem = QStringLiteral("texture");
    if (!suffix.isEmpty())
        suffix.prepend(QLatin1Char('.'));

    QString candidate = stem + suffix;
    int counter = 1;
    while (reservedNames.contains(candidate.toLower())
           || destinationDir.exists(candidate)) {
        candidate = QStringLiteral("%1_%2%3").arg(stem).arg(counter).arg(suffix);
        ++counter;
    }
    reservedNames.insert(candidate.toLower(), candidate);
    return candidate;
}

// The texture names a PLY or OBJ file will carry, one per texture slot, so the faces'
// texture indices keep pointing at the right one. A texture with a file is referred to where
// it is, by a path relative to the mesh, or copied next to the mesh with copyStored; one with
// only an image is written next to the mesh under its name, suffixed when that is taken, so
// nothing already there is overwritten. One that has neither keeps the name it was declared
// with. savedPaths gets, per slot, the file that now holds it (empty for the last kind).
// Fails only when a texture cannot be written or copied.
bool prepareTextureReferences(
    const QString &filename,
    const VCGMesh &mesh,
    const MeshIOTextureContext *textureContext,
    bool copyStored,
    std::vector<std::string> &outTextures,
    QStringList &savedPaths)
{
    const QDir destinationDir = QFileInfo(filename).absoluteDir();
    QHash<QString, QString> copiedTargetsBySourcePath;
    QHash<QString, QString> reservedNames;

    const QStringList *textureFileNames = textureContext ? textureContext->textureFileNames : nullptr;
    const QStringList *textureFilePaths = textureContext ? textureContext->textureFilePaths : nullptr;
    const std::vector<MeshIOTextureAsset> *textureAssets =
        textureContext ? textureContext->textureAssets : nullptr;

    int textureCount = int(mesh.textures.size());
    if (textureFileNames)
        textureCount = std::max(textureCount, int(textureFileNames->size()));
    if (textureFilePaths)
        textureCount = std::max(textureCount, int(textureFilePaths->size()));
    if (textureAssets)
        textureCount = std::max(textureCount, int(textureAssets->size()));
    outTextures.reserve(size_t(std::max(0, textureCount)));

    for (int textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
        QString sourcePath;
        QString displayName;
        QImage image;
        if (textureAssets && textureIndex < int(textureAssets->size())) {
            const MeshIOTextureAsset &asset = textureAssets->at(size_t(textureIndex));
            sourcePath = asset.sourcePath.trimmed();
            displayName = asset.name.trimmed();
            if (asset.hasImage())
                image = asset.image;
        } else {
            // The legacy lists only where there is no asset: mesh.textures leaves out the
            // assets that have no file, so reading it by an asset's index can find another
            // texture's file.
            if (textureFilePaths && textureIndex < textureFilePaths->size())
                sourcePath = textureFilePaths->at(textureIndex).trimmed();
            if (textureFileNames && textureIndex < textureFileNames->size())
                displayName = textureFileNames->at(textureIndex).trimmed();
            if (sourcePath.isEmpty() && textureIndex < int(mesh.textures.size()))
                sourcePath = QString::fromStdString(mesh.textures[size_t(textureIndex)]).trimmed();
        }
        if (displayName.isEmpty() && !sourcePath.isEmpty())
            displayName = QFileInfo(sourcePath).fileName();
        if (displayName.isEmpty())
            displayName = QStringLiteral("texture_%1.png").arg(textureIndex + 1);
        if (!image.isNull() && QFileInfo(displayName).suffix().isEmpty())
            displayName += QStringLiteral(".png");

        const QFileInfo sourceInfo(sourcePath);
        const bool hasFile = !sourcePath.isEmpty() && sourceInfo.isFile();
        QString reference;
        QString savedPath;
        if (hasFile && !copyStored) {
            savedPath = sourceInfo.absoluteFilePath();
            reference = destinationDir.relativeFilePath(savedPath);
        } else if (hasFile) {
            const QString normalizedSource = normalizeExistingPath(sourceInfo.absoluteFilePath());
            reference = copiedTargetsBySourcePath.value(normalizedSource);
            if (reference.isEmpty()) {
                reference = uniqueTextureFileName(destinationDir, sourceInfo.fileName(), reservedNames);
                const QString destinationPath = destinationDir.filePath(reference);
                const QString normalizedDestination = normalizeExistingPath(destinationPath);
                if (normalizedSource != normalizedDestination) {
                    QFile::remove(destinationPath);
                    if (!QFile::copy(sourceInfo.absoluteFilePath(), destinationPath))
                        return false;
                }
                copiedTargetsBySourcePath.insert(normalizedSource, reference);
            }
            savedPath = destinationDir.absoluteFilePath(reference);
        } else if (!image.isNull()) {
            reference = uniqueTextureFileName(destinationDir, displayName, reservedNames);
            savedPath = destinationDir.absoluteFilePath(reference);
            if (!image.save(savedPath))
                return false;
        } else {
            reference = sourcePath.isEmpty() ? displayName : sourcePath;
        }

        outTextures.push_back(QDir::toNativeSeparators(reference).toStdString());
        savedPaths.append(savedPath);
    }
    return true;
}

class VCGImportPlugin final : public MeshIOPlugin
{
public:
    QString pluginId() const override
    {
        return QStringLiteral("io_vcg");
    }

    QString name() const override
    {
        return QObject::tr("VCGLib Generic Import/Export");
    }

    QStringList supportedExtensions() const override
    {
        return {
            QStringLiteral("ply"),
            QStringLiteral("obj"),
            QStringLiteral("stl"),
            QStringLiteral("off"),
            QStringLiteral("vmi")
        };
    }

    bool canLoad(const QString &filename) const override
    {
        return isSupportedLoadExtension(normalizedExtension(filename));
    }

    bool canSave(const QString &filename) const override
    {
        return isSupportedSaveExtension(normalizedExtension(filename));
    }

    MeshIOCapabilities loadCapabilities(const QString &filename) const override
    {
        const QString ext = normalizedExtension(filename);
        return { loadMaskCapabilityForExtension(ext), ext == QLatin1String("obj"),
            ext == QLatin1String("obj") || ext == QLatin1String("ply") };
    }

    MeshIOCapabilities saveCapabilities(const QString &filename) const override
    {
        const QString ext = normalizedExtension(filename);
        return { saveMaskCapabilityForExtension(ext), ext == QLatin1String("obj"),
            ext == QLatin1String("obj") || ext == QLatin1String("ply") };
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask) const override
    {
        int loadMask = 0;
        const QFileInfo fi(filename);
        const QString ext = fi.suffix().toLower();
        const QString previousCwd = QDir::currentPath();
        bool switchedCwd = false;

        // VCGLib OBJ importer resolves mtllib/map_Kd paths against the process cwd.
        // Temporarily switch to the OBJ directory so relative material/texture paths resolve.
        if (ext == QLatin1String("obj")) {
            const QString objDir = fi.absolutePath();
            if (!objDir.isEmpty())
                switchedCwd = QDir::setCurrent(objDir);
        }

        vcg::tri::io::Importer<VCGMesh>::LoadMask(filename.toStdString().c_str(), loadMask);
        const int err = vcg::tri::io::Importer<VCGMesh>::Open(mesh, filename.toStdString().c_str(), loadMask, cb);

        if (switchedCwd)
            QDir::setCurrent(previousCwd);

        if (outLoadMask)
            *outLoadMask = loadMask;
        return err;
    }

    QString filterString() const override
    {
        return QObject::tr("Mesh Files (*.ply *.obj *.stl *.off *.vmi)");
    }

    int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb) const override
    {
        return save(filename, mesh, options, cb, nullptr);
    }

    int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb,
        const MeshIOTextureContext *textureContext) const override
    {
        const QString ext = normalizedExtension(filename);
        if (!isSupportedSaveExtension(ext))
            return kErrSaveUnsupportedFormat;

        const int capabilityMask = saveMaskCapabilityForExtension(ext);
        const int saveMask = effectiveSaveMask(mesh, capabilityMask, options.mask);
        const QByteArray encodedPath = QFile::encodeName(filename);
        const char *path = encodedPath.constData();

        // PLY and OBJ refer to texture files by name: the mesh's own list is swapped for the
        // names this save writes, and put back afterwards.
        std::vector<std::string> savedTextureNames;
        QStringList savedTexturePaths;
        const bool hasTextureAssociations =
            !mesh.textures.empty()
            || (textureContext
                && ((textureContext->textureFileNames && !textureContext->textureFileNames->isEmpty())
                    || (textureContext->textureFilePaths && !textureContext->textureFilePaths->isEmpty())
                    || (textureContext->textureAssets && !textureContext->textureAssets->empty())));
        const bool writesTextures = (ext == QLatin1String("ply") || ext == QLatin1String("obj"))
            && saveWritesTextureReferences(saveMask) && hasTextureAssociations;
        if (writesTextures) {
            if (!prepareTextureReferences(filename, mesh, textureContext, options.copyAssociatedTextures,
                                          savedTextureNames, savedTexturePaths))
                return kErrSaveTextureCopyFailed;
            std::swap(mesh.textures, savedTextureNames);
        }
        const auto restoreTextures = qScopeGuard([&] {
            if (writesTextures)
                std::swap(mesh.textures, savedTextureNames);
        });
        const auto reportTextures = [&](int err) {
            if (err == 0 && writesTextures && textureContext && textureContext->savedTexturePaths)
                *textureContext->savedTexturePaths = savedTexturePaths;
            return err;
        };

        if (ext == QLatin1String("ply")) {
            // Polygon reconstruction traverses faux edges through FF adjacency.
            // Keep this ancillary OCF component enabled only for the export.
            const bool restoreDisabledFF =
                (saveMask & vcg::tri::io::Mask::IOM_BITPOLYGONAL) != 0
                && !mesh.face.IsFFAdjacencyEnabled();
            if (restoreDisabledFF)
                mesh.face.EnableFFAdjacency();
            const int err = vcg::tri::io::ExporterPLY<VCGMesh>::Save(
                mesh,
                path,
                saveMask,
                options.binary,
                cb);
            if (restoreDisabledFF)
                mesh.face.DisableFFAdjacency();
            return reportTextures(encodeExporterError(kErrSavePlyBase, err));
        }
        if (ext == QLatin1String("obj") || ext == QLatin1String("off")) {
            const bool restoreDisabledFF =
                (saveMask & vcg::tri::io::Mask::IOM_BITPOLYGONAL) != 0
                && !mesh.face.IsFFAdjacencyEnabled();
            if (restoreDisabledFF)
                mesh.face.EnableFFAdjacency();
            const bool obj = ext == QLatin1String("obj");
            const int err = obj
                ? vcg::tri::io::ExporterOBJ<VCGMesh>::Save(mesh, path, saveMask, cb)
                : vcg::tri::io::ExporterOFF<VCGMesh>::Save(mesh, path, saveMask);
            if (restoreDisabledFF)
                mesh.face.DisableFFAdjacency();
            return reportTextures(encodeExporterError(obj ? kErrSaveObjBase : kErrSaveOffBase, err));
        }
        if (ext == QLatin1String("stl")) {
            const int err =
                vcg::tri::io::ExporterSTL<VCGMesh>::Save(mesh, path, options.binary, saveMask);
            return encodeExporterError(kErrSaveStlBase, err);
        }

        return kErrSaveUnsupportedFormat;
    }

    QString saveFilterString() const override
    {
        return QObject::tr("PLY (*.ply);;Wavefront OBJ (*.obj);;STL (*.stl);;OFF (*.off)");
    }

    int saveMaskCapability(const QString &filename) const override
    {
        return saveMaskCapabilityForExtension(normalizedExtension(filename));
    }

    QString errorString(int errCode) const override
    {
        if (errCode == kErrSaveUnsupportedFormat)
            return QObject::tr("Unsupported export format");
        if (errCode == kErrSaveTextureCopyFailed)
            return QObject::tr("Failed to write or copy one or more texture files");

        int exporterErr = 0;
        if (decodeExporterError(errCode, kErrSavePlyBase, &exporterErr))
            return QString::fromLatin1(vcg::tri::io::ExporterPLY<VCGMesh>::ErrorMsg(exporterErr));
        if (decodeExporterError(errCode, kErrSaveObjBase, &exporterErr))
            return QString::fromLatin1(vcg::tri::io::ExporterOBJ<VCGMesh>::ErrorMsg(exporterErr));
        if (decodeExporterError(errCode, kErrSaveStlBase, &exporterErr))
            return QString::fromLatin1(vcg::tri::io::ExporterSTL<VCGMesh>::ErrorMsg(exporterErr));
        if (decodeExporterError(errCode, kErrSaveOffBase, &exporterErr))
            return QString::fromLatin1(vcg::tri::io::ExporterOFF<VCGMesh>::ErrorMsg(exporterErr));

        return QString::fromLatin1(vcg::tri::io::Importer<VCGMesh>::ErrorMsg(errCode));
    }

    bool isLoadErrorCritical(const QString &, int errCode) const override
    {
        return vcg::tri::io::Importer<VCGMesh>::ErrorCritical(errCode);
    }
};
}

void registerVcgImportPlugin(MeshIOPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<VCGImportPlugin>());
}
