// USD import and export (.usda, .usdc, .usd, .usdz) through LightUSD's `next` core and its
// render-scene converter, Tydra Next. See docs/design/proposals/usd_support.md, stage 1:
// the composed stage is read flattened into one layer, and one layer is written as one mesh.
#include "plugins/io_usd/usdplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"

#include <vcg/complex/algorithms/polygon_support.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/io_mask.h>

#include "next/lightusd-next.hh"
#include "next/load-usd.hh"
#include "next/writer/usda-writer.hh"
#include "next/writer/usdc-writer.hh"
#include "next/writer/usdz-writer.hh"
#include "tydra/next/render-converter.hh"
#include "tydra/next/render-data.hh"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMatrix4x4>
#include <QObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <unordered_map>

namespace {
namespace ln = lightusd::next;
namespace tn = lightusd::tydra::next;
using M = vcg::tri::io::Mask;

constexpr int kErrOpen = -1;
constexpr int kErrParse = -2;
constexpr int kErrConvert = -3;
constexpr int kErrNoGeometry = -4;
constexpr int kErrSaveEmpty = -101;
constexpr int kErrSaveWrite = -102;

void report(vcg::CallBackPos *cb, int pos, const QString &message)
{
    if (cb) {
        const QByteArray bytes = message.toLocal8Bit();
        cb(pos, bytes.constData());
    }
}

QString ext(const QString &filename)
{
    return QFileInfo(filename).suffix().toLower();
}

// ---- import ----------------------------------------------------------------------------------

// The unit metersPerUnit names, when it names one of MeshLab's exactly (to a part in a million).
LengthUnit unitFromMetersPerUnit(double metersPerUnit)
{
    for (const LengthUnits::Info &info : LengthUnits::all()) {
        if (std::abs(metersPerUnit - info.meters) <= 1e-6 * info.meters)
            return info.unit;
    }
    return LengthUnit::Unknown;
}

// How many values an array of the given interpolation holds on a mesh.
size_t elementCount(tn::Interpolation interp, size_t points, size_t faces, size_t corners)
{
    switch (interp) {
    case tn::Interpolation::Constant: return 1;
    case tn::Interpolation::Uniform: return faces;
    case tn::Interpolation::FaceVarying: return corners;
    case tn::Interpolation::Vertex:
    case tn::Interpolation::Varying: break;
    }
    return points;
}

// Which element of such an array a corner reads.
size_t elementIndex(tn::Interpolation interp, size_t vertex, size_t face, size_t corner)
{
    switch (interp) {
    case tn::Interpolation::Constant: return 0;
    case tn::Interpolation::Uniform: return face;
    case tn::Interpolation::FaceVarying: return corner;
    case tn::Interpolation::Vertex:
    case tn::Interpolation::Varying: break;
    }
    return vertex;
}

vcg::Color4b toColor(const tn::FloatChunked &colors, size_t index, int components,
    const tn::FloatChunked &opacities, size_t opacityIndex)
{
    const auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    const size_t base = index * size_t(components);
    float alpha = components == 4 ? colors[base + 3] : 1.0f;
    if (opacityIndex < opacities.size())
        alpha = opacities[opacityIndex];
    return vcg::Color4b(byte(colors[base]), byte(colors[base + 1]), byte(colors[base + 2]), byte(alpha));
}

// Turns the converted scene into one VCGMesh, and its UsdPreviewSurface materials into
// MeshLab's material slots. One slot per material whose diffuse color is a texture; the other
// materials give their faces a face color instead.
class SceneImporter
{
public:
    SceneImporter(const QString &filename, const tn::RenderScene &scene, VCGMesh &mesh, vcg::CallBackPos *cb)
        : m_filename(filename), m_scene(scene), m_mesh(mesh), m_cb(cb)
    {
    }

    int run(int *outLoadMask, MeshIOMaterialSet *outMaterialSet)
    {
        m_yUp = m_scene.up_axis == tn::RenderScene::UpAxis::Y;
        if (m_yUp)
            report(m_cb, 0, QObject::tr("USD stage is Y-up: rotated by +90 degrees about X to MeshLab's Z-up."));

        int meshNodes = 0, pointNodes = 0, skippedInvisible = 0, skippedOther = 0;
        for (const tn::SceneNode &node : m_scene.nodes) {
            const bool isMesh = node.type == tn::NodeType::Mesh
                && node.data_id >= 0 && size_t(node.data_id) < m_scene.meshes.size();
            const bool isPoints = node.type == tn::NodeType::Points
                && node.data_id >= 0 && size_t(node.data_id) < m_scene.points.size();
            if (!isMesh && !isPoints) {
                if (node.type != tn::NodeType::Xform)
                    ++skippedOther;
                continue;
            }
            if (!node.visible) {
                ++skippedInvisible;
                continue;
            }
            if (isMesh) {
                appendMesh(m_scene.meshes[size_t(node.data_id)], node.world_transform);
                ++meshNodes;
            } else {
                appendPoints(m_scene.points[size_t(node.data_id)], node.world_transform);
                ++pointNodes;
            }
            report(m_cb, 10 + 80 * (meshNodes + pointNodes) / int(m_scene.nodes.size()),
                QObject::tr("Reading USD prims...") + QStringLiteral("\r"));
        }
        if (skippedInvisible > 0)
            report(m_cb, 90, QObject::tr("USD: skipped %1 invisible prim(s).").arg(skippedInvisible));
        if (skippedOther > 0)
            report(m_cb, 90, QObject::tr("USD: skipped %1 prim(s) that are not meshes or points "
                                         "(cameras, lights, curves, skeletons...).").arg(skippedOther));
        for (const QString &name : std::as_const(m_droppedInputs))
            report(m_cb, 90, QObject::tr("USD: material input %1 is not represented in MeshLab and was dropped.").arg(name));
        if (m_mesh.VN() == 0)
            return kErrNoGeometry;

        report(m_cb, 95, QObject::tr("USD: %1 mesh prim(s), %2 point prim(s), %3 material(s) with a texture.")
                              .arg(meshNodes).arg(pointNodes).arg(m_materials.entries.size()));

        int mask = M::IOM_VERTCOORD;
        if (m_mesh.FN() > 0)
            mask |= M::IOM_FACEINDEX;
        if (m_hasPolygons)
            mask |= M::IOM_BITPOLYGONAL;
        if (m_hasNormals)
            mask |= M::IOM_VERTNORMAL;
        if (m_hasVertexColors)
            mask |= M::IOM_VERTCOLOR;
        if (m_hasFaceColors)
            mask |= M::IOM_FACECOLOR;
        if (m_hasTexCoords) {
            mask |= M::IOM_WEDGTEXCOORD;
            if (m_materials.entries.size() > 1)
                mask |= M::IOM_WEDGTEXMULTI;
        }
        if (outLoadMask)
            *outLoadMask = mask;
        if (outMaterialSet)
            *outMaterialSet = std::move(m_materials);
        return 0;
    }

private:
    // The world transform as a column-vector matrix, with the Y-up rotation applied after it.
    // USD stores row-vector matrices, whose flat array is the transpose of Qt's row-major input.
    QMatrix4x4 worldMatrix(const tn::Matrix4 &m) const
    {
        QMatrix4x4 world = QMatrix4x4(m.m).transposed();
        if (m_yUp) {
            QMatrix4x4 up;
            up.rotate(90.0f, 1.0f, 0.0f, 0.0f);
            world = up * world;
        }
        return world;
    }

