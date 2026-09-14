#include "texturefilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "meshioplugin.h"
#include "src/core/pushpull.h"
#include "rastering.h"
#include "textureassociationutils.h"
#include "texture_packer.hpp"
#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/attribute_seam.h>
#include <vcg/complex/algorithms/parametrization/voronoi_atlas.h>
#include <vcg/complex/algorithms/point_sampling.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/algorithms/update/texture.h>
#include <wrap/io_trimesh/io_mask.h>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMatrix4x4>
#include <QVector4D>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
#include <optional>

namespace {
constexpr QLatin1StringView kFilterVoronoiAtlas("parametrize_by_voronoi_atlas_vcglib");
constexpr QLatin1StringView kFilterWedgeToVertex("convert_per_wedge_uv_to_per_vertex_uv");
constexpr QLatin1StringView kFilterVertexToWedge("convert_per_vertex_uv_to_per_wedge_uv");
constexpr QLatin1StringView kFilterPlanarMapping("parametrize_by_flat_plane");
constexpr QLatin1StringView kFilterTriangleMapping("parametrize_by_trivial_per_triangle_layout");
constexpr QLatin1StringView kFilterSetTexture("set_texture");
constexpr QLatin1StringView kFilterColorToTexture("transfer_color_from_vertex_to_texture");
constexpr QLatin1StringView kFilterTextureToVertexColor("transfer_color_from_texture_to_vertex_by_closest_point");
constexpr QLatin1StringView kFilterTransferToTexture("transfer_vertex_attributes_to_texture_by_closest_point");
constexpr QLatin1StringView kFilterObjectToTangentNormal("convert_object_space_normal_map_to_tangent_space");
constexpr QLatin1StringView kFilterPackTextures("pack_texture_images");

using Mask = vcg::tri::io::Mask;
namespace Tex = TextureAssociationUtils;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult r;
    r.success = false;
    r.documentModified = false;
    r.errorMessage = message;
    return r;
}

QString withSlotSuffix(const QString &basePath, int slotIndex, int slotCount)
{
    if (slotCount <= 1)
        return basePath;

    const QFileInfo info(basePath);
    const QString dir = info.absolutePath();
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();
    const QString fileName = suffix.isEmpty()
        ? QStringLiteral("%1_%2").arg(stem).arg(slotIndex)
        : QStringLiteral("%1_%2.%3").arg(stem).arg(slotIndex).arg(suffix);
    return QDir(dir).filePath(fileName);
}

int ensureTextureSlotIndices(VCGMesh &mesh)
{
    int maxSlot = -1;
    for (auto &face : mesh.face) {
        int faceSlot = face.WT(0).N();
        if (faceSlot < 0)
            faceSlot = 0;
        for (int k = 0; k < 3; ++k) {
            if (face.WT(k).N() < 0)
                face.WT(k).N() = faceSlot;
            maxSlot = std::max(maxSlot, int(face.WT(k).N()));
        }
    }
    return maxSlot + 1;
}

QStringList makeOutputPaths(const QString &requestedPath, int slotCount)
{
    QStringList out;
    out.reserve(slotCount);
    for (int i = 0; i < slotCount; ++i)
        out.push_back(QDir::toNativeSeparators(withSlotSuffix(requestedPath, i, slotCount)));
    return out;
}

QStringList overwriteOutputPaths(const Document::MeshEntry &entry, int slotCount)
{
    const QStringList availablePaths = Tex::associatedTexturePaths(entry);
    QStringList out;
    if (availablePaths.size() < slotCount)
        return out;
    out.reserve(slotCount);
    for (int i = 0; i < slotCount; ++i)
        out.push_back(availablePaths.at(i));
    return out;
}

int textureAssociationCount(const Document::MeshEntry &entry)
{
    return Document::meshTextureAssociationCount(entry);
}

void offsetTextureSlotIndices(VCGMesh &mesh, int slotOffset)
{
    if (slotOffset == 0)
        return;
    for (VCGFace &face : mesh.face) {
        for (int k = 0; k < 3; ++k) {
            if (face.WT(k).N() >= 0)
                face.WT(k).N() = short(face.WT(k).N() + slotOffset);
        }
    }
}

