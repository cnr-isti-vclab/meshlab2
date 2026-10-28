#include "textureconsolidationfilterplugin.h"

#include "camerashot.h"
#include "document.h"
#include "helperprocess.h"
#include "meshfilterpluginmanager.h"
#include "pinholecamera.h"
#include "textureassociationutils.h"
#include "vcgmesh.h"

#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/stat.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/append.h>
#include <wrap/io_trimesh/export_ply.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <memory>
#include <vector>

namespace {

constexpr QLatin1StringView kFilter(
    "transfer_color_from_visible_rasters_to_texture_texture_consolidation");

// The authors prepare their data at a total surface area of 5 and tune the flow's
// defaults there (the vase example's README: "mesh and cameras scaled to surface area
// 5"); their own README warns that the flow parameters depend on the mesh's scale. So the
// mesh and the cameras are scaled to that area before export, and the defaults then hold
// whatever units the layer is in.
constexpr double kReferenceArea = 5.0;

using Mask = vcg::tri::io::Mask;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.errorMessage = message;
    return result;
}

void progress(Document &doc, int percentage, const char *message)
{
    if (vcg::CallBackPos *callback = doc.progressCallback())
        (*callback)(percentage, message);
}

QString executableName(const QString &name)
{
#ifdef Q_OS_WIN
    return name + QStringLiteral(".exe");
#else
    return name;
#endif
}

// Where the three helper programs are: an explicit override, then the location the
// application bundle puts them, then the build tree, which is what a developer runs.
QString helperDirectory()
{
    const QString override = qEnvironmentVariable("MESHLAB2_TEXTURE_CONSOLIDATION_ROOT");
    if (!override.isEmpty())
        return override;
    const QDir appDir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    const QString bundled =
        QDir::cleanPath(appDir.absoluteFilePath(QStringLiteral("../Helpers/texture-consolidation")));
#else
    const QString bundled = appDir.absoluteFilePath(QStringLiteral("texture-consolidation/bin"));
#endif
    if (QFileInfo::exists(QDir(bundled).filePath(executableName(QStringLiteral("BackProjection")))))
        return bundled;
#ifdef MESHLAB2_TEXTURE_CONSOLIDATION_BUILD_DIR
    return QStringLiteral(MESHLAB2_TEXTURE_CONSOLIDATION_BUILD_DIR);
#else
    return bundled;
#endif
}

QJsonArray jsonRow(const std::array<double, 4> &row)
{
    return QJsonArray{row[0], row[1], row[2], row[3]};
}

// A step succeeded only if it exited zero *and* left the files it was asked for. The exit
// status alone was not enough before the stitcher was built through its MK_ERROR_OUT
// wrapper, and the check costs nothing, so it stays.
bool producedImages(const QString &pattern, int count, QSize expected, QString &error)
{
    for (int i = 0; i < count; ++i) {
        const QString path = pattern.arg(i);
        const QImage image(path);
        if (image.isNull()) {
            error = QObject::tr("Expected output %1 was not written.").arg(QFileInfo(path).fileName());
            return false;
        }
        if (expected.isValid() && image.size() != expected) {
            error = QObject::tr("Output %1 is %2 x %3, expected %4 x %5.")
                        .arg(QFileInfo(path).fileName())
                        .arg(image.width()).arg(image.height())
                        .arg(expected.width()).arg(expected.height());
            return false;
        }
    }
    return true;
}

} // namespace

QString TextureConsolidationFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.texture_consolidation");
}

QString TextureConsolidationFilterPlugin::name() const
{
    return QObject::tr("Texture Consolidation Filters");
}