    void appendMesh(const tn::RenderMesh &src, const tn::Matrix4 &transform)
    {
        const QMatrix4x4 world = worldMatrix(transform);
        const QMatrix4x4 normalMatrix = world.inverted().transposed();
        // A mirroring transform reverses the winding, as leftHanded does (which the converter
        // has already undone when triangulating).
        const bool mirrored = world.determinant() < 0.0f;

        const size_t pointCount = src.points.size() / 3;
        const size_t faceCount = src.face_vertex_counts.size();
        const size_t cornerCount = src.face_vertex_indices.size();
        const size_t triangleCount = src.triangulated_indices.size() / 3;
        if (pointCount == 0 || src.triangulated_face_vertex_indices.size() != src.triangulated_indices.size()
            || src.face_triangle_offsets.size() != faceCount + 1)
            return;

        const int baseVertex = m_mesh.VN();
        auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(m_mesh, pointCount);
        for (size_t i = 0; i < pointCount; ++i, ++vi) {
            const QVector3D p = world.map(QVector3D(src.points[3 * i], src.points[3 * i + 1], src.points[3 * i + 2]));
            vi->P() = VCGMesh::CoordType(p.x(), p.y(), p.z());
            vi->C() = vcg::Color4b::White;
        }
        auto vertex = [&](size_t i) -> VCGVertex & { return m_mesh.vert[size_t(baseVertex) + i]; };

        // Normals. Vertex ones are read as they are; faceVarying ones are averaged per vertex,
        // because MeshLab's faces carry no per-corner normal. Per-face and constant ones are
        // left to MeshLab to recompute.
        const tn::Interpolation ni = src.normals_interp;
        const bool perVertexNormals = ni == tn::Interpolation::Vertex || ni == tn::Interpolation::Varying;
        const bool cornerNormals = ni == tn::Interpolation::FaceVarying;
        if (!src.normals.empty() && src.normals.size() / 3 == elementCount(ni, pointCount, faceCount, cornerCount)
            && (perVertexNormals || cornerNormals)) {
            std::vector<QVector3D> sum(pointCount);
            if (perVertexNormals) {
                for (size_t i = 0; i < pointCount; ++i)
                    sum[i] = QVector3D(src.normals[3 * i], src.normals[3 * i + 1], src.normals[3 * i + 2]);
            } else {
                for (size_t c = 0; c < cornerCount; ++c) {
                    const uint32_t v = src.face_vertex_indices[c];
                    if (v < pointCount)
                        sum[v] += QVector3D(src.normals[3 * c], src.normals[3 * c + 1], src.normals[3 * c + 2]);
                }
                m_averagedNormals = true;
            }
            for (size_t i = 0; i < pointCount; ++i) {
                const QVector3D n = normalMatrix.mapVector(sum[i]).normalized();
                vertex(i).N() = VCGMesh::CoordType(n.x(), n.y(), n.z());
            }
            m_hasNormals = true;
        }

        // Colors: displayColor and displayOpacity. Vertex ones become vertex colors, faceVarying
        // ones too (a vertex takes the value of its last corner), uniform ones face colors, and a
        // constant one the color of every vertex.
        const tn::Interpolation ci = src.colors_interp;
        const size_t colorElements = elementCount(ci, pointCount, faceCount, cornerCount);
        const int colorComponents = colorElements ? int(src.colors.size() / colorElements) : 0;
        const bool hasColors = !src.colors.empty() && (colorComponents == 3 || colorComponents == 4)
            && src.colors.size() == colorElements * size_t(colorComponents);
        const tn::FloatChunked noOpacity;
        const tn::FloatChunked &opacities =
            src.opacities_interp == ci && src.opacities.size() == colorElements ? src.opacities : noOpacity;
        if (hasColors && ci != tn::Interpolation::Uniform) {
            for (size_t c = 0; c < (ci == tn::Interpolation::FaceVarying ? cornerCount : pointCount); ++c) {
                const size_t v = ci == tn::Interpolation::FaceVarying ? src.face_vertex_indices[c] : c;
                const size_t e = elementIndex(ci, v, 0, c);
                if (v < pointCount)
                    vertex(v).C() = toColor(src.colors, e, colorComponents, opacities, e);
            }
            m_hasVertexColors = true;
        }

        // Texture coordinates, by the primary UV set's interpolation; v is up in USD as in MeshLab.
        const tn::Interpolation ti = src.texcoords_0_interp;
        const bool hasTexCoords = m_mesh.face.IsWedgeTexCoordEnabled() && !src.texcoords_0.empty()
            && src.texcoords_0.size() == 2 * elementCount(ti, pointCount, faceCount, cornerCount);
        m_hasTexCoords |= hasTexCoords;

        // Material of each authored face: the mesh's own, then its GeomSubsets. Subset ranges
        // are in triangle space once the mesh is triangulated, which it always is here.
        std::vector<int32_t> triangleMaterial(triangleCount, src.material_id);
        for (const tn::RenderMesh::MaterialSubset &subset : src.material_subsets) {
            const size_t end = std::min<size_t>(size_t(subset.face_start) + subset.face_count, triangleCount);
            for (size_t t = subset.face_start; t < end; ++t)
                triangleMaterial[t] = subset.material_id;
        }

        // Corner offset of each authored face, to tell a polygon's sides from its diagonals.
        std::vector<uint32_t> faceCorner(faceCount + 1, 0);
        for (size_t f = 0; f < faceCount; ++f)
            faceCorner[f + 1] = faceCorner[f] + src.face_vertex_counts[f];

        auto fi = vcg::tri::Allocator<VCGMesh>::AddFaces(m_mesh, triangleCount);
        size_t face = 0;
        for (size_t t = 0; t < triangleCount; ++t, ++fi) {
            while (face + 1 < faceCount && src.face_triangle_offsets[face + 1] <= t)
                ++face;
            uint32_t corner[3] = { src.triangulated_face_vertex_indices[3 * t],
                src.triangulated_face_vertex_indices[3 * t + 1], src.triangulated_face_vertex_indices[3 * t + 2] };
            uint32_t vert[3] = { src.triangulated_indices[3 * t], src.triangulated_indices[3 * t + 1],
                src.triangulated_indices[3 * t + 2] };
            if (mirrored) {
                std::swap(corner[1], corner[2]);
                std::swap(vert[1], vert[2]);
            }
            for (int k = 0; k < 3; ++k)
                fi->V(k) = &vertex(std::min<size_t>(vert[k], pointCount - 1));

            // A polygon split into triangles keeps its sides; its diagonals become faux edges.
            const uint32_t sides = src.face_vertex_counts[face];
            if (sides > 3) {
                m_hasPolygons = true;
                for (int k = 0; k < 3; ++k) {
                    const uint32_t a = corner[k] - faceCorner[face], b = corner[(k + 1) % 3] - faceCorner[face];
                    const uint32_t step = (b + sides - a) % sides;
                    if (step != 1 && step != sides - 1)
                        fi->SetF(k);
                }
            }

            const MaterialInfo material = materialInfo(triangleMaterial[t]);
            if (hasColors && ci == tn::Interpolation::Uniform) {
                fi->C() = toColor(src.colors, face, colorComponents, opacities, face);
                m_hasFaceColors = true;
            } else {
                fi->C() = vcg::Color4b::White;
            }
            if (material.hasColor) {
                fi->C() = material.color;
                m_hasFaceColors = true;
            }
            // Wedge coordinates are an optional component: Document::loadMesh enables them, a
            // direct caller may not have.
            for (int k = 0; k < 3 && m_mesh.face.IsWedgeTexCoordEnabled(); ++k) {
                if (hasTexCoords) {
                    const size_t e = elementIndex(ti, vert[k], face, corner[k]);
                    fi->WT(k).U() = src.texcoords_0[2 * e];
                    fi->WT(k).V() = src.texcoords_0[2 * e + 1];
                }
                fi->WT(k).N() = material.slot;
            }
        }
    }

