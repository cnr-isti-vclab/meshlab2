#include "plymcfilterplugin.h"

#include "document.h"
#include "filterparam.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <vcg/complex/algorithms/create/plymc/plymc.h>
#include <vcg/complex/algorithms/create/plymc/simplemeshprovider.h>
#include <vcg/complex/algorithms/smooth.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/flag.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/position.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/algorithms/geodesic.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/append.h>
#include <vcg/complex/allocate.h>
#include <wrap/io_trimesh/export_vmi.h>
#include <wrap/io_trimesh/import_ply.h>
#include <wrap/io_trimesh/io_mask.h>

#include <QDir>
#include <QTemporaryDir>

namespace {

constexpr QLatin1StringView kRecon("reconstruct_surface_by_volumetric_merging");
constexpr QLatin1StringView kMCSimplify("simplify_marching_cubes_mesh_by_edge_collapse");

MeshFilterRunResult fail(const QString &m) { return {false, false, m}; }
MeshFilterRunResult ok(const QStringList &info = {}) {
    MeshFilterRunResult r; r.success = true; r.documentModified = true;
    r.infoMessages = info; return r;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

QString PlyMCFilterPlugin::pluginId() const
{ return QStringLiteral("meshlab2.filter.plymc"); }

QString PlyMCFilterPlugin::name() const
{ return QStringLiteral("PlyMC Reconstruction Filters"); }

MeshFilterRunResult PlyMCFilterPlugin::runFilter(
    const QString &fid, const FilterParams &p, Document &doc) const
{
    using namespace vcg::tri::io;

    // --- MC Simplify ---
    if (fid == QString::fromLatin1(kMCSimplify)) {
        int mi = doc.currentMeshIndex();
        if (mi < 0) return fail(QObject::tr("No current mesh."));
        VCGMesh &m = doc.mesh(mi).mesh;
        if (m.FN() == 0) return fail(QObject::tr("Cannot simplify: no faces."));

        float cellError = float(p.getDouble(QStringLiteral("cellError"), 0.25f));
        bool preserveBB = p.getBool(QStringLiteral("preserveBB"), false);

        // Detect MC grid spacing
        std::vector<float> zSet;
        for (const auto &f : m.face) {
            if (f.IsD()) continue;
            auto v0 = f.V(0)->P()[2], v1 = f.V(1)->P()[2], v2 = f.V(2)->P()[2];
            auto p0 = f.V(0)->P(), p1 = f.V(1)->P(), p2 = f.V(2)->P();
            if (v0 == v1 && p0[1] != p1[1] && p0[0] != p1[0]) zSet.push_back(v0);
            if (v0 == v2 && p0[1] != p2[1] && p0[0] != p2[0]) zSet.push_back(v0);
            if (v1 == v2 && p1[1] != p2[1] && p1[0] != p2[0]) zSet.push_back(v1);
            if (zSet.size() > 100) break;
        }
        if (zSet.empty())
            return fail(QObject::tr("Cannot detect MC grid spacing. Not an MC mesh."));

        std::sort(zSet.begin(), zSet.end());
        auto last = std::unique(zSet.begin(), zSet.end());
        zSet.resize(size_t(last - zSet.begin()));
        float cellSize = 0;
        for (size_t i = 0; i < zSet.size() - 1; ++i)
            cellSize = std::max(zSet[i + 1] - zSet[i], cellSize);

        float absErr = (cellError > 0.0f) ? cellError * cellSize : cellSize / 4.0f;
        doc.writeLog(QObject::tr("MC cell size: %1, error: %2 (cell unit: %3)")
            .arg(cellSize, 0, 'f', 6).arg(absErr, 0, 'f', 6).arg(cellError, 0, 'f', 3),
            Document::LogSource::Application);

        int result = tri::MCSimplify<VCGMesh>(m, absErr, preserveBB);
        if (result != 1)
            return fail(QObject::tr("Cannot simplify: this is not a Marching Cube-generated mesh."));

        tri::Allocator<VCGMesh>::CompactEveryVector(m);
        float flipThreshold = float(p.getDouble(QStringLiteral("flipThreshold"), 10.0));
        tri::Clean<VCGMesh>::RemoveTVertexByFlip(m, flipThreshold, true);
        tri::Clean<VCGMesh>::RemoveFaceFoldByFlip(m);
        tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(m);

        auto &ent = doc.mesh(mi);
        ent.ioMask |= Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_FACENORMAL;
        doc.markMeshGeometryChanged(mi, QObject::tr("Simplified MC mesh '%1'.").arg(ent.name));
        return ok({QObject::tr("MC simplification done.")});
    }

    // --- Reconstruct Surface by Volumetric Merging ---
    if (fid == QString::fromLatin1(kRecon)) {
        // Collect visible meshes
        std::vector<int> visibleMeshes;
        for (int mi = 0; mi < doc.meshCount(); ++mi)
            if (doc.mesh(mi).visible)
                visibleMeshes.push_back(mi);

        if (visibleMeshes.empty())
            return fail(QObject::tr("No visible meshes to reconstruct from."));

        // Save old dir and switch to temp dir
        const QString oldDir = QDir::currentPath();
        QTemporaryDir tmpDir;
        if (!tmpDir.isValid())
            return fail(QObject::tr("Cannot create temporary directory."));
        QDir::setCurrent(tmpDir.path());

        // Parameters
        float voxSizePerc = float(p.getDouble(QStringLiteral("voxSize"), 0.01));
        int subdiv = p.getInt(QStringLiteral("subdiv"), 1);
        float geodesic = float(p.getDouble(QStringLiteral("geodesic"), 2.0));
        int smoothNum = p.getInt(QStringLiteral("smoothNum"), 1);
        int wideNum = p.getInt(QStringLiteral("wideNum"), 3);
        bool mergeColor = p.getBool(QStringLiteral("mergeColor"), false);
        bool simplification = p.getBool(QStringLiteral("simplification"), false);
        int normalSmooth = p.getInt(QStringLiteral("normalSmooth"), 3);

        tri::PlyMC<SMesh, SimpleMeshProvider<SMesh>> pmc;
        pmc.MP.setCacheSize(64);
        auto &pmcp = pmc.p;

        pmcp.IDiv = vcg::Point3i(subdiv, subdiv, subdiv);
        pmcp.IPosS = vcg::Point3i(0, 0, 0);
        pmcp.IPosE[0] = pmcp.IDiv[0] - 1;
        pmcp.IPosE[1] = pmcp.IDiv[1] - 1;
        pmcp.IPosE[2] = pmcp.IDiv[2] - 1;

        pmcp.VoxSize = voxSizePerc;
        pmcp.QualitySmoothVox = geodesic;
        pmcp.SmoothNum = smoothNum;
        pmcp.WideNum = wideNum;
        pmcp.NCell = 0;
        pmcp.FullyPreprocessedFlag = true;
        pmcp.MergeColor = mergeColor;
        pmcp.VertSplatFlag = mergeColor;
        pmcp.SimplificationFlag = simplification;

        // Preprocess each visible mesh
        for (int mi : visibleMeshes) {
            VCGMesh &mesh = doc.mesh(mi).mesh;
            auto &ent = doc.mesh(mi);

            SMesh sm;
            tri::Append<SMesh, VCGMesh>::Mesh(sm, mesh);
            vcg::Matrix44f mtx;
            const float *d = ent.transform.constData();
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    mtx[r][c] = d[size_t(c * 4 + r)];
            tri::UpdatePosition<SMesh>::Matrix(sm, mtx, true);
            tri::UpdateBounding<SMesh>::Box(sm);
            tri::UpdateNormal<SMesh>::NormalizePerVertex(sm);
            tri::UpdateTopology<SMesh>::VertexFace(sm);
            tri::UpdateFlags<SMesh>::VertexBorderFromNone(sm);
            tri::Geodesic<SMesh>::DistanceFromBorder(sm);
            for (int i = 0; i < normalSmooth; ++i)
                tri::Smooth<SMesh>::FaceNormalLaplacianVF(sm);

            // Ensure face quality is available
            for (auto &f : sm.face)
                f.Q() = float(1);

            QString vmiPath = QStringLiteral("__TMP%1.vmi").arg(ent.name);
            int ret = tri::io::ExporterVMI<SMesh>::Save(sm, qPrintable(vmiPath));
            if (ret != 0)
                return fail(QObject::tr("Failed to write VMI temp file: %1").arg(vmiPath));
            pmc.MP.AddSingleMesh(qPrintable(vmiPath));

            doc.writeLog(QObject::tr("Preprocessed mesh '%1' for reconstruction.").arg(ent.name),
                         Document::LogSource::Application);
        }

        // Run reconstruction
        if (!pmc.Process(nullptr))
            return fail(QObject::tr("Reconstruction failed: %1").arg(
                QString::fromStdString(pmc.errorMessage)));

        // Load results as new meshes
        int loadedCount = 0;
        for (size_t i = 0; i < pmcp.OutNameVec.size(); ++i) {
            const std::string &outName = pmcp.SimplificationFlag
                ? pmcp.OutNameSimpVec[i] : pmcp.OutNameVec[i];

            VCGMesh outMesh;
            int loadMask = -1;
            int ret = tri::io::ImporterPLY<VCGMesh>::Open(outMesh, outName.c_str(), loadMask);
            if (ret != 0) continue;

            tri::UpdateBounding<VCGMesh>::Box(outMesh);
            tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(outMesh);

            QString meshName = QStringLiteral("Reconstruction_%1").arg(i);
            int newIdx = doc.addMesh(outMesh, meshName, loadMask);
            if (newIdx >= 0) {
                auto &newEnt = doc.mesh(newIdx);
                newEnt.ioMask |= Mask::IOM_VERTQUALITY;
                if (pmcp.MergeColor || pmcp.VertSplatFlag)
                    newEnt.ioMask |= Mask::IOM_VERTCOLOR;
                ++loadedCount;
            }
        }

        // Clean up VMI temp files
        for (int mi : visibleMeshes) {
            const auto &ent = doc.mesh(mi);
            QFile::remove(QStringLiteral("__TMP%1.vmi").arg(ent.name));
        }

        // Restore current directory
        QDir::setCurrent(oldDir);

        if (loadedCount == 0)
            return fail(QObject::tr("Reconstruction produced no output."));

        doc.writeLog(QObject::tr("Loaded %1 reconstructed meshes.").arg(loadedCount),
                     Document::LogSource::Application);
        return ok({QObject::tr("Reconstructed %1 mesh(es).").arg(loadedCount)});
    }

    return fail(QObject::tr("Unknown filter: %1").arg(fid));
}

void registerPlyMCFilterPlugin(MeshFilterPluginManager &pm)
{
    pm.registerPlugin(std::make_unique<PlyMCFilterPlugin>());
}
