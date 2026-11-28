// USD import and export (.usda, .usdc, .usd, .usdz) through LightUSD's `next` core and its
// render-scene converter, Tydra Next. See docs/design/proposals/usd_support.md: load() reads
// the composed stage flattened into one layer and save() writes one layer (stage 1);
// loadScene() reads a layer per prim and saveScene() writes layers as prims (stage 2).
#include "plugins/io_usd/usdplugin.h"

#include "meshioplugin.h"
#include "meshiopluginmanager.h"

#include <vcg/complex/algorithms/clean.h>
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

// Turns the converted scene into meshes -- one VCGMesh with every visible prim baked into it
// (run), or a layer per prim placed by its own matrix (runLayers) -- and its UsdPreviewSurface
// materials into MeshLab's material slots. One slot per material whose diffuse color is a
// texture; the other materials give their faces a face color instead.
class SceneImporter
{
public:
    // What the import did that the log keeps goes to messages (MeshIOLoadContext::messages);
    // cb only shows progress.
    SceneImporter(const QString &filename, const ln::Stage &stage, const tn::RenderScene &scene,
        vcg::CallBackPos *cb, QStringList &messages)
        : m_filename(filename), m_stage(stage), m_scene(scene), m_cb(cb), m_messages(messages)
    {
    }

    // The scene flattened: every visible mesh, point and curve prim, placed by its world
    // transform, into mesh.
    int run(VCGMesh &mesh, int *outLoadMask, MeshIOMaterialSet *outMaterialSet)
    {
        Target flat(mesh);
        walk([&](const tn::SceneNode &, const QMatrix4x4 &world) -> std::pair<Target *, QMatrix4x4> {
            return { &flat, world };
        }, false);
        if (mesh.VN() == 0)
            return kErrNoGeometry;
        if (outLoadMask)
            *outLoadMask = flat.mask();
        if (outMaterialSet)
            *outMaterialSet = std::move(flat.materials);
        return 0;
    }

    // A layer per mesh, point and curve prim, in the prim's own coordinates, its world
    // transform the layer matrix, its displayName (or name) the label; an invisible prim is a
    // hidden layer.
    int runLayers(std::vector<MeshIOSceneLayer> &layers)
    {
        std::vector<std::unique_ptr<Target>> targets;
        walk([&](const tn::SceneNode &node, const QMatrix4x4 &world) -> std::pair<Target *, QMatrix4x4> {
            MeshIOSceneLayer &layer = layers.emplace_back();
            layer.mesh->face.EnableWedgeTexCoord();
            layer.transform = world;
            layer.label = label(node);
            layer.visible = node.visible;
            targets.push_back(std::make_unique<Target>(*layer.mesh));
            return { targets.back().get(), QMatrix4x4() };
        }, true);
        for (size_t i = 0; i < layers.size(); ++i) {
            layers[i].loadMask = targets[i]->mask();
            layers[i].materialSet = std::move(targets[i]->materials);
        }
        return layers.empty() ? kErrNoGeometry : 0;
    }

    bool averagedNormals() const { return m_averagedNormals; }

private:
    struct MaterialInfo {
        int slot = -1;
        bool hasColor = false;
        vcg::Color4b color = vcg::Color4b::White;
    };

    // A mesh being filled, and what has been found to fill it with.
    struct Target {
        explicit Target(VCGMesh &m) : mesh(m) {}
        VCGMesh &mesh;
        bool hasNormals = false, hasVertexColors = false, hasFaceColors = false;
        bool hasTexCoords = false, hasPolygons = false;
        MeshIOMaterialSet materials;
        size_t baseAssets = 0;
        std::unordered_map<int32_t, MaterialInfo> materialInfo;

        int mask() const
        {
            int mask = M::IOM_VERTCOORD;
            if (mesh.FN() > 0)
                mask |= M::IOM_FACEINDEX;
            if (mesh.EN() > 0)
                mask |= M::IOM_EDGEINDEX;
            if (hasPolygons)
                mask |= M::IOM_BITPOLYGONAL;
            if (hasNormals)
                mask |= M::IOM_VERTNORMAL;
            if (hasVertexColors)
                mask |= M::IOM_VERTCOLOR;
            if (hasFaceColors)
                mask |= M::IOM_FACECOLOR;
            if (hasTexCoords) {
                mask |= M::IOM_WEDGTEXCOORD;
                if (materials.entries.size() > 1)
                    mask |= M::IOM_WEDGTEXMULTI;
            }
            return mask;
        }
    };