    void appendPoints(const tn::RenderPoints &src, const tn::Matrix4 &transform)
    {
        const QMatrix4x4 world = worldMatrix(transform);
        const QMatrix4x4 normalMatrix = world.inverted().transposed();
        const size_t count = src.point_count();
        if (count == 0)
            return;
        const bool normals = src.normals.size() == 3 * count;
        const int components = src.colors.size() == 4 * count ? 4 : (src.colors.size() == 3 * count ? 3 : 0);
        const bool constantColor = src.colors.size() == 3 || src.colors.size() == 4;
        const tn::FloatChunked noOpacity;
        const tn::FloatChunked &opacities = src.opacities.size() == count ? src.opacities : noOpacity;
        auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(m_mesh, count);
        for (size_t i = 0; i < count; ++i, ++vi) {
            const QVector3D p = world.map(QVector3D(src.points[3 * i], src.points[3 * i + 1], src.points[3 * i + 2]));
            vi->P() = VCGMesh::CoordType(p.x(), p.y(), p.z());
            if (normals) {
                const QVector3D n = normalMatrix.mapVector(QVector3D(src.normals[3 * i], src.normals[3 * i + 1],
                                                       src.normals[3 * i + 2])).normalized();
                vi->N() = VCGMesh::CoordType(n.x(), n.y(), n.z());
            }
            if (components)
                vi->C() = toColor(src.colors, i, components, opacities, i);
            else if (constantColor)
                vi->C() = toColor(src.colors, 0, int(src.colors.size()), noOpacity, 0);
            else
                vi->C() = vcg::Color4b::White;
        }
        m_hasNormals |= normals;
        m_hasVertexColors |= components || constantColor;
    }

    struct MaterialInfo {
        int slot = -1;
        bool hasColor = false;
        vcg::Color4b color = vcg::Color4b::White;
    };

    // What a scene material means for a face: its slot when its diffuse color is a texture,
    // its color when that is a constant. Computed once per material.
    MaterialInfo materialInfo(int32_t materialId)
    {
        if (materialId < 0 || size_t(materialId) >= m_scene.materials.size())
            return {};
        const auto cached = m_materialInfo.find(materialId);
        if (cached != m_materialInfo.end())
            return cached->second;

        MaterialInfo info;
        const tn::RenderMaterial &material = m_scene.materials[size_t(materialId)];
        const tn::PreviewSurfaceShader *surface = material.preview_surface.get();
        if (!surface) {
            m_droppedInputs.insert(QObject::tr("of %1 (not a UsdPreviewSurface)").arg(QString::fromStdString(material.prim_path)));
        } else if (MeshIOMaterialTextureRef base = textureRef(surface->diffuse_color.texture_id, true);
                   base.isValid()) {
            MeshIOMaterialSlot slot;
            slot.name = QString::fromStdString(material.name);
            slot.baseColorTexture = base;
            slot.normalTexture = textureRef(surface->normal.texture_id, false);
            slot.occlusionTexture = textureRef(surface->occlusion.texture_id, false);
            slot.roughnessTexture = textureRef(surface->roughness.texture_id, false);
            if (surface->roughness.is_value())
                slot.roughnessFactor = std::clamp(surface->roughness.as_float(), 0.0f, 1.0f);
            info.slot = int(m_materials.entries.size());
            m_materials.entries.push_back(std::move(slot));
            noteDropped(*surface);
        } else {
            // A constant diffuse color, or white for a texture that could not be read.
            const tn::Float4 c = surface->diffuse_color.is_texture() ? tn::Float4{ 1, 1, 1, 1 }
                                                                     : surface->diffuse_color.value;
            info.hasColor = true;
            info.color = toColorValue(c.x, c.y, c.z, surface->opacity.is_value() ? surface->opacity.as_float() : 1.0f);
            noteDropped(*surface);
        }
        m_materialInfo.emplace(materialId, info);
        return info;
    }

    static vcg::Color4b toColorValue(float r, float g, float b, float a)
    {
        const auto byte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        return vcg::Color4b(byte(r), byte(g), byte(b), byte(a));
    }

    // The inputs stage 1 cannot carry, logged once each when they differ from the fallback.
    void noteDropped(const tn::PreviewSurfaceShader &s)
    {
        const auto differs = [](const tn::ShaderParam &p, float x, float y = 0, float z = 0) {
            return p.is_texture() || p.value.x != x || p.value.y != y || p.value.z != z;
        };
        if (differs(s.metallic, 0))
            m_droppedInputs.insert(QStringLiteral("metallic"));
        if (differs(s.emissive_color, 0))
            m_droppedInputs.insert(QStringLiteral("emissiveColor"));
        if (s.opacity.is_texture())
            m_droppedInputs.insert(QStringLiteral("opacity (texture)"));
        if (differs(s.clearcoat, 0))
            m_droppedInputs.insert(QStringLiteral("clearcoat"));
        if (s.use_specular_workflow)
            m_droppedInputs.insert(QStringLiteral("useSpecularWorkflow"));
    }

