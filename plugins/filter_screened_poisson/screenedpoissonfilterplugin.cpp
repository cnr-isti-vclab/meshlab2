#include "screenedpoissonfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "poissonrecon_backend.h"

#include <algorithm>
#include <limits>
#include <thread>

#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/update/normal.h>

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

QString invalidNormalsMessage()
{
    return QObject::tr(
        "Filter requires correct per-vertex normals.\n"
        "All input vertices must have a proper, non-null normal.\n\n"
        "Try enabling the Pre-Clean option and retry.\n\n"
        "To permanently remove this problem:\n"
        "- on triangulated meshes, use Remove Unreferenced Vertices\n"
        "- on point clouds, use Select Vertices by Expression with\n"
        "  (nx==0.0) && (ny==0.0) && (nz==0.0)\n"
        "  and then delete the selected vertices.");
}

template<class MeshType>
void cleanInputMesh(MeshType &mesh, bool scaleNormalByQuality, bool cleanFlag)
{
    vcg::tri::UpdateNormal<MeshType>::NormalizePerVertex(mesh);

    if (cleanFlag) {
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
            if (vcg::SquaredNorm(vi->N()) < std::numeric_limits<float>::min() * 10.0f)
                vcg::tri::Allocator<MeshType>::DeleteVertex(mesh, *vi);
        }

        for (auto fi = mesh.face.begin(); fi != mesh.face.end(); ++fi) {
            if (fi->V(0)->IsD() || fi->V(1)->IsD() || fi->V(2)->IsD())
                vcg::tri::Allocator<MeshType>::DeleteFace(mesh, *fi);
        }
    }

    vcg::tri::Allocator<MeshType>::CompactEveryVector(mesh);
    if (scaleNormalByQuality) {
        for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi)
            vi->N() *= vi->Q();
    }
}

bool hasGoodNormals(VCGMesh &mesh)
{
    for (auto vi = mesh.vert.begin(); vi != mesh.vert.end(); ++vi) {
        if (vcg::SquaredNorm(vi->N()) < std::numeric_limits<float>::min() * 10.0f)
            return false;
    }
    return true;
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

        const bool confidence = params.getBool(QStringLiteral("confidence"));
        const bool preClean = params.getBool(QStringLiteral("preClean"));

        for (int meshIndex : meshIndices) {
            if (meshIndex < 0 || meshIndex >= doc.meshCount())
                continue;
            Document::MeshEntry &entry = doc.mesh(meshIndex);
            cleanInputMesh(entry.mesh, confidence, preClean);
            if (!hasGoodNormals(entry.mesh))
                return { false, false, invalidNormalsMessage() };

            if (preClean) {
                doc.markMeshGeometryChanged(meshIndex);
            } else {
                doc.markMeshMaterialChanged(meshIndex);
            }
        }

        MeshFilterRunResult result =
            (filterId == QString::fromLatin1(kFilterSSDRecon))
                ? ScreenedPoisson::runSSDReconFilter(doc, meshIndices, mergeVisible, params.rawValues())
                : ScreenedPoisson::runScreenedPoissonFilter(doc, meshIndices, mergeVisible, params.rawValues());
        // Reconstruction merges however many layers were visible, treating them alike, so
        // the framework names the result after the set rather than after any one of them.
        if (result.success) {
            for (int meshIndex : meshIndices)
                result.sourceMeshIndices.push_back(meshIndex);
        }
        return result;
    }

    return { false, false, QObject::tr("Unknown filter id: %1").arg(filterId) };
}

void registerScreenedPoissonFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<ScreenedPoissonFilterPlugin>());
}
