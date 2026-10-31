#include "textureassociationutils.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QObject>

#include <algorithm>
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

QImage makeDummyTexture(int imageSize, int checkSize, const QString &type)
{
    QImage image(imageSize, imageSize, QImage::Format_RGB32);
    if (type == QLatin1String("grid") || type == QLatin1String("uv_grid")) {
        // Image rows are lines of constant V, running along U; columns run along V. The UV
        // grid colors them red and blue, red like the U axis the UV view draws.
        const bool twoColors = (type == QLatin1String("uv_grid"));
        const QRgb alongU = twoColors ? qRgb(220, 40, 40) : qRgb(255, 255, 255);
        const QRgb alongV = twoColors ? qRgb(40, 90, 230) : qRgb(255, 255, 255);
        for (int y = 0; y < imageSize; ++y) {
            for (int x = 0; x < imageSize; ++x) {
                QRgb color = qRgb(128, 128, 128);
                if (x % checkSize == 0)
                    color = alongV;
                if (y % checkSize == 0)
                    color = alongU;
                image.setPixel(x, y, color);
            }
        }
    } else {
        for (int y = 0; y < imageSize; ++y) {
            for (int x = 0; x < imageSize; ++x) {
                image.setPixel(
                    x,
                    y,
                    (((x / checkSize) % 2) == ((y / checkSize) % 2)) ? 0xFFFFFF : 0x808080);
            }
        }
    }
    return image;
}

