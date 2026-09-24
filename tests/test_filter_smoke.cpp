#include <QtTest/QtTest>

#include <QImage>

#include "document.h"

#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/selection.h>

#include <wrap/io_trimesh/io_mask.h>

#include <cstdint>
#include <cmath>

// Smoke sweep: every SingleMesh filter, run with its default parameters, one row
// per filter.  The contract is not "produces the right answer" -- that is what
// the behavioural tests in test_filters.cpp are for -- it is the weaker but
// completely uncovered one:
//
//     a filter either runs, or refuses with an explanation. Never anything else.
//
// "Anything else" means crashing, hanging, or returning success while producing
// nothing, all of which have happened here and none of which any suite noticed:
// sample_offset_surface_recursively spent its whole life aborting on a missing
// OCF component, hidden behind a parameter range that made it unreachable.
//
// Filters are not all runnable on the same input, so each one is tried against a
// ladder of fixtures, poorest first, and the first that works is the answer. The
// ladder is ordered so that the level a filter needs *is* its real input
// requirement, which makes this suite's own output the evidence for declaring
// those requirements in the manifests (see docs/design/history/filter_classification.md).
//
// A filter that fails on every rung must be listed in kExpectedRefusals with a
// reason. That table is the to-do list, not a suppression file: a listed filter
// that starts passing fails the run too, so the table cannot rot.