    // A reference to a texture's image. A base color image lands in the asset list at the
    // index of its slot, as Document::loadMesh pairs mesh.textures[i] with textureAssets[i];
    // the other channels are appended after. Images inside a .usdz are decoded into memory.
    MeshIOMaterialTextureRef textureRef(int32_t textureId, bool baseColor)
    {
        MeshIOMaterialTextureRef ref;
        if (textureId < 0 || size_t(textureId) >= m_scene.textures.size())
            return ref;
        const tn::RenderTexture &texture = m_scene.textures[size_t(textureId)];
        if (texture.image_id < 0 || size_t(texture.image_id) >= m_scene.images.size())
            return ref;
        if (texture.is_udim)
            m_droppedInputs.insert(QObject::tr("UDIM texture %1").arg(QString::fromStdString(texture.asset_path)));
        if (!baseColor && texture.output_channel != tn::RenderTexture::Channel::RGB
            && texture.output_channel != tn::RenderTexture::Channel::RGBA)
            m_droppedInputs.insert(QObject::tr("the channel selection of %1 (MeshLab reads all channels)")
                                       .arg(QString::fromStdString(texture.asset_path)));

        const QString resolved = QString::fromStdString(m_scene.images[size_t(texture.image_id)].resolved_path);
        MeshIOTextureAsset asset;
        const qsizetype bracket = resolved.indexOf(QLatin1Char('['));
        if (bracket > 0 && resolved.endsWith(QLatin1Char(']'))) {
            // "package.usdz[entry]": an image stored inside the package.
            const QString entry = resolved.mid(bracket + 1, resolved.size() - bracket - 2);
            asset.name = QFileInfo(entry).fileName();
            asset.image = packageImage(resolved.left(bracket), entry);
            if (asset.image.isNull()) {
                report(m_cb, 0, QObject::tr("USD warning: cannot decode texture %1 inside the package.").arg(entry));
                return ref;
            }
            ref.fileName = asset.name;
        } else {
            QString path = resolved;
            if (QFileInfo(path).isRelative())
                path = QFileInfo(m_filename).absoluteDir().filePath(path);
            if (!QFileInfo::exists(path)) {
                report(m_cb, 0, QObject::tr("USD warning: texture file not found: %1").arg(path));
                return ref;
            }
            path = QFileInfo(path).absoluteFilePath();
            asset.name = QFileInfo(path).fileName();
            asset.sourcePath = path;
            ref.fileName = asset.name;
            ref.filePath = path;
        }
        if (baseColor) {
            // One entry per slot, so that the base color list stays aligned with the slots.
            m_mesh.textures.push_back((asset.hasImage() ? asset.name : asset.sourcePath).toStdString());
            m_materials.textureAssets.insert(m_materials.textureAssets.begin() + qsizetype(m_baseAssets), asset);
            // Channel textures already appended move one place up.
            for (MeshIOMaterialSlot &slot : m_materials.entries)
                for (MeshIOMaterialTextureRef *r : { &slot.normalTexture, &slot.occlusionTexture, &slot.roughnessTexture })
                    if (r->assetIndex >= int(m_baseAssets))
                        ++r->assetIndex;
            ref.assetIndex = int(m_baseAssets++);
        } else if (asset.hasImage()) {
            ref.assetIndex = int(m_materials.textureAssets.size());
            m_materials.textureAssets.push_back(std::move(asset));
        }
        return ref;
    }

    QImage packageImage(const QString &package, const QString &entry)
    {
        if (!m_package || m_packagePath != package) {
            m_package = std::make_unique<ln::USDZReader>();
            m_packagePath = package;
            if (!m_package->OpenFile(package.toStdString()))
                return {};
        }
        const std::string wanted = entry.toStdString();
        for (size_t i = 0; i < m_package->NumEntries(); ++i) {
            if (m_package->EntryName(i) == wanted)
                return QImage::fromData(m_package->EntryData(i), int(m_package->EntrySize(i)));
        }
        return {};
    }

    const QString m_filename;
    const tn::RenderScene &m_scene;
    VCGMesh &m_mesh;
    vcg::CallBackPos *m_cb;
    bool m_yUp = false;
    bool m_hasNormals = false, m_averagedNormals = false, m_hasVertexColors = false, m_hasFaceColors = false;
    bool m_hasTexCoords = false, m_hasPolygons = false;
    MeshIOMaterialSet m_materials;
    size_t m_baseAssets = 0;
    std::unordered_map<int32_t, MaterialInfo> m_materialInfo;
    QSet<QString> m_droppedInputs;
    std::unique_ptr<ln::USDZReader> m_package;
    QString m_packagePath;

public:
    bool averagedNormals() const { return m_averagedNormals; }
};

// ---- export ----------------------------------------------------------------------------------

// Writing goes through LightUSD's LayerBuilder, which is low level: it records each property's
// USD type name itself, and two things must be declared that a higher-level API would add.
// A shader output must be created as a property slot (declareOutput), or connections point at
// nothing; and a materialBind GeomSubset family must state its type, or OpenUSD's validator
// rejects it. tests/test_document.cpp checks the written files with OpenUSD when it is present.
void addTyped(ln::LayerBuilder &b, const std::string &name, ln::Value value, const char *type, uint16_t flags = 0)
{
    b.add_property(name, std::move(value), flags);
    b.current()->set_property_type_name(name, type);
}

void setInterpolation(ln::LayerBuilder &b, const std::string &name, const char *interpolation)
{
    ln::PropMeta &meta = b.current()->ensure_property_meta(name);
    meta.interpolation = interpolation;
    meta.authored |= ln::PropMeta::kInterpolation;
}

void declareOutput(ln::LayerBuilder &b, const std::string &name, ln::TypeId type, const char *typeName)
{
    b.current()->add_property_slot(ln::GetPropNameTable().intern(name), type, 0);
    b.current()->set_property_type_name(name, typeName);
}

void connect(ln::LayerBuilder &b, const std::string &input, const std::string &source, const char *type)
{
    b.add_connection(input, ln::Path(source));
    b.current()->set_property_type_name(input, type);
}

void applyMaterialBindingApi(ln::LayerBuilder &b)
{
    auto &meta = b.current()->meta();
    meta.apiSchemas().push_back("MaterialBindingAPI");
    meta.apiSchemasQualifier() = "prepend";
    meta.setApiSchemasAuthored();
}

// A valid prim name: letters, digits and underscores, not starting with a digit, unique.
std::string primName(const QString &wanted, const QString &fallback, QSet<QString> &taken)
{
    QString name;
    for (const QChar c : wanted)
        name += (c.isLetterOrNumber() && c.unicode() < 128) ? c : QChar(u'_');
    if (name.isEmpty() || name.front().isDigit())
        name = fallback + name;
    QString unique = name;
    for (int i = 1; taken.contains(unique); ++i)
        unique = QStringLiteral("%1_%2").arg(name).arg(i);
    taken.insert(unique);
    return unique.toStdString();
}

// Where the textures of one save go, and the files that end up holding them.
class TextureWriter
{
public:
    TextureWriter(const QString &filename, const MeshIOSaveOptions &options, const MeshIOTextureContext *context,
        vcg::CallBackPos *cb)
        : m_filename(QFileInfo(filename).absoluteFilePath()), m_package(ext(filename) == QLatin1String("usdz")),
          m_copy(options.copyAssociatedTextures), m_context(context), m_cb(cb)
    {
        if (context && context->textureAssets)
            m_saved = QStringList(qsizetype(context->textureAssets->size()));
    }

