#include "texturesignalprocessingfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "textureassociationutils.h"
#include "tsp_adapter.h"
#include "uvlayer.h"

#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/parametrization/distortion.h>
#include <vcg/complex/algorithms/parametrization/tangent_field_operators.h>
#include <vcg/complex/algorithms/parametrization/uv_utils.h>

#include <QImage>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace {

constexpr QLatin1StringView kFilterRepairSeams("repair_texture_seams");
constexpr QLatin1StringView kFilterDilate("dilate_texture_across_seams");
constexpr QLatin1StringView kFilterSmooth("smooth_texture");
constexpr QLatin1StringView kFilterSharpen("sharpen_texture");
constexpr QLatin1StringView kFilterInpaint("inpaint_texture_on_selected_faces");
constexpr QLatin1StringView kFilterTransferDetail("transfer_texture_detail");
constexpr QLatin1StringView kFilterLic("colorize_texture_by_principal_directions_line_integral_convolution");
constexpr QLatin1StringView kFilterReactionDiffusion("colorize_texture_by_reaction_diffusion");
constexpr QLatin1StringView kFilterCurvature("compute_principal_curvature_directions_per_face_texture_signal_processing");

using UvLayerUtils::UVMesh;

// A UV triangle smaller than this, in square texels, is left out: it covers no texel, and TSP
// cannot process a triangle with no area. Not only exact zeros, since the coordinates TSP sees
// are rescaled and rounded, which can give a collinear triangle a tiny area of either sign.
constexpr double kMinimumAreaTexels = 1e-10;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

struct PreparedAtlas
{
    TspAdapter::Atlas atlas;
    std::vector<char> selected; // per triangle of atlas: whether its face is selected
    std::vector<int> face;      // per triangle of atlas: its face, as an index among the live ones
    int leftOut = 0;            // faces of the texture with no UV area
};

// The faces that use `texture`, as TSP takes them. Coincident vertices are merged first, so that a
// seam is two faces sharing its vertices on the surface, as for the seam measures (findLayerSeams):
// a layer saved with one vertex per corner has its seams apart. With `refuseFolds`, a layer with a
// face folded over its own UV island is refused, which TSP's operators cannot process (a fault in
// _InitializeChart, on a 3M-face scan and on a 32-face test); a whole island laid out mirrored is
// not folded, and works.
QString prepareAtlas(const Document::MeshEntry &entry, int texture, QSize size, bool refuseFolds,
                     PreparedAtlas &prepared)
{
    UvLayerUtils::UvLayer layer;
    const QString error = UvLayerUtils::buildUvLayer(entry, 0, 0, layer, false, false);
    if (!error.isEmpty())
        return error;
    UVMesh &um = layer.mesh;
    vcg::tri::Clean<UVMesh>::RemoveDuplicateVertex(um);
    vcg::tri::Allocator<UVMesh>::CompactEveryVector(um);

    if (refuseFolds) {
        std::vector<int> faceChart;
        vcg::tri::UV_Utils<UVMesh>::PerWedgeCharts(um, faceChart);
        const int folded = vcg::tri::Distortion<UVMesh, true>::FoldedNum(um, faceChart);
        if (folded > 0)
            return QObject::tr("'%1' has %2 face(s) whose UV triangles are folded over their own island, "
                               "which this method cannot process. Select Folded UV Faces shows them.")
                .arg(entry.name).arg(folded);
    }

    // buildUvLayer copies the layer's faces in order, deleted ones left out: the i-th face of
    // the UV mesh is the i-th live face of the layer, whose selection it does not copy.
    std::vector<char> faceSelected;
    for (const VCGFace &face : entry.mesh.face)
        if (!face.IsD())
            faceSelected.push_back(face.IsS());

    std::vector<int> newIndex(um.vert.size(), -1);
    TspAdapter::Atlas &atlas = prepared.atlas;
    for (size_t i = 0; i < um.face.size(); ++i) {
        const auto &f = um.face[i];
        if (f.WT(0).N() != texture)
            continue;
        const vcg::Point2d a = f.cWT(0).P(), b = f.cWT(1).P(), c = f.cWT(2).P();
        const double areaTexels = std::abs((b - a) ^ (c - a)) * 0.5 * size.width() * size.height();
        if (!(areaTexels >= kMinimumAreaTexels)) {
            ++prepared.leftOut;
            continue;
        }
        std::array<unsigned int, 3> triangle;
        for (int k = 0; k < 3; ++k) {
            const size_t v = vcg::tri::Index(um, f.cV(k));
            if (newIndex[v] < 0) {
                newIndex[v] = int(atlas.positions.size());
                const vcg::Point3d &p = f.cV(k)->cP();
                atlas.positions.push_back({ p[0], p[1], p[2] });
            }
            triangle[size_t(k)] = unsigned(newIndex[v]);
            atlas.uv.push_back({ f.cWT(k).P()[0], f.cWT(k).P()[1] });
        }
        atlas.triangles.push_back(triangle);
        prepared.selected.push_back(faceSelected[i]);
        prepared.face.push_back(int(i));
    }
    if (atlas.triangles.empty())
        return QObject::tr("No face of '%1' uses texture %2 with any UV area.").arg(entry.name).arg(texture + 1);
    return {};
}

