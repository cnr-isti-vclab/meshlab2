#include "threemfplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"

#include <lib3mf_implicit.hpp>

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QFileInfo>
#include <QImage>
#include <QMatrix4x4>
#include <QObject>
#include <QVector4D>

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <stdexcept>
#include <vector>

namespace {

enum ErrorCode {
    Ok = 0,
    OpenError = -1,
    ParseError = -2,
    EmptyError = -3,
    InvalidGeometryError = -4,
    WriteError = -5
};

using Mask = vcg::tri::io::Mask;

void progress(vcg::CallBackPos *cb, int value, const QString &message)
{
    if (!cb)
        return;
    const QByteArray bytes = message.toLocal8Bit();
    (*cb)(value, bytes.constData());
}

QMatrix4x4 matrix(const Lib3MF::sTransform &t)
{
    return QMatrix4x4(
        t.m_Fields[0][0], t.m_Fields[1][0], t.m_Fields[2][0], t.m_Fields[3][0],
        t.m_Fields[0][1], t.m_Fields[1][1], t.m_Fields[2][1], t.m_Fields[3][1],
        t.m_Fields[0][2], t.m_Fields[1][2], t.m_Fields[2][2], t.m_Fields[3][2],
        0.0f, 0.0f, 0.0f, 1.0f);
}

vcg::Color4b color(const Lib3MF::sColor &c)
{
    return vcg::Color4b(c.m_Red, c.m_Green, c.m_Blue, c.m_Alpha);
}

struct ImportContext {
    Lib3MF::PModel model;
    VCGMesh &mesh;
    std::map<Lib3MF_uint64, int> textureSlots;
    bool hasFaceColors = false;
    bool hasTexcoords = false;
    int meshObjects = 0;
};

void appendProperties(ImportContext &ctx, const Lib3MF::PMeshObject &source, int firstFace)
{
    for (Lib3MF_uint32 i = 0; i < source->GetTriangleCount(); ++i) {
        Lib3MF::sTriangleProperties properties;
        source->GetTriangleProperties(i, properties);
        if (properties.m_ResourceID == 0)
            continue;

        VCGFace &face = ctx.mesh.face[std::size_t(firstFace + int(i))];
        switch (ctx.model->GetPropertyTypeByID(properties.m_ResourceID)) {
        case Lib3MF::ePropertyType::BaseMaterial: {
            const auto group = ctx.model->GetBaseMaterialGroupByID(properties.m_ResourceID);
            face.C() = color(group->GetDisplayColor(properties.m_PropertyIDs[0]));
            ctx.hasFaceColors = true;
            break;
        }
        case Lib3MF::ePropertyType::Colors: {
            const auto group = ctx.model->GetColorGroupByID(properties.m_ResourceID);
            face.C() = color(group->GetColor(properties.m_PropertyIDs[0]));
            ctx.hasFaceColors = true;
            break;
        }
        case Lib3MF::ePropertyType::TexCoord: {
            const auto group = ctx.model->GetTexture2DGroupByID(properties.m_ResourceID);
            const auto slot = ctx.textureSlots.find(group->GetTexture2D()->GetUniqueResourceID());
            if (slot == ctx.textureSlots.end())
                break;
            for (int corner = 0; corner < 3; ++corner) {
                const auto uv = group->GetTex2Coord(properties.m_PropertyIDs[corner]);
                face.WT(corner).U() = uv.m_U;
                face.WT(corner).V() = uv.m_V;
                face.WT(corner).N() = slot->second;
            }
            ctx.hasTexcoords = true;
            break;
        }
        default:
            break;
        }
    }
}

void appendMesh(ImportContext &ctx, const Lib3MF::PMeshObject &source, const QMatrix4x4 &world)
{
    const int vertexCount = int(source->GetVertexCount());
    const int triangleCount = int(source->GetTriangleCount());
    if (vertexCount == 0)
        return;

    const int firstVertex = ctx.mesh.VN();
    const int firstFace = ctx.mesh.FN();
    vcg::tri::Allocator<VCGMesh>::AddVertices(ctx.mesh, vertexCount);
    vcg::tri::Allocator<VCGMesh>::AddFaces(ctx.mesh, triangleCount);

    for (int i = 0; i < vertexCount; ++i) {
        const Lib3MF::sPosition p = source->GetVertex(Lib3MF_uint32(i));
        const QVector4D transformed = world * QVector4D(
            p.m_Coordinates[0], p.m_Coordinates[1], p.m_Coordinates[2], 1.0f);
        ctx.mesh.vert[std::size_t(firstVertex + i)].P() =
            vcg::Point3f(transformed.x(), transformed.y(), transformed.z());
    }
    for (int i = 0; i < triangleCount; ++i) {
        const Lib3MF::sTriangle triangle = source->GetTriangle(Lib3MF_uint32(i));
        if (triangle.m_Indices[0] >= Lib3MF_uint32(vertexCount)
            || triangle.m_Indices[1] >= Lib3MF_uint32(vertexCount)
            || triangle.m_Indices[2] >= Lib3MF_uint32(vertexCount))
            throw std::runtime_error("3MF triangle references an invalid vertex");
        VCGFace &face = ctx.mesh.face[std::size_t(firstFace + i)];
        for (int corner = 0; corner < 3; ++corner)
            face.V(corner) = &ctx.mesh.vert[
                std::size_t(firstVertex + int(triangle.m_Indices[corner]))];
    }
    appendProperties(ctx, source, firstFace);
    ++ctx.meshObjects;
}

void appendObject(
    ImportContext &ctx,
    const Lib3MF::PObject &object,
    const QMatrix4x4 &world,
    std::set<Lib3MF_uint64> &path)
{
    const Lib3MF_uint64 id = object->GetUniqueResourceID();
    if (!path.insert(id).second)
        throw std::runtime_error("Cyclic 3MF component hierarchy");

    if (object->IsMeshObject()) {
        appendMesh(ctx, ctx.model->GetMeshObjectByID(id), world);
    } else if (object->IsComponentsObject()) {
        const auto components = ctx.model->GetComponentsObjectByID(id);
        for (Lib3MF_uint32 i = 0; i < components->GetComponentCount(); ++i) {
            const auto component = components->GetComponent(i);
            QMatrix4x4 child = world;
            if (component->HasTransform())
                child *= matrix(component->GetTransform());
            appendObject(ctx, component->GetObjectResource(), child, path);
        }
    }
    path.erase(id);
}

class ThreeMFPlugin final : public MeshIOPlugin
{
public:
    QString pluginId() const override { return QStringLiteral("io_3mf"); }
    QString name() const override { return QObject::tr("3MF Import/Export (lib3mf)"); }
    QStringList supportedExtensions() const override { return { QStringLiteral("3mf") }; }
    bool canLoad(const QString &filename) const override
    {
        return QFileInfo(filename).suffix().compare(QStringLiteral("3mf"), Qt::CaseInsensitive) == 0;
    }
    bool canSave(const QString &filename) const override { return canLoad(filename); }
    MeshIOCapabilities loadCapabilities(const QString &) const override
    {
        return { Mask::IOM_VERTCOORD | Mask::IOM_FACEINDEX | Mask::IOM_FACECOLOR
                | Mask::IOM_WEDGTEXCOORD | Mask::IOM_WEDGTEXMULTI,
            true, true };
    }
    QString filterString() const override { return QObject::tr("3MF Files (*.3mf)"); }
    QString saveFilterString() const override { return filterString(); }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *mask) const override
    {
        return load(filename, mesh, cb, mask, nullptr);
    }

