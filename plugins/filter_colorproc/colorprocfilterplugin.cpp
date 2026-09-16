#include "colorprocfilterplugin.h"

#include "document.h"
#include "filterparam.h"
#include "meshfilterpluginmanager.h"
#include "src/render/colormap.h"
#include "src/render/qualityrange.h"
#include "textureassociationutils.h"
#include "vcgmesh.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/bitquad_support.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/parametrization/distortion.h>
#include <vcg/complex/algorithms/polygon_support.h>
#include <vcg/complex/algorithms/smooth.h>
#include <vcg/complex/algorithms/stat.h>
#include <vcg/complex/algorithms/update/color.h>
#include <vcg/complex/algorithms/update/curvature.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/quality.h>
#include <vcg/complex/algorithms/update/selection.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/math/histogram.h>
#include <vcg/math/random_generator.h>
#include <vcg/space/color4.h>
#include <vcg/space/colorspace.h>
#include <vcg/space/fitting3.h>
#include <vcg/space/intersection3.h>
#include <vcg/space/plane3.h>
#include <vcg/space/triangle3.h>
#include <QImage>
#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace {
constexpr QLatin1StringView kFilterFilling("set_vertex_color");
constexpr QLatin1StringView kFilterThresholding("threshold_vertex_color");
constexpr QLatin1StringView kFilterContrastBright("adjust_vertex_color_brightness_contrast_gamma");
constexpr QLatin1StringView kFilterInvert("invert_vertex_color");
constexpr QLatin1StringView kFilterLevels("adjust_vertex_color_levels");
constexpr QLatin1StringView kFilterColourisation("tint_vertex_color");
constexpr QLatin1StringView kFilterDesaturation("desaturate_vertex_color");
constexpr QLatin1StringView kFilterEqualize("equalize_vertex_color");
constexpr QLatin1StringView kFilterWhiteBalance("adjust_vertex_color_white_balance");
constexpr QLatin1StringView kFilterPerlinColor("colorize_vertices_by_perlin_noise");
constexpr QLatin1StringView kFilterColorNoise("add_noise_to_vertex_color");
constexpr QLatin1StringView kFilterScatterPerMesh("set_random_layer_color");
constexpr QLatin1StringView kFilterSetMeshColor("set_mesh_color");
constexpr QLatin1StringView kFilterClampQuality("clamp_vertex_scalar");
constexpr QLatin1StringView kFilterSaturateQuality("clamp_vertex_scalar_gradient");
constexpr QLatin1StringView kFilterMapVQuality("colorize_vertices_by_scalar");
constexpr QLatin1StringView kFilterMapFQuality("colorize_faces_by_scalar");
constexpr QLatin1StringView kFilterMapEQuality("colorize_edges_by_scalar");
constexpr QLatin1StringView kFilterDiscreteCurvature("compute_curvature_discrete");
constexpr QLatin1StringView kFilterGeometricQuality("compute_face_scalar_from_geometry");
constexpr QLatin1StringView kFilterTextureDistortion("compute_uv_distortion");
constexpr QLatin1StringView kFilterVertexSmooth("smooth_vertex_color");
constexpr QLatin1StringView kFilterFaceSmooth("smooth_face_color");
constexpr QLatin1StringView kFilterVertexToFace("transfer_color_from_vertex_to_face");
constexpr QLatin1StringView kFilterMeshToFace("transfer_color_from_mesh_to_face");
constexpr QLatin1StringView kFilterFaceToVertex("transfer_color_from_face_to_vertex");
constexpr QLatin1StringView kFilterTextureToVertex("transfer_color_from_texture_to_vertex");
constexpr QLatin1StringView kFilterRandomFace("set_random_face_color");
constexpr QLatin1StringView kFilterRandomConnected("set_random_component_color");
constexpr QLatin1StringView kFilterVertexToFaceQuality("transfer_scalar_from_vertex_to_face");
constexpr QLatin1StringView kFilterFaceToVertexQuality("transfer_scalar_from_face_to_vertex");

using Mask = vcg::tri::io::Mask;
namespace Tex = TextureAssociationUtils;
using Scalar = VCGMesh::ScalarType;
using Point = vcg::Point3f;
using Histogramf = vcg::Histogram<float>;

enum class TextureDistortionMetric {
    Angle,
    Area,
    Edge,
    L2Stretch,
    LInfStretch
};

TextureDistortionMetric textureDistortionMetric(const QString &id)
{
    if (id == QStringLiteral("area")) return TextureDistortionMetric::Area;
    if (id == QStringLiteral("edge")) return TextureDistortionMetric::Edge;
    if (id == QStringLiteral("l2_stretch")) return TextureDistortionMetric::L2Stretch;
    if (id == QStringLiteral("linf_stretch")) return TextureDistortionMetric::LInfStretch;
    return TextureDistortionMetric::Angle;
}