namespace {

// Ordered poorest to richest. Not a chain of supersets -- Open swaps the closed
// sphere for a bounded grid -- but a search order.
enum class Fixture {
    Bare,          // vertices, faces, normals
    Attributed,    // + vertex/face colour and a varying quality range
    Parametrized,  // + per-vertex and per-wedge texture coordinates
    Textured,      // + an associated texture image
    Selected,      // + every vertex and face selected
    Open,          // a bounded grid instead of a closed sphere, otherwise as Selected
    Rasters,       // a real photogrammetry project: scan mesh + four calibrated views
    Polyline       // an edge mesh: vertices and edges, no faces
};

const std::array<Fixture, 8> &fixtureLadder()
{
    static const std::array<Fixture, 8> ladder = {
        Fixture::Bare, Fixture::Attributed, Fixture::Parametrized,
        Fixture::Textured, Fixture::Selected, Fixture::Open, Fixture::Rasters,
        Fixture::Polyline
    };
    return ladder;
}

QString fixtureName(Fixture f)
{
    switch (f) {
    case Fixture::Bare:         return QStringLiteral("bare");
    case Fixture::Attributed:   return QStringLiteral("attributed");
    case Fixture::Parametrized: return QStringLiteral("parametrized");
    case Fixture::Textured:     return QStringLiteral("textured");
    case Fixture::Selected:     return QStringLiteral("selected");
    case Fixture::Open:         return QStringLiteral("open");
    case Fixture::Rasters:      return QStringLiteral("rasters");
    case Fixture::Polyline:     return QStringLiteral("polyline");
    }
    return QStringLiteral("?");
}

int ioMaskFor(Fixture f)
{
    using vcg::tri::io::Mask;
    // Not a rung of the ladder but a different shape of mesh, so it declares its own
    // set rather than extending the surface one: no faces, but real edges carrying
    // colour of their own.
    if (f == Fixture::Polyline) {
        return Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL | Mask::IOM_VERTCOLOR
             | Mask::IOM_VERTQUALITY | Mask::IOM_EDGEINDEX | Mask::IOM_EDGECOLOR
             | Mask::IOM_EDGEQUALITY;
    }
    int mask = Mask::IOM_VERTCOORD | Mask::IOM_VERTNORMAL
             | Mask::IOM_FACEINDEX | Mask::IOM_FACENORMAL;
    if (f == Fixture::Bare)
        return mask;
    mask |= Mask::IOM_VERTCOLOR | Mask::IOM_VERTQUALITY
          | Mask::IOM_FACECOLOR | Mask::IOM_FACEQUALITY;
    if (f == Fixture::Attributed)
        return mask;
    mask |= Mask::IOM_VERTTEXCOORD | Mask::IOM_WEDGTEXCOORD;
    return mask;
}

// Everything above plain geometry: colour, a quality range, UVs, selection. Split
// out of the geometry so the loaded photogrammetry mesh can be dressed the same way
// as the generated ones and the raster rung stays a superset of the others.
void decorateFixtureMesh(VCGMesh &mesh, Fixture f);

void buildFixtureMesh(VCGMesh &mesh, Fixture f)
{
    mesh.Clear();
    if (f == Fixture::Polyline) {
        // Two disjoint chains, because a single one cannot tell a filter that walks
        // connected components from one that just iterates edges. The helix keeps
        // every edge a different length and direction, so length- and
        // direction-based expressions produce a range rather than one value.
        constexpr int kPerChain = 12;
        for (int chain = 0; chain < 2; ++chain) {
            const int base = int(mesh.vert.size());
            vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, kPerChain);
            for (int i = 0; i < kPerChain; ++i) {
                const float t = float(i) / float(kPerChain - 1);
                const float angle = t * 3.0f * float(M_PI) + float(chain);
                mesh.vert[std::size_t(base + i)].P() = vcg::Point3f(
                    std::cos(angle) * (0.4f + 0.6f * t),
                    std::sin(angle) * (0.4f + 0.6f * t),
                    -1.0f + 2.0f * t + 0.3f * float(chain));
            }
            vcg::tri::Allocator<VCGMesh>::AddEdges(mesh, kPerChain - 1);
            for (int i = 0; i < kPerChain - 1; ++i) {
                auto &e = mesh.edge[std::size_t(mesh.edge.size()) - std::size_t(kPerChain - 1 - i)];
                e.V(0) = &mesh.vert[std::size_t(base + i)];
                e.V(1) = &mesh.vert[std::size_t(base + i + 1)];
                e.C() = vcg::Color4b(40 + 15 * i, 90, 200 - 10 * i, 255);
                e.Q() = float(i) / float(kPerChain - 2);
            }
        }
        decorateFixtureMesh(mesh, f);
        return;
    }
    if (f == Fixture::Open) {
        vcg::tri::Grid<VCGMesh>(mesh, 12, 12, 2.0f, 2.0f);
        // Domed rather than flat. A perfectly coplanar patch is a degenerate input
        // for anything that builds a 3D triangulation of the vertices -- CGAL's
        // alpha shape segfaults on one -- and no real open mesh is planar, so the
        // flat version would be testing a case the ladder does not mean to cover.
        for (auto &v : mesh.vert) {
            const float x = v.P().X() - 1.0f;
            const float y = v.P().Y() - 1.0f;
            v.P().Z() = 0.35f * (1.0f - x * x - y * y);
        }
    } else {
        vcg::tri::Sphere(mesh, 3);
        // Dithered, for the same reason the open fixture is domed rather than flat. An
        // analytic sphere puts every vertex exactly on one sphere, which is degenerate
        // for anything building a 3D triangulation: CGAL's Poisson surface mesher spins
        // forever on it, taking the whole suite past its watchdog. A millionth of the
        // radius is far below what any filter can be measuring and breaks the symmetry.
        // Deterministic, so the fixture is the same mesh on every run.
        for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
            std::uint32_t h = std::uint32_t(i) * 2654435761u;
            const auto next = [&h]() {
                h ^= h >> 15;
                h *= 2246822519u;
                h ^= h >> 13;
                return float(double(h) / 2147483647.5 - 1.0);
            };
            mesh.vert[i].P() += vcg::Point3f(next(), next(), next()) * 1e-6f;
        }
    }

    decorateFixtureMesh(mesh, f);
}

