#include "texturesignalprocessingfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "textureassociationutils.h"
#include "tsp_adapter.h"
#include "uvlayer.h"

#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/parametrization/distortion.h>
#include <vcg/complex/algorithms/parametrization/uv_utils.h>

#include <QImage>
#include <cmath>
#include <stdexcept>

namespace {

constexpr QLatin1StringView kFilterRepairSeams("repair_texture_seams");
constexpr QLatin1StringView kFilterDilate("dilate_texture_across_seams");

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
    int leftOut = 0; // faces of the texture with no UV area
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

    std::vector<int> newIndex(um.vert.size(), -1);
    TspAdapter::Atlas &atlas = prepared.atlas;
    for (const auto &f : um.face) {
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
    const bool repair = filterId == QString::fromLatin1(kFilterRepairSeams);
    if (!repair && filterId != QString::fromLatin1(kFilterDilate))
        return fail(QObject::tr("Unknown filter: %1").arg(filterId));

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

    vcg::CallBackPos *cb = doc.progressCallback();
    if (cb)
        cb(0, repair ? "Repairing texture seams" : "Dilating texture across seams");
    PreparedAtlas prepared;
    error = prepareAtlas(entry, texture, source.size(), repair, prepared);
    if (!error.isEmpty())
        return fail(error);

    TspAdapter::Image image = toAdapterImage(source);
    std::size_t written = 0;
    try {
        written = repair ? TspAdapter::repairSeams(prepared.atlas, image)
                         : TspAdapter::dilate(prepared.atlas, image, unsigned(params.getInt(QStringLiteral("radius"))));
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
    doc.markMeshMaterialChanged(
        meshIndex,
        repair ? QObject::tr("Repaired the seams of texture '%1' of '%2'.").arg(textureName, entry.name)
               : QObject::tr("Dilated texture '%1' of '%2' across its seams.").arg(textureName, entry.name));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages << (repair
        ? QObject::tr("Set %1 texels outside the UV islands of texture '%2'; saving the mesh writes it.")
        : QObject::tr("Filled %1 texels outside the UV islands of texture '%2'; saving the mesh writes it."))
            .arg(written).arg(textureName);
    if (prepared.leftOut > 0)
        result.infoMessages << QObject::tr("Left out %1 face(s) with no UV area.").arg(prepared.leftOut);
    result.outputValues["texels_written"] = qlonglong(written);
    return result;
}

void registerTextureSignalProcessingFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<TextureSignalProcessingFilterPlugin>());
}