    // The asset path to author for a reference, or empty when it has no image. A texture with a
    // file is referred to where it is, or copied byte for byte (into the package for .usdz, or
    // next to the file when asked); one with only an image is written as PNG.
    std::string assetPath(const MeshIOMaterialTextureRef &ref)
    {
        const MeshIOTextureAsset *asset = nullptr;
        if (m_context && m_context->textureAssets && ref.assetIndex >= 0
            && ref.assetIndex < int(m_context->textureAssets->size()))
            asset = &m_context->textureAssets->at(size_t(ref.assetIndex));
        const QString source = asset ? asset->sourcePath.trimmed() : ref.filePath.trimmed();
        const bool hasFile = !source.isEmpty() && QFileInfo(source).isFile();
        if (!hasFile && !(asset && asset->hasImage()))
            return {};
        const QString key = hasFile ? QFileInfo(source).absoluteFilePath() : QStringLiteral("asset:%1").arg(ref.assetIndex);
        const auto done = m_written.find(key);
        if (done != m_written.end())
            return done->second;

        QString name = QFileInfo(hasFile ? source : asset->name).fileName();
        if (!hasFile)
            name = QFileInfo(name).completeBaseName() + QStringLiteral(".png");
        if (QFileInfo(name).completeBaseName().isEmpty())
            name = QStringLiteral("texture_%1.png").arg(m_written.size());
        QString authored, holder;
        const QDir outDir = QFileInfo(m_filename).absoluteDir();
        if (m_package) {
            authored = uniqueName(name, [&](const QString &n) { return m_entries.count(n.toStdString()) > 0; });
            QByteArray bytes;
            if (hasFile) {
                QFile file(source);
                if (file.open(QIODevice::ReadOnly))
                    bytes = file.readAll();
            } else {
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                asset->image.save(&buffer, "PNG");
            }
            if (bytes.isEmpty())
                return warn(name);
            m_entries[authored.toStdString()] = std::vector<uint8_t>(bytes.begin(), bytes.end());
            holder = m_filename;
        } else if (hasFile && !m_copy) {
            authored = outDir.relativeFilePath(QFileInfo(source).absoluteFilePath());
            holder = QFileInfo(source).absoluteFilePath();
        } else if (hasFile && outDir.absoluteFilePath(name) == QFileInfo(source).absoluteFilePath()) {
            authored = name;
            holder = QFileInfo(source).absoluteFilePath();
        } else {
            authored = uniqueName(name, [&](const QString &n) { return QFileInfo::exists(outDir.filePath(n)); });
            const bool ok = hasFile ? QFile::copy(source, outDir.filePath(authored))
                                    : asset->image.save(outDir.filePath(authored), "PNG");
            if (!ok)
                return warn(name);
            holder = outDir.filePath(authored);
        }
        if (asset && ref.assetIndex < m_saved.size())
            m_saved[ref.assetIndex] = holder;
        m_written[key] = authored.toStdString();
        return m_written[key];
    }

    std::map<std::string, std::vector<uint8_t>> &packageEntries() { return m_entries; }

    void finish() const
    {
        if (m_context && m_context->savedTexturePaths)
            *m_context->savedTexturePaths = m_saved;
    }

private:
    template<class Taken>
    static QString uniqueName(const QString &name, Taken taken)
    {
        const QFileInfo info(name);
        QString candidate = name;
        for (int i = 1; taken(candidate); ++i)
            candidate = QStringLiteral("%1_%2.%3").arg(info.completeBaseName()).arg(i).arg(info.suffix());
        return candidate;
    }

    std::string warn(const QString &name)
    {
        report(m_cb, 0, QObject::tr("USD export warning: could not store texture %1.").arg(name));
        return {};
    }

    const QString m_filename;
    const bool m_package;
    const bool m_copy;
    const MeshIOTextureContext *m_context;
    vcg::CallBackPos *m_cb;
    QStringList m_saved;
    std::map<QString, std::string> m_written;
    std::map<std::string, std::vector<uint8_t>> m_entries;
};

// One UsdShadeMaterial with a UsdPreviewSurface, fed by a UsdUVTexture for each channel the
// slot has. Normal maps are read raw and remapped from [0, 1] to [-1, 1]; roughness reads the
// green channel and occlusion the red one, where glTF packs them.
void writeMaterial(ln::LayerBuilder &b, const std::string &path, const std::string &name,
    const MeshIOMaterialSlot &slot, TextureWriter &textures)
{
    b.begin_prim(name, "Material");
    connect(b, "outputs:surface", path + "/Surface.outputs:surface", "token");

    struct Channel {
        const char *input, *shader, *output, *inputType;
        ln::TypeId outputType;
        const char *outputTypeName;
        bool raw, remap;
        std::string asset;
    };
    std::vector<Channel> channels = {
        { "inputs:diffuseColor", "BaseColor", "outputs:rgb", "color3f", ln::TypeId::Float3, "float3", false, false,
            textures.assetPath(slot.baseColorTexture) },
        { "inputs:normal", "Normal", "outputs:rgb", "normal3f", ln::TypeId::Float3, "float3", true, true,
            textures.assetPath(slot.normalTexture) },
        { "inputs:occlusion", "Occlusion", "outputs:r", "float", ln::TypeId::Float, "float", true, false,
            textures.assetPath(slot.occlusionTexture) },
        { "inputs:roughness", "Roughness", "outputs:g", "float", ln::TypeId::Float, "float", true, false,
            textures.assetPath(slot.roughnessTexture) },
    };
    const uint16_t uniform = ln::PropSlot::kFlagUniform;
    b.begin_prim("Surface", "Shader");
    addTyped(b, "info:id", ln::Value::MakeToken("UsdPreviewSurface"), "token", uniform);
    bool anyTexture = false;
    for (const Channel &c : channels) {
        if (!c.asset.empty()) {
            connect(b, c.input, path + "/" + c.shader + "." + c.output, c.inputType);
            anyTexture = true;
        }
    }
    if (channels[3].asset.empty())
        addTyped(b, "inputs:roughness", ln::Value(slot.roughnessFactor), "float");
    declareOutput(b, "outputs:surface", ln::TypeId::Token, "token");
    b.end_prim();

    for (const Channel &c : channels) {
        if (c.asset.empty())
            continue;
        b.begin_prim(c.shader, "Shader");
        addTyped(b, "info:id", ln::Value::MakeToken("UsdUVTexture"), "token", uniform);
        addTyped(b, "inputs:file", ln::Value::MakeAssetPath(c.asset), "asset");
        addTyped(b, "inputs:sourceColorSpace", ln::Value::MakeToken(c.raw ? "raw" : "sRGB"), "token");
        addTyped(b, "inputs:wrapS", ln::Value::MakeToken("repeat"), "token");
        addTyped(b, "inputs:wrapT", ln::Value::MakeToken("repeat"), "token");
        if (c.remap) {
            addTyped(b, "inputs:scale", ln::Value::MakeFloat4(2, 2, 2, 1), "float4");
            addTyped(b, "inputs:bias", ln::Value::MakeFloat4(-1, -1, -1, 0), "float4");
        }
        connect(b, "inputs:st", path + "/TexCoords.outputs:result", "float2");
        declareOutput(b, c.output, c.outputType, c.outputTypeName);
        b.end_prim();
    }
    if (anyTexture) {
        b.begin_prim("TexCoords", "Shader");
        addTyped(b, "info:id", ln::Value::MakeToken("UsdPrimvarReader_float2"), "token", uniform);
        addTyped(b, "inputs:varname", ln::Value::MakeStringLike("st", ln::TypeId::String), "string");
        declareOutput(b, "outputs:result", ln::TypeId::Float2, "float2");
        b.end_prim();
    }
    b.end_prim();
}

class USDPlugin final : public MeshIOPlugin
{
public:
    QString pluginId() const override { return QStringLiteral("io_usd"); }
    QString name() const override { return QObject::tr("USD Import/Export (LightUSD)"); }

