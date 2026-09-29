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
std::vector<MeshIOTextureAsset> makeTextureAssetsFromSavedImages(
    const QStringList &paths,
    const std::vector<QImage> &images);

QStringList associatedTexturePaths(const Document::MeshEntry &entry);
bool loadAssociatedTextureImage(
    const Document::MeshEntry &entry,
    int textureIndex,
    QImage &image,
    QString &error);
bool saveImages(const QStringList &paths, const std::vector<QImage> &images, QString &error);

void rebuildLegacyTextureAssociation(Document::MeshEntry &entry);
void replaceTextureAssociations(
    Document::MeshEntry &entry,
    const std::vector<MeshIOTextureAsset> &assets);
void appendTextureAssociations(
    Document::MeshEntry &entry,
    const std::vector<MeshIOTextureAsset> &assets);
void ensureMaterialSlotCount(Document::MeshEntry &entry, int count);
int ensureTextureListed(Document::MeshEntry &entry, const QString &path);

} // namespace TextureAssociationUtils