TspAdapter::Image toAdapterImage(const QImage &source)
{
    const QImage image = source.convertToFormat(QImage::Format_ARGB32);
    TspAdapter::Image out;
    out.width = image.width();
    out.height = image.height();
    out.texels.reserve(size_t(out.width) * size_t(out.height));
    for (int y = 0; y < image.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); ++x)
            out.texels.push_back({ double(qRed(row[x])), double(qGreen(row[x])), double(qBlue(row[x])), double(qAlpha(row[x])) });
    }
    return out;
}

QImage fromAdapterImage(const TspAdapter::Image &image, bool alpha)
{
    QImage out(image.width, image.height, alpha ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    auto channel = [](double v) { return int(std::lround(std::clamp(v, 0.0, 255.0))); };
    for (int y = 0; y < image.height; ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < image.width; ++x) {
            const std::array<double, 4> &t = image.texels[size_t(y) * size_t(image.width) + size_t(x)];
            row[x] = qRgba(channel(t[0]), channel(t[1]), channel(t[2]), alpha ? channel(t[3]) : 255);
        }
    }
    return out;
}

// Compute Principal Curvature Directions per Face (TextureSignalProcessing): TSP's per-triangle
// curvature, stored in the faces' curvature directions. Coincident vertices are merged first, so
// that the normals are continuous across a seam where a mesh repeats its vertices; faces with no
// area are left out, and get no directions.
MeshFilterRunResult computeCurvature(const FilterParams &params, Document &doc)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;

    TspAdapter::Surface surface;
    std::map<std::array<float, 3>, unsigned int> merged;
    std::vector<VCGFace *> faces;
    for (VCGFace &f : mesh.face) {
        if (f.IsD() || vcg::DoubleArea(f) <= 0)
            continue;
        std::array<unsigned int, 3> triangle;
        for (int k = 0; k < 3; ++k) {
            const vcg::Point3f &p = f.cV(k)->cP();
            const auto [it, added] = merged.try_emplace({ p[0], p[1], p[2] }, unsigned(surface.positions.size()));
            if (added)
                surface.positions.push_back({ p[0], p[1], p[2] });
            triangle[size_t(k)] = it->second;
        }
        surface.triangles.push_back(triangle);
        faces.push_back(&f);
    }
    if (faces.empty())
        return fail(QObject::tr("'%1' has no face with any area.").arg(entry.name));

    std::vector<TspAdapter::TriangleCurvature> curvature;
    try {
        curvature = TspAdapter::principalCurvature(surface, unsigned(params.getInt(QStringLiteral("smoothingIterations"))),
                                                   params.getDouble(QStringLiteral("smoothingSize")));
    } catch (const std::exception &e) {
        return fail(QObject::tr("TextureSignalProcessing failed: %1").arg(QString::fromLocal8Bit(e.what())));
    }

    mesh.face.EnableCurvatureDir();
    for (VCGFace &f : mesh.face)
        if (!f.IsD()) {
            f.PD1() = f.PD2() = vcg::Point3f(0, 0, 0);
            f.K1() = f.K2() = 0;
        }
    for (size_t i = 0; i < faces.size(); ++i) {
        const TspAdapter::TriangleCurvature &c = curvature[i];
        faces[i]->PD1() = vcg::Point3f(float(c.maxDirection[0]), float(c.maxDirection[1]), float(c.maxDirection[2]));
        faces[i]->PD2() = vcg::Point3f(float(c.minDirection[0]), float(c.minDirection[1]), float(c.minDirection[2]));
        faces[i]->K1() = float(c.maxCurvature);
        faces[i]->K2() = float(c.minCurvature);
    }
    doc.markMeshGeometryChanged(meshIndex, QObject::tr("Computed curvature directions per face on '%1'").arg(entry.name));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages << QObject::tr("Computed the principal curvature directions of %1 faces of '%2'.").arg(faces.size()).arg(entry.name);
    const int leftOut = mesh.FN() - int(faces.size());
    if (leftOut > 0)
        result.infoMessages << QObject::tr("Left out %1 face(s) with no area, which have no directions.").arg(leftOut);
    result.outputValues["faces"] = qlonglong(faces.size());
    return result;
}

