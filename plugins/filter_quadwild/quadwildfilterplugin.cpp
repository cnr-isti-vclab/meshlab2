#include "quadwildfilterplugin.h"

#include "document.h"
#include "helperprocess.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <wrap/io_trimesh/export_obj.h>
#include <wrap/io_trimesh/import_obj.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <memory>

namespace {

constexpr QLatin1StringView kQuadWildFilter("remesh_to_quads_quadwild_bimdf");
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

struct RuntimePaths
{
    QString helpers;
    QString resources;
};

RuntimePaths runtimePaths()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    return {
        QDir::cleanPath(appDir.absoluteFilePath(QStringLiteral("../Helpers/quadwild-bimdf"))),
        QDir::cleanPath(appDir.absoluteFilePath(QStringLiteral("../Resources/quadwild-bimdf")))
    };
#else
    const QString resources = appDir.absoluteFilePath(QStringLiteral("quadwild-bimdf"));
    return {QDir(resources).filePath(QStringLiteral("bin")), resources};
#endif
}

QString executableName(QString baseName)
{
#ifdef Q_OS_WIN
    baseName += QStringLiteral(".exe");
#endif
    return baseName;
}

bool writeScaledConfig(
    const QString &sourcePath,
    const QString &targetPath,
    double scale,
    QString &error)
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Cannot read bundled QuadWild configuration: %1")
                    .arg(sourcePath);
        return false;
    }

    QString text = QString::fromUtf8(source.readAll());
    static const QRegularExpression scaleLine(
        QStringLiteral("^scaleFact\\s+[^\\r\\n]+"),
        QRegularExpression::MultilineOption);
    if (!text.contains(scaleLine)) {
        error = QObject::tr("Bundled QuadWild configuration has no scaleFact: %1")
                    .arg(sourcePath);
        return false;
    }
    text.replace(scaleLine, QStringLiteral("scaleFact %1").arg(scale, 0, 'g', 12));

    QFile target(targetPath);
    if (!target.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || target.write(text.toUtf8()) < 0) {
        error = QObject::tr("Cannot create temporary QuadWild configuration: %1")
                    .arg(targetPath);
        return false;
    }
    return true;
}

} // namespace

QString QuadWildFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.quadwild");
}

QString QuadWildFilterPlugin::name() const
{
    return QObject::tr("QuadWild-BiMDF Remeshing Filters");
}

