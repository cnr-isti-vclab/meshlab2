#include "filedialogdirectory.h"
#include "textureassociationutils.h"
#include "layerwidget.h"
#include "document.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/allocate.h>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDateTime>
#include <QHash>
#include <QHBoxLayout>
#include <QImageReader>
#include <QImageWriter>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMetaObject>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStackedWidget>
#include <QSplitter>
#include <QToolTip>
#include <functional>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
constexpr int kFirstColumnMinWidth = 56;
constexpr int kFirstColumnPadding = 10;
constexpr int kFirstColumnTreePadding = 28;
constexpr int kRoleLayerKind = Qt::UserRole;
constexpr int kRoleMeshIndex = Qt::UserRole + 1;
constexpr int kRoleMeshId = Qt::UserRole + 2;
constexpr int kRoleRasterIndex = Qt::UserRole + 3;
constexpr int kRoleRasterId = Qt::UserRole + 4;
constexpr int kRolePlaneIndex = Qt::UserRole + 5;
constexpr int kLayerKindMesh = 1;
constexpr int kLayerKindRaster = 2;

struct MeshCustomAttributeInfo {
    QStringList vertexScalars;
    QStringList vertexColors;
    QStringList vertexPoints;
    QStringList faceScalars;
    QStringList faceColors;
    QStringList facePoints;

    int vertexCount() const
    {
        return vertexScalars.size() + vertexColors.size() + vertexPoints.size();
    }
    int faceCount() const
    {
        return faceScalars.size() + faceColors.size() + facePoints.size();
    }
};

struct LayerTextureInfo {
    QString displayName;
    QString path;
    QStringList usage;
    QImage image;       // in-memory texture (e.g. a generated dummy) when there is no file
    int assetIndex = -1; // index into MeshEntry::textureAssets, -1 when unknown
};

std::vector<LayerTextureInfo> collectLayerTextures(const Document::MeshEntry &entry);

// Draws an open/closed eye icon in place of the standard checkbox indicator.
// Modifier-key clicks on the eye are intercepted in editorEvent and routed
// to the provided callback instead of toggling the item's own check state.
class EyeCheckDelegate : public QStyledItemDelegate
{
public:
    enum class EyeAction { HideOthers, ShowOthers, InvertAll };
    using ModifierCallback = std::function<void(const QModelIndex &, EyeAction)>;

    EyeCheckDelegate(ModifierCallback cb, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), m_modifierCb(std::move(cb)) {}

    // Intercept mouse-press/release events that land on the eye icon area.
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (event->type() != QEvent::MouseButtonPress
            && event->type() != QEvent::MouseButtonRelease)
            return QStyledItemDelegate::editorEvent(event, model, option, index);

        auto *me = static_cast<QMouseEvent *>(event);
        const Qt::KeyboardModifiers mods = me->modifiers();
        const bool hasModifier = (mods & (Qt::ShiftModifier | Qt::AltModifier | Qt::ControlModifier)) != 0;
        if (!hasModifier && event->type() == QEvent::MouseButtonRelease && m_modifierPressed) {
            // Consume the release following a consumed modifier press
            m_modifierPressed = false;
            return true;
        }
        if (!hasModifier) {
            m_modifierPressed = false;
            return QStyledItemDelegate::editorEvent(event, model, option, index);
        }

        // Determine whether the click lands on the eye icon.
        const QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QRect checkRect = style->subElementRect(
            QStyle::SE_ItemViewItemCheckIndicator, &opt, option.widget);
        if (!checkRect.contains(me->pos()))
            return QStyledItemDelegate::editorEvent(event, model, option, index);

        // Modifier-click on eye: invoke callback, do NOT toggle this item.
        if (event->type() == QEvent::MouseButtonPress && m_modifierCb) {
            m_modifierPressed = true;
            EyeAction action;
            if (mods & Qt::ShiftModifier)
                action = EyeAction::InvertAll;
            else if (mods & Qt::AltModifier)
                action = EyeAction::ShowOthers;
            else // Qt::ControlModifier == Command on macOS
                action = EyeAction::HideOthers;
            m_modifierCb(index, action);
        }
        return true; // consumed
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        if (!index.data(Qt::CheckStateRole).isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);

        // Determine the checkbox rect before suppressing the indicator.
        const QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        const QRect checkRect = style->subElementRect(
            QStyle::SE_ItemViewItemCheckIndicator, &opt, option.widget);

        // Remove checkbox so the base class does not draw it.
        opt.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        QStyledItemDelegate::paint(painter, opt, index);

        const bool visible = (index.data(Qt::CheckStateRole).toInt() == Qt::Checked);
        const QPixmap &px = visible ? eyeOpen() : eyeClose();
        if (!px.isNull()) {
            const QPixmap scaled = px.scaled(checkRect.size(),
                Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const int dx = (checkRect.width()  - scaled.width())  / 2;
            const int dy = (checkRect.height() - scaled.height()) / 2;
            painter->drawPixmap(checkRect.topLeft() + QPoint(dx, dy), scaled);
        }
    }

private:
    static const QPixmap &eyeOpen()
    {
        static const QPixmap px(QStringLiteral(":/img/layer_eye_open.png"));
        return px;
    }
    static const QPixmap &eyeClose()
    {
        static const QPixmap px(QStringLiteral(":/img/layer_eye_close.png"));
        return px;
    }

    ModifierCallback m_modifierCb;
    bool m_modifierPressed = false;
};

template<typename T>
QStringList collectPerVertexAttributeNames(VCGMesh &mesh)
{
    std::vector<std::string> names;
    vcg::tri::Allocator<VCGMesh>::GetAllPerVertexAttribute<T>(mesh, names);
    QStringList out;
    out.reserve(int(names.size()));
    for (const std::string &name : names) {
        if (name.empty())
            continue;
        out.push_back(QString::fromStdString(name));
    }
    out.removeDuplicates();
    out.sort(Qt::CaseInsensitive);
    return out;
}

template<typename T>
QStringList collectPerFaceAttributeNames(VCGMesh &mesh)
{
    std::vector<std::string> names;
    vcg::tri::Allocator<VCGMesh>::GetAllPerFaceAttribute<T>(mesh, names);
    QStringList out;
    out.reserve(int(names.size()));
    for (const std::string &name : names) {
        if (name.empty())
            continue;
        out.push_back(QString::fromStdString(name));
    }
    out.removeDuplicates();
    out.sort(Qt::CaseInsensitive);
    return out;
}

MeshCustomAttributeInfo collectCustomAttributes(const VCGMesh &mesh)
{
    VCGMesh &mutableMesh = const_cast<VCGMesh &>(mesh);
    MeshCustomAttributeInfo attrs;
    attrs.vertexScalars = collectPerVertexAttributeNames<float>(mutableMesh);
    attrs.vertexColors = collectPerVertexAttributeNames<vcg::Color4b>(mutableMesh);
    attrs.vertexPoints = collectPerVertexAttributeNames<vcg::Point3f>(mutableMesh);
    attrs.faceScalars = collectPerFaceAttributeNames<float>(mutableMesh);
    attrs.faceColors = collectPerFaceAttributeNames<vcg::Color4b>(mutableMesh);
    attrs.facePoints = collectPerFaceAttributeNames<vcg::Point3f>(mutableMesh);
    return attrs;
}

QString ownerAttributeSummary(
    const QStringList &scalars,
    const QStringList &colors,
    const QStringList &points)
{
    QStringList parts;
    if (!scalars.isEmpty())
        parts << QObject::tr("S[%1]").arg(scalars.join(QStringLiteral(", ")));
    if (!colors.isEmpty())
        parts << QObject::tr("C[%1]").arg(colors.join(QStringLiteral(", ")));
    if (!points.isEmpty())
        parts << QObject::tr("P[%1]").arg(points.join(QStringLiteral(", ")));
    return parts.join(QStringLiteral("  "));
}

QString ownerAttributeTooltip(
    const QString &ownerLabel,
    const QStringList &scalars,
    const QStringList &colors,
    const QStringList &points)
{
    QStringList lines;
    lines << QObject::tr("%1 custom attributes:").arg(ownerLabel);
    if (!scalars.isEmpty())
        lines << QObject::tr("  Scalar: %1").arg(scalars.join(QStringLiteral(", ")));
    if (!colors.isEmpty())
        lines << QObject::tr("  Color: %1").arg(colors.join(QStringLiteral(", ")));
    if (!points.isEmpty())
        lines << QObject::tr("  Point3: %1").arg(points.join(QStringLiteral(", ")));
    return lines.join(QLatin1Char('\n'));
}

int selectedVertexCount(const VCGMesh &mesh)
{
    int count = 0;
    for (const auto &v : mesh.vert) {
        if (!v.IsD() && v.IsS())
            ++count;
    }
    return count;
}

int selectedEdgeCount(const VCGMesh &mesh)
{
    int count = 0;
    for (const auto &e : mesh.edge) {
        if (!e.IsD() && e.IsS())
            ++count;
    }
    return count;
}

int selectedFaceCount(const VCGMesh &mesh)
{
    int count = 0;
    for (const auto &f : mesh.face) {
        if (!f.IsD() && f.IsS())
            ++count;
    }
    return count;
}

int displayedFaceCount(const Document::MeshEntry &entry)
{
    return entry.polygonFaceCount >= 0 ? entry.polygonFaceCount : entry.mesh.FN();
}

bool isIdentityTransform(const QMatrix4x4 &m, float eps = 1e-6f)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float expected = (r == c) ? 1.0f : 0.0f;
            if (std::abs(m(r, c) - expected) > eps)
                return false;
        }
    }
    return true;
}