void decorateFixtureMesh(VCGMesh &mesh, Fixture f)
{
    if (f >= Fixture::Parametrized) {
        mesh.vert.EnableTexCoord();
        mesh.face.EnableWedgeTexCoord();
    }

    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);

    const float spanX = std::max(1e-6f, mesh.bbox.DimX());
    for (auto &v : mesh.vert) {
        if (f >= Fixture::Attributed) {
            v.C() = vcg::Color4b(200, 180, 160, 255);
            // A genuine interval, so @qualityVMin/@qualityVMax bind to a range
            // rather than to a single value.
            v.Q() = (v.P().X() - mesh.bbox.min.X()) / spanX;
        }
        if (f >= Fixture::Parametrized) {
            v.T().U() = 0.5f + 0.4f * v.P().X();
            v.T().V() = 0.5f + 0.4f * v.P().Y();
        }
    }
    const size_t faceCount = mesh.face.size();
    const int cellsPerSide = std::max(1, int(std::ceil(std::sqrt(double(faceCount)))));
    const float cellStep = 1.0f / float(cellsPerSide);
    for (size_t i = 0; i < faceCount; ++i) {
        auto &face = mesh.face[i];
        if (f >= Fixture::Attributed) {
            face.C() = vcg::Color4b(160, 180, 200, 255);
            face.Q() = float(i) / float(std::max<size_t>(1, faceCount - 1));
        }
        if (f >= Fixture::Parametrized) {
            // A trivial per-triangle layout: each face gets its own cell of a square
            // grid. Giving every face the same full-size UV triangle instead would
            // be a parametrization no mesh ever has -- hundreds of identical
            // overlapping charts -- and it turns atlas packing into a problem that
            // takes minutes to solve.
            const int cell = int(i % size_t(cellsPerSide));
            const int row = int(i / size_t(cellsPerSide));
            const float u = float(cell) * cellStep;
            const float v = float(row) * cellStep;
            const float inner = cellStep * 0.8f;
            face.WT(0) = VCGFace::TexCoordType(u, v);
            face.WT(1) = VCGFace::TexCoordType(u + inner, v);
            face.WT(2) = VCGFace::TexCoordType(u, v + inner);
            // Half the faces reference the second texture, so the multi-texture
            // paths (atlas packing, defragmentation) get a real atlas rather than
            // the single-image special case they all short-circuit on.
            const short slot = (f >= Fixture::Textured && i >= faceCount / 2) ? 1 : 0;
            for (int corner = 0; corner < 3; ++corner)
                face.WT(corner).N() = slot;
        }
    }

    if (f >= Fixture::Selected) {
        vcg::tri::UpdateSelection<VCGMesh>::VertexAll(mesh);
        vcg::tri::UpdateSelection<VCGMesh>::FaceAll(mesh);
        // Edge elements carry their own selection bit, so the "only on selection"
        // path of the edge filters is unreachable without this.
        vcg::tri::UpdateSelection<VCGMesh>::EdgeAll(mesh);
        // Per-face edge selection is a separate set of bits from face selection, and
        // the filters that read it (polyline extraction, crease handling) see nothing
        // if only faces are selected.
        for (auto &face : mesh.face) {
            for (int corner = 0; corner < 3; ++corner)
                face.SetFaceEdgeS(corner);
        }
    } else {
        vcg::tri::UpdateSelection<VCGMesh>::VertexClear(mesh);
        vcg::tri::UpdateSelection<VCGMesh>::FaceClear(mesh);
        vcg::tri::UpdateSelection<VCGMesh>::EdgeClear(mesh);
    }
}

void attachTexture(Document::MeshEntry &entry, int slot, const QColor &fill)
{
    QImage image(64, 64, QImage::Format_RGBA8888);
    image.fill(fill);
    MeshIOTextureAsset asset;
    asset.name = QStringLiteral("fixture%1.png").arg(slot);
    asset.image = image;
    entry.textureAssets.push_back(asset);
    entry.textureFileNames << asset.name;
    entry.mesh.textures.push_back(asset.name.toStdString());
}