template<bool PerWedge>
Scalar faceTextureDistortion(
    const VCGFace &face,
    TextureDistortionMetric metric,
    Scalar areaScale,
    Scalar edgeScale)
{
    using Distortion = vcg::tri::Distortion<VCGMesh, PerWedge>;
    switch (metric) {
    case TextureDistortionMetric::Area:
        return Distortion::AreaDistortion(&face, areaScale);
    case TextureDistortionMetric::Edge:
        return (
            Distortion::EdgeDistortion(&face, 0, edgeScale)
            + Distortion::EdgeDistortion(&face, 1, edgeScale)
            + Distortion::EdgeDistortion(&face, 2, edgeScale)) / 3;
    case TextureDistortionMetric::L2Stretch:
        return std::sqrt(Distortion::L2StretchEnergySquared(&face, areaScale));
    case TextureDistortionMetric::LInfStretch:
        return Distortion::LInfStretchEnergy(&face, areaScale);
    case TextureDistortionMetric::Angle:
        return Distortion::AngleDistortion(&face);
    }
    return 0;
}

struct CurrentMeshRef {
    int index = -1;
    Document::MeshEntry *entry = nullptr;
};

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = message;
    return result;
}

MeshFilterRunResult success(const QStringList &info = {})
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages = info;
    return result;
}

MeshFilterRunResult qualitySuccess(
    int meshIndex,
    MeshFilterVisualizationAttribute attribute,
    const QStringList &info = {})
{
    MeshFilterRunResult result = success(info);
    result.visualizationHints.push_back({ meshIndex, attribute });
    return result;
}

std::optional<CurrentMeshRef> currentMesh(Document &doc, QString &errorMessage)
{
    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount()) {
        errorMessage = QObject::tr("No current mesh selected.");
        return std::nullopt;
    }
    return CurrentMeshRef{ meshIndex, &doc.mesh(meshIndex) };
}

QString meshLabel(const Document::MeshEntry &entry, int meshIndex)
{
    const QString trimmed = entry.name.trimmed();
    if (!trimmed.isEmpty())
        return trimmed;
    return QObject::tr("Mesh %1").arg(meshIndex + 1);
}

vcg::ColorMap colorMapFromId(const QString &id)
{
    if (id == QStringLiteral("viridis")) return vcg::ColorMap::Viridis;
    if (id == QStringLiteral("plasma"))  return vcg::ColorMap::Plasma;
    if (id == QStringLiteral("cividis")) return vcg::ColorMap::Cividis;
    if (id == QStringLiteral("turbo"))   return vcg::ColorMap::Turbo;
    if (id == QStringLiteral("rdpu"))    return vcg::ColorMap::RdPu;
    return vcg::ColorMap::RGB;
}

bool isLegacyVcgColorMap(const QString &id)
{
    return id == QStringLiteral("rgb") || id == QStringLiteral("rdpu");
}

QString resolvedColorMapId(const FilterParams &params)
{
    const QString overrideId =
        params.getString(QStringLiteral("colorMapId"), QString()).trimmed().toLower();
    if (!overrideId.isEmpty())
        return overrideId;
    return params.getEnum(QStringLiteral("colorMap"), QStringLiteral("rainbow")).trimmed().toLower();
}

unsigned char rgbMaskFromParams(const FilterParams &params)
{
    unsigned char mask = vcg::tri::UpdateColor<VCGMesh>::NO_CHANNELS;
    if (params.getBool(QStringLiteral("rCh"), false))
        mask = mask | vcg::tri::UpdateColor<VCGMesh>::RED_CHANNEL;
    if (params.getBool(QStringLiteral("gCh"), false))
        mask = mask | vcg::tri::UpdateColor<VCGMesh>::GREEN_CHANNEL;
    if (params.getBool(QStringLiteral("bCh"), false))
        mask = mask | vcg::tri::UpdateColor<VCGMesh>::BLUE_CHANNEL;
    if (mask == vcg::tri::UpdateColor<VCGMesh>::NO_CHANNELS)
        mask = vcg::tri::UpdateColor<VCGMesh>::ALL_CHANNELS;
    return mask;
}

QColor colorParam(const FilterParams &params, const QString &id)
{
    return params.getColor(id, QColor(Qt::white));
}

vcg::Color4b toColor4b(const QColor &color)
{
    return vcg::Color4b(color.red(), color.green(), color.blue(), color.alpha());
}

void markGeometry(Document &doc, int meshIndex, const QString &message)
{
    if (meshIndex >= 0 && meshIndex < doc.meshCount())
        doc.markMeshGeometryChanged(meshIndex, message);
}

void ensureVertexColor(Document::MeshEntry &entry)
{
    entry.ioMask |= Mask::IOM_VERTCOLOR;
}

void ensureFaceColor(Document::MeshEntry &entry)
{
    entry.ioMask |= Mask::IOM_FACECOLOR;
}

void ensureEdgeColor(Document::MeshEntry &entry)
{
    entry.ioMask |= Mask::IOM_EDGECOLOR;
}

void ensureVertexQuality(Document::MeshEntry &entry)
{
    entry.ioMask |= Mask::IOM_VERTQUALITY;
}

void ensureFaceQuality(Document::MeshEntry &entry)
{
    entry.ioMask |= Mask::IOM_FACEQUALITY;
}

std::vector<float> finiteVertexQualityValues(const VCGMesh &mesh)
{
    std::vector<float> values;
    values.reserve(size_t(mesh.VN()));
    for (int vi = 0; vi < mesh.VN(); ++vi) {
        const auto &v = mesh.vert[vi];
        if (v.IsD())
            continue;
        const float q = static_cast<float>(v.cQ());
        if (std::isfinite(q))
            values.push_back(q);
    }
    return values;
}