QString meshTransformSummary(const QMatrix4x4 &m)
{
    const QVector3D t(m(0, 3), m(1, 3), m(2, 3));
    const QVector3D cx(m(0, 0), m(1, 0), m(2, 0));
    const QVector3D cy(m(0, 1), m(1, 1), m(2, 1));
    const QVector3D cz(m(0, 2), m(1, 2), m(2, 2));
    const float sx = cx.length();
    const float sy = cy.length();
    const float sz = cz.length();
    return QObject::tr("T(%1, %2, %3)  S(%4, %5, %6)")
        .arg(t.x(), 0, 'g', 4)
        .arg(t.y(), 0, 'g', 4)
        .arg(t.z(), 0, 'g', 4)
        .arg(sx, 0, 'g', 4)
        .arg(sy, 0, 'g', 4)
        .arg(sz, 0, 'g', 4);
}

QString meshTransformTooltip(const QMatrix4x4 &m)
{
    QStringList lines;
    lines << QObject::tr("Mesh transform (4x4):");
    for (int r = 0; r < 4; ++r) {
        lines << QStringLiteral("[ %1  %2  %3  %4 ]")
                     .arg(m(r, 0), 0, 'g', 8)
                     .arg(m(r, 1), 0, 'g', 8)
                     .arg(m(r, 2), 0, 'g', 8)
                     .arg(m(r, 3), 0, 'g', 8);
    }
    return lines.join(QLatin1Char('\n'));
}

QString meshDataSummary(const Document::MeshEntry &entry)
{
    using Mask = vcg::tri::io::Mask;

    QStringList tokens;
    const int mask = entry.ioMask;
    const MeshCustomAttributeInfo attrs = collectCustomAttributes(entry.mesh);
    const bool hasFaces = entry.mesh.FN() > 0;

    if ((mask & Mask::IOM_VERTCOLOR) != 0)
        tokens << QObject::tr("VC");
    if ((mask & Mask::IOM_FACECOLOR) != 0)
        tokens << QObject::tr("FC");
    if (hasFaces || (mask & Mask::IOM_VERTNORMAL) != 0)
        tokens << QObject::tr("VN");
    if (hasFaces || (mask & Mask::IOM_FACENORMAL) != 0)
        tokens << QObject::tr("FN");
    if ((mask & Mask::IOM_WEDGNORMAL) != 0)
        tokens << QObject::tr("WN");
    if ((mask & Mask::IOM_VERTTEXCOORD) != 0)
        tokens << QObject::tr("VT");
    if ((mask & Mask::IOM_WEDGTEXCOORD) != 0)
        tokens << QObject::tr("WT");
    if ((mask & Mask::IOM_WEDGTEXMULTI) != 0)
        tokens << QObject::tr("MT");
    if ((mask & Mask::IOM_VERTQUALITY) != 0)
        tokens << QObject::tr("VQ");
    if ((mask & Mask::IOM_FACEQUALITY) != 0)
        tokens << QObject::tr("FQ");
    if (!entry.mesh.vert.empty() && entry.mesh.vert[0].IsCurvatureDirEnabled())
        tokens << QObject::tr("CD");
    if ((mask & Mask::IOM_EDGEINDEX) != 0 || entry.mesh.EN() > 0)
        tokens << QObject::tr("EI");
    if (attrs.vertexCount() > 0)
        tokens << QObject::tr("VA %1").arg(attrs.vertexCount());
    if (attrs.faceCount() > 0)
        tokens << QObject::tr("FA %1").arg(attrs.faceCount());

    if (!entry.pluginData.empty())
        tokens << QObject::tr("PD %1").arg(entry.pluginData.size());

    const std::vector<LayerTextureInfo> textures = collectLayerTextures(entry);
    if (!textures.empty()) {
        int foundCount = 0;
        for (const LayerTextureInfo &tex : textures) {
            const QString &path = tex.path;
            if (!tex.image.isNull() || QFileInfo::exists(path))
                ++foundCount;
        }
        tokens << QObject::tr("TX %1/%2").arg(foundCount).arg(textures.size());
    }

    if (tokens.isEmpty())
        return QObject::tr("none");
    return tokens.join(QLatin1Char(' '));
}

QString meshDataTooltip(const Document::MeshEntry &entry, int faceCount)
{
    const MeshCustomAttributeInfo attrs = collectCustomAttributes(entry.mesh);
    QStringList lines;
    lines << QObject::tr("Data: %1").arg(meshDataSummary(entry));
    const QString triangleDetail =
        (entry.ioMask & vcg::tri::io::Mask::IOM_BITPOLYGONAL) != 0
            ? QObject::tr(" T=%1").arg(entry.mesh.FN())
            : QString();
    lines << QObject::tr("Counts: V=%1 F=%2%3 E=%4")
                 .arg(entry.mesh.VN())
                 .arg(faceCount)
                 .arg(triangleDetail)
                 .arg(entry.mesh.EN());
    lines << QObject::tr("Selected: V=%1 F=%2 E=%3")
                 .arg(selectedVertexCount(entry.mesh))
                 .arg(selectedFaceCount(entry.mesh))
                 .arg(selectedEdgeCount(entry.mesh));
    if (!isIdentityTransform(entry.transform))
        lines << QObject::tr("Transform: %1").arg(meshTransformSummary(entry.transform));
    if (attrs.vertexCount() > 0) {
        lines << QObject::tr("Vertex custom attributes: %1")
                     .arg(ownerAttributeSummary(attrs.vertexScalars, attrs.vertexColors, attrs.vertexPoints));
    }
    if (attrs.faceCount() > 0) {
        lines << QObject::tr("Face custom attributes: %1")
                     .arg(ownerAttributeSummary(attrs.faceScalars, attrs.faceColors, attrs.facePoints));
    }
    if (!entry.pluginData.empty()) {
        lines << QObject::tr("Plugin data:");
        for (const auto &[ownerKey, data] : entry.pluginData) {
            lines << QObject::tr("  %1: %2")
                         .arg(ownerKey, data ? data->describe() : QObject::tr("(empty)"));
        }
    }
    const std::vector<LayerTextureInfo> textures = collectLayerTextures(entry);
    if (!textures.empty()) {
        lines << QObject::tr("Textures:");
        for (size_t i = 0; i < textures.size(); ++i) {
            const LayerTextureInfo &tex = textures[i];
            const bool exists = !tex.image.isNull()
                || (!tex.path.isEmpty() && QFileInfo::exists(tex.path));
            const QString usage = tex.usage.isEmpty()
                ? QString()
                : QObject::tr(" (%1)").arg(tex.usage.join(QStringLiteral(", ")));
            lines << QObject::tr("  [%1] %2%3 (%4)")
                        .arg(i)
                        .arg(tex.displayName)
                        .arg(usage)
                        .arg(exists ? QObject::tr("found") : QObject::tr("missing"));
        }
    }
    return lines.join(QLatin1Char('\n'));
}

// assetIndex is the position in MeshEntry::textureAssets this ref was built from,
// or -1 when that correspondence is not known (importer-provided material sets).
void appendLayerTexture(
    std::vector<LayerTextureInfo> &out,
    QHash<QString, int> &byKey,
    const MeshIOMaterialTextureRef &ref,
    const QString &usage,
    int assetIndex = -1)
{
    if (!ref.isValid())
        return;

    const QString path = QDir::fromNativeSeparators(ref.filePath.trimmed());
    QString displayName = QFileInfo(ref.fileName.trimmed()).fileName();
    if (displayName.isEmpty())
        displayName = QFileInfo(path).fileName();
    if (displayName.isEmpty())
        displayName = QObject::tr("texture");

    // Identity key: a file path when there is one, else the asset index — keying
    // path-less textures by name would collapse two distinct in-memory textures
    // that happen to share a name into a single row.
    const QString key = !path.isEmpty()
        ? QStringLiteral("path:%1").arg(path.toLower())
        : (assetIndex >= 0 ? QStringLiteral("asset:%1").arg(assetIndex)
                           : QStringLiteral("name:%1").arg(displayName.toLower()));
    const auto it = byKey.constFind(key);
    if (it != byKey.constEnd()) {
        LayerTextureInfo &existing = out[size_t(it.value())];
        if (existing.path.isEmpty() && !path.isEmpty())
            existing.path = path;
        if (existing.assetIndex < 0)
            existing.assetIndex = assetIndex;
        if (!usage.isEmpty() && !existing.usage.contains(usage))
            existing.usage.push_back(usage);
        return;
    }

    LayerTextureInfo info;
    info.displayName = displayName;
    info.path = path;
    info.assetIndex = assetIndex;
    if (!usage.isEmpty())
        info.usage.push_back(usage);
    byKey.insert(key, int(out.size()));
    out.push_back(std::move(info));
}

