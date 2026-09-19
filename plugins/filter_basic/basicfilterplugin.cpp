#include "basicfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include <wrap/io_trimesh/io_mask.h>
#include <vcg/math/perlin_noise.h>
#include <vcg/complex/allocate.h>
#include <vcg/complex/algorithms/create/marching_cubes.h>
#include <vcg/complex/algorithms/create/mc_trivial_walker.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
constexpr QLatin1StringView kFilterMeshInfo("measure_mesh_summary");
constexpr QLatin1StringView kFilterCreateNoisyIso("create_isosurface_from_perlin_noise");

}

QString BasicFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.basic");
}

QString BasicFilterPlugin::name() const
{
    return QObject::tr("Basic Filters");
}

MeshFilterRunResult BasicFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kFilterMeshInfo)) {
        const int meshIndex = doc.currentMeshIndex();
        if (meshIndex < 0 || meshIndex >= doc.meshCount()) {
            return { false, false, QObject::tr("No current mesh selected.") };
        }

        const int precision = std::clamp(params.getInt(QStringLiteral("precision")), 0, 8);
        const Document::MeshEntry &entry = doc.mesh(meshIndex);
        const vcg::Box3f &bbox = entry.mesh.bbox;
        const QString diag = QString::number(bbox.Diag(), 'f', precision);

        QStringList info;
        info << QObject::tr("Mesh: %1").arg(entry.name);
        info << QObject::tr("Vertices: %1").arg(entry.mesh.VN());
        info << QObject::tr("Edges: %1").arg(entry.mesh.EN());
        info << QObject::tr("Faces: %1").arg(entry.mesh.FN());
        info << QObject::tr("BBox diagonal: %1").arg(diag);
        info << QObject::tr("BBox min: (%1, %2, %3)")
                    .arg(QString::number(bbox.min.X(), 'f', precision))
                    .arg(QString::number(bbox.min.Y(), 'f', precision))
                    .arg(QString::number(bbox.min.Z(), 'f', precision));
        info << QObject::tr("BBox max: (%1, %2, %3)")
                    .arg(QString::number(bbox.max.X(), 'f', precision))
                    .arg(QString::number(bbox.max.Y(), 'f', precision))
                    .arg(QString::number(bbox.max.Z(), 'f', precision));

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = false;
        result.infoMessages = info;
        return result;
    }

    if (filterId == QString::fromLatin1(kFilterCreateNoisyIso)) {
        const int gridSize = std::clamp(params.getInt(QStringLiteral("resolution")), 8, 256);

        using Scalar = float;
        using VolumeType = vcg::SimpleVolume<vcg::SimpleVoxel<Scalar>>;
        using WalkerType = vcg::tri::TrivialWalker<VCGMesh, VolumeType>;
        using MarchingCubesType = vcg::tri::MarchingCubes<VCGMesh, WalkerType>;

        VolumeType volume;
        volume.Init(
            vcg::Point3i(gridSize, gridSize, gridSize),
            vcg::Box3f(vcg::Point3f(0.0f, 0.0f, 0.0f), vcg::Point3f(1.0f, 1.0f, 1.0f)));

        const float halfGrid = float(gridSize) * 0.5f;
        const float perlinFreq = 0.2f;
        const float perlinAmp = float(gridSize) * 0.2f;
        for (int i = 0; i < gridSize; ++i) {
            for (int j = 0; j < gridSize; ++j) {
                for (int k = 0; k < gridSize; ++k) {
                    const float fy = float(j) - halfGrid;
                    const float fz = float(k) - halfGrid;
                    const float noise = float(vcg::math::Perlin::Noise(
                        double(i) * perlinFreq,
                        double(j) * perlinFreq,
                        double(k) * perlinFreq));
                    volume.Val(i, j, k) = fy * fy + fz * fz + float(i) * perlinAmp * noise;
                }
            }
        }

        VCGMesh generatedMesh;
        WalkerType walker;
        MarchingCubesType mc(generatedMesh, walker);
        // Keep historical noisy-isosurface appearance by extracting at the
        // same threshold that was previously used in this filter.
        const float isoThreshold = float(std::max(16, (gridSize * gridSize) / 10));
        walker.BuildMesh<MarchingCubesType>(
            generatedMesh,
            volume,
            mc,
            isoThreshold,
            nullptr);

        if (generatedMesh.VN() <= 0 || generatedMesh.FN() <= 0) {
            return { false, false, QObject::tr("Noisy isosurface generation produced an empty mesh.") };
        }

        vcg::tri::Allocator<VCGMesh>::CompactEveryVector(generatedMesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(generatedMesh);
        vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(generatedMesh);

        const int ioMask =
            vcg::tri::io::Mask::IOM_VERTNORMAL
            | vcg::tri::io::Mask::IOM_FACENORMAL;
        const int newIndex = doc.addMesh(generatedMesh, {}, ioMask);
        if (newIndex < 0)
            return { false, false, QObject::tr("Failed to add generated isosurface to document.") };

        MeshFilterRunResult result;
        result.success = true;
        result.documentModified = true;
        result.newMeshIndices = { newIndex };
        result.infoMessages = {
            QObject::tr("Generated '%1' at resolution %2 (%3 vertices, %4 faces)")
                .arg(doc.mesh(newIndex).name)
                .arg(gridSize)
                .arg(doc.mesh(newIndex).mesh.VN())
                .arg(doc.mesh(newIndex).mesh.FN())
        };
        return result;
    }

    return { false, false, QObject::tr("Unknown filter id: %1").arg(filterId) };
}

void registerBasicFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<BasicFilterPlugin>());
}