template<class Scalar>
vcg::Point3<Scalar> qMatrixMapPoint(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &point)
{
    const QVector4D mapped = matrix * QVector4D(point[0], point[1], point[2], 1.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

template<class Scalar>
vcg::Point3<Scalar> qMatrixMapDirection(const QMatrix4x4 &matrix, const vcg::Point3<Scalar> &direction)
{
    const QVector4D mapped = matrix * QVector4D(direction[0], direction[1], direction[2], 0.0f);
    return vcg::Point3<Scalar>(Scalar(mapped.x()), Scalar(mapped.y()), Scalar(mapped.z()));
}

std::unique_ptr<VCGMesh> makeWorldMesh(const Document::MeshEntry &entry, bool recomputeNormals = false)
{
    auto mesh = std::make_unique<VCGMesh>();
    if (vcg::tri::HasPerWedgeTexCoord(entry.mesh))
        mesh->face.EnableWedgeTexCoord();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(*mesh, entry.mesh);
    const QMatrix4x4 transform = entry.transform;
    for (VCGVertex &vertex : mesh->vert) {
        vertex.P() = qMatrixMapPoint(transform, vertex.cP());
        if (!recomputeNormals)
            vertex.N() = qMatrixMapDirection(transform, vertex.cN());
    }
    if (mesh->FN() > 0 && recomputeNormals)
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(*mesh);
    vcg::tri::UpdateBounding<VCGMesh>::Box(*mesh);
    return mesh;
}

void extractVertexForWedgeTexcoord(
    const VCGMesh &srcMesh,
    const VCGFace &face,
    int whichWedge,
    const VCGMesh &dstMesh,
    VCGVertex &vertex)
{
    (void) srcMesh;
    (void) dstMesh;
    vertex.ImportData(*face.cV(whichWedge));
    vertex.T() = face.cWT(whichWedge);
}

bool compareVertexTexcoord(const VCGMesh &mesh, const VCGVertex &a, const VCGVertex &b)
{
    (void) mesh;
    return a.cT() == b.cT();
}

int longestEdgeIndex(const VCGFace &face)
{
    const VCGMesh::CoordType &p0 = face.cP(0);
    const VCGMesh::CoordType &p1 = face.cP(1);
    const VCGMesh::CoordType &p2 = face.cP(2);
    const double d01 = vcg::SquaredDistance(p0, p1);
    const double d12 = vcg::SquaredDistance(p1, p2);
    const double d20 = vcg::SquaredDistance(p2, p0);
    if (d01 > d12)
        return d01 > d20 ? 0 : 2;
    return d12 > d20 ? 1 : 2;
}

using TexTriangle2 = vcg::Triangle2<VCGFace::TexCoordType::ScalarType>;

void buildTrianglesCache(
    std::vector<TexTriangle2> &triangles,
    int maxLevels,
    float border,
    float quadSize,
    int index = -1)
{
    TexTriangle2 &t0 = triangles[size_t(2 * index + 2)];
    TexTriangle2 &t1 = triangles[size_t(2 * index + 3)];
    if (index == -1) {
        t0.P(1).X() = quadSize - (0.5f + float(M_SQRT1_2)) * border;
        t0.P(0).X() = 0.5f * border;
        t0.P(1).Y() = 1.0f - t0.P(0).X();
        t0.P(0).Y() = 1.0f - t0.P(1).X();
        t0.P(2).X() = t0.P(0).X();
        t0.P(2).Y() = t0.P(1).Y();

        t1.P(1).X() = (0.5f + float(M_SQRT1_2)) * border;
        t1.P(0).X() = quadSize - 0.5f * border;
        t1.P(1).Y() = 1.0f - t1.P(0).X();
        t1.P(0).Y() = 1.0f - t1.P(1).X();
        t1.P(2).X() = t1.P(0).X();
        t1.P(2).Y() = t1.P(1).Y();
    } else {
        TexTriangle2 &t = triangles[size_t(index)];
        TexTriangle2::CoordType midPoint = (t.P(0) + t.P(1)) / 2;
        TexTriangle2::CoordType vec10 = (t.P(0) - t.P(1)).Normalize() * (border / 2.0f);
        t0.P(1) = t.P(0);
        t1.P(0) = t.P(1);
        t0.P(2) = midPoint + vec10;
        t1.P(2) = midPoint - vec10;
        t0.P(0) = t.P(2) + ((t.P(0) - t.P(2)).Normalize() * border / float(M_SQRT2));
        t1.P(1) = t.P(2) + ((t.P(1) - t.P(2)).Normalize() * border / float(M_SQRT2));
    }
    if (--maxLevels <= 0)
        return;
    buildTrianglesCache(triangles, maxLevels, border, quadSize, 2 * index + 2);
    buildTrianglesCache(triangles, maxLevels, border, quadSize, 2 * index + 3);
}

template<class T>
T logBase2(T num)
{
    return T(std::log(num) / std::log(T(2)));
}

enum class TransferAttributeMode
{
    VertexColor,
    VertexNormal,
    VertexQuality,
    TextureColor
};

std::optional<TransferAttributeMode> transferModeFromId(const QString &id)
{
    if (id == QStringLiteral("vertex_color"))
        return TransferAttributeMode::VertexColor;
    if (id == QStringLiteral("vertex_normal"))
        return TransferAttributeMode::VertexNormal;
    if (id == QStringLiteral("vertex_quality"))
        return TransferAttributeMode::VertexQuality;
    if (id == QStringLiteral("texture_color"))
        return TransferAttributeMode::TextureColor;
    return std::nullopt;
}

vcg::Point3f decodeNormalMapPixel(QRgb px)
{
    vcg::Point3f n(
        (float(qRed(px)) / 255.0f) * 2.0f - 1.0f,
        (float(qGreen(px)) / 255.0f) * 2.0f - 1.0f,
        (float(qBlue(px)) / 255.0f) * 2.0f - 1.0f);
    const float len = n.Norm();
    if (!(len > 1e-12f))
        return vcg::Point3f(0.0f, 0.0f, 1.0f);
    return n / len;
}

QRgb encodeNormalMapPixel(const vcg::Point3f &n)
{
    vcg::Point3f nn = n;
    const float len = nn.Norm();
    if (!(len > 1e-12f))
        nn = vcg::Point3f(0.0f, 0.0f, 1.0f);
    else
        nn /= len;

    const int r = std::clamp(int(std::lround((nn.X() * 0.5f + 0.5f) * 255.0f)), 0, 255);
    const int g = std::clamp(int(std::lround((nn.Y() * 0.5f + 0.5f) * 255.0f)), 0, 255);
    const int b = std::clamp(int(std::lround((nn.Z() * 0.5f + 0.5f) * 255.0f)), 0, 255);
    return qRgba(r, g, b, 255);
}

vcg::Point3f fallbackTangentForNormal(const vcg::Point3f &normal)
{
    vcg::Point3f axis = (std::abs(normal.Z()) < 0.999f)
        ? vcg::Point3f(0.0f, 0.0f, 1.0f)
        : vcg::Point3f(0.0f, 1.0f, 0.0f);
    vcg::Point3f tangent = axis ^ normal;
    if (tangent.Norm() <= 1e-12f)
        tangent = vcg::Point3f(1.0f, 0.0f, 0.0f);
    else
        tangent.Normalize();
    return tangent;
}

class ObjectToTangentNormalSampler
{
public:
    ObjectToTangentNormalSampler(
        const QImage &sourceImage,
        QImage &targetImage,
        bool invertX,
        bool invertY,
        bool invertZ)
        : m_sourceImage(sourceImage)
        , m_targetImage(targetImage)
        , m_invertX(invertX)
        , m_invertY(invertY)
        , m_invertZ(invertZ)
    {
    }

    void InitCallback(vcg::CallBackPos *cb, int faceNo, int start = 0, int offset = 100)
    {
        m_cb = cb;
        m_faceNo = std::max(faceNo, 1);
        m_start = start;
        m_offset = offset;
        m_faceCnt = 0;
        m_currFace = nullptr;
    }

    void AddTextureSample(const VCGFace &f, const VCGMesh::CoordType &bary, const vcg::Point2i &tp, float edgeDist = 0.0f)
    {
        (void) edgeDist;
        const int tx = tp.X();
        const int ty = m_targetImage.height() - 1 - tp.Y();
        if (!validPixelCoord(m_targetImage, tx, ty))
            return;

        const float u = bary[0] * f.cWT(0).U() + bary[1] * f.cWT(1).U() + bary[2] * f.cWT(2).U();
        const float v = bary[0] * f.cWT(0).V() + bary[1] * f.cWT(1).V() + bary[2] * f.cWT(2).V();

        const int sx = ((int)std::floor(u * m_sourceImage.width())) % m_sourceImage.width();
        const int sy = ((int)std::floor((1.0f - v) * m_sourceImage.height())) % m_sourceImage.height();
        const int wrappedSx = (sx + m_sourceImage.width()) % m_sourceImage.width();
        const int wrappedSy = (sy + m_sourceImage.height()) % m_sourceImage.height();
        const vcg::Point3f objectNormal = decodeNormalMapPixel(m_sourceImage.pixel(wrappedSx, wrappedSy));

        const vcg::Point3f p0 = f.cV(0)->cP();
        const vcg::Point3f p1 = f.cV(1)->cP();
        const vcg::Point3f p2 = f.cV(2)->cP();
        const vcg::Point2f uv0(f.cWT(0).U(), 1.0f - f.cWT(0).V());
        const vcg::Point2f uv1(f.cWT(1).U(), 1.0f - f.cWT(1).V());
        const vcg::Point2f uv2(f.cWT(2).U(), 1.0f - f.cWT(2).V());

        const vcg::Point3f dp1 = p1 - p0;
        const vcg::Point3f dp2 = p2 - p0;
        const vcg::Point2f duv1 = uv1 - uv0;
        const vcg::Point2f duv2 = uv2 - uv0;
        const float det = duv1.X() * duv2.Y() - duv1.Y() * duv2.X();

        vcg::Point3f shadingNormal =
            f.cV(0)->cN() * bary[0]
            + f.cV(1)->cN() * bary[1]
            + f.cV(2)->cN() * bary[2];
        if (!(shadingNormal.Norm() > 1e-12f))
            shadingNormal = vcg::NormalizedTriangleNormal(f);
        shadingNormal.Normalize();

        vcg::Point3f tangent(1.0f, 0.0f, 0.0f);
        vcg::Point3f bitangent(0.0f, 1.0f, 0.0f);
        if (std::abs(det) > 1e-20f) {
            tangent = (dp1 * duv2.Y() - dp2 * duv1.Y()) / det;
            bitangent = (dp2 * duv1.X() - dp1 * duv2.X()) / det;

            tangent = tangent - shadingNormal * (tangent * shadingNormal);
            if (tangent.Norm() > 1e-12f)
                tangent.Normalize();
            else
                tangent = fallbackTangentForNormal(shadingNormal);

            bitangent = bitangent - shadingNormal * (bitangent * shadingNormal) - tangent * (bitangent * tangent);
            if (bitangent.Norm() > 1e-12f)
                bitangent.Normalize();
            else
                bitangent = (shadingNormal ^ tangent);
            if (bitangent.Norm() > 1e-12f)
                bitangent.Normalize();
        } else {
            tangent = fallbackTangentForNormal(shadingNormal);
            bitangent = shadingNormal ^ tangent;
            if (bitangent.Norm() > 1e-12f)
                bitangent.Normalize();
        }

        vcg::Point3f tangentNormal(
            objectNormal * tangent,
            objectNormal * bitangent,
            objectNormal * shadingNormal);
        if (!(tangentNormal.Norm() > 1e-12f))
            tangentNormal = vcg::Point3f(0.0f, 0.0f, 1.0f);
        else
            tangentNormal.Normalize();

        if (m_invertX)
            tangentNormal.X() = -tangentNormal.X();
        if (m_invertY)
            tangentNormal.Y() = -tangentNormal.Y();
        if (m_invertZ)
            tangentNormal.Z() = -tangentNormal.Z();

        m_targetImage.setPixel(tx, ty, encodeNormalMapPixel(tangentNormal));

        if (m_cb) {
            if (&f != m_currFace) {
                m_currFace = &f;
                ++m_faceCnt;
            }
            m_cb(m_start + m_faceCnt * m_offset / m_faceNo, "Converting normal map...");
        }
    }

private:
    const QImage &m_sourceImage;
    QImage &m_targetImage;
    vcg::CallBackPos *m_cb = nullptr;
    const VCGFace *m_currFace = nullptr;
    int m_faceNo = 1;
    int m_faceCnt = 0;
    int m_start = 0;
    int m_offset = 100;
    bool m_invertX = false;
    bool m_invertY = false;
    bool m_invertZ = false;
};

} // namespace

QString TextureFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.texture");
}