void applyMeshLab2VertexQualityColorMap(
    VCGMesh &mesh,
    const RenderQualityRange &range,
    const QString &colorMapId,
    bool invert)
{
    const ColorMapRegistry &registry = ColorMapRegistry::instance();
    const bool constantWhite = colorMapId == QStringLiteral("constant");
    const QString mapId = (!constantWhite && registry.hasMap(colorMapId))
        ? colorMapId
        : registry.fallbackMapId();
    const ColorMapDefinition *definition = constantWhite ? nullptr : registry.definition(mapId);

    for (int vi = 0; vi < mesh.VN(); ++vi) {
        auto &v = mesh.vert[vi];
        if (v.IsD())
            continue;
        QColor color(Qt::white);
        if (!constantWhite) {
            float t = normalizedRenderQuality(static_cast<float>(v.cQ()), range);
            if (invert)
                t = 1.0f - t;
            color = registry.sampleQColor(definition, t, 1.0f);
        }
        v.C() = vcg::Color4b(color.red(), color.green(), color.blue(), color.alpha());
    }
}

QRgb sampleWrappedTexture(const QImage &image, const vcg::Point2f &uv)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return qRgb(255, 255, 255);

    const float wrappedU = uv.X() - std::floor(uv.X());
    const float wrappedV = uv.Y() - std::floor(uv.Y());
    const int x = std::clamp(int(wrappedU * float(image.width())), 0, image.width() - 1);
    const int y = std::clamp(int((1.0f - wrappedV) * float(image.height()) - 1.0f), 0, image.height() - 1);
    return image.pixel(x, y);
}

} // namespace

QString ColorProcFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.colorproc");
}

QString ColorProcFilterPlugin::name() const
{
    return QObject::tr("Color Filters");
}

