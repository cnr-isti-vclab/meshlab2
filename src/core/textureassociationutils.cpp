#include "textureassociationutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QObject>

#include <limits>

#if defined(MESHLAB2_HAS_STB_IMAGE)
// The implementation lives in stbimageimpl.cpp; this is declarations only.
#include <stb_image.h>
#endif

namespace TextureAssociationUtils {

QString normalizeExistingPath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return canonical;
    return info.absoluteFilePath();
}

QImage makeDummyTexture(int imageSize, int checkSize, bool checkerboard)
{
    QImage image(imageSize, imageSize, QImage::Format_RGB32);
    if (checkerboard) {
        for (int y = 0; y < imageSize; ++y) {
            for (int x = 0; x < imageSize; ++x) {
                image.setPixel(
                    x,
                    y,
                    (((x / checkSize) % 2) == ((y / checkSize) % 2)) ? 0xFFFFFF : 0x808080);
            }
        }
    } else {
        for (int y = 0; y < imageSize; ++y) {
            for (int x = 0; x < imageSize; ++x) {
                image.setPixel(
                    x,
                    y,
                    ((x % checkSize) == 0 || (y % checkSize) == 0) ? 0xFFFFFF : 0x808080);
            }
        }
    }
    return image;
}

namespace {

// Second opinion for the files Qt declines. Qt's own plugins are always asked first --
// they honour EXIF orientation, ICC profiles and 16-bit channels, and their QImage
// formats are the ones the rest of the application expects -- so this only ever runs
// after QImageReader has already given up.
//
// The case that motivates it: QTgaHandler accepts only TrueVision 2.0 Targas, meaning
// files that carry the 18-byte "TRUEVISION-XFILE." footer. A perfectly well-formed
// Targa written to the original 1984 spec has no footer, and Qt rejects it outright
// even with qtimageformats deployed. stb_image reads both, along with a handful of
// other formats Qt has no plugin for at all.
bool readWithStb(const QString &path, QImage &image)
{
#if defined(MESHLAB2_HAS_STB_IMAGE)
    // stb takes a byte path; hand it the file's bytes instead so non-ASCII paths work
    // the same on every platform. Textures are small enough to read whole.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.isEmpty() || bytes.size() > qsizetype(std::numeric_limits<int>::max()))
        return false;

    int width = 0;
    int height = 0;
    int channelsInFile = 0;
    // Always ask for RGBA: it is the one request stb satisfies for every input, and it
    // saves branching on the source channel count. QImage keeps the same byte order.
    stbi_uc *pixels = stbi_load_from_memory(
        reinterpret_cast<const stbi_uc *>(bytes.constData()),
        int(bytes.size()),
        &width,
        &height,
        &channelsInFile,
        4);
    if (!pixels)
        return false;

    // Copy rather than adopt: the QImage has to outlive stbi_image_free, and this also
    // hands back an image with QImage's own row alignment.
    const QImage wrapped(pixels, width, height, width * 4, QImage::Format_RGBA8888);
    image = wrapped.copy();
    stbi_image_free(pixels);
    return !image.isNull();
#else
    Q_UNUSED(path);
    Q_UNUSED(image);
    return false;
#endif
}

} // namespace