    int load(
        const QString &filename,
        VCGMesh &mesh,
        vcg::CallBackPos *cb,
        int *mask,
        MeshIOMaterialSet *materials) const override
    {
        if (mask)
            *mask = 0;
        if (materials)
            materials->clear();
        if (!QFileInfo::exists(filename))
            return OpenError;

        try {
            progress(cb, 0, QObject::tr("Reading 3MF file..."));
            const auto wrapper = Lib3MF::CWrapper::loadLibrary();
            const auto model = wrapper->CreateModel();
            model->QueryReader("3mf")->ReadFromFile(filename.toStdString());

            mesh.Clear();
            mesh.face.EnableWedgeTexCoord();
            ImportContext ctx { model, mesh };

            auto textures = model->GetTexture2Ds();
            while (textures->MoveNext()) {
                const auto texture = textures->GetCurrentTexture2D();
                std::vector<Lib3MF_uint8> bytes;
                const auto attachment = texture->GetAttachment();
                if (!attachment)
                    continue;
                attachment->WriteToBuffer(bytes);
                QImage image;
                image.loadFromData(bytes.data(), int(bytes.size()));
                const int slot = int(ctx.textureSlots.size());
                const QString name = QStringLiteral("3mf_texture_%1.png")
                                         .arg(texture->GetUniqueResourceID());
                ctx.textureSlots.emplace(texture->GetUniqueResourceID(), slot);
                mesh.textures.push_back(name.toStdString());
                if (materials) {
                    MeshIOTextureAsset asset;
                    asset.name = name;
                    asset.image = image;
                    materials->textureAssets.push_back(std::move(asset));
                    MeshIOMaterialSlot material;
                    material.name = QObject::tr("3MF texture %1").arg(slot + 1);
                    material.baseColorTexture.fileName = name;
                    material.baseColorTexture.assetIndex = slot;
                    materials->entries.push_back(std::move(material));
                }
            }

            auto buildItems = model->GetBuildItems();
            const Lib3MF_uint64 count = buildItems->Count();
            Lib3MF_uint64 index = 0;
            while (buildItems->MoveNext()) {
                const auto item = buildItems->GetCurrent();
                QMatrix4x4 world;
                world.setToIdentity();
                if (item->HasObjectTransform())
                    world = matrix(item->GetObjectTransform());
                std::set<Lib3MF_uint64> path;
                appendObject(ctx, item->GetObjectResource(), world, path);
                progress(cb, count ? int((++index * 90) / count) : 90,
                    QObject::tr("Reading 3MF build items..."));
            }

            if (mesh.VN() == 0)
                return EmptyError;
            if (!ctx.hasTexcoords)
                mesh.face.DisableWedgeTexCoord();
            vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
            if (mesh.FN() > 0)
                vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);

            int loadMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL;
            if (mesh.FN() > 0)
                loadMask |= Mask::IOM_FACEINDEX | Mask::IOM_FACENORMAL;
            if (ctx.hasFaceColors)
                loadMask |= Mask::IOM_FACECOLOR;
            if (ctx.hasTexcoords)
                loadMask |= Mask::IOM_WEDGTEXCOORD;
            if (ctx.hasTexcoords && ctx.textureSlots.size() > 1)
                loadMask |= Mask::IOM_WEDGTEXMULTI;
            if (mask)
                *mask = loadMask;
            progress(cb, 100,
                QObject::tr("Loaded %1 3MF mesh object(s).").arg(ctx.meshObjects));
            return Ok;
        } catch (const Lib3MF::ELib3MFException &e) {
            progress(cb, 0, QObject::tr("3MF error: %1").arg(QString::fromUtf8(e.what())));
            return ParseError;
        } catch (const std::exception &e) {
            progress(cb, 0, QObject::tr("3MF error: %1").arg(QString::fromUtf8(e.what())));
            return InvalidGeometryError;
        }
    }

    int save(
        const QString &filename,
        VCGMesh &mesh,
        const MeshIOSaveOptions &options,
        vcg::CallBackPos *cb) const override
    {
        if (mesh.VN() == 0)
            return EmptyError;
        try {
            progress(cb, 0, QObject::tr("Writing 3MF file..."));
            const auto wrapper = Lib3MF::CWrapper::loadLibrary();
            const auto model = wrapper->CreateModel();
            model->GetMetaDataGroup()->AddMetaData(
                "", "Application", "MeshLab", "string", false);
            const auto object = model->AddMeshObject();
            object->SetName(QFileInfo(filename).completeBaseName().toStdString());

            std::vector<Lib3MF::sPosition> vertices;
            std::vector<Lib3MF_uint32> remap(mesh.vert.size(), Lib3MF_uint32(-1));
            vertices.reserve(std::size_t(mesh.VN()));
            for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
                const VCGVertex &vertex = mesh.vert[i];
                if (vertex.IsD())
                    continue;
                remap[i] = Lib3MF_uint32(vertices.size());
                Lib3MF::sPosition p {};
                p.m_Coordinates[0] = vertex.cP().X();
                p.m_Coordinates[1] = vertex.cP().Y();
                p.m_Coordinates[2] = vertex.cP().Z();
                vertices.push_back(p);
            }

            std::vector<Lib3MF::sTriangle> triangles;
            std::vector<std::uint32_t> triangleColors;
            triangles.reserve(std::size_t(mesh.FN()));
            const bool writeFaceColors = (options.mask & Mask::IOM_FACECOLOR) != 0;
            if (writeFaceColors)
                triangleColors.reserve(std::size_t(mesh.FN()));
            const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
            for (const VCGFace &face : mesh.face) {
                if (face.IsD())
                    continue;
                Lib3MF::sTriangle triangle {};
                for (int corner = 0; corner < 3; ++corner) {
                    const ptrdiff_t source = base ? face.cV(corner) - base : -1;
                    if (source < 0 || std::size_t(source) >= remap.size()
                        || remap[std::size_t(source)] == Lib3MF_uint32(-1))
                        return InvalidGeometryError;
                    triangle.m_Indices[corner] = remap[std::size_t(source)];
                }
                triangles.push_back(triangle);
                if (writeFaceColors) {
                    const vcg::Color4b &c = face.cC();
                    triangleColors.push_back(
                        (std::uint32_t(c[0]) << 24) | (std::uint32_t(c[1]) << 16)
                        | (std::uint32_t(c[2]) << 8) | std::uint32_t(c[3]));
                }
            }

            object->SetGeometry(vertices, triangles);
            if (!triangleColors.empty()) {
                const auto colors = model->AddColorGroup();
                std::map<std::uint32_t, Lib3MF_uint32> properties;
                for (const std::uint32_t rgba : triangleColors) {
                    if (properties.find(rgba) != properties.end())
                        continue;
                    Lib3MF::sColor c {};
                    c.m_Red = Lib3MF_uint8(rgba >> 24);
                    c.m_Green = Lib3MF_uint8(rgba >> 16);
                    c.m_Blue = Lib3MF_uint8(rgba >> 8);
                    c.m_Alpha = Lib3MF_uint8(rgba);
                    properties.emplace(rgba, colors->AddColor(c));
                }

                const Lib3MF_uint32 resourceId = colors->GetUniqueResourceID();
                if (properties.size() == 1) {
                    object->SetObjectLevelProperty(resourceId, properties.begin()->second);
                } else {
                    for (std::size_t i = 0; i < triangleColors.size(); ++i) {
                        const Lib3MF_uint32 property = properties.at(triangleColors[i]);
                        Lib3MF::sTriangleProperties p {};
                        p.m_ResourceID = resourceId;
                        p.m_PropertyIDs[0] = property;
                        p.m_PropertyIDs[1] = property;
                        p.m_PropertyIDs[2] = property;
                        object->SetTriangleProperties(Lib3MF_uint32(i), p);
                    }
                }
            }
            model->AddBuildItem(object.get(), wrapper->GetIdentityTransform());
            model->QueryWriter("3mf")->WriteToFile(filename.toStdString());
            progress(cb, 100, QObject::tr("Writing 3MF done."));
            return Ok;
        } catch (const std::exception &e) {
            progress(cb, 0, QObject::tr("3MF write error: %1").arg(QString::fromUtf8(e.what())));
            return WriteError;
        }
    }

    int saveMaskCapability(const QString &) const override
    {
        return Mask::IOM_VERTCOORD | Mask::IOM_FACEINDEX | Mask::IOM_FACECOLOR;
    }

    QString errorString(int code) const override
    {
        switch (code) {
        case OpenError: return QObject::tr("Cannot open the 3MF file.");
        case ParseError: return QObject::tr("The 3MF file could not be parsed.");
        case EmptyError: return QObject::tr("The 3MF file contains no mesh geometry.");
        case InvalidGeometryError: return QObject::tr("The 3MF geometry is invalid or unsupported.");
        case WriteError: return QObject::tr("The 3MF file could not be written.");
        default: return QObject::tr("Unknown 3MF import/export error.");
        }
    }
};

} // namespace

void register3MFPlugin(MeshIOPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<ThreeMFPlugin>());
}