    // Every mesh, point and curve prim, handed to the target place() names with the matrix to
    // bake into its coordinates. Invisible prims are skipped unless keepInvisible.
    template<class Place>
    void walk(Place place, bool keepInvisible)
    {
        int meshNodes = 0, pointNodes = 0, curveNodes = 0, skippedInvisible = 0, skippedOther = 0;
        for (const tn::SceneNode &node : m_scene.nodes) {
            const auto valid = [&node](size_t count) { return node.data_id >= 0 && size_t(node.data_id) < count; };
            const bool isMesh = node.type == tn::NodeType::Mesh && valid(m_scene.meshes.size());
            const bool isPoints = node.type == tn::NodeType::Points && valid(m_scene.points.size());
            const bool isCurves = node.type == tn::NodeType::Curves && valid(m_scene.curves.size());
            if (!isMesh && !isPoints && !isCurves) {
                if (node.type != tn::NodeType::Xform)
                    ++skippedOther;
                continue;
            }
            if (!node.visible && !keepInvisible) {
                ++skippedInvisible;
                continue;
            }
            const auto [target, bake] = place(node, worldMatrix(node.world_transform));
            m_t = target;
            if (isMesh) {
                appendMesh(m_scene.meshes[size_t(node.data_id)], bake);
                ++meshNodes;
            } else if (isPoints) {
                appendPoints(m_scene.points[size_t(node.data_id)], bake);
                ++pointNodes;
            } else {
                appendCurves(m_scene.curves[size_t(node.data_id)], bake);
                ++curveNodes;
            }
            report(m_cb, 10 + 80 * (meshNodes + pointNodes + curveNodes) / int(m_scene.nodes.size()),
                QObject::tr("Reading USD prims...") + QStringLiteral("\r"));
        }
        m_t = nullptr;
        if (skippedInvisible > 0)
            m_messages << QObject::tr("skipped %1 invisible prim(s).").arg(skippedInvisible);
        if (skippedOther > 0)
            m_messages << QObject::tr("skipped %1 prim(s) that are not meshes, points or curves "
                                      "(cameras, lights, skeletons...).").arg(skippedOther);
        for (const QString &name : std::as_const(m_droppedInputs))
            m_messages << QObject::tr("material input %1 is not represented in MeshLab and was dropped.").arg(name);
        m_messages << QObject::tr("%1 mesh prim(s), %2 point prim(s), %3 curve prim(s), %4 material(s) with a texture.")
                          .arg(meshNodes).arg(pointNodes).arg(curveNodes).arg(m_texturedMaterials);
    }

    // A layer's label: the prim's displayName when it has one, else its name.
    QString label(const tn::SceneNode &node) const
    {
        const ln::UsdPrim prim = m_stage.GetPrimAtPath(node.prim_path);
        if (prim && prim.GetMeta().displayNameAuthored() && !prim.GetMeta().displayName().empty())
            return QString::fromStdString(prim.GetMeta().displayName());
        return QString::fromStdString(node.name);
    }

    // The world transform as a column-vector matrix. USD stores row-vector matrices, whose
    // flat array is the transpose of Qt's row-major input.
    QMatrix4x4 worldMatrix(const tn::Matrix4 &m) const
    {
        return QMatrix4x4(m.m).transposed();
    }

    void appendMesh(const tn::RenderMesh &src, const QMatrix4x4 &world)
    {
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

        const int baseVertex = m_t->mesh.VN();
        auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(m_t->mesh, pointCount);
        for (size_t i = 0; i < pointCount; ++i, ++vi) {
            const QVector3D p = world.map(QVector3D(src.points[3 * i], src.points[3 * i + 1], src.points[3 * i + 2]));
            vi->P() = VCGMesh::CoordType(p.x(), p.y(), p.z());
            vi->C() = vcg::Color4b::White;
        }
        auto vertex = [&](size_t i) -> VCGVertex & { return m_t->mesh.vert[size_t(baseVertex) + i]; };

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
            m_t->hasNormals = true;
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
            m_t->hasVertexColors = true;
        }

        // Texture coordinates, by the primary UV set's interpolation; v is up in USD as in MeshLab.
        const tn::Interpolation ti = src.texcoords_0_interp;
        const bool hasTexCoords = m_t->mesh.face.IsWedgeTexCoordEnabled() && !src.texcoords_0.empty()
            && src.texcoords_0.size() == 2 * elementCount(ti, pointCount, faceCount, cornerCount);
        m_t->hasTexCoords |= hasTexCoords;

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

