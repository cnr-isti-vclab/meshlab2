#pragma once

#include "document.h"

#include <vcg/complex/complex.h>
#include <vcg/complex/algorithms/parametrization/seam_discrepancy.h>

#include <QImage>
#include <QSizeF>
#include <QString>

#include <cmath>
#include <vector>

// A layer's texture coordinates as the filters that measure or draw the UV mapping take them: the
// faces copied into a mesh of its own, in double precision, so that vcglib's UV algorithms (the
// charts, the distortion, the texel coverage, the seams) run on them exactly as TexMetro's do on its
// mesh, whatever the layer stores. The copy is the filter's own: nothing here touches the document.
namespace UvLayerUtils {

class UVMeshVertex;
class UVMeshFace;
struct UVMeshUsedTypes : vcg::UsedTypes<
    vcg::Use<UVMeshVertex>::AsVertexType,
    vcg::Use<UVMeshFace>::AsFaceType> {};
class UVMeshVertex : public vcg::Vertex<UVMeshUsedTypes, vcg::vertex::Coord3d, vcg::vertex::BitFlags> {};
class UVMeshFace : public vcg::Face<
    UVMeshUsedTypes,
    vcg::face::VertexRef,
    vcg::face::FFAdj,
    vcg::face::WedgeTexCoord2d,
    vcg::face::BitFlags> {};
class UVMesh : public vcg::tri::TriMesh<std::vector<UVMeshVertex>, std::vector<UVMeshFace>> {};

// The faces as a UVMesh, and the size in texels of each texture those faces use.
struct UvLayer
{
    UVMesh mesh;
    std::vector<QSizeF> sizes;
    bool texelsKnown = false; // the sizes are real ones, from the images or asked for, not the unit square
    QString units;            // how the UV were taken, for the report
};

// The texels a face covers per unit of length on the mesh, in a layer built in texels: the square
// root of its UV area over its 3D area, so that it reads like a raster's pixels per unit. 0 for a
// face without area, or without UV area (one with no texture).
inline double faceTexelDensity(const UVMeshFace &f)
{
    const double area3D = ((f.cP(1) - f.cP(0)) ^ (f.cP(2) - f.cP(0))).Norm();
    const double areaUV = std::abs((f.cWT(1).P() - f.cWT(0).P()) ^ (f.cWT(2).P() - f.cWT(0).P()));
    return area3D > 0 ? std::sqrt(areaUV / area3D) : 0;
}

// Fill `layer` from a layer of the document. The texture size is the one asked for (both sides
// positive), or that of the layer's images, or, with neither, the unit square. The UV are taken in
// texels, or, with inTexels false, as they are on the layer; the positions in world space, through
// the layer matrix, or, with worldSpace false, in the layer's own frame. Returns why that cannot be
// done, or an empty string.
QString buildUvLayer(const Document::MeshEntry &entry, int askedWidth, int askedHeight, UvLayer &layer,
                     bool inTexels = true, bool worldSpace = true);

// The textures of a layer as vcglib's SeamDiscrepancy reads them.
struct SeamTextures
{
    std::vector<QImage> images; // ARGB32, so that a texel is a word
    int Width(int texture) const { return images[size_t(texture)].width(); }
    int Height(int texture) const { return images[size_t(texture)].height(); }
    vcg::Point3d Texel(int texture, int x, int y) const
    {
        const QRgb c = reinterpret_cast<const QRgb *>(images[size_t(texture)].constScanLine(y))[x];
        return vcg::Point3d(qRed(c), qGreen(c), qBlue(c));
    }
};

// Read the first `count` textures of a layer. Returns why that cannot be done, or an empty string.
QString loadSeamTextures(const Document::MeshEntry &entry, size_t count, SeamTextures &textures);

using Seam = vcg::tri::SeamDiscrepancy<UVMesh>::Seam;

// A seam of the layer, with its discrepancy from each of its two sides (see MeasuredSeams).
struct MeasuredSeam
{
    Seam seam;
    double fromFirst = 0;  // measured from the face `seam.f` lists, NaN without textures
    double fromSecond = 0; // and from the other, `seam.g`
    double mean() const { return 0.5 * (fromFirst + fromSecond); }
};

struct MeasuredSeams
{
    int found = 0;                   // the seams the UV cut, before any is left out
    std::vector<MeasuredSeam> seams; // those kept: both islands have UV area and, with textures, a sample
};

// The texture seams of a UV layer and, if `textures` is given, how much the colors of each differ
// across it. Coincident vertices are merged first (in layer.mesh), since a seam must look like one,
// two faces that share its vertices in 3D, and a layer exported with a vertex per corner has them
// apart. Each seam is measured from both of its sides, as TexMetro visits it from both faces: the
// sample points of a side start at the vertex its face lists first, so the two differ a little.
// Seams between islands without UV area have no texture to compare, and are left out.
void findLayerSeams(UvLayer &layer, const SeamTextures *textures,
                    const vcg::tri::SeamDiscrepancy<UVMesh>::Options &options, MeasuredSeams &result);

} // namespace UvLayerUtils