QString dummyTextureName(const QString &type)
{
    if (type == QLatin1String("grid"))
        return QStringLiteral("Grid");
    if (type == QLatin1String("uv_grid"))
        return QStringLiteral("UV Grid");
    return QStringLiteral("Checkerboard");
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

QStringList outputTextureNames(
    const Document::MeshEntry &entry,
    const QString &requestedName,
    const QString &role,
    int slotCount)
{
    QString name = requestedName.trimmed();
    if (name.isEmpty()) {
        QString layer = QFileInfo(entry.name.trimmed()).completeBaseName();
        if (layer.isEmpty())
            layer = QStringLiteral("texture");
        name = QStringLiteral("%1_%2.png").arg(layer, role);
    }
    if (QFileInfo(name).suffix().isEmpty())
        name += QStringLiteral(".png");
    QStringList names;
    if (slotCount <= 1) {
        names << name;
        return names;
    }
    const QFileInfo info(name);
    for (int i = 0; i < slotCount; ++i)
        names << QStringLiteral("%1_%2.%3").arg(info.completeBaseName()).arg(i).arg(info.suffix());
    return names;
}

std::vector<MeshIOTextureAsset> makeTextureAssetsFromImages(
    const QStringList &names,
    const std::vector<QImage> &images)
{
    std::vector<MeshIOTextureAsset> assets;
    assets.reserve(images.size());
    for (int i = 0; i < names.size() && size_t(i) < images.size(); ++i)
        assets.push_back(makeTextureAssetFromImage(images[size_t(i)], names.at(i)));
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

QSize associatedTextureSize(const Document::MeshEntry &entry, int textureIndex)
{
    if (const MeshIOTextureAsset *asset = Document::meshTextureAsset(entry, textureIndex)) {
        if (asset->hasImage())
            return asset->image.size();
    }

    const QString sourcePath = Document::meshTextureSourcePath(entry, textureIndex).trimmed();
    if (sourcePath.isEmpty())
        return {};

    const QSize size = QImageReader(sourcePath).size();
    if (size.isValid())
        return size;

    // A format Qt has no reader for may still be one readImageFile can decode.
    QImage image;
    QString error;
    return readImageFile(sourcePath, image, error) ? image.size() : QSize();
}

TextureSizes resolveTextureSizes(const Document::MeshEntry &entry, int askedWidth, int askedHeight)
{
    TextureSizes result;
    if ((askedWidth > 0) != (askedHeight > 0)) {
        result.error = QObject::tr("Give both Texture Width and Texture Height, or neither.");
        return result;
    }
    const int textureCount = Document::meshTextureAssociationCount(entry);
    if (askedWidth > 0) {
        result.sizes.assign(size_t(std::max(textureCount, 1)), QSizeF(askedWidth, askedHeight));
        result.units = QObject::tr("texels of a %1 x %2 texture").arg(askedWidth).arg(askedHeight);
    } else if (textureCount > 0) {
        QStringList described;
        for (int i = 0; i < textureCount; ++i) {
            const QSize size = associatedTextureSize(entry, i);
            if (!size.isValid()) {
                result.error = QObject::tr(
                    "The size of texture %1 is unknown: it has no image data and no readable file. "
                    "Give Texture Width and Texture Height to measure without it.")
                        .arg(Document::meshTextureDisplayName(entry, i));
                return result;
            }
            result.sizes.push_back(QSizeF(size));
            described << QObject::tr("%1 x %2").arg(size.width()).arg(size.height());
        }
        result.units = QObject::tr("texels (texture sizes: %1)").arg(described.join(QStringLiteral(", ")));
    } else {
        result.sizes.push_back(QSizeF(1, 1));
        result.known = false;
        result.units = QObject::tr("the unit square: the layer has no texture image, so give Texture Width and "
                                   "Texture Height to measure in texels");
    }
    return result;
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

int removeTextureAsset(Document::MeshEntry &entry, int textureIndex)
{
    if (textureIndex < 0 || textureIndex >= int(entry.textureAssets.size()))
        return -1;
    const QString removedPath = entry.textureAssets[size_t(textureIndex)].sourcePath.trimmed();
    const QString removed = removedPath.isEmpty() ? QString() : normalizeExistingPath(removedPath);
    entry.textureAssets.erase(entry.textureAssets.begin() + textureIndex);

    // A slot names its images by index once they are in the layer (glTF) or by path as the
    // file declared them (OBJ, PLY), so both are matched.
    const auto release = [&](MeshIOMaterialTextureRef &ref) {
        const bool named = ref.assetIndex == textureIndex
            || (ref.assetIndex < 0 && !removed.isEmpty() && !ref.filePath.trimmed().isEmpty()
                && normalizeExistingPath(ref.filePath.trimmed()) == removed);
        if (named)
            ref = MeshIOMaterialTextureRef();
        else if (ref.assetIndex > textureIndex)
            --ref.assetIndex;
        return named;
    };

    // What a face's texture index becomes. It names a material slot, or, on a layer with
    // none, the image itself.
    std::vector<int> remap;
    std::vector<MeshIOMaterialSlot> &materials = entry.materialSet.entries;
    if (materials.empty()) {
        for (int i = 0; i <= int(entry.textureAssets.size()); ++i)
            remap.push_back(i < textureIndex ? i : (i == textureIndex ? -1 : i - 1));
    } else {
        std::vector<MeshIOMaterialSlot> kept;
        for (MeshIOMaterialSlot &slot : materials) {
            bool named = false;
            for (MeshIOMaterialTextureRef *ref : { &slot.baseColorTexture, &slot.normalTexture,
                                                   &slot.occlusionTexture, &slot.roughnessTexture })
                named = release(*ref) || named;
            const bool emptied = named && !slot.baseColorTexture.isValid() && !slot.normalTexture.isValid()
                && !slot.occlusionTexture.isValid() && !slot.roughnessTexture.isValid();
            remap.push_back(emptied ? -1 : int(kept.size()));
            if (!emptied)
                kept.push_back(std::move(slot));
        }
        materials = std::move(kept);
    }

    int untextured = 0;
    VCGMesh &mesh = entry.mesh;
    const auto apply = [&](auto &texCoord) {
        const int n = texCoord.N();
        if (n >= 0 && n < int(remap.size()))
            texCoord.N() = short(remap[size_t(n)]);
    };
    if (mesh.face.IsWedgeTexCoordEnabled()) {
        for (VCGFace &f : mesh.face) {
            if (f.IsD())
                continue;
            const int n = f.cWT(0).N();
            if (n >= 0 && n < int(remap.size()) && remap[size_t(n)] < 0)
                ++untextured;
            for (int k = 0; k < 3; ++k)
                apply(f.WT(k));
        }
    }
    if (mesh.vert.IsTexCoordEnabled()) {
        for (VCGVertex &v : mesh.vert)
            if (!v.IsD())
                apply(v.T());
    }
    rebuildLegacyTextureAssociation(entry);
    return untextured;
}

} // namespace TextureAssociationUtils
