#include "screenedpoissonfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "poissonrecon_backend.h"

#include "poissonrecon_adapter.h"

namespace {
constexpr QLatin1StringView kFilterScreenedPoisson("reconstruct_surface_by_screened_poisson");
constexpr QLatin1StringView kFilterSSDRecon("reconstruct_surface_by_smooth_signed_distance");
constexpr QLatin1StringView kFilterSurfaceTrimmer("trim_surface_by_scalar_isovalue");

std::vector<int> selectedMeshIndices(const Document &doc, bool mergeVisible)
{
    std::vector<int> indices;
    if (!mergeVisible) {
        const int currentIndex = doc.currentMeshIndex();
        if (currentIndex >= 0 && currentIndex < doc.meshCount())
            indices.push_back(currentIndex);
        return indices;
    }

    indices.reserve(doc.meshCount());
    for (int i = 0; i < doc.meshCount(); ++i) {
        if (doc.mesh(i).visible)
            indices.push_back(i);
    }
    return indices;
}

QString noNormalsMessage()
{
    return QObject::tr(
        "Filter requires oriented points, and none of the input vertices has a usable "
        "(non-null) normal.\n\n"
        "On a point cloud, compute and orient the normals first (Compute Point Cloud Normals).");
}
}

QString ScreenedPoissonFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.screened_poisson");
}

QString ScreenedPoissonFilterPlugin::name() const
{
    return QObject::tr("Screened Poisson Reconstruction Filters");
}

MeshFilterRunResult ScreenedPoissonFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kFilterSurfaceTrimmer)) {
        const int meshIndex = doc.currentMeshIndex();
        if (meshIndex < 0 || meshIndex >= doc.meshCount())
            return { false, false, QObject::tr("No current mesh selected.") };
        return ScreenedPoisson::runSurfaceTrimmerFilter(doc, meshIndex, params.rawValues());
    }

    if (filterId == QString::fromLatin1(kFilterScreenedPoisson) || filterId == QString::fromLatin1(kFilterSSDRecon)) {
        const bool mergeVisible = params.getBool(QStringLiteral("visibleLayer"));
        const std::vector<int> meshIndices = selectedMeshIndices(doc, mergeVisible);
        if (meshIndices.empty()) {
            return {
                false,
                false,
                mergeVisible
                    ? QObject::tr("No visible meshes available for reconstruction.")
                    : QObject::tr("No current mesh selected.")
            };
        }

        // The layers are only read. A point without a normal -- the vertices of a triangle mesh that
        // belong to no face have none -- is not fed to the reconstruction, and is not removed from
        // the layer either.
        const qsizetype usablePoints = ScreenedPoisson::countInputSamples(doc, meshIndices);
        const qsizetype withoutNormal = ScreenedPoisson::countPointsWithoutNormal(doc, meshIndices);
        if (usablePoints == 0)
            return { false, false, noNormalsMessage() };

        MeshFilterRunResult result =
            (filterId == QString::fromLatin1(kFilterSSDRecon))
                ? ScreenedPoisson::runSSDReconFilter(doc, meshIndices, mergeVisible, params.rawValues())
                : ScreenedPoisson::runScreenedPoissonFilter(doc, meshIndices, mergeVisible, params.rawValues());
        // Reconstruction merges however many layers were visible, treating them alike, so
        // the framework names the result after the set rather than after any one of them.
        if (result.success) {
            for (int meshIndex : meshIndices)
                result.sourceMeshIndices.push_back(meshIndex);
            if (withoutNormal > 0)
                result.infoMessages.push_back(
                    QObject::tr("%1 points without a normal were left out.").arg(withoutNormal));
        }
        return result;
    }

    return { false, false, QObject::tr("Unknown filter id: %1").arg(filterId) };
}

void registerScreenedPoissonFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<ScreenedPoissonFilterPlugin>());
}