// The two filters that fill new textures from no texture: Colorize Texture by Principal Directions
// (Line Integral Convolution) and Colorize Texture by Reaction-Diffusion. One new texture for each
// texture slot the faces use, appended after the layer's own, which stay; the faces are switched to
// it. The field, which LIC needs and reaction-diffusion may follow, is each face's principal
// direction scaled by k1 - k2, as LineIntegralConvolution scales its own, from the faces' directions
// when they have them, otherwise interpolated from the vertices' as Transfer Curvature Directions
// from Vertex to Face does, without storing them.
MeshFilterRunResult colorizeNewTextures(const FilterParams &params, Document &doc, bool reaction)
{
    namespace Tex = TextureAssociationUtils;
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    Document::MeshEntry &entry = doc.mesh(meshIndex);
    VCGMesh &mesh = entry.mesh;
    const QString direction = params.getEnum(QStringLiteral("direction"));
    const bool useField = !reaction || direction != QStringLiteral("none");
    const bool maximal = direction != QStringLiteral("minimal");
    const bool fromFaces = mesh.face.IsCurvatureDirEnabled();
    if (useField && !fromFaces && !mesh.vert.IsCurvatureDirEnabled())
        return fail(QObject::tr("'%1' has no principal curvature directions: compute them first, with one of the "
                                "Compute Principal Curvature Directions filters.").arg(entry.name));

    std::vector<VCGFace *> live;
    for (VCGFace &f : mesh.face)
        if (!f.IsD())
            live.push_back(&f);
    const int side = params.getInt(QStringLiteral("textureSize"));
    const QSize size(side, side);
    const unsigned int seed = params.getRandomSeed().value;
    vcg::CallBackPos *cb = doc.progressCallback();
    const char *message = reaction ? "Colorizing texture by reaction-diffusion" : "Colorizing texture by principal directions";

    Tex::ensureTextureSlotIndices(mesh);
    const int slotCount = std::max(1, Tex::compactTextureSlotIndices(mesh));
    std::vector<QImage> images;
    long long texels = 0;
    int leftOut = 0;
    unsigned int vCycles = 0;
    bool converged = true;
    for (int slot = 0; slot < slotCount; ++slot) {
        QImage black(size, QImage::Format_ARGB32);
        black.fill(qRgba(0, 0, 0, 255));
        PreparedAtlas prepared;
        const QString error = prepareAtlas(entry, slot, size, true, prepared);
        if (!error.isEmpty())
            return fail(error);
        leftOut += prepared.leftOut;

        std::vector<std::array<double, 3>> field;
        if (useField) {
            field.reserve(prepared.face.size());
            for (int i : prepared.face) {
                VCGFace &f = *live[size_t(i)];
                vcg::Point3f d1, d2;
                float k1 = 0, k2 = 0;
                if (fromFaces) {
                    d1 = f.cPD1(), d2 = f.cPD2(), k1 = f.cK1(), k2 = f.cK2();
                } else {
                    using Cross = vcg::tri::CrossField<VCGMesh>;
                    d1 = Cross::InterpolateCrossField(f.cV(0)->cPD1(), f.cV(1)->cPD1(), f.cV(2)->cPD1(), f.cV(0)->cN(),
                                                      f.cV(1)->cN(), f.cV(2)->cN(), f.cN(), vcg::Point3f(1.f / 3, 1.f / 3, 1.f / 3));
                    d1.Normalize();
                    d2 = (f.cN() ^ d1).Normalize();
                    std::vector<VCGVertex *> verts = { f.V(0), f.V(1), f.V(2) };
                    Cross::CrossMagnitudes(verts, f.cN(), d1, d2, k1, k2);
                }
                // The maximal direction is the one of the larger curvature, whichever is first.
                vcg::Point3f d = ((k1 >= k2) == maximal) ? d1 : d2;
                d = d.Normalize() * std::abs(k1 - k2);
                field.push_back({ d[0], d[1], d[2] });
            }
        }

        TspAdapter::Image image = toAdapterImage(black);
        TspAdapter::FilterOutcome outcome;
        try {
            if (reaction) {
                const unsigned int steps = unsigned(params.getInt(QStringLiteral("steps")));
                const auto progress = [cb, slot, slotCount, steps, message](unsigned int step) {
                    return !cb || cb(int((100 * slot + 10 + 90 * step / std::max(1u, steps)) / slotCount), message);
                };
                outcome = TspAdapter::reactionDiffusion(
                    prepared.atlas, image, useField ? &field : nullptr, params.getDouble(QStringLiteral("anisotropyScale")),
                    params.getDouble(QStringLiteral("anisotropyExponent")),
                    params.getEnum(QStringLiteral("pattern")) == QStringLiteral("dots") ? TspAdapter::Pattern::Dots
                                                                                       : TspAdapter::Pattern::Stripes,
                    steps, params.getDouble(QStringLiteral("timeStep")), params.getDouble(QStringLiteral("diffusionScale")),
                    seed + unsigned(slot), progress);
            } else {
                const QString noiseId = params.getEnum(QStringLiteral("noise"));
                const TspAdapter::Noise noise = noiseId == QStringLiteral("black_and_white") ? TspAdapter::Noise::BlackAndWhite
                                                : noiseId == QStringLiteral("gray")          ? TspAdapter::Noise::Gray
                                                                                             : TspAdapter::Noise::Hue;
                const auto progress = [cb, slot, slotCount, message](unsigned int cycles) {
                    return !cb || cb(int((100 * slot + std::min(95u, 40 + cycles)) / slotCount), message);
                };
                outcome = TspAdapter::lineIntegralConvolution(
                    prepared.atlas, image, field, params.getDouble(QStringLiteral("streakLength")),
                    params.getDouble(QStringLiteral("sharpenDetailSize")), params.getDouble(QStringLiteral("sharpenScale")),
                    params.getDouble(QStringLiteral("anisotropyExponent")), noise, seed + unsigned(slot), progress);
            }
        } catch (const std::exception &e) {
            return fail(QObject::tr("TextureSignalProcessing failed: %1").arg(QString::fromLocal8Bit(e.what())));
        }
        if (outcome.canceled)
            return fail(QObject::tr("Operation canceled."));
        texels += outcome.texels;
        vCycles = std::max(vCycles, outcome.vCycles);
        converged = converged && outcome.converged;
        images.push_back(fromAdapterImage(image, true));
    }
    if (cb)
        cb(100, "Done");

    const std::vector<MeshIOTextureAsset> assets = Tex::makeTextureAssetsFromImages(
        Tex::outputTextureNames(entry, {}, reaction ? QStringLiteral("reaction_diffusion") : QStringLiteral("lic"), slotCount), images);
    const int previous = Document::meshTextureAssociationCount(entry);
    const QString what = reaction
        ? QObject::tr("Colorized %1 new texture(s) of '%2' by reaction-diffusion").arg(slotCount).arg(entry.name)
        : QObject::tr("Colorized %1 new texture(s) of '%2' by principal directions").arg(slotCount).arg(entry.name);
    if (previous > 0) {
        Tex::offsetTextureSlotIndices(mesh, previous);
        Tex::appendTextureAssociations(entry, assets);
        ++entry.materialRevision;
        doc.markMeshGeometryChanged(meshIndex, what);
    } else {
        Tex::replaceTextureAssociations(entry, assets);
        doc.markMeshMaterialChanged(meshIndex, what);
    }

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    if (reaction) {
        result.infoMessages << QObject::tr("Grew %1 over %2 steps on '%3'%4, into %5 texels of %6 new texture(s), which its "
                                           "faces now show; saving the mesh writes them.")
                                   .arg(params.getEnum(QStringLiteral("pattern")) == QStringLiteral("dots") ? QObject::tr("dots")
                                                                                                            : QObject::tr("stripes"))
                                   .arg(vCycles)
                                   .arg(entry.name,
                                        useField ? QObject::tr(", along its %1 principal directions from its %2")
                                                       .arg(maximal ? QObject::tr("maximal") : QObject::tr("minimal"),
                                                            fromFaces ? QObject::tr("faces") : QObject::tr("vertices"))
                                                 : QString())
                                   .arg(texels).arg(slotCount);
    } else {
        result.infoMessages << QObject::tr("Drew the %1 principal directions of '%2', from its %3, into %4 texels of %5 new "
                                           "texture(s), which its faces now show; saving the mesh writes them.")
                                   .arg(maximal ? QObject::tr("maximal") : QObject::tr("minimal"), entry.name,
                                        fromFaces ? QObject::tr("faces") : QObject::tr("vertices"))
                                   .arg(texels).arg(slotCount);
        result.infoMessages << (converged
            ? QObject::tr("The sharpening converged in %1 V-cycles.").arg(vCycles)
            : QObject::tr("The sharpening stopped after %1 V-cycles without converging.").arg(vCycles));
    }
    if (leftOut > 0)
        result.infoMessages << QObject::tr("Left out %1 face(s) with no UV area.").arg(leftOut);
    result.visualizationHints.push_back({ meshIndex, MeshFilterVisualizationAttribute::Texture });
    result.outputValues["texels_written"] = qlonglong(texels);
    result.outputValues["texture"] = previous + 1;
    return result;
}

} // namespace