        auto fi = vcg::tri::Allocator<VCGMesh>::AddFaces(m_t->mesh, triangleCount);
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
                m_t->hasPolygons = true;
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
                m_t->hasFaceColors = true;
            } else {
                fi->C() = vcg::Color4b::White;
            }
            if (material.hasColor) {
                fi->C() = material.color;
                m_t->hasFaceColors = true;
            }
            // Wedge coordinates are an optional component: Document::loadMesh enables them, a
            // direct caller may not have.
            for (int k = 0; k < 3 && m_t->mesh.face.IsWedgeTexCoordEnabled(); ++k) {
                if (hasTexCoords) {
                    const size_t e = elementIndex(ti, vert[k], face, corner[k]);
                    fi->WT(k).U() = src.texcoords_0[2 * e];
                    fi->WT(k).V() = src.texcoords_0[2 * e + 1];
                }
                fi->WT(k).N() = material.slot;
            }
        }
    }

    void appendPoints(const tn::RenderPoints &src, const QMatrix4x4 &world)
    {
        const QMatrix4x4 normalMatrix = world.inverted().transposed();
        const size_t count = src.point_count();
        if (count == 0)
            return;
        const bool normals = src.normals.size() == 3 * count;
        const int components = src.colors.size() == 4 * count ? 4 : (src.colors.size() == 3 * count ? 3 : 0);
        const bool constantColor = src.colors.size() == 3 || src.colors.size() == 4;
        const tn::FloatChunked noOpacity;
        const tn::FloatChunked &opacities = src.opacities.size() == count ? src.opacities : noOpacity;
        auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(m_t->mesh, count);
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
        m_t->hasNormals |= normals;
        m_t->hasVertexColors |= components || constantColor;
    }

    // A curve prim as a polyline per curve, from the converter's tessellation (linear curves
    // pass through unchanged): one vertex per point, an edge between consecutive ones. Colors
    // per point become vertex colors.
    void appendCurves(const tn::RenderCurves &src, const QMatrix4x4 &world)
    {
        const size_t count = src.tessellated_points.size() / 3;
        if (count == 0)
            return;
        const bool colors = src.tessellated_colors.size() == 3 * count;
        const tn::FloatChunked noOpacity;
        const tn::FloatChunked &opacities = src.tessellated_opacities.size() == count ? src.tessellated_opacities : noOpacity;
        const int base = m_t->mesh.VN();
        auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(m_t->mesh, count);
        for (size_t i = 0; i < count; ++i, ++vi) {
            const QVector3D p = world.map(QVector3D(src.tessellated_points[3 * i], src.tessellated_points[3 * i + 1],
                src.tessellated_points[3 * i + 2]));
            vi->P() = VCGMesh::CoordType(p.x(), p.y(), p.z());
            vi->C() = colors ? toColor(src.tessellated_colors, i, 3, opacities, i) : vcg::Color4b::White;
        }
        m_t->hasVertexColors |= colors;
        // A closed polyline repeats its first point at the end, as MeshLab writes a loop and as
        // the converter closes a periodic curve: its last edge goes back to the first vertex,
        // and the repeated one is dropped (the document compacts it away).
        size_t start = 0;
        for (const uint32_t n : src.tessellated_vertex_counts) {
            if (start + n > count)
                break;
            const size_t first = size_t(base) + start, last = first + n - 1;
            const bool closed = n > 3 && m_t->mesh.vert[first].cP() == m_t->mesh.vert[last].cP();
            for (size_t k = 1; k < n; ++k)
                vcg::tri::Allocator<VCGMesh>::AddEdge(m_t->mesh, first + k - 1, closed && k == n - 1 ? first : first + k);
            if (closed)
                vcg::tri::Allocator<VCGMesh>::DeleteVertex(m_t->mesh, m_t->mesh.vert[last]);
            start += n;
        }
    }

    // What a scene material means for a face: its slot when its diffuse color is a texture,
    // its color when that is a constant. Computed once per material.
    MaterialInfo materialInfo(int32_t materialId)
    {
        if (materialId < 0 || size_t(materialId) >= m_scene.materials.size())
            return {};
        const auto cached = m_t->materialInfo.find(materialId);
        if (cached != m_t->materialInfo.end())
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
            info.slot = int(m_t->materials.entries.size());
            m_t->materials.entries.push_back(std::move(slot));
            ++m_texturedMaterials;
            noteDropped(*surface);
        } else {
            // A constant diffuse color, or white for a texture that could not be read.
            const tn::Float4 c = surface->diffuse_color.is_texture() ? tn::Float4{ 1, 1, 1, 1 }
                                                                     : surface->diffuse_color.value;
            info.hasColor = true;
            info.color = toColorValue(c.x, c.y, c.z, surface->opacity.is_value() ? surface->opacity.as_float() : 1.0f);
            noteDropped(*surface);
        }
        m_t->materialInfo.emplace(materialId, info);
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
                m_messages << QObject::tr("cannot decode texture %1 inside the package.").arg(entry);
                return ref;
            }
            ref.fileName = asset.name;
        } else {
            QString path = resolved;
            if (QFileInfo(path).isRelative())
                path = QFileInfo(m_filename).absoluteDir().filePath(path);
            if (!QFileInfo::exists(path)) {
                m_messages << QObject::tr("texture file not found: %1").arg(path);
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
            m_t->mesh.textures.push_back((asset.hasImage() ? asset.name : asset.sourcePath).toStdString());
            m_t->materials.textureAssets.insert(m_t->materials.textureAssets.begin() + qsizetype(m_t->baseAssets), asset);
            // Channel textures already appended move one place up.
            for (MeshIOMaterialSlot &slot : m_t->materials.entries)
                for (MeshIOMaterialTextureRef *r : { &slot.normalTexture, &slot.occlusionTexture, &slot.roughnessTexture })
                    if (r->assetIndex >= int(m_t->baseAssets))
                        ++r->assetIndex;
            ref.assetIndex = int(m_t->baseAssets++);
        } else if (asset.hasImage()) {
            ref.assetIndex = int(m_t->materials.textureAssets.size());
            m_t->materials.textureAssets.push_back(std::move(asset));
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
    const ln::Stage &m_stage;
    vcg::CallBackPos *m_cb;
    QStringList &m_messages;
    Target *m_t = nullptr;   // the mesh being filled, during walk()
    bool m_averagedNormals = false;
    int m_texturedMaterials = 0;
    QSet<QString> m_droppedInputs;
    std::unique_ptr<ln::USDZReader> m_package;
    QString m_packagePath;
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

// Where the textures of one save go, and the files that end up holding them. A scene save
// writes the textures of several layers, each with its own texture context; one file or image
// used by several is written once. Only the primary context (a one-layer save) hears back
// where its textures went.
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
    std::string assetPath(const MeshIOMaterialTextureRef &ref, const MeshIOTextureContext *context)
    {
        const MeshIOTextureAsset *asset = nullptr;
        if (context && context->textureAssets && ref.assetIndex >= 0
            && ref.assetIndex < int(context->textureAssets->size()))
            asset = &context->textureAssets->at(size_t(ref.assetIndex));
        const QString source = asset ? asset->sourcePath.trimmed() : ref.filePath.trimmed();
        const bool hasFile = !source.isEmpty() && QFileInfo(source).isFile();
        if (!hasFile && !(asset && asset->hasImage()))
            return {};
        const QString key = hasFile ? QFileInfo(source).absoluteFilePath()
                                    : QStringLiteral("asset:%1:%2").arg(quintptr(asset->image.cacheKey())).arg(ref.assetIndex);
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
        if (asset && context == m_context && ref.assetIndex < m_saved.size())
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
    const MeshIOMaterialSlot &slot, TextureWriter &textures, const MeshIOTextureContext *context)
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
            textures.assetPath(slot.baseColorTexture, context) },
        { "inputs:normal", "Normal", "outputs:rgb", "normal3f", ln::TypeId::Float3, "float3", true, true,
            textures.assetPath(slot.normalTexture, context) },
        { "inputs:occlusion", "Occlusion", "outputs:r", "float", ln::TypeId::Float, "float", true, false,
            textures.assetPath(slot.occlusionTexture, context) },
        { "inputs:roughness", "Roughness", "outputs:g", "float", ln::TypeId::Float, "float", true, false,
            textures.assetPath(slot.roughnessTexture, context) },
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

// A layer's geometry as USD arrays: vertices compacted past deleted ones, polygons rebuilt
// from faux edges as PLY's exporter does (VISITED is scratch state, FF adjacency enabled for
// the save), each face with its representative triangle's color and texture slot, and the
// edges as polylines, one curve per chain (Clean::EdgeMeshChains), so that a polyline's
// vertices stay shared when it is read back.
struct LayerArrays
{
    bool faces = false, faceColors = false, translucent = false;
    std::vector<float> points, normals, colors, opacities, scalars, vertexSt, wedgeSt;
    std::vector<int32_t> counts, indices, faceSlot;
    std::vector<float> curvePoints, curveColors;
    std::vector<int32_t> curveCounts;
};

LayerArrays gatherLayer(VCGMesh &mesh, int mask)
{
    LayerArrays a;
    a.faces = mesh.FN() > 0;
    const bool polygons = a.faces && (mask & M::IOM_BITPOLYGONAL);
    const bool vertexColors = (mask & M::IOM_VERTCOLOR) != 0;
    a.faceColors = a.faces && !vertexColors && (mask & M::IOM_FACECOLOR);
    const bool wedgeTexCoords = a.faces && (mask & M::IOM_WEDGTEXCOORD) && mesh.face.IsWedgeTexCoordEnabled();
    const bool vertexTexCoords = !wedgeTexCoords && (mask & M::IOM_VERTTEXCOORD) && mesh.vert.IsTexCoordEnabled();

    std::vector<int> index(mesh.vert.size(), -1);
    for (size_t i = 0; i < mesh.vert.size(); ++i) {
        const VCGVertex &v = mesh.vert[i];
        if (v.IsD())
            continue;
        index[i] = int(a.points.size() / 3);
        a.points.insert(a.points.end(), { v.cP()[0], v.cP()[1], v.cP()[2] });
        if (mask & M::IOM_VERTNORMAL)
            a.normals.insert(a.normals.end(), { v.cN()[0], v.cN()[1], v.cN()[2] });
        if (vertexColors) {
            a.colors.insert(a.colors.end(), { v.cC()[0] / 255.0f, v.cC()[1] / 255.0f, v.cC()[2] / 255.0f });
            a.opacities.push_back(v.cC()[3] / 255.0f);
            a.translucent |= v.cC()[3] < 255;
        }
        if (mask & M::IOM_VERTQUALITY)
            a.scalars.push_back(v.cQ());
        if (vertexTexCoords)
            a.vertexSt.insert(a.vertexSt.end(), { v.cT().U(), v.cT().V() });
    }

    const bool restoreFF = polygons && !mesh.face.IsFFAdjacencyEnabled();
    if (a.faces) {
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
            a.counts.push_back(int32_t(corners.size()));
            for (const auto &[face, k] : corners) {
                a.indices.push_back(index[vcg::tri::Index(mesh, face->cV(k))]);
                if (wedgeTexCoords)
                    a.wedgeSt.insert(a.wedgeSt.end(), { face->cWT(k).U(), face->cWT(k).V() });
            }
            if (a.faceColors) {
                a.colors.insert(a.colors.end(), { f.cC()[0] / 255.0f, f.cC()[1] / 255.0f, f.cC()[2] / 255.0f });
                a.opacities.push_back(f.cC()[3] / 255.0f);
                a.translucent |= f.cC()[3] < 255;
            }
            a.faceSlot.push_back(wedgeTexCoords ? f.cWT(0).N() : -1);
        }
        if (polygons) {
            vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);
            if (restoreFF)
                mesh.face.DisableFFAdjacency();
        }
    }

    if (mesh.EN() > 0 && (mask & M::IOM_EDGEINDEX)) {
        const bool restoreVE = !mesh.vert.IsVEAdjacencyEnabled();
        if (restoreVE)
            mesh.vert.EnableVEAdjacency();
        std::vector<int> chainOf, position;
        std::vector<float> arc, length;
        const int chainCount = vcg::tri::Clean<VCGMesh>::EdgeMeshChains(mesh, chainOf, position, arc, length);
        std::vector<std::vector<VCGEdge *>> chains(size_t(std::max(chainCount, 0)));
        for (size_t e = 0; e < mesh.edge.size(); ++e) {
            if (chainOf[e] < 0)
                continue;
            std::vector<VCGEdge *> &chain = chains[size_t(chainOf[e])];
            if (chain.size() <= size_t(position[e]))
                chain.resize(size_t(position[e]) + 1, nullptr);
            chain[size_t(position[e])] = &mesh.edge[e];
        }
        const auto addPoint = [&](const VCGVertex *v) {
            a.curvePoints.insert(a.curvePoints.end(), { v->cP()[0], v->cP()[1], v->cP()[2] });
            if (vertexColors)
                a.curveColors.insert(a.curveColors.end(), { v->cC()[0] / 255.0f, v->cC()[1] / 255.0f, v->cC()[2] / 255.0f });
        };
        for (const std::vector<VCGEdge *> &chain : chains) {
            if (chain.empty() || std::find(chain.begin(), chain.end(), nullptr) != chain.end())
                continue;
            // The walk starts at the end of the first edge that the second does not touch.
            VCGVertex *at = chain[0]->V(0);
            if (chain.size() > 1 && (chain[1]->V(0) == at || chain[1]->V(1) == at) && chain[0]->V(1) != chain[1]->V(0)
                && chain[0]->V(1) != chain[1]->V(1))
                at = chain[0]->V(1);
            addPoint(at);
            for (VCGEdge *e : chain) {
                at = e->V(0) == at ? e->V(1) : e->V(0);
                addPoint(at);
            }
            a.curveCounts.push_back(int32_t(chain.size() + 1));
        }
        if (restoreVE)
            mesh.vert.DisableVEAdjacency();
    }
    return a;
}