void attachTextures(Document::MeshEntry &entry)
{
    attachTexture(entry, 0, QColor(180, 140, 100));
    attachTexture(entry, 1, QColor(100, 140, 180));
}

// Two layers: filters taking a second mesh parameter default to the other one,
// and the copy is offset by half a radius so booleans and intersection curves
// have a genuine overlap to work with rather than a coincident twin.
std::unique_ptr<Document> buildDocument(Fixture f)
{
    auto doc = std::make_unique<Document>();
    doc->setSuppressUndo(true);

    if (f == Fixture::Rasters) {
        // A real photogrammetry project: a decimated scan of the gargoyle plus four
        // calibrated views of it, spread ~100 degrees apart. Generated geometry
        // cannot stand in here -- the shots are calibrated against this mesh in this
        // coordinate frame, and the filters that need them project through them for
        // real.
        const QString project =
            QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/gargoyle_small/gargoyle.mlp");
        if (doc->loadMeshLabProject(project) != 0 || doc->meshCount() == 0)
            return doc;
        // Dressed like the richest generated rung, so a filter reaching this rung
        // wanted the rasters and not some attribute the earlier rungs carry.
        decorateFixtureMesh(doc->mesh(0).mesh, Fixture::Selected);
        doc->mesh(0).ioMask |= ioMaskFor(f);
        doc->markMeshGeometryChanged(0);
        attachTextures(doc->mesh(0));

        VCGMesh offsetCopy;
        offsetCopy.vert.EnableTexCoord();
        offsetCopy.face.EnableWedgeTexCoord();
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopyConst(offsetCopy, doc->mesh(0).mesh);
        vcg::tri::UpdateBounding<VCGMesh>::Box(offsetCopy);
        const float shift = 0.5f * offsetCopy.bbox.DimX();
        for (auto &v : offsetCopy.vert)
            v.P().X() += shift;
        vcg::tri::UpdateBounding<VCGMesh>::Box(offsetCopy);
        const int offset =
            doc->addMesh(offsetCopy, QStringLiteral("fixture offset"), ioMaskFor(f));
        attachTextures(doc->mesh(offset));
        doc->setCurrentMeshIndex(0);
        return doc;
    }

    VCGMesh mesh;
    buildFixtureMesh(mesh, f);
    const int first = doc->addMesh(mesh, QStringLiteral("fixture"), ioMaskFor(f));

    for (auto &v : mesh.vert)
        v.P().X() += 0.5f;
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    const int second = doc->addMesh(mesh, QStringLiteral("fixture offset"), ioMaskFor(f));

    if (f >= Fixture::Textured) {
        attachTextures(doc->mesh(first));
        attachTextures(doc->mesh(second));
    }
    doc->setCurrentMeshIndex(first);
    return doc;
}

// Filters that cannot be driven by any rung of the ladder, each with the reason.
// Every entry is a gap in what the fixtures or the descriptors can express, not an
// excuse. The raster rung emptied out two whole groups that used to live here -- the
// thirteen camera/projection filters, and three reconstructors whose defaults are
// sized for scan data and produced nothing on an analytic sphere.
const QHash<QString, QString> &expectedRefusals()
{
    static const QHash<QString, QString> table = {

        // Needs a mesh shape the ladder does not build.
        {QStringLiteral("create_polyline_from_self_intersections_trueform"), QStringLiteral("needs a self-intersecting mesh")},

        // Needs a value no default can supply: a formula, a name, a viewport.
        {QStringLiteral("compute_vertex_normals_by_expression"),           QStringLiteral("needs an expression")},
        {QStringLiteral("rename_current_mesh_layer"),                      QStringLiteral("needs a new name")},
        {QStringLiteral("project_vertices_onto_line_of_sight"),            QStringLiteral("needs an attribute name")},
        {QStringLiteral("select_by_screen_rectangle"),                     QStringLiteral("needs a live camera state")},
        {QStringLiteral("reconstruct_surface_by_ball_pivoting_gruber"),    QStringLiteral("needs a ball radius; this backend has no guess of its own, unlike the vcglib one")},


        // Needs a prior filter's output on the layer, which the ladder does not build.
        {QStringLiteral("measure_abstract_domain"),                        QStringLiteral("needs an abstract domain from Parametrize by Abstract Domain")},
        {QStringLiteral("remesh_by_abstract_domain"),                      QStringLiteral("needs an abstract domain from Parametrize by Abstract Domain")},
        {QStringLiteral("create_atlased_mesh_from_abstract_domain"),       QStringLiteral("needs an abstract domain from Parametrize by Abstract Domain")},
        {QStringLiteral("transfer_abstract_domain_to_another_layer"),      QStringLiteral("needs an abstract domain on a second layer")},

        // Optional component, absent from some build configurations.
        {QStringLiteral("remesh_to_quads_quadwild_bimdf"),                 QStringLiteral("bundled helpers not present in every build")},
    };
    return table;
}

} // namespace

