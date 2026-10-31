#pragma once

#include "document.h"
#include "meshioplugin.h"
#include <QImage>
#include <QString>
#include <QStringList>
#include <vector>

namespace TextureAssociationUtils {

QString normalizeExistingPath(const QString &path);
// A generated placeholder texture, by the id a filter's `dummy_type` parameter uses:
// "checkerboard", "grid", or "uv_grid", a grid whose lines along U are red and along V blue so
// the two directions can be told apart. Anything else gives the checkerboard.
QImage makeDummyTexture(int imageSize, int checkSize, const QString &type);
// The pattern's name, for naming the texture made from it: Checkerboard, Grid or UV Grid.
QString dummyTextureName(const QString &type);

// Read an image file, reporting *why* it could not be read. Prefer this over
// QImage(path) anywhere the failure reaches the user: a missing image-format
// plugin is otherwise indistinguishable from a corrupt file.
bool readImageFile(const QString &path, QImage &image, QString &error);

MeshIOTextureAsset makeTextureAssetFromPath(const QString &path);
MeshIOTextureAsset makeTextureAssetFromImage(
    const QImage &image,
    const QString &name,
    const QString &sourcePath = {});
// The textures a filter makes have a name and no file: saving the mesh writes them
// (docs/design/proposals/project_persistence.md). outputTextureNames gives one name per
// texture slot -- the one asked for, or "<layer>_<role>.png" when none was -- with _0, _1, ...
// before the suffix when there are several.
QStringList outputTextureNames(
    const Document::MeshEntry &entry,
    const QString &requestedName,
    const QString &role,
    int slotCount);
std::vector<MeshIOTextureAsset> makeTextureAssetsFromImages(
    const QStringList &names,
    const std::vector<QImage> &images);

QStringList associatedTexturePaths(const Document::MeshEntry &entry);
bool loadAssociatedTextureImage(
    const Document::MeshEntry &entry,
    int textureIndex,
    QImage &image,
    QString &error);
// The size in texels of one of the layer's textures, or an invalid size if it has no image data
// and no readable file. A texture held in memory answers at once, and a file is asked only for
// its header, so this is cheap where loadAssociatedTextureImage reads the whole image.
QSize associatedTextureSize(const Document::MeshEntry &entry, int textureIndex);

// The size in texels of each texture a layer's faces can use, for the measures that count texels.
// A size asked for (both sides positive) is that of every texture; without one each associated
// image gives its own; a layer with neither is measured in its unit square, which `known` tells
// from a real size. `units` says which, for the report; `error` is not empty when a size is
// needed and cannot be had (half a size asked for, or an image that cannot be read).
struct TextureSizes
{
    std::vector<QSizeF> sizes;
    bool known = true;
    QString units;
    QString error;
};
TextureSizes resolveTextureSizes(const Document::MeshEntry &entry, int askedWidth, int askedHeight);

void rebuildLegacyTextureAssociation(Document::MeshEntry &entry);
void replaceTextureAssociations(
    Document::MeshEntry &entry,
    const std::vector<MeshIOTextureAsset> &assets);
void appendTextureAssociations(
    Document::MeshEntry &entry,
    const std::vector<MeshIOTextureAsset> &assets);
void ensureMaterialSlotCount(Document::MeshEntry &entry, int count);
int ensureTextureListed(Document::MeshEntry &entry, const QString &path);
// Removes the layer's texture image `textureIndex`. The material slots stop referring to it,
// by index or by path, and a slot left with no image at all goes as well: the faces that used
// it lose their texture (index -1) and those of the slots after it move down one. Returns how
// many faces lost their texture, or -1, with nothing changed, if there is no such image.
int removeTextureAsset(Document::MeshEntry &entry, int textureIndex);

} // namespace TextureAssociationUtils