    QStringList supportedExtensions() const override
    {
        return { QStringLiteral("usd"), QStringLiteral("usda"), QStringLiteral("usdc"), QStringLiteral("usdz") };
    }

    bool canLoad(const QString &filename) const override { return supportedExtensions().contains(ext(filename)); }
    bool canSave(const QString &filename) const override { return canLoad(filename); }

    MeshIOCapabilities loadCapabilities(const QString &) const override
    {
        return { M::IOM_VERTCOORD | M::IOM_VERTNORMAL | M::IOM_VERTCOLOR | M::IOM_FACEINDEX | M::IOM_FACECOLOR
                | M::IOM_WEDGTEXCOORD | M::IOM_WEDGTEXMULTI | M::IOM_BITPOLYGONAL,
            true, true };
    }

    int saveMaskCapability(const QString &) const override
    {
        return M::IOM_VERTCOORD | M::IOM_VERTNORMAL | M::IOM_VERTCOLOR | M::IOM_VERTQUALITY | M::IOM_VERTTEXCOORD
            | M::IOM_FACEINDEX | M::IOM_FACECOLOR | M::IOM_WEDGTEXCOORD | M::IOM_WEDGTEXMULTI | M::IOM_BITPOLYGONAL;
    }

    MeshIOCapabilities saveCapabilities(const QString &filename) const override
    {
        return { saveMaskCapability(filename), true, true };
    }

    QString filterString() const override { return QObject::tr("USD (*.usd *.usda *.usdc *.usdz)"); }

    QString saveFilterString() const override
    {
        return QObject::tr("USD Binary (*.usdc);;USD Text (*.usda);;USD Package (*.usdz)");
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask) const override
    {
        return load(filename, mesh, cb, outLoadMask, nullptr);
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet) const override
    {
        MeshIOLoadContext context;
        return load(filename, mesh, cb, outLoadMask, outMaterialSet, context);
    }

    int load(const QString &filename, VCGMesh &mesh, vcg::CallBackPos *cb, int *outLoadMask,
        MeshIOMaterialSet *outMaterialSet, MeshIOLoadContext &context) const override
    {
        if (outLoadMask)
            *outLoadMask = 0;
        if (outMaterialSet)
            outMaterialSet->clear();
        if (!QFileInfo::exists(filename))
            return kErrOpen;

        report(cb, 0, QObject::tr("Reading USD stage...") + QStringLiteral("\r"));
        ln::Stage stage;
        std::string warn, err;
        if (!ln::LoadUSDComposed(QFile::encodeName(filename).toStdString(), &stage, &warn, &err)) {
            report(cb, 0, QObject::tr("USD load error: %1").arg(QString::fromStdString(err)));
            return kErrParse;
        }
        if (!warn.empty())
            report(cb, 0, QObject::tr("USD warning: %1").arg(QString::fromStdString(warn).trimmed()));

        // Polygons are triangulated by the converter, which also reports which authored face
        // and corner each triangle comes from: MeshLab keeps the polygons as faux edges, and
        // faceVarying data and GeomSubsets are indexed through it. Normals are not computed
        // there: MeshLab computes what the file lacks. Images are decoded here, with QImage.
        tn::ConverterConfig config;
        config.mesh.triangulate = true;
        config.mesh.compute_normals = false;
        config.material.load_textures = false;
        config.asset_base_dir = QFile::encodeName(QFileInfo(filename).absolutePath()).toStdString();
        tn::RenderSceneConverter converter(config);
        report(cb, 5, QObject::tr("Converting USD stage...") + QStringLiteral("\r"));
        const tn::ConvertResult result = converter.Convert(stage);
        for (const std::string &w : result.warnings)
            report(cb, 5, QObject::tr("USD warning: %1").arg(QString::fromStdString(w)));
        if (!result.success) {
            report(cb, 0, QObject::tr("USD conversion error: %1").arg(QString::fromStdString(result.error)));
            return kErrConvert;
        }

        mesh.Clear();
        mesh.textures.clear();
        SceneImporter importer(filename, result.scene, mesh, cb);
        const int status = importer.run(outLoadMask, outMaterialSet);
        if (status != 0)
            return status;
        if (importer.averagedNormals())
            report(cb, 100, QObject::tr("USD: per-corner (faceVarying) normals were averaged per vertex."));

        // metersPerUnit falls back to centimeters by the USD specification; the converter's own
        // fallback (meters) is not used, as it does not distinguish an authored value.
        const auto &meta = stage.GetMeta();
        const double metersPerUnit = meta.metersPerUnit_set ? result.scene.meters_per_unit : 0.01;
        context.unit = unitFromMetersPerUnit(metersPerUnit);
        if (!meta.metersPerUnit_set)
            report(cb, 100, QObject::tr("USD stage authors no metersPerUnit: centimeters, by the USD specification."));
        else if (context.unit == LengthUnit::Unknown)
            report(cb, 100, QObject::tr("USD metersPerUnit %1 is not a unit MeshLab names: unit left unset.")
                                .arg(metersPerUnit));
        return 0;
    }