class FilterSmokeTests : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void runsOrRefuses_data();
    void runsOrRefuses();
    void cleanupTestCase();

private:
    // Outcome of one attempt on one rung.
    struct Attempt {
        bool success = false;
        QString error;
    };
    Attempt attempt(Fixture f, const QString &filterId);

    // Filter keys are plugin id + filter id, so they do not depend on the document.
    // Resolving them once matters: filterInfos() rebuilds and re-resolves every
    // descriptor in the registry, which at six attempts per filter dominated the
    // run time of the whole sweep.
    QHash<QString, QString> m_keyById;
    QHash<QString, MeshFilterOutputDomain> m_outputDomainById;

    QStringList m_ladderReport;
    int m_ran = 0;
    int m_refused = 0;
};

void FilterSmokeTests::initTestCase()
{
    Document probe;
    const std::vector<Document::FilterInfo> infos = probe.filterInfos();
    QVERIFY2(!infos.empty(), "filter registry is empty");
    for (const auto &info : infos) {
        m_keyById.insert(info.descriptor.id, info.key);
        m_outputDomainById.insert(info.descriptor.id, info.descriptor.outputDomain);
    }
}

void FilterSmokeTests::runsOrRefuses_data()
{
    QTest::addColumn<QString>("filterId");

    Document probe;
    for (const auto &info : probe.filterInfos()) {
        if (info.descriptor.inputDomain != MeshFilterInputDomain::SingleMesh)
            continue;
        QTest::newRow(qPrintable(info.descriptor.id)) << info.descriptor.id;
    }
}

// One attempt: a fresh document at this rung, the filter run with defaults, and the
// result checked against what its descriptor promises to produce.
FilterSmokeTests::Attempt FilterSmokeTests::attempt(Fixture f, const QString &filterId)
{
    Attempt out;
    const QString key = m_keyById.value(filterId);
    if (key.isEmpty()) {
        out.error = QStringLiteral("not registered");
        return out;
    }
    const MeshFilterOutputDomain outputDomain =
        m_outputDomainById.value(filterId, MeshFilterOutputDomain::Information);

    std::unique_ptr<Document> doc = buildDocument(f);
    const int currentBefore = doc->currentMeshIndex();
    const int verticesBefore = (currentBefore >= 0 && currentBefore < doc->meshCount())
        ? doc->mesh(currentBefore).mesh.VN()
        : 0;
    // runFilter validates the input domain itself and refuses with the same message
    // the menu would grey the entry out with, so the applicability check is left to it.
    const MeshFilterRunResult result = doc->runFilter(key, {});
    if (!result.success) {
        out.error = result.errorMessage;
        if (out.error.trimmed().isEmpty())
            out.error = QStringLiteral("<refused without an error message>");
        return out;
    }

    // A filter that says it creates meshes has to have created one with something
    // in it; "succeeded and produced nothing" is the failure mode this catches.
    if (outputDomain == MeshFilterOutputDomain::NewMeshes) {
        if (result.newMeshIndices.isEmpty()) {
            out.error = QStringLiteral("reported success but created no layer");
            return out;
        }
        int vertices = 0;
        for (int index : result.newMeshIndices) {
            if (index >= 0 && index < doc->meshCount())
                vertices += doc->mesh(index).mesh.VN();
        }
        if (vertices <= 0) {
            out.error = QStringLiteral("reported success but the new layer is empty");
            return out;
        }
    }

    // The same failure mode from the other direction: a filter that edits the current
    // layer must not quietly consume it. `trim_surface_by_scalar_isovalue` did exactly
    // that on a mesh with no per-vertex scalar -- every vertex read 0.0, the default
    // threshold is 0.0, and the layer was emptied and reported as a success. A filter
    // whose honest answer is "nothing survives" should refuse and say so.
    if (outputDomain == MeshFilterOutputDomain::ModifyCurrentMesh && verticesBefore > 0) {
        const int currentAfter = doc->currentMeshIndex();
        const int verticesAfter = (currentAfter >= 0 && currentAfter < doc->meshCount())
            ? doc->mesh(currentAfter).mesh.VN()
            : 0;
        if (verticesAfter <= 0) {
            out.error = QStringLiteral("reported success but emptied the current layer");
            return out;
        }
    }

    out.success = true;
    return out;
}