QString TextureFilterPlugin::name() const
{
    return QObject::tr("Texture Filters");
}

MeshFilterRunResult TextureFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));

    auto &entry = doc.mesh(meshIndex);
    auto &mesh = entry.mesh;

    if (filterId == QString::fromLatin1(kFilterVoronoiAtlas)) {
        if (mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh must have faces."));

        const int regionNum = params.getInt(QStringLiteral("regionNum"));
        const bool overlap = params.getBool(QStringLiteral("overlapFlag"));
        if (regionNum <= 0)
            return fail(QObject::tr("Approx. Region Num must be positive."));

        doc.beginFilterProgress(QObject::tr("Parametrize by Voronoi Atlas (vcglib)"));

        VCGMesh baseMesh;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(baseMesh, mesh);
        baseMesh.face.EnableFFAdjacency();
        baseMesh.vert.EnableVFAdjacency();
        baseMesh.face.EnableVFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(baseMesh);
        const int nonManifoldVertices = vcg::tri::Clean<VCGMesh>::CountNonManifoldVertexFF(baseMesh, false);
        const int nonManifoldEdges = vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(baseMesh, false);
        if (nonManifoldVertices > 0 || nonManifoldEdges > 0) {
            const QString message =
                QObject::tr("Mesh is not manifold (%1 non-manifold vertices, %2 non-manifold edges).")
                    .arg(nonManifoldVertices)
                    .arg(nonManifoldEdges);
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        VCGMesh paraMesh;
        paraMesh.face.EnableWedgeTexCoord();
        vcg::tri::VoronoiAtlas<VCGMesh>::VoronoiAtlasParam pp;
        pp.sampleNum = regionNum;
        pp.overlap = overlap;
        const RandomSeed seed = params.getRandomSeed();
        pp.randomSeed = seed.value;
        // The per-region coloring is a debugging aid: it is written onto the working mesh
        // and then appended into the atlas, so it would replace whatever per-vertex color
        // the layer arrived with.
        pp.colorizeRegions = false;
        pp.cb = doc.progressCallback() ? doc.progressCallback() : vcg::DummyCallBackPos;

        vcg::tri::VoronoiAtlas<VCGMesh>::Build(baseMesh, paraMesh, pp);
        // Build selects each region's faces in order to extract them, and Append carries
        // the flags across, so the atlas arrives with everything selected.
        vcg::tri::UpdateSelection<VCGMesh>::Clear(paraMesh);
        if (!overlap)
            vcg::tri::Clean<VCGMesh>::RemoveDuplicateVertex(paraMesh);
        vcg::tri::Clean<VCGMesh>::RemoveUnreferencedVertex(paraMesh);
        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(paraMesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(paraMesh);
        if (paraMesh.FN() > 0)
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(paraMesh);

        if (paraMesh.FN() <= 0) {
            // Every Voronoi region failed to unwrap. On a coarse mesh a region can cover
            // the whole closed surface, which is not homeomorphic to a disk and so cannot
            // be flattened; subdividing first gives the partition something to work with.
            const QString message = QObject::tr(
                "No region could be unwrapped, so the atlas is empty. This usually means the "
                "mesh is too coarse for the requested number of regions: subdivide it, or ask "
                "for fewer regions.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        // Vertex color is declared only when the source layer actually had some; the
        // atlas now inherits it rather than inventing it.
        const int ioMask = Mask::IOM_WEDGTEXCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL
            | (entry.ioMask & Mask::IOM_VERTCOLOR);
        const int newIndex = doc.addMesh(paraMesh, QStringLiteral("VoroAtlas"), ioMask);
        doc.finishFilterProgress(true, QObject::tr("Generated Voronoi atlas mesh."));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices = { newIndex };
        result.infoMessages = {
            QObject::tr("Generated Voronoi atlas with %1 regions after %2 iterations.")
                .arg(pp.vas.regionNum)
                .arg(pp.vas.iterNum),
            seed.message()
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterWedgeToVertex)) {
        if (mesh.VN() <= 0 || mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh must have vertices and faces."));
        if ((entry.ioMask & Mask::IOM_WEDGTEXCOORD) == 0)
            return fail(QObject::tr("Current mesh does not have per-wedge texture coordinates."));

        const int originalVertexCount = mesh.VN();
        const bool ok = vcg::tri::AttributeSeam::SplitVertex(
            mesh,
            extractVertexForWedgeTexcoord,
            compareVertexTexcoord);
        if (!ok)
            return fail(QObject::tr("Failed converting per-wedge UVs to per-vertex UVs."));

        vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
        entry.ioMask |= Mask::IOM_VERTTEXCOORD;

        const bool vertexCountChanged = (mesh.VN() != originalVertexCount);
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Converted per-wedge UVs to per-vertex UVs on '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            vertexCountChanged
                ? QObject::tr("Converted UVs and split vertices where wedge coordinates disagreed.")
                : QObject::tr("Converted UVs without splitting vertices.")
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterVertexToWedge)) {
        if (mesh.VN() <= 0 || mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh must have vertices and faces."));
        if ((entry.ioMask & Mask::IOM_VERTTEXCOORD) == 0)
            return fail(QObject::tr("Current mesh does not have per-vertex texture coordinates."));

        vcg::tri::UpdateTexture<VCGMesh>::WedgeTexFromVertexTex(mesh);
        entry.ioMask |= Mask::IOM_WEDGTEXCOORD;
        // Per-wedge UVs live in the mesh; writing them is a geometry change (undo
        // interns geometry by geometryRevision), so bump geometry, not material.
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Converted per-vertex UVs to per-wedge UVs on '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Converted per-vertex UVs to per-wedge UVs.")
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterPlanarMapping)) {
        if (mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh must have faces."));

        const QString projectionId = params.getEnum(QStringLiteral("projectionPlane"));
        int projectionPlane = -1;
        if (projectionId == QStringLiteral("xy"))
            projectionPlane = 0;
        else if (projectionId == QStringLiteral("xz"))
            projectionPlane = 1;
        else if (projectionId == QStringLiteral("yz"))
            projectionPlane = 2;
        if (projectionPlane < 0)
            return fail(QObject::tr("Unsupported projection plane '%1'.").arg(projectionId));

        const bool preserveAspect = params.getBool(QStringLiteral("aspectRatio"));
        const float sideGutter = float(params.getDouble(QStringLiteral("sideGutter")));
        if (sideGutter < 0.0f || sideGutter > 0.5f)
            return fail(QObject::tr("Side Gutter must be in the range [0.0, 0.5]."));

        const VCGMesh::CoordType planeVectors[3][2] = {
            { VCGMesh::CoordType(1, 0, 0), VCGMesh::CoordType(0, 1, 0) },
            { VCGMesh::CoordType(0, 0, 1), VCGMesh::CoordType(1, 0, 0) },
            { VCGMesh::CoordType(0, 1, 0), VCGMesh::CoordType(0, 0, 1) }
        };

        std::unique_ptr<VCGMesh> worldMesh = makeWorldMesh(entry, false);
        vcg::tri::UpdateTexture<VCGMesh>::WedgeTexFromPlane(
            *worldMesh,
            planeVectors[projectionPlane][0],
            planeVectors[projectionPlane][1],
            preserveAspect,
            sideGutter);

        for (size_t i = 0; i < mesh.face.size() && i < worldMesh->face.size(); ++i) {
            for (int k = 0; k < 3; ++k)
                mesh.face[i].WT(k) = worldMesh->face[i].WT(k);
        }

        entry.ioMask |= Mask::IOM_WEDGTEXCOORD;
        // Writing per-wedge UVs into the mesh is a geometry change (see above).
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Computed planar UV parametrization for '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Generated flat-plane UVs on the %1 projection plane.")
                .arg(projectionId.toUpper())
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterTriangleMapping)) {
        if (mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh must have faces."));

        int sideDim = params.getInt(QStringLiteral("sidedim"));
        const int textDim = params.getInt(QStringLiteral("textdim"));
        const int pixelBorder = params.getInt(QStringLiteral("border"));
        const QString methodId = params.getEnum(QStringLiteral("method"));
        const bool advanced = (methodId == QStringLiteral("space_optimizing"));
        if (!advanced && methodId != QStringLiteral("basic"))
            return fail(QObject::tr("Unsupported parametrization method '%1'.").arg(methodId));

        if (textDim <= 0)
            return fail(QObject::tr("Texture Dimension has an incorrect value."));
        if (pixelBorder < 0)
            return fail(QObject::tr("Inter-Triangle border has an incorrect value."));
        if (sideDim < 0)
            return fail(QObject::tr("Quads per line has an incorrect value."));

        vcg::CallBackPos *cb = doc.progressCallback();

        if (advanced) {
            const float border = float(pixelBorder) / float(textDim);
            double maxArea = -1.0;
            double minArea = DBL_MAX;
            std::vector<double> areas(mesh.face.size(), -1.0);
            int faceCount = 0;
            for (size_t i = 0; i < mesh.face.size(); ++i) {
                double area = vcg::DoubleArea(mesh.face[i]);
                if (area == 0.0)
                    area = DBL_MIN;
                maxArea = std::max(maxArea, area);
                minArea = std::min(minArea, area);
                areas[i] = area;
                ++faceCount;
            }
            if (faceCount == 0)
                return fail(QObject::tr("Current mesh must have at least one face."));

            const int bucketSize = int(std::ceil(logBase2(maxArea) - logBase2(minArea) + DBL_EPSILON));
            std::vector<std::vector<size_t>> buckets{size_t(bucketSize)};
            for (size_t i = 0; i < areas.size(); ++i) {
                if (areas[i] < 0.0)
                    continue;
                const int slot = int(std::ceil(logBase2(maxArea) - logBase2(areas[i]) + DBL_EPSILON)) - 1;
                if (slot < 0 || slot >= bucketSize)
                    continue;
                buckets[size_t(slot)].push_back(i);
            }

            int dim = 0;
            int halfeningLevels = 0;
            double qn = 0.0;
            double divisor = 2.0;
            int rest = faceCount;
            int oneFactor = 1;
            int sqrt2Factor = 1;
            bool enough = false;
            while (halfeningLevels < bucketSize) {
                int tmp = int(std::ceil(std::sqrt(qn + rest / divisor)));
                bool newEnough = true;
                if (sideDim != 0) {
                    newEnough = sideDim >= tmp;
                    tmp = sideDim;
                }
                if (newEnough
                    && 1.0 / tmp < (sqrt2Factor / double(M_SQRT2) + oneFactor) * border
                        + (oneFactor != sqrt2Factor
                               ? oneFactor * double(M_SQRT2) * 2.0 / textDim
                               : oneFactor * 2.0 / textDim)) {
                    break;
                }

                enough = newEnough;
                rest -= int(buckets[size_t(halfeningLevels)].size());
                qn += buckets[size_t(halfeningLevels)].size() / divisor;
                divisor *= 2.0;
                if (halfeningLevels % 2)
                    oneFactor *= 2;
                else
                    sqrt2Factor *= 2;
                dim = tmp;
                ++halfeningLevels;
            }

            if (!enough && halfeningLevels == bucketSize) {
                return fail(
                    QObject::tr("Quads per line aren't enough to obtain a correct parametrization. Try setting at least %1.")
                        .arg(int(std::ceil(std::sqrt(qn)))));
            }
            if (halfeningLevels == 0 || !enough)
                return fail(QObject::tr("Inter-Triangle border is too much."));

            std::vector<TexTriangle2> cache(size_t((1 << (halfeningLevels + 1)) - 2));
            buildTrianglesCache(cache, halfeningLevels, border, 1.0f / dim);

            TexTriangle2::CoordType origin;
            TexTriangle2::CoordType tmp;
            int bucketIndex = 0;
            int faceCounter = 0;
            auto it = buckets[0].begin();
            int currentLevel = 1;
            for (int i = 0; i < dim && faceCounter < faceCount; ++i) {
                origin.Y() = -float(i) / dim;
                for (int j = 0; j < dim && faceCounter < faceCount; ++j) {
                    origin.X() = float(j) / dim;
                    for (int pos = (1 << currentLevel) - 2;
                         pos < (1 << (currentLevel + 1)) - 2 && faceCounter < faceCount;
                         ++pos, ++faceCounter) {
                        while (it == buckets[size_t(bucketIndex)].end()) {
                            if (++bucketIndex < halfeningLevels) {
                                ++currentLevel;
                                pos = 2 * pos + 2;
                            }
                            it = buckets[size_t(bucketIndex)].begin();
                        }
                        const size_t faceIndex = *it;
                        int longestEdge = longestEdgeIndex(mesh.face[faceIndex]);
                        TexTriangle2 &triangle = cache[size_t(pos)];
                        tmp = triangle.P(0) + origin;
                        mesh.face[faceIndex].WT(longestEdge) = VCGFace::TexCoordType(tmp.X(), tmp.Y());
                        mesh.face[faceIndex].WT(longestEdge).N() = 0;
                        longestEdge = (longestEdge + 1) % 3;
                        tmp = triangle.P(1) + origin;
                        mesh.face[faceIndex].WT(longestEdge) = VCGFace::TexCoordType(tmp.X(), tmp.Y());
                        mesh.face[faceIndex].WT(longestEdge).N() = 0;
                        longestEdge = (longestEdge + 1) % 3;
                        tmp = triangle.P(2) + origin;
                        mesh.face[faceIndex].WT(longestEdge) = VCGFace::TexCoordType(tmp.X(), tmp.Y());
                        mesh.face[faceIndex].WT(longestEdge).N() = 0;
                        ++it;
                        if (cb && !(*cb)(faceCounter * 100 / faceCount, "Generating parametrization..."))
                            return fail(QObject::tr("Operation canceled."));
                    }
                }
            }
        } else {
            const int faceNo = int(mesh.face.size());
            int undeletedFaces = 0;
            for (const VCGFace &face : mesh.face) {
                
                    ++undeletedFaces;
            }
            const int optimalDim = int(std::ceil(std::sqrt(undeletedFaces / 2.0)));
            if (sideDim == 0)
                sideDim = optimalDim;
            else if (optimalDim > sideDim) {
                return fail(
                    QObject::tr("Quads per line aren't enough to obtain a correct parametrization. Try setting at least %1.")
                        .arg(optimalDim));
            }

            const float border = float(pixelBorder) / float(textDim);
            if (border * (1.0f + float(M_SQRT2)) + 2.0f / textDim > 1.0f / sideDim)
                return fail(QObject::tr("Inter-Triangle border is too much."));

            const float borderSq2 = border / float(M_SQRT2);
            const float halfBorder = border / 2.0f;
            bool odd = true;
            VCGFace::TexCoordType bottomLeft, topRight;
            int faceCounter = 0;
            bottomLeft.V() = 1.0f;
            for (int i = 0; i < sideDim && faceCounter < faceNo; ++i) {
                topRight.V() = bottomLeft.V();
                topRight.U() = 0.0f;
                bottomLeft.V() = 1.0f - 1.0f / sideDim * (i + 1);
                for (int j = 0; j < 2 * sideDim && faceCounter < faceNo; ++faceCounter) {
                    int longestEdge = longestEdgeIndex(mesh.face[size_t(faceCounter)]);
                    if (odd) {
                        bottomLeft.U() = topRight.U();
                        topRight.U() = 1.0f / sideDim * (j / 2 + 1);
                        VCGFace::TexCoordType bl(bottomLeft.U() + halfBorder, bottomLeft.V() + halfBorder + borderSq2);
                        VCGFace::TexCoordType tr(topRight.U() - (halfBorder + borderSq2), topRight.V() - halfBorder);
                        bl.N() = 0;
                        tr.N() = 0;
                        mesh.face[size_t(faceCounter)].WT(longestEdge) = bl;
                        mesh.face[size_t(faceCounter)].WT((++longestEdge) % 3) = tr;
                        mesh.face[size_t(faceCounter)].WT((++longestEdge) % 3) = VCGFace::TexCoordType(bl.U(), tr.V());
                        mesh.face[size_t(faceCounter)].WT(longestEdge % 3).N() = 0;
                    } else {
                        VCGFace::TexCoordType bl(bottomLeft.U() + (halfBorder + borderSq2), bottomLeft.V() + halfBorder);
                        VCGFace::TexCoordType tr(topRight.U() - halfBorder, topRight.V() - (halfBorder + borderSq2));
                        bl.N() = 0;
                        tr.N() = 0;
                        mesh.face[size_t(faceCounter)].WT(longestEdge) = tr;
                        mesh.face[size_t(faceCounter)].WT((++longestEdge) % 3) = bl;
                        mesh.face[size_t(faceCounter)].WT((++longestEdge) % 3) = VCGFace::TexCoordType(tr.U(), bl.V());
                        mesh.face[size_t(faceCounter)].WT(longestEdge % 3).N() = 0;
                    }
                    if (cb && !(*cb)(faceCounter * 100 / faceNo, "Generating parametrization..."))
                        return fail(QObject::tr("Operation canceled."));
                    odd = !odd;
                    ++j;
                }
            }
        }

        entry.ioMask |= Mask::IOM_WEDGTEXCOORD;
        // Writing per-wedge UVs into the mesh is a geometry change (see above).
        doc.markMeshGeometryChanged(
            meshIndex,
            QObject::tr("Computed trivial per-triangle UV parametrization for '%1'.").arg(entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            advanced
                ? QObject::tr("Generated space-optimizing triangle-by-triangle UVs.")
                : QObject::tr("Generated basic triangle-by-triangle UVs.")
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterSetTexture)) {
        if (mesh.VN() <= 0 || mesh.FN() <= 0)
            return fail(QObject::tr("Current mesh must have vertices and faces."));

        const bool hasTexCoords = (entry.ioMask & (Mask::IOM_WEDGTEXCOORD | Mask::IOM_VERTTEXCOORD)) != 0;
        if (!hasTexCoords)
            return fail(QObject::tr("Current mesh does not have texture coordinates."));

        QString displayName;
        std::vector<MeshIOTextureAsset> newAssets;
        if (params.getBool(QStringLiteral("use_dummy_texture"))) {
            const int imageSize = params.getInt(QStringLiteral("dummy_img_size"));
            const int checkSize = params.getInt(QStringLiteral("dummy_check_size"));
            const QString dummyType = params.getEnum(QStringLiteral("dummy_type"));
            if (imageSize <= 0)
                return fail(QObject::tr("Dummy size has an incorrect value."));
            if (checkSize <= 0)
                return fail(QObject::tr("Check size has an incorrect value."));
            const bool checkerboard = (dummyType != QStringLiteral("grid"));
            const QImage dummyTexture = Tex::makeDummyTexture(imageSize, checkSize, checkerboard);
            displayName = checkerboard
                ? QStringLiteral("Dummy Checkerboard")
                : QStringLiteral("Dummy Grid");
            newAssets.push_back(Tex::makeTextureAssetFromImage(dummyTexture, displayName));
        } else {
            const QString chosenPath = params.getFileOpen(QStringLiteral("textName")).trimmed();
            if (chosenPath.isEmpty())
                return fail(QObject::tr("Texture file not specified."));

            const QString normalizedPath = Tex::normalizeExistingPath(chosenPath);
            const QFileInfo info(normalizedPath);
            if (!info.exists() || !info.isFile())
                return fail(QObject::tr("Texture file '%1' does not exist.").arg(chosenPath));
            displayName = info.fileName();
            newAssets.push_back(Tex::makeTextureAssetFromPath(normalizedPath));
        }

        Tex::replaceTextureAssociations(entry, newAssets);

        doc.markMeshMaterialChanged(
            meshIndex,
            QObject::tr("Associated texture '%1' with '%2'.")
                .arg(displayName, entry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Set '%1' as the mesh base texture.").arg(displayName)
        };
        // Suggest the UI switch this mesh to textured shading so the new texture
        // is immediately visible.
        result.visualizationHints.push_back(
            { meshIndex, MeshFilterVisualizationAttribute::Texture });
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterColorToTexture)) {
        if ((entry.ioMask & Mask::IOM_VERTCOLOR) == 0)
            return fail(QObject::tr("Current mesh does not have per-vertex color."));

        const int textW = params.getInt(QStringLiteral("textW"));
        const int textH = params.getInt(QStringLiteral("textH"));
        const bool overwrite = params.getBool(QStringLiteral("overwrite"));
        const bool pullPush = params.getBool(QStringLiteral("pullpush"));
        const QString requestedPath = params.getFileSave(QStringLiteral("textName")).trimmed();

        if (textW <= 0)
            return fail(QObject::tr("Texture Width has an incorrect value."));
        if (textH <= 0)
            return fail(QObject::tr("Texture Height has an incorrect value."));
        if (!overwrite && requestedPath.isEmpty())
            return fail(QObject::tr("Texture file not specified."));
        if (overwrite && !Document::hasMeshTextureAssociation(entry))
            return fail(QObject::tr("Mesh has no associated texture to overwrite."));

        const int slotCount = std::max(1, ensureTextureSlotIndices(mesh));
        const int previousTextureCount = textureAssociationCount(entry);
        const QStringList outputPaths = overwrite
            ? overwriteOutputPaths(entry, slotCount)
            : makeOutputPaths(requestedPath, slotCount);
        if (outputPaths.size() != slotCount) {
            return fail(QObject::tr("Existing texture association does not match the used texture slots."));
        }

        std::vector<QImage> targetImages;
        targetImages.reserve(size_t(slotCount));
        for (int texIndex = 0; texIndex < slotCount; ++texIndex) {
            QImage img(QSize(textW, textH), QImage::Format_ARGB32);
            img.fill(qRgba(0, 0, 0, 0));
            targetImages.push_back(std::move(img));
        }

        VCGMeshFFAdjScope _ffAdj(mesh);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFaceFromTexCoord(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);

        RasterSampler sampler(targetImages);
        sampler.InitCallback(doc.progressCallback(), mesh.FN(), 0, 80);
        vcg::tri::SurfaceSampling<VCGMesh, RasterSampler>::Texture(mesh, sampler, textW, textH, true);

        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);

        for (int texIndex = 0; texIndex < slotCount; ++texIndex) {
            for (int y = 0; y < textH; ++y) {
                for (int x = 0; x < textW; ++x) {
                    const QRgb px = targetImages[size_t(texIndex)].pixel(x, y);
                    if (qAlpha(px) < 255 && (!pullPush || qAlpha(px) > 0))
                        targetImages[size_t(texIndex)].setPixel(x, y, px | 0xff000000);
                }
            }
            if (pullPush)
                vcg::PullPush(targetImages[size_t(texIndex)], qRgba(0, 0, 0, 0));
        }

        QString saveError;
        if (!Tex::saveImages(outputPaths, targetImages, saveError))
            return fail(saveError);

        const std::vector<MeshIOTextureAsset> outputAssets =
            Tex::makeTextureAssetsFromSavedImages(outputPaths, targetImages);

        const bool appendNewTextures = !overwrite && previousTextureCount > 0;
        if (appendNewTextures) {
            offsetTextureSlotIndices(entry.mesh, previousTextureCount);
            Tex::appendTextureAssociations(entry, outputAssets);
            ++entry.materialRevision;
            doc.markMeshGeometryChanged(
                meshIndex,
                QObject::tr("Baked vertex color to %1 new texture image(s) and appended them to '%2'.")
                    .arg(slotCount)
                    .arg(entry.name));
        } else {
            Tex::replaceTextureAssociations(entry, outputAssets);
            doc.markMeshMaterialChanged(
                meshIndex,
                QObject::tr("Baked vertex color to %1 texture image(s) for '%2'.")
                    .arg(slotCount)
                    .arg(entry.name));
        }

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            appendNewTextures
                ? QObject::tr("Saved %1 texture image(s) and appended them to the existing texture list.").arg(slotCount)
                : QObject::tr("Saved %1 texture image(s).").arg(slotCount)
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterTextureToVertexColor)) {
        const int sourceMeshIndex = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
        const int targetMeshIndex = params.getMesh(QStringLiteral("targetMesh"), doc.currentMeshIndex());
        const Document::MeshEntry &sourceEntry = doc.mesh(sourceMeshIndex);
        Document::MeshEntry &targetEntry = doc.mesh(targetMeshIndex);

        const int sourceTextureCount = textureAssociationCount(sourceEntry);
        std::vector<QImage> sourceImages;
        sourceImages.reserve(size_t(std::max(0, sourceTextureCount)));
        for (int textureIndex = 0; textureIndex < sourceTextureCount; ++textureIndex) {
            QImage image;
            QString imageError;
            if (!Tex::loadAssociatedTextureImage(sourceEntry, textureIndex, image, imageError))
                return fail(imageError);
            sourceImages.push_back(std::move(image));
        }

        const int requestedTextureSlot = params.getTextureRef(QStringLiteral("sourceTexture"), 0);
        if (requestedTextureSlot < 0)
            return fail(QObject::tr("Source Texture must be automatic or a positive selection."));
        if (requestedTextureSlot > int(sourceImages.size())) {
            return fail(
                QObject::tr("Source Texture %1 is out of range. The source mesh has %2 associated texture(s).")
                    .arg(requestedTextureSlot)
                    .arg(sourceImages.size()));
        }

        std::unique_ptr<VCGMesh> sourceWorldMesh = makeWorldMesh(sourceEntry, true);
        std::unique_ptr<VCGMesh> targetWorldMesh = makeWorldMesh(targetEntry, false);
        if (targetWorldMesh->VN() <= 0)
            return fail(QObject::tr("Target mesh must have vertices."));

        if (requestedTextureSlot > 0) {
            const short forcedSlot = short(requestedTextureSlot - 1);
            for (VCGFace &face : sourceWorldMesh->face) {
                for (int k = 0; k < 3; ++k)
                    face.WT(k).N() = forcedSlot;
            }
        }

        VCGMeshMarkScope _markSource(*sourceWorldMesh);
        VertexSampler sampler(*sourceWorldMesh, sourceImages, float(params.getDouble(QStringLiteral("upperBound"))));
        sampler.InitCallback(doc.progressCallback(), targetWorldMesh->VN(), 0, 100);
        vcg::tri::SurfaceSampling<VCGMesh, VertexSampler>::VertexUniform(
            *targetWorldMesh,
            sampler,
            targetWorldMesh->VN());

        const int count = std::min(targetWorldMesh->VN(), targetEntry.mesh.VN());
        for (int i = 0; i < count; ++i) {
            const VCGVertex &src = targetWorldMesh->vert[i];
            VCGVertex &dst = targetEntry.mesh.vert[i];
            dst.C() = src.C();
        }

        targetEntry.ioMask |= Mask::IOM_VERTCOLOR;
        doc.markMeshGeometryChanged(
            targetMeshIndex,
            QObject::tr("Transferred texture colors from '%1' to vertex colors of '%2'.")
                .arg(sourceEntry.name, targetEntry.name));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        if (requestedTextureSlot > 0) {
            result.infoMessages = {
                QObject::tr("Transferred texture slot %1 from '%2' to '%3'.")
                    .arg(requestedTextureSlot)
                    .arg(sourceEntry.name, targetEntry.name)
            };
        } else {
            result.infoMessages = {
                QObject::tr("Transferred texture colors from '%1' to '%2'.")
                    .arg(sourceEntry.name, targetEntry.name)
            };
        }
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterTransferToTexture)) {
        const int sourceMeshIndex = params.getMesh(QStringLiteral("sourceMesh"), doc.currentMeshIndex());
        const int targetMeshIndex = params.getMesh(QStringLiteral("targetMesh"), doc.currentMeshIndex());
        const QString attributeModeId = params.getEnum(QStringLiteral("AttributeEnum"));
        const std::optional<TransferAttributeMode> mode = transferModeFromId(attributeModeId);
        if (!mode)
            return fail(QObject::tr("Unsupported source attribute mode '%1'.").arg(attributeModeId));

        const int textW = params.getInt(QStringLiteral("textW"));
        const int textH = params.getInt(QStringLiteral("textH"));
        const bool overwrite = params.getBool(QStringLiteral("overwrite"));
        const bool pullPush = params.getBool(QStringLiteral("pullpush"));
        const QString requestedPath = params.getFileSave(QStringLiteral("textName")).trimmed();
        if (textW <= 0)
            return fail(QObject::tr("Texture Width has an incorrect value."));
        if (textH <= 0)
            return fail(QObject::tr("Texture Height has an incorrect value."));

        const Document::MeshEntry &sourceEntry = doc.mesh(sourceMeshIndex);
        Document::MeshEntry &targetEntry = doc.mesh(targetMeshIndex);
        if (overwrite && !Document::hasMeshTextureAssociation(targetEntry))
            return fail(QObject::tr("Target mesh has no associated texture to overwrite."));
        if (!overwrite && requestedPath.isEmpty())
            return fail(QObject::tr("Texture file not specified."));

        bool samplingFromTexture = false;
        switch (*mode) {
        case TransferAttributeMode::VertexColor:
            if ((sourceEntry.ioMask & Mask::IOM_VERTCOLOR) == 0)
                return fail(QObject::tr("Source mesh doesn't have per-vertex color."));
            break;
        case TransferAttributeMode::VertexNormal:
            break;
        case TransferAttributeMode::VertexQuality:
            if ((sourceEntry.ioMask & Mask::IOM_VERTQUALITY) == 0)
                return fail(QObject::tr("Source mesh doesn't have per-vertex quality."));
            break;
        case TransferAttributeMode::TextureColor:
            samplingFromTexture = true;
            if (sourceEntry.mesh.FN() <= 0)
                return fail(QObject::tr("Source mesh needs to have faces."));
            if ((sourceEntry.ioMask & Mask::IOM_WEDGTEXCOORD) == 0)
                return fail(QObject::tr("Source mesh does not have per-wedge texture coordinates."));
            break;
        }

        const int sourceTextureCount = samplingFromTexture ? textureAssociationCount(sourceEntry) : 0;
        if (samplingFromTexture && sourceTextureCount <= 0)
            return fail(QObject::tr("Source mesh does not have any associated texture."));

        std::vector<QImage> sourceImages;
        if (samplingFromTexture) {
            sourceImages.reserve(size_t(std::max(0, sourceTextureCount)));
            for (int textureIndex = 0; textureIndex < sourceTextureCount; ++textureIndex) {
                QImage image;
                QString imageError;
                if (!Tex::loadAssociatedTextureImage(sourceEntry, textureIndex, image, imageError))
                    return fail(imageError);
                sourceImages.push_back(std::move(image));
            }
        }

        if (!Document::hasMeshTextureAssociation(targetEntry))
            ensureTextureSlotIndices(targetEntry.mesh);
        const int targetSlotCount = std::max(1, ensureTextureSlotIndices(targetEntry.mesh));
        const int previousTextureCount = textureAssociationCount(targetEntry);
        const QStringList outputPaths = overwrite
            ? overwriteOutputPaths(targetEntry, targetSlotCount)
            : makeOutputPaths(requestedPath, targetSlotCount);
        if (outputPaths.size() != targetSlotCount)
            return fail(QObject::tr("Existing target texture association does not match the used texture slots."));

        std::vector<QImage> targetImages;
        targetImages.reserve(size_t(targetSlotCount));
        for (int texIndex = 0; texIndex < targetSlotCount; ++texIndex) {
            QImage img(QSize(textW, textH), QImage::Format_ARGB32);
            img.fill(qRgba(0, 0, 0, 0));
            targetImages.push_back(std::move(img));
        }

        std::unique_ptr<VCGMesh> sourceWorldMesh = makeWorldMesh(sourceEntry, true);
        std::unique_ptr<VCGMesh> targetWorldMesh = makeWorldMesh(targetEntry, false);
        ensureTextureSlotIndices(*targetWorldMesh);

        if (samplingFromTexture) {
            const int requestedTextureSlot = params.getTextureRef(QStringLiteral("sourceTexture"), 0);
            if (requestedTextureSlot < 0)
                return fail(QObject::tr("Source Texture must be zero or positive."));
            if (requestedTextureSlot > int(sourceImages.size())) {
                return fail(
                    QObject::tr("Source Texture %1 is out of range. The source mesh has %2 associated texture(s).")
                        .arg(requestedTextureSlot)
                        .arg(sourceImages.size()));
            }
            if (requestedTextureSlot > 0) {
                const short forcedSlot = short(requestedTextureSlot - 1);
                for (VCGFace &face : sourceWorldMesh->face) {
                    for (int k = 0; k < 3; ++k)
                        face.WT(k).N() = forcedSlot;
                }
            }
        }

        VCGMeshFFAdjScope _ffAdjTarget(*targetWorldMesh);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFaceFromTexCoord(*targetWorldMesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(*targetWorldMesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerFaceNormalized(*sourceWorldMesh);

        VCGMeshMarkScope _markSource(*sourceWorldMesh);
        if (!samplingFromTexture) {
            int vertexMode = 0;
            if (*mode == TransferAttributeMode::VertexNormal)
                vertexMode = 1;
            else if (*mode == TransferAttributeMode::VertexQuality)
                vertexMode = 2;

            TransferColorSampler sampler(*sourceWorldMesh, targetImages, float(params.getDouble(QStringLiteral("upperBound"))), vertexMode);
            sampler.InitCallback(doc.progressCallback(), targetWorldMesh->FN(), 0, 80);
            vcg::tri::SurfaceSampling<VCGMesh, TransferColorSampler>::Texture(
                *targetWorldMesh,
                sampler,
                textW,
                textH,
                false);
        } else {
            TransferColorSampler sampler(*sourceWorldMesh, targetImages, &sourceImages, float(params.getDouble(QStringLiteral("upperBound"))));
            sampler.InitCallback(doc.progressCallback(), targetWorldMesh->FN(), 0, 80);
            vcg::tri::SurfaceSampling<VCGMesh, TransferColorSampler>::Texture(
                *targetWorldMesh,
                sampler,
                textW,
                textH,
                false);
        }

        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(*targetWorldMesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(*targetWorldMesh);

        for (int texIndex = 0; texIndex < targetSlotCount; ++texIndex) {
            for (int y = 0; y < textH; ++y) {
                for (int x = 0; x < textW; ++x) {
                    const QRgb px = targetImages[size_t(texIndex)].pixel(x, y);
                    if (qAlpha(px) < 255 && (!pullPush || qAlpha(px) > 0))
                        targetImages[size_t(texIndex)].setPixel(x, y, px | 0xff000000);
                }
            }
            if (pullPush)
                vcg::PullPush(targetImages[size_t(texIndex)], qRgba(0, 0, 0, 0));
        }

        QString saveError;
        if (!Tex::saveImages(outputPaths, targetImages, saveError))
            return fail(saveError);
        const std::vector<MeshIOTextureAsset> outputAssets =
            Tex::makeTextureAssetsFromSavedImages(outputPaths, targetImages);

        QString attributeLabel = attributeModeId;
        attributeLabel.replace(QLatin1Char('_'), QLatin1Char(' '));
        const bool appendNewTextures = !overwrite && previousTextureCount > 0;
        if (appendNewTextures) {
            offsetTextureSlotIndices(targetEntry.mesh, previousTextureCount);
            Tex::appendTextureAssociations(targetEntry, outputAssets);
            ++targetEntry.materialRevision;
            doc.markMeshGeometryChanged(
                targetMeshIndex,
                QObject::tr("Transferred %1 into %2 new texture image(s) and appended them to '%3'.")
                    .arg(attributeLabel)
                    .arg(targetSlotCount)
                    .arg(targetEntry.name));
        } else {
            Tex::replaceTextureAssociations(targetEntry, outputAssets);
            doc.markMeshMaterialChanged(
                targetMeshIndex,
                QObject::tr("Transferred %1 into %2 texture image(s) for '%3'.")
                    .arg(attributeLabel)
                    .arg(targetSlotCount)
                    .arg(targetEntry.name));
        }

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            appendNewTextures
                ? QObject::tr("Saved %1 texture image(s) on '%2' and appended them to the existing texture list.")
                      .arg(targetSlotCount)
                      .arg(targetEntry.name)
                : QObject::tr("Saved %1 texture image(s) on '%2'.")
                      .arg(targetSlotCount)
                      .arg(targetEntry.name)
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterObjectToTangentNormal)) {
        const int availableTextureCount = textureAssociationCount(entry);
        if (availableTextureCount <= 0)
            return fail(QObject::tr("Current mesh must already have at least one associated texture slot."));

        const int chosenSlotValue = params.getTextureRef(QStringLiteral("targetTexture"), -1);
        if (chosenSlotValue <= 0)
            return fail(QObject::tr("Choose the target texture slot to convert."));
        const int selectedSlot = chosenSlotValue - 1;
        if (selectedSlot >= availableTextureCount)
            return fail(QObject::tr("Selected texture slot is out of range."));

        const int sourceSlotValue = params.getTextureRef(QStringLiteral("sourceNormalMap"), -1);
        if (sourceSlotValue <= 0)
            return fail(QObject::tr("Choose the source object-space normal map texture."));
        const int sourceSlot = sourceSlotValue - 1;
        if (sourceSlot >= availableTextureCount)
            return fail(QObject::tr("Selected source normal map texture is out of range."));

        const TextureOutputRefValue outputTarget =
            params.getTextureOutputRef(QStringLiteral("targetNormalMap"));
        QString outputPath;
        bool writeOutputToFile = false;
        if (outputTarget.overwriteExisting) {
            if (outputTarget.textureSlot < 0 || outputTarget.textureSlot >= availableTextureCount)
                return fail(QObject::tr("Selected output texture slot is out of range."));
            outputPath = Document::meshTextureSourcePath(entry, outputTarget.textureSlot).trimmed();
            writeOutputToFile = !outputPath.isEmpty();
        } else {
            outputPath = outputTarget.filePath.trimmed();
            writeOutputToFile = !outputPath.isEmpty();
        }
        const bool bindToPbr = params.getBool(QStringLiteral("bindAsPbrNormal"));
        const bool invertX = params.getBool(QStringLiteral("invertX"), false);
        const bool invertY = params.getBool(QStringLiteral("invertY"), false);
        const bool invertZ = params.getBool(QStringLiteral("invertZ"), false);
        const double normalScale = params.getDouble(QStringLiteral("normalScale"), 1.0);
        if (!writeOutputToFile && !outputTarget.overwriteExisting)
            return fail(QObject::tr("Output tangent-space normal map path not specified."));

        QImage sourceImage;
        QString sourceImageError;
        if (!Tex::loadAssociatedTextureImage(entry, sourceSlot, sourceImage, sourceImageError))
            return fail(sourceImageError);

        VCGMesh workMesh;
        if (vcg::tri::HasPerWedgeTexCoord(mesh))
            workMesh.face.EnableWedgeTexCoord();
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(workMesh, mesh);
        int keptFaceCount = 0;
        for (VCGFace &face : workMesh.face) {
            if (face.IsD())
                continue;
            const int faceSlot = std::max(0, int(face.cWT(0).N()));
            if (faceSlot != selectedSlot) {
                face.SetD();
                continue;
            }
            for (int k = 0; k < 3; ++k)
                face.WT(k).N() = 0;
            ++keptFaceCount;
        }
        if (keptFaceCount == 0)
            return fail(QObject::tr("No faces use the selected texture slot."));

        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(workMesh);
        if ((entry.ioMask & Mask::IOM_VERTNORMAL) == 0)
            vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(workMesh);

        QImage targetImage(sourceImage.size(), QImage::Format_ARGB32);
        targetImage.fill(qRgba(128, 128, 255, 255));

        workMesh.face.EnableFFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFaceFromTexCoord(workMesh);
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(workMesh);
        ObjectToTangentNormalSampler sampler(sourceImage, targetImage, invertX, invertY, invertZ);
        sampler.InitCallback(doc.progressCallback(), std::max(1, keptFaceCount), 0, 90);
        vcg::tri::SurfaceSampling<VCGMesh, ObjectToTangentNormalSampler>::Texture(
            workMesh,
            sampler,
            targetImage.width(),
            targetImage.height(),
            false);

        const QString nativeOutputPath = QDir::toNativeSeparators(outputPath);
        if (writeOutputToFile) {
            QString saveError;
            if (!Tex::saveImages({ nativeOutputPath }, std::vector<QImage>{ targetImage }, saveError))
                return fail(saveError);
        }

        int associatedIndex = -1;
        if (outputTarget.overwriteExisting) {
            associatedIndex = outputTarget.textureSlot;
            if (associatedIndex >= 0 && associatedIndex < int(entry.textureAssets.size())) {
                MeshIOTextureAsset &asset = entry.textureAssets[size_t(associatedIndex)];
                asset.image = targetImage;
                if (writeOutputToFile) {
                    asset.sourcePath = nativeOutputPath;
                    if (asset.name.trimmed().isEmpty())
                        asset.name = QFileInfo(nativeOutputPath).fileName();
                } else if (asset.name.trimmed().isEmpty()) {
                    asset.name = QObject::tr("Generated Tangent Normal");
                }
                Tex::rebuildLegacyTextureAssociation(entry);
            }
        } else {
            entry.textureAssets.push_back(Tex::makeTextureAssetFromImage(
                targetImage,
                QFileInfo(nativeOutputPath).fileName(),
                nativeOutputPath));
            Tex::rebuildLegacyTextureAssociation(entry);
            associatedIndex = int(entry.textureAssets.size()) - 1;
        }

        if (bindToPbr) {
            Tex::ensureMaterialSlotCount(entry, selectedSlot + 1);
            MeshIOMaterialSlot &slot = entry.materialSet.entries[size_t(selectedSlot)];
            if (associatedIndex >= 0 && associatedIndex < int(entry.textureAssets.size())) {
                const MeshIOTextureAsset &asset = entry.textureAssets[size_t(associatedIndex)];
                slot.normalTexture.fileName = asset.name.trimmed();
                slot.normalTexture.filePath = asset.sourcePath.trimmed();
            }
            slot.normalScale = float(std::max(0.0, normalScale));

            doc.markMeshMaterialChanged(
                meshIndex,
                QObject::tr("Converted object-space normal map to tangent space for '%1'.").arg(entry.name));

            MeshFilterRunResult result;
            result.success = true;
            result.documentModified = true;
            result.infoMessages = {
                QObject::tr("Saved tangent-space normal map for slot %1 and bound it as the PBR normal texture (associated texture index %2).")
                    .arg(selectedSlot + 1)
                    .arg(associatedIndex + 1)
            };
            return result;
        }

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = false;
        result.infoMessages = {
            QObject::tr("Saved tangent-space normal map for slot %1.").arg(selectedSlot + 1)
        };
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterPackTextures)) {
        const Document::MeshEntry &sourceEntry = doc.mesh(meshIndex);
        const VCGMesh &sourceMesh = sourceEntry.mesh;
        if (sourceMesh.VN() <= 0 || sourceMesh.FN() <= 0)
            return fail(QObject::tr("Pack Texture Images requires a non-empty triangular mesh."));
        if (!vcg::tri::HasPerWedgeTexCoord(sourceMesh))
            return fail(QObject::tr("Pack Texture Images requires per-wedge texture coordinates."));

        std::vector<int> usedGroups;
        for (const VCGFace &face : sourceMesh.face) {
            if (face.IsD())
                continue;
            const int group = face.cWT(0).N();
            if (group < 0)
                return fail(QObject::tr("A face references a negative texture index."));
            for (int corner = 1; corner < 3; ++corner) {
                if (face.cWT(corner).N() != group)
                    return fail(QObject::tr("A face references more than one texture."));
            }
            usedGroups.push_back(group);
        }
        std::sort(usedGroups.begin(), usedGroups.end());
        usedGroups.erase(std::unique(usedGroups.begin(), usedGroups.end()), usedGroups.end());
        if (usedGroups.empty())
            return fail(QObject::tr("Pack Texture Images requires used texture images."));
        if (usedGroups.size() < 2)
            return fail(QObject::tr("Pack Texture Images requires at least two used texture images."));

        const int outputCount = params.getInt(QStringLiteral("containerNum"));
        const int gutter = params.getInt(QStringLiteral("gutter"));
        if (outputCount <= 0 || outputCount >= int(usedGroups.size())) {
            return fail(QObject::tr(
                "Target textures must be between 1 and %1 for this mesh.")
                            .arg(std::max(0, int(usedGroups.size()) - 1)));
        }

        std::vector<QImage> srcTextures;
        srcTextures.reserve(usedGroups.size());
        for (const int group : usedGroups) {
            QImage image;
            QString error;
            if (!Tex::loadAssociatedTextureImage(sourceEntry, group, image, error))
                return fail(error);
            srcTextures.push_back(std::move(image));
        }

        doc.beginFilterProgress(QObject::tr("Pack Texture Images"));
        auto progress = [&](int pct, const char *label) {
            if (vcg::CallBackPos *cb = doc.progressCallback())
                (*cb)(pct, label);
        };
        progress(10, "Preparing texture groups...");

        VCGMesh outputMesh;
        outputMesh.face.EnableWedgeTexCoord();
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(outputMesh, sourceMesh);
        for (VCGFace &face : outputMesh.face) {
            if (face.IsD())
                continue;
            const auto it = std::lower_bound(
                usedGroups.begin(), usedGroups.end(), int(face.cWT(0).N()));
            const int compactGroup = int(it - usedGroups.begin());
            for (int corner = 0; corner < 3; ++corner)
                face.WT(corner).N() = compactGroup;
        }

        progress(50, "Packing texture images...");
        QString packingError;
        std::vector<QImage> packedTextures = TexturePacker::simplePacking(
            srcTextures, outputCount, gutter, outputMesh, &packingError);
        if (packedTextures.empty()) {
            doc.finishFilterProgress(false, packingError);
            return fail(packingError);
        }

        outputMesh.textures.clear();
        const QString outputName = QObject::tr("packed_textures_%1").arg(sourceEntry.name);
        const QMatrix4x4 outputTransform = sourceEntry.transform;
        const int outputMask =
            (sourceEntry.ioMask | Mask::IOM_WEDGTEXCOORD) & ~Mask::IOM_VERTTEXCOORD;
        const int outputIndex = doc.addMesh(outputMesh, outputName, outputMask);
        if (outputIndex < 0) {
            const QString message = QObject::tr("Failed to add packed-texture mesh to the document.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }
        Document::MeshEntry &outputEntry = doc.mesh(outputIndex);
        outputEntry.transform = outputTransform;

        std::vector<MeshIOTextureAsset> outputAssets;
        outputAssets.reserve(packedTextures.size());
        for (int i = 0; i < int(packedTextures.size()); ++i) {
            const QString name = QStringLiteral("packed_texture_%1.png").arg(i);
            outputAssets.push_back(
                Tex::makeTextureAssetFromImage(packedTextures[size_t(i)], name));
        }
        Tex::replaceTextureAssociations(outputEntry, outputAssets);
        doc.markMeshMaterialChanged(
            outputIndex,
            QObject::tr("Created packed-texture mesh '%1'.").arg(outputEntry.name));

        progress(100, "Packing complete.");
        doc.finishFilterProgress(true, QObject::tr("Pack Texture Images completed."));
        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.infoMessages = {
            QObject::tr("Packed %1 source images into %2 textures with a %3 px gutter.")
                .arg(srcTextures.size()).arg(packedTextures.size()).arg(gutter)
        };
        result.newMeshIndices = { outputIndex };
        return result;
    }

    return fail(QObject::tr("Unknown texture filter '%1'.").arg(filterId));
}

void registerTextureFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<TextureFilterPlugin>());
}
