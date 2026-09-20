#include "geogramatlas.h"

#include "document.h"
#include "filterparam.h"
#include "geogrammeshadapter.h"

#include <geogram/parameterization/mesh_atlas_maker.h>
#include <geogram/parameterization/mesh_param_packer.h>
#include <geogram/parameterization/mesh_segmentation.h>

#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <QStringList>
#include <algorithm>
#include <exception>

namespace {

constexpr QLatin1StringView kAtlas("parametrize_by_atlas_geogram");
constexpr QLatin1StringView kPack("pack_uv_charts_geogram");
constexpr QLatin1StringView kSegment("compute_chart_segmentation_geogram");

using Mask = vcg::tri::io::Mask;
namespace GeoAdapter = meshlab::geogram;

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

::GEO::ChartParameterizer chartParameterizer(const QString &id)
{
    if (id == QLatin1StringView("projection"))
        return ::GEO::PARAM_PROJECTION;
    if (id == QLatin1StringView("lscm"))
        return ::GEO::PARAM_LSCM;
    return ::GEO::PARAM_ABF;
}

::GEO::ChartPacker chartPacker(const QString &id)
{
    if (id == QLatin1StringView("none"))
        return ::GEO::PACK_NONE;
    if (id == QLatin1StringView("xatlas"))
        return ::GEO::PACK_XATLAS;
    return ::GEO::PACK_TETRIS;
}

::GEO::MeshSegmenter meshSegmenter(const QString &id)
{
    if (id == QLatin1StringView("vsaL12"))
        return ::GEO::SEGMENT_GEOMETRIC_VSA_L12;
    if (id == QLatin1StringView("inertiaAxis"))
        return ::GEO::SEGMENT_INERTIA_AXIS;
    return ::GEO::SEGMENT_GEOMETRIC_VSA_L2;
}

// Geogram's atlas pipeline stores UVs on facet corners, which is exactly
// MeshLab's per-wedge domain, so this is a straight copy rather than the
// per-vertex path geogramparametrization takes.
bool writeWedgeTexCoords(
    Document::MeshEntry &entry,
    const ::GEO::Mesh &geoMesh,
    const GeoAdapter::GeoMesh &source,
    const std::vector<std::array<float, 2>> &uv,
    QString &error)
{
    VCGMesh &mesh = entry.mesh;
    mesh.face.EnableWedgeTexCoord();

    const ::GEO::index_t facetCount = geoMesh.facets.nb();
    if (size_t(facetCount) != source.faceToSourceIndex.size()) {
        error = QObject::tr("geogram returned %1 faces for %2 input faces.")
                    .arg(facetCount)
                    .arg(source.faceToSourceIndex.size());
        return false;
    }

    for (::GEO::index_t f = 0; f < facetCount; ++f) {
        const int sourceFaceIndex = source.faceToSourceIndex[size_t(f)];
        if (sourceFaceIndex < 0 || size_t(sourceFaceIndex) >= mesh.face.size())
            continue;
        VCGFace &face = mesh.face[size_t(sourceFaceIndex)];
        if (face.IsD())
            continue;
        for (int corner = 0; corner < 3; ++corner) {
            const ::GEO::index_t c = geoMesh.facets.corner(f, ::GEO::index_t(corner));
            if (size_t(c) >= uv.size())
                continue;
            face.WT(corner).U() = uv[size_t(c)][0];
            face.WT(corner).V() = uv[size_t(c)][1];
            face.WT(corner).N() = 0;
        }
    }

    entry.ioMask |= Mask::IOM_WEDGTEXCOORD;
    return true;
}

MeshFilterRunResult runSegmentation(
    Document &doc,
    Document::MeshEntry &entry,
    int meshIndex,
    const FilterParams &params)
{
    const QString segmenterId = params.getEnum(QStringLiteral("segmenter"), QStringLiteral("vsaL2"));
    const int segmentCount = std::max(2, params.getInt(QStringLiteral("segmentCount"), 8));

    GeoAdapter::ensureInitialized();

    QString error;
    GeoAdapter::GeoMesh input;
    if (!GeoAdapter::meshToGeo(entry.mesh, input, error))
        return fail(error);
    input.mesh.facets.connect();

    doc.beginFilterProgress(QObject::tr("Compute chart segmentation"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, "Segmenting...");

    QStringList geoLog;
    ::GEO::index_t chartCount = 0;
    try {
        GeoAdapter::LogCapture capture(geoLog);
        chartCount = ::GEO::mesh_segment(
            input.mesh,
            meshSegmenter(segmenterId),
            ::GEO::index_t(segmentCount));
    } catch (const std::exception &e) {
        error = QObject::tr("geogram segmentation failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("geogram segmentation failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    std::vector<int> charts;
    if (!GeoAdapter::readFacetCharts(input.mesh, charts, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    VCGMesh &mesh = entry.mesh;
    mesh.face.EnableQuality();
    for (size_t f = 0; f < charts.size() && f < input.faceToSourceIndex.size(); ++f) {
        const int sourceFaceIndex = input.faceToSourceIndex[f];
        if (sourceFaceIndex < 0 || size_t(sourceFaceIndex) >= mesh.face.size())
            continue;
        VCGFace &face = mesh.face[size_t(sourceFaceIndex)];
        if (!face.IsD())
            face.Q() = float(charts[f]);
    }
    entry.ioMask |= Mask::IOM_FACEQUALITY;

    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Computed chart segmentation for '%1'").arg(entry.name));
    doc.finishFilterProgress(true, QObject::tr("Computed chart segmentation."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.visualizationHints.push_back(
        { meshIndex, MeshFilterVisualizationAttribute::FaceQuality });
    result.infoMessages
        << QObject::tr("Charts: %1").arg(chartCount)
        << QObject::tr("The chart index of each face is stored in the face scalar field.");
    result.infoMessages << geoLog;
    return result;
}

} // namespace

MeshFilterRunResult runGeogramAtlasFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    const bool isAtlas = filterId == QString::fromLatin1(kAtlas);
    const bool isPack = filterId == QString::fromLatin1(kPack);
    const bool isSegment = filterId == QString::fromLatin1(kSegment);
    if (!isAtlas && !isPack && !isSegment)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh."));
    Document::MeshEntry &entry = doc.mesh(meshIndex);

    if (isSegment)
        return runSegmentation(doc, entry, meshIndex, params);

    GeoAdapter::ensureInitialized();

    QString error;
    GeoAdapter::GeoMesh input;
    if (!GeoAdapter::meshToGeo(entry.mesh, input, error))
        return fail(error);
    input.mesh.facets.connect();

    const QString label = isAtlas ? QObject::tr("atlas") : QObject::tr("chart packing");
    doc.beginFilterProgress(QObject::tr("Parametrize by %1").arg(label));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(20, isAtlas ? "Segmenting and flattening..." : "Packing charts...");

    QStringList geoLog;
    try {
        GeoAdapter::LogCapture capture(geoLog);
        if (isAtlas) {
            ::GEO::mesh_make_atlas(
                input.mesh,
                params.getDouble(QStringLiteral("hardAngleThreshold"), 45.0),
                chartParameterizer(params.getEnum(
                    QStringLiteral("chartParametrizer"), QStringLiteral("abf"))),
                chartPacker(params.getEnum(
                    QStringLiteral("chartPacker"), QStringLiteral("tetris"))));
        } else {
            // The packers read the per-corner UVs already on the mesh, so the
            // existing per-wedge coordinates have to be carried across first.
            if (!GeoAdapter::writeWedgeTexCoordsToGeo(entry.mesh, input, error)) {
                doc.finishFilterProgress(false, error);
                return fail(error);
            }
            const QString packer =
                params.getEnum(QStringLiteral("packer"), QStringLiteral("tetris"));
            if (packer == QLatin1StringView("normalizeOnly"))
                ::GEO::pack_atlas_only_normalize_charts(input.mesh);
            else if (packer == QLatin1StringView("xatlas"))
                ::GEO::pack_atlas_using_xatlas(input.mesh);
            else
                ::GEO::pack_atlas_using_tetris_packer(input.mesh);
        }
    } catch (const std::exception &e) {
        error = QObject::tr("geogram failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("geogram failed with an unknown error.");
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(90, "Writing texture coordinates...");

    std::vector<std::array<float, 2>> uv;
    if (!GeoAdapter::readCornerTexCoords(input.mesh, uv, error)
        || !writeWedgeTexCoords(entry, input.mesh, input, uv, error)) {
        doc.finishFilterProgress(false, error);
        return fail(error);
    }

    doc.markMeshGeometryChanged(
        meshIndex,
        QObject::tr("Computed %1 for '%2'").arg(label, entry.name));
    doc.finishFilterProgress(true, QObject::tr("Computed UV layout."));

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages << QObject::tr("Computed %1 for '%2'.").arg(label, entry.name);
    if (isAtlas) {
        std::vector<int> charts;
        QString chartError;
        if (GeoAdapter::readFacetCharts(input.mesh, charts, chartError) && !charts.empty())
            result.infoMessages << QObject::tr("Charts: %1").arg(*std::max_element(charts.begin(), charts.end()) + 1);
    }
    result.infoMessages << geoLog;
    return result;
}

bool isGeogramAtlasFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kAtlas)
        || filterId == QString::fromLatin1(kPack)
        || filterId == QString::fromLatin1(kSegment);
}