MeshFilterRunResult QuadWildFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId != kQuadWildFilter)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
    if (doc.currentMeshIndex() < 0)
        return fail(QObject::tr("No current mesh selected."));

    Document::MeshEntry &input = doc.mesh(doc.currentMeshIndex());
    if (input.mesh.VN() == 0 || input.mesh.FN() == 0)
        return fail(QObject::tr("QuadWild-BiMDF requires a non-empty triangle mesh."));
    const QString inputName = input.name;
    const QMatrix4x4 inputTransform = input.transform;

    const RuntimePaths runtime = runtimePaths();
    const QString quadwild =
        QDir(runtime.helpers).filePath(executableName(QStringLiteral("quadwild")));
    const QString quadFromPatches =
        QDir(runtime.helpers).filePath(executableName(QStringLiteral("quad_from_patches")));
    if (!QFileInfo::exists(quadwild) || !QFileInfo::exists(quadFromPatches)) {
        return fail(QObject::tr(
            "Bundled QuadWild-BiMDF helpers are missing. Rebuild MeshLab with "
            "MESHLAB2_PLUGIN_FILTER_QUADWILD enabled."));
    }

    QTemporaryDir temporary(
        QDir(QDir::tempPath()).filePath(QStringLiteral("MeshLab-QuadWild-XXXXXX")));
    if (!temporary.isValid())
        return fail(QObject::tr("Cannot create a temporary directory for QuadWild-BiMDF."));

    const QString inputPath = QDir(temporary.path()).filePath(QStringLiteral("input.obj"));
    progress(doc, 2, "Writing QuadWild input...");
    // ExporterOBJ's legacy signature is non-const, although saving is read-only.
    const int exportError = vcg::tri::io::ExporterOBJ<VCGMesh>::Save(
        input.mesh,
        QFile::encodeName(inputPath).constData(),
        Mask::IOM_VERTCOORD | Mask::IOM_FACEINDEX);
    if (exportError != 0) {
        return fail(QObject::tr("Cannot write QuadWild input: %1")
                        .arg(QString::fromLatin1(
                            vcg::tri::io::ExporterOBJ<VCGMesh>::ErrorMsg(exportError))));
    }

    const bool organic =
        params.getEnum(QStringLiteral("surfacePreset")) == QStringLiteral("organic");
    const bool alignSingularities =
        params.getBool(QStringLiteral("alignSingularities"));
    const double scale = params.getDouble(QStringLiteral("outputScale"));

    const QString prepSource = QDir(runtime.resources).filePath(
        organic ? QStringLiteral("config/prep_config/basic_setup_Organic.txt")
                : QStringLiteral("config/prep_config/basic_setup.txt"));
    const QString mainSource = QDir(runtime.resources).filePath(
        alignSingularities ? QStringLiteral("config/main_config/flow.txt")
                           : QStringLiteral("config/main_config/flow_noalign.txt"));
    const QString prepConfig =
        QDir(temporary.path()).filePath(QStringLiteral("prepare.txt"));
    const QString mainConfig =
        QDir(temporary.path()).filePath(QStringLiteral("quadrangulate.txt"));
    QString error;
    if (!writeScaledConfig(prepSource, prepConfig, scale, error)
        || !writeScaledConfig(mainSource, mainConfig, scale, error)) {
        return fail(error);
    }

    progress(doc, 5, "Computing field and patch layout with QuadWild...");
    HelperProcess::Request fieldAndPatches;
    fieldAndPatches.program = quadwild;
    fieldAndPatches.arguments = {inputPath, QStringLiteral("2"), prepConfig};
    fieldAndPatches.workingDirectory = runtime.resources;
    fieldAndPatches.label = QStringLiteral("quadwild");
    // The banners this helper prints, verified against a real run. Its third phase,
    // quadrangulation, is not one of them: the "2" argument above stops it after
    // tracing, and quad_from_patches takes over from there.
    fieldAndPatches.stageMarkers = {
        QStringLiteral("1 - Remesh and field"),
        QStringLiteral("2 - Tracing")
    };
    fieldAndPatches.progressBegin = 5;
    fieldAndPatches.progressEnd = 55;
    if (!HelperProcess::run(fieldAndPatches, doc, error))
        return fail(error);

    const QString patchMesh =
        QDir(temporary.path()).filePath(QStringLiteral("input_rem_p0.obj"));
    if (!QFileInfo::exists(patchMesh))
        return fail(QObject::tr("QuadWild did not produce the expected patch layout."));

    progress(doc, 55, "Quantizing and tessellating QuadWild patches...");
    // No stage markers: this helper narrates its work but not in phases, so the bar
    // holds while its output streams to the log.
    HelperProcess::Request quantize;
    quantize.program = quadFromPatches;
    quantize.arguments = {patchMesh, QStringLiteral("0"), mainConfig};
    quantize.workingDirectory = runtime.resources;
    quantize.label = QStringLiteral("quad_from_patches");
    if (!HelperProcess::run(quantize, doc, error))
        return fail(error);

    const bool smooth = params.getBool(QStringLiteral("smoothOutput"));
    const QString outputPath = QDir(temporary.path()).filePath(
        smooth ? QStringLiteral("input_rem_p0_0_quadrangulation_smooth.obj")
               : QStringLiteral("input_rem_p0_0_quadrangulation.obj"));
    if (!QFileInfo::exists(outputPath))
        return fail(QObject::tr("QuadWild-BiMDF did not produce the expected output mesh."));

    progress(doc, 95, "Importing QuadWild result...");
    VCGMesh output;
    int loadMask = 0;
    const QByteArray encodedOutput = QFile::encodeName(outputPath);

    // Both LoadMask and Open resolve the relative MTL path against the process
    // working directory. The helper writes its patch colors beside the OBJ.
    const QString previousDirectory = QDir::currentPath();
    QDir::setCurrent(temporary.path());
    vcg::tri::io::ImporterOBJ<VCGMesh>::LoadMask(encodedOutput.constData(), loadMask);
    const int importError = vcg::tri::io::ImporterOBJ<VCGMesh>::Open(
        output, encodedOutput.constData(), loadMask);
    QDir::setCurrent(previousDirectory);
    if (vcg::tri::io::ImporterOBJ<VCGMesh>::ErrorCritical(importError)) {
        return fail(QObject::tr("Cannot import QuadWild output: %1")
                        .arg(QString::fromLatin1(
                            vcg::tri::io::ImporterOBJ<VCGMesh>::ErrorMsg(importError))));
    }
    if (output.VN() == 0 || output.FN() == 0)
        return fail(QObject::tr("QuadWild-BiMDF produced an empty mesh."));

    vcg::tri::UpdateBounding<VCGMesh>::Box(output);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(output);
    loadMask |= Mask::IOM_VERTCOORD | Mask::IOM_FACEINDEX
        | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;

    const int newIndex = doc.addMesh(
        output, QObject::tr("QuadWild-BiMDF - %1").arg(inputName), loadMask);
    if (newIndex < 0)
        return fail(QObject::tr("Failed to add the QuadWild-BiMDF result."));
    doc.setMeshTransform(newIndex, inputTransform);

    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = true;
    result.newMeshIndices.push_back(newIndex);
    if (importError != 0) {
        result.infoMessages << QObject::tr("OBJ import warning: %1")
                                   .arg(QString::fromLatin1(
                                       vcg::tri::io::ImporterOBJ<VCGMesh>::ErrorMsg(importError)));
    }
    result.infoMessages
        << QObject::tr("Created mesh '%1' with QuadWild-BiMDF.")
               .arg(doc.mesh(newIndex).name)
        << QObject::tr("Output: %1 vertices, %2 triangles with polygonal faux edges.")
               .arg(output.VN()).arg(output.FN());
    progress(doc, 100, "QuadWild-BiMDF remeshing complete.");
    return result;
}

void registerQuadWildFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<QuadWildFilterPlugin>());
}