void addExtent(ln::LayerBuilder &b, const std::vector<float> &points)
{
    float lo[3] = { points[0], points[1], points[2] }, hi[3] = { points[0], points[1], points[2] };
    for (size_t i = 0; i < points.size(); ++i) {
        lo[i % 3] = std::min(lo[i % 3], points[i]);
        hi[i % 3] = std::max(hi[i % 3], points[i]);
    }
    addTyped(b, "extent", ln::Value::MakeFloatCompArray({ lo[0], lo[1], lo[2], hi[0], hi[1], hi[2] },
                              ln::TypeId::Float3, 3), "float3[]");
}

// In a scene, a layer's prim carries its label as displayName and its matrix as the prim's own
// transform, not baked into the points, so that moving it elsewhere moves it as in MeshLab.
// USD matrices act on row vectors: their row-major array is Qt's column-major one.
void placePrim(ln::LayerBuilder &b, const MeshIOSceneLayerView &view)
{
    if (!view.label.isEmpty()) {
        b.current()->meta().displayName() = view.label.toStdString();
        b.current()->meta().setDisplayNameAuthored();
    }
    if (view.transform.isIdentity())
        return;
    double m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = double(view.transform.constData()[i]);
    addTyped(b, "xformOp:transform", ln::Value::MakeMatrix4d(m), "matrix4d");
    addTyped(b, "xformOpOrder", ln::Value::MakeTokenArray({ std::string("xformOp:transform") }), "token[]",
        ln::PropSlot::kFlagUniform);
}