MeshFilterRunResult ColorProcFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    vcg::CallBackPos *cb = doc.progressCallback();

    if (filterId == QString::fromLatin1(kFilterSetMeshColor)) {
        int mi = doc.currentMeshIndex();
        if (mi < 0) return fail(QObject::tr("No current mesh."));
        Document::MeshEntry &entry = doc.mesh(mi);
        QColor c = params.getColor(QStringLiteral("color"), QColor(128, 128, 128));
        entry.mesh.C() = vcg::Color4b(c.red(), c.green(), c.blue(), c.alpha());
        doc.writeLog(QObject::tr("Set per-mesh color of '%1' to (%2,%3,%4,%5)")
            .arg(entry.name).arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()),
            Document::LogSource::Application);
        return success({QObject::tr("Set mesh color for '%1'.").arg(entry.name)});
    }

    if (filterId == QString::fromLatin1(kFilterScatterPerMesh)) {
        std::vector<int> visibleMeshes;
        visibleMeshes.reserve(size_t(doc.meshCount()));
        for (int i = 0; i < doc.meshCount(); ++i) {
            if (doc.mesh(i).visible)
                visibleMeshes.push_back(i);
        }
        if (visibleMeshes.empty())
            return fail(QObject::tr("No visible mesh layers available."));

        const RandomSeed seed = params.getRandomSeed();
        vcg::math::MarsenneTwisterRNG rng{ seed.value };
        int colorIndex = rng.generate(int(visibleMeshes.size()));
        for (int meshIndex : visibleMeshes) {
            Document::MeshEntry &entry = doc.mesh(meshIndex);
            entry.mesh.C() = vcg::Color4b::Scatter(int(visibleMeshes.size()), colorIndex);
            markGeometry(doc, meshIndex, QObject::tr("Assigned scattered mesh colors"));
            ++colorIndex;
            colorIndex %= int(visibleMeshes.size());
        }
        MeshFilterRunResult result = success({
            QObject::tr("Assigned scattered colors to %1 visible mesh layers.").arg(visibleMeshes.size()),
            seed.message()
        });
        // Writing the colour is only half the job: a layer already on screen keeps whatever
        // colour source it had, so the scatter would be invisible without asking for it.
        for (int meshIndex : visibleMeshes) {
            result.visualizationHints.push_back(
                { meshIndex, MeshFilterVisualizationAttribute::PerMeshColor });
        }
        return result;
    }

    QString errorMessage;
    auto current = currentMesh(doc, errorMessage);
    if (!current)
        return fail(errorMessage);

    const int meshIndex = current->index;
    Document::MeshEntry &entry = *current->entry;
    VCGMesh &mesh = entry.mesh;

    if (filterId == QString::fromLatin1(kFilterFilling)) {
        vcg::tri::UpdateColor<VCGMesh>::PerVertexConstant(
            mesh,
            toColor4b(colorParam(params, QStringLiteral("color1"))),
            params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Filled vertex colors of '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterThresholding)) {
        const Scalar threshold = vcg::math::Clamp<Scalar>(Scalar(params.getDouble(QStringLiteral("threshold"), 128.0)), Scalar(0), Scalar(255));
        vcg::tri::UpdateColor<VCGMesh>::PerVertexThresholding(
            mesh,
            threshold,
            toColor4b(colorParam(params, QStringLiteral("color1"))),
            toColor4b(colorParam(params, QStringLiteral("color2"))),
            params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Applied vertex color thresholding to '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterContrastBright)) {
        const Scalar gamma = vcg::math::Clamp<Scalar>(Scalar(params.getDouble(QStringLiteral("gamma"), 1.0)), Scalar(0.1), Scalar(5.0));
        const Scalar brightness = Scalar(params.getDouble(QStringLiteral("brightness"), 0.0));
        const Scalar contrast = Scalar(params.getDouble(QStringLiteral("contrast"), 0.0));
        const bool selected = params.getBool(QStringLiteral("onSelected"), false);
        vcg::tri::UpdateColor<VCGMesh>::PerVertexGamma(mesh, gamma, selected);
        vcg::tri::UpdateColor<VCGMesh>::PerVertexBrightnessContrast(mesh, brightness / 256.0f, contrast / 256.0f, selected);
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Adjusted vertex color brightness/contrast/gamma on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterInvert)) {
        vcg::tri::UpdateColor<VCGMesh>::PerVertexInvert(mesh, params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Inverted vertex colors of '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterLevels)) {
        vcg::tri::UpdateColor<VCGMesh>::PerVertexLevels(
            mesh,
            Scalar(params.getDouble(QStringLiteral("gamma"), 1.0)),
            Scalar(params.getDouble(QStringLiteral("in_min"), 0.0) / 255.0),
            Scalar(params.getDouble(QStringLiteral("in_max"), 255.0) / 255.0),
            Scalar(params.getDouble(QStringLiteral("out_min"), 0.0) / 255.0),
            Scalar(params.getDouble(QStringLiteral("out_max"), 255.0) / 255.0),
            rgbMaskFromParams(params),
            params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Adjusted vertex color levels on '%1'").arg(meshLabel(entry, meshIndex)));
        return success({ QObject::tr("Adjusted color levels on current mesh.") });
    }

    if (filterId == QString::fromLatin1(kFilterColourisation)) {
        const Scalar hue = vcg::math::Clamp<Scalar>(Scalar(params.getDouble(QStringLiteral("hue"), 0.0) / 360.0), Scalar(0), Scalar(1));
        const Scalar saturation = vcg::math::Clamp<Scalar>(Scalar(params.getDouble(QStringLiteral("saturation"), 100.0) / 100.0), Scalar(0), Scalar(1));
        const Scalar luminance = vcg::math::Clamp<Scalar>(Scalar(params.getDouble(QStringLiteral("luminance"), 50.0) / 100.0), Scalar(0), Scalar(1));
        const Scalar intensity = vcg::math::Clamp<Scalar>(Scalar(params.getDouble(QStringLiteral("intensity"), 50.0) / 100.0), Scalar(0), Scalar(1));
        double r = 0.0, g = 0.0, b = 0.0;
        vcg::ColorSpace<unsigned char>::HSLtoRGB(double(hue), double(saturation), double(luminance), r, g, b);
        vcg::tri::UpdateColor<VCGMesh>::PerVertexColourisation(
            mesh,
            vcg::Color4b(int(r * 255.0), int(g * 255.0), int(b * 255.0), 255),
            intensity,
            params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Colourised vertex colors of '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterDesaturation)) {
        int method = 0;
        const QString methodId = params.getEnum(QStringLiteral("method"), QStringLiteral("lightness"));
        if (methodId == QStringLiteral("luminosity")) method = 1;
        else if (methodId == QStringLiteral("average")) method = 2;
        vcg::tri::UpdateColor<VCGMesh>::PerVertexDesaturation(mesh, method, params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Desaturated vertex colors of '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterEqualize)) {
        vcg::tri::UpdateColor<VCGMesh>::PerVertexEqualize(mesh, rgbMaskFromParams(params), params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Equalized vertex colors of '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterWhiteBalance)) {
        vcg::tri::UpdateColor<VCGMesh>::PerVertexWhiteBalance(
            mesh,
            toColor4b(colorParam(params, QStringLiteral("color"))),
            params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Applied white balance to '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterPerlinColor)) {
        const Scalar freq = Scalar(params.getDouble(QStringLiteral("freq"), 10.0));
        const Scalar period = Scalar(std::max(1e-9, double(doc.mesh(meshIndex).mesh.bbox.Diag())) / std::max(1e-6, double(freq)));
        const QVector3D offsetVec = params.getPoint3f(QStringLiteral("offset"), QVector3D(0, 0, 0));
        vcg::tri::UpdateColor<VCGMesh>::PerVertexPerlinColoring(
            mesh,
            period,
            Point(offsetVec.x(), offsetVec.y(), offsetVec.z()),
            toColor4b(colorParam(params, QStringLiteral("color1"))),
            toColor4b(colorParam(params, QStringLiteral("color2"))),
            params.getBool(QStringLiteral("onSelected"), false));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Colorized '%1' by Perlin noise").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterColorNoise)) {
        const RandomSeed seed = params.getRandomSeed();
        vcg::tri::UpdateColor<VCGMesh>::PerVertexAddNoise(
            mesh,
            params.getInt(QStringLiteral("noiseBits"), 1),
            params.getBool(QStringLiteral("onSelected"), false),
            int(seed.value));
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Added color noise to '%1'").arg(meshLabel(entry, meshIndex)));
        return success({ seed.message() });
    }

    if (filterId == QString::fromLatin1(kFilterSaturateQuality)) {
        vcg::tri::UpdateQuality<VCGMesh>::VertexSaturate(mesh, Scalar(params.getDouble(QStringLiteral("gradientThr"), 1.0)));
        ensureVertexQuality(entry);
        QStringList info;
        if (params.getBool(QStringLiteral("updateColor"), false)) {
            Histogramf hist;
            vcg::tri::Stat<VCGMesh>::ComputePerVertexQualityHistogram(mesh, hist);
            vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(mesh, hist.Percentile(0.1f), hist.Percentile(0.9f));
            ensureVertexColor(entry);
            info << QObject::tr("Updated vertex color ramp from saturated quality values.");
        }
        markGeometry(doc, meshIndex, QObject::tr("Saturated vertex quality of '%1'").arg(meshLabel(entry, meshIndex)));
        return qualitySuccess(meshIndex, MeshFilterVisualizationAttribute::VertexQuality, info);
    }

    if (filterId == QString::fromLatin1(kFilterMapVQuality)) {
        Histogramf hist;
        vcg::tri::Stat<VCGMesh>::ComputePerVertexQualityHistogram(mesh, hist);
        const QString colorMapId = resolvedColorMapId(params);
        const bool invert = params.getBool(QStringLiteral("invert"), false);
        const bool zeroSym = params.getBool(QStringLiteral("zeroSym"), false);
        const float perc = std::clamp(
            static_cast<float>(params.getDouble(QStringLiteral("perc"), 0.0)) / 100.0f,
            0.0f,
            0.5f);
        float rangeMin = static_cast<float>(params.getDouble(QStringLiteral("minVal"), hist.MinV()));
        float rangeMax = static_cast<float>(params.getDouble(QStringLiteral("maxVal"), hist.MaxV()));
        RenderQualityRange range;
        if (perc > 0.0f) {
            range = sampledRenderQualityRange(finiteVertexQualityValues(mesh), zeroSym, perc);
        } else {
            if (zeroSym) {
                const float absMax = std::max(std::abs(rangeMin), std::abs(rangeMax));
                rangeMin = -absMax;
                rangeMax = absMax;
            }
            range = fixedRenderQualityRange(rangeMin, rangeMax);
        }
        if (!range.valid)
            return fail(QObject::tr("Cannot map vertex quality to color: quality range is not finite."));

        if (isLegacyVcgColorMap(colorMapId)) {
            const vcg::ColorMap cmap = colorMapFromId(colorMapId);
            vcg::tri::UpdateColor<VCGMesh>::PerVertexQualityRamp(
                mesh,
                Scalar(range.minV),
                Scalar(range.maxV),
                cmap);
        } else {
            applyMeshLab2VertexQualityColorMap(mesh, range, colorMapId, invert);
        }
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Mapped vertex quality into color on '%1'").arg(meshLabel(entry, meshIndex)));
        return success({
            QObject::tr("Quality Range: %1 %2").arg(range.minV).arg(range.maxV)
        });
    }

    if (filterId == QString::fromLatin1(kFilterClampQuality)) {
        Histogramf hist;
        vcg::tri::Stat<VCGMesh>::ComputePerVertexQualityHistogram(mesh, hist);
        Scalar rangeMin = Scalar(params.getDouble(QStringLiteral("minVal"), hist.MinV()));
        Scalar rangeMax = Scalar(params.getDouble(QStringLiteral("maxVal"), hist.MaxV()));
        Scalar perc = Scalar(params.getDouble(QStringLiteral("perc"), 0.0));
        Scalar percLo = hist.Percentile(perc / 100.0f);
        Scalar percHi = hist.Percentile(1.0f - perc / 100.0f);
        if (params.getBool(QStringLiteral("zeroSym"), false)) {
            rangeMin = std::min(rangeMin, -vcg::math::Abs(rangeMax));
            rangeMax = std::max(vcg::math::Abs(rangeMin), rangeMax);
            percLo = std::min(percLo, -vcg::math::Abs(percHi));
            percHi = std::max(vcg::math::Abs(percLo), percHi);
        }
        if (perc > 0)
            vcg::tri::UpdateQuality<VCGMesh>::VertexClamp(mesh, percLo, percHi);
        else
            vcg::tri::UpdateQuality<VCGMesh>::VertexClamp(mesh, rangeMin, rangeMax);
        ensureVertexQuality(entry);
        markGeometry(doc, meshIndex, QObject::tr("Clamped vertex quality of '%1'").arg(meshLabel(entry, meshIndex)));
        return qualitySuccess(meshIndex, MeshFilterVisualizationAttribute::VertexQuality);
    }

    if (filterId == QString::fromLatin1(kFilterMapEQuality)) {
        // VCGLib has ComputePerEdgeQualityMinMax but no per-edge histogram, so the
        // percentile crop the vertex and face versions offer has nothing to stand on.
        // A custom range covers the case it was mostly used for -- making two layers
        // comparable -- without pretending to a statistic we cannot compute.
        const auto minmax = vcg::tri::Stat<VCGMesh>::ComputePerEdgeQualityMinMax(mesh);
        Scalar rangeMin = minmax.first;
        Scalar rangeMax = minmax.second;
        if (params.getBool(QStringLiteral("useCustomRange"), false)) {
            rangeMin = Scalar(params.getDouble(QStringLiteral("minVal"), 0.0));
            rangeMax = Scalar(params.getDouble(QStringLiteral("maxVal"), 1.0));
        }
        if (params.getBool(QStringLiteral("zeroSym"), false)) {
            rangeMin = std::min(rangeMin, -vcg::math::Abs(rangeMax));
            rangeMax = std::max(vcg::math::Abs(rangeMin), rangeMax);
        }
        if (!(rangeMax > rangeMin)) {
            return fail(QObject::tr(
                "Every edge has the same scalar (%1), so there is no range to map. "
                "Write varying values with Compute Edge Scalar by Expression first.")
                    .arg(rangeMin));
        }
        const vcg::ColorMap cmap = colorMapFromId(params.getEnum(QStringLiteral("colorMap"), QStringLiteral("rgb")));
        vcg::tri::UpdateColor<VCGMesh>::PerEdgeQualityRamp(mesh, rangeMin, rangeMax, false, cmap);
        ensureEdgeColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Mapped edge scalar into color on '%1'").arg(meshLabel(entry, meshIndex)));
        return qualitySuccess(meshIndex, MeshFilterVisualizationAttribute::EdgeColor, {
            QObject::tr("Mapped scalar range %1 .. %2 onto %3 edges.")
                .arg(rangeMin).arg(rangeMax).arg(mesh.EN())
        });
    }

    if (filterId == QString::fromLatin1(kFilterMapFQuality)) {
        Histogramf hist;
        vcg::tri::Stat<VCGMesh>::ComputePerFaceQualityHistogram(mesh, hist);
        Scalar rangeMin = Scalar(params.getDouble(QStringLiteral("minVal"), hist.MinV()));
        Scalar rangeMax = Scalar(params.getDouble(QStringLiteral("maxVal"), hist.MaxV()));
        Scalar perc = Scalar(params.getDouble(QStringLiteral("perc"), 0.0));
        Scalar percLo = hist.Percentile(perc / 100.0f);
        Scalar percHi = hist.Percentile(1.0f - perc / 100.0f);
        if (params.getBool(QStringLiteral("zeroSym"), false)) {
            rangeMin = std::min(rangeMin, -vcg::math::Abs(rangeMax));
            rangeMax = std::max(vcg::math::Abs(rangeMin), rangeMax);
            percLo = std::min(percLo, -vcg::math::Abs(percHi));
            percHi = std::max(vcg::math::Abs(percLo), percHi);
        }
        const vcg::ColorMap cmap = colorMapFromId(params.getEnum(QStringLiteral("colorMap"), QStringLiteral("rgb")));
        if (perc > 0)
            vcg::tri::UpdateColor<VCGMesh>::PerFaceQualityRamp(mesh, percLo, percHi, false, cmap);
        else
            vcg::tri::UpdateColor<VCGMesh>::PerFaceQualityRamp(mesh, rangeMin, rangeMax, false, cmap);
        ensureFaceColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Mapped face quality into color on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterDiscreteCurvature)) {
        vcg::tri::UpdateFlags<VCGMesh>::FaceBorderFromFF(mesh);
        if (vcg::tri::Clean<VCGMesh>::CountNonManifoldEdgeFF(mesh) > 0)
            return fail(QObject::tr("Mesh has some not 2-manifold faces; curvature computation requires manifoldness."));
        vcg::tri::UpdateCurvature<VCGMesh>::MeanAndGaussian(mesh);
        const QString curvatureType = params.getEnum(QStringLiteral("CurvatureType"), QStringLiteral("mean"));
        if (curvatureType == QStringLiteral("mean"))
            vcg::tri::UpdateQuality<VCGMesh>::VertexFromAttributeName(mesh, "KH");
        else if (curvatureType == QStringLiteral("gaussian"))
            vcg::tri::UpdateQuality<VCGMesh>::VertexFromAttributeName(mesh, "KG");
        else if (curvatureType == QStringLiteral("rms"))
            vcg::tri::UpdateQuality<VCGMesh>::VertexRMSCurvatureFromHGAttribute(mesh);
        else
            vcg::tri::UpdateQuality<VCGMesh>::VertexAbsoluteCurvatureFromHGAttribute(mesh);

        ensureVertexQuality(entry);
        Histogramf hist;
        vcg::tri::Stat<VCGMesh>::ComputePerVertexQualityHistogram(mesh, hist);
        markGeometry(doc, meshIndex, QObject::tr("Computed discrete curvature on '%1'").arg(meshLabel(entry, meshIndex)));
        return qualitySuccess(meshIndex, MeshFilterVisualizationAttribute::VertexQuality, {
            QObject::tr("Curvature Range: %1 %2 (Used 10/90 percentiles %3 %4)")
                .arg(hist.MinV())
                .arg(hist.MaxV())
                .arg(hist.Percentile(0.1f))
                .arg(hist.Percentile(0.9f))
        });
    }

    const bool geometricQuality = filterId == QString::fromLatin1(kFilterGeometricQuality);
    const bool textureDistortion = filterId == QString::fromLatin1(kFilterTextureDistortion);
    if (geometricQuality || textureDistortion) {
        const QString metricId = params.getEnum(
            QStringLiteral("metric"),
            geometricQuality ? QStringLiteral("area_max_side") : QStringLiteral("angle"));
        const TextureDistortionMetric textureMetric = textureDistortionMetric(metricId);
        const bool useWedgeTex = (entry.ioMask & Mask::IOM_WEDGTEXCOORD) != 0;
        const bool hasAnyTex = (entry.ioMask & (Mask::IOM_WEDGTEXCOORD | Mask::IOM_VERTTEXCOORD)) != 0;
        const bool planarity =
            metricId == QStringLiteral("polygonal_planarity_max")
            || metricId == QStringLiteral("polygonal_planarity_relative");

        if (textureDistortion && !hasAnyTex)
            return fail(QObject::tr("Texture distortion requires texture coordinates."));
        if (geometricQuality && planarity && (entry.ioMask & Mask::IOM_BITPOLYGONAL) == 0)
            return fail(QObject::tr("This metric is meaningless for triangle-only meshes (all faces are planar by definition)."));

        Scalar areaScaleVal = 0;
        Scalar edgeScaleVal = 0;
        if (textureDistortion && textureMetric != TextureDistortionMetric::Angle) {
            if (useWedgeTex)
                vcg::tri::Distortion<VCGMesh, true>::MeshScalingFactor(
                    mesh, areaScaleVal, edgeScaleVal);
            else
                vcg::tri::Distortion<VCGMesh, false>::MeshScalingFactor(
                    mesh, areaScaleVal, edgeScaleVal);
            if (textureMetric == TextureDistortionMetric::Edge
                && (!std::isfinite(edgeScaleVal) || edgeScaleVal <= 0))
                return fail(QObject::tr(
                    "Texture edge distortion is undefined because the total edge length is zero."));
            if (textureMetric == TextureDistortionMetric::Area
                && (!std::isfinite(areaScaleVal) || areaScaleVal == 0))
                return fail(QObject::tr(
                    "Texture area distortion is undefined because the total oriented UV area is zero."));
            if ((textureMetric == TextureDistortionMetric::L2Stretch
                    || textureMetric == TextureDistortionMetric::LInfStretch)
                && (!std::isfinite(areaScaleVal) || areaScaleVal <= 0))
                return fail(QObject::tr(
                    "Texture stretch is undefined because the total oriented UV area is not positive."));
        }

        ensureFaceQuality(entry);
        for (VCGFace &face : mesh.face) {
            if (face.IsD())
                continue;
            if (geometricQuality && metricId == QStringLiteral("area_max_side"))
                face.Q() = vcg::Quality(face.P(0), face.P(1), face.P(2));
            else if (geometricQuality && metricId == QStringLiteral("normalized_radius_ratio"))
                face.Q() = vcg::QualityRadii(face.P(0), face.P(1), face.P(2));
            else if (geometricQuality && metricId == QStringLiteral("mean_ratio"))
                face.Q() = vcg::QualityMeanRatio(face.P(0), face.P(1), face.P(2));
            else if (geometricQuality && metricId == QStringLiteral("area"))
                face.Q() = vcg::DoubleArea(face) * 0.5f;
            else if (textureDistortion)
                face.Q() = useWedgeTex
                    ? faceTextureDistortion<true>(face, textureMetric, areaScaleVal, edgeScaleVal)
                    : faceTextureDistortion<false>(face, textureMetric, areaScaleVal, edgeScaleVal);
        }

        if (geometricQuality && planarity) {
            vcg::tri::UpdateFlags<VCGMesh>::FaceClearV(mesh);
            std::vector<VCGMesh::VertexPointer> vertVec;
            std::vector<VCGMesh::FacePointer> faceVec;
            for (size_t i = 0; i < mesh.face.size(); ++i) {
                if (mesh.face[i].IsV())
                    continue;
                vcg::tri::PolygonSupport<VCGMesh, VCGMesh>::ExtractPolygon(&mesh.face[i], vertVec, faceVec);
                std::vector<VCGMesh::CoordType> points;
                points.reserve(vertVec.size());
                for (VCGMesh::VertexPointer vp : vertVec)
                    points.push_back(vp->P());
                vcg::Plane3f plane;
                vcg::FitPlaneToPointSet(points, plane);
                float maxDist = 0.0f;
                float sumDist = 0.0f;
                float halfPerim = 0.0f;
                for (size_t j = 0; j < points.size(); ++j) {
                    const float d = std::fabs(vcg::SignedDistancePlanePoint(plane, points[j]));
                    sumDist += d;
                    maxDist = std::max(maxDist, d);
                    halfPerim += vcg::Distance(points[j], points[(j + 1) % points.size()]);
                }
                const float avgDist = sumDist / std::max<size_t>(1, points.size());
                for (VCGMesh::FacePointer fp : faceVec)
                    fp->Q() = (metricId == QStringLiteral("polygonal_planarity_max")) ? maxDist : (avgDist / std::max(1e-12f, halfPerim));
            }
        }

        markGeometry(
            doc,
            meshIndex,
            geometricQuality
                ? QObject::tr("Computed per-face geometric quality on '%1'").arg(meshLabel(entry, meshIndex))
                : QObject::tr("Computed per-face texture distortion on '%1'").arg(meshLabel(entry, meshIndex)));
        return qualitySuccess(meshIndex, MeshFilterVisualizationAttribute::FaceQuality);
    }

    if (filterId == QString::fromLatin1(kFilterRandomConnected)) {
        vcg::tri::UpdateColor<VCGMesh>::PerFaceRandomConnectedComponent(mesh);
        ensureFaceColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Assigned random connected-component colors on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterRandomFace)) {
        vcg::tri::UpdateColor<VCGMesh>::PerFaceRandom(mesh);
        ensureFaceColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Assigned random face colors on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterVertexSmooth)) {
        vcg::tri::Smooth<VCGMesh>::VertexColorLaplacian(mesh, params.getInt(QStringLiteral("iteration"), 1), false, cb);
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Smoothed vertex colors on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterFaceSmooth)) {
        vcg::tri::Smooth<VCGMesh>::FaceColorLaplacian(mesh, params.getInt(QStringLiteral("iteration"), 1), false, cb);
        ensureFaceColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Smoothed face colors on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterFaceToVertex)) {
        vcg::tri::UpdateColor<VCGMesh>::PerVertexFromFace(mesh);
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Transferred face colors to vertices on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterMeshToFace)) {
        vcg::tri::UpdateColor<VCGMesh>::PerFaceConstant(mesh, mesh.C());
        ensureFaceColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Transferred mesh color to faces on '%1'").arg(meshLabel(entry, meshIndex)));
        return success({ QObject::tr("Transferred mesh color to faces on current mesh.") });
    }

    if (filterId == QString::fromLatin1(kFilterVertexToFace)) {
        vcg::tri::UpdateColor<VCGMesh>::PerFaceFromVertex(mesh);
        ensureFaceColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Transferred vertex colors to faces on '%1'").arg(meshLabel(entry, meshIndex)));
        return success();
    }

    if (filterId == QString::fromLatin1(kFilterTextureToVertex)) {
        const int textureCount = Document::meshTextureAssociationCount(entry);
        if (textureCount <= 0)
            return fail(QObject::tr("Current mesh has no associated textures."));

        const int requestedTextureSlot = params.getTextureRef(QStringLiteral("sourceTexture"), 0);
        if (requestedTextureSlot < 0)
            return fail(QObject::tr("Source Texture must be automatic or a positive selection."));
        if (requestedTextureSlot > textureCount) {
            return fail(
                QObject::tr("Source Texture %1 is out of range. The current mesh has %2 associated texture(s).")
                    .arg(requestedTextureSlot)
                    .arg(textureCount));
        }

        std::vector<QImage> images(static_cast<size_t>(textureCount), QImage());
        for (int textureIndex = 0; textureIndex < textureCount; ++textureIndex) {
            QString textureError;
            if (!Tex::loadAssociatedTextureImage(entry, textureIndex, images[size_t(textureIndex)], textureError))
                return fail(textureError);
        }

        for (VCGFace &face : mesh.face) {
            if (face.IsD())
                continue;
            for (int k = 0; k < 3; ++k) {
                const int texIndex = requestedTextureSlot > 0
                    ? requestedTextureSlot - 1
                    : int(face.WT(k).N());
                if (texIndex >= 0 && texIndex < textureCount) {
                    const QRgb value = sampleWrappedTexture(images[size_t(texIndex)], face.WT(k).P());
                    face.V(k)->C() = vcg::Color4b(qRed(value), qGreen(value), qBlue(value), 255);
                } else {
                    face.V(k)->C() = vcg::Color4b(255, 255, 255, 255);
                }
            }
        }
        ensureVertexColor(entry);
        markGeometry(doc, meshIndex, QObject::tr("Transferred texture colors to vertices on '%1'").arg(meshLabel(entry, meshIndex)));
        if (requestedTextureSlot > 0) {
            return success({
                QObject::tr("Transferred texture slot %1 to vertex colors.").arg(requestedTextureSlot)
            });
        }
        return success({
            QObject::tr("Transferred texture colors to vertices using per-face texture assignments.")
        });
    }

    if (filterId == QString::fromLatin1(kFilterVertexToFaceQuality)) {
        vcg::tri::UpdateQuality<VCGMesh>::FaceFromVertex(mesh);
        ensureFaceQuality(entry);
        markGeometry(doc, meshIndex, QObject::tr("Transferred vertex quality to faces on '%1'").arg(meshLabel(entry, meshIndex)));
        return qualitySuccess(meshIndex, MeshFilterVisualizationAttribute::FaceQuality);
    }

    if (filterId == QString::fromLatin1(kFilterFaceToVertexQuality)) {
        vcg::tri::UpdateQuality<VCGMesh>::VertexFromFace(mesh, params.getBool(QStringLiteral("areaWeight"), true));
        ensureVertexQuality(entry);
        markGeometry(doc, meshIndex, QObject::tr("Transferred face quality to vertices on '%1'").arg(meshLabel(entry, meshIndex)));
        return qualitySuccess(meshIndex, MeshFilterVisualizationAttribute::VertexQuality);
    }

    return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
}

void registerColorProcFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<ColorProcFilterPlugin>());
}