std::vector<LayerTextureInfo> collectLayerTextures(const Document::MeshEntry &entry)
{
    std::vector<LayerTextureInfo> out;
    QHash<QString, int> byKey;

    if (!entry.materialSet.empty()) {
        for (int slotIndex = 0; slotIndex < int(entry.materialSet.entries.size()); ++slotIndex) {
            const MeshIOMaterialSlot &slot = entry.materialSet.entries[size_t(slotIndex)];
            const QString materialName = slot.name.trimmed();
            const QString prefix = materialName.isEmpty() ? QString() : materialName + QStringLiteral(":");
            appendLayerTexture(out, byKey, slot.baseColorTexture,
                prefix + QObject::tr("Base"), slot.baseColorTexture.assetIndex);
            appendLayerTexture(out, byKey, slot.normalTexture,
                prefix + QObject::tr("Normal"), slot.normalTexture.assetIndex);
            appendLayerTexture(out, byKey, slot.occlusionTexture,
                prefix + QObject::tr("AO"), slot.occlusionTexture.assetIndex);
            appendLayerTexture(out, byKey, slot.roughnessTexture,
                prefix + QObject::tr("Rough"), slot.roughnessTexture.assetIndex);
        }
    }

    if (out.empty()) {
        for (int i = 0; i < entry.textureFilePaths.size(); ++i) {
            MeshIOMaterialTextureRef ref;
            ref.filePath = entry.textureFilePaths.at(i);
            if (i >= 0 && i < entry.textureFileNames.size())
                ref.fileName = entry.textureFileNames.at(i);
            appendLayerTexture(out, byKey, ref, QObject::tr("Base"), i);
        }
    }

    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].displayName.trimmed().isEmpty())
            out[i].displayName = QObject::tr("texture_%1").arg(int(i));
    }

    // Resolve in-memory images (e.g. dummy textures with no file on disk) so the
    // thumbnail can render them instead of falling back to a placeholder.
    // Resolution order, strongest evidence first:
    //   1. source path  — authoritative whenever the texture is backed by a file;
    //   2. asset index  — the same link the renderer uses (textureAssets[group]),
    //                     and the only dependable one for path-less textures;
    //   3. name         — last resort for importer-built material sets, whose
    //                     slots need not correspond 1:1 to textureAssets.
    // Name matching alone is fragile: displayName is QFileInfo(fileName).fileName(),
    // so any asset name containing a separator, or a slot labelled differently from
    // its asset, silently yields a blank thumbnail and a "missing" size.
    for (LayerTextureInfo &info : out) {
        const MeshIOTextureAsset *match = nullptr;

        if (!info.path.isEmpty()) {
            for (const MeshIOTextureAsset &asset : entry.textureAssets) {
                if (asset.hasImage()
                    && QDir::fromNativeSeparators(asset.sourcePath.trimmed()) == info.path) {
                    match = &asset;
                    break;
                }
            }
        }

        if (!match && info.assetIndex >= 0
            && info.assetIndex < int(entry.textureAssets.size())) {
            const MeshIOTextureAsset &asset = entry.textureAssets[size_t(info.assetIndex)];
            const QString assetPath = QDir::fromNativeSeparators(asset.sourcePath.trimmed());
            // Trust the index only where it cannot contradict a known path.
            if (asset.hasImage()
                && (assetPath.isEmpty() || info.path.isEmpty() || assetPath == info.path))
                match = &asset;
        }

        if (!match && info.path.isEmpty()) {
            for (const MeshIOTextureAsset &asset : entry.textureAssets) {
                if (asset.hasImage() && !asset.name.trimmed().isEmpty()
                    && asset.name.trimmed().compare(info.displayName, Qt::CaseInsensitive) == 0) {
                    match = &asset;
                    break;
                }
            }
        }

        if (match)
            info.image = match->image;
    }
    return out;
}

QString textureDisplaySize(const LayerTextureInfo &texture)
{
    if (!texture.image.isNull())
        return QStringLiteral("%1x%2").arg(texture.image.width()).arg(texture.image.height());

    const QString &path = texture.path;
    if (path.isEmpty() || !QFileInfo::exists(path))
        return QObject::tr("missing");

    QImageReader reader(path);
    const QSize sz = reader.size();
    if (!sz.isValid())
        return QObject::tr("unknown");

    return QStringLiteral("%1x%2").arg(sz.width()).arg(sz.height());
}

QString rasterSizeText(const QSize &size)
{
    if (!size.isValid() || size.width() <= 0 || size.height() <= 0)
        return QObject::tr("unknown");
    return QStringLiteral("%1x%2").arg(size.width()).arg(size.height());
}

QString rasterCameraTypeLabel(CameraShot::CameraType type)
{
    switch (type) {
    case CameraShot::CameraType::Perspective:  return QObject::tr("Perspective");
    case CameraShot::CameraType::Orthographic: return QObject::tr("Orthographic");
    case CameraShot::CameraType::Isometric:    return QObject::tr("Isometric");
    case CameraShot::CameraType::Cavalieri:    return QObject::tr("Cavalieri");
    }
    return QObject::tr("Camera");
}

QString rasterCameraSummary(const Document::RasterEntry &entry)
{
    if (!entry.shot.isValid())
        return QObject::tr("—");

    const QVector3D eye = entry.shot.viewPoint();
    return QObject::tr("%1  eye(%2, %3, %4)")
        .arg(rasterCameraTypeLabel(entry.shot.cameraType()))
        .arg(eye.x(), 0, 'g', 4)
        .arg(eye.y(), 0, 'g', 4)
        .arg(eye.z(), 0, 'g', 4);
}

QString rasterCameraTooltip(const Document::RasterEntry &entry)
{
    if (!entry.shot.isValid())
        return QObject::tr("Raster camera: none");

    const QVector3D eye = entry.shot.viewPoint();
    const QVector3D dir = entry.shot.viewDirection();
    const QSize viewport = entry.shot.viewportPx();
    const QVector2D center = entry.shot.centerPx();
    const QVector2D pixelSize = entry.shot.pixelSizeMm();
    QStringList lines;
    lines << QObject::tr("Raster camera: %1").arg(rasterCameraTypeLabel(entry.shot.cameraType()));
    lines << QObject::tr("Viewport: %1").arg(rasterSizeText(viewport));
    lines << QObject::tr("Center: %1, %2 px").arg(center.x(), 0, 'g', 6).arg(center.y(), 0, 'g', 6);
    lines << QObject::tr("Focal: %1").arg(entry.shot.focalMm(), 0, 'g', 6);
    lines << QObject::tr("Pixel size: %1, %2")
                 .arg(pixelSize.x(), 0, 'g', 6)
                 .arg(pixelSize.y(), 0, 'g', 6);
    lines << QObject::tr("Eye: %1, %2, %3")
                 .arg(eye.x(), 0, 'g', 6)
                 .arg(eye.y(), 0, 'g', 6)
                 .arg(eye.z(), 0, 'g', 6);
    lines << QObject::tr("View dir: %1, %2, %3")
                 .arg(dir.x(), 0, 'g', 6)
                 .arg(dir.y(), 0, 'g', 6)
                 .arg(dir.z(), 0, 'g', 6);
    return lines.join(QLatin1Char('\n'));
}

QString rasterPlaneSummary(const RasterPlane &plane)
{
    QString summary = rasterSizeText(plane.size);
    const QString sourcePath = Document::rasterPlaneSourcePath(plane);
    if (!sourcePath.isEmpty())
        summary += QObject::tr("  %1").arg(QFileInfo(sourcePath).fileName());
    return summary;
}

QString rasterDataSummary(const Document::RasterEntry &entry)
{
    QStringList tokens;
    const RasterPlane *plane = entry.currentPlane();
    if (plane)
        tokens << rasterSizeText(plane->size);
    tokens << QObject::tr("PL %1").arg(entry.planes.size());
    if (entry.shot.isValid())
        tokens << QObject::tr("CAM");
    return tokens.join(QLatin1Char(' '));
}

QString rasterDataTooltip(const Document::RasterEntry &entry)
{
    QStringList lines;
    lines << QObject::tr("Raster: %1").arg(entry.name);
    if (!entry.sourcePath.isEmpty())
        lines << QObject::tr("Source: %1").arg(entry.sourcePath);
    lines << QObject::tr("Data: %1").arg(rasterDataSummary(entry));
    lines << QObject::tr("Image revision: %1").arg(entry.imageRevision);
    lines << QObject::tr("Camera revision: %1").arg(entry.cameraRevision);
    lines << rasterCameraTooltip(entry);
    return lines.join(QLatin1Char('\n'));
}

QPixmap textureThumbnail(const QString &path, int w, int h)
{
    w = std::max(8, w);
    h = std::max(8, h);
    const QString cacheKey = QStringLiteral("%1|%2x%3").arg(path).arg(w).arg(h);
    static QHash<QString, QPixmap> cache;
    const auto it = cache.constFind(cacheKey);
    if (it != cache.constEnd())
        return it.value();

    // Check disk cache first
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        const QFileInfo srcInfo(path);
        const QString thumbDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
            + QStringLiteral("/raster_thumbnails");
        const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex());
        const QString cachePath = thumbDir
            + QStringLiteral("/%1_%2x%3.png").arg(hash).arg(w).arg(h);
        if (QFileInfo::exists(cachePath)) {
            const QFileInfo cacheInfo(cachePath);
            if (cacheInfo.lastModified() >= srcInfo.lastModified()) {
                QPixmap cached;
                if (cached.load(cachePath)) {
                    cache.insert(cacheKey, cached);
                    return cached;
                }
            }
        }
    }

    QPixmap out(w, h);
    out.fill(QColor(30, 30, 30));
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);

    bool ok = false;
    QString cachePath;
    if (!path.isEmpty() && QFileInfo::exists(path)) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize native = reader.size();
        if (native.isValid())
            reader.setScaledSize(native.scaled(w, h, Qt::KeepAspectRatio));
        QImage img = reader.read();
        if (img.isNull()) {
            // Scaled reads are the cheap path and worth keeping, but they only work for
            // formats Qt itself decodes. Anything it declines comes back full size
            // through the fallback and gets scaled below like everything else.
            QString imageError;
            TextureAssociationUtils::readImageFile(path, img, imageError);
        }
        if (!img.isNull()) {
            QImage normalized = img;
            normalized.setDevicePixelRatio(1.0);
            const QImage scaled =
                normalized.scaled(w, h, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const QPixmap tex = QPixmap::fromImage(scaled);
            const int x = (w - tex.width()) / 2;
            const int y = (h - tex.height()) / 2;
            p.drawPixmap(x, y, tex);
            ok = true;

            // Persist to disk cache
            const QString thumbDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                + QStringLiteral("/raster_thumbnails");
            const QString hash = QString::fromLatin1(
                QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex());
            cachePath = thumbDir
                + QStringLiteral("/%1_%2x%3.png").arg(hash).arg(w).arg(h);
            QDir().mkpath(thumbDir);
            out.save(cachePath, "PNG");
        }
    }

    if (!ok) {
        p.fillRect(out.rect(), QColor(45, 45, 48));
        p.setPen(QColor(110, 110, 115));
        p.drawLine(2, 2, w - 3, h - 3);
        p.drawLine(2, h - 3, w - 3, 2);
    }

    p.setPen(QColor(95, 95, 100));
    p.drawRect(out.rect().adjusted(0, 0, -1, -1));

    cache.insert(cacheKey, out);
    return out;
}

