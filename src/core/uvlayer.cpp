#include "uvlayer.h"

#include "textureassociationutils.h"

#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/parametrization/distortion.h>
#include <vcg/complex/algorithms/parametrization/uv_utils.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QMatrix4x4>
#include <QObject>
#include <QVector4D>

#include <algorithm>
#include <cmath>

namespace Tex = TextureAssociationUtils;

namespace UvLayerUtils {

QString buildUvLayer(const Document::MeshEntry &entry, int askedWidth, int askedHeight, UvLayer &layer,
                     bool inTexels, bool worldSpace)
{
    using Mask = vcg::tri::io::Mask;
    const VCGMesh &mesh = entry.mesh;
    if (mesh.FN() == 0)
        return QObject::tr("'%1' has no faces: a UV mapping needs triangles.").arg(entry.name);
    const bool wedgeUV = (entry.ioMask & Mask::IOM_WEDGTEXCOORD) && mesh.face.IsWedgeTexCoordEnabled();
    const bool vertexUV = (entry.ioMask & Mask::IOM_VERTTEXCOORD) && mesh.vert.IsTexCoordEnabled();
    if (!wedgeUV && !vertexUV)
        return QObject::tr("'%1' has no texture coordinates.").arg(entry.name);

    // The texel size of each texture: the one asked for, or that of the images, or, with neither,
    // the unit square (in which case the UV are left as they are).
    Tex::TextureSizes textureSizes = Tex::resolveTextureSizes(entry, askedWidth, askedHeight);
    if (!textureSizes.error.isEmpty())
        return textureSizes.error;
    const int textureCount = Document::meshTextureAssociationCount(entry);
    layer.sizes = std::move(textureSizes.sizes);
    layer.texelsKnown = textureSizes.known;
    layer.units = textureSizes.units;
    std::vector<QSizeF> &sizes = layer.sizes;

    // A single size serves every face (and they all count as texture 0); with several textures a
    // face must name one that exists. A face with no texture (see vcgFaceTextureGroup) is no part
    // of the mapping, and goes in with its UVs collapsed to a point on texture 0: with no UV area
    // it makes an island of its own that the texel, distortion and seam measures pass by, while
    // its 3D area still counts as surface the mapping leaves uncovered.
    UVMesh &um = layer.mesh;
    vcg::tri::Allocator<UVMesh>::AddVertices(um, mesh.vert.size());
    const bool moved = worldSpace && !entry.transform.isIdentity();
    for (size_t i = 0; i < mesh.vert.size(); ++i) {
        const auto &p = mesh.vert[i].cP();
        if (moved) {
            const QVector4D mapped = entry.transform * QVector4D(p[0], p[1], p[2], 1.0f);
            um.vert[i].P() = vcg::Point3d(mapped.x(), mapped.y(), mapped.z());
        } else {
            um.vert[i].P() = vcg::Point3d(p[0], p[1], p[2]);
        }
    }
    vcg::tri::Allocator<UVMesh>::AddFaces(um, size_t(mesh.FN()));
    size_t next = 0;
    for (const VCGFace &face : mesh.face) {
        if (face.IsD())
            continue;
        UVMeshFace &f = um.face[next++];
        const bool untextured = vcgFaceTextureGroup(entry.ioMask, face) < 0;
        int texture = wedgeUV ? face.cWT(0).N() : face.cV(0)->cT().N();
        if (sizes.size() == 1 || untextured)
            texture = 0;
        else if (texture < 0 || texture >= int(sizes.size()))
            return QObject::tr("A face uses texture %1, but the layer has %2 texture(s).")
                .arg(texture).arg(textureCount);
        const QSizeF &size = inTexels ? sizes[size_t(texture)] : QSizeF(1, 1);
        for (int c = 0; c < 3; ++c) {
            f.V(c) = &um.vert[vcg::tri::Index(mesh, face.cV(c))];
            // By value: the optional per-vertex cT() returns a copy, so a reference to its P()
            // would dangle -- GCC at -O3 read zeros through it, and found no fold in a folded layout.
            const vcg::Point2f uv = untextured ? vcg::Point2f(0, 0)
                                  : wedgeUV    ? face.cWT(c).P()
                                               : face.cV(c)->cT().P();
            f.WT(c).P() = vcg::Point2d(double(uv[0]) * size.width(), double(uv[1]) * size.height());
            f.WT(c).N() = short(texture);
        }
    }
    return {};
}

QString loadSeamTextures(const Document::MeshEntry &entry, size_t count, SeamTextures &textures)
{
    textures.images.clear();
    for (size_t t = 0; t < count; ++t) {
        QImage image;
        QString error;
        if (!Tex::loadAssociatedTextureImage(entry, int(t), image, error))
            return error;
        textures.images.push_back(image.convertToFormat(QImage::Format_ARGB32));
    }
    return {};
}

void findLayerSeams(UvLayer &layer, const SeamTextures *textures,
                    const vcg::tri::SeamDiscrepancy<UVMesh>::Options &options, MeasuredSeams &result)
{
    using SD = vcg::tri::SeamDiscrepancy<UVMesh>;
    using UV = vcg::tri::UV_Utils<UVMesh>;
    UVMesh &um = layer.mesh;
    vcg::tri::Clean<UVMesh>::RemoveDuplicateVertex(um);
    vcg::tri::Allocator<UVMesh>::CompactEveryVector(um);
    std::vector<SD::Seam> found;
    SD::FindSeams(um, found);
    std::vector<int> faceChart;
    const int chartNum = UV::PerWedgeCharts(um, faceChart);
    std::vector<double> chartAreaUV(size_t(chartNum), 0.0);
    for (size_t i = 0; i < um.face.size(); ++i)
        chartAreaUV[size_t(faceChart[i])] += std::abs(vcg::tri::Distortion<UVMesh, true>::AreaUV(&um.face[i]));

    result.found = int(found.size());
    result.seams.clear();
    for (const SD::Seam &seam : found) {
        if (!(chartAreaUV[size_t(faceChart[vcg::tri::Index(um, seam.f)])] > 0)
            || !(chartAreaUV[size_t(faceChart[vcg::tri::Index(um, seam.g)])] > 0))
            continue;
        MeasuredSeam measured;
        measured.seam = seam;
        if (textures) {
            const SD::Seam fromOtherSide = { seam.g, seam.ge, seam.f, seam.e };
            measured.fromFirst = SD::Discrepancy(seam, *textures, options);
            measured.fromSecond = SD::Discrepancy(fromOtherSide, *textures, options);
            if (std::isnan(measured.fromFirst) || std::isnan(measured.fromSecond))
                continue;
        } else {
            measured.fromFirst = measured.fromSecond = std::numeric_limits<double>::quiet_NaN();
        }
        result.seams.push_back(measured);
    }
}

} // namespace UvLayerUtils