void FilterSmokeTests::runsOrRefuses()
{
    QFETCH(QString, filterId);

    // A sweep that runs three hundred algorithms will eventually meet one that
    // crashes, and Qt Test names the function but not the data row, so the run
    // dies without saying which filter or which input did it. Set
    // MESHLAB2_SMOKE_TRACE=1 to get a breadcrumb before every attempt.
    const bool trace = qEnvironmentVariableIsSet("MESHLAB2_SMOKE_TRACE");

    QStringList refusals;
    for (Fixture f : fixtureLadder()) {
        if (trace) {
            QTextStream(stderr) << "attempting " << filterId << " on "
                                << fixtureName(f) << '\n' << Qt::flush;
        }
        const Attempt a = attempt(f, filterId);
        if (a.success) {
            m_ladderReport << QStringLiteral("%1\t%2").arg(fixtureName(f), filterId);
            ++m_ran;
            // A filter listed as un-runnable that now runs means the table is stale.
            QVERIFY2(!expectedRefusals().contains(filterId),
                     qPrintable(QStringLiteral("%1 is listed in kExpectedRefusals (\"%2\") but "
                                               "runs on the '%3' fixture; remove the entry")
                                    .arg(filterId, expectedRefusals().value(filterId),
                                         fixtureName(f))));
            return;
        }
        refusals << QStringLiteral("%1: %2").arg(fixtureName(f), a.error.simplified());
    }

    ++m_refused;
    m_ladderReport << QStringLiteral("none\t%1").arg(filterId);
    QVERIFY2(expectedRefusals().contains(filterId),
             qPrintable(QStringLiteral("%1 runs on no fixture and is not listed in "
                                       "kExpectedRefusals. Either build it an input, or add it "
                                       "with a reason.\n  %2")
                            .arg(filterId, refusals.join(QStringLiteral("\n  ")))));
}

// The rung each filter needed is the evidence for declaring its real input
// requirement in the manifest, so it is printed rather than thrown away.
void FilterSmokeTests::cleanupTestCase()
{
    std::sort(m_ladderReport.begin(), m_ladderReport.end());
    // Written straight to stdout rather than through qInfo(): some of the filters
    // this sweep runs reconfigure Qt logging on the way past, and the report is the
    // one piece of output worth more than the pass/fail line.
    QTextStream report(stdout);
    report << "\nfixture\tfilter\n" << m_ladderReport.join(QLatin1Char('\n')) << '\n'
           << m_ran << " filters ran, " << m_refused << " refused on every fixture\n"
           << Qt::flush;
}

QTEST_MAIN(FilterSmokeTests)
#include "test_filter_smoke.moc"