QPixmap imageThumbnail(const QImage &image, const QString &path, int w, int h)
{
    w = std::max(8, w);
    h = std::max(8, h);
    if (!image.isNull()) {
        QPixmap out(w, h);
        out.fill(QColor(30, 30, 30));
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing, true);
        QImage normalized = image;
        normalized.setDevicePixelRatio(1.0);
        // Smooth (area-averaging) scaling is right for mild downscales, but past a
        // large factor it averages high-frequency content into flat grey — a 512px
        // checkerboard with 8px checks shown at ~54px turns into a uniform square.
        // Nearest-neighbour keeps the pattern recognisable at those ratios.
        const int srcMax = std::max(normalized.width(), normalized.height());
        const bool extremeDownscale = srcMax > 6 * std::max(w, h);
        const QImage scaled = normalized.scaled(
            w, h, Qt::KeepAspectRatio,
            extremeDownscale ? Qt::FastTransformation : Qt::SmoothTransformation);
        const QPixmap px = QPixmap::fromImage(scaled);
        const int x = (w - px.width()) / 2;
        const int y = (h - px.height()) / 2;
        p.drawPixmap(x, y, px);
        p.setPen(QColor(95, 95, 100));
        p.drawRect(out.rect().adjusted(0, 0, -1, -1));
        return out;
    }

    return textureThumbnail(path, w, h);
}

QWidget *textureInfoWidget(
    QTreeWidget *owner,
    const LayerTextureInfo &textureInfo,
    const QFontMetrics &fm)
{
    const QString path = textureInfo.path;
    const QString name = textureInfo.displayName;
    const QString size = textureDisplaySize(textureInfo);
    const QString usageText = textureInfo.usage.join(QStringLiteral(", "));
    const int lineH = std::max(8, fm.lineSpacing());
    const int thumbSide = std::max(14, lineH * 2);

    auto *container = new QWidget(owner);
    auto *h = new QHBoxLayout(container);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);

    auto *thumb = new QLabel(container);
    thumb->setFixedSize(thumbSide, thumbSide);
    thumb->setPixmap(imageThumbnail(textureInfo.image, path, thumbSide, thumbSide));

    auto *textCol = new QWidget(container);
    auto *v = new QVBoxLayout(textCol);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *nameLabel = new QLabel(name, textCol);
    nameLabel->setTextFormat(Qt::PlainText);
    nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nameLabel->setFixedHeight(lineH);

    QString metaLine = size;
    if (!usageText.isEmpty())
        metaLine = QObject::tr("%1  %2").arg(size, usageText);

    auto *sizeLabel = new QLabel(metaLine, textCol);
    sizeLabel->setTextFormat(Qt::PlainText);
    sizeLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    sizeLabel->setFixedHeight(lineH);
    sizeLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));

    v->addWidget(nameLabel);
    v->addWidget(sizeLabel);

    h->addWidget(thumb, 0, Qt::AlignVCenter);
    h->addWidget(textCol, 1);

    QString tooltip;
    if (!path.isEmpty())
        tooltip = path;
    if (!textureInfo.usage.isEmpty()) {
        const QString usageLine = QObject::tr("Usage: %1").arg(textureInfo.usage.join(QStringLiteral(", ")));
        tooltip = tooltip.isEmpty() ? usageLine : (tooltip + QLatin1Char('\n') + usageLine);
    }
    if (!tooltip.isEmpty()) {
        container->setToolTip(tooltip);
        thumb->setToolTip(tooltip);
        nameLabel->setToolTip(tooltip);
        sizeLabel->setToolTip(tooltip);
    }

    return container;
}

QWidget *rasterPlaneInfoWidget(
    QTreeWidget *owner,
    const RasterPlane &plane,
    int planeIndex,
    const QFontMetrics &fm)
{
    const QString path = Document::rasterPlaneSourcePath(plane);
    const QString name = Document::rasterPlaneDisplayName(plane, planeIndex);
    const int lineH = std::max(8, fm.lineSpacing());
    const int thumbH = std::max(14, lineH * 2);

    const QSize imgSize = !plane.image.isNull() ? plane.image.size() : plane.size;
    const int thumbW = (imgSize.width() > 0 && imgSize.height() > 0)
        ? std::max(14, int(qreal(thumbH) * imgSize.width() / imgSize.height()))
        : thumbH;

    auto *container = new QWidget(owner);
    auto *h = new QHBoxLayout(container);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(6);

    auto *thumb = new QLabel(container);
    thumb->setFixedSize(thumbW, thumbH);
    thumb->setPixmap(imageThumbnail(plane.image, path, thumbW, thumbH));

    auto *textCol = new QWidget(container);
    auto *v = new QVBoxLayout(textCol);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    auto *nameLabel = new QLabel(name, textCol);
    nameLabel->setTextFormat(Qt::PlainText);
    nameLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    nameLabel->setFixedHeight(lineH);

    auto *metaLabel = new QLabel(rasterPlaneSummary(plane), textCol);
    metaLabel->setTextFormat(Qt::PlainText);
    metaLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    metaLabel->setFixedHeight(lineH);
    metaLabel->setStyleSheet(QStringLiteral("color: palette(mid);"));

    v->addWidget(nameLabel);
    v->addWidget(metaLabel);

    h->addWidget(thumb, 0, Qt::AlignVCenter);
    h->addWidget(textCol, 1);

    QStringList tooltipLines;
    tooltipLines << name;
    tooltipLines << rasterPlaneSummary(plane);
    if (!path.isEmpty())
        tooltipLines << path;
    const QString tooltip = tooltipLines.join(QLatin1Char('\n'));
    container->setToolTip(tooltip);
    thumb->setToolTip(tooltip);
    nameLabel->setToolTip(tooltip);
    metaLabel->setToolTip(tooltip);

    return container;
}

QString layerItemKey(int kind, qulonglong id)
{
    if (kind == 0 || id == 0)
        return QString();
    return QStringLiteral("%1:%2").arg(kind).arg(id);
}

QString layerItemKey(QTreeWidgetItem *item)
{
    if (!item)
        return QString();
    QTreeWidgetItem *top = item;
    while (top->parent())
        top = top->parent();

    bool kindOk = false;
    const int kind = top->data(0, kRoleLayerKind).toInt(&kindOk);
    if (!kindOk)
        return QString();

    bool idOk = false;
    const qulonglong id = (kind == kLayerKindRaster)
        ? top->data(0, kRoleRasterId).toULongLong(&idOk)
        : top->data(0, kRoleMeshId).toULongLong(&idOk);
    return idOk ? layerItemKey(kind, id) : QString();
}

// Numeric sort helper for table columns
class NumericTableItem : public QTableWidgetItem
{
public:
    NumericTableItem(const QString &text, qlonglong value)
        : QTableWidgetItem(text), m_value(value) {}
    bool operator<(const QTableWidgetItem &other) const override
    {
        const auto *o = dynamic_cast<const NumericTableItem *>(&other);
        return o ? m_value < o->m_value : QTableWidgetItem::operator<(other);
    }
private:
    qlonglong m_value;
};
}

// ============================================================================
// LayerWidget implementation
// ============================================================================

// Helper: create the modifier-key eye callback shared by both tree and table views.
static auto makeEyeModifierCallback(Document *doc, int kind) {
    return [doc, kind](const QModelIndex &index, EyeCheckDelegate::EyeAction action) {
        Q_UNUSED(index)
        auto layerCount = [&]() {
            return (kind == kLayerKindRaster) ? doc->rasterCount() : doc->meshCount();
        };
        auto layerVisible = [&](int i) {
            return (kind == kLayerKindRaster) ? doc->raster(i).visible : doc->mesh(i).visible;
        };
        auto applyVisibility = [&](int i, bool v) {
            if (kind == kLayerKindRaster)
                doc->setRasterVisible(i, v);
            else
                doc->setMeshVisible(i, v);
        };

        // Find the self index from the model index's data role
        int selfIdx = -1;
        if (kind == kLayerKindRaster) {
            bool ok = false;
            selfIdx = index.data(kRoleRasterIndex).toInt(&ok);
            if (!ok) selfIdx = -1;
        } else {
            bool ok = false;
            selfIdx = index.data(kRoleMeshIndex).toInt(&ok);
            if (!ok) selfIdx = -1;
        }
        if (selfIdx < 0)
            return;

        switch (action) {
        case EyeCheckDelegate::EyeAction::HideOthers:
            applyVisibility(selfIdx, true);
            for (int i = 0; i < layerCount(); ++i)
                if (i != selfIdx)
                    applyVisibility(i, false);
            break;
        case EyeCheckDelegate::EyeAction::ShowOthers:
            for (int i = 0; i < layerCount(); ++i)
                if (i != selfIdx)
                    applyVisibility(i, true);
            break;
        case EyeCheckDelegate::EyeAction::InvertAll:
            for (int i = 0; i < layerCount(); ++i)
                applyVisibility(i, !layerVisible(i));
            break;
        }
    };
}