    int save(const QString &filename, VCGMesh &mesh, const MeshIOSaveOptions &options, vcg::CallBackPos *cb) const override
    {
        return save(filename, mesh, options, cb, nullptr);
    }

    int save(const QString &filename, VCGMesh &mesh, const MeshIOSaveOptions &options, vcg::CallBackPos *cb,
        const MeshIOTextureContext *textureContext) const override
    {
        if (mesh.VN() == 0)
            return kErrSaveEmpty;
        // As the other savers: no mask asks for all the format holds, and faces are always written.
        const int mask = (options.mask != 0 ? options.mask : saveMaskCapability(filename))
            & saveMaskCapability(filename);
        const bool faces = mesh.FN() > 0;
        const bool polygons = faces && (mask & M::IOM_BITPOLYGONAL);
        const bool vertexColors = (mask & M::IOM_VERTCOLOR) != 0;
        const bool faceColors = faces && !vertexColors && (mask & M::IOM_FACECOLOR);
        const bool wedgeTexCoords = faces && (mask & M::IOM_WEDGTEXCOORD) && mesh.face.IsWedgeTexCoordEnabled();
        const bool vertexTexCoords = !wedgeTexCoords && (mask & M::IOM_VERTTEXCOORD) && mesh.vert.IsTexCoordEnabled();
        if (mesh.EN() > 0)
            report(cb, 0, QObject::tr("USD export: %1 edge(s) not written (curves come with scene export).").arg(mesh.EN()));

        // Vertices, compacted past deleted ones.
        std::vector<int> index(mesh.vert.size(), -1);
        std::vector<float> points, normals, colors, opacities, scalars, vertexSt;
        bool translucent = false;
        for (size_t i = 0; i < mesh.vert.size(); ++i) {
            const VCGVertex &v = mesh.vert[i];
            if (v.IsD())
                continue;
            index[i] = int(points.size() / 3);
            points.insert(points.end(), { v.cP()[0], v.cP()[1], v.cP()[2] });
            if (mask & M::IOM_VERTNORMAL)
                normals.insert(normals.end(), { v.cN()[0], v.cN()[1], v.cN()[2] });
            if (vertexColors) {
                colors.insert(colors.end(), { v.cC()[0] / 255.0f, v.cC()[1] / 255.0f, v.cC()[2] / 255.0f });
                opacities.push_back(v.cC()[3] / 255.0f);
                translucent |= v.cC()[3] < 255;
            }
            if (mask & M::IOM_VERTQUALITY)
                scalars.push_back(v.cQ());
            if (vertexTexCoords)
                vertexSt.insert(vertexSt.end(), { v.cT().U(), v.cT().V() });
        }

        // Faces: polygons rebuilt from faux edges as PLY's exporter does (VISITED is scratch
        // state, FF adjacency enabled for the save), each written with its representative
        // triangle's color and texture slot.
        std::vector<int32_t> counts, indices, faceSlot;
        std::vector<float> wedgeSt;
        const bool restoreFF = polygons && !mesh.face.IsFFAdjacencyEnabled();
        if (faces) {
            if (polygons) {
                if (restoreFF)
                    mesh.face.EnableFFAdjacency();
                vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
                vcg::tri::PolygonSupport<VCGMesh, VCGMesh>::ClearInconsistentFauxEdges(mesh);
                vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);
            }
            std::vector<VCGMesh::VertexPointer> loop;
            std::vector<VCGMesh::FacePointer> group;
            std::vector<std::pair<VCGMesh::FacePointer, int>> corners;
            for (VCGFace &f : mesh.face) {
                if (f.IsD() || (polygons && f.IsV()))
                    continue;
                corners.clear();
                if (polygons) {
                    vcg::tri::PolygonSupport<VCGMesh, VCGMesh>::ExtractPolygon(&f, loop, group, corners);
                    if (corners.size() > 3)
                        std::reverse(corners.begin(), corners.end());
                } else {
                    for (int k = 0; k < 3; ++k)
                        corners.emplace_back(&f, k);
                }
                if (corners.size() < 3)
                    continue;
                counts.push_back(int32_t(corners.size()));
                for (const auto &[face, k] : corners) {
                    indices.push_back(index[vcg::tri::Index(mesh, face->cV(k))]);
                    if (wedgeTexCoords)
                        wedgeSt.insert(wedgeSt.end(), { face->cWT(k).U(), face->cWT(k).V() });
                }
                if (faceColors) {
                    colors.insert(colors.end(), { f.cC()[0] / 255.0f, f.cC()[1] / 255.0f, f.cC()[2] / 255.0f });
                    opacities.push_back(f.cC()[3] / 255.0f);
                    translucent |= f.cC()[3] < 255;
                }
                faceSlot.push_back(wedgeTexCoords ? f.cWT(0).N() : -1);
            }
            if (polygons) {
                vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);
                if (restoreFF)
                    mesh.face.DisableFFAdjacency();
            }
        }

        report(cb, 30, QObject::tr("Building USD stage...") + QStringLiteral("\r"));
        const uint16_t uniform = ln::PropSlot::kFlagUniform;
        ln::StageBuilder sb;
        sb.SetDefaultPrim("World");
        sb.SetUpAxis("Z");
        if (options.unit != LengthUnit::Unknown)
            sb.SetMetersPerUnit(LengthUnits::metersPer(options.unit));
        ln::LayerBuilder &b = sb.GetLayerBuilder();
        QSet<QString> taken;
        b.begin_prim("World", "Xform");
        const std::string meshName = primName(QFileInfo(filename).completeBaseName(), QStringLiteral("Mesh"), taken);
        const std::string meshPath = "/World/" + meshName;

        // The material slots the faces use, each bound to its faces by a GeomSubset, or to the
        // whole mesh when there is only one and every face uses it.
        std::map<int, std::vector<int32_t>> slotFaces;
        size_t unboundFaces = 0;
        const int slotCount = std::max(int(mesh.textures.size()),
            textureContext && textureContext->materialSet ? int(textureContext->materialSet->entries.size()) : 0);
        for (size_t f = 0; f < faceSlot.size(); ++f) {
            if (faceSlot[f] >= 0 && faceSlot[f] < slotCount)
                slotFaces[faceSlot[f]].push_back(int32_t(f));
            else
                ++unboundFaces;
        }
        const bool wholeMesh = slotFaces.size() == 1 && unboundFaces == 0;