bool readImageFile(const QString &path, QImage &image, QString &error)
{
    image = QImage();
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        error = QObject::tr("No image file path was given.");
        return false;
    }

    const QFileInfo info(trimmed);
    const QString shown = QDir::toNativeSeparators(trimmed);
    if (!info.exists() || !info.isFile()) {
        error = QObject::tr("Image file '%1' does not exist.").arg(shown);
        return false;
    }

    QImageReader reader(trimmed);
    reader.setAutoTransform(true);
    image = reader.read();
    if (!image.isNull())
        return true;

    if (readWithStb(trimmed, image))
        return true;

    // A format is readable only when its plugin from Qt's qtimageformats module sits
    // beside the application, so on a build without it .tga, .tif and .webp files die
    // here with nothing but "Unknown error". Content sniffing cannot be used to say so
    // either: an uncompressed .tga header is byte-identical to a Windows .cur one, so
    // QImageReader::imageFormat() answers "ico" for a Targa file and canRead() returns
    // true. Only the suffix is worth reporting.
    const QString suffix = info.suffix().toLower();
    bool handled = suffix.isEmpty();
    for (const QByteArray &format : QImageReader::supportedImageFormats()) {
        if (suffix == QString::fromLatin1(format).toLower()) {
            handled = true;
            break;
        }
    }
    if (!handled) {
        error = QObject::tr("Cannot read '%1': no decoder in this build handles .%2 files "
                            "(Qt's image plugins and the stb fallback both declined it). "
                            "Convert the image to PNG.")
                    .arg(shown, suffix);
        return false;
    }

    error = QObject::tr("Cannot read image '%1': %2").arg(shown, reader.errorString());
    return false;
}

MeshIOTextureAsset makeTextureAssetFromPath(const QString &path)
{
    const QFileInfo info(path);
    MeshIOTextureAsset asset;
    asset.name = info.fileName();
    asset.sourcePath = QDir::toNativeSeparators(path);
    return asset;
}

MeshIOTextureAsset makeTextureAssetFromImage(
    const QImage &image,
    const QString &name,
    const QString &sourcePath)
{
    MeshIOTextureAsset asset;
    asset.name = name.trimmed();
    asset.sourcePath = QDir::toNativeSeparators(sourcePath.trimmed());
    asset.image = image;
    return asset;
}

std::vector<MeshIOTextureAsset> makeTextureAssetsFromSavedImages(
    const QStringList &paths,
    const std::vector<QImage> &images)
{
    std::vector<MeshIOTextureAsset> assets;
    assets.reserve(images.size());
    for (int i = 0; i < paths.size() && size_t(i) < images.size(); ++i) {
        const QFileInfo info(paths.at(i));
        assets.push_back(makeTextureAssetFromImage(images[size_t(i)], info.fileName(), paths.at(i)));
    }
    return assets;
}

QStringList associatedTexturePaths(const Document::MeshEntry &entry)
{
    QStringList paths;
    for (const MeshIOTextureAsset &asset : entry.textureAssets) {
        const QString path = asset.sourcePath.trimmed();
        if (!path.isEmpty())
            paths.push_back(normalizeExistingPath(path));
    }
    if (!paths.isEmpty())
        return paths;

    for (const QString &path : entry.textureFilePaths) {
        const QString trimmed = path.trimmed();
        if (!trimmed.isEmpty())
            paths.push_back(normalizeExistingPath(trimmed));
    }
    if (!paths.isEmpty())
        return paths;

    const QFileInfo meshInfo(entry.sourcePath);
    const QDir meshDir = meshInfo.absoluteDir();
    for (const std::string &declared : entry.mesh.textures) {
        const QString name = QString::fromStdString(declared).trimmed();
        if (name.isEmpty())
            continue;
        QFileInfo texInfo(name);
        const QString resolved = texInfo.isAbsolute()
            ? texInfo.absoluteFilePath()
            : meshDir.filePath(name);
        paths.push_back(normalizeExistingPath(resolved));
    }
    return paths;
}

bool loadAssociatedTextureImage(
    const Document::MeshEntry &entry,
    int textureIndex,
    QImage &image,
    QString &error)
{
    if (const MeshIOTextureAsset *asset = Document::meshTextureAsset(entry, textureIndex)) {
        if (asset->hasImage()) {
            image = asset->image;
            return !image.isNull();
        }
    }

    const QString sourcePath = Document::meshTextureSourcePath(entry, textureIndex).trimmed();
    if (sourcePath.isEmpty()) {
        error = QObject::tr("Texture %1 does not have image data or a backing file path.")
                    .arg(Document::meshTextureDisplayName(entry, textureIndex));
        return false;
    }

    return readImageFile(sourcePath, image, error);
}