LayerWidget::LayerWidget(Document *doc, QWidget *parent)
    : QWidget(parent), m_doc(doc)
{
    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // Stacked widget holding tree (page 0) and table splitter (page 1)
    m_stack = new QStackedWidget(this);
    mainLayout->addWidget(m_stack, 1);

    // --- Tree view (page 0) ---
    auto *treeSplitter = new QSplitter(Qt::Vertical, this);

    m_meshTree = new QTreeWidget(this);
    m_meshTree->setColumnCount(3);
    m_meshTree->setHeaderHidden(true);
    m_meshTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_meshTree->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_meshTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_meshTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_meshTree->setColumnWidth(0, kFirstColumnMinWidth);
    m_meshTree->setItemDelegate(new EyeCheckDelegate(
        makeEyeModifierCallback(doc, kLayerKindMesh), m_meshTree));
    treeSplitter->addWidget(m_meshTree);

    connect(m_meshTree, &QTreeWidget::itemChanged, this, &LayerWidget::onTreeItemChanged);
    connect(m_meshTree, &QTreeWidget::currentItemChanged, this, &LayerWidget::onTreeCurrentItemChanged);

    m_rasterTree = new QTreeWidget(this);
    m_rasterTree->setColumnCount(3);
    m_rasterTree->setHeaderHidden(true);
    m_rasterTree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rasterTree->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_rasterTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_rasterTree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_rasterTree->setColumnWidth(0, kFirstColumnMinWidth);
    m_rasterTree->setItemDelegate(new EyeCheckDelegate(
        makeEyeModifierCallback(doc, kLayerKindRaster), m_rasterTree));
    treeSplitter->addWidget(m_rasterTree);

    connect(m_rasterTree, &QTreeWidget::itemChanged, this, &LayerWidget::onTreeItemChanged);
    connect(m_rasterTree, &QTreeWidget::currentItemChanged, this, &LayerWidget::onTreeCurrentItemChanged);

    treeSplitter->setStretchFactor(0, 3);
    treeSplitter->setStretchFactor(1, 2);

    m_stack->addWidget(treeSplitter); // page 0

    // --- Table view (page 1) ---
    auto *tableSplitter = new QSplitter(Qt::Vertical, this);
    m_stack->addWidget(tableSplitter); // page 1

    // Mesh table — columns: Eye(0) | Name(1) | V(2) | E(3) | F(4) | Data(5)
    m_meshTable = new QTableWidget(0, 6, this);
    m_meshTable->setHorizontalHeaderLabels({QString(), tr("Name"), tr("V"), tr("E"), tr("F"), tr("Data")});
    m_meshTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_meshTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_meshTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_meshTable->setColumnWidth(0, 24);
    m_meshTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_meshTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_meshTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_meshTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_meshTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_meshTable->verticalHeader()->setVisible(false);
    m_meshTable->setSortingEnabled(true);
    m_meshTable->setShowGrid(false);

    auto *meshTableEyeDelegate = new EyeCheckDelegate(
        makeEyeModifierCallback(doc, kLayerKindMesh), m_meshTable);
    m_meshTable->setItemDelegateForColumn(0, meshTableEyeDelegate);

    connect(m_meshTable, &QTableWidget::itemChanged, this, &LayerWidget::onMeshTableCellChanged);
    connect(m_meshTable, &QTableWidget::currentItemChanged, this, &LayerWidget::onMeshTableCurrentItemChanged);

    tableSplitter->addWidget(m_meshTable);

    // Raster table — columns: Eye(0) | Thumb(1) | Name(2) | Size(3) | Planes(4) | Camera(5)
    m_rasterTable = new QTableWidget(0, 6, this);
    m_rasterTable->setHorizontalHeaderLabels({QString(), tr("Thumb"), tr("Name"), tr("Size"), tr("Planes"), tr("Camera")});
    m_rasterTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_rasterTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_rasterTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Fixed);
    m_rasterTable->setColumnWidth(0, 24);
    m_rasterTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    m_rasterTable->setColumnWidth(1, 56);
    m_rasterTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_rasterTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_rasterTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_rasterTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_rasterTable->verticalHeader()->setVisible(false);
    m_rasterTable->verticalHeader()->setDefaultSectionSize(56);
    m_rasterTable->setSortingEnabled(true);
    m_rasterTable->setShowGrid(false);

    auto *rasterTableEyeDelegate = new EyeCheckDelegate(
        makeEyeModifierCallback(doc, kLayerKindRaster), m_rasterTable);
    m_rasterTable->setItemDelegateForColumn(0, rasterTableEyeDelegate);

    connect(m_rasterTable, &QTableWidget::itemChanged, this, &LayerWidget::onRasterTableCellChanged);
    connect(m_rasterTable, &QTableWidget::currentItemChanged, this, &LayerWidget::onRasterTableCurrentItemChanged);

    tableSplitter->addWidget(m_rasterTable);

    // Set initial splitter proportions (60/40)
    tableSplitter->setStretchFactor(0, 3);
    tableSplitter->setStretchFactor(1, 2);

    // Document connections — always rebuild
    connect(m_doc, &Document::meshAdded, this, [this](int) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::meshRemoved, this, &LayerWidget::rebuild);
    connect(m_doc, &Document::meshVisibilityChanged, this, [this](int, bool) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::currentMeshChanged, this, [this](int) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::currentLayerChanged, this, [this](CurrentLayerKind, int) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::meshDataChanged, this, [this](int) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::rasterAdded, this, [this](int) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::rasterRemoved, this, &LayerWidget::rebuild);
    connect(m_doc, &Document::rasterVisibilityChanged, this, [this](int, bool) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::currentRasterChanged, this, [this](int) {
        scheduleRebuild();
    });
    connect(m_doc, &Document::rasterDataChanged, this, [this](int) {
        scheduleRebuild();
    });

    rebuild();
}

void LayerWidget::setViewMode(ViewMode mode)
{
    if (m_viewMode == mode)
        return;
    m_viewMode = mode;
    m_stack->setCurrentIndex(mode == ViewMode::Table ? 1 : 0);
    rebuild();
}

void LayerWidget::toggleViewMode()
{
    setViewMode(m_viewMode == ViewMode::Tree ? ViewMode::Table : ViewMode::Tree);
}

void LayerWidget::scheduleRebuild()
{
    if (m_rebuildPending)
        return;
    m_rebuildPending = true;
    QMetaObject::invokeMethod(this, [this]() {
        m_rebuildPending = false;
        rebuild();
    }, Qt::QueuedConnection);
}

void LayerWidget::rebuild()
{
    if (m_viewMode == ViewMode::Table)
        rebuildTable();
    else
        rebuildTree();
}

// ============================================================================
// Tree view
// ============================================================================