MeshFilterRunResult TextureConsolidationFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId != QString::fromLatin1(kFilter))
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    const int meshIndex = doc.currentMeshIndex();
    if (meshIndex < 0 || meshIndex >= doc.meshCount())
        return fail(QObject::tr("No current mesh."));
    Document::MeshEntry &entry = doc.mesh(meshIndex);
    if (!vcg::tri::HasPerWedgeTexCoord(entry.mesh))
        return fail(QObject::tr("The mesh has no texture atlas. Create one first, for example "
                                "with Parametrize by Atlas (xatlas)."));

    const int textureSize = params.getInt(QStringLiteral("textureSize"));
    const QString textureFile = params.getFileSave(QStringLiteral("textureFile")).trimmed();
    if (textureFile.isEmpty())
        return fail(QObject::tr("No texture file given."));

    // The views: every visible raster with a valid camera and an image. The helpers take
    // pinhole cameras, so anything else is refused here rather than misprojected there.
    struct View {
        QString name;
        QImage image;
        CameraShot::VcgShot shot;
    };
    std::vector<View> views;
    for (int ri = 0; ri < doc.rasterCount(); ++ri) {
        Document::RasterEntry &raster = doc.raster(ri);
        if (!raster.visible || !raster.shot.isValid() || raster.planes.empty())
            continue;
        RasterPlane *plane = raster.currentPlane();
        if (!plane)
            continue;
        Document::ensureRasterPlaneImage(*plane);
        if (!plane->hasImage())
            continue;
        const CameraShot::VcgShot shot = raster.shot.toVcgShot();
        if (shot.Intrinsics.cameraType != vcg::Camera<float>::PERSPECTIVE) {
            return fail(QObject::tr("Raster '%1' does not have a perspective camera; this filter "
                                    "needs pinhole cameras.").arg(raster.name));
        }
        const auto k = raster.shot.distortionK();
        if (k[0] != 0.0f || k[1] != 0.0f || k[2] != 0.0f || k[3] != 0.0f) {
            return fail(QObject::tr("Raster '%1' has lens distortion, which this filter does not "
                                    "model. Undistort the image first.").arg(raster.name));
        }
        views.push_back({raster.name, plane->image, shot});
    }
    if (views.size() < 2) {
        return fail(QObject::tr("This filter needs at least two visible rasters with cameras "
                                "and images; found %1.").arg(views.size()));
    }

    const QDir helpers(helperDirectory());
    const QString backProjection = helpers.filePath(executableName(QStringLiteral("BackProjection")));
    const QString textureFlow = helpers.filePath(executableName(QStringLiteral("TextureFlow")));
    const QString stitching = helpers.filePath(executableName(QStringLiteral("TextureDomainStitching")));
    for (const QString &program : {backProjection, textureFlow, stitching}) {
        if (!QFileInfo::exists(program)) {
            return fail(QObject::tr("The TextureConsolidation helper %1 is missing. Rebuild MeshLab "
                                    "with MESHLAB2_PLUGIN_FILTER_TEXTURE_CONSOLIDATION enabled.")
                            .arg(QFileInfo(program).fileName()));
        }
    }

    QTemporaryDir temporary(
        QDir(QDir::tempPath()).filePath(QStringLiteral("MeshLab-TextureConsolidation-XXXXXX")));
    if (!temporary.isValid())
        return fail(QObject::tr("Cannot create a temporary directory."));
    // MESHLAB2_TEXTURE_CONSOLIDATION_KEEP keeps the helpers' inputs and outputs even when
    // the run succeeds, so that a result can be reproduced with the programs by hand.
    const bool keepFiles = qEnvironmentVariableIsSet("MESHLAB2_TEXTURE_CONSOLIDATION_KEEP");
    if (keepFiles)
        temporary.setAutoRemove(false);
    const QDir work(temporary.path());
    // Any failure from here on keeps the directory, and says where it is.
    const auto failKeeping = [&](const QString &message) {
        temporary.setAutoRemove(false);
        return fail(QObject::tr("%1 The intermediate files are in %2.").arg(message, temporary.path()));
    };
    QElapsedTimer clock;
    clock.start();

    // The mesh in world space, since that is where the cameras are, scaled to the
    // reference area.
    progress(doc, 1, "Writing the mesh, cameras and images...");
    VCGMesh mesh;
    mesh.face.EnableWedgeTexCoord();
    vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(mesh, entry.mesh);
    for (auto &v : mesh.vert) {
        const QVector3D p = entry.transform.map(QVector3D(v.P()[0], v.P()[1], v.P()[2]));
        v.P() = vcg::Point3f(p.x(), p.y(), p.z());
    }
    // The charts of the atlas: faces joined across edges whose UVs agree on both sides.
    // The stitcher's cost grows with them, and an atlas of one triangle per chart -- what a
    // per-triangle parametrization produces -- is beyond it: on the 20,000-face gargoyle
    // laid out that way it ran for minutes holding 9 to 12 GB, where the authors' vase,
    // 65 charts for 9,998 faces, takes seconds. Refused before any helper starts.
    int charts = 0;
    {
        VCGMeshFFAdjScope adjacency(mesh);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFaceFromTexCoord(mesh);
        charts = vcg::tri::Clean<VCGMesh>::CountConnectedComponents(mesh);
    }
    const double facesPerChart = double(mesh.FN()) / double(std::max(1, charts));
    if (facesPerChart < 2.0) {
        return fail(QObject::tr("The atlas has %1 charts for %2 faces, about one per face, as a "
                                "per-triangle parametrization makes. The stitching step cannot "
                                "handle that many charts. Create a chart-based atlas first, for "
                                "example with Parametrize by Atlas (xatlas).")
                        .arg(charts).arg(mesh.FN()));
    }

    const double area = vcg::tri::Stat<VCGMesh>::ComputeMeshArea(mesh);
    if (!(area > 0.0))
        return fail(QObject::tr("The mesh has no surface area."));
    const double scale = std::sqrt(kReferenceArea / area);
    for (auto &v : mesh.vert)
        v.P() *= float(scale);

    const QString meshPath = work.filePath(QStringLiteral("mesh.ply"));
    if (vcg::tri::io::ExporterPLY<VCGMesh>::Save(
            mesh, QFile::encodeName(meshPath).constData(),
            Mask::IOM_VERTCOORD | Mask::IOM_FACEINDEX | Mask::IOM_WEDGTEXCOORD, true) != 0) {
        return failKeeping(QObject::tr("Cannot write the mesh for the helpers."));
    }

    work.mkpath(QStringLiteral("images"));
    QJsonArray frames;
    for (size_t i = 0; i < views.size(); ++i) {
        const QString relative = QStringLiteral("images/%1.png").arg(i);
        const QImage rgb = views[i].image.convertToFormat(QImage::Format_RGB888);
        if (!rgb.save(work.filePath(relative), "PNG"))
            return failKeeping(QObject::tr("Cannot write the image of raster '%1'.").arg(views[i].name));
        const auto cam = texture_consolidation::toPinholeCamera(views[i].shot, scale, rgb.width(), rgb.height());
        frames.append(QJsonObject{
            {QStringLiteral("w"), cam.width}, {QStringLiteral("h"), cam.height},
            {QStringLiteral("fx"), cam.fx}, {QStringLiteral("fy"), cam.fy},
            {QStringLiteral("cx"), cam.cx}, {QStringLiteral("cy"), cam.cy},
            {QStringLiteral("w2c"), QJsonArray{jsonRow(cam.w2c[0]), jsonRow(cam.w2c[1]),
                                               jsonRow(cam.w2c[2]), jsonRow(cam.w2c[3])}},
            {QStringLiteral("file_path"), relative}});
    }
    QFile cameras(work.filePath(QStringLiteral("cameras.json")));
    if (!cameras.open(QIODevice::WriteOnly)
        || cameras.write(QJsonDocument(QJsonObject{{QStringLiteral("frames"), frames}}).toJson()) < 0) {
        return failKeeping(QObject::tr("Cannot write the cameras for the helpers."));
    }
    cameras.close();

    const int viewCount = int(views.size());
    const QString number = QStringLiteral("%1");
    const QString backTextures = work.filePath(QStringLiteral("backprojected/%d.png"));
    const QString backConfidences = work.filePath(QStringLiteral("backprojected/%d.confidence.png"));
    const QString flowTextures = work.filePath(QStringLiteral("registered/%d.png"));
    const QString flowConfidences = work.filePath(QStringLiteral("registered/%d.confidence.png"));
    const QString outputTexture = work.filePath(QStringLiteral("texture.png"));
    const auto qtPattern = [&](QString pattern) { return pattern.replace(QStringLiteral("%d"), number); };
    const QSize textureExtent(textureSize, textureSize);
    QString error;

    HelperProcess::Request back;
    back.program = backProjection;
    back.arguments = {
        QStringLiteral("--mesh"), meshPath,
        QStringLiteral("--cameras"), cameras.fileName(),
        QStringLiteral("--outTextures"), backTextures,
        QStringLiteral("--outConfidences"), backConfidences,
        QStringLiteral("--res"), QString::number(textureSize), QString::number(textureSize),
        QStringLiteral("--depthRadius"), QString::number(params.getInt(QStringLiteral("depthRadius"))),
        QStringLiteral("--cutoff"), QString::number(params.getDouble(QStringLiteral("confidenceCutoff")), 'g', 17),
        QStringLiteral("--verbose")};
    back.workingDirectory = work.path();
    back.label = QStringLiteral("BackProjection");
    back.progressBegin = 3;
    back.progressEnd = 15;
    if (!HelperProcess::run(back, doc, error))
        return failKeeping(error);
    if (!producedImages(qtPattern(backTextures), viewCount, textureExtent, error)
        || !producedImages(qtPattern(backConfidences), viewCount, textureExtent, error)) {
        return failKeeping(error);
    }

    // The flow is where the time goes, and it reports every hierarchy level.
    const int levels = params.getInt(QStringLiteral("levels"));
    HelperProcess::Request flow;
    flow.program = textureFlow;
    flow.arguments = {
        QStringLiteral("--mesh"), meshPath,
        QStringLiteral("--inTextures"), backTextures,
        QStringLiteral("--inConfidences"), backConfidences,
        QStringLiteral("--outTextures"), flowTextures,
        QStringLiteral("--outConfidences"), flowConfidences,
        QStringLiteral("--levels"), QString::number(levels),
        QStringLiteral("--sDiffusion"), QString::number(params.getDouble(QStringLiteral("sourceSmoothing")), 'g', 17),
        QStringLiteral("--sWeight"), QString::number(params.getDouble(QStringLiteral("flowSmoothness")), 'g', 17),
        QStringLiteral("--factor"), QString::number(params.getDouble(QStringLiteral("levelFactor")), 'g', 17),
        QStringLiteral("--normalization"), QString::number(params.getDouble(QStringLiteral("normalizationExtent")), 'g', 17),
        QStringLiteral("--sub"), QString::number(params.getInt(QStringLiteral("subdivisions"))),
        QStringLiteral("--verbose")};
    flow.workingDirectory = work.path();
    flow.label = QStringLiteral("TextureFlow");
    for (int l = 0; l < levels; ++l)
        flow.stageMarkers << QStringLiteral("Level %1:").arg(l);
    flow.progressBegin = 15;
    flow.progressEnd = 85;
    if (!HelperProcess::run(flow, doc, error))
        return failKeeping(error);
    if (!producedImages(qtPattern(flowTextures), viewCount, textureExtent, error)
        || !producedImages(qtPattern(flowConfidences), viewCount, textureExtent, error)) {
        return failKeeping(error);
    }

    // --multi and --wType 1 are what the upstream README calls required for this pipeline,
    // and the erosion is what it prescribes; they are not parameters.
    HelperProcess::Request stitch;
    stitch.program = stitching;
    stitch.arguments = {
        QStringLiteral("--in"), meshPath, flowTextures,
        QStringLiteral("--mask"), flowConfidences,
        QStringLiteral("--multi"), QStringLiteral("--wType"), QStringLiteral("1"),
        QStringLiteral("--erode"), QStringLiteral("1"), QStringLiteral("--erodeEps"), QStringLiteral("0.1"),
        QStringLiteral("--gWeight"), QString::number(params.getDouble(QStringLiteral("gradientWeight")), 'g', 17),
        QStringLiteral("--out"), outputTexture,
        QStringLiteral("--verbose")};
    stitch.workingDirectory = work.path();
    stitch.label = QStringLiteral("TextureDomainStitching");
    stitch.progressBegin = 85;
    stitch.progressEnd = 97;
    if (!HelperProcess::run(stitch, doc, error)) {
        // The stitcher solves at texel resolution, so an atlas whose triangles are much
        // smaller than a texel leaves it a singular system. Measured on the gargoyle
        // fixture (20,000 faces, xatlas): 256 x 256 fails this way, 512 succeeds with
        // warnings, 1024 is clean.
        if (error.contains(QStringLiteral("Failed to factorize matrix"))) {
            error = QObject::tr("The texture is too coarse for this atlas: the stitching could "
                                "not be solved at %1 x %1, which usually means many atlas "
                                "triangles are much smaller than a texel. Try a larger Texture "
                                "Size. (%2)").arg(textureSize).arg(error);
        }
        return failKeeping(error);
    }

    QImage texture(outputTexture);
    if (texture.isNull())
        return failKeeping(QObject::tr("The stitcher did not write the texture."));
    if (texture.size() != textureExtent) {
        return failKeeping(QObject::tr("The stitched texture is %1 x %2, expected %3 x %3.")
                               .arg(texture.width()).arg(texture.height()).arg(textureSize));
    }

    progress(doc, 98, "Attaching the texture...");
    if (!TextureAssociationUtils::saveImages({textureFile}, {texture}, error))
        return failKeeping(error);
    const auto assets = TextureAssociationUtils::makeTextureAssetsFromSavedImages({textureFile}, {texture});
    TextureAssociationUtils::replaceTextureAssociations(entry, assets);
    doc.markMeshMaterialChanged(
        meshIndex, QObject::tr("Consolidated %1 rasters into texture '%2'.").arg(viewCount).arg(textureFile));
    progress(doc, 100, "Texture consolidation complete.");

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.infoMessages
        << QObject::tr("Consolidated %1 rasters into a %2 x %2 texture: %3")
               .arg(viewCount).arg(textureSize).arg(textureFile)
        << QObject::tr("Atlas: %1 charts for %2 faces.").arg(charts).arg(mesh.FN())
        << QObject::tr("Mesh scaled by %1 to a surface area of %2 for the helpers.")
               .arg(scale, 0, 'g', 6).arg(kReferenceArea)
        << QObject::tr("Took %1 s.").arg(double(clock.elapsed()) / 1000.0, 0, 'f', 1);
    if (keepFiles)
        result.infoMessages << QObject::tr("Intermediate files kept in %1.").arg(temporary.path());
    return result;
}

void registerTextureConsolidationFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<TextureConsolidationFilterPlugin>());
}