// A material a layer's faces are bound to, written under /World/Looks once every layer is.
struct PendingMaterial
{
    std::string path;
    MeshIOMaterialSlot slot;
    const MeshIOTextureContext *context = nullptr;
};

// One layer under /World: a Mesh, or Points when it has no faces, and its edges as linear
// BasisCurves beside it (alone, for a polyline layer). placed: a scene's layer, with its label
// and matrix (placePrim).
void writeLayer(ln::LayerBuilder &b, const MeshIOSceneLayerView &view, int mask, bool placed, QSet<QString> &taken,
    std::vector<PendingMaterial> &looks)
{
    VCGMesh &mesh = *view.mesh;
    const MeshIOTextureContext *context = view.textures;
    const LayerArrays a = gatherLayer(mesh, mask);
    const uint16_t uniform = ln::PropSlot::kFlagUniform;
    const bool surface = a.faces || a.curveCounts.empty();
    const std::string name = primName(view.label, QStringLiteral("Mesh"), taken);

    if (surface) {
        // The material slots the faces use, each bound to its faces by a GeomSubset, or to the
        // whole mesh when there is only one and every face uses it.
        std::map<int, std::vector<int32_t>> slotFaces;
        size_t unboundFaces = 0;
        const int slotCount = std::max(int(mesh.textures.size()),
            context && context->materialSet ? int(context->materialSet->entries.size()) : 0);
        for (size_t f = 0; f < a.faceSlot.size(); ++f) {
            if (a.faceSlot[f] >= 0 && a.faceSlot[f] < slotCount)
                slotFaces[a.faceSlot[f]].push_back(int32_t(f));
            else
                ++unboundFaces;
        }
        const bool wholeMesh = slotFaces.size() == 1 && unboundFaces == 0;

        b.begin_prim(name, a.faces ? "Mesh" : "Points");
        if (placed)
            placePrim(b, view);
        if (!slotFaces.empty())
            applyMaterialBindingApi(b);
        addTyped(b, "points", ln::Value::MakeFloatCompArray(a.points, ln::TypeId::Point3f, 3), "point3f[]");
        addExtent(b, a.points);
        if (a.faces) {
            addTyped(b, "faceVertexCounts", ln::Value::MakeIntArray(a.counts), "int[]");
            addTyped(b, "faceVertexIndices", ln::Value::MakeIntArray(a.indices), "int[]");
            addTyped(b, "subdivisionScheme", ln::Value::MakeToken("none"), "token", uniform);
        }
        if (!a.normals.empty()) {
            addTyped(b, "normals", ln::Value::MakeFloatCompArray(a.normals, ln::TypeId::Normal3f, 3), "normal3f[]");
            setInterpolation(b, "normals", "vertex");
        }
        if (!a.colors.empty()) {
            const char *interpolation = a.faceColors ? "uniform" : "vertex";
            addTyped(b, "primvars:displayColor", ln::Value::MakeFloatCompArray(a.colors, ln::TypeId::Color3f, 3), "color3f[]");
            setInterpolation(b, "primvars:displayColor", interpolation);
            if (a.translucent) {
                addTyped(b, "primvars:displayOpacity", ln::Value::MakeFloatArray(a.opacities), "float[]");
                setInterpolation(b, "primvars:displayOpacity", interpolation);
            }
        }
        if (!a.wedgeSt.empty() || !a.vertexSt.empty()) {
            const bool wedge = !a.wedgeSt.empty();
            addTyped(b, "primvars:st",
                ln::Value::MakeFloatCompArray(wedge ? a.wedgeSt : a.vertexSt, ln::TypeId::Texcoord2f, 2), "texCoord2f[]");
            setInterpolation(b, "primvars:st", wedge ? "faceVarying" : "vertex");
        }
        if (!a.scalars.empty()) {
            addTyped(b, "primvars:scalar", ln::Value::MakeFloatArray(a.scalars), "float[]");
            setInterpolation(b, "primvars:scalar", "vertex");
        }

        std::map<int, std::string> slotMaterial;
        for (const auto &entry : slotFaces) {
            MeshIOMaterialSlot slot;
            if (context && context->materialSet && entry.first < int(context->materialSet->entries.size()))
                slot = context->materialSet->entries[size_t(entry.first)];
            // A slot listed only in mesh.textures is a base color texture, held by the asset of
            // the same index when there is one.
            if (!slot.baseColorTexture.isValid()) {
                slot.baseColorTexture.assetIndex = entry.first;
                if (entry.first < int(mesh.textures.size()))
                    slot.baseColorTexture.filePath = QString::fromStdString(mesh.textures[size_t(entry.first)]);
            }
            slotMaterial[entry.first] = "/World/Looks/" + primName(slot.name, QStringLiteral("Material_"), taken);
            looks.push_back({ slotMaterial[entry.first], slot, context });
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
    }

    if (!a.curveCounts.empty()) {
        b.begin_prim(surface ? primName(view.label + QStringLiteral("_Edges"), QStringLiteral("Edges"), taken) : name,
            "BasisCurves");
        if (placed)
            placePrim(b, view);
        addTyped(b, "type", ln::Value::MakeToken("linear"), "token", uniform);
        addTyped(b, "wrap", ln::Value::MakeToken("nonperiodic"), "token", uniform);
        addTyped(b, "curveVertexCounts", ln::Value::MakeIntArray(a.curveCounts), "int[]");
        addTyped(b, "points", ln::Value::MakeFloatCompArray(a.curvePoints, ln::TypeId::Point3f, 3), "point3f[]");
        addExtent(b, a.curvePoints);
        if (!a.curveColors.empty()) {
            addTyped(b, "primvars:displayColor", ln::Value::MakeFloatCompArray(a.curveColors, ln::TypeId::Color3f, 3),
                "color3f[]");
            setInterpolation(b, "primvars:displayColor", "vertex");
        }
        b.end_prim();
    }
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
                | M::IOM_WEDGTEXCOORD | M::IOM_WEDGTEXMULTI | M::IOM_BITPOLYGONAL | M::IOM_EDGEINDEX,
            true, true };
    }

    int saveMaskCapability(const QString &) const override
    {
        return M::IOM_VERTCOORD | M::IOM_VERTNORMAL | M::IOM_VERTCOLOR | M::IOM_VERTQUALITY | M::IOM_VERTTEXCOORD
            | M::IOM_FACEINDEX | M::IOM_FACECOLOR | M::IOM_WEDGTEXCOORD | M::IOM_WEDGTEXMULTI | M::IOM_BITPOLYGONAL
            | M::IOM_EDGEINDEX;
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

    // A stage may declare itself Y-up; MeshLab keeps its frame unless asked.
    std::vector<MeshFilterParameterDescriptor> loadParameters(const QString &) const override
    {
        return { UpAxisOption::descriptor() };
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
        ln::Stage stage;
        tn::ConvertResult result;
        if (const int err = readStage(filename, cb, context, stage, result))
            return err;
        mesh.Clear();
        mesh.textures.clear();
        SceneImporter importer(filename, stage, result.scene, cb, context.messages);
        if (const int status = importer.run(mesh, outLoadMask, outMaterialSet))
            return status;
        UpAxisOption::apply(mesh, result.scene.up_axis == tn::RenderScene::UpAxis::Y, context);
        finishLoad(importer, stage, result, context);
        return 0;
    }

    bool holdsScenes(const QString &filename) const override { return canLoad(filename); }

    int loadScene(const QString &filename, std::vector<MeshIOSceneLayer> &layers, vcg::CallBackPos *cb,
        MeshIOLoadContext &context) const override
    {
        layers.clear();
        ln::Stage stage;
        tn::ConvertResult result;
        if (const int err = readStage(filename, cb, context, stage, result))
            return err;
        SceneImporter importer(filename, stage, result.scene, cb, context.messages);
        if (const int status = importer.runLayers(layers))
            return status;
        // The prims keep their coordinates; turning a Y-up stage Z-up, when asked, is a
        // rotation in front of every layer matrix.
        if (result.scene.up_axis == tn::RenderScene::UpAxis::Y) {
            if (UpAxisOption::requested(context)) {
                QMatrix4x4 up;
                up.rotate(90.0f, 1.0f, 0.0f, 0.0f);
                for (MeshIOSceneLayer &layer : layers)
                    layer.transform = up * layer.transform;
                context.messages << QObject::tr("the file is Y-up and was rotated by +90 degrees about X to Z-up "
                                                "in every layer matrix, as the Rotate Y-up to Z-up load option asks.");
            } else {
                context.messages << QObject::tr("the file is Y-up; its coordinates are kept as they are "
                                                "(the Rotate Y-up to Z-up load option turns it Z-up).");
            }
        }
        finishLoad(importer, stage, result, context);
        return 0;
    }

    int save(const QString &filename, VCGMesh &mesh, const MeshIOSaveOptions &options, vcg::CallBackPos *cb) const override
    {
        return save(filename, mesh, options, cb, nullptr);
    }

    int save(const QString &filename, VCGMesh &mesh, const MeshIOSaveOptions &options, vcg::CallBackPos *cb,
        const MeshIOTextureContext *textureContext) const override
    {
        // One layer, its coordinates as they are: no prim transform.
        MeshIOSceneLayerView view;
        view.mesh = &mesh;
        view.mask = options.mask;
        view.label = QFileInfo(filename).completeBaseName();
        view.textures = textureContext;
        return writeStage(filename, { view }, options, cb, false);
    }

    int saveScene(const QString &filename, const std::vector<MeshIOSceneLayerView> &layers,
        const MeshIOSaveOptions &options, vcg::CallBackPos *cb) const override
    {
        return writeStage(filename, layers, options, cb, true);
    }

private:
    // Writes layers under /World, each as writeLayer does, their materials under /World/Looks
    // with each texture written once, and the stage as usda, usdc or usdz by the suffix.
    // placed: a scene save, whose prims carry each layer's label and matrix.
    int writeStage(const QString &filename, const std::vector<MeshIOSceneLayerView> &layers,
        const MeshIOSaveOptions &options, vcg::CallBackPos *cb, bool placed) const
    {
        if (std::none_of(layers.begin(), layers.end(), [](const MeshIOSceneLayerView &v) { return v.mesh && v.mesh->VN() > 0; }))
            return kErrSaveEmpty;
        const int capability = saveMaskCapability(filename);

        report(cb, 30, QObject::tr("Building USD stage...") + QStringLiteral("\r"));
        ln::StageBuilder sb;
        sb.SetDefaultPrim("World");
        // Y, as MeshLab's default view shows the coordinates and as USD's own fallback says: a
        // Y-up stage read as it is (frames are kept) comes back as it was, and what stands up in
        // MeshLab stands up elsewhere. A Z-up stage read as it is comes back declared Y-up.
        sb.SetUpAxis("Y");
        if (options.unit != LengthUnit::Unknown)
            sb.SetMetersPerUnit(LengthUnits::metersPer(options.unit));
        ln::LayerBuilder &b = sb.GetLayerBuilder();
        QSet<QString> taken;
        std::vector<PendingMaterial> looks;
        b.begin_prim("World", "Xform");
        for (const MeshIOSceneLayerView &view : layers) {
            if (!view.mesh || view.mesh->VN() == 0)
                continue;
            const int mask = (view.mask != 0 ? view.mask : capability) & capability;
            writeLayer(b, view, mask, placed, taken, looks);
        }
        TextureWriter textures(filename, options, layers.size() == 1 ? layers.front().textures : nullptr, cb);
        if (!looks.empty()) {
            b.begin_prim("Looks", "Scope");
            for (const PendingMaterial &material : looks)
                writeMaterial(b, material.path, material.path.substr(material.path.rfind('/') + 1), material.slot,
                    textures, material.context);
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

    // Reads the composed stage and converts it. Polygons are triangulated by the converter,
    // which also reports which authored face and corner each triangle comes from: MeshLab
    // keeps the polygons as faux edges, and faceVarying data and GeomSubsets are indexed through
    // it. Normals are not computed there: MeshLab computes what the file lacks. Images are
    // decoded here, with QImage.
    static int readStage(const QString &filename, vcg::CallBackPos *cb, MeshIOLoadContext &context,
        ln::Stage &stage, tn::ConvertResult &result)
    {
        if (!QFileInfo::exists(filename))
            return kErrOpen;
        report(cb, 0, QObject::tr("Reading USD stage...") + QStringLiteral("\r"));
        std::string warn, err;
        if (!ln::LoadUSDComposed(QFile::encodeName(filename).toStdString(), &stage, &warn, &err)) {
            report(cb, 0, QObject::tr("USD load error: %1").arg(QString::fromStdString(err)));
            return kErrParse;
        }
        if (!warn.empty())
            context.messages << QObject::tr("reader warning: %1").arg(QString::fromStdString(warn).trimmed());
        tn::ConverterConfig config;
        config.mesh.triangulate = true;
        config.mesh.compute_normals = false;
        config.material.load_textures = false;
        config.asset_base_dir = QFile::encodeName(QFileInfo(filename).absolutePath()).toStdString();
        tn::RenderSceneConverter converter(config);
        report(cb, 5, QObject::tr("Converting USD stage...") + QStringLiteral("\r"));
        result = converter.Convert(stage);
        for (const std::string &w : result.warnings)
            context.messages << QObject::tr("converter warning: %1").arg(QString::fromStdString(w));
        if (!result.success) {
            report(cb, 0, QObject::tr("USD conversion error: %1").arg(QString::fromStdString(result.error)));
            return kErrConvert;
        }
        return 0;
    }

    // What every load reports at its end: averaged normals, and the unit. metersPerUnit falls
    // back to centimeters by the USD specification; the converter's own fallback (meters) is
    // not used, as it does not distinguish an authored value.
    static void finishLoad(const SceneImporter &importer, const ln::Stage &stage, const tn::ConvertResult &result,
        MeshIOLoadContext &context)
    {
        if (importer.averagedNormals())
            context.messages << QObject::tr("per-corner (faceVarying) normals were averaged per vertex.");
        const auto &meta = stage.GetMeta();
        const double metersPerUnit = meta.metersPerUnit_set ? result.scene.meters_per_unit : 0.01;
        context.unit = unitFromMetersPerUnit(metersPerUnit);
        if (!meta.metersPerUnit_set)
            context.messages << QObject::tr("the stage authors no metersPerUnit: centimeters, by the USD specification.");
        else if (context.unit == LengthUnit::Unknown)
            context.messages << QObject::tr("metersPerUnit %1 is not a unit MeshLab names: unit left unset.")
                                    .arg(metersPerUnit);
    }

public:
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