        b.begin_prim(meshName, faces ? "Mesh" : "Points");
        if (!slotFaces.empty())
            applyMaterialBindingApi(b);
        addTyped(b, "points", ln::Value::MakeFloatCompArray(points, ln::TypeId::Point3f, 3), "point3f[]");
        {
            float lo[3] = { points[0], points[1], points[2] }, hi[3] = { points[0], points[1], points[2] };
            for (size_t i = 0; i < points.size(); ++i) {
                lo[i % 3] = std::min(lo[i % 3], points[i]);
                hi[i % 3] = std::max(hi[i % 3], points[i]);
            }
            addTyped(b, "extent", ln::Value::MakeFloatCompArray({ lo[0], lo[1], lo[2], hi[0], hi[1], hi[2] },
                                      ln::TypeId::Float3, 3), "float3[]");
        }
        if (faces) {
            addTyped(b, "faceVertexCounts", ln::Value::MakeIntArray(counts), "int[]");
            addTyped(b, "faceVertexIndices", ln::Value::MakeIntArray(indices), "int[]");
            addTyped(b, "subdivisionScheme", ln::Value::MakeToken("none"), "token", uniform);
        }
        if (!normals.empty()) {
            addTyped(b, "normals", ln::Value::MakeFloatCompArray(normals, ln::TypeId::Normal3f, 3), "normal3f[]");
            setInterpolation(b, "normals", "vertex");
        }
        if (!colors.empty()) {
            const char *interpolation = faceColors ? "uniform" : "vertex";
            addTyped(b, "primvars:displayColor", ln::Value::MakeFloatCompArray(colors, ln::TypeId::Color3f, 3), "color3f[]");
            setInterpolation(b, "primvars:displayColor", interpolation);
            if (translucent) {
                addTyped(b, "primvars:displayOpacity", ln::Value::MakeFloatArray(opacities), "float[]");
                setInterpolation(b, "primvars:displayOpacity", interpolation);
            }
        }
        if (!wedgeSt.empty() || !vertexSt.empty()) {
            const bool wedge = !wedgeSt.empty();
            addTyped(b, "primvars:st",
                ln::Value::MakeFloatCompArray(wedge ? wedgeSt : vertexSt, ln::TypeId::Texcoord2f, 2), "texCoord2f[]");
            setInterpolation(b, "primvars:st", wedge ? "faceVarying" : "vertex");
        }
        if (!scalars.empty()) {
            addTyped(b, "primvars:scalar", ln::Value::MakeFloatArray(scalars), "float[]");
            setInterpolation(b, "primvars:scalar", "vertex");
        }

        std::map<int, std::string> slotMaterial;
        for (const auto &entry : slotFaces) {
            const MeshIOMaterialSlot *slot = textureContext && textureContext->materialSet
                    && entry.first < int(textureContext->materialSet->entries.size())
                ? &textureContext->materialSet->entries[size_t(entry.first)] : nullptr;
            slotMaterial[entry.first] = "/World/Looks/"
                + primName(slot ? slot->name : QString(), QStringLiteral("Material_"), taken);
        }
        if (wholeMesh) {
            b.add_relationship("material:binding", ln::Path(slotMaterial.begin()->second));
        } else if (!slotFaces.empty()) {
            addTyped(b, "subsetFamily:materialBind:familyType",
                ln::Value::MakeToken(unboundFaces == 0 ? "partition" : "nonOverlapping"), "token", uniform);
            for (const auto &[slot, list] : slotFaces) {
                b.begin_prim(primName(QStringLiteral("Slot%1").arg(slot), QString(), taken), "GeomSubset");
                applyMaterialBindingApi(b);
                addTyped(b, "elementType", ln::Value::MakeToken("face"), "token", uniform);
                addTyped(b, "familyName", ln::Value::MakeToken("materialBind"), "token", uniform);
                addTyped(b, "indices", ln::Value::MakeIntArray(list), "int[]");
                b.add_relationship("material:binding", ln::Path(slotMaterial[slot]));
                b.end_prim();
            }
        }
        b.end_prim();

        TextureWriter textures(filename, options, textureContext, cb);
        if (!slotMaterial.empty()) {
            b.begin_prim("Looks", "Scope");
            for (const auto &[slotIndex, path] : slotMaterial) {
                MeshIOMaterialSlot slot;
                if (textureContext && textureContext->materialSet
                    && slotIndex < int(textureContext->materialSet->entries.size()))
                    slot = textureContext->materialSet->entries[size_t(slotIndex)];
                // A slot listed only in mesh.textures is a base color texture, held by the
                // asset of the same index when there is one.
                if (!slot.baseColorTexture.isValid()) {
                    slot.baseColorTexture.assetIndex = slotIndex;
                    if (slotIndex < int(mesh.textures.size()))
                        slot.baseColorTexture.filePath = QString::fromStdString(mesh.textures[size_t(slotIndex)]);
                }
                writeMaterial(b, path, path.substr(path.rfind('/') + 1), slot, textures);
            }
            b.end_prim();
        }
        b.end_prim();
        b.finalize();
        const ln::Stage stage = sb.Build();

        report(cb, 60, QObject::tr("Writing USD file...") + QStringLiteral("\r"));
        const std::string path = QFile::encodeName(filename).toStdString();
        const QString suffix = ext(filename);
        bool ok = false;
        std::string error;
        if (suffix == QLatin1String("usdz")) {
            std::vector<uint8_t> layer, zip;
            const auto c = ln::WriteUSDCToMemory(layer, stage);
            if (c.success) {
                const auto z = ln::WriteUSDZFromUSDCAndAssetsToMemory(zip, layer.data(), layer.size(),
                    textures.packageEntries());
                std::ofstream out(path, std::ios::binary);
                out.write(reinterpret_cast<const char *>(zip.data()), std::streamsize(zip.size()));
                ok = z.success && out.good();
                error = z.error;
            } else {
                error = c.error;
            }
        } else if (suffix == QLatin1String("usda")) {
            const auto a = ln::WriteUSDAToFile(path, stage);
            ok = a.success;
            error = a.error;
        } else {
            const auto c = ln::WriteUSDCToFile(path, stage);
            ok = c.success;
            error = c.error;
        }
        if (!ok) {
            report(cb, 0, QObject::tr("USD write error: %1").arg(QString::fromStdString(error)));
            return kErrSaveWrite;
        }
        textures.finish();
        report(cb, 100, QObject::tr("USD file written.") + QStringLiteral("\r"));
        return 0;
    }

    QString errorString(int errCode) const override
    {
        switch (errCode) {
        case kErrOpen: return QObject::tr("Cannot open the USD file.");
        case kErrParse: return QObject::tr("The USD file could not be read.");
        case kErrConvert: return QObject::tr("The USD stage could not be converted to meshes.");
        case kErrNoGeometry: return QObject::tr("The USD stage holds no visible mesh or points.");
        case kErrSaveEmpty: return QObject::tr("No exportable geometry found.");
        case kErrSaveWrite: return QObject::tr("Failed to write the USD file.");
        default: return QObject::tr("Unknown USD import/export error.");
        }
    }
};
} // namespace

void registerUsdPlugin(MeshIOPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<USDPlugin>());
}
