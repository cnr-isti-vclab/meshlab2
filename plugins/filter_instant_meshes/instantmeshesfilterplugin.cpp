#include "instantmeshesfilterplugin.h"

#include "document.h"
#include "instantmeshes_adapter.h"
#include "meshfilterpluginmanager.h"

#include <wrap/io_trimesh/io_mask.h>

#include <QObject>
#include <algorithm>
#include <memory>
#include <mutex>

namespace {

constexpr QLatin1StringView kFilterId("remesh_to_quads_instant_meshes");
using Mask = vcg::tri::io::Mask;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.errorMessage = message;
    return result;
}

std::mutex &instantMeshesMutex()
{
    static std::mutex mutex;
    return mutex;
}

void progress(Document &doc, int percentage, const char *message)
{
    if (vcg::CallBackPos *callback = doc.progressCallback())
        (*callback)(percentage, message);
}

} // namespace

QString InstantMeshesFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.instant_meshes");
}

QString InstantMeshesFilterPlugin::name() const
{
    return QObject::tr("Instant Meshes Remeshing Filters");
}

MeshFilterRunResult InstantMeshesFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId != kFilterId)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));
    const int currentIndex = doc.currentMeshIndex();
    if (currentIndex < 0)
        return fail(QObject::tr("No current mesh selected."));

    const Document::MeshEntry &input = doc.mesh(currentIndex);
    if (input.mesh.VN() == 0 || input.mesh.FN() == 0)
        return fail(QObject::tr("Instant Meshes requires a non-empty triangle mesh."));

    InstantMeshesParameters options;
    options.targetEdgeLength = float(params.getDouble(QStringLiteral("targetEdgeLength")));
    options.creaseAngleDegrees = params.getBool(QStringLiteral("detectCreases"))
        ? float(params.getDouble(QStringLiteral("creaseAngle"))) : -1.0f;
    options.alignBoundaries = params.getBool(QStringLiteral("alignBoundaries"));
    options.extrinsic = params.getEnum(QStringLiteral("optimizationSpace"))
        != QStringLiteral("intrinsic");
    options.pureQuads = params.getEnum(QStringLiteral("outputTopology"))
        == QStringLiteral("pure_quads");
    options.deterministic = params.getBool(QStringLiteral("deterministic"));
    options.smoothingIterations =
        params.getInt(QStringLiteral("smoothingIterations"));
    options.threads = params.getInt(QStringLiteral("threads"));

    const QString inputName = input.name;
    const QMatrix4x4 inputTransform = input.transform;
    std::lock_guard<std::mutex> lock(instantMeshesMutex());
    try {
        progress(doc, 5, "Converting mesh for Instant Meshes...");
        VCGMesh output;
        QString error;
        if (!runInstantMeshes(input.mesh, output, options, error))
            return fail(error);

        progress(doc, 95, "Adding Instant Meshes result...");
        const int ioMask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL
            | Mask::IOM_FACENORMAL | Mask::IOM_BITPOLYGONAL;
        const int newIndex = doc.addMesh(
            output, QObject::tr("Instant Meshes - %1").arg(inputName), ioMask);
        if (newIndex < 0)
            return fail(QObject::tr("Failed to add the Instant Meshes result."));
        doc.setMeshTransform(newIndex, inputTransform);

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices.push_back(newIndex);
        result.infoMessages
            << QObject::tr("Created '%1' with the original Instant Meshes algorithm.")
                   .arg(doc.mesh(newIndex).name)
            << QObject::tr("Output: %1 vertices and %2 triangulated polygon faces.")
                   .arg(output.VN()).arg(output.FN());
        progress(doc, 100, "Instant Meshes remeshing complete.");
        return result;
    } catch (const std::exception &exception) {
        return fail(QObject::tr("Instant Meshes failed: %1")
            .arg(QString::fromLocal8Bit(exception.what())));
    } catch (...) {
        return fail(QObject::tr("Instant Meshes failed with an unknown error."));
    }
}

void registerInstantMeshesFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<InstantMeshesFilterPlugin>());
}