QString TextureSignalProcessingFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.texture_signal_processing");
}

QString TextureSignalProcessingFilterPlugin::name() const
{
    return QObject::tr("Texture Signal Processing Filters");
}

MeshFilterRunResult TextureSignalProcessingFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kFilterCurvature))
        return computeCurvature(params, doc);
    if (filterId == QString::fromLatin1(kFilterLic))
        return colorizeNewTextures(params, doc, false);
    if (filterId == QString::fromLatin1(kFilterReactionDiffusion))
        return colorizeNewTextures(params, doc, true);

    enum class Kind { Repair, Dilate, Smooth, Sharpen, Inpaint, TransferDetail };
    Kind kind;
    if (filterId == QString::fromLatin1(kFilterRepairSeams))
        kind = Kind::Repair;
    else if (filterId == QString::fromLatin1(kFilterDilate))
        kind = Kind::Dilate;
    else if (filterId == QString::fromLatin1(kFilterSmooth))
        kind = Kind::Smooth;
    else if (filterId == QString::fromLatin1(kFilterSharpen))
        kind = Kind::Sharpen;
    else if (filterId == QString::fromLatin1(kFilterInpaint))
        kind = Kind::Inpaint;
    else if (filterId == QString::fromLatin1(kFilterTransferDetail))
        kind = Kind::TransferDetail;
    else
        return fail(QObject::tr("Unknown filter: %1").arg(filterId));
    const bool filtering = kind == Kind::Smooth || kind == Kind::Sharpen || kind == Kind::TransferDetail;
    const bool normals = params.getBool(QStringLiteral("objectSpaceNormals"), false);

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh selected."));
    Document::MeshEntry &entry = doc.mesh(meshIndex);

    const int texture = params.getTextureRef(QStringLiteral("texture"), 1) - 1;
    if (texture < 0 || texture >= Document::meshTextureAssociationCount(entry))
        return fail(QObject::tr("'%1' has no texture %2.").arg(entry.name).arg(texture + 1));
    QImage source;
    QString error;
    if (!TextureAssociationUtils::loadAssociatedTextureImage(entry, texture, source, error))
        return fail(error);

    // The texture whose gradients Transfer Texture Detail keeps, read through the UVs of `texture`,
    // and resampled to its size if it has another: TSP takes the two as one grid of texels.
    QImage detail;
    int detailTexture = -1;
    if (kind == Kind::TransferDetail) {
        detailTexture = params.getTextureRef(QStringLiteral("detailTexture"), 1) - 1;
        if (detailTexture < 0 || detailTexture >= Document::meshTextureAssociationCount(entry))
            return fail(QObject::tr("'%1' has no texture %2.").arg(entry.name).arg(detailTexture + 1));
        if (!TextureAssociationUtils::loadAssociatedTextureImage(entry, detailTexture, detail, error))
            return fail(error);
        if (detail.size() != source.size())
            detail = detail.scaled(source.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    static const char *const kProgress[] = { "Repairing texture seams", "Dilating texture across seams",
                                             "Smoothing texture", "Sharpening texture", "Inpainting texture",
                                             "Transferring texture detail" };
    vcg::CallBackPos *cb = doc.progressCallback();
    if (cb)
        cb(0, kProgress[int(kind)]);
    PreparedAtlas prepared;
    // Every filter but the dilation builds TSP's operators, which cannot take a folded face.
    error = prepareAtlas(entry, texture, source.size(), kind != Kind::Dilate, prepared);
    if (!error.isEmpty())
        return fail(error);
    if (kind == Kind::Inpaint && std::find(prepared.selected.begin(), prepared.selected.end(), 1) == prepared.selected.end())
        return fail(QObject::tr("No selected face of '%1' uses texture %2: select the faces whose texture to inpaint.")
                        .arg(entry.name).arg(texture + 1));

    // The image solved for, which receives the result. Transfer Texture Detail takes its gradients
    // from the detail texture instead.
    TspAdapter::Image image = toAdapterImage(source);
    const TspAdapter::Image detailImage = kind == Kind::TransferDetail ? toAdapterImage(detail) : TspAdapter::Image();
    TspAdapter::FilterOutcome outcome;
    try {
        switch (kind) {
        case Kind::Repair:
            outcome.texels = TspAdapter::repairSeams(prepared.atlas, image);
            break;
        case Kind::Dilate:
            outcome.texels = TspAdapter::dilate(prepared.atlas, image, unsigned(params.getInt(QStringLiteral("radius"))));
            break;
        case Kind::Inpaint:
            outcome.texels = TspAdapter::inpaint(prepared.atlas, image, prepared.selected, normals);
            break;
        case Kind::Smooth:
        case Kind::Sharpen:
        case Kind::TransferDetail: {
            const bool selectedOnly = params.getBool(QStringLiteral("selectedOnly"), false);
            // The operators are built in one call that cannot be interrupted; the V-cycles that follow
            // can, and the bar moves with them, though how many are needed is not known in advance.
            const char *message = kProgress[int(kind)];
            outcome = TspAdapter::filter(prepared.atlas, image, params.getDouble(QStringLiteral("detailSize")),
                                         params.getDouble(QStringLiteral("gradientScale")),
                                         selectedOnly ? &prepared.selected : nullptr,
                                         kind == Kind::TransferDetail ? &detailImage : nullptr, normals,
                                         [cb, message](unsigned int cycles) {
                                             return !cb || cb(int(std::min(95u, 20 + cycles / 2)), message);
                                         });
            if (outcome.canceled)
                return fail(QObject::tr("Operation canceled."));
            break;
        }
        }
    } catch (const std::exception &e) {
        return fail(QObject::tr("TextureSignalProcessing failed: %1").arg(QString::fromLocal8Bit(e.what())));
    }
    if (cb)
        cb(100, "Done");

    // The image is replaced in the texture's asset, which keeps its name and file path.
    MeshIOTextureAsset *asset = TextureAssociationUtils::writableTextureAsset(entry, texture);
    if (!asset)
        return fail(QObject::tr("Texture %1 of '%2' has no image entry to write the result to.")
                        .arg(texture + 1).arg(entry.name));
    asset->image = fromAdapterImage(image, source.hasAlphaChannel());
    TextureAssociationUtils::rebuildLegacyTextureAssociation(entry);
    const QString textureName = Document::meshTextureDisplayName(entry, texture);
    static const char *const kDone[] = { QT_TR_NOOP("Repaired the seams of texture '%1' of '%2'."),
                                         QT_TR_NOOP("Dilated texture '%1' of '%2' across its seams."),
                                         QT_TR_NOOP("Smoothed texture '%1' of '%2'."),
                                         QT_TR_NOOP("Sharpened texture '%1' of '%2'."),
                                         QT_TR_NOOP("Inpainted texture '%1' of '%2' on the selected faces."),
                                         QT_TR_NOOP("Transferred detail to texture '%1' of '%2'.") };
    doc.markMeshMaterialChanged(meshIndex, QObject::tr(kDone[int(kind)]).arg(textureName, entry.name));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    static const char *const kWritten[] = {
        QT_TR_NOOP("Set %1 texels outside the UV islands of texture '%2'; saving the mesh writes it."),
        QT_TR_NOOP("Filled %1 texels outside the UV islands of texture '%2'; saving the mesh writes it."),
        QT_TR_NOOP("Smoothed %1 texels of texture '%2'; saving the mesh writes it."),
        QT_TR_NOOP("Sharpened %1 texels of texture '%2'; saving the mesh writes it."),
        QT_TR_NOOP("Inpainted %1 texels of texture '%2' on the selected faces; saving the mesh writes it."),
        QT_TR_NOOP("Wrote %1 texels of texture '%2', with the detail of texture '%3'; saving the mesh writes it.") };
    QString written = QObject::tr(kWritten[int(kind)]).arg(outcome.texels).arg(textureName);
    if (kind == Kind::TransferDetail)
        written = written.arg(Document::meshTextureDisplayName(entry, detailTexture));
    result.infoMessages << written;
    if (filtering) {
        result.infoMessages << (outcome.converged
            ? QObject::tr("The solver converged in %1 V-cycles.").arg(outcome.vCycles)
            : QObject::tr("The solver stopped after %1 V-cycles without converging; the result may "
                          "still be a little away from the solution.").arg(outcome.vCycles));
        result.outputValues["v_cycles"] = int(outcome.vCycles);
    }
    if (prepared.leftOut > 0)
        result.infoMessages << QObject::tr("Left out %1 face(s) with no UV area.").arg(prepared.leftOut);
    result.outputValues["texels_written"] = qlonglong(outcome.texels);
    return result;
}

void registerTextureSignalProcessingFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<TextureSignalProcessingFilterPlugin>());
}