void LayerWidget::rebuildTree()
{
    m_rebuilding = true;
    const QLocale locale = QLocale::system();

    // Collect expanded state keys from both trees
    QHash<QString, bool> expandedState;
    const auto collectExpanded = [&](QTreeWidget *tree) {
        for (int row = 0; row < tree->topLevelItemCount(); ++row) {
            QTreeWidgetItem *item = tree->topLevelItem(row);
            if (!item) continue;
            const QString key = layerItemKey(item);
            if (!key.isEmpty())
                expandedState.insert(key, item->isExpanded());
        }
    };
    collectExpanded(m_meshTree);
    collectExpanded(m_rasterTree);

    // --- Rebuild mesh tree ---
    {
        QSignalBlocker b(m_meshTree);
        const QFontMetrics fm(m_meshTree->font());
        int maxCountTextWidth = 0;

        m_meshTree->clear();
        QTreeWidgetItem *meshCurrentItem = nullptr;

        for (int i = 0; i < m_doc->meshCount(); ++i) {
            const auto &entry = m_doc->mesh(i);
            const int faceCount = displayedFaceCount(entry);
            const MeshCustomAttributeInfo attrs = collectCustomAttributes(entry.mesh);
            const std::vector<LayerTextureInfo> textures = collectLayerTextures(entry);
            const bool loaded = !entry.sourcePath.trimmed().isEmpty();
            QString displayName;
            if (loaded)
                displayName = QStringLiteral("[D]");
            else
                displayName = QStringLiteral("[G]");
            if (entry.modified)
                displayName.append(QLatin1Char('*'));
            displayName.append(QLatin1Char(' '));
            displayName += entry.name;
            auto *item = new QTreeWidgetItem(m_meshTree, {displayName, QString()});
            const QString itemKey = layerItemKey(kLayerKindMesh, qulonglong(entry.meshId));
            item->setData(0, kRoleLayerKind, kLayerKindMesh);
            item->setData(0, kRoleMeshIndex, i);
            item->setData(0, kRoleMeshId, qulonglong(entry.meshId));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            item->setCheckState(0, entry.visible ? Qt::Checked : Qt::Unchecked);
            item->setFirstColumnSpanned(true);

            const QString vertCountText = locale.toString(static_cast<qlonglong>(entry.mesh.VN()));
            const QString edgeCountText = locale.toString(static_cast<qlonglong>(entry.mesh.EN()));
            const QString faceCountText = locale.toString(static_cast<qlonglong>(faceCount));
            const int selectedVertCount = selectedVertexCount(entry.mesh);
            const int selectedEdgeCountValue = selectedEdgeCount(entry.mesh);
            const int selectedFaceCountValue = selectedFaceCount(entry.mesh);
            const QString selectedVertText =
                selectedVertCount > 0 ? locale.toString(static_cast<qlonglong>(selectedVertCount)) : QString();
            const QString selectedEdgeText =
                selectedEdgeCountValue > 0
                    ? locale.toString(static_cast<qlonglong>(selectedEdgeCountValue))
                    : QString();
            const QString selectedFaceText =
                selectedFaceCountValue > 0
                    ? locale.toString(static_cast<qlonglong>(selectedFaceCountValue))
                    : QString();
            maxCountTextWidth = std::max(maxCountTextWidth, fm.horizontalAdvance(vertCountText));
            maxCountTextWidth = std::max(maxCountTextWidth, fm.horizontalAdvance(edgeCountText));
            maxCountTextWidth = std::max(maxCountTextWidth, fm.horizontalAdvance(faceCountText));

            auto *vItem = new QTreeWidgetItem({vertCountText, tr("Vert"), selectedVertText});
            vItem->setFlags(vItem->flags() & ~Qt::ItemIsSelectable);
            vItem->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            vItem->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->addChild(vItem);
            auto *eItem = new QTreeWidgetItem({edgeCountText, tr("Edge"), selectedEdgeText});
            eItem->setFlags(eItem->flags() & ~Qt::ItemIsSelectable);
            eItem->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            eItem->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->addChild(eItem);
            auto *fItem = new QTreeWidgetItem({faceCountText, tr("Face"), selectedFaceText});
            fItem->setFlags(fItem->flags() & ~Qt::ItemIsSelectable);
            fItem->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            fItem->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->addChild(fItem);
            auto *dItem = new QTreeWidgetItem({QString(), tr("Data"), meshDataSummary(entry)});
            dItem->setFlags(dItem->flags() & ~Qt::ItemIsSelectable);
            item->addChild(dItem);

            if (attrs.vertexCount() > 0) {
                auto *vAttrItem = new QTreeWidgetItem(
                    {QString(), tr("VAttr"), ownerAttributeSummary(attrs.vertexScalars, attrs.vertexColors, attrs.vertexPoints)});
                vAttrItem->setFlags(vAttrItem->flags() & ~Qt::ItemIsSelectable);
                const QString tip = ownerAttributeTooltip(
                    tr("Vertex"),
                    attrs.vertexScalars,
                    attrs.vertexColors,
                    attrs.vertexPoints);
                vAttrItem->setToolTip(0, tip);
                vAttrItem->setToolTip(1, tip);
                vAttrItem->setToolTip(2, tip);
                item->addChild(vAttrItem);
            }

            if (attrs.faceCount() > 0) {
                auto *fAttrItem = new QTreeWidgetItem(
                    {QString(), tr("FAttr"), ownerAttributeSummary(attrs.faceScalars, attrs.faceColors, attrs.facePoints)});
                fAttrItem->setFlags(fAttrItem->flags() & ~Qt::ItemIsSelectable);
                const QString tip = ownerAttributeTooltip(
                    tr("Face"),
                    attrs.faceScalars,
                    attrs.faceColors,
                    attrs.facePoints);
                fAttrItem->setToolTip(0, tip);
                fAttrItem->setToolTip(1, tip);
                fAttrItem->setToolTip(2, tip);
                item->addChild(fAttrItem);
            }

            if (!isIdentityTransform(entry.transform)) {
                auto *xItem = new QTreeWidgetItem(
                    {QString(), tr("Xf"), meshTransformSummary(entry.transform)});
                xItem->setFlags(xItem->flags() & ~Qt::ItemIsSelectable);
                const QString xfTip = meshTransformTooltip(entry.transform);
                xItem->setToolTip(0, xfTip);
                xItem->setToolTip(1, xfTip);
                xItem->setToolTip(2, xfTip);
                item->addChild(xItem);
            }

            for (int texIdx = 0; texIdx < int(textures.size()); ++texIdx) {
                const LayerTextureInfo &tex = textures[size_t(texIdx)];
                auto *tItem = new QTreeWidgetItem({QString(), tr("Tex %1").arg(texIdx), QString()});
                tItem->setFlags(tItem->flags() & ~Qt::ItemIsSelectable);
                QString texTip;
                if (!tex.path.isEmpty())
                    texTip = tex.path;
                if (!tex.usage.isEmpty()) {
                    const QString usageLine = tr("Usage: %1").arg(tex.usage.join(QStringLiteral(", ")));
                    texTip = texTip.isEmpty() ? usageLine : (texTip + QLatin1Char('\n') + usageLine);
                }
                if (!texTip.isEmpty()) {
                    tItem->setToolTip(0, texTip);
                    tItem->setToolTip(1, texTip);
                    tItem->setToolTip(2, texTip);
                }
                item->addChild(tItem);
                m_meshTree->setItemWidget(tItem, 2, textureInfoWidget(m_meshTree, tex, fm));
            }

            const QString dataTip = meshDataTooltip(entry, faceCount);
            item->setToolTip(0, dataTip);
            item->setToolTip(1, dataTip);
            item->setToolTip(2, dataTip);
            dItem->setToolTip(0, dataTip);
            dItem->setToolTip(1, dataTip);
            dItem->setToolTip(2, dataTip);

            const auto it = expandedState.constFind(itemKey);
            item->setExpanded(it != expandedState.constEnd() ? it.value() : true);

            if (m_doc->currentLayerKind() == CurrentLayerKind::Mesh
                && i == m_doc->currentMeshIndex()) {
                meshCurrentItem = item;
            }
        }

        if (meshCurrentItem)
            m_meshTree->setCurrentItem(meshCurrentItem);

        const int requiredWidth =
            maxCountTextWidth + kFirstColumnPadding + m_meshTree->indentation() + kFirstColumnTreePadding;
        m_meshTree->setColumnWidth(0, std::max(kFirstColumnMinWidth, requiredWidth));
        m_meshTree->resizeColumnToContents(1);
    }

    // --- Rebuild raster tree ---
    {
        QSignalBlocker b(m_rasterTree);
        const QFontMetrics fm(m_rasterTree->font());

        m_rasterTree->clear();
        QTreeWidgetItem *rasterCurrentItem = nullptr;

        for (int i = 0; i < m_doc->rasterCount(); ++i) {
            const auto &entry = m_doc->raster(i);
            auto *item = new QTreeWidgetItem(m_rasterTree, {entry.name, QString()});
            const QString itemKey = layerItemKey(kLayerKindRaster, qulonglong(entry.rasterId));
            item->setData(0, kRoleLayerKind, kLayerKindRaster);
            item->setData(0, kRoleRasterIndex, i);
            item->setData(0, kRoleRasterId, qulonglong(entry.rasterId));
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            item->setCheckState(0, entry.visible ? Qt::Checked : Qt::Unchecked);
            item->setFirstColumnSpanned(true);

            const RasterPlane *currentPlane = entry.currentPlane();
            const QString sizeText = currentPlane ? rasterSizeText(currentPlane->size) : tr("unknown");

            auto *imageItem = new QTreeWidgetItem(
                {sizeText, tr("Image"), currentPlane ? Document::rasterPlaneDisplayName(*currentPlane, entry.currentPlaneIndex) : QString()});
            imageItem->setFlags(imageItem->flags() & ~Qt::ItemIsSelectable);
            imageItem->setTextAlignment(0, Qt::AlignRight | Qt::AlignVCenter);
            item->addChild(imageItem);

            auto *cameraItem = new QTreeWidgetItem({QString(), tr("Camera"), rasterCameraSummary(entry)});
            cameraItem->setFlags(cameraItem->flags() & ~Qt::ItemIsSelectable);
            const QString cameraTip = rasterCameraTooltip(entry);
            cameraItem->setToolTip(0, cameraTip);
            cameraItem->setToolTip(1, cameraTip);
            cameraItem->setToolTip(2, cameraTip);
            item->addChild(cameraItem);

            for (int planeIndex = 0; planeIndex < int(entry.planes.size()); ++planeIndex) {
                const RasterPlane &plane = entry.planes[size_t(planeIndex)];
                auto *planeItem = new QTreeWidgetItem({QString(), tr("Plane %1").arg(planeIndex), QString()});
                planeItem->setData(0, kRolePlaneIndex, planeIndex);
                planeItem->setFlags((planeItem->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled)
                    & ~Qt::ItemIsUserCheckable);
                if (planeIndex == entry.currentPlaneIndex) {
                    QFont pf = planeItem->font(1);
                    pf.setBold(true);
                    planeItem->setFont(1, pf);
                    planeItem->setFont(2, pf);
                }
                const QString path = Document::rasterPlaneSourcePath(plane);
                if (!path.isEmpty()) {
                    planeItem->setToolTip(0, path);
                    planeItem->setToolTip(1, path);
                    planeItem->setToolTip(2, path);
                }
                item->addChild(planeItem);
                m_rasterTree->setItemWidget(planeItem, 2, rasterPlaneInfoWidget(m_rasterTree, plane, planeIndex, fm));
            }

            const QString dataTip = rasterDataTooltip(entry);
            item->setToolTip(0, dataTip);
            item->setToolTip(1, dataTip);
            item->setToolTip(2, dataTip);
            imageItem->setToolTip(0, dataTip);
            imageItem->setToolTip(1, dataTip);
            imageItem->setToolTip(2, dataTip);

            const auto it = expandedState.constFind(itemKey);
            item->setExpanded(it != expandedState.constEnd() ? it.value() : true);

            if (m_doc->currentLayerKind() == CurrentLayerKind::Raster
                && i == m_doc->currentRasterIndex()) {
                rasterCurrentItem = item;
            }
        }

        if (rasterCurrentItem)
            m_rasterTree->setCurrentItem(rasterCurrentItem);

        m_rasterTree->setVisible(m_doc->rasterCount() > 0);
    }

    updateCurrentItemVisuals();
    m_rebuilding = false;
}

// ============================================================================
// Table view
// ============================================================================