bool saveImages(const QStringList &paths, const std::vector<QImage> &images, QString &error)
{
    if (paths.size() != int(images.size())) {
        error = QObject::tr("Texture path/image count mismatch.");
        return false;
    }

    for (int i = 0; i < paths.size(); ++i) {
        const QString path = paths.at(i).trimmed();
        if (path.isEmpty()) {
            error = QObject::tr("Texture output path is empty.");
            return false;
        }
        QFileInfo info(path);
        QDir dir = info.absoluteDir();
        if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
            error = QObject::tr("Cannot create directory '%1'.").arg(dir.path());
            return false;
        }
        if (!images[size_t(i)].save(path)) {
            error = QObject::tr("Failed to save texture '%1'.").arg(path);
            return false;
        }
    }
    return true;
}

void rebuildLegacyTextureAssociation(Document::MeshEntry &entry)
{
    entry.mesh.textures.clear();
    entry.textureFileNames.clear();
    entry.textureFilePaths.clear();
    for (const MeshIOTextureAsset &asset : entry.textureAssets) {
        entry.textureFileNames.push_back(asset.name.trimmed());
        entry.textureFilePaths.push_back(asset.sourcePath.trimmed());
        if (!asset.sourcePath.trimmed().isEmpty())
            entry.mesh.textures.push_back(QDir::toNativeSeparators(asset.sourcePath).toStdString());
    }
}

void replaceTextureAssociations(
    Document::MeshEntry &entry,
    const std::vector<MeshIOTextureAsset> &assets)
{
    entry.textureAssets = assets;
    rebuildLegacyTextureAssociation(entry);
    entry.materialSet.clear();
    for (int i = 0; i < int(entry.textureAssets.size()); ++i) {
        const MeshIOTextureAsset &asset = entry.textureAssets[size_t(i)];
        MeshIOMaterialSlot slot;
        slot.name = QObject::tr("Material %1").arg(i + 1);
        slot.baseColorTexture.fileName = asset.name.trimmed();
        slot.baseColorTexture.filePath = asset.sourcePath.trimmed();
        slot.baseColorTexture.assetIndex = i;
        entry.materialSet.entries.push_back(std::move(slot));
    }
}

void appendTextureAssociations(
    Document::MeshEntry &entry,
    const std::vector<MeshIOTextureAsset> &assets)
{
    const int slotBase = Document::meshTextureAssociationCount(entry);
    const int assetBase = int(entry.textureAssets.size());
    for (const MeshIOTextureAsset &asset : assets)
        entry.textureAssets.push_back(asset);
    rebuildLegacyTextureAssociation(entry);
    for (int i = 0; i < int(assets.size()); ++i) {
        const MeshIOTextureAsset &asset = assets[size_t(i)];
        MeshIOMaterialSlot slot;
        slot.name = QObject::tr("Material %1").arg(slotBase + i + 1);
        slot.baseColorTexture.fileName = asset.name.trimmed();
        slot.baseColorTexture.filePath = asset.sourcePath.trimmed();
        slot.baseColorTexture.assetIndex = assetBase + i;
        entry.materialSet.entries.push_back(std::move(slot));
    }
}

void ensureMaterialSlotCount(Document::MeshEntry &entry, int count)
{
    if (count <= 0)
        return;
    while (int(entry.materialSet.entries.size()) < count) {
        MeshIOMaterialSlot slot;
        slot.name = QObject::tr("Material %1").arg(int(entry.materialSet.entries.size()) + 1);
        entry.materialSet.entries.push_back(std::move(slot));
    }
}

int ensureTextureListed(Document::MeshEntry &entry, const QString &path)
{
    const QString normalized = normalizeExistingPath(path);
    for (int i = 0; i < int(entry.textureAssets.size()); ++i) {
        if (normalizeExistingPath(entry.textureAssets[size_t(i)].sourcePath) == normalized)
            return i;
    }

    entry.textureAssets.push_back(makeTextureAssetFromPath(path));
    rebuildLegacyTextureAssociation(entry);
    return int(entry.textureAssets.size()) - 1;
}

} // namespace TextureAssociationUtils