void LayerWidget::rebuildTable()
{
    m_rebuilding = true;
    const QLocale locale = QLocale::system();
    const auto rowForLayer = [](QTableWidget *table, int role, int layerIndex) {
        for (int row = 0; row < table->rowCount(); ++row) {
            QTableWidgetItem *item = table->item(row, 0);
            if (item && item->data(role).toInt() == layerIndex)
                return row;
        }
        return -1;
    };

    // --- Mesh table ---
    {
        QSignalBlocker meshBlocker(m_meshTable);

        m_meshTable->setSortingEnabled(false);
        m_meshTable->setRowCount(m_doc->meshCount());

        for (int i = 0; i < m_doc->meshCount(); ++i) {
            const auto &entry = m_doc->mesh(i);
            const int faceCount = displayedFaceCount(entry);

            // Eye column (0) — always read from document state
            auto *eyeItem = new QTableWidgetItem();
            eyeItem->setFlags(eyeItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            eyeItem->setData(kRoleMeshIndex, i);
            eyeItem->setCheckState(entry.visible ? Qt::Checked : Qt::Unchecked);
            m_meshTable->setItem(i, 0, eyeItem);

            // Name column (1)
            const bool loaded = !entry.sourcePath.trimmed().isEmpty();
            QString displayName;
            if (loaded)
                displayName = QStringLiteral("[D]");
            else
                displayName = QStringLiteral("[G]");
            if (entry.modified)
                displayName.append(QLatin1Char('*'));
            displayName.append(QLatin1Char(' '));
            displayName += entry.name;

            auto *nameItem = new QTableWidgetItem(displayName);
            nameItem->setData(kRoleMeshIndex, i);
            nameItem->setFlags(nameItem->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            nameItem->setToolTip(meshDataTooltip(entry, faceCount));
            m_meshTable->setItem(i, 1, nameItem);

            // V column (2)
            auto *vItem = new NumericTableItem(
                locale.toString(static_cast<qlonglong>(entry.mesh.VN())),
                static_cast<qlonglong>(entry.mesh.VN()));
            vItem->setFlags(vItem->flags() & ~Qt::ItemIsEditable);
            vItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_meshTable->setItem(i, 2, vItem);

            // E column (3)
            auto *eItem = new NumericTableItem(
                locale.toString(static_cast<qlonglong>(entry.mesh.EN())),
                static_cast<qlonglong>(entry.mesh.EN()));
            eItem->setFlags(eItem->flags() & ~Qt::ItemIsEditable);
            eItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_meshTable->setItem(i, 3, eItem);

            // F column (4)
            auto *fItem = new NumericTableItem(
                locale.toString(static_cast<qlonglong>(faceCount)),
                static_cast<qlonglong>(faceCount));
            fItem->setFlags(fItem->flags() & ~Qt::ItemIsEditable);
            fItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_meshTable->setItem(i, 4, fItem);

            // Data column (5)
            auto *dItem = new QTableWidgetItem(meshDataSummary(entry));
            dItem->setFlags(dItem->flags() & ~Qt::ItemIsEditable);
            m_meshTable->setItem(i, 5, dItem);
        }

        m_meshTable->setSortingEnabled(true);

        const int currentMeshRow =
            rowForLayer(m_meshTable, kRoleMeshIndex, m_doc->currentMeshIndex());
        if (m_doc->currentLayerKind() == CurrentLayerKind::Mesh && currentMeshRow >= 0)
            m_meshTable->selectRow(currentMeshRow);
        else
            m_meshTable->clearSelection();
    }

    // --- Raster table ---
    {
        QSignalBlocker rasterBlocker(m_rasterTable);

        m_rasterTable->setSortingEnabled(false);
        m_rasterTable->setRowCount(m_doc->rasterCount());

        for (int i = 0; i < m_doc->rasterCount(); ++i) {
            const auto &entry = m_doc->raster(i);

            // Eye column (0) — always read from document state
            auto *eyeItem = new QTableWidgetItem();
            eyeItem->setFlags(eyeItem->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
            eyeItem->setData(kRoleRasterIndex, i);
            eyeItem->setCheckState(entry.visible ? Qt::Checked : Qt::Unchecked);
            m_rasterTable->setItem(i, 0, eyeItem);

            // Thumb column (1)
            const RasterPlane *plane = entry.currentPlane();
            const QString sourcePath = plane ? Document::rasterPlaneSourcePath(*plane) : QString();
            const QImage planeImg = plane ? plane->image : QImage();
            auto *thumbLabel = new QLabel();
            thumbLabel->setFixedSize(54, 54);
            thumbLabel->setAlignment(Qt::AlignCenter);
            thumbLabel->setPixmap(imageThumbnail(planeImg, sourcePath, 54, 54));
            m_rasterTable->setCellWidget(i, 1, thumbLabel);

            // Name column (2)
            auto *nameItem = new QTableWidgetItem(entry.name);
            nameItem->setData(kRoleRasterIndex, i);
            nameItem->setFlags(nameItem->flags() | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            nameItem->setToolTip(rasterDataTooltip(entry));
            m_rasterTable->setItem(i, 2, nameItem);

            // Size column (3)
            QString sizeStr = plane ? rasterSizeText(plane->size) : tr("—");
            auto *sizeItem = new QTableWidgetItem(sizeStr);
            sizeItem->setFlags(sizeItem->flags() & ~Qt::ItemIsEditable);
            sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_rasterTable->setItem(i, 3, sizeItem);

            // Planes column (4)
            auto *planesItem = new NumericTableItem(
                locale.toString(static_cast<qlonglong>(entry.planes.size())),
                static_cast<qlonglong>(entry.planes.size()));
            planesItem->setFlags(planesItem->flags() & ~Qt::ItemIsEditable);
            planesItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_rasterTable->setItem(i, 4, planesItem);

            // Camera column (5)
            QString camStr = entry.shot.isValid()
                ? rasterCameraTypeLabel(entry.shot.cameraType())
                : tr("—");
            auto *camItem = new QTableWidgetItem(camStr);
            camItem->setFlags(camItem->flags() & ~Qt::ItemIsEditable);
            camItem->setToolTip(rasterCameraTooltip(entry));
            m_rasterTable->setItem(i, 5, camItem);
        }

        m_rasterTable->setSortingEnabled(true);
        m_rasterTable->setVisible(m_doc->rasterCount() > 0);

        const int currentRasterRow =
            rowForLayer(m_rasterTable, kRoleRasterIndex, m_doc->currentRasterIndex());
        if (m_doc->currentLayerKind() == CurrentLayerKind::Raster && currentRasterRow >= 0)
            m_rasterTable->selectRow(currentRasterRow);
        else
            m_rasterTable->clearSelection();
    }

    m_rebuilding = false;
}

// ============================================================================
// Tree slots
// ============================================================================

void LayerWidget::onTreeItemChanged(QTreeWidgetItem *item, int column)
{
    if (m_rebuilding || !item || column != 0)
        return;
    if (item->parent())
        return;

    const LayerItemRef ref = layerRefForItem(item);
    if (ref.index < 0)
        return;

    const bool visible = (item->checkState(0) == Qt::Checked);
    if (ref.kind == LayerItemKind::Mesh)
        m_doc->setMeshVisible(ref.index, visible);
    else if (ref.kind == LayerItemKind::Raster)
        m_doc->setRasterVisible(ref.index, visible);
}

void LayerWidget::onTreeCurrentItemChanged(QTreeWidgetItem *current, QTreeWidgetItem *)
{
    if (m_rebuilding)
        return;

    // Check if the clicked item is a plane sub-item
    if (current && current->parent()) {
        bool planeOk = false;
        const int planeIndex = current->data(0, kRolePlaneIndex).toInt(&planeOk);
        if (planeOk) {
            const LayerItemRef ref = layerRefForItem(current);
            if (ref.kind == LayerItemKind::Raster && ref.index >= 0) {
                m_doc->setCurrentRasterIndex(ref.index);
                m_doc->setCurrentRasterPlaneIndex(ref.index, planeIndex);
                updateCurrentItemVisuals();
                return;
            }
        }
    }

    const LayerItemRef ref = layerRefForItem(current);
    if (ref.kind == LayerItemKind::Mesh && ref.index >= 0)
        m_doc->setCurrentMeshIndex(ref.index);
    else if (ref.kind == LayerItemKind::Raster && ref.index >= 0)
        m_doc->setCurrentRasterIndex(ref.index);

    updateCurrentItemVisuals();
}

// ============================================================================
// Table slots
// ============================================================================

void LayerWidget::onMeshTableCellChanged(QTableWidgetItem *item)
{
    if (m_rebuilding || !item)
        return;
    // Only the eye column (0) is checkable
    if (item->column() != 0)
        return;

    bool ok = false;
    const int index = item->data(kRoleMeshIndex).toInt(&ok);
    if (!ok || index < 0 || index >= m_doc->meshCount())
        return;

    const bool visible = (item->checkState() == Qt::Checked);
    m_doc->setMeshVisible(index, visible);
}

void LayerWidget::onRasterTableCellChanged(QTableWidgetItem *item)
{
    if (m_rebuilding || !item)
        return;
    // Only the eye column (0) is checkable
    if (item->column() != 0)
        return;

    bool ok = false;
    const int index = item->data(kRoleRasterIndex).toInt(&ok);
    if (!ok || index < 0 || index >= m_doc->rasterCount())
        return;

    const bool visible = (item->checkState() == Qt::Checked);
    m_doc->setRasterVisible(index, visible);
}

void LayerWidget::onMeshTableCurrentItemChanged(QTableWidgetItem *current, QTableWidgetItem *)
{
    if (m_rebuilding || !current)
        return;

    // Get the name column item for this row (column 1 has kRoleMeshIndex)
    QTableWidgetItem *nameItem = m_meshTable->item(current->row(), 1);
    if (!nameItem)
        return;

    bool ok = false;
    const int index = nameItem->data(kRoleMeshIndex).toInt(&ok);
    if (ok && index >= 0 && index < m_doc->meshCount())
        m_doc->setCurrentMeshIndex(index);
}

void LayerWidget::onRasterTableCurrentItemChanged(QTableWidgetItem *current, QTableWidgetItem *)
{
    if (m_rebuilding || !current)
        return;

    QTableWidgetItem *nameItem = m_rasterTable->item(current->row(), 2);
    if (!nameItem)
        return;

    bool ok = false;
    const int index = nameItem->data(kRoleRasterIndex).toInt(&ok);
    if (ok && index >= 0 && index < m_doc->rasterCount())
        m_doc->setCurrentRasterIndex(index);
}

// ============================================================================
// Shared helpers
// ============================================================================

LayerWidget::LayerItemRef LayerWidget::layerRefForItem(QTreeWidgetItem *item) const
{
    if (!item)
        return {};

    QTreeWidgetItem *top = item;
    while (top->parent())
        top = top->parent();

    bool ok = false;
    const int kind = top->data(0, kRoleLayerKind).toInt(&ok);
    if (!ok)
        return {};
    if (kind == kLayerKindMesh) {
        const int idx = top->data(0, kRoleMeshIndex).toInt(&ok);
        return ok ? LayerItemRef{LayerItemKind::Mesh, idx} : LayerItemRef{};
    }
    if (kind == kLayerKindRaster) {
        const int idx = top->data(0, kRoleRasterIndex).toInt(&ok);
        return ok ? LayerItemRef{LayerItemKind::Raster, idx} : LayerItemRef{};
    }
    return {};
}

void LayerWidget::updateCurrentItemVisuals()
{
    const int currentMeshIdx = m_doc->currentMeshIndex();
    const int currentRasterIdx = m_doc->currentRasterIndex();
    const auto setBold = [](QTreeWidgetItem *item, bool bold) {
        QFont f0 = item->font(0), f1 = item->font(1), f2 = item->font(2);
        f0.setBold(bold); f1.setBold(bold); f2.setBold(bold);
        item->setFont(0, f0); item->setFont(1, f1); item->setFont(2, f2);
    };
    for (int i = 0; i < m_meshTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_meshTree->topLevelItem(i);
        const LayerItemRef ref = layerRefForItem(item);
        const bool isCurrent = ref.kind == LayerItemKind::Mesh && currentMeshIdx >= 0 && ref.index == currentMeshIdx;
        setBold(item, isCurrent);
    }
    for (int i = 0; i < m_rasterTree->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_rasterTree->topLevelItem(i);
        const LayerItemRef ref = layerRefForItem(item);
        const bool isCurrent = ref.kind == LayerItemKind::Raster && currentRasterIdx >= 0 && ref.index == currentRasterIdx;
        setBold(item, isCurrent);
    }
}

void LayerWidget::savePlaneImage(int rasterIndex, int planeIndex)
{
    if (rasterIndex < 0 || rasterIndex >= m_doc->rasterCount())
        return;
    const Document::RasterEntry &entry = m_doc->raster(rasterIndex);
    if (planeIndex < 0 || planeIndex >= int(entry.planes.size()))
        return;
    const RasterPlane &plane = entry.planes[size_t(planeIndex)];
    if (plane.image.isNull()) {
        QMessageBox::warning(this, tr("Save Plane Image"), tr("The selected plane has no image data."));
        return;
    }
    const QString defaultName = Document::rasterPlaneDisplayName(plane, planeIndex);
    const QString path = QFileDialog::getSaveFileName(
        this,
        tr("Save Plane Image"),
        FileDialogDirectory::startingPath(QStringLiteral("raster"), defaultName),
        tr("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg);;BMP Image (*.bmp);;All Files (*)"));
    if (path.isEmpty())
        return;
    FileDialogDirectory::remember(QStringLiteral("raster"), path);
    if (!plane.image.save(path)) {
        QMessageBox::critical(this, tr("Save Plane Image"),
            tr("Failed to save image to:\n%1").arg(path));
    }
}

LayerWidget::LayerItemRef LayerWidget::layerRefAt(const QPoint &widgetPos) const
{
    const auto localPos = [this, &widgetPos](const QAbstractScrollArea *view) {
        return view->viewport()->mapFrom(this, widgetPos);
    };
    const auto isUnder = [&](const QAbstractScrollArea *view) {
        return view && view->isVisible()
            && view->viewport()->rect().contains(localPos(view));
    };

    if (m_viewMode == ViewMode::Tree) {
        if (isUnder(m_meshTree))
            return layerRefForItem(m_meshTree->itemAt(localPos(m_meshTree)));
        if (isUnder(m_rasterTree))
            return layerRefForItem(m_rasterTree->itemAt(localPos(m_rasterTree)));
        return {};
    }

    // Both tables can be sorted by any column, so a row number is not a layer number. The
    // layer index travels with the row, in the eye column's role data.
    const auto indexInRow = [](const QTableWidget *table, int row, int role) {
        const QTableWidgetItem *item = table->item(row, 0);
        if (!item)
            return -1;
        bool ok = false;
        const int index = item->data(role).toInt(&ok);
        return ok ? index : -1;
    };

    if (isUnder(m_meshTable)) {
        const int row = m_meshTable->rowAt(localPos(m_meshTable).y());
        const int index = (row >= 0) ? indexInRow(m_meshTable, row, kRoleMeshIndex) : -1;
        if (index >= 0 && index < m_doc->meshCount())
            return LayerItemRef { LayerItemKind::Mesh, index };
    }
    if (isUnder(m_rasterTable)) {
        const int row = m_rasterTable->rowAt(localPos(m_rasterTable).y());
        const int index = (row >= 0) ? indexInRow(m_rasterTable, row, kRoleRasterIndex) : -1;
        if (index >= 0 && index < m_doc->rasterCount())
            return LayerItemRef { LayerItemKind::Raster, index };
    }
    return {};
}

void LayerWidget::contextMenuEvent(QContextMenuEvent *event)
{
    // A raster's planes are children of its row, so they exist only in the tree. Everything
    // below this block works in either view.
    QTreeWidgetItem *itemUnderCursor = nullptr;
    if (m_viewMode == ViewMode::Tree) {
        const QPoint meshPos = m_meshTree->viewport()->mapFrom(this, event->pos());
        const QPoint rasterPos = m_rasterTree->viewport()->mapFrom(this, event->pos());
        if (m_meshTree->viewport()->rect().contains(meshPos))
            itemUnderCursor = m_meshTree->itemAt(meshPos);
        else if (m_rasterTree->viewport()->rect().contains(rasterPos))
            itemUnderCursor = m_rasterTree->itemAt(rasterPos);
    }

    // Right-click on a plane child item
    if (itemUnderCursor && itemUnderCursor->parent()) {
        bool planeOk = false;
        const int planeIndex = itemUnderCursor->data(0, kRolePlaneIndex).toInt(&planeOk);
        if (planeOk) {
            const LayerItemRef ref = layerRefForItem(itemUnderCursor);
            if (ref.kind == LayerItemKind::Raster && ref.index >= 0
                && planeIndex < int(m_doc->raster(ref.index).planes.size())) {
                QMenu menu(this);
                const Document::RasterEntry &entry = m_doc->raster(ref.index);
                QAction *setPlaneAction = menu.addAction(tr("Set as Current Plane"));
                setPlaneAction->setEnabled(entry.currentPlaneIndex != planeIndex);
                connect(setPlaneAction, &QAction::triggered, this,
                    [this, rasterIndex = ref.index, planeIndex]() {
                        m_doc->setCurrentRasterPlaneIndex(rasterIndex, planeIndex);
                    });
                const RasterPlane &plane = entry.planes[size_t(planeIndex)];
                QAction *saveAction = menu.addAction(tr("Save Plane Image..."));
                saveAction->setEnabled(!plane.image.isNull());
                connect(saveAction, &QAction::triggered, this,
                    [this, rasterIndex = ref.index, planeIndex]() {
                        savePlaneImage(rasterIndex, planeIndex);
                    });
                menu.exec(event->globalPos());
                return;
            }
        }
    }

    const LayerItemRef ref = layerRefAt(event->pos());
    if (ref.kind == LayerItemKind::Raster && ref.index >= 0) {
        QMenu menu(this);
        QAction *currentAction = menu.addAction(tr("Set Current Raster"));
        currentAction->setEnabled(
            m_doc->currentLayerKind() != CurrentLayerKind::Raster
            || ref.index != m_doc->currentRasterIndex());
        connect(currentAction, &QAction::triggered, this, [this, index = ref.index]() {
            m_doc->setCurrentRasterIndex(index);
        });
        const Document::RasterEntry &entry = m_doc->raster(ref.index);
        const RasterPlane *plane = entry.currentPlane();
        QAction *saveAction = menu.addAction(tr("Save Current Plane Image..."));
        saveAction->setEnabled(plane != nullptr && !plane->image.isNull());
        connect(saveAction, &QAction::triggered, this,
            [this, index = ref.index]() {
                savePlaneImage(index, m_doc->raster(index).currentPlaneIndex);
            });
        QAction *removeAction = menu.addAction(tr("Remove Raster"));
        connect(removeAction, &QAction::triggered, this, [this, index = ref.index]() {
            m_doc->removeRaster(index);
        });
        menu.exec(event->globalPos());
        return;
    }

    const std::vector<Document::FilterInfo> infos = m_doc->filterInfos();

    QMenu menu(this);
    bool anyAdded = false;
    for (const Document::FilterInfo &info : infos) {
        // Layer-management filters, wherever they sit in their category list.
        if (!info.descriptor.categories.contains(QStringLiteral("Document/Layer"),
                                                 Qt::CaseInsensitive))
            continue;
        QAction *action = menu.addAction(info.descriptor.name);
        action->setData(info.key);
        action->setEnabled(info.applicable);
        if (!info.applicable && !info.applicabilityError.isEmpty())
            action->setToolTip(info.applicabilityError);
        connect(action, &QAction::triggered, this, [this, key = info.key]() {
            emit filterActionRequested(key);
        });
        anyAdded = true;
    }

    if (anyAdded)
        menu.exec(event->globalPos());
}
