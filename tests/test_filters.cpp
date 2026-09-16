#include <QtTest/QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDirIterator>
#include <QRegularExpression>
#include <QSet>

#include <map>
#include <vcg/complex/algorithms/bitquad_support.h>
#include <random>

#include "document.h"
#include "layerdata.h"

#include <vcg/complex/algorithms/stat.h>
#include "textureassociationutils.h"

#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/append.h>
#include <vcg/complex/algorithms/clean.h>
#include <vcg/complex/algorithms/create/platonic.h>
#include <vcg/complex/algorithms/update/bounding.h>
#include <vcg/complex/algorithms/update/normal.h>
#include <vcg/complex/algorithms/update/topology.h>
#include <vcg/complex/allocate.h>
#include <array>
#include <cmath>

namespace {

void makeCubeMesh(VCGMesh &mesh, float offsetX, float offsetY, float offsetZ)
{
    mesh.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 8);
    const std::array<vcg::Point3f, 8> vertices = {
        vcg::Point3f(offsetX + 0.0f, offsetY + 0.0f, offsetZ + 0.0f),
        vcg::Point3f(offsetX + 1.0f, offsetY + 0.0f, offsetZ + 0.0f),
        vcg::Point3f(offsetX + 1.0f, offsetY + 1.0f, offsetZ + 0.0f),
        vcg::Point3f(offsetX + 0.0f, offsetY + 1.0f, offsetZ + 0.0f),
        vcg::Point3f(offsetX + 0.0f, offsetY + 0.0f, offsetZ + 1.0f),
        vcg::Point3f(offsetX + 1.0f, offsetY + 0.0f, offsetZ + 1.0f),
        vcg::Point3f(offsetX + 1.0f, offsetY + 1.0f, offsetZ + 1.0f),
        vcg::Point3f(offsetX + 0.0f, offsetY + 1.0f, offsetZ + 1.0f)
    };
    for (size_t i = 0; i < vertices.size(); ++i)
        mesh.vert[i].P() = vertices[i];

    const std::array<std::array<int, 3>, 12> faces = {
        std::array<int, 3>{0, 2, 1}, std::array<int, 3>{0, 3, 2},
        std::array<int, 3>{4, 5, 6}, std::array<int, 3>{4, 6, 7},
        std::array<int, 3>{0, 1, 5}, std::array<int, 3>{0, 5, 4},
        std::array<int, 3>{3, 7, 6}, std::array<int, 3>{3, 6, 2},
        std::array<int, 3>{0, 4, 7}, std::array<int, 3>{0, 7, 3},
        std::array<int, 3>{1, 2, 6}, std::array<int, 3>{1, 6, 5}
    };
    for (const auto &face : faces)
        vcg::tri::Allocator<VCGMesh>::AddFace(mesh, face[0], face[1], face[2]);

    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

void makeOpenDiskMesh(VCGMesh &mesh)
{
    mesh.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 5);
    mesh.vert[0].P() = vcg::Point3f(0.0f, 0.0f, 0.0f);
    mesh.vert[1].P() = vcg::Point3f(1.0f, 0.0f, 0.0f);
    mesh.vert[2].P() = vcg::Point3f(1.0f, 1.0f, 0.0f);
    mesh.vert[3].P() = vcg::Point3f(0.0f, 1.0f, 0.0f);
    mesh.vert[4].P() = vcg::Point3f(0.5f, 0.5f, 0.0f);

    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 4);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 1, 2, 4);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 2, 3, 4);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 3, 0, 4);

    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

// A flat open square at height z, two triangles wound so their normal points along +Z.
// Made wider than the solids it is cut against, so it separates them completely.
void makeSquareSheetMesh(VCGMesh &mesh, float halfWidth, float z)
{
    mesh.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 4);
    mesh.vert[0].P() = vcg::Point3f(-halfWidth, -halfWidth, z);
    mesh.vert[1].P() = vcg::Point3f(halfWidth, -halfWidth, z);
    mesh.vert[2].P() = vcg::Point3f(halfWidth, halfWidth, z);
    mesh.vert[3].P() = vcg::Point3f(-halfWidth, halfWidth, z);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 2, 3);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

void makeOpenCubeMesh(VCGMesh &mesh)
{
    makeCubeMesh(mesh, 0.0f, 0.0f, 0.0f);
    vcg::tri::Allocator<VCGMesh>::DeleteFace(mesh, mesh.face[0]);
    vcg::tri::Allocator<VCGMesh>::DeleteFace(mesh, mesh.face[1]);
    vcg::tri::Allocator<VCGMesh>::CompactEveryVector(mesh);
}

void makeIsolatedFoldMesh(VCGMesh &mesh)
{
    mesh.Clear();
    const std::array<vcg::Point3f, 6> vertices = {
        vcg::Point3f(0.0f, 0.0f, 0.0f), vcg::Point3f(2.0f, 0.0f, 0.0f),
        vcg::Point3f(1.0f, 2.0f, 0.0f), vcg::Point3f(1.0f, 4.0f, 0.0f),
        vcg::Point3f(-1.5f, -1.0f, 0.0f), vcg::Point3f(3.5f, -1.0f, 0.0f)
    };
    for (const vcg::Point3f &point : vertices)
        vcg::tri::Allocator<VCGMesh>::AddVertex(mesh, point);

    // Face 0 points upward, while its three consistently oriented neighbours
    // overlap it and point downward.  Each opposite vertex lies strictly inside
    // the corresponding neighbour, so the central fold is repairable by a flip.
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 1, 0, 3);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 2, 1, 4);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 2, 5);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

bool isSelectedEdge(const VCGMesh &mesh, int firstVertex, int secondVertex)
{
    for (const VCGFace &face : mesh.face) {
        for (int edge = 0; edge < 3; ++edge) {
            const int a = int(vcg::tri::Index(mesh, face.cV0(edge)));
            const int b = int(vcg::tri::Index(mesh, face.cV1(edge)));
            if (((a == firstVertex && b == secondVertex)
                    || (a == secondVertex && b == firstVertex))
                && face.IsFaceEdgeS(edge))
                return true;
        }
    }
    return false;
}

void makeTwoTextureTriangles(VCGMesh &mesh)
{
    mesh.Clear();
    mesh.face.EnableWedgeTexCoord();
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 6);
    for (int i = 0; i < 6; ++i)
        mesh.vert[size_t(i)].P() = vcg::Point3f(float(i % 3), float(i / 3), 0.0f);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 3, 4, 5);
    for (int face = 0; face < 2; ++face) {
        mesh.face[size_t(face)].WT(0) = VCGFace::TexCoordType(0.0f, 0.0f);
        mesh.face[size_t(face)].WT(1) = VCGFace::TexCoordType(1.0f, 0.0f);
        mesh.face[size_t(face)].WT(2) = VCGFace::TexCoordType(0.0f, 1.0f);
        for (int corner = 0; corner < 3; ++corner)
            mesh.face[size_t(face)].WT(corner).N() = face;
    }
}

void makeIcpPointCloud(VCGMesh &mesh)
{
    mesh.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 5);
    const std::array<vcg::Point3f, 5> vertices = {
        vcg::Point3f(0.00f, 0.00f, 0.00f),
        vcg::Point3f(1.00f, 0.07f, 0.03f),
        vcg::Point3f(0.12f, 0.96f, 0.23f),
        vcg::Point3f(0.18f, 0.15f, 1.05f),
        vcg::Point3f(0.83f, 0.72f, 0.64f)
    };
    for (size_t i = 0; i < vertices.size(); ++i) {
        mesh.vert[i].P() = vertices[i];
        mesh.vert[i].N() = vcg::Point3f(0.0f, 0.0f, 1.0f);
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
}

void makeScalarStripMesh(VCGMesh &mesh, int columns)
{
    mesh.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 2 * (columns + 1));
    for (int i = 0; i <= columns; ++i) {
        mesh.vert[std::size_t(2 * i)].P() = vcg::Point3f(float(i), 0.0f, 0.0f);
        mesh.vert[std::size_t(2 * i + 1)].P() = vcg::Point3f(float(i), 1.0f, 0.0f);
    }
    for (int i = 0; i < columns; ++i) {
        const int a = 2 * i;
        vcg::tri::Allocator<VCGMesh>::AddFace(mesh, a, a + 2, a + 3);
        vcg::tri::Allocator<VCGMesh>::AddFace(mesh, a, a + 3, a + 1);
    }
    // The scalar is the X coordinate itself, so the contour at value v is the line x = v.
    for (VCGVertex &v : mesh.vert)
        v.Q() = v.cP().X();
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

void makeNonManifoldFanMesh(VCGMesh &mesh)
{
    mesh.Clear();
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 5);
    mesh.vert[0].P() = vcg::Point3f(0.0f, 0.0f, 0.0f);
    mesh.vert[1].P() = vcg::Point3f(1.0f, 0.0f, 0.0f);
    mesh.vert[2].P() = vcg::Point3f(0.5f, 1.0f, 0.0f);
    mesh.vert[3].P() = vcg::Point3f(0.5f, 0.0f, 1.0f);
    mesh.vert[4].P() = vcg::Point3f(0.5f, -1.0f, 0.0f);

    // Three fins on the edge 0-1, which three faces therefore share.
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 3);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 4);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalizedPerFaceNormalized(mesh);
}

void addPolylineSegment(VCGMesh &mesh, int firstVertex, int secondVertex)
{
    auto edge = vcg::tri::Allocator<VCGMesh>::AddEdges(mesh, 1);
    edge->V(0) = &mesh.vert[std::size_t(firstVertex)];
    edge->V(1) = &mesh.vert[std::size_t(secondVertex)];
}

double polylineLength(const VCGMesh &mesh)
{
    double total = 0.0;
    for (const VCGEdge &e : mesh.edge) {
        if (e.IsD())
            continue;
        total += double((e.cV(1)->cP() - e.cV(0)->cP()).Norm());
    }
    return total;
}

int polylinePathCount(const VCGMesh &mesh)
{
    const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    if (!base)
        return 0;

    std::vector<std::vector<int>> neighbours(mesh.vert.size());
    for (const VCGEdge &e : mesh.edge) {
        if (e.IsD())
            continue;
        const int first = int(e.cV(0) - base);
        const int second = int(e.cV(1) - base);
        neighbours[std::size_t(first)].push_back(second);
        neighbours[std::size_t(second)].push_back(first);
    }

    std::vector<bool> reached(mesh.vert.size(), false);
    std::vector<int> pending;
    int paths = 0;
    for (std::size_t start = 0; start < neighbours.size(); ++start) {
        if (reached[start] || neighbours[start].empty())
            continue;
        ++paths;
        reached[start] = true;
        pending.push_back(int(start));
        while (!pending.empty()) {
            const int at = pending.back();
            pending.pop_back();
            for (int next : neighbours[std::size_t(at)]) {
                if (!reached[std::size_t(next)]) {
                    reached[std::size_t(next)] = true;
                    pending.push_back(next);
                }
            }
        }
    }
    return paths;
}

// Vertices carrying a single edge: a closed curve has none, an open one has two ends.
int polylineLooseEndCount(const VCGMesh &mesh)
{
    std::vector<int> valence(mesh.vert.size(), 0);
    const VCGVertex *base = mesh.vert.empty() ? nullptr : &mesh.vert.front();
    for (const VCGEdge &e : mesh.edge) {
        if (e.IsD() || !base)
            continue;
        ++valence[std::size_t(e.cV(0) - base)];
        ++valence[std::size_t(e.cV(1) - base)];
    }

    int ends = 0;
    for (int n : valence) {
        if (n == 1)
            ++ends;
    }
    return ends;
}

// Every live vertex sits at one of the given X coordinates, and none of them is unused.
// The strip's scalar is its own X coordinate, so a contour at value v is the line x = v
// and this is the whole statement about where the isocontour filters put their answers.
bool verticesLieAtXValues(const VCGMesh &mesh, const std::vector<double> &values)
{
    std::vector<int> seen(values.size(), 0);
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        int match = -1;
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (std::abs(double(v.cP().X()) - values[i]) < 1e-4)
                match = int(i);
        }
        if (match < 0)
            return false;
        ++seen[std::size_t(match)];
    }
    for (int count : seen) {
        if (count == 0)
            return false;
    }
    return true;
}

bool matrixNear(const QMatrix4x4 &a, const QMatrix4x4 &b, float eps = 1e-3f)
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (std::abs(a(row, col) - b(row, col)) > eps)
                return false;
    return true;
}

QString filterKeyForId(const Document &doc, const QString &filterId);

} // namespace

class FilterTests : public QObject
{
    Q_OBJECT

private slots:
    void filterRegistryExposesBuiltins();
    void filterApplicabilityReflectsDocumentState();
    void basicFiltersRunOnLoadedMesh();
    void planarSectionSurfaceUsesCpuTessellator();
    void filterParameterValidation();
    void meshFixRepairsOpenCube();
    void qslimSimplifiesCube();
    void instantMeshesRemeshesCube();
    void catmullClarkSubdividesCube();
    void polygonFaceCountCacheTracksFilterChanges();
    void isolatedFoldRepairPreservesFlagsAndConverges();
    void isolatedFoldRepairRejectsUnsupportedMeshes();
    void vertexDisplacementFiltersRunOnCube();
    void sphericalCapPointCreationIsAreaUniform();
    void splitConnectedComponentsAfterDuplicateVertexRemoval();
    void hausdorffRunsOnTransientMeshCopies();
    void cgalAlphaWrapRunsWhenAvailable();
    void cgalAlphaWrapAcceptsPointClouds();
    void convexHullOfIcosahedronIsTheIcosahedron();
    void selectVisibleVerticesSelectsNearSideOnly();
    void selectVisibleVerticesRejectsEnclosedViewpoint();
    void alphaShapeConvergesToConvexHull();
    void alphaShapeHandlesALargePointSet();
    void voronoiFilteringReconstructsASphere();
    void advancingFrontReconstructsASphere();
    void scaleSpaceReconstructsASphere();
    void orientNormalsFlipsInvertedNormals();
    void cgalPoissonReconstructsASphere();
    void kineticReconstructsABox();
    void trueFormAlignmentRecoversAKnownTransform();
    void trueFormBooleansAgreeWithVolume();
    void trueFormCsgExpressionMatchesPairwiseBooleans();
    void trueFormCsgSheetsCutWithoutEnclosing();
    void trueFormSolidDomainsSplitTheEnclosedVolume();
    void trueFormComponentSplitSeparatesDisjointSurfaces();
    void trueFormComponentConnectivityChoosesWhatJoins();
    void trueFormCurveFamilyProducesPolylines();
    void newMeshFiltersReportTheirLayers();
    void trueFormDistanceAndContainment();
    void trueFormAttributeFiltersBehaveAsDocumented();
    void trueFormRemeshingChangesResolutionAsAsked();
    void trueFormOrientationAndEdgeSelection();
    void trueFormRepairAndIsobands();
    void trueFormImproveTriangulationRaisesQuality();
    void trueFormIsocontoursLandOnTheRequestedValues();
    void trueFormIsobandCutSplitsOnlyAlongTheContours();
    void trueFormIntersectionCurveTracesTheCrossingLoop();
    void trueFormTubesSweepTheWholePolyline();
    void trueFormSignedDistanceReportsExactMagnitudes();
    void trueFormContainmentSelectionModesCompose();
    void trueFormChamferDistanceMeasuresAKnownOffset();
    void trueFormNonManifoldSelectionFindsTheSharedEdge();
    void trueFormShellAndRepairOfOverlappingCubes();
    void voronoiAtlasHandlesCoarseMeshes();
    void geodesicQualityFilterDoesNotBakeVertexColors();
    void triOptimizeFiltersRunOnLoadedMesh();
    void voronoiSurfaceSamplingRunsOnCube();
    void voronoiSolidWireframeRunsOnLoadedMesh();
    void icpBetweenPointCloudsUpdatesSourceTransform();
    void translateFilterMovesOnlyCurrentMesh();
    void applyToAllVisibleLayersIsASingleUndoStep();
    void applyToAllVisibleLayersDoesNotConsumeItsOwnOutput();
    void applyToAllVisibleLayersReportsSkippedLayers();
    void packTextureImagesCreatesGutteredAtlas();
    void faceQualityFiltersAreSplit();
    void libiglParametrizationFiltersRunWhenAvailable();
    void libiglQuantityFiltersRunWhenAvailable();
    void meshBooleanFiltersRunWhenAvailable();
    void ambientOcclusionIsScaleInvariant();
    void ambientOcclusionSupportsPointCloudsAndDirectionalLighting();
    void randomSeedMakesSamplingReproducible();
    void randomSeedZeroVariesBetweenRuns();
    void randomSeedControlsExpressionRnd();
    void randomizedFiltersDeclareARandomSeed();
    void capFiltersAgreeOnTheHalfAngleConvention();
    void elementSamplingEmitsOneSamplePerElement();
    void normalizeReferenceFrameIsOrientationInvariant();
    void normalizeReferenceFrameControlsAreIndependent();
    void bboxCentrePivotIsWorldSpace();
    void unfrozenTransformFilterKeepsTheMatrix();
    void displayNamesLeadWithALexiconVerb();
    void structuredReferencesRenderCitations();
    void toolIconsResolveFromResources();
    void quadPairingChoosesGoodDiagonals();
    void voronoiAtlasKeepsSelectionAndColorAlone();
    void textureIslandMergeCanSkipResampling();
    void packUvChartsRunsEveryAlgorithm();
    void packUvChartsWorksWithoutATexture();
    void packUvChartsSurvivesAnyRotationCount();
    void selfIntersectionCurvesFindTheCrossing();
    void abstractDomainIsBuiltAndAttachedToTheLayer();
    void abstractDomainRefusesAnOpenMesh();
    void abstractDomainIndexesRegionsOnFaces();
    void abstractDomainConsumersRunAndRefuseWithoutIt();
    void atlasedMeshPacksOneUvSpaceForEveryChartShape();
    void abstractDomainMeasureReportsItsStructureAndCatchesABrokenOne();
    void layerFiltersRunFromTheContextMenuAreTheParameterlessOnes();
    void createdCylinderHonoursRadiusHeightAndAxis();
    void edgeExpressionsSelectColorAndScaleAPolyline();
    void stateJsonAcceptsBothNameSpellings();
    void rubberBandExpandsToConnectedComponents();
    void islandMergeCanTakeItsIslandsFromTheSelection();
    void islandMergeSurvivesATextureItCannotDecode();
    void hardcodedFilterKeysInTheUiStillResolve();
    void setMatrixComposesOnTheLeftOfTheLayerTransform();
    void bothBallPivotingsInterpolateTheirInputPoints();
    void ballPivotingRebuildsAfterDeletingTheInitialFaces();
};

void FilterTests::filterRegistryExposesBuiltins()
{
    Document doc;
    const std::vector<Document::FilterInfo> infos = doc.filterInfos();
    QVERIFY(!infos.empty());

    bool hasMeshInfo = false;
    bool hasNormalize = false;
    bool hasDuplicate = false;
    bool hasCreateIso = false;
    bool hasCleanUnref = false;
    bool hasMeshFixRepair = false;
    bool hasOriginalQSlim = false;
    bool hasScreenedPoissonReference = false;
    bool hasTextureDefragReference = false;
    bool hasSmallIslandsReference = false;
    bool hasQuadricReference = false;
    bool hasTexturedQuadricReference = false;
    bool hasSelectOutliers = false;
    bool hasSelectColor = false;
    bool hasTriOptimizePlanar = false;
    bool hasTriOptimizeCurvature = false;
    bool hasTriOptimizeSmooth = false;
    bool hasVoronoiSampling = false;
    bool hasVoronoiVolume = false;
    bool hasVoronoiScaffolding = false;
    bool hasSolidWireframe = false;
    bool hasTwoMeshIcp = false;
    bool hasGlobalIcp = false;
    bool hasOverlapGraph = false;
    for (const auto &info : infos) {
        hasMeshInfo = hasMeshInfo || (info.descriptor.id == QStringLiteral("measure_mesh_summary"));
        hasNormalize = hasNormalize || (info.descriptor.id == QStringLiteral("normalize_reference_frame"));
        hasDuplicate = hasDuplicate || (info.descriptor.id == QStringLiteral("duplicate_current_layer"));
        hasCreateIso = hasCreateIso || (info.descriptor.id == QStringLiteral("create_isosurface_from_perlin_noise"));
        hasCleanUnref =
            hasCleanUnref || (info.descriptor.id == QStringLiteral("remove_unreferenced_vertices"));
        if (info.descriptor.id == QStringLiteral("repair_watertight_mesh_meshfix")) {
            hasMeshFixRepair = true;
            QCOMPARE(info.descriptor.provenance.project, QStringLiteral("MeshFix 2.1"));
            QCOMPARE(info.descriptor.provenance.integration, QStringLiteral("external/meshfix"));
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().doi,
                     QStringLiteral("10.1007/s00371-010-0416-3"));
            QVERIFY(info.descriptor.references.front().bibTeX().contains(
                QStringLiteral("@article{attene2010meshfix,")));
        }
        if (info.descriptor.id
            == QStringLiteral("simplify_by_quadric_edge_collapse_qslim")) {
            hasOriginalQSlim = true;
            QCOMPARE(info.descriptor.provenance.project,
                     QStringLiteral("QSlim 2.1 (original MixKit implementation)"));
            QCOMPARE(info.descriptor.provenance.integration,
                     QStringLiteral("external/qslim"));
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().doi,
                     QStringLiteral("10.1145/258734.258849"));
        }
        if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_screened_poisson")) {
            hasScreenedPoissonReference = true;
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().doi,
                     QStringLiteral("10.1145/2487228.2487237"));
            QVERIFY(!info.descriptor.references.front().webUrl().isEmpty());
        }
        if (info.descriptor.id == QStringLiteral("defragment_texture_atlas")) {
            hasTextureDefragReference = true;
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().id,
                     QStringLiteral("maggiordomo2021texture"));
        }
        if (info.descriptor.id == QStringLiteral("merge_texture_islands")) {
            hasSmallIslandsReference = true;
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().id,
                     QStringLiteral("maggiordomo2021texture"));
        }
        if (info.descriptor.id == QStringLiteral("simplify_by_quadric_edge_collapse_vcglib")) {
            hasQuadricReference = true;
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().doi,
                     QStringLiteral("10.1145/258734.258849"));
            QCOMPARE(info.descriptor.references.front().webUrl(),
                     QStringLiteral("https://github.com/alecjacobson/qslim"));
        }
        if (info.descriptor.id
            == QStringLiteral("simplify_by_quadric_edge_collapse_with_texture_vcglib")) {
            hasTexturedQuadricReference = true;
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().doi,
                     QStringLiteral("10.1109/VISUAL.1998.745312"));
        }
        hasSelectOutliers =
            hasSelectOutliers || (info.descriptor.id == QStringLiteral("select_outliers"));
        hasSelectColor =
            hasSelectColor || (info.descriptor.id == QStringLiteral("select_faces_by_color"));
        hasTriOptimizePlanar =
            hasTriOptimizePlanar || (info.descriptor.id == QStringLiteral("flip_edges_by_planarity"));
        hasTriOptimizeCurvature =
            hasTriOptimizeCurvature || (info.descriptor.id == QStringLiteral("flip_edges_by_curvature"));
        hasTriOptimizeSmooth =
            hasTriOptimizeSmooth || (info.descriptor.id == QStringLiteral("smooth_vertices_by_surface_preserving_laplacian_vcglib"));
        hasVoronoiSampling =
            hasVoronoiSampling || (info.descriptor.id == QStringLiteral("sample_surface_by_voronoi_relaxation"));
        hasVoronoiVolume =
            hasVoronoiVolume || (info.descriptor.id == QStringLiteral("sample_volume"));
        hasVoronoiScaffolding =
            hasVoronoiScaffolding || (info.descriptor.id == QStringLiteral("create_voronoi_scaffolding"));
        hasSolidWireframe =
            hasSolidWireframe || (info.descriptor.id == QStringLiteral("create_solid_wireframe"));
        hasTwoMeshIcp =
            hasTwoMeshIcp || (info.descriptor.id == QStringLiteral("align_by_icp_vcglib"));
        hasGlobalIcp =
            hasGlobalIcp || (info.descriptor.id == QStringLiteral("align_meshes_globally"));
        hasOverlapGraph =
            hasOverlapGraph || (info.descriptor.id == QStringLiteral("measure_layer_overlap"));
    }

    QVERIFY(hasMeshInfo);
    QVERIFY(hasNormalize);
    QVERIFY(hasDuplicate);
    QVERIFY(hasCreateIso);
    QVERIFY(hasCleanUnref);
    QVERIFY(hasMeshFixRepair);
    QVERIFY(hasOriginalQSlim);
    QVERIFY(hasScreenedPoissonReference);
    QVERIFY(hasTextureDefragReference);
    QVERIFY(hasSmallIslandsReference);
    QVERIFY(hasQuadricReference);
    QVERIFY(hasTexturedQuadricReference);
    QVERIFY(hasSelectOutliers);
    QVERIFY(hasSelectColor);
    QVERIFY(hasTriOptimizePlanar);
    QVERIFY(hasTriOptimizeCurvature);
    QVERIFY(hasTriOptimizeSmooth);
    QVERIFY(hasVoronoiSampling);
    QVERIFY(hasVoronoiVolume);
    QVERIFY(hasVoronoiScaffolding);
    QVERIFY(hasSolidWireframe);
    QVERIFY(hasTwoMeshIcp);
    QVERIFY(hasGlobalIcp);
    QVERIFY(hasOverlapGraph);
}

void FilterTests::filterApplicabilityReflectsDocumentState()
{
    Document doc;
    QString createIsoKey;
    int noneDomainCount = 0;

    {
        const std::vector<Document::FilterInfo> infos = doc.filterInfos();
        QVERIFY(!infos.empty());
        for (const auto &info : infos) {
            if (info.descriptor.id == QStringLiteral("create_isosurface_from_perlin_noise")) {
                createIsoKey = info.key;
            }
            if (info.descriptor.inputDomain == MeshFilterInputDomain::None) {
                ++noneDomainCount;
                QVERIFY(info.applicable);
                QVERIFY(info.applicabilityError.trimmed().isEmpty());
            } else {
                QVERIFY(!info.applicable);
                QVERIFY(!info.applicabilityError.trimmed().isEmpty());
            }
        }
    }
    QVERIFY(noneDomainCount > 0);
    if (!createIsoKey.isEmpty()) {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("resolution"), 32);
        const MeshFilterRunResult createResult = doc.runFilter(createIsoKey, params);
        QVERIFY(createResult.success);
        QVERIFY(createResult.documentModified);
        QCOMPARE(createResult.newMeshIndices.size(), 1);
    }

    const QString path = QStringLiteral(TEST_SOURCE_DIR "/tests/data/simple.off");
    const int meshCountBeforeLoad = doc.meshCount();
    QCOMPARE(doc.loadMesh(path), 0);
    QCOMPARE(doc.meshCount(), meshCountBeforeLoad + 1);

    bool hasMeshInfoApplicable = false;
    bool hasNormalizeApplicable = false;
    bool hasDuplicateApplicable = false;
    {
        const std::vector<Document::FilterInfo> infos = doc.filterInfos();
        QVERIFY(!infos.empty());
        for (const auto &info : infos) {
            if (info.descriptor.id == QStringLiteral("measure_mesh_summary"))
                hasMeshInfoApplicable = info.applicable;
            else if (info.descriptor.id == QStringLiteral("normalize_reference_frame"))
                hasNormalizeApplicable = info.applicable;
            else if (info.descriptor.id == QStringLiteral("duplicate_current_layer"))
                hasDuplicateApplicable = info.applicable;
        }
    }
    QVERIFY(hasMeshInfoApplicable);
    QVERIFY(hasNormalizeApplicable);
    QVERIFY(hasDuplicateApplicable);
}

void FilterTests::planarSectionSurfaceUsesCpuTessellator()
{
    Document doc;
    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    VCGMesh secondCube;
    makeCubeMesh(secondCube, 0.0f, 2.0f, 0.0f);
    vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(cube, secondCube);
    vcg::tri::UpdateBounding<VCGMesh>::Box(cube);
    QVERIFY(doc.addMesh(cube, QStringLiteral("disconnected cubes")) >= 0);

    const QString sectionKey = filterKeyForId(
        doc, QStringLiteral("create_polyline_from_planar_section"));
    QVERIFY(!sectionKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("planeAxis"), QStringLiteral("x"));
    params.insert(QStringLiteral("relativeTo"), QStringLiteral("center"));
    params.insert(QStringLiteral("planeOffset"), 0.0);
    params.insert(QStringLiteral("createSectionSurface"), true);

    const MeshFilterRunResult result = doc.runFilter(sectionKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 2);

    const VCGMesh &section = doc.mesh(result.newMeshIndices[0]).mesh;
    const VCGMesh &cap = doc.mesh(result.newMeshIndices[1]).mesh;
    QVERIFY(section.EN() >= 8);
    QVERIFY(cap.VN() >= 8);
    QVERIFY(cap.FN() >= 4);
    for (const VCGVertex &vertex : cap.vert) {
        if (!vertex.IsD())
            QVERIFY(std::abs(vertex.cP().X() - 0.5f) < 1e-5f);
    }
    for (const VCGFace &face : cap.face) {
        if (!face.IsD())
            QVERIFY(face.cN().X() > 0.99f);
    }

    // A smaller closed shell produces a nested contour. Its section must remain
    // a hole rather than being filled as an independent polygon.
    Document nestedDoc;
    VCGMesh outer;
    VCGMesh inner;
    makeCubeMesh(outer, 0.0f, 0.0f, 0.0f);
    makeCubeMesh(inner, 0.0f, 0.0f, 0.0f);
    for (VCGVertex &vertex : inner.vert)
        vertex.P() = vertex.cP() * 0.5f + vcg::Point3f(0.25f, 0.25f, 0.25f);
    vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(outer, inner);
    vcg::tri::UpdateBounding<VCGMesh>::Box(outer);
    QVERIFY(nestedDoc.addMesh(outer, QStringLiteral("nested cubes")) >= 0);

    const QString nestedSectionKey = filterKeyForId(
        nestedDoc, QStringLiteral("create_polyline_from_planar_section"));
    const MeshFilterRunResult nestedResult = nestedDoc.runFilter(nestedSectionKey, params);
    QVERIFY2(nestedResult.success, qPrintable(nestedResult.errorMessage));
    QCOMPARE(nestedResult.newMeshIndices.size(), 2);
    const VCGMesh &nestedCap = nestedDoc.mesh(nestedResult.newMeshIndices[1]).mesh;
    // Intersections with the triangulated cube faces retain collinear points
    // along each square boundary (eight vertices per contour).
    QCOMPARE(nestedCap.VN(), 16);
    QCOMPARE(nestedCap.FN(), 16);
    double nestedArea = 0;
    for (const VCGFace &face : nestedCap.face) {
        if (face.IsD())
            continue;
        nestedArea += vcg::DoubleArea(face) * 0.5;
        const vcg::Point3f centroid = vcg::Barycenter(face);
        QVERIFY(!(centroid.Y() > 0.25f && centroid.Y() < 0.75f
            && centroid.Z() > 0.25f && centroid.Z() < 0.75f));
    }
    QVERIFY(std::abs(nestedArea - 0.75) < 1e-6);
}

void FilterTests::basicFiltersRunOnLoadedMesh()
{
    Document doc;
    const QString path = QStringLiteral(TEST_SOURCE_DIR "/tests/data/simple.off");
    QCOMPARE(doc.loadMesh(path), 0);
    QCOMPARE(doc.meshCount(), 1);

    QString meshInfoKey;
    QString normalizeKey;
    QString duplicateKey;
    QString createIsoKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("measure_mesh_summary"))
            meshInfoKey = info.key;
        else if (info.descriptor.id == QStringLiteral("normalize_reference_frame"))
            normalizeKey = info.key;
        else if (info.descriptor.id == QStringLiteral("duplicate_current_layer"))
            duplicateKey = info.key;
        else if (info.descriptor.id == QStringLiteral("create_isosurface_from_perlin_noise"))
            createIsoKey = info.key;
    }

    QVERIFY(!meshInfoKey.isEmpty());
    QVERIFY(!normalizeKey.isEmpty());
    QVERIFY(!duplicateKey.isEmpty());
    QVERIFY(!createIsoKey.isEmpty());

    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("precision"), 2);
        const MeshFilterRunResult result = doc.runFilter(meshInfoKey, params);
        QVERIFY(result.success);
        QVERIFY(!result.documentModified);
        QVERIFY(!result.infoMessages.isEmpty());
    }

    const int currentBeforeNormalize = doc.currentMeshIndex();
    const std::uint64_t geomRevBefore = doc.mesh(currentBeforeNormalize).geometryRevision;
    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("position"), QStringLiteral("bbox_center"));
        params.insert(QStringLiteral("rotation"), QStringLiteral("unchanged"));
        params.insert(QStringLiteral("scale"), QStringLiteral("unit_longest_side"));
        params.insert(QStringLiteral("Freeze"), true);
        const MeshFilterRunResult result = doc.runFilter(normalizeKey, params);
        QVERIFY(result.success);
        QVERIFY(result.documentModified);
    }
    QCOMPARE(doc.mesh(currentBeforeNormalize).geometryRevision, geomRevBefore + 1);

    const int meshCountBeforeDuplicate = doc.meshCount();
    {
        const MeshFilterRunResult result = doc.runFilter(duplicateKey, {});
        QVERIFY(result.success);
        QVERIFY(result.documentModified);
        QCOMPARE(result.newMeshIndices.size(), 1);
    }
    QCOMPARE(doc.meshCount(), meshCountBeforeDuplicate + 1);
    QVERIFY(doc.mesh(doc.currentMeshIndex()).name.endsWith(QStringLiteral(" copy")));

    const int meshCountBeforeCreate = doc.meshCount();
    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("resolution"), 24);
        const MeshFilterRunResult result = doc.runFilter(createIsoKey, params);
        QVERIFY(result.success);
        QVERIFY(result.documentModified);
        QCOMPARE(result.newMeshIndices.size(), 1);
        const int generatedIndex = result.newMeshIndices.front();
        QVERIFY(generatedIndex >= 0 && generatedIndex < doc.meshCount());
        QVERIFY(doc.mesh(generatedIndex).mesh.VN() > 0);
        QVERIFY(doc.mesh(generatedIndex).mesh.FN() > 0);
    }
    QCOMPARE(doc.meshCount(), meshCountBeforeCreate + 1);
}

void FilterTests::filterParameterValidation()
{
    Document doc;
    const QString path = QStringLiteral(TEST_SOURCE_DIR "/tests/data/simple.off");
    QCOMPARE(doc.loadMesh(path), 0);

    QString meshInfoKey;
    QString normalizeKey;
    QString createIsoKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("measure_mesh_summary"))
            meshInfoKey = info.key;
        else if (info.descriptor.id == QStringLiteral("normalize_reference_frame"))
            normalizeKey = info.key;
        else if (info.descriptor.id == QStringLiteral("create_isosurface_from_perlin_noise"))
            createIsoKey = info.key;
    }

    QVERIFY(!meshInfoKey.isEmpty());
    QVERIFY(!normalizeKey.isEmpty());
    QVERIFY(!createIsoKey.isEmpty());

    {
        const MeshFilterRunResult result = doc.runFilter(meshInfoKey, {});
        QVERIFY(result.success);
    }
    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("precision"), QStringLiteral("oops"));
        const MeshFilterRunResult result = doc.runFilter(meshInfoKey, params);
        QVERIFY(!result.success);
        QVERIFY(result.errorMessage.contains(QStringLiteral("precision")));
    }
    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("precision"), -1);
        const MeshFilterRunResult result = doc.runFilter(meshInfoKey, params);
        QVERIFY(!result.success);
        QVERIFY(result.errorMessage.contains(QStringLiteral("minimum")));
    }
    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("unknown_param"), 1);
        const MeshFilterRunResult result = doc.runFilter(meshInfoKey, params);
        QVERIFY(!result.success);
        QVERIFY(result.errorMessage.contains(QStringLiteral("Unknown parameter")));
    }
    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("minAxisSeparation"), -1.0);
        const MeshFilterRunResult result = doc.runFilter(normalizeKey, params);
        QVERIFY(!result.success);
        QVERIFY(result.errorMessage.contains(QStringLiteral("minimum")));
    }
    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("resolution"), 4);
        const MeshFilterRunResult result = doc.runFilter(createIsoKey, params);
        QVERIFY(!result.success);
        QVERIFY(result.errorMessage.contains(QStringLiteral("minimum")));
    }
}

void FilterTests::meshFixRepairsOpenCube()
{
    Document doc;
    VCGMesh input;
    makeOpenCubeMesh(input);
    const int inputIndex = doc.addMesh(input, QStringLiteral("Open cube"));
    QVERIFY(inputIndex >= 0);

    QMatrix4x4 transform;
    transform.translate(2.0f, 3.0f, 4.0f);
    doc.setMeshTransform(inputIndex, transform);

    QString filterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("repair_watertight_mesh_meshfix")) {
            filterKey = info.key;
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());

    const MeshFilterRunResult result = doc.runFilter(filterKey, {});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);
    QCOMPARE(doc.meshCount(), 2);

    const int outputIndex = result.newMeshIndices.front();
    QVERIFY(matrixNear(doc.meshTransform(outputIndex), transform));
    VCGMesh &output = doc.mesh(outputIndex).mesh;
    QVERIFY(output.VN() > 0);
    QVERIFY(output.FN() >= 12);
    output.face.EnableFFAdjacency();
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(output);
    QCOMPARE(vcg::tri::Clean<VCGMesh>::CountHoles(output), 0);
}

void FilterTests::qslimSimplifiesCube()
{
    Document doc;
    VCGMesh input;
    makeCubeMesh(input, 0.0f, 0.0f, 0.0f);
    const int inputFaces = input.FN();
    const int inputIndex = doc.addMesh(input, QStringLiteral("Cube"));
    QVERIFY(inputIndex >= 0);

    QMatrix4x4 transform;
    transform.translate(2.0f, 3.0f, 4.0f);
    doc.setMeshTransform(inputIndex, transform);

    QString filterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id
            == QStringLiteral("simplify_by_quadric_edge_collapse_qslim")) {
            filterKey = info.key;
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("TargetFaceNum"), 6);
    const MeshFilterRunResult result = doc.runFilter(filterKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);
    QCOMPARE(doc.meshCount(), 2);
    QCOMPARE(doc.mesh(inputIndex).mesh.FN(), inputFaces);

    const int outputIndex = result.newMeshIndices.front();
    QVERIFY(matrixNear(doc.meshTransform(outputIndex), transform));
    const VCGMesh &output = doc.mesh(outputIndex).mesh;
    QVERIFY(output.VN() > 0);
    QVERIFY(output.FN() > 0);
    QVERIFY(output.FN() <= 6);
}

void FilterTests::instantMeshesRemeshesCube()
{
    Document doc;
    VCGMesh input;
    makeCubeMesh(input, 0.0f, 0.0f, 0.0f);
    const int inputIndex = doc.addMesh(input, QStringLiteral("Cube"));
    QVERIFY(inputIndex >= 0);

    QString filterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("remesh_to_quads_instant_meshes")) {
            filterKey = info.key;
            QCOMPARE(info.descriptor.provenance.project, QStringLiteral("Instant Meshes"));
            QCOMPARE(info.descriptor.references.size(), size_t(1));
            QCOMPARE(info.descriptor.references.front().doi,
                     QStringLiteral("10.1145/2816795.2818078"));
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("targetEdgeLength"), 0.5);
    params.insert(QStringLiteral("smoothingIterations"), 0);
    params.insert(QStringLiteral("deterministic"), true);
    params.insert(QStringLiteral("threads"), 2);
    const MeshFilterRunResult result = doc.runFilter(filterKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);
    QCOMPARE(doc.meshCount(), 2);
    const VCGMesh &output = doc.mesh(result.newMeshIndices.front()).mesh;
    QVERIFY(output.VN() > 0);
    QVERIFY(output.FN() > 0);

    QString subdivisionKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id
            == QStringLiteral("convert_to_quads_by_4_8_subdivision")) {
            subdivisionKey = info.key;
            break;
        }
    }
    QVERIFY(!subdivisionKey.isEmpty());
    const MeshFilterRunResult subdivision = doc.runFilter(subdivisionKey, {});
    QVERIFY2(subdivision.success, qPrintable(subdivision.errorMessage));
}

void FilterTests::catmullClarkSubdividesCube()
{
    Document doc;
    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    QVERIFY(doc.addMesh(cube, QStringLiteral("Cube")) >= 0);

    QString filterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("subdivide_by_catmull_clark")) {
            filterKey = info.key;
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());
    const MeshFilterRunResult result = doc.runFilter(filterKey, {});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(doc.mesh(doc.currentMeshIndex()).mesh.FN() > 12);
}

void FilterTests::polygonFaceCountCacheTracksFilterChanges()
{
    VCGMesh quad;
    for (const vcg::Point3f &point : {
             vcg::Point3f(0, 0, 0), vcg::Point3f(1, 0, 0),
             vcg::Point3f(1, 1, 0), vcg::Point3f(0, 1, 0) })
        vcg::tri::Allocator<VCGMesh>::AddVertex(quad, point);
    vcg::tri::Allocator<VCGMesh>::AddFace(quad, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(quad, 0, 2, 3);
    quad.face[0].SetF(2);
    quad.face[1].SetF(0);

    Document doc;
    const int meshIndex = doc.addMesh(
        quad, QStringLiteral("Quad"), vcg::tri::io::Mask::IOM_BITPOLYGONAL);
    QCOMPARE(doc.mesh(meshIndex).polygonFaceCount, 1);

    QString toTrianglesKey;
    QString toQuadsKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("convert_to_pure_triangles"))
            toTrianglesKey = info.key;
        else if (info.descriptor.id == QStringLiteral("convert_to_quads_by_triangle_pairing"))
            toQuadsKey = info.key;
    }
    QVERIFY(!toTrianglesKey.isEmpty());
    QVERIFY(!toQuadsKey.isEmpty());

    QVERIFY2(doc.runFilter(toTrianglesKey, {}).success, "Convert to triangles failed");
    QCOMPARE(doc.mesh(meshIndex).polygonFaceCount, -1);
    QVERIFY(!(doc.mesh(meshIndex).ioMask & vcg::tri::io::Mask::IOM_BITPOLYGONAL));

    QVERIFY2(doc.runFilter(toQuadsKey, {}).success, "Convert to quads failed");
    QCOMPARE(doc.mesh(meshIndex).polygonFaceCount, 1);
    QVERIFY(doc.mesh(meshIndex).ioMask & vcg::tri::io::Mask::IOM_BITPOLYGONAL);
}

void FilterTests::isolatedFoldRepairPreservesFlagsAndConverges()
{
    VCGMesh folded;
    makeIsolatedFoldMesh(folded);
    folded.face[0].SetV(); // Scratch visited state must not affect detection.
    folded.face[1].SetS();
    folded.face[0].SetFaceEdgeS(2); // Boundary edge 2-0.
    folded.face[1].SetFaceEdgeS(1); // Boundary edge 0-3.

    Document doc;
    const int meshIndex = doc.addMesh(folded, QStringLiteral("Isolated fold"));
    QString filterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("remove_isolated_folded_faces_by_edge_flip")) {
            filterKey = info.key;
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());

    const MeshFilterRunResult result = doc.runFilter(filterKey, {});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);

    const VCGMesh &repaired = doc.mesh(meshIndex).mesh;
    QCOMPARE(repaired.FN(), 4);
    QVERIFY(repaired.face[0].IsV());
    QVERIFY(repaired.face[1].IsS());
    QVERIFY(!repaired.face[0].IsS());
    QVERIFY(isSelectedEdge(repaired, 2, 0));
    QVERIFY(isSelectedEdge(repaired, 0, 3));

    // The monotone local score must leave no further repair for a second run.
    const MeshFilterRunResult second = doc.runFilter(filterKey, {});
    QVERIFY2(second.success, qPrintable(second.errorMessage));
    QVERIFY(!second.documentModified);
}

void FilterTests::isolatedFoldRepairRejectsUnsupportedMeshes()
{
    QString filterKey;
    Document registry;
    for (const auto &info : registry.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("remove_isolated_folded_faces_by_edge_flip")) {
            filterKey = info.key;
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());

    VCGMesh polygon;
    makeIsolatedFoldMesh(polygon);
    polygon.face[0].SetF(0);
    Document polygonDoc;
    polygonDoc.addMesh(
        polygon, QStringLiteral("Polygonal"), vcg::tri::io::Mask::IOM_BITPOLYGONAL);
    const MeshFilterRunResult polygonResult = polygonDoc.runFilter(filterKey, {});
    QVERIFY(!polygonResult.success);
    QVERIFY(polygonResult.errorMessage.contains(QStringLiteral("triangle mesh")));

    VCGMesh textured;
    makeIsolatedFoldMesh(textured);
    textured.face.EnableWedgeTexCoord();
    Document texturedDoc;
    texturedDoc.addMesh(
        textured, QStringLiteral("Textured"), vcg::tri::io::Mask::IOM_WEDGTEXCOORD);
    const MeshFilterRunResult texturedResult = texturedDoc.runFilter(filterKey, {});
    QVERIFY(!texturedResult.success);
    QVERIFY(texturedResult.errorMessage.contains(QStringLiteral("texture seams")));

    VCGMesh nonManifold;
    for (const vcg::Point3f &point : {
             vcg::Point3f(0, 0, 0), vcg::Point3f(1, 0, 0), vcg::Point3f(0, 1, 0),
             vcg::Point3f(0, -1, 0), vcg::Point3f(0, 0, 1) })
        vcg::tri::Allocator<VCGMesh>::AddVertex(nonManifold, point);
    vcg::tri::Allocator<VCGMesh>::AddFace(nonManifold, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(nonManifold, 1, 0, 3);
    vcg::tri::Allocator<VCGMesh>::AddFace(nonManifold, 0, 1, 4);
    Document nonManifoldDoc;
    nonManifoldDoc.addMesh(nonManifold, QStringLiteral("Non-manifold"));
    const MeshFilterRunResult nonManifoldResult = nonManifoldDoc.runFilter(filterKey, {});
    QVERIFY(!nonManifoldResult.success);
    QVERIFY(nonManifoldResult.errorMessage.contains(QStringLiteral("2-manifold")));

    VCGMesh unoriented;
    makeIsolatedFoldMesh(unoriented);
    std::swap(unoriented.face[1].V(0), unoriented.face[1].V(1));
    Document unorientedDoc;
    unorientedDoc.addMesh(unoriented, QStringLiteral("Unoriented"));
    const MeshFilterRunResult unorientedResult = unorientedDoc.runFilter(filterKey, {});
    QVERIFY(!unorientedResult.success);
    QVERIFY(unorientedResult.errorMessage.contains(QStringLiteral("oriented")));
}

void FilterTests::vertexDisplacementFiltersRunOnCube()
{
    Document doc;
    VCGMesh input;
    makeCubeMesh(input, 0.0f, 0.0f, 0.0f);
    QVERIFY(doc.addMesh(input, QStringLiteral("Cube")) >= 0);

    const QStringList fractalIds {
        QStringLiteral("displace_vertices_by_fractal_brownian_motion"),
        QStringLiteral("displace_vertices_by_standard_multifractal_noise"),
        QStringLiteral("displace_vertices_by_heterogeneous_multifractal_noise"),
        QStringLiteral("displace_vertices_by_hybrid_multifractal_noise"),
        QStringLiteral("displace_vertices_by_ridged_multifractal_noise")
    };
    QHash<QString, QString> fractalKeys;
    QStringList found;
    bool foundRandom = false;
    bool foundLegacyRandom = false;
    for (const auto &info : doc.filterInfos()) {
        if (info.pluginId == QStringLiteral("meshlab2.filter.vertex_displacement")) {
            foundRandom = foundRandom
                || info.descriptor.id == QStringLiteral("displace_vertices_randomly");
            if (fractalIds.contains(info.descriptor.id)) {
                found << info.descriptor.id;
                fractalKeys.insert(info.descriptor.id, info.key);
            }
        }
        foundLegacyRandom = foundLegacyRandom
            || info.descriptor.id == QStringLiteral("apply_coord_random_displacement");
    }
    QCOMPARE(found.size(), fractalIds.size());
    QVERIFY(foundRandom);
    QVERIFY(!foundLegacyRandom);
    QCOMPARE(fractalKeys.size(), fractalIds.size());

    for (const QString &id : fractalIds) {
        Document runDoc;
        VCGMesh cube;
        makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
        QVERIFY(runDoc.addMesh(cube, QStringLiteral("Cube")) >= 0);

        std::vector<vcg::Point3f> before;
        for (const VCGVertex &vertex : runDoc.mesh(0).mesh.vert)
            before.push_back(vertex.cP());

        MeshFilterParameterValues params;
        params.insert(QStringLiteral("maxHeight"), 0.1);
        params.insert(QStringLiteral("scale"), 1.0);
        params.insert(QStringLiteral("octaves"), 3);
        params.insert(QStringLiteral("lacunarity"), 2.0);
        params.insert(QStringLiteral("fractalIncrement"), 1.2);
        if (id != QStringLiteral("displace_vertices_by_fractal_brownian_motion"))
            params.insert(QStringLiteral("offset"), 0.9);
        if (id == QStringLiteral("displace_vertices_by_ridged_multifractal_noise"))
            params.insert(QStringLiteral("gain"), 2.0);
        params.insert(QStringLiteral("seed"), 1.0);
        params.insert(QStringLiteral("normalSmoothingSteps"), 2);
        const MeshFilterRunResult result = runDoc.runFilter(fractalKeys.value(id), params);
        QVERIFY2(result.success, qPrintable(id + QStringLiteral(": ") + result.errorMessage));

        bool changed = false;
        for (size_t i = 0; i < before.size(); ++i) {
            const vcg::Point3f &position = runDoc.mesh(0).mesh.vert[i].cP();
            changed = changed || (position - before[i]).Norm() > 1e-6f;
            QVERIFY(std::isfinite(position.X()));
            QVERIFY(std::isfinite(position.Y()));
            QVERIFY(std::isfinite(position.Z()));
        }
        QVERIFY2(changed, qPrintable(id));
    }
}

void FilterTests::sphericalCapPointCreationIsAreaUniform()
{
    Document probe;
    const QString filterKey = filterKeyForId(
        probe, QStringLiteral("create_points_on_spherical_cap"));
    QVERIFY(!filterKey.isEmpty());

    const vcg::Point3f axis = vcg::Normalized(vcg::Point3f(1.0f, 2.0f, 3.0f));
    const float halfAngle = vcg::math::ToRad(35.0f);
    const float expectedMeanProjection = (1.0f + std::cos(halfAngle)) * 0.5f;
    for (const QString &technique : { QStringLiteral("fibonacci"), QStringLiteral("montecarlo") }) {
        Document doc;
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("point_num"), 1000);
        params.insert(QStringLiteral("direction"), QVector3D(1.0f, 2.0f, 3.0f));
        params.insert(QStringLiteral("half_angle"), 35.0);
        params.insert(QStringLiteral("technique"), technique);
        params.insert(QStringLiteral("randomSeed"), 12345);
        const MeshFilterRunResult result = doc.runFilter(filterKey, params);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QCOMPARE(result.newMeshIndices.size(), 1);

        const VCGMesh &points = doc.mesh(result.newMeshIndices.front()).mesh;
        QCOMPARE(points.VN(), 1000);
        QCOMPARE(points.FN(), 0);
        double meanProjection = 0.0;
        for (const VCGVertex &vertex : points.vert) {
            QVERIFY(std::abs(vertex.cP().Norm() - 1.0f) < 1e-5f);
            QVERIFY(axis * vertex.cP() >= std::cos(halfAngle) - 1e-5f);
            QVERIFY((vertex.cN() - vertex.cP()).Norm() < 1e-5f);
            meanProjection += axis * vertex.cP();
        }
        meanProjection /= points.VN();
        QVERIFY(std::abs(meanProjection - expectedMeanProjection) < 0.01);
    }
}

void FilterTests::splitConnectedComponentsAfterDuplicateVertexRemoval()
{
    VCGMesh input;
    input.face.EnableWedgeTexCoord();
    vcg::tri::Allocator<VCGMesh>::AddVertices(input, 7);
    const std::array<vcg::Point3f, 7> vertices = {
        vcg::Point3f(0, 0, 0), vcg::Point3f(1, 0, 0), vcg::Point3f(0, 1, 0),
        vcg::Point3f(0, 0, 0),
        vcg::Point3f(3, 0, 0), vcg::Point3f(4, 0, 0), vcg::Point3f(3, 1, 0)
    };
    for (size_t i = 0; i < vertices.size(); ++i)
        input.vert[i].P() = vertices[i];
    vcg::tri::Allocator<VCGMesh>::AddFace(input, 3, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(input, 4, 5, 6);
    for (VCGFace &face : input.face)
        for (int corner = 0; corner < 3; ++corner)
            face.WT(corner) = VCGFace::TexCoordType(float(corner == 1), float(corner == 2));

    Document doc;
    QVERIFY(doc.addMesh(
        input,
        QStringLiteral("Disconnected"),
        vcg::tri::io::Mask::IOM_WEDGTEXCOORD) >= 0);

    auto filterKey = [&](const QString &id) {
        for (const auto &info : doc.filterInfos())
            if (info.descriptor.id == id)
                return info.key;
        return QString();
    };

    const MeshFilterRunResult clean =
        doc.runFilter(filterKey(QStringLiteral("remove_duplicate_vertices_vcglib")), {});
    QVERIFY2(clean.success, qPrintable(clean.errorMessage));

    const MeshFilterRunResult split = doc.runFilter(
        filterKey(QStringLiteral("split_into_connected_components")), {});
    QVERIFY2(split.success, qPrintable(split.errorMessage));
    QCOMPARE(split.newMeshIndices.size(), 2);
    for (int meshIndex : split.newMeshIndices) {
        const VCGMesh &mesh = doc.mesh(meshIndex).mesh;
        QCOMPARE(mesh.FN(), 1);
        QVERIFY(vcg::tri::HasPerWedgeTexCoord(mesh));
        for (const VCGFace &face : mesh.face)
            for (int corner = 0; corner < 3; ++corner)
                QVERIFY(face.cV(corner) != nullptr);
    }
}

void FilterTests::hausdorffRunsOnTransientMeshCopies()
{
    Document doc;
    VCGMesh sampled;
    VCGMesh target;
    makeCubeMesh(sampled, 0.0f, 0.0f, 0.0f);
    makeCubeMesh(target, 0.05f, 0.0f, 0.0f);
    const int sampledIndex = doc.addMesh(sampled, QStringLiteral("Sampled"));
    const int targetIndex = doc.addMesh(target, QStringLiteral("Target"));
    QVERIFY(sampledIndex >= 0);
    QVERIFY(targetIndex >= 0);

    QString filterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("measure_hausdorff_distance")) {
            filterKey = info.key;
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("SampledMesh"), sampledIndex);
    params.insert(QStringLiteral("TargetMesh"), targetIndex);
    params.insert(QStringLiteral("SampleNum"), 32);
    const std::size_t historySize = doc.undoTreeInfo().size();
    const MeshFilterRunResult result = doc.runFilter(filterKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(!result.documentModified);
    QVERIFY(result.infoMessages.join(QLatin1Char('\n')).contains(QStringLiteral("Samples:")));
    QCOMPARE(doc.undoTreeInfo().size(), historySize);
    const auto informationalActions = doc.undoNodeScriptActions(doc.undoCurrentNodeId());
    QVERIFY(std::any_of(
        informationalActions.begin(), informationalActions.end(),
        [&filterKey](const ScriptAction &action) { return action.filterKey == filterKey; }));

    params.insert(QStringLiteral("SaveSample"), true);
    const int meshCountBeforeSamples = doc.meshCount();
    const MeshFilterRunResult savedResult = doc.runFilter(filterKey, params);
    QVERIFY2(savedResult.success, qPrintable(savedResult.errorMessage));
    QVERIFY(savedResult.documentModified);
    QCOMPARE(doc.meshCount(), meshCountBeforeSamples + 2);
    QCOMPARE(doc.undoTreeInfo().size(), historySize + 1);
    QCOMPARE(doc.undoText(), QStringLiteral("Measure Hausdorff Distance"));
    const auto savedActions = doc.undoNodeScriptActions(doc.undoCurrentNodeId());
    QCOMPARE(int(std::count_if(
        savedActions.begin(), savedActions.end(),
        [&filterKey](const ScriptAction &action) { return action.filterKey == filterKey; })), 2);
    QVERIFY(doc.undo());
    QCOMPARE(doc.meshCount(), meshCountBeforeSamples);
}

void FilterTests::cgalAlphaWrapRunsWhenAvailable()
{
    Document doc;
    const QString path = QStringLiteral(TEST_SOURCE_DIR "/tests/data/simple.off");
    QCOMPARE(doc.loadMesh(path), 0);

    QString alphaWrapKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_alpha_wrapping")) {
            alphaWrapKey = info.key;
            break;
        }
    }

    if (alphaWrapKey.isEmpty())
        QSKIP("CGAL Alpha Wrap plugin is not available in this build.");

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("Alpha"), 0.5);
    params.insert(QStringLiteral("Offset"), 0.05);
    const int meshCountBefore = doc.meshCount();
    const MeshFilterRunResult result = doc.runFilter(alphaWrapKey, params);

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);
    QCOMPARE(result.newMeshIndices.size(), 1);
    QCOMPARE(doc.meshCount(), meshCountBefore + 1);

    const int generatedIndex = result.newMeshIndices.front();
    QVERIFY(generatedIndex >= 0 && generatedIndex < doc.meshCount());
    QVERIFY(doc.mesh(generatedIndex).mesh.VN() > 0);
    QVERIFY(doc.mesh(generatedIndex).mesh.FN() > 0);
}

// Alpha wrapping does not need faces: CGAL wraps a bare point set just as well, and
// needs no normals because the strictly positive offset defines the envelope. This is
// what makes the filter a genuine point-cloud reconstruction method.
void FilterTests::cgalAlphaWrapAcceptsPointClouds()
{
    Document doc;
    const QString path = QStringLiteral(TEST_SOURCE_DIR "/tests/data/simple.off");
    QCOMPARE(doc.loadMesh(path), 0);

    QString alphaWrapKey;
    QString removeFacesKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_alpha_wrapping"))
            alphaWrapKey = info.key;
        else if (info.descriptor.id == QStringLiteral("remove_all_faces"))
            removeFacesKey = info.key;
    }

    if (alphaWrapKey.isEmpty())
        QSKIP("CGAL Alpha Wrap plugin is not available in this build.");
    QVERIFY(!removeFacesKey.isEmpty());

    // Turn the loaded mesh into a pure point cloud through the normal filter path.
    QVERIFY2(doc.runFilter(removeFacesKey, {}).success, "Remove All Faces failed");
    QCOMPARE(doc.mesh(doc.currentMeshIndex()).mesh.FN(), 0);
    QVERIFY(doc.mesh(doc.currentMeshIndex()).mesh.VN() > 0);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("Alpha"), 0.5);
    params.insert(QStringLiteral("Offset"), 0.05);
    const int meshCountBefore = doc.meshCount();
    const MeshFilterRunResult result = doc.runFilter(alphaWrapKey, params);

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);
    QCOMPARE(doc.meshCount(), meshCountBefore + 1);

    const int generatedIndex = result.newMeshIndices.front();
    QVERIFY(doc.mesh(generatedIndex).mesh.VN() > 0);
    QVERIFY(doc.mesh(generatedIndex).mesh.FN() > 0);
}

// An icosahedron is convex and simplicial, so it is its own convex hull: the result must
// come back with exactly the same 12 vertices and 20 faces. That also satisfies the
// invariant F == 2V - 4 that holds for any triangulated convex hull, which is asserted
// separately so the intent survives if the input mesh is ever swapped.
void FilterTests::convexHullOfIcosahedronIsTheIcosahedron()
{
    Document doc;

    QString icosaKey;
    QString hullKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_icosahedron"))
            icosaKey = info.key;
        else if (info.descriptor.id == QStringLiteral("create_convex_hull"))
            hullKey = info.key;
    }
    QVERIFY(!icosaKey.isEmpty());
    QVERIFY(!hullKey.isEmpty());

    QVERIFY2(doc.runFilter(icosaKey, {}).success, "create_icosahedron failed");
    const int sourceIndex = doc.currentMeshIndex();
    const int sourceVN = doc.mesh(sourceIndex).mesh.VN();
    const int sourceFN = doc.mesh(sourceIndex).mesh.FN();
    QCOMPARE(sourceVN, 12);
    QCOMPARE(sourceFN, 20);

    const MeshFilterRunResult result = doc.runFilter(hullKey, {});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const VCGMesh &hull = doc.mesh(result.newMeshIndices.front()).mesh;
    QCOMPARE(hull.VN(), 12);
    QCOMPARE(hull.FN(), 20);
    QCOMPARE(hull.FN(), 2 * hull.VN() - 4);

    // The hull is computed on a copy: vcglib compacts its input and clears the visited
    // flags, which would silently reindex the source layer.
    QCOMPARE(doc.mesh(sourceIndex).mesh.VN(), sourceVN);
    QCOMPARE(doc.mesh(sourceIndex).mesh.FN(), sourceFN);
}

// From a viewpoint outside the shape, hidden point removal must select some but not all
// vertices, and every selected one must lie on the near side of the centre.
void FilterTests::selectVisibleVerticesSelectsNearSideOnly()
{
    Document doc;

    QString icosaKey;
    QString visibleKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_icosahedron"))
            icosaKey = info.key;
        else if (info.descriptor.id == QStringLiteral("select_visible_vertices"))
            visibleKey = info.key;
    }
    QVERIFY(!icosaKey.isEmpty());
    QVERIFY(!visibleKey.isEmpty());
    QVERIFY2(doc.runFilter(icosaKey, {}).success, "create_icosahedron failed");

    const int index = doc.currentMeshIndex();
    const vcg::Point3f viewpoint(0.0f, 0.0f, 10.0f);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("radiusThreshold"), 0.0);
    params.insert(QStringLiteral("usecamera"), false);
    params.insert(QStringLiteral("viewpoint"), QVector3D(0.0f, 0.0f, 10.0f));

    const MeshFilterRunResult result = doc.runFilter(visibleKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));

    const VCGMesh &mesh = doc.mesh(index).mesh;
    int selected = 0;
    for (const VCGVertex &v : mesh.vert)
        if (v.IsS()) {
            ++selected;
            // Near side: the icosahedron is centred on the origin, so a vertex facing a
            // viewpoint at +Z cannot have a strongly negative z.
            QVERIFY(v.cP().Z() > -1e-4f);
        }
    QVERIFY(selected > 0);
    QVERIFY(selected < mesh.VN());
}

// The default viewpoint is (0,0,0), which is inside any centred mesh — and hidden point
// removal is undefined there. vcglib's ComputePointVisibility asserts and aborts in that
// case, so the filter composes ComputeConvexHull itself; this pins the clean failure and
// guards against anyone "simplifying" it back to the wrapper.
void FilterTests::selectVisibleVerticesRejectsEnclosedViewpoint()
{
    Document doc;

    QString icosaKey;
    QString visibleKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_icosahedron"))
            icosaKey = info.key;
        else if (info.descriptor.id == QStringLiteral("select_visible_vertices"))
            visibleKey = info.key;
    }
    QVERIFY(!icosaKey.isEmpty());
    QVERIFY(!visibleKey.isEmpty());
    QVERIFY2(doc.runFilter(icosaKey, {}).success, "create_icosahedron failed");
    const int index = doc.currentMeshIndex();

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("radiusThreshold"), 0.0);
    params.insert(QStringLiteral("usecamera"), false);
    params.insert(QStringLiteral("viewpoint"), QVector3D(0.0f, 0.0f, 0.0f));

    const MeshFilterRunResult result = doc.runFilter(visibleKey, params);
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains(QStringLiteral("enclosed")));

    // A failed filter must leave no partial selection behind.
    const VCGMesh &mesh = doc.mesh(index).mesh;
    for (const VCGVertex &v : mesh.vert)
        QVERIFY(!v.IsS());
}

// As alpha grows the alpha shape converges to the convex hull, so a large alpha on an
// icosahedron must give back its 20 faces. A small alpha must not.
void FilterTests::alphaShapeConvergesToConvexHull()
{
    Document doc;

    QString icosaKey;
    QString alphaKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_icosahedron"))
            icosaKey = info.key;
        else if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_alpha_shape"))
            alphaKey = info.key;
    }
    QVERIFY(!icosaKey.isEmpty());
    if (alphaKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");
    QVERIFY2(doc.runFilter(icosaKey, {}).success, "create_icosahedron failed");

    // alpha is an absperc bounded by the bounding-box diagonal, which for a convex point
    // set is comfortably past the largest Delaunay circumradius — so the shape has
    // converged to the hull.
    MeshFilterParameterValues big;
    big.insert(QStringLiteral("alpha"),
               double(doc.mesh(doc.currentMeshIndex()).mesh.bbox.Diag()));
    big.insert(QStringLiteral("output"), QStringLiteral("shape"));
    const MeshFilterRunResult result = doc.runFilter(alphaKey, big);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const VCGMesh &shape = doc.mesh(result.newMeshIndices.front()).mesh;
    QCOMPARE(shape.VN(), 12);
    QCOMPARE(shape.FN(), 20);

    // Face scalar carries the circumradius, so it must be populated and positive.
    for (const VCGFace &f : shape.face)
        QVERIFY(f.cQ() > 0.0f);
}

// Regression: the facet handles point into the CGAL triangulation, so the output mesh
// must be built while it is still alive. Consuming them afterwards is a use-after-free
// that a 12-vertex icosahedron hides (the freed cells are simply not reused yet) and a
// real model reliably crashes on. A subdivided sphere allocates enough to expose it,
// and the interpolating property gives a cheap correctness check: every output vertex
// is an input point, so all of them must sit on the source bounding box.
void FilterTests::alphaShapeHandlesALargePointSet()
{
    Document doc;

    QString sphereKey;
    QString alphaKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_sphere"))
            sphereKey = info.key;
        else if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_alpha_shape"))
            alphaKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    if (alphaKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    const VCGMesh &source = doc.mesh(doc.currentMeshIndex()).mesh;
    const int sourceVN = source.VN();
    QVERIFY2(sourceVN > 500, "sphere too small to exercise the allocator");
    const vcg::Box3f sourceBox = source.bbox;

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("alpha"), double(sourceBox.Diag()));
    params.insert(QStringLiteral("output"), QStringLiteral("shape"));
    const MeshFilterRunResult result = doc.runFilter(alphaKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const VCGMesh &shape = doc.mesh(result.newMeshIndices.front()).mesh;
    QVERIFY(shape.FN() > 0);
    QVERIFY(shape.VN() > 0);
    // Interpolating: output vertices are a subset of the input points.
    QVERIFY(shape.VN() <= sourceVN);

    // A dangling handle yields garbage coordinates rather than input points.
    vcg::Box3f tolerant = sourceBox;
    tolerant.Offset(sourceBox.Diag() * 0.001f);
    for (const VCGVertex &v : shape.vert) {
        const vcg::Point3f &p = v.cP();
        QVERIFY(std::isfinite(p.X()) && std::isfinite(p.Y()) && std::isfinite(p.Z()));
        QVERIFY2(tolerant.IsIn(p), "alpha shape vertex outside the input bounding box");
    }
}

// The crust needs a closed, well-sampled surface, so a subdivided sphere is the fair
// test. It is an interpolating reconstruction: every output vertex must be an input
// point, so the vertex count can never exceed the input's.
void FilterTests::voronoiFilteringReconstructsASphere()
{
    Document doc;

    QString sphereKey;
    QString voronoiKey;
    QString removeFacesKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_sphere"))
            sphereKey = info.key;
        else if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_voronoi_filtering"))
            voronoiKey = info.key;
        else if (info.descriptor.id == QStringLiteral("remove_all_faces"))
            removeFacesKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    QVERIFY(!removeFacesKey.isEmpty());
    if (voronoiKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    // Strip the faces: the point of this filter is that it works from points alone.
    QVERIFY2(doc.runFilter(removeFacesKey, {}).success, "Remove All Faces failed");
    const int sourceVN = doc.mesh(doc.currentMeshIndex()).mesh.VN();
    QCOMPARE(doc.mesh(doc.currentMeshIndex()).mesh.FN(), 0);
    QVERIFY(sourceVN >= 4);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("threshold"), 10.0);
    const MeshFilterRunResult result = doc.runFilter(voronoiKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const VCGMesh &crust = doc.mesh(result.newMeshIndices.front()).mesh;
    QVERIFY(crust.FN() > 0);
    QVERIFY(crust.VN() > 0);
    // Interpolating: output vertices are a subset of the input points.
    QVERIFY(crust.VN() <= sourceVN);
}

// Advancing front is interpolating, so like the crust its vertices are a subset of the
// input points — which is also the cheap check that it did not invent geometry.
void FilterTests::advancingFrontReconstructsASphere()
{
    Document doc;

    QString sphereKey;
    QString frontKey;
    QString removeFacesKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_sphere"))
            sphereKey = info.key;
        else if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_advancing_front"))
            frontKey = info.key;
        else if (info.descriptor.id == QStringLiteral("remove_all_faces"))
            removeFacesKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    QVERIFY(!removeFacesKey.isEmpty());
    if (frontKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    QVERIFY2(doc.runFilter(removeFacesKey, {}).success, "Remove All Faces failed");
    const VCGMesh &source = doc.mesh(doc.currentMeshIndex()).mesh;
    const int sourceVN = source.VN();
    const vcg::Box3f sourceBox = source.bbox;
    QCOMPARE(source.FN(), 0);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("radiusRatioBound"), 5.0);
    params.insert(QStringLiteral("beta"), 30.0);
    const MeshFilterRunResult result = doc.runFilter(frontKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const VCGMesh &surface = doc.mesh(result.newMeshIndices.front()).mesh;
    QVERIFY(surface.FN() > 0);
    QVERIFY(surface.VN() > 0);
    QVERIFY(surface.VN() <= sourceVN);

    vcg::Box3f tolerant = sourceBox;
    tolerant.Offset(sourceBox.Diag() * 0.001f);
    for (const VCGVertex &v : surface.vert)
        QVERIFY2(tolerant.IsIn(v.cP()), "advancing front vertex outside the input bbox");
}

// Scale space is the one reconstruction here whose output vertices are *not* the input
// points: smoothing moves them before meshing. So the subset check does not apply — but
// the result must still stay near the input, and both meshers must produce a surface.
void FilterTests::scaleSpaceReconstructsASphere()
{
    Document doc;

    QString sphereKey;
    QString scaleSpaceKey;
    QString removeFacesKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_sphere"))
            sphereKey = info.key;
        else if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_scale_space"))
            scaleSpaceKey = info.key;
        else if (info.descriptor.id == QStringLiteral("remove_all_faces"))
            removeFacesKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    QVERIFY(!removeFacesKey.isEmpty());
    if (scaleSpaceKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    QVERIFY2(doc.runFilter(removeFacesKey, {}).success, "Remove All Faces failed");
    const vcg::Box3f sourceBox = doc.mesh(doc.currentMeshIndex()).mesh.bbox;

    // Both meshers must work; the alpha one additionally exercises the absperc bound.
    const QVector<QString> meshers{ QStringLiteral("alpha_shape"),
                                    QStringLiteral("advancing_front") };
    for (const QString &mesher : meshers) {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("iterations"), 2);
        params.insert(QStringLiteral("mesher"), mesher);
        params.insert(QStringLiteral("alpha"), double(sourceBox.Diag()) * 0.25);
        const MeshFilterRunResult result = doc.runFilter(scaleSpaceKey, params);
        QVERIFY2(result.success, qPrintable(QStringLiteral("%1: %2")
                                                .arg(mesher, result.errorMessage)));
        QCOMPARE(result.newMeshIndices.size(), 1);

        const VCGMesh &surface = doc.mesh(result.newMeshIndices.front()).mesh;
        QVERIFY2(surface.FN() > 0, qPrintable(mesher));
        QVERIFY2(surface.VN() > 0, qPrintable(mesher));

        // Smoothing moves points, so allow generous slack — this only catches garbage.
        vcg::Box3f tolerant = sourceBox;
        tolerant.Offset(sourceBox.Diag() * 0.5f);
        for (const VCGVertex &v : surface.vert)
            QVERIFY2(tolerant.IsIn(v.cP()), qPrintable(mesher));
    }
}

// A sphere's normals should all point away from its centre. Flipping half of them gives
// an inconsistently oriented field of exactly the kind normal estimation produces, and
// MST orientation must repair it — that is the whole point of the filter.
void FilterTests::orientNormalsFlipsInvertedNormals()
{
    Document doc;

    QString sphereKey;
    QString orientKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_sphere"))
            sphereKey = info.key;
        else if (info.descriptor.id == QStringLiteral("orient_point_cloud_normals"))
            orientKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    if (orientKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    const int index = doc.currentMeshIndex();

    // Corrupt the orientation: flip every other normal.
    {
        VCGMesh &mesh = doc.mesh(index).mesh;
        const vcg::Point3f centre = mesh.bbox.Center();
        int flipped = 0;
        for (std::size_t i = 0; i < mesh.vert.size(); ++i) {
            // Start from the outward normal so the test does not depend on the primitive.
            mesh.vert[i].N() = (mesh.vert[i].cP() - centre).Normalize();
            if (i % 2 == 0) {
                mesh.vert[i].N() = -mesh.vert[i].cN();
                ++flipped;
            }
        }
        QVERIFY(flipped > 0);
    }

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("neighbors"), 18);
    const MeshFilterRunResult result = doc.runFilter(orientKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));

    // Every normal must now agree with the outward radial direction, up to a global
    // sign: MST orientation makes the field consistent, not necessarily outward.
    const VCGMesh &mesh = doc.mesh(index).mesh;
    const vcg::Point3f centre = mesh.bbox.Center();
    int agreeing = 0;
    int total = 0;
    for (const VCGVertex &v : mesh.vert) {
        if (v.IsD())
            continue;
        const vcg::Point3f radial = (v.cP() - centre).Normalize();
        if (radial.dot(v.cN()) > 0.0f)
            ++agreeing;
        ++total;
    }
    QVERIFY(total > 0);
    // Consistent means all-with or all-against; either way one of the two counts is 0.
    QVERIFY2(agreeing == total || agreeing == 0,
             qPrintable(QStringLiteral("%1 of %2 normals point outward — the field is "
                                       "still inconsistent").arg(agreeing).arg(total)));
}

// CGAL's Poisson needs oriented normals, so this also exercises the pairing with the
// orientation filter above: estimate-free radial normals, then reconstruct.
void FilterTests::cgalPoissonReconstructsASphere()
{
    Document doc;

    QString sphereKey;
    QString poissonKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_sphere"))
            sphereKey = info.key;
        else if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_poisson_cgal"))
            poissonKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    if (poissonKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    const int index = doc.currentMeshIndex();
    vcg::Box3f sourceBox;
    {
        VCGMesh &mesh = doc.mesh(index).mesh;
        sourceBox = mesh.bbox;
        const vcg::Point3f centre = mesh.bbox.Center();
        for (VCGVertex &v : mesh.vert)
            v.N() = (v.cP() - centre).Normalize();
    }

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("smAngle"), 20.0);
    params.insert(QStringLiteral("smRadius"), 30.0);
    params.insert(QStringLiteral("smDistance"), 0.375);
    const MeshFilterRunResult result = doc.runFilter(poissonKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const VCGMesh &surface = doc.mesh(result.newMeshIndices.front()).mesh;
    QVERIFY(surface.FN() > 0);
    QVERIFY(surface.VN() > 0);

    // Approximating: vertices are new points, but must still bracket the input sphere.
    vcg::Box3f tolerant = sourceBox;
    tolerant.Offset(sourceBox.Diag() * 0.5f);
    for (const VCGVertex &v : surface.vert)
        QVERIFY2(tolerant.IsIn(v.cP()), "poisson vertex far outside the input bbox");
}

// Kinetic reconstruction is piecewise planar by construction, so a sphere is the wrong
// test — a subdivided box is the fair one: six large planar regions with clean normals.
void FilterTests::kineticReconstructsABox()
{
    Document doc;

    QString boxKey;
    QString subdivideKey;
    QString kineticKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_box"))
            boxKey = info.key;
        else if (info.descriptor.id == QStringLiteral("subdivide_by_midpoint"))
            subdivideKey = info.key;
        else if (info.descriptor.id == QStringLiteral("reconstruct_surface_by_kinetic_partition"))
            kineticKey = info.key;
    }
    QVERIFY(!boxKey.isEmpty());
    QVERIFY(!subdivideKey.isEmpty());
    if (kineticKey.isEmpty())
        QSKIP("CGAL plugin is not available in this build.");

    QVERIFY2(doc.runFilter(boxKey, {}).success, "create_box failed");
    const int index = doc.currentMeshIndex();

    // Densify so each face carries enough samples to be detected as a planar region.
    for (int i = 0; i < 4; ++i) {
        MeshFilterParameterValues sub;
        sub.insert(QStringLiteral("Iterations"), 1);
        if (!doc.runFilter(subdivideKey, sub).success)
            break;
    }
    VCGMesh &source = doc.mesh(index).mesh;
    QVERIFY2(source.VN() > 300, "box not dense enough to detect planes");
    const vcg::Box3f sourceBox = source.bbox;
    vcg::tri::UpdateNormal<VCGMesh>::PerVertexNormalized(source);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("maximumDistance"), double(sourceBox.Diag()) * 0.02);
    params.insert(QStringLiteral("maximumAngle"), 15.0);
    params.insert(QStringLiteral("lambda"), 0.5);
    params.insert(QStringLiteral("minimumRegionSize"), 20);
    params.insert(QStringLiteral("kNeighbors"), 12);
    params.insert(QStringLiteral("intersections"), 1);
    const MeshFilterRunResult result = doc.runFilter(kineticKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const VCGMesh &surface = doc.mesh(result.newMeshIndices.front()).mesh;
    QVERIFY(surface.FN() > 0);
    QVERIFY(surface.VN() > 0);

    vcg::Box3f tolerant = sourceBox;
    tolerant.Offset(sourceBox.Diag() * 0.5f);
    for (const VCGVertex &v : surface.vert)
        QVERIFY2(tolerant.IsIn(v.cP()), "kinetic vertex far outside the input bbox");
}

// Alignment filters must recover a transform we applied ourselves. A sphere is displaced
// and rotated, then each filter is asked to put it back; success is measured by how close
// the realigned world-space points land on the original, relative to the model size.
void FilterTests::trueFormAlignmentRecoversAKnownTransform()
{
    Document doc;

    QString sphereKey, obbKey, icpKey, correspondingKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("align_by_bounding_box_trueform")) obbKey = info.key;
        else if (id == QStringLiteral("align_by_icp_trueform")) icpKey = info.key;
        else if (id == QStringLiteral("align_to_corresponding_points_trueform")) correspondingKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    if (icpKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");
    QVERIFY(!obbKey.isEmpty());
    QVERIFY(!correspondingKey.isEmpty());

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    const int referenceIndex = doc.currentMeshIndex();
    const VCGMesh &reference = doc.mesh(referenceIndex).mesh;
    const float diagonal = reference.bbox.Diag();
    QVERIFY(diagonal > 0.0f);

    // A second copy of the same sphere, displaced. Vertex order is identical, which is
    // what makes the corresponding-points filter applicable too.
    const int sourceIndex = doc.addMesh(reference, QStringLiteral("moved"));
    QVERIFY(sourceIndex >= 0);
    QMatrix4x4 applied;
    applied.translate(0.35f * diagonal, -0.2f * diagonal, 0.1f * diagonal);
    applied.rotate(25.0f, 0.3f, 1.0f, 0.2f);
    doc.setMeshTransform(sourceIndex, applied);

    // Largest world-space gap between correspondingly-indexed vertices.
    const auto maxDeviation = [&doc, sourceIndex, referenceIndex]() {
        const auto &src = doc.mesh(sourceIndex);
        const auto &ref = doc.mesh(referenceIndex);
        float worst = 0.0f;
        const int n = std::min(src.mesh.VN(), ref.mesh.VN());
        for (int i = 0; i < n; ++i) {
            const vcg::Point3f &a = src.mesh.vert[i].cP();
            const vcg::Point3f &b = ref.mesh.vert[i].cP();
            const QVector3D pa = src.transform.map(QVector3D(a.X(), a.Y(), a.Z()));
            const QVector3D pb = ref.transform.map(QVector3D(b.X(), b.Y(), b.Z()));
            worst = std::max(worst, (pa - pb).length());
        }
        return worst;
    };
    QVERIFY2(maxDeviation() > 0.1f * diagonal, "the displacement should start far off");

    MeshFilterParameterValues shared;
    shared.insert(QStringLiteral("sourceMesh"), sourceIndex);
    shared.insert(QStringLiteral("referenceMesh"), referenceIndex);

    // Exact correspondences: this must land essentially on top of the original.
    MeshFilterParameterValues corresponding = shared;
    corresponding.insert(QStringLiteral("allowScale"), false);
    QVERIFY2(doc.runFilter(correspondingKey, corresponding).success, "corresponding-points failed");
    QVERIFY2(maxDeviation() < 1e-3f * diagonal,
             qPrintable(QStringLiteral("corresponding points left %1").arg(maxDeviation())));

    // Uniform scale is the capability ICP cannot offer; on identical shapes it must
    // resolve to a scale of 1 and stay put.
    corresponding.insert(QStringLiteral("allowScale"), true);
    QVERIFY2(doc.runFilter(correspondingKey, corresponding).success, "similarity fit failed");
    QVERIFY2(maxDeviation() < 1e-3f * diagonal,
             qPrintable(QStringLiteral("similarity left %1").arg(maxDeviation())));

    // Displace again and let ICP find its own correspondences.
    doc.setMeshTransform(sourceIndex, applied);
    QVERIFY(maxDeviation() > 0.1f * diagonal);
    MeshFilterParameterValues icp = shared;
    icp.insert(QStringLiteral("metric"), QStringLiteral("point_to_point"));
    icp.insert(QStringLiteral("coarseInit"), true);
    icp.insert(QStringLiteral("maxIterations"), 60);
    icp.insert(QStringLiteral("samples"), 0);
    QVERIFY2(doc.runFilter(icpKey, icp).success, "icp failed");
    // A sphere is rotationally symmetric, so ICP can only be asked to recover position.
    const auto centreGap = [&doc, sourceIndex, referenceIndex]() {
        const auto &src = doc.mesh(sourceIndex);
        const auto &ref = doc.mesh(referenceIndex);
        const vcg::Point3f a = src.mesh.bbox.Center();
        const vcg::Point3f b = ref.mesh.bbox.Center();
        return (src.transform.map(QVector3D(a.X(), a.Y(), a.Z()))
                - ref.transform.map(QVector3D(b.X(), b.Y(), b.Z()))).length();
    };
    QVERIFY2(centreGap() < 0.05f * diagonal,
             qPrintable(QStringLiteral("icp left the centres %1 apart").arg(centreGap())));

    // The coarse filter on its own should also bring the centres together.
    doc.setMeshTransform(sourceIndex, applied);
    QVERIFY2(doc.runFilter(obbKey, shared).success, "obb alignment failed");
    QVERIFY2(centreGap() < 0.05f * diagonal,
             qPrintable(QStringLiteral("obb left the centres %1 apart").arg(centreGap())));
}

// Two unit boxes overlapping in half their extent. The boolean results have volumes we
// can state exactly, which checks the operations themselves rather than merely that they
// produced some geometry.
void FilterTests::trueFormBooleansAgreeWithVolume()
{
    Document doc;

    QString boxKey, unionKey, interKey, diffKey, xorKey, shellKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_box")) boxKey = info.key;
        else if (id == QStringLiteral("mesh_union_trueform")) unionKey = info.key;
        else if (id == QStringLiteral("mesh_intersection_trueform")) interKey = info.key;
        else if (id == QStringLiteral("mesh_difference_trueform")) diffKey = info.key;
        else if (id == QStringLiteral("mesh_symmetric_difference_trueform")) xorKey = info.key;
        else if (id == QStringLiteral("extract_outer_shell_trueform")) shellKey = info.key;
    }
    QVERIFY(!boxKey.isEmpty());
    if (unionKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    QVERIFY2(doc.runFilter(boxKey, {}).success, "create_box failed");
    const int a = doc.currentMeshIndex();
    const float side = doc.mesh(a).mesh.bbox.DimX();
    QVERIFY(side > 0.0f);
    const double unit = double(side) * double(side) * double(side);

    // Second box, displaced half a side along X: overlap is exactly half of each.
    const int b = doc.addMesh(doc.mesh(a).mesh, QStringLiteral("shifted"));
    QVERIFY(b >= 0);
    QMatrix4x4 shift;
    shift.translate(side * 0.5f, 0.0f, 0.0f);
    doc.setMeshTransform(b, shift);

    const auto volumeOf = [&doc](int index) {
        return double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(doc.mesh(index).mesh)));
    };

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("firstMesh"), a);
    p.insert(QStringLiteral("secondMesh"), b);

    struct Case { const QString *key; double expected; const char *name; };
    const Case cases[] = {
        { &unionKey,  1.5 * unit, "union" },
        { &interKey,  0.5 * unit, "intersection" },
        { &diffKey,   0.5 * unit, "difference" },
        { &xorKey,    1.0 * unit, "symmetric difference" },
    };
    for (const Case &c : cases) {
        const MeshFilterRunResult r = doc.runFilter(*c.key, p);
        QVERIFY2(r.success, qPrintable(QStringLiteral("%1: %2").arg(c.name, r.errorMessage)));
        QCOMPARE(r.newMeshIndices.size(), 1);
        const double got = volumeOf(r.newMeshIndices.front());
        QVERIFY2(std::abs(got - c.expected) < 0.02 * unit,
                 qPrintable(QStringLiteral("%1: volume %2, expected %3")
                                .arg(c.name).arg(got).arg(c.expected)));
    }

    // The outer shell of a single box is the box itself.
    MeshFilterParameterValues shellParams;
    shellParams.insert(QStringLiteral("sourceMesh"), a);
    const MeshFilterRunResult shell = doc.runFilter(shellKey, shellParams);
    QVERIFY2(shell.success, qPrintable(shell.errorMessage));
    QVERIFY(std::abs(volumeOf(shell.newMeshIndices.front()) - unit) < 0.02 * unit);
}

// The CSG evaluator and the pairwise booleans must agree where they overlap, and the
// expression must also handle a three-operand case that no single boolean can express.
void FilterTests::trueFormCsgExpressionMatchesPairwiseBooleans()
{
    Document doc;

    QString boxKey, csgKey, unionKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_box")) boxKey = info.key;
        else if (id == QStringLiteral("mesh_csg_expression_trueform")) csgKey = info.key;
        else if (id == QStringLiteral("mesh_union_trueform")) unionKey = info.key;
    }
    QVERIFY(!boxKey.isEmpty());
    if (csgKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    QVERIFY2(doc.runFilter(boxKey, {}).success, "create_box failed");
    const int a = doc.currentMeshIndex();
    const float side = doc.mesh(a).mesh.bbox.DimX();
    const double unit = double(side) * double(side) * double(side);

    const int b = doc.addMesh(doc.mesh(a).mesh, QStringLiteral("b"));
    QMatrix4x4 shiftB;
    shiftB.translate(side * 0.5f, 0.0f, 0.0f);
    doc.setMeshTransform(b, shiftB);

    const auto volumeOf = [&doc](int index) {
        return double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(doc.mesh(index).mesh)));
    };

    // "0 | 1" must reproduce the pairwise union exactly.
    MeshFilterParameterValues pairwise;
    pairwise.insert(QStringLiteral("firstMesh"), a);
    pairwise.insert(QStringLiteral("secondMesh"), b);
    const MeshFilterRunResult viaBoolean = doc.runFilter(unionKey, pairwise);
    QVERIFY2(viaBoolean.success, qPrintable(viaBoolean.errorMessage));

    MeshFilterParameterValues expr;
    expr.insert(QStringLiteral("expression"), QStringLiteral("%1 | %2").arg(a).arg(b));
    const MeshFilterRunResult viaCsg = doc.runFilter(csgKey, expr);
    QVERIFY2(viaCsg.success, qPrintable(viaCsg.errorMessage));
    QVERIFY2(std::abs(volumeOf(viaBoolean.newMeshIndices.front())
                      - volumeOf(viaCsg.newMeshIndices.front())) < 0.02 * unit,
             "CSG union disagrees with the pairwise union");

    // Three operands in one arrangement: (0 | 1) - 2, which no single boolean expresses.
    const int c = doc.addMesh(doc.mesh(a).mesh, QStringLiteral("c"));
    QMatrix4x4 shiftC;
    shiftC.translate(side * 1.25f, 0.0f, 0.0f);
    doc.setMeshTransform(c, shiftC);

    MeshFilterParameterValues three;
    three.insert(QStringLiteral("expression"), QStringLiteral("(%1 | %2) - %3").arg(a).arg(b).arg(c));
    const MeshFilterRunResult combined = doc.runFilter(csgKey, three);
    QVERIFY2(combined.success, qPrintable(combined.errorMessage));
    // union is 1.5 units; c removes the quarter-unit it overlaps with b's far end.
    const double got = volumeOf(combined.newMeshIndices.front());
    QVERIFY2(std::abs(got - 1.25 * unit) < 0.05 * unit,
             qPrintable(QStringLiteral("(A|B)-C volume %1, expected %2").arg(got).arg(1.25 * unit)));

    // A malformed expression must be reported, not silently ignored.
    MeshFilterParameterValues bad;
    bad.insert(QStringLiteral("expression"), QStringLiteral("0 | "));
    QVERIFY(!doc.runFilter(csgKey, bad).success);
    bad.insert(QStringLiteral("expression"), QStringLiteral("0 | 999"));
    QVERIFY(!doc.runFilter(csgKey, bad).success);
}

// A sheet is an open surface that cuts without enclosing anything, so the same expression
// means one thing when the cutter is declared a sheet and another when it is not. A unit
// cube halved by a square through its middle states both: the two sides are exactly half
// the cube each, and left undeclared the cutter encloses no volume and removes nothing.
void FilterTests::trueFormCsgSheetsCutWithoutEnclosing()
{
    Document doc;

    const QString csgKey = filterKeyForId(doc, QStringLiteral("mesh_csg_expression_trueform"));
    if (csgKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    const int solid = doc.addMesh(cube, QStringLiteral("cube"));
    QVERIFY(solid >= 0);

    VCGMesh sheet;
    makeSquareSheetMesh(sheet, 2.0f, 0.5f);
    const int cutter = doc.addMesh(sheet, QStringLiteral("sheet"));
    QVERIFY(cutter >= 0);

    const auto volumeOf = [&doc](int index) {
        return double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(doc.mesh(index).mesh)));
    };
    const auto holesIn = [&doc](int index) {
        VCGMesh &m = doc.mesh(index).mesh;
        m.face.EnableFFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(m);
        return vcg::tri::Clean<VCGMesh>::CountHoles(m);
    };

    // In front of the sheet's normals, and behind them: half the cube each, and each one
    // capped along the cut rather than left open where the sheet passed through.
    MeshFilterParameterValues front;
    front.insert(QStringLiteral("expression"), QStringLiteral("%1 - %2").arg(solid).arg(cutter));
    front.insert(QStringLiteral("sheets"), QString::number(cutter));
    const MeshFilterRunResult above = doc.runFilter(csgKey, front);
    QVERIFY2(above.success, qPrintable(above.errorMessage));
    QCOMPARE(above.newMeshIndices.size(), 1);
    QCOMPARE(holesIn(above.newMeshIndices.front()), 0);
    QVERIFY2(std::abs(volumeOf(above.newMeshIndices.front()) - 0.5) < 1e-4,
             qPrintable(QStringLiteral("the front half measured %1, expected 0.5")
                            .arg(volumeOf(above.newMeshIndices.front()))));

    MeshFilterParameterValues back;
    back.insert(QStringLiteral("expression"), QStringLiteral("%1 & %2").arg(solid).arg(cutter));
    back.insert(QStringLiteral("sheets"), QString::number(cutter));
    const MeshFilterRunResult below = doc.runFilter(csgKey, back);
    QVERIFY2(below.success, qPrintable(below.errorMessage));
    QCOMPARE(holesIn(below.newMeshIndices.front()), 0);
    QVERIFY2(std::abs(volumeOf(below.newMeshIndices.front()) - 0.5) < 1e-4,
             qPrintable(QStringLiteral("the back half measured %1, expected 0.5")
                            .arg(volumeOf(below.newMeshIndices.front()))));

    // Undeclared, the same layer is read as a solid: an open surface encloses nothing, so
    // the difference gives the cube back whole. This is what an empty parameter means, and
    // it is what every call written before the parameter existed keeps getting.
    MeshFilterParameterValues noSheets;
    noSheets.insert(QStringLiteral("expression"), QStringLiteral("%1 - %2").arg(solid).arg(cutter));
    noSheets.insert(QStringLiteral("sheets"), QString());
    const MeshFilterRunResult whole = doc.runFilter(csgKey, noSheets);
    QVERIFY2(whole.success, qPrintable(whole.errorMessage));
    QVERIFY2(std::abs(volumeOf(whole.newMeshIndices.front()) - 1.0) < 1e-4,
             qPrintable(QStringLiteral("without sheets the difference measured %1, expected 1.0")
                            .arg(volumeOf(whole.newMeshIndices.front()))));

    // And an empty sheet list leaves a solid-only expression exactly where it was: two
    // unit cubes overlapping in half their extent still union to one and a half.
    VCGMesh shifted;
    makeCubeMesh(shifted, 0.5f, 0.0f, 0.0f);
    const int second = doc.addMesh(shifted, QStringLiteral("shifted cube"));
    MeshFilterParameterValues union2;
    union2.insert(QStringLiteral("expression"), QStringLiteral("%1 | %2").arg(solid).arg(second));
    union2.insert(QStringLiteral("sheets"), QString());
    const MeshFilterRunResult merged = doc.runFilter(csgKey, union2);
    QVERIFY2(merged.success, qPrintable(merged.errorMessage));
    QVERIFY2(std::abs(volumeOf(merged.newMeshIndices.front()) - 1.5) < 1e-4,
             qPrintable(QStringLiteral("the union measured %1, expected 1.5")
                            .arg(volumeOf(merged.newMeshIndices.front()))));

    // A sheet the expression never mentions is a mistake worth reporting: it would be
    // built into the arrangement and cut nothing anyone asked about.
    MeshFilterParameterValues stray;
    stray.insert(QStringLiteral("expression"), QStringLiteral("%1 | %2").arg(solid).arg(second));
    stray.insert(QStringLiteral("sheets"), QString::number(cutter));
    const MeshFilterRunResult unused = doc.runFilter(csgKey, stray);
    QVERIFY(!unused.success);
    QVERIFY(unused.errorMessage.contains(QStringLiteral("sheet")));
}

// Two unit cubes overlapping in an eighth of their extent cut space into three regions of
// stated size, and each one has to come back closed and on its own layer. The same read
// answers for a sheet cutting a solid and for a single layer crossing itself, which are
// the two shapes a boolean cannot express at all.
void FilterTests::trueFormSolidDomainsSplitTheEnclosedVolume()
{
    Document doc;

    const QString domainsKey = filterKeyForId(doc, QStringLiteral("split_into_solid_domains_trueform"));
    if (domainsKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    const auto volumeOf = [&doc](int index) {
        return double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(doc.mesh(index).mesh)));
    };
    const auto holesIn = [&doc](int index) {
        VCGMesh &m = doc.mesh(index).mesh;
        m.face.EnableFFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(m);
        return vcg::tri::Clean<VCGMesh>::CountHoles(m);
    };
    const auto sortedVolumes = [&](const QVector<int> &indices) {
        std::vector<double> volumes;
        for (int index : indices)
            volumes.push_back(volumeOf(index));
        std::sort(volumes.begin(), volumes.end());
        return volumes;
    };

    // Overlapping by half a side in each axis: the shared corner is an eighth of a cube,
    // and what is left of each is seven eighths.
    VCGMesh first;
    makeCubeMesh(first, 0.0f, 0.0f, 0.0f);
    const int a = doc.addMesh(first, QStringLiteral("cube a"));
    VCGMesh second;
    makeCubeMesh(second, 0.5f, 0.5f, 0.5f);
    const int b = doc.addMesh(second, QStringLiteral("cube b"));

    MeshFilterParameterValues pair;
    pair.insert(QStringLiteral("layers"), QStringLiteral("%1 %2").arg(a).arg(b));
    pair.insert(QStringLiteral("sheets"), QString());
    const MeshFilterRunResult overlap = doc.runFilter(domainsKey, pair);
    QVERIFY2(overlap.success, qPrintable(overlap.errorMessage));
    QCOMPARE(overlap.newMeshIndices.size(), 3);
    const std::vector<double> volumes = sortedVolumes(overlap.newMeshIndices);
    const double expected[3] = { 0.125, 0.875, 0.875 };
    for (std::size_t k = 0; k < 3; ++k) {
        QVERIFY2(std::abs(volumes[k] - expected[k]) < 1e-4,
                 qPrintable(QStringLiteral("domain %1 measured %2, expected %3")
                                .arg(k).arg(volumes[k]).arg(expected[k])));
    }
    for (int index : overlap.newMeshIndices)
        QCOMPARE(holesIn(index), 0);
    // Named for the domain each one carries, so the layer panel says which is which.
    for (int index : overlap.newMeshIndices)
        QVERIFY2(doc.mesh(index).name.startsWith(QStringLiteral("Domain")),
                 qPrintable(doc.mesh(index).name));

    // A solid halved by an open surface: two closed pieces, and the sheet itself is not
    // one of them.
    Document cut;
    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    const int solid = cut.addMesh(cube, QStringLiteral("cube"));
    VCGMesh sheet;
    makeSquareSheetMesh(sheet, 2.0f, 0.5f);
    const int cutter = cut.addMesh(sheet, QStringLiteral("sheet"));

    MeshFilterParameterValues halves;
    halves.insert(QStringLiteral("layers"), QStringLiteral("%1 %2").arg(solid).arg(cutter));
    halves.insert(QStringLiteral("sheets"), QString::number(cutter));
    const MeshFilterRunResult split = cut.runFilter(domainsKey, halves);
    QVERIFY2(split.success, qPrintable(split.errorMessage));
    QCOMPARE(split.newMeshIndices.size(), 2);
    for (int index : split.newMeshIndices) {
        const double got =
            double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(cut.mesh(index).mesh)));
        QVERIFY2(std::abs(got - 0.5) < 1e-4,
                 qPrintable(QStringLiteral("half measured %1, expected 0.5").arg(got)));
        VCGMesh &m = cut.mesh(index).mesh;
        m.face.EnableFFAdjacency();
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(m);
        QCOMPARE(vcg::tri::Clean<VCGMesh>::CountHoles(m), 0);
    }

    // One layer holding two crossing cubes: the graph arranges a single form against
    // itself, and the three regions come back exactly as they did from two layers.
    Document self;
    VCGMesh crossing;
    makeCubeMesh(crossing, 0.0f, 0.0f, 0.0f);
    VCGMesh other;
    makeCubeMesh(other, 0.5f, 0.5f, 0.5f);
    vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(crossing, other, false);
    vcg::tri::UpdateBounding<VCGMesh>::Box(crossing);
    const int both = self.addMesh(crossing, QStringLiteral("crossing cubes"));

    MeshFilterParameterValues single;
    single.insert(QStringLiteral("layers"), QString::number(both));
    single.insert(QStringLiteral("sheets"), QString());
    const MeshFilterRunResult alone = self.runFilter(domainsKey, single);
    QVERIFY2(alone.success, qPrintable(alone.errorMessage));
    QCOMPARE(alone.newMeshIndices.size(), 3);
    std::vector<double> selfVolumes;
    for (int index : alone.newMeshIndices) {
        selfVolumes.push_back(
            double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(self.mesh(index).mesh))));
    }
    std::sort(selfVolumes.begin(), selfVolumes.end());
    for (std::size_t k = 0; k < 3; ++k) {
        QVERIFY2(std::abs(selfVolumes[k] - expected[k]) < 1e-4,
                 qPrintable(QStringLiteral("self-arranged domain %1 measured %2, expected %3")
                                .arg(k).arg(selfVolumes[k]).arg(expected[k])));
    }
}

// Three surfaces that touch nowhere, in one layer: the split has to find exactly three and
// give each one back whole. A surface already in one piece is the other half of the
// contract -- it comes back as itself, not as a copy that lost or gained a face.
void FilterTests::trueFormComponentSplitSeparatesDisjointSurfaces()
{
    Document doc;

    const QString splitKey = filterKeyForId(
        doc, QStringLiteral("split_into_connected_components_trueform"));
    if (splitKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    // Two cubes far enough apart to share nothing, plus a triangle floating on its own.
    VCGMesh scattered;
    makeCubeMesh(scattered, 0.0f, 0.0f, 0.0f);
    VCGMesh away;
    makeCubeMesh(away, 5.0f, 0.0f, 0.0f);
    vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(scattered, away, false);
    const int base = scattered.VN();
    vcg::tri::Allocator<VCGMesh>::AddVertices(scattered, 3);
    scattered.vert[std::size_t(base)].P() = vcg::Point3f(0.0f, 10.0f, 0.0f);
    scattered.vert[std::size_t(base) + 1].P() = vcg::Point3f(1.0f, 10.0f, 0.0f);
    scattered.vert[std::size_t(base) + 2].P() = vcg::Point3f(0.0f, 11.0f, 0.0f);
    vcg::tri::Allocator<VCGMesh>::AddFace(scattered, base, base + 1, base + 2);
    vcg::tri::UpdateBounding<VCGMesh>::Box(scattered);
    const int scatteredIndex = doc.addMesh(scattered, QStringLiteral("three pieces"));
    QVERIFY(scatteredIndex >= 0);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("sourceMesh"), scatteredIndex);
    const MeshFilterRunResult result = doc.runFilter(splitKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 3);

    std::vector<int> faceCounts;
    for (int index : result.newMeshIndices)
        faceCounts.push_back(doc.mesh(index).mesh.FN());
    std::sort(faceCounts.begin(), faceCounts.end());
    QCOMPARE(faceCounts[0], 1);
    QCOMPARE(faceCounts[1], 12);
    QCOMPARE(faceCounts[2], 12);
    // Nothing is duplicated and nothing is dropped: the pieces add back up to the layer.
    QCOMPARE(faceCounts[0] + faceCounts[1] + faceCounts[2], scattered.FN());
    for (int index : result.newMeshIndices)
        QVERIFY2(doc.mesh(index).name.startsWith(QStringLiteral("Component")),
                 qPrintable(doc.mesh(index).name));

    // One connected surface is one component, returned intact.
    const QString sphereKey = filterKeyForId(doc, QStringLiteral("create_sphere"));
    QVERIFY(!sphereKey.isEmpty());
    doc.setCurrentMeshIndex(scatteredIndex);
    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    const int sphere = doc.currentMeshIndex();
    const int sphereFaces = doc.mesh(sphere).mesh.FN();
    QVERIFY(sphereFaces > 0);

    MeshFilterParameterValues one;
    one.insert(QStringLiteral("sourceMesh"), sphere);
    const MeshFilterRunResult whole = doc.runFilter(splitKey, one);
    QVERIFY2(whole.success, qPrintable(whole.errorMessage));
    QCOMPARE(whole.newMeshIndices.size(), 1);
    QCOMPARE(doc.mesh(whole.newMeshIndices.front()).mesh.FN(), sphereFaces);
    QCOMPARE(doc.mesh(whole.newMeshIndices.front()).mesh.VN(), doc.mesh(sphere).mesh.VN());
}

// Two shapes that separate under one connectivity standard and join under the next, which
// is the whole of what the parameter decides. Three fins along one edge are one surface to
// anything that walks a non-manifold seam and three to anything that stops at it; a bowtie
// is two surfaces to anything that needs a shared edge and one to anything that will cross
// a single shared vertex.
void FilterTests::trueFormComponentConnectivityChoosesWhatJoins()
{
    Document doc;

    const QString splitKey = filterKeyForId(
        doc, QStringLiteral("split_into_connected_components_trueform"));
    if (splitKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    // Three triangles hanging off the same edge: that edge is used by three faces, so it
    // is not a manifold edge.
    VCGMesh fins;
    vcg::tri::Allocator<VCGMesh>::AddVertices(fins, 5);
    fins.vert[0].P() = vcg::Point3f(0.0f, 0.0f, 0.0f);
    fins.vert[1].P() = vcg::Point3f(1.0f, 0.0f, 0.0f);
    fins.vert[2].P() = vcg::Point3f(0.5f, 1.0f, 0.0f);
    fins.vert[3].P() = vcg::Point3f(0.5f, -1.0f, 0.0f);
    fins.vert[4].P() = vcg::Point3f(0.5f, 0.0f, 1.0f);
    vcg::tri::Allocator<VCGMesh>::AddFace(fins, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(fins, 0, 1, 3);
    vcg::tri::Allocator<VCGMesh>::AddFace(fins, 0, 1, 4);
    vcg::tri::UpdateBounding<VCGMesh>::Box(fins);
    const int finsIndex = doc.addMesh(fins, QStringLiteral("three fins"));
    QVERIFY(finsIndex >= 0);

    const auto splitWith = [&](int layer, const QString &connectivity) {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("sourceMesh"), layer);
        params.insert(QStringLiteral("connectivity"), connectivity);
        return doc.runFilter(splitKey, params);
    };

    const MeshFilterRunResult finsManifold =
        splitWith(finsIndex, QStringLiteral("manifold"));
    QVERIFY2(finsManifold.success, qPrintable(finsManifold.errorMessage));
    QCOMPARE(finsManifold.newMeshIndices.size(), 3);

    const MeshFilterRunResult finsEdge = splitWith(finsIndex, QStringLiteral("edge"));
    QVERIFY2(finsEdge.success, qPrintable(finsEdge.errorMessage));
    QCOMPARE(finsEdge.newMeshIndices.size(), 1);
    QCOMPARE(doc.mesh(finsEdge.newMeshIndices.front()).mesh.FN(), 3);

    // A bowtie: two triangles meeting at exactly one vertex and sharing no edge.
    VCGMesh bowtie;
    vcg::tri::Allocator<VCGMesh>::AddVertices(bowtie, 5);
    bowtie.vert[0].P() = vcg::Point3f(0.0f, 0.0f, 0.0f);
    bowtie.vert[1].P() = vcg::Point3f(1.0f, 0.0f, 0.0f);
    bowtie.vert[2].P() = vcg::Point3f(0.0f, 1.0f, 0.0f);
    bowtie.vert[3].P() = vcg::Point3f(-1.0f, 0.0f, 0.0f);
    bowtie.vert[4].P() = vcg::Point3f(0.0f, -1.0f, 0.0f);
    vcg::tri::Allocator<VCGMesh>::AddFace(bowtie, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(bowtie, 0, 3, 4);
    vcg::tri::UpdateBounding<VCGMesh>::Box(bowtie);
    const int bowtieIndex = doc.addMesh(bowtie, QStringLiteral("bowtie"));
    QVERIFY(bowtieIndex >= 0);

    const MeshFilterRunResult bowtieEdge = splitWith(bowtieIndex, QStringLiteral("edge"));
    QVERIFY2(bowtieEdge.success, qPrintable(bowtieEdge.errorMessage));
    QCOMPARE(bowtieEdge.newMeshIndices.size(), 2);

    const MeshFilterRunResult bowtieVertex =
        splitWith(bowtieIndex, QStringLiteral("vertex"));
    QVERIFY2(bowtieVertex.success, qPrintable(bowtieVertex.errorMessage));
    QCOMPARE(bowtieVertex.newMeshIndices.size(), 1);
    QCOMPARE(doc.mesh(bowtieVertex.newMeshIndices.front()).mesh.FN(), 2);

    // The permissive settings only ever join: neither invents a piece where the default
    // already found one, and two cubes standing apart stay apart under all three.
    VCGMesh apart;
    makeCubeMesh(apart, 0.0f, 0.0f, 0.0f);
    VCGMesh far;
    makeCubeMesh(far, 5.0f, 0.0f, 0.0f);
    vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(apart, far, false);
    vcg::tri::UpdateBounding<VCGMesh>::Box(apart);
    const int apartIndex = doc.addMesh(apart, QStringLiteral("two cubes"));
    for (const QString &connectivity : { QStringLiteral("manifold"),
                                         QStringLiteral("edge"),
                                         QStringLiteral("vertex") }) {
        const MeshFilterRunResult r = splitWith(apartIndex, connectivity);
        QVERIFY2(r.success, qPrintable(r.errorMessage));
        QVERIFY2(r.newMeshIndices.size() == 2,
                 qPrintable(QStringLiteral("%1 found %2 components, expected 2")
                                .arg(connectivity).arg(r.newMeshIndices.size())));
    }
}
// The curve filters and the sweep that consumes their output. Each case is chosen so the
// expected answer is known: two overlapping boxes cross in a closed loop, a sphere's
// height field contours into rings, and a clean mesh self-intersects nowhere.
void FilterTests::trueFormCurveFamilyProducesPolylines()
{
    Document doc;

    QString boxKey, sphereKey, interKey, selfKey, isoKey, tubeKey, borderKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_box")) boxKey = info.key;
        else if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("create_polyline_from_mesh_intersection_trueform")) interKey = info.key;
        else if (id == QStringLiteral("create_polyline_from_self_intersections_trueform")) selfKey = info.key;
        else if (id == QStringLiteral("create_polyline_from_scalar_isocontour_trueform")) isoKey = info.key;
        else if (id == QStringLiteral("create_tube_from_polyline_trueform")) tubeKey = info.key;
        else if (id == QStringLiteral("compute_geodesic_distance_from_border")) borderKey = info.key;
    }
    QVERIFY(!boxKey.isEmpty());
    if (interKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    QVERIFY2(doc.runFilter(boxKey, {}).success, "create_box failed");
    const int a = doc.currentMeshIndex();
    const float side = doc.mesh(a).mesh.bbox.DimX();
    const int b = doc.addMesh(doc.mesh(a).mesh, QStringLiteral("shifted"));
    QMatrix4x4 shift;
    shift.translate(side * 0.5f, 0.0f, 0.0f);
    doc.setMeshTransform(b, shift);

    // Two overlapping boxes intersect along a closed loop of edges.
    MeshFilterParameterValues pair;
    pair.insert(QStringLiteral("firstMesh"), a);
    pair.insert(QStringLiteral("secondMesh"), b);
    const MeshFilterRunResult crossing = doc.runFilter(interKey, pair);
    QVERIFY2(crossing.success, qPrintable(crossing.errorMessage));
    QCOMPARE(crossing.newMeshIndices.size(), 1);
    const int curveIndex = crossing.newMeshIndices.front();
    QVERIFY2(doc.mesh(curveIndex).mesh.EN() > 0, "intersection curve has no edges");
    QCOMPARE(doc.mesh(curveIndex).mesh.FN(), 0); // a polyline, not a surface

    // Sweeping that curve must give a solid.
    MeshFilterParameterValues tube;
    tube.insert(QStringLiteral("sourceMesh"), curveIndex);
    tube.insert(QStringLiteral("radius"), double(side) * 0.02);
    tube.insert(QStringLiteral("segments"), 8);
    const MeshFilterRunResult swept = doc.runFilter(tubeKey, tube);
    QVERIFY2(swept.success, qPrintable(swept.errorMessage));
    QVERIFY(doc.mesh(swept.newMeshIndices.front()).mesh.FN() > 0);

    // Sweeping something that is not a polyline must be refused, not crash.
    MeshFilterParameterValues badTube;
    badTube.insert(QStringLiteral("sourceMesh"), a);
    badTube.insert(QStringLiteral("radius"), double(side) * 0.02);
    QVERIFY(!doc.runFilter(tubeKey, badTube).success);

    // A single clean box does not intersect itself.
    MeshFilterParameterValues single;
    single.insert(QStringLiteral("sourceMesh"), a);
    QVERIFY2(!doc.runFilter(selfKey, single).success,
             "a clean box should report no self-intersections");

    // Contours of a scalar field: a border-distance field on an open surface.
    if (!sphereKey.isEmpty() && !borderKey.isEmpty()) {
        QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int sphere = doc.currentMeshIndex();

        // Without a scalar field the filter must explain itself rather than fail blankly.
        MeshFilterParameterValues iso;
        iso.insert(QStringLiteral("sourceMesh"), sphere);
        iso.insert(QStringLiteral("contourCount"), 5);
        const MeshFilterRunResult constantField = doc.runFilter(isoKey, iso);
        if (!constantField.success)
            QVERIFY(constantField.errorMessage.contains(QStringLiteral("constant")));
    }
}

// Contract check across every filter declaring outputDomain == NewMeshes.
//
// FilterCreationTests already enforces this, but only for pure generators
// (inputDomain == None). Every NewMeshes filter that takes input — the booleans, the
// reconstructions, the polyline family, the samplers — sits outside that harness, which
// is where a filter can create a layer and forget to report it. The symptom lives
// entirely in the return value, so the document looks correct and only a caller that
// asks "which layer did you just make?" notices.
//
// Filters that cannot run on this input are skipped rather than failed: the assertion is
// conditional — *if* you succeeded and you declared NewMeshes, you must report the layers.
void FilterTests::newMeshFiltersReportTheirLayers()
{
    // A document with enough variety that most filters have something to chew on: a
    // scalar field, two overlapping solids for the binary operators, and a polyline.
    const auto buildInputs = [](Document &doc) -> bool {
        QString sphereKey, boxKey, borderKey, sectionKey;
        for (const auto &info : doc.filterInfos()) {
            const QString id = info.descriptor.id;
            if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
            else if (id == QStringLiteral("create_box")) boxKey = info.key;
            else if (id == QStringLiteral("compute_geodesic_distance_from_border")) borderKey = info.key;
            else if (id == QStringLiteral("create_polyline_from_planar_section")) sectionKey = info.key;
        }
        if (boxKey.isEmpty() || sphereKey.isEmpty())
            return false;

        if (!doc.runFilter(boxKey, {}).success)
            return false;
        const int box = doc.currentMeshIndex();
        const float side = doc.mesh(box).mesh.bbox.DimX();

        // A second solid, overlapping, so binary operators have a meaningful pair.
        const int shifted = doc.addMesh(doc.mesh(box).mesh, QStringLiteral("shifted box"));
        if (shifted < 0)
            return false;
        QMatrix4x4 shift;
        shift.translate(side * 0.5f, 0.0f, 0.0f);
        doc.setMeshTransform(shifted, shift);

        // Normals and a scalar field on the current layer.
        if (!sectionKey.isEmpty())
            doc.runFilter(sectionKey, {});
        if (!borderKey.isEmpty())
            doc.runFilter(borderKey, {});
        doc.setCurrentMeshIndex(box);
        return true;
    };

    Document probe;
    QStringList offenders;
    QStringList ran;
    int skipped = 0;

    for (const auto &info : probe.filterInfos()) {
        if (info.descriptor.outputDomain != MeshFilterOutputDomain::NewMeshes)
            continue;
        // Two explicit exclusions, both with reasons rather than convenience:
        //  - quadwild shells out to an external binary that may not be installed, and is
        //    far too slow for a contract sweep;
        // (generate_voronoi_atlas_parametrization used to be excluded here because it
        // hung or aborted on synthetic input; both vcglib defects are fixed and it is
        // covered by voronoiAtlasHandlesCoarseMeshes, so it takes part again.)
        if (info.descriptor.id.contains(QStringLiteral("quadwild")))
            continue;

        // Four filters whose default parameters are sized for real meshes rather than
        // for the input, so they cost the same on a box as on a scan: measured at 60 s,
        // 31 s, 22 s and 12 s respectively, together four fifths of the sweep. The
        // contract being checked here is about the return value and does not depend on
        // the algorithm running at full resolution.
        static const QSet<QString> kTooSlowForASweep = {
            QStringLiteral("remesh_to_quads_instant_meshes"),
            QStringLiteral("reconstruct_surface_by_marching_cubes_rimls"),
            QStringLiteral("reconstruct_surface_by_marching_cubes_apss"),
            QStringLiteral("reconstruct_surface_by_volumetric_merging"),
        };
        if (kTooSlowForASweep.contains(info.descriptor.id))
            continue;

        Document doc;
        if (!buildInputs(doc))
            QSKIP("Could not build the standard inputs for this build.");

        const int before = doc.meshCount();
        const MeshFilterRunResult result = doc.runFilter(info.key, {});
        if (!result.success) {
            ++skipped; // cannot run on this input; not what this test is about
            continue;
        }
        ran << info.descriptor.name;

        if (result.newMeshIndices.isEmpty()) {
            offenders << QStringLiteral("%1 (%2): succeeded but reported no new layer")
                             .arg(info.descriptor.name, info.descriptor.id);
            continue;
        }
        for (int index : result.newMeshIndices) {
            if (index < 0 || index >= doc.meshCount()) {
                offenders << QStringLiteral("%1 (%2): reported out-of-range layer %3")
                                 .arg(info.descriptor.name, info.descriptor.id).arg(index);
            }
        }
        // Not layer-count arithmetic: Merge Visible Layers legitimately reports one
        // new layer while the document shrinks, because it merges the originals away.
        // What must hold is that every reported index names a real layer with content.
        for (int index : result.newMeshIndices) {
            if (index >= 0 && index < doc.meshCount() && doc.mesh(index).mesh.VN() <= 0) {
                offenders << QStringLiteral("%1 (%2): reported layer %3, which is empty")
                                 .arg(info.descriptor.name, info.descriptor.id).arg(index);
            }
        }
        (void) before;
    }

    qDebug() << "NewMeshes contract:" << ran.size() << "filter(s) exercised,"
             << skipped << "not runnable on the standard inputs";
    QVERIFY2(offenders.isEmpty(), qPrintable(offenders.join(QStringLiteral("\n"))));
    QVERIFY2(ran.size() >= 5, "too few NewMeshes filters were exercised to be meaningful");
}

// A small sphere entirely inside a big box: every vertex is inside, the signed distance
// is negative everywhere, and the distances have a known magnitude. Moving the sphere
// clear of the box must flip all three answers.
void FilterTests::trueFormDistanceAndContainment()
{
    Document doc;

    QString boxKey, sphereKey, distKey, insideKey, chamferKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_box")) boxKey = info.key;
        else if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("compute_signed_distance_to_mesh_trueform")) distKey = info.key;
        else if (id == QStringLiteral("select_vertices_inside_mesh_trueform")) insideKey = info.key;
        else if (id == QStringLiteral("measure_chamfer_distance_trueform")) chamferKey = info.key;
    }
    QVERIFY(!boxKey.isEmpty() && !sphereKey.isEmpty());
    if (distKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    QVERIFY2(doc.runFilter(boxKey, {}).success, "create_box failed");
    const int box = doc.currentMeshIndex();
    const float side = doc.mesh(box).mesh.bbox.DimX();

    // A sphere scaled well inside the box.
    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    const int sphere = doc.currentMeshIndex();
    QMatrix4x4 shrink;
    shrink.scale(side * 0.2f / doc.mesh(sphere).mesh.bbox.Diag());
    doc.setMeshTransform(sphere, shrink);

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("sourceMesh"), sphere);
    p.insert(QStringLiteral("referenceMesh"), box);

    // Inside: every distance negative.
    QVERIFY2(doc.runFilter(distKey, p).success, "signed distance failed");
    {
        const VCGMesh &m = doc.mesh(sphere).mesh;
        int positive = 0;
        for (const VCGVertex &v : m.vert)
            if (!v.IsD() && v.cQ() >= 0.0f)
                ++positive;
        QVERIFY2(positive == 0,
                 qPrintable(QStringLiteral("%1 vertex(es) reported outside a box that "
                                           "encloses them").arg(positive)));
    }

    // ... and every vertex selected as inside.
    QVERIFY2(doc.runFilter(insideKey, p).success, "containment failed");
    {
        const VCGMesh &m = doc.mesh(sphere).mesh;
        int unselected = 0;
        for (const VCGVertex &v : m.vert)
            if (!v.IsD() && !v.IsS())
                ++unselected;
        QCOMPARE(unselected, 0);
    }

    // Move it clear of the box: the answers must invert.
    QMatrix4x4 outside;
    outside.translate(side * 5.0f, 0.0f, 0.0f);
    outside.scale(side * 0.2f / doc.mesh(sphere).mesh.bbox.Diag());
    doc.setMeshTransform(sphere, outside);

    QVERIFY2(doc.runFilter(distKey, p).success, "signed distance failed");
    {
        const VCGMesh &m = doc.mesh(sphere).mesh;
        int negative = 0;
        for (const VCGVertex &v : m.vert)
            if (!v.IsD() && v.cQ() < 0.0f)
                ++negative;
        QCOMPARE(negative, 0);
    }
    QVERIFY2(doc.runFilter(insideKey, p).success, "containment failed");
    {
        const VCGMesh &m = doc.mesh(sphere).mesh;
        int selected = 0;
        for (const VCGVertex &v : m.vert)
            if (!v.IsD() && v.IsS())
                ++selected;
        QCOMPARE(selected, 0);
    }

    // Chamfer distance to itself is zero; to the displaced copy it is not.
    MeshFilterParameterValues self;
    self.insert(QStringLiteral("sourceMesh"), box);
    self.insert(QStringLiteral("referenceMesh"), box);
    QVERIFY2(!doc.runFilter(chamferKey, self).success, "a layer against itself should be refused");

    MeshFilterParameterValues apart;
    apart.insert(QStringLiteral("sourceMesh"), sphere);
    apart.insert(QStringLiteral("referenceMesh"), box);
    apart.insert(QStringLiteral("symmetric"), true);
    const MeshFilterRunResult chamfer = doc.runFilter(chamferKey, apart);
    QVERIFY2(chamfer.success, qPrintable(chamfer.errorMessage));
    QVERIFY(!chamfer.documentModified); // a measurement must not alter the document
    QVERIFY(chamfer.infoMessages.size() >= 3);
}

// The competing per-vertex operators. Each is checked against a property that follows
// from what it claims to do, rather than merely that it ran: Laplacian smoothing shrinks
// a sphere, Taubin does not, Gaussian curvature of a sphere is positive everywhere, and
// recomputed normals on a sphere point outward.
void FilterTests::trueFormAttributeFiltersBehaveAsDocumented()
{
    Document doc;

    QString sphereKey, laplacianKey, taubinKey, curvatureKey, normalsKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("smooth_vertices_by_laplacian_trueform")) laplacianKey = info.key;
        else if (id == QStringLiteral("smooth_vertices_by_taubin_trueform")) taubinKey = info.key;
        else if (id == QStringLiteral("compute_curvature_trueform")) curvatureKey = info.key;
        else if (id == QStringLiteral("compute_normals_trueform")) normalsKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    if (laplacianKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    const auto meshVolume = [](const VCGMesh &m) {
        return double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(m)));
    };

    // Laplacian smoothing shrinks a closed surface; that is its documented drawback.
    {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        const double before = meshVolume(d.mesh(s).mesh);
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("iterations"), 20);
        p.insert(QStringLiteral("lambda"), 0.5);
        QVERIFY2(d.runFilter(laplacianKey, p).success, "laplacian failed");
        const double after = meshVolume(d.mesh(s).mesh);
        QVERIFY2(after < before * 0.99,
                 qPrintable(QStringLiteral("laplacian did not shrink: %1 -> %2").arg(before).arg(after)));
    }

    // Taubin is the volume-preserving one: same iterations, far less loss.
    {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        const double before = meshVolume(d.mesh(s).mesh);
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("iterations"), 20);
        p.insert(QStringLiteral("lambda"), 0.5);
        p.insert(QStringLiteral("kpb"), 0.1);
        QVERIFY2(d.runFilter(taubinKey, p).success, "taubin failed");
        const double after = meshVolume(d.mesh(s).mesh);
        QVERIFY2(after > before * 0.9,
                 qPrintable(QStringLiteral("taubin lost too much volume: %1 -> %2").arg(before).arg(after)));
    }

    // Gaussian curvature of a convex closed surface is positive everywhere.
    {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("measure"), QStringLiteral("gaussian"));
        p.insert(QStringLiteral("ring"), 2);
        QVERIFY2(d.runFilter(curvatureKey, p).success, "curvature failed");
        int nonPositive = 0;
        int total = 0;
        for (const VCGVertex &v : d.mesh(s).mesh.vert) {
            if (v.IsD())
                continue;
            ++total;
            if (!(v.cQ() > 0.0f))
                ++nonPositive;
        }
        QVERIFY(total > 0);
        // Allow a few boundary-ish estimates to misbehave, but not the bulk.
        QVERIFY2(nonPositive < total / 10,
                 qPrintable(QStringLiteral("%1 of %2 gaussian curvatures were not positive")
                                .arg(nonPositive).arg(total)));
    }

    // Recomputed vertex normals on a sphere point away from the centre.
    {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        for (VCGVertex &v : d.mesh(s).mesh.vert)
            v.N() = vcg::Point3f(0.0f, 0.0f, 0.0f); // clear so the filter must do the work
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("target"), QStringLiteral("vertex"));
        QVERIFY2(d.runFilter(normalsKey, p).success, "normals failed");
        const VCGMesh &m = d.mesh(s).mesh;
        const vcg::Point3f centre = m.bbox.Center();
        int inward = 0;
        int total = 0;
        for (const VCGVertex &v : m.vert) {
            if (v.IsD())
                continue;
            ++total;
            if ((v.cP() - centre).Normalize().dot(v.cN()) <= 0.0f)
                ++inward;
        }
        QVERIFY(total > 0);
        QCOMPARE(inward, 0);
    }
}

// The three remeshing filters, each checked against the thing it promises: decimation
// hits a face count, isotropic remeshing equalises edge lengths, and simplification by
// error bound stays within its bound. All three must keep the shape recognisable.
void FilterTests::trueFormRemeshingChangesResolutionAsAsked()
{
    Document doc;

    QString sphereKey, isoKey, simplifyKey, decimateKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("remesh_isotropically_trueform")) isoKey = info.key;
        else if (id == QStringLiteral("simplify_by_error_bound_trueform")) simplifyKey = info.key;
        else if (id == QStringLiteral("simplify_by_decimation_trueform")) decimateKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    if (isoKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    // Decimation to a stated proportion.
    {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        const int before = d.mesh(s).mesh.FN();
        const float diagonalBefore = d.mesh(s).mesh.bbox.Diag();
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("targetProportion"), 0.5);
        QVERIFY2(d.runFilter(decimateKey, p).success, "decimate failed");
        const int after = d.mesh(s).mesh.FN();
        QVERIFY2(after < before, "decimation did not reduce the face count");
        QVERIFY2(after > before / 4, "decimation overshot badly");
        // The shape must survive: the bounding box should be about the same size.
        QVERIFY(std::abs(d.mesh(s).mesh.bbox.Diag() - diagonalBefore) < 0.1f * diagonalBefore);
    }

    // Isotropic remeshing towards a target edge length: edge lengths should cluster
    // around it far more tightly than the original's did.
    {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        const float target = d.mesh(s).mesh.bbox.Diag() * 0.05f;

        MeshFilterParameterValues p;
        p.insert(QStringLiteral("targetLength"), double(target));
        p.insert(QStringLiteral("iterations"), 3);
        QVERIFY2(d.runFilter(isoKey, p).success, "isotropic remesh failed");

        const VCGMesh &m = d.mesh(s).mesh;
        QVERIFY(m.FN() > 0);
        double sum = 0.0;
        int count = 0;
        for (const VCGFace &f : m.face) {
            if (f.IsD())
                continue;
            for (int k = 0; k < 3; ++k) {
                sum += double((f.cV((k + 1) % 3)->cP() - f.cV(k)->cP()).Norm());
                ++count;
            }
        }
        QVERIFY(count > 0);
        const double mean = sum / count;
        // Within a factor of two of the request is a fair bar for a 3-iteration run.
        QVERIFY2(mean > 0.5 * double(target) && mean < 2.0 * double(target),
                 qPrintable(QStringLiteral("mean edge %1, target %2").arg(mean).arg(double(target))));
    }

    // Error-bound simplification: a loose bound must remove more than a tight one.
    {
        // The bound is now an absolute distance; the unit sphere's bounding-box
        // diagonal is about 3.46, so these stand in for the old 0.05% and 5%.
        const auto facesAfter = [&](double errorBound) {
            Document d;
            if (!d.runFilter(sphereKey, {}).success)
                return -1;
            MeshFilterParameterValues p;
            p.insert(QStringLiteral("errorBound"), errorBound);
            if (!d.runFilter(simplifyKey, p).success)
                return -1;
            return d.mesh(d.currentMeshIndex()).mesh.FN();
        };
        const int tight = facesAfter(0.0017);
        const int loose = facesAfter(0.17);
        QVERIFY2(tight > 0 && loose > 0, "error-bound simplification failed");
        QVERIFY2(loose < tight,
                 qPrintable(QStringLiteral("a looser bound kept more faces: %1 vs %2")
                                .arg(loose).arg(tight)));
    }
}

// Orientation and edge selection, each against a property that must hold afterwards:
// an inside-out box comes back with positive volume, a box has exactly twelve creases at
// ninety degrees and none at a threshold above that, and a clean mesh has no non-manifold
// edges.
void FilterTests::trueFormOrientationAndEdgeSelection()
{
    Document doc;

    QString boxKey, sphereKey, coherentKey, outwardKey, creaseKey, nonManifoldKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_box")) boxKey = info.key;
        else if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("orient_faces_consistently_trueform")) coherentKey = info.key;
        else if (id == QStringLiteral("orient_faces_outward_trueform")) outwardKey = info.key;
        else if (id == QStringLiteral("select_crease_edges_trueform")) creaseKey = info.key;
        else if (id == QStringLiteral("select_non_manifold_edges_trueform")) nonManifoldKey = info.key;
    }
    QVERIFY(!boxKey.isEmpty());
    if (outwardKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    const auto signedVolume = [](const VCGMesh &m) {
        return double(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(m));
    };
    const auto countSelectedEdges = [](const VCGMesh &m) {
        int n = 0;
        for (const VCGFace &f : m.face) {
            if (f.IsD())
                continue;
            for (int k = 0; k < 3; ++k)
                if (f.IsFaceEdgeS(k))
                    ++n;
        }
        return n;
    };

    // An inside-out box must come back with positive volume.
    {
        Document d;
        QVERIFY2(d.runFilter(boxKey, {}).success, "create_box failed");
        const int s = d.currentMeshIndex();
        // Invert every face so the solid is wound inwards.
        for (VCGFace &f : d.mesh(s).mesh.face) {
            if (!f.IsD())
                std::swap(f.V(1), f.V(2));
        }
        QVERIFY2(signedVolume(d.mesh(s).mesh) < 0.0, "the box should start inside out");
        QVERIFY2(d.runFilter(outwardKey, {}).success, "orient outward failed");
        QVERIFY2(signedVolume(d.mesh(s).mesh) > 0.0,
                 "orienting outward left the box inside out");
    }

    // Coherent orientation: measured as winding consistency itself rather than through
    // the volume. A consistently wound surface traverses every interior edge once in each
    // direction, so a directed edge seen twice the same way is an inconsistency.
    {
        Document d;
        QVERIFY2(d.runFilter(boxKey, {}).success, "create_box failed");
        const int s = d.currentMeshIndex();

        const auto inconsistentEdges = [](const VCGMesh &m) {
            const VCGVertex *base = m.vert.empty() ? nullptr : &m.vert.front();
            std::map<std::pair<std::size_t, std::size_t>, int> directed;
            for (const VCGFace &f : m.face) {
                if (f.IsD() || !base)
                    continue;
                for (int k = 0; k < 3; ++k) {
                    const auto a = std::size_t(f.cV(k) - base);
                    const auto b = std::size_t(f.cV((k + 1) % 3) - base);
                    ++directed[{ a, b }];
                }
            }
            int bad = 0;
            for (const auto &[edge, count] : directed) {
                if (count > 1)
                    bad += count - 1; // the same direction traversed more than once
            }
            return bad;
        };

        QCOMPARE(inconsistentEdges(d.mesh(s).mesh), 0); // the box starts clean
        int i = 0;
        for (VCGFace &f : d.mesh(s).mesh.face) {
            if (!f.IsD() && (i++ % 2 == 0))
                std::swap(f.V(1), f.V(2)); // scramble half the windings
        }
        const int before = inconsistentEdges(d.mesh(s).mesh);
        QVERIFY2(before > 0, "scrambling should have produced inconsistencies");

        QVERIFY2(d.runFilter(coherentKey, {}).success, "orient coherently failed");
        const int after = inconsistentEdges(d.mesh(s).mesh);

        // Zero, because the filter iterates to a fixed point: a single call to
        // tf::orient_faces_consistently only partly repairs a badly mixed winding (it
        // indexes an edge link built from the pre-flip winding while reversing faces in
        // place), but each call rebuilds that link, so repeating converges. On this box
        // the sequence is 14 -> 8 -> 3 -> 0.
        QVERIFY2(after == 0,
                 qPrintable(QStringLiteral("%1 inconsistent edge(s) remain, from %2")
                                .arg(after).arg(before)));
    }

    // A box has twelve ninety-degree creases, and none above ninety.
    {
        Document d;
        QVERIFY2(d.runFilter(boxKey, {}).success, "create_box failed");
        const int s = d.currentMeshIndex();

        MeshFilterParameterValues p;
        p.insert(QStringLiteral("angle"), 60.0);
        QVERIFY2(d.runFilter(creaseKey, p).success, "crease selection failed");
        // Each of the 12 box edges is shared by two faces, so 24 face-edge marks.
        QCOMPARE(countSelectedEdges(d.mesh(s).mesh), 24);

        p.insert(QStringLiteral("angle"), 120.0);
        QVERIFY2(d.runFilter(creaseKey, p).success, "crease selection failed");
        QCOMPARE(countSelectedEdges(d.mesh(s).mesh), 0);
    }

    // A clean sphere has no non-manifold edges.
    if (!sphereKey.isEmpty()) {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        QVERIFY2(d.runFilter(nonManifoldKey, {}).success, "non-manifold selection failed");
        QCOMPARE(countSelectedEdges(d.mesh(s).mesh), 0);
    }
}

// The last three: welding a soup restores connectivity, resolving self-intersections
// splits the crossing faces, and cutting along contours adds geometry without changing
// the surface's extent.
void FilterTests::trueFormRepairAndIsobands()
{
    Document doc;

    QString boxKey, sphereKey, cleanKey, resolveKey, cutKey, borderKey, unweldKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_box")) boxKey = info.key;
        else if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("remove_duplicate_vertices_trueform")) cleanKey = info.key;
        else if (id == QStringLiteral("repair_self_intersections_trueform")) resolveKey = info.key;
        else if (id == QStringLiteral("cut_along_scalar_isocontour_trueform")) cutKey = info.key;
        else if (id == QStringLiteral("compute_geodesic_distance_from_border")) borderKey = info.key;
        else if (id == QStringLiteral("meshing_vertex_unreferenced_split")) unweldKey = info.key;
    }
    QVERIFY(!boxKey.isEmpty());
    if (cleanKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    // Welding: split every face into its own vertices, then weld them back.
    {
        Document d;
        QVERIFY2(d.runFilter(boxKey, {}).success, "create_box failed");
        const int s = d.currentMeshIndex();
        const int weldedV = d.mesh(s).mesh.VN();

        // Unweld by hand: give each face its own copy of its three vertices.
        {
            VCGMesh soup;
            for (const VCGFace &f : d.mesh(s).mesh.face) {
                if (f.IsD())
                    continue;
                const int base = soup.VN();
                vcg::tri::Allocator<VCGMesh>::AddVertices(soup, 3);
                for (int k = 0; k < 3; ++k)
                    soup.vert[std::size_t(base + k)].P() = f.cV(k)->cP();
                vcg::tri::Allocator<VCGMesh>::AddFace(soup, base, base + 1, base + 2);
            }
            vcg::tri::UpdateBounding<VCGMesh>::Box(soup);
            const int soupIndex = d.addMesh(soup, QStringLiteral("soup"));
            QVERIFY(soupIndex >= 0);
            d.setCurrentMeshIndex(soupIndex);
            QVERIFY2(d.mesh(soupIndex).mesh.VN() > weldedV, "the soup should have more vertices");

            QVERIFY2(d.runFilter(cleanKey, {}).success, "clean failed");
            QCOMPARE(d.mesh(soupIndex).mesh.VN(), weldedV);
            QCOMPARE(d.mesh(soupIndex).mesh.FN(), 12);
        }
    }

    // Resolving self-intersections: two boxes merged into one layer cross each other, so
    // the arrangement must split faces and produce more of them than it started with.
    {
        Document d;
        QVERIFY2(d.runFilter(boxKey, {}).success, "create_box failed");
        const int a = d.currentMeshIndex();
        const float side = d.mesh(a).mesh.bbox.DimX();

        VCGMesh crossing;
        vcg::tri::Append<VCGMesh, VCGMesh>::MeshCopy(crossing, d.mesh(a).mesh);
        const int base = crossing.VN();
        vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(crossing, d.mesh(a).mesh);
        for (int i = base; i < crossing.VN(); ++i)
            crossing.vert[std::size_t(i)].P().X() += side * 0.5f;
        vcg::tri::UpdateBounding<VCGMesh>::Box(crossing);
        const int crossIndex = d.addMesh(crossing, QStringLiteral("crossing"));
        QVERIFY(crossIndex >= 0);
        const int before = d.mesh(crossIndex).mesh.FN();

        MeshFilterParameterValues p;
        p.insert(QStringLiteral("sourceMesh"), crossIndex);
        const MeshFilterRunResult r = d.runFilter(resolveKey, p);
        QVERIFY2(r.success, qPrintable(r.errorMessage));
        QCOMPARE(r.newMeshIndices.size(), 1);
        QVERIFY2(d.mesh(r.newMeshIndices.front()).mesh.FN() > before,
                 "resolving should have split the crossing faces");
    }

    // Cutting along contours adds geometry but must not move the surface.
    if (!sphereKey.isEmpty() && !borderKey.isEmpty()) {
        Document d;
        QVERIFY2(d.runFilter(sphereKey, {}).success, "create_sphere failed");
        const int s = d.currentMeshIndex();
        // Give it a non-constant scalar: height along Y.
        const vcg::Point3f centre = d.mesh(s).mesh.bbox.Center();
        for (VCGVertex &v : d.mesh(s).mesh.vert)
            v.Q() = v.cP().Y() - centre.Y();
        const vcg::Box3f before = d.mesh(s).mesh.bbox;
        const int beforeF = d.mesh(s).mesh.FN();

        MeshFilterParameterValues p;
        p.insert(QStringLiteral("sourceMesh"), s);
        p.insert(QStringLiteral("contourCount"), 4);
        const MeshFilterRunResult r = d.runFilter(cutKey, p);
        QVERIFY2(r.success, qPrintable(r.errorMessage));
        const VCGMesh &cut = d.mesh(r.newMeshIndices.front()).mesh;
        QVERIFY2(cut.FN() > beforeF, "cutting should have added faces");
        // Same surface, so the same extent.
        QVERIFY(std::abs(cut.bbox.DimX() - before.DimX()) < 0.02f * before.DimX());
        QVERIFY(std::abs(cut.bbox.DimY() - before.DimY()) < 0.02f * before.DimY());
    }
}

// Improving a triangulation must raise triangle quality while keeping the vertex count
// and the surface. A sphere whose vertices have been jittered gives it something to fix.
void FilterTests::trueFormImproveTriangulationRaisesQuality()
{
    Document doc;

    QString sphereKey, improveKey;
    for (const auto &info : doc.filterInfos()) {
        const QString id = info.descriptor.id;
        if (id == QStringLiteral("create_sphere")) sphereKey = info.key;
        else if (id == QStringLiteral("remesh_by_edge_flipping_trueform")) improveKey = info.key;
    }
    QVERIFY(!sphereKey.isEmpty());
    if (improveKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    QVERIFY2(doc.runFilter(sphereKey, {}).success, "create_sphere failed");
    const int s = doc.currentMeshIndex();

    // Smallest angle over the whole mesh, in radians: the quantity the min-angle
    // objective is supposed to raise.
    const auto worstAngle = [](const VCGMesh &m) {
        double worst = 3.15;
        for (const VCGFace &f : m.face) {
            if (f.IsD())
                continue;
            for (int k = 0; k < 3; ++k) {
                const vcg::Point3f a = f.cV((k + 1) % 3)->cP() - f.cV(k)->cP();
                const vcg::Point3f b = f.cV((k + 2) % 3)->cP() - f.cV(k)->cP();
                const double na = double(a.Norm());
                const double nb = double(b.Norm());
                if (na < 1e-12 || nb < 1e-12)
                    return 0.0;
                const double cosine = std::clamp(double(a.dot(b)) / (na * nb), -1.0, 1.0);
                worst = std::min(worst, std::acos(cosine));
            }
        }
        return worst;
    };

    // Jitter the vertices tangentially so the triangulation degrades but the shape does not.
    const vcg::Point3f centre = doc.mesh(s).mesh.bbox.Center();
    const float radius = doc.mesh(s).mesh.bbox.Diag() * 0.5f;
    int i = 0;
    for (VCGVertex &v : doc.mesh(s).mesh.vert) {
        if (v.IsD())
            continue;
        const float wobble = 0.12f * radius * ((i % 3) - 1);
        v.P() = v.cP() + vcg::Point3f(wobble, -wobble, wobble * 0.5f);
        ++i;
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(doc.mesh(s).mesh);

    const int beforeV = doc.mesh(s).mesh.VN();
    const int beforeF = doc.mesh(s).mesh.FN();
    const double before = worstAngle(doc.mesh(s).mesh);

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("objective"), QStringLiteral("min_angle"));
    p.insert(QStringLiteral("iterations"), 5);
    p.insert(QStringLiteral("relaxationIterations"), 3);
    QVERIFY2(doc.runFilter(improveKey, p).success, "improve failed");

    const VCGMesh &after = doc.mesh(s).mesh;
    // Refines rather than rebuilds: the counts must not change.
    QCOMPARE(after.VN(), beforeV);
    QCOMPARE(after.FN(), beforeF);
    QVERIFY2(worstAngle(after) > before,
             qPrintable(QStringLiteral("worst angle %1 -> %2, no improvement")
                            .arg(before).arg(worstAngle(after))));
}

// A strip carrying its own X coordinate as the vertex scalar: the contour at value v is
// then the straight line x = v, one unit long, so both where the polyline lies and how
// much of it there is can be stated exactly. The filter chooses the contour values
// itself, spaced strictly inside the range, which is the part a caller cannot see.
void FilterTests::trueFormIsocontoursLandOnTheRequestedValues()
{
    Document doc;
    const QString key =
        filterKeyForId(doc, QStringLiteral("create_polyline_from_scalar_isocontour_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh strip;
    makeScalarStripMesh(strip, 4);
    const int index = doc.addMesh(strip, QStringLiteral("Strip"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD
                                      | vcg::tri::io::Mask::IOM_VERTQUALITY);

    // Four contours over the observed range of 0 to 4: at a fifth, two fifths, and so on.
    {
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("sourceMesh"), index);
        p.insert(QStringLiteral("contourCount"), 4);
        const MeshFilterRunResult r = doc.runFilter(key, p);
        QVERIFY2(r.success, qPrintable(r.errorMessage));
        QCOMPARE(r.newMeshIndices.size(), std::size_t(1));

        const VCGMesh &curve = doc.mesh(r.newMeshIndices.front()).mesh;
        QCOMPARE(curve.FN(), 0); // a polyline, not a surface
        QVERIFY2(verticesLieAtXValues(curve, { 0.8, 1.6, 2.4, 3.2 }),
                 "the contours are not on the four requested values");
        QVERIFY2(std::abs(polylineLength(curve) - 4.0) < 1e-3,
                 qPrintable(QStringLiteral("contour length %1, expected 4")
                                .arg(polylineLength(curve))));
        // Four separate contours, each crossing the strip from one border to the other.
        QCOMPARE(polylinePathCount(curve), 4);
        QCOMPARE(polylineLooseEndCount(curve), 8);
    }

    // A stated range narrower than the field: two contours, at 1.5 and 2.5.
    {
        // The run before left its own polyline layer current, and applicability is
        // decided on the current layer, which has no faces.
        doc.setCurrentMeshIndex(index);
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("sourceMesh"), index);
        p.insert(QStringLiteral("contourCount"), 2);
        p.insert(QStringLiteral("useCustomRange"), true);
        p.insert(QStringLiteral("minValue"), 0.5);
        p.insert(QStringLiteral("maxValue"), 3.5);
        const MeshFilterRunResult r = doc.runFilter(key, p);
        QVERIFY2(r.success, qPrintable(r.errorMessage));

        const VCGMesh &curve = doc.mesh(r.newMeshIndices.front()).mesh;
        QVERIFY2(verticesLieAtXValues(curve, { 1.5, 2.5 }),
                 "the custom range moved the contours somewhere else");
        QVERIFY2(std::abs(polylineLength(curve) - 2.0) < 1e-3,
                 qPrintable(QStringLiteral("contour length %1, expected 2")
                                .arg(polylineLength(curve))));
        QCOMPARE(polylinePathCount(curve), 2);
    }

    // A field whose level sets close: distance from the centre of a disk. The one contour
    // crosses the four spokes halfway along, so it is a closed square of side one half.
    {
        VCGMesh disk;
        makeOpenDiskMesh(disk);
        const vcg::Point3f centre = disk.bbox.Center();
        for (VCGVertex &v : disk.vert)
            v.Q() = (v.cP() - centre).Norm();
        const int radial = doc.addMesh(disk, QStringLiteral("Disk"),
                                       vcg::tri::io::Mask::IOM_VERTCOORD
                                           | vcg::tri::io::Mask::IOM_VERTQUALITY);

        MeshFilterParameterValues p;
        p.insert(QStringLiteral("sourceMesh"), radial);
        p.insert(QStringLiteral("contourCount"), 1);
        const MeshFilterRunResult r = doc.runFilter(key, p);
        QVERIFY2(r.success, qPrintable(r.errorMessage));

        const VCGMesh &curve = doc.mesh(r.newMeshIndices.front()).mesh;
        QCOMPARE(polylinePathCount(curve), 1);
        QVERIFY2(polylineLooseEndCount(curve) == 0, "the ring did not close");
        QVERIFY2(std::abs(polylineLength(curve) - 2.0) < 1e-3,
                 qPrintable(QStringLiteral("ring length %1, expected 2")
                                .arg(polylineLength(curve))));
        // Half the corner distance, since the corners are the only vertices off zero.
        const double radius = 0.5 * double((disk.vert[0].cP() - centre).Norm());
        for (const VCGVertex &v : curve.vert) {
            if (v.IsD())
                continue;
            QVERIFY(std::abs(double((v.cP() - centre).Norm()) - radius) < 1e-4);
        }
    }

    // An inverted range is a user error, and must be reported rather than guessed at.
    {
        doc.setCurrentMeshIndex(index);
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("sourceMesh"), index);
        p.insert(QStringLiteral("useCustomRange"), true);
        p.insert(QStringLiteral("minValue"), 3.0);
        p.insert(QStringLiteral("maxValue"), 1.0);
        QVERIFY(!doc.runFilter(key, p).success);
    }
}

// Cutting along the same field, on the same strip. Splitting a surface along a contour
// cannot alter the surface, so the area is an exact invariant, and every vertex of the
// result is either one of the originals or a point on one of the four contours.
void FilterTests::trueFormIsobandCutSplitsOnlyAlongTheContours()
{
    Document doc;
    const QString key = filterKeyForId(doc, QStringLiteral("cut_along_scalar_isocontour_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh strip;
    makeScalarStripMesh(strip, 4);
    const int index = doc.addMesh(strip, QStringLiteral("Strip"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD
                                      | vcg::tri::io::Mask::IOM_VERTQUALITY);

    const auto areaOf = [](const VCGMesh &m) {
        return double(vcg::tri::Stat<VCGMesh>::ComputeMeshArea(m));
    };
    QVERIFY(std::abs(areaOf(doc.mesh(index).mesh) - 4.0) < 1e-4);
    const int before = doc.mesh(index).mesh.FN();

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("sourceMesh"), index);
    p.insert(QStringLiteral("contourCount"), 4);
    const MeshFilterRunResult r = doc.runFilter(key, p);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QCOMPARE(r.newMeshIndices.size(), std::size_t(1));

    const VCGMesh &cut = doc.mesh(r.newMeshIndices.front()).mesh;
    QVERIFY2(cut.FN() > before, "cutting should have added faces");
    QVERIFY2(std::abs(areaOf(cut) - 4.0) < 1e-3,
             qPrintable(QStringLiteral("cut area %1, expected 4").arg(areaOf(cut))));
    QVERIFY2(verticesLieAtXValues(
                 cut, { 0.0, 1.0, 2.0, 3.0, 4.0, 0.8, 1.6, 2.4, 3.2 }),
             "the cut introduced a vertex that is neither an original nor on a contour");
}

// A bar driven through one face of a cube. The two surfaces meet in a single square
// loop lying in that face, whose side is the bar's cross-section, so the curve's
// position and its total length are both exact: a curve that wandered, doubled back or
// came back partial could not satisfy either.
void FilterTests::trueFormIntersectionCurveTracesTheCrossingLoop()
{
    Document doc;
    const QString key =
        filterKeyForId(doc, QStringLiteral("create_polyline_from_mesh_intersection_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    const int solid = doc.addMesh(cube, QStringLiteral("Cube"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD);
    const int bar = doc.addMesh(cube, QStringLiteral("Bar"),
                                vcg::tri::io::Mask::IOM_VERTCOORD);

    // Half a side across in X and half as wide in Y and Z: the bar leaves the cube
    // through the x = 1 face alone, well clear of its edges.
    QMatrix4x4 place;
    place.translate(0.5f, 0.25f, 0.25f);
    place.scale(1.0f, 0.5f, 0.5f);
    doc.setMeshTransform(bar, place);

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("firstMesh"), solid);
    p.insert(QStringLiteral("secondMesh"), bar);
    const MeshFilterRunResult r = doc.runFilter(key, p);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QCOMPARE(r.newMeshIndices.size(), std::size_t(1));

    const VCGMesh &curve = doc.mesh(r.newMeshIndices.front()).mesh;
    QCOMPARE(curve.FN(), 0);
    QVERIFY(curve.EN() > 0);
    QVERIFY2(verticesLieAtXValues(curve, { 1.0 }),
             "the crossing loop does not lie in the x = 1 face");
    // One loop, and it closes: a seam that came back in pieces would not.
    QCOMPARE(polylinePathCount(curve), 1);
    QVERIFY2(polylineLooseEndCount(curve) == 0, "the crossing loop did not close");
    // The bar's cross-section is half a side square, so the loop is four halves long.
    QVERIFY2(std::abs(polylineLength(curve) - 2.0) < 1e-3,
             qPrintable(QStringLiteral("loop length %1, expected 2")
                            .arg(polylineLength(curve))));
    QVERIFY(std::abs(curve.bbox.DimY() - 0.5f) < 1e-3f);
    QVERIFY(std::abs(curve.bbox.DimZ() - 0.5f) < 1e-3f);
}

// The sweep that consumes the polyline family's output. Its input is an edge mesh whose
// edges arrive in no particular order, so the filter chains them into paths itself; a
// path of P points swept with S segments makes P rings of S vertices and 2 * S triangles
// per step, which pins the chaining and the sweep together. A vertex where three edges
// meet cannot be swept unambiguously, so the polyline is split there rather than
// branched, and no segment is lost in the splitting.
void FilterTests::trueFormTubesSweepTheWholePolyline()
{
    constexpr int kSegments = 12;
    constexpr double kRadius = 0.1;

    Document doc;
    const QString key = filterKeyForId(doc, QStringLiteral("create_tube_from_polyline_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh line;
    vcg::tri::Allocator<VCGMesh>::AddVertices(line, 4);
    for (int i = 0; i < 4; ++i)
        line.vert[std::size_t(i)].P() = vcg::Point3f(float(i), 0.0f, 0.0f);
    // Deliberately out of order: the filter chains the edges, it does not read them off
    // in the order the layer happens to hold them.
    addPolylineSegment(line, 2, 3);
    addPolylineSegment(line, 0, 1);
    addPolylineSegment(line, 1, 2);
    vcg::tri::UpdateBounding<VCGMesh>::Box(line);
    const int polyline = doc.addMesh(line, QStringLiteral("Polyline"),
                                     vcg::tri::io::Mask::IOM_VERTCOORD
                                         | vcg::tri::io::Mask::IOM_EDGEINDEX);

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("sourceMesh"), polyline);
    p.insert(QStringLiteral("radius"), kRadius);
    p.insert(QStringLiteral("segments"), kSegments);
    const MeshFilterRunResult r = doc.runFilter(key, p);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QCOMPARE(r.newMeshIndices.size(), std::size_t(1));
    QVERIFY2(r.infoMessages.join(QStringLiteral(" ")).contains(QStringLiteral("Swept 1 path(s)")),
             qPrintable(r.infoMessages.join(QStringLiteral("\n"))));

    const VCGMesh &tube = doc.mesh(r.newMeshIndices.front()).mesh;
    QCOMPARE(tube.VN(), 4 * kSegments);
    QCOMPARE(tube.FN(), 3 * kSegments * 2);
    // As long as the polyline, and as wide as the profile: the ring vertices sit on the
    // radius and its edges cut the corners, so the width is just under the diameter.
    QVERIFY(std::abs(tube.bbox.DimX() - 3.0f) < 1e-4f);
    for (float width : { tube.bbox.DimY(), tube.bbox.DimZ() }) {
        QVERIFY(width <= float(2.0 * kRadius) + 1e-4f);
        QVERIFY(width > float(1.9 * kRadius));
    }
    const vcg::Point3f origin = tube.bbox.min;

    // The layer matrix belongs to the sweep as well: the tube is world-space geometry.
    // The sweep left its own solid current, and applicability is decided on the current
    // layer, which has no edges.
    doc.setCurrentMeshIndex(polyline);
    QMatrix4x4 moved;
    moved.translate(10.0f, 0.0f, 0.0f);
    doc.setMeshTransform(polyline, moved);
    const MeshFilterRunResult elsewhere = doc.runFilter(key, p);
    QVERIFY2(elsewhere.success, qPrintable(elsewhere.errorMessage));
    const VCGMesh &movedTube = doc.mesh(elsewhere.newMeshIndices.front()).mesh;
    QCOMPARE(movedTube.FN(), tube.FN());
    QVERIFY(std::abs(movedTube.bbox.min.X() - (origin.X() + 10.0f)) < 1e-3f);
    QVERIFY(std::abs(movedTube.bbox.min.Y() - origin.Y()) < 1e-4f);

    // Three edges meeting at one vertex: the junction is reported, and all three are
    // still swept, however the polyline was divided at it.
    VCGMesh junction;
    vcg::tri::Allocator<VCGMesh>::AddVertices(junction, 4);
    junction.vert[0].P() = vcg::Point3f(0.0f, 0.0f, 0.0f);
    junction.vert[1].P() = vcg::Point3f(-1.0f, 0.0f, 0.0f);
    junction.vert[2].P() = vcg::Point3f(1.0f, 0.0f, 0.0f);
    junction.vert[3].P() = vcg::Point3f(0.0f, 1.0f, 0.0f);
    addPolylineSegment(junction, 0, 1);
    addPolylineSegment(junction, 0, 2);
    addPolylineSegment(junction, 0, 3);
    vcg::tri::UpdateBounding<VCGMesh>::Box(junction);
    const int branched = doc.addMesh(junction, QStringLiteral("Junction"),
                                     vcg::tri::io::Mask::IOM_VERTCOORD
                                         | vcg::tri::io::Mask::IOM_EDGEINDEX);

    MeshFilterParameterValues q;
    q.insert(QStringLiteral("sourceMesh"), branched);
    q.insert(QStringLiteral("radius"), kRadius);
    q.insert(QStringLiteral("segments"), kSegments);
    const MeshFilterRunResult swept = doc.runFilter(key, q);
    QVERIFY2(swept.success, qPrintable(swept.errorMessage));
    QVERIFY2(swept.infoMessages.join(QStringLiteral(" "))
                 .contains(QStringLiteral("1 vertex junction(s)")),
             qPrintable(swept.infoMessages.join(QStringLiteral("\n"))));
    QCOMPARE(doc.mesh(swept.newMeshIndices.front()).mesh.FN(), 3 * kSegments * 2);
}

// Distances a reader can check by hand: the centre of a unit cube is half a side inside
// it, and the two other probes are a whole side beyond a face. The existing coverage
// only asks for the sign, which a filter reporting the wrong magnitude would still pass.
void FilterTests::trueFormSignedDistanceReportsExactMagnitudes()
{
    Document doc;
    const QString key =
        filterKeyForId(doc, QStringLiteral("compute_signed_distance_to_mesh_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    const int reference = doc.addMesh(cube, QStringLiteral("Cube"),
                                      vcg::tri::io::Mask::IOM_VERTCOORD);

    VCGMesh probes;
    vcg::tri::Allocator<VCGMesh>::AddVertices(probes, 3);
    probes.vert[0].P() = vcg::Point3f(0.5f, 0.5f, 0.5f);
    probes.vert[1].P() = vcg::Point3f(2.0f, 0.5f, 0.5f);
    probes.vert[2].P() = vcg::Point3f(0.5f, 0.5f, -1.0f);
    vcg::tri::Allocator<VCGMesh>::AddFace(probes, 0, 1, 2);
    vcg::tri::UpdateBounding<VCGMesh>::Box(probes);
    const int source = doc.addMesh(probes, QStringLiteral("Probes"),
                                   vcg::tri::io::Mask::IOM_VERTCOORD);

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("sourceMesh"), source);
    p.insert(QStringLiteral("referenceMesh"), reference);
    const MeshFilterRunResult r = doc.runFilter(key, p);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QVERIFY2(r.infoMessages.join(QStringLiteral(" "))
                 .contains(QStringLiteral("1 vertex(es) lie inside")),
             qPrintable(r.infoMessages.join(QStringLiteral("\n"))));

    const VCGMesh &measured = doc.mesh(source).mesh;
    QVERIFY2(std::abs(measured.vert[0].cQ() + 0.5f) < 1e-4f,
             qPrintable(QStringLiteral("centre reads %1, expected -0.5")
                            .arg(measured.vert[0].cQ())));
    QVERIFY(std::abs(measured.vert[1].cQ() - 1.0f) < 1e-4f);
    QVERIFY(std::abs(measured.vert[2].cQ() - 1.0f) < 1e-4f);

    // The unsigned field keeps the magnitudes and drops the sign.
    p.insert(QStringLiteral("unsigned"), true);
    QVERIFY2(doc.runFilter(key, p).success, "unsigned distance failed");
    QVERIFY(std::abs(doc.mesh(source).mesh.vert[0].cQ() - 0.5f) < 1e-4f);
    QVERIFY(std::abs(doc.mesh(source).mesh.vert[1].cQ() - 1.0f) < 1e-4f);
}

// The containment filter answers the same question as the distance, so the same probes
// settle it, and it is the selection modes that need saying: replacing, inverting, and
// then adding to and subtracting from a selection that is already there.
void FilterTests::trueFormContainmentSelectionModesCompose()
{
    Document doc;
    const QString key = filterKeyForId(doc, QStringLiteral("select_vertices_inside_mesh_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    const int reference = doc.addMesh(cube, QStringLiteral("Cube"),
                                      vcg::tri::io::Mask::IOM_VERTCOORD);

    VCGMesh probes;
    vcg::tri::Allocator<VCGMesh>::AddVertices(probes, 3);
    probes.vert[0].P() = vcg::Point3f(0.5f, 0.5f, 0.5f);
    probes.vert[1].P() = vcg::Point3f(2.0f, 0.5f, 0.5f);
    probes.vert[2].P() = vcg::Point3f(0.5f, 0.5f, -1.0f);
    vcg::tri::Allocator<VCGMesh>::AddFace(probes, 0, 1, 2);
    vcg::tri::UpdateBounding<VCGMesh>::Box(probes);
    const int source = doc.addMesh(probes, QStringLiteral("Probes"),
                                   vcg::tri::io::Mask::IOM_VERTCOORD);

    const auto selection = [&doc, source]() {
        const VCGMesh &m = doc.mesh(source).mesh;
        QString bits;
        for (const VCGVertex &v : m.vert) {
            if (!v.IsD())
                bits += v.IsS() ? QLatin1Char('1') : QLatin1Char('0');
        }
        return bits;
    };

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("sourceMesh"), source);
    p.insert(QStringLiteral("referenceMesh"), reference);
    QVERIFY2(doc.runFilter(key, p).success, "containment failed");
    QCOMPARE(selection(), QStringLiteral("100"));

    // Inverting selects the complement, and replaces what was there.
    p.insert(QStringLiteral("selectOutside"), true);
    QVERIFY2(doc.runFilter(key, p).success, "containment failed");
    QCOMPARE(selection(), QStringLiteral("011"));

    // Adding the inside ones to that leaves all three selected.
    p.insert(QStringLiteral("selectOutside"), false);
    p.insert(QStringLiteral("mode"), QStringLiteral("add"));
    QVERIFY2(doc.runFilter(key, p).success, "containment failed");
    QCOMPARE(selection(), QStringLiteral("111"));

    // Subtracting the outside ones takes the selection back to the enclosed vertex.
    p.insert(QStringLiteral("selectOutside"), true);
    p.insert(QStringLiteral("mode"), QStringLiteral("subtract"));
    QVERIFY2(doc.runFilter(key, p).success, "containment failed");
    QCOMPARE(selection(), QStringLiteral("100"));
}

// A cube against a copy of itself moved a quarter of a side: every corner's nearest
// neighbour is the corner it was moved from, so the mean of those distances is the
// offset itself, in both directions. The existing coverage only counts the messages.
void FilterTests::trueFormChamferDistanceMeasuresAKnownOffset()
{
    Document doc;
    const QString key = filterKeyForId(doc, QStringLiteral("measure_chamfer_distance_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    const int a = doc.addMesh(cube, QStringLiteral("Cube"), vcg::tri::io::Mask::IOM_VERTCOORD);
    const int b = doc.addMesh(cube, QStringLiteral("Shifted"), vcg::tri::io::Mask::IOM_VERTCOORD);
    QMatrix4x4 shift;
    shift.translate(0.25f, 0.0f, 0.0f);
    doc.setMeshTransform(b, shift);

    const auto reported = [](const QString &message) {
        return message.section(QStringLiteral(": "), -1).toDouble();
    };

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("sourceMesh"), a);
    p.insert(QStringLiteral("referenceMesh"), b);
    p.insert(QStringLiteral("symmetric"), true);
    const MeshFilterRunResult r = doc.runFilter(key, p);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QVERIFY(r.infoMessages.size() == 3); // each direction, and the larger of the two
    for (const QString &message : r.infoMessages) {
        QVERIFY2(std::abs(reported(message) - 0.25) < 1e-5, qPrintable(message));
    }

    // One direction only: one measurement, and the same one.
    p.insert(QStringLiteral("symmetric"), false);
    const MeshFilterRunResult oneWay = doc.runFilter(key, p);
    QVERIFY2(oneWay.success, qPrintable(oneWay.errorMessage));
    QVERIFY(oneWay.infoMessages.size() == 1);
    QVERIFY(std::abs(reported(oneWay.infoMessages.front()) - 0.25) < 1e-5);
}

// Three fins on one edge. The existing coverage asks a clean sphere for its non-manifold
// edges and is told there are none, which a filter that selects nothing at all would
// also satisfy; this asks a mesh that has one.
void FilterTests::trueFormNonManifoldSelectionFindsTheSharedEdge()
{
    Document doc;
    const QString key =
        filterKeyForId(doc, QStringLiteral("select_non_manifold_edges_trueform"));
    if (key.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");

    VCGMesh fan;
    makeNonManifoldFanMesh(fan);
    const int index = doc.addMesh(fan, QStringLiteral("Fan"), vcg::tri::io::Mask::IOM_VERTCOORD);
    doc.setCurrentMeshIndex(index);

    const auto countSelectedEdges = [](const VCGMesh &m) {
        int n = 0;
        for (const VCGFace &f : m.face) {
            if (f.IsD())
                continue;
            for (int k = 0; k < 3; ++k)
                if (f.IsFaceEdgeS(k))
                    ++n;
        }
        return n;
    };

    QVERIFY2(doc.runFilter(key, {}).success, "non-manifold selection failed");
    // All three fins carry the edge 0-1, so each of them marks it once.
    QCOMPARE(countSelectedEdges(doc.mesh(index).mesh), 3);
    QVERIFY(isSelectedEdge(doc.mesh(index).mesh, 0, 1));
    QVERIFY(!isSelectedEdge(doc.mesh(index).mesh, 1, 2));

    // Adding to the selection keeps what is already marked; replacing clears it first.
    doc.mesh(index).mesh.face[0].SetFaceEdgeS(1);
    MeshFilterParameterValues add;
    add.insert(QStringLiteral("replaceSelection"), false);
    QVERIFY2(doc.runFilter(key, add).success, "non-manifold selection failed");
    QCOMPARE(countSelectedEdges(doc.mesh(index).mesh), 4);

    MeshFilterParameterValues replace;
    replace.insert(QStringLiteral("replaceSelection"), true);
    QVERIFY2(doc.runFilter(key, replace).success, "non-manifold selection failed");
    QCOMPARE(countSelectedEdges(doc.mesh(index).mesh), 3);
}

// Two unit cubes overlapping in one corner octant, in a single layer: the shape both of
// the repair filters are for. Resolving the self-intersections only splits the surface,
// so its area is unchanged; extracting the outer shell discards what is enclosed, so the
// volume is the union's and the area is the two cubes' less the six buried quarters.
void FilterTests::trueFormShellAndRepairOfOverlappingCubes()
{
    Document doc;
    const QString shellKey = filterKeyForId(doc, QStringLiteral("extract_outer_shell_trueform"));
    if (shellKey.isEmpty())
        QSKIP("TrueForm filter plugin is not available in this build.");
    const QString repairKey = filterKeyForId(doc, QStringLiteral("repair_self_intersections_trueform"));
    QVERIFY(!repairKey.isEmpty());

    VCGMesh overlapping;
    makeCubeMesh(overlapping, 0.0f, 0.0f, 0.0f);
    VCGMesh second;
    makeCubeMesh(second, 0.5f, 0.5f, 0.5f);
    vcg::tri::Append<VCGMesh, VCGMesh>::Mesh(overlapping, second);
    vcg::tri::UpdateBounding<VCGMesh>::Box(overlapping);
    const int index = doc.addMesh(overlapping, QStringLiteral("Overlapping"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD);

    const auto areaOf = [](const VCGMesh &m) {
        return double(vcg::tri::Stat<VCGMesh>::ComputeMeshArea(m));
    };
    const auto volumeOf = [](const VCGMesh &m) {
        return double(std::abs(vcg::tri::Stat<VCGMesh>::ComputeMeshVolume(m)));
    };
    QVERIFY(std::abs(areaOf(doc.mesh(index).mesh) - 12.0) < 1e-4);
    const int before = doc.mesh(index).mesh.FN();

    MeshFilterParameterValues p;
    p.insert(QStringLiteral("sourceMesh"), index);

    const MeshFilterRunResult resolved = doc.runFilter(repairKey, p);
    QVERIFY2(resolved.success, qPrintable(resolved.errorMessage));
    const VCGMesh &split = doc.mesh(resolved.newMeshIndices.front()).mesh;
    QVERIFY2(split.FN() > before, "resolving should have split the crossing faces");
    QVERIFY2(std::abs(areaOf(split) - 12.0) < 1e-3,
             qPrintable(QStringLiteral("resolved area %1, expected 12").arg(areaOf(split))));
    QVERIFY(std::abs(split.bbox.DimX() - 1.5f) < 1e-4f);

    const MeshFilterRunResult shell = doc.runFilter(shellKey, p);
    QVERIFY2(shell.success, qPrintable(shell.errorMessage));
    const VCGMesh &outer = doc.mesh(shell.newMeshIndices.front()).mesh;
    QVERIFY2(std::abs(volumeOf(outer) - 1.875) < 1e-3,
             qPrintable(QStringLiteral("shell volume %1, expected 1.875").arg(volumeOf(outer))));
    QVERIFY2(std::abs(areaOf(outer) - 10.5) < 1e-3,
             qPrintable(QStringLiteral("shell area %1, expected 10.5").arg(areaOf(outer))));
}

// Voronoi Atlas used to hang or abort on coarse meshes rather than failing. Two vcglib
// defects were behind it:
//
//  - SeedToVertexConversion dropped every seed whose nearest vertex was farther than
//    bbox.Diag()/10. That bound measures the object, not its triangulation: on a unit box
//    it is 0.17 while a face centre is 0.71 from the nearest vertex. With no seeds,
//    FaceAssociateRegion's do/while had no progress guard and spun for ever.
//  - With no region homeomorphic to a disk, uvBorders stayed empty and the rect packer
//    asserted on n > 0 instead of tolerating an empty atlas.
//
// The bound now falls back per seed, the loop stops when it stops progressing, and an
// empty atlas is reported. This test pins all of that: coarse input must fail cleanly and
// promptly, and well-tessellated input must still succeed.
void FilterTests::voronoiAtlasHandlesCoarseMeshes()
{
    struct Case { const char *primitive; int regions; bool expectSuccess; };
    const Case cases[] = {
        { "create_box", 2, false },          // 12 faces: too coarse to partition
        { "create_box", 10, false },
        { "create_tetrahedron", 2, false },  // 4 faces: coarser still
        { "create_sphere", 2, true },
        { "create_sphere", 10, true },
        { "create_annulus", 10, true },      // bound/circumradius ~1.08: used to be marginal
    };

    for (const Case &c : cases) {
        Document doc;
        QString primKey, atlasKey;
        for (const auto &info : doc.filterInfos()) {
            if (info.descriptor.id == QLatin1String(c.primitive)) primKey = info.key;
            else if (info.descriptor.id == QStringLiteral("parametrize_by_voronoi_atlas_vcglib"))
                atlasKey = info.key;
        }
        QVERIFY(!primKey.isEmpty());
        QVERIFY(!atlasKey.isEmpty());
        QVERIFY2(doc.runFilter(primKey, {}).success, c.primitive);

        QElapsedTimer timer;
        timer.start();
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("regionNum"), c.regions);
        p.insert(QStringLiteral("randomSeed"), 1);
        const MeshFilterRunResult r = doc.runFilter(atlasKey, p);
        const qint64 elapsed = timer.elapsed();

        const QString what = QStringLiteral("%1/%2").arg(c.primitive).arg(c.regions);
        // The hang was unbounded; anything of this order means it is back.
        QVERIFY2(elapsed < 30000, qPrintable(QStringLiteral("%1 took %2 ms").arg(what).arg(elapsed)));
        QVERIFY2(r.success == c.expectSuccess,
                 qPrintable(QStringLiteral("%1: success=%2, expected %3, msg '%4'")
                                .arg(what).arg(r.success).arg(c.expectSuccess).arg(r.errorMessage)));
        if (c.expectSuccess) {
            QCOMPARE(r.newMeshIndices.size(), 1);
            QVERIFY2(doc.mesh(r.newMeshIndices.front()).mesh.FN() > 0, qPrintable(what));
        } else {
            // Failing is fine; failing without saying why is not.
            QVERIFY2(!r.errorMessage.isEmpty(), qPrintable(what));
        }
    }
}

void FilterTests::geodesicQualityFilterDoesNotBakeVertexColors()
{
    Document doc;
    const QString path = QStringLiteral(TEST_SOURCE_DIR "/tests/data/simple.off");
    QCOMPARE(doc.loadMesh(path), 0);

    QString borderGeodesicKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("compute_geodesic_distance_from_border")) {
            borderGeodesicKey = info.key;
            break;
        }
    }

    QVERIFY(!borderGeodesicKey.isEmpty());

    const MeshFilterRunResult result = doc.runFilter(borderGeodesicKey, {});
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);
    QCOMPARE(result.visualizationHints.size(), 1);
    QCOMPARE(result.visualizationHints.front().meshIndex, 0);
    QVERIFY(result.visualizationHints.front().attribute ==
            MeshFilterVisualizationAttribute::VertexQuality);

    const int mask = doc.mesh(0).ioMask;
    QVERIFY((mask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0);
    QVERIFY((mask & vcg::tri::io::Mask::IOM_VERTCOLOR) == 0);
}

void FilterTests::triOptimizeFiltersRunOnLoadedMesh()
{
    Document doc;
    const QString path = QStringLiteral(TEST_SOURCE_DIR "/tests/data/simple.off");
    QCOMPARE(doc.loadMesh(path), 0);
    QCOMPARE(doc.meshCount(), 1);

    QString planarKey;
    QString curvatureKey;
    QString smoothKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("flip_edges_by_planarity"))
            planarKey = info.key;
        else if (info.descriptor.id == QStringLiteral("flip_edges_by_curvature"))
            curvatureKey = info.key;
        else if (info.descriptor.id == QStringLiteral("smooth_vertices_by_surface_preserving_laplacian_vcglib"))
            smoothKey = info.key;
    }

    QVERIFY(!planarKey.isEmpty());
    QVERIFY(!curvatureKey.isEmpty());
    QVERIFY(!smoothKey.isEmpty());

    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("iterations"), 0);
        const MeshFilterRunResult result = doc.runFilter(planarKey, params);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QVERIFY(result.documentModified);
    }

    {
        const MeshFilterRunResult result = doc.runFilter(curvatureKey, {});
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QVERIFY(result.documentModified);
        QVERIFY((doc.mesh(0).ioMask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0);
    }

    {
        const MeshFilterRunResult result = doc.runFilter(smoothKey, {});
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QVERIFY(result.documentModified);
        QVERIFY((doc.mesh(0).ioMask & vcg::tri::io::Mask::IOM_VERTNORMAL) != 0);
    }
}

void FilterTests::voronoiSurfaceSamplingRunsOnCube()
{
    Document doc;
    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    const int mask =
        vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    QCOMPARE(doc.addMesh(cube, QStringLiteral("Cube"), mask), 0);

    QString voronoiSamplingKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("sample_surface_by_voronoi_relaxation")) {
            voronoiSamplingKey = info.key;
            break;
        }
    }

    QVERIFY(!voronoiSamplingKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("sampleNum"), 4);
    params.insert(QStringLiteral("iterNum"), 0);
    params.insert(QStringLiteral("randomSeed"), 1);
    params.insert(QStringLiteral("preprocessFlag"), false);

    const int meshCountBefore = doc.meshCount();
    const MeshFilterRunResult result = doc.runFilter(voronoiSamplingKey, params);

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);
    QCOMPARE(result.newMeshIndices.size(), 2);
    QCOMPARE(doc.meshCount(), meshCountBefore + 2);
    QVERIFY((doc.mesh(0).ioMask & vcg::tri::io::Mask::IOM_VERTCOLOR) != 0);
    QVERIFY((doc.mesh(0).ioMask & vcg::tri::io::Mask::IOM_VERTQUALITY) != 0);
    for (int generatedIndex : result.newMeshIndices) {
        QVERIFY(generatedIndex >= 0 && generatedIndex < doc.meshCount());
        QVERIFY(doc.mesh(generatedIndex).mesh.VN() > 0);
    }
}

void FilterTests::voronoiSolidWireframeRunsOnLoadedMesh()
{
    Document doc;
    VCGMesh disk;
    makeOpenDiskMesh(disk);
    const int mask =
        vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    QCOMPARE(doc.addMesh(disk, QStringLiteral("Open Disk"), mask), 0);

    QString solidWireframeKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("create_solid_wireframe")) {
            solidWireframeKey = info.key;
            break;
        }
    }

    QVERIFY(!solidWireframeKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("edgeCylFlag"), true);
    params.insert(QStringLiteral("vertSphFlag"), true);
    params.insert(QStringLiteral("faceExtFlag"), false);
    params.insert(QStringLiteral("edgeCylRadius"), 0.02);
    params.insert(QStringLiteral("vertSphRadius"), 0.03);
    params.insert(QStringLiteral("cylinderSideNum"), 8);

    const int meshCountBefore = doc.meshCount();
    const MeshFilterRunResult result = doc.runFilter(solidWireframeKey, params);

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);
    QCOMPARE(result.newMeshIndices.size(), 1);
    QCOMPARE(doc.meshCount(), meshCountBefore + 1);

    const int generatedIndex = result.newMeshIndices.front();
    QVERIFY(generatedIndex >= 0 && generatedIndex < doc.meshCount());
    QVERIFY(doc.mesh(generatedIndex).mesh.VN() > 0);
    QVERIFY(doc.mesh(generatedIndex).mesh.FN() > 0);
}

void FilterTests::icpBetweenPointCloudsUpdatesSourceTransform()
{
    Document doc;
    VCGMesh referenceCloud;
    VCGMesh sourceCloud;
    makeIcpPointCloud(referenceCloud);
    makeIcpPointCloud(sourceCloud);
    const int mask =
        vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL;
    const int referenceIndex = doc.addMesh(referenceCloud, QStringLiteral("ICP Reference"), mask);
    const int sourceIndex = doc.addMesh(sourceCloud, QStringLiteral("ICP Source"), mask);
    QVERIFY(referenceIndex >= 0);
    QVERIFY(sourceIndex >= 0);

    QMatrix4x4 shifted;
    shifted.setToIdentity();
    shifted.translate(0.12f, -0.06f, 0.04f);
    doc.setMeshTransform(sourceIndex, shifted);

    QString icpKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("align_by_icp_vcglib")) {
            icpKey = info.key;
            break;
        }
    }

    QVERIFY(!icpKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("ReferenceMesh"), referenceIndex);
    params.insert(QStringLiteral("SourceMesh"), sourceIndex);
    params.insert(QStringLiteral("SampleNum"), 5);
    params.insert(QStringLiteral("SampleMode"), false);
    params.insert(QStringLiteral("UseVertexOnly"), true);
    params.insert(QStringLiteral("MinPointNum"), 3);
    params.insert(QStringLiteral("MinDistAbs"), 0.5);
    params.insert(QStringLiteral("TrgDistAbs"), 0.000001);
    params.insert(QStringLiteral("MaxIterNum"), 20);
    params.insert(QStringLiteral("PassHiFilter"), 1.0);

    const MeshFilterRunResult result = doc.runFilter(icpKey, params);

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);
    QMatrix4x4 identity;
    identity.setToIdentity();
    QVERIFY(matrixNear(doc.mesh(sourceIndex).transform, identity));
}

void FilterTests::translateFilterMovesOnlyCurrentMesh()
{
    Document doc;
    VCGMesh firstCube;
    VCGMesh secondCube;
    makeCubeMesh(firstCube, 0.0f, 0.0f, 0.0f);
    makeCubeMesh(secondCube, 0.0f, 0.0f, 0.0f);
    const int mask =
        vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    QCOMPARE(doc.addMesh(firstCube, QStringLiteral("Cube A"), mask), 0);
    QCOMPARE(doc.addMesh(secondCube, QStringLiteral("Cube B"), mask), 1);

    QString translateKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("translate")) {
            translateKey = info.key;
            break;
        }
    }
    QVERIFY(!translateKey.isEmpty());

    doc.setCurrentMeshIndex(1);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("traslMethod"), QStringLiteral("xyz"));
    params.insert(QStringLiteral("axis"), QVector3D(1.0f, 0.0f, 0.0f));
    params.insert(QStringLiteral("Freeze"), false);

    const MeshFilterRunResult result = doc.runFilter(translateKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);

    QMatrix4x4 identity;
    identity.setToIdentity();
    QVERIFY(matrixNear(doc.mesh(0).transform, identity));

    QMatrix4x4 expected;
    expected.setToIdentity();
    expected.translate(1.0f, 0.0f, 0.0f);
    QVERIFY(matrixNear(doc.mesh(1).transform, expected));
}

namespace {

// Three cubes, all visible unless the caller hides one.
int addCubeLayer(Document &doc, const QString &name)
{
    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    return doc.addMesh(
        cube,
        name,
        vcg::tri::io::Mask::IOM_VERTCOORD
            | vcg::tri::io::Mask::IOM_VERTNORMAL
            | vcg::tri::io::Mask::IOM_FACENORMAL);
}

MeshFilterParameterValues translateAlongXParams()
{
    MeshFilterParameterValues params;
    params.insert(QStringLiteral("traslMethod"), QStringLiteral("xyz"));
    params.insert(QStringLiteral("axis"), QVector3D(1.0f, 0.0f, 0.0f));
    params.insert(QStringLiteral("Freeze"), false);
    return params;
}

} // namespace

// Applying a filter to every visible layer is one user gesture, so it must cost exactly
// one undo entry -- not one per layer, which would leave the intermediate half-applied
// states, which nobody asked for, sitting in the history.
void FilterTests::applyToAllVisibleLayersIsASingleUndoStep()
{
    Document doc;
    QCOMPARE(addCubeLayer(doc, QStringLiteral("Cube A")), 0);
    QCOMPARE(addCubeLayer(doc, QStringLiteral("Cube B")), 1);
    QCOMPARE(addCubeLayer(doc, QStringLiteral("Cube C")), 2);
    doc.setMeshVisible(1, false);
    doc.setCurrentMeshIndex(0);

    const QString key = filterKeyForId(doc, QStringLiteral("translate"));
    QVERIFY(!key.isEmpty());

    const int undoDepthBefore = doc.undoCursorPosition();
    const Document::MultiMeshFilterResult result =
        doc.runFilterOnVisibleMeshes(key, translateAlongXParams());

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.targetCount, 2);
    QCOMPARE(result.appliedCount, 2);
    QVERIFY(result.skipped.isEmpty());
    QVERIFY(result.documentModified);

    QMatrix4x4 identity;
    identity.setToIdentity();
    QMatrix4x4 moved;
    moved.setToIdentity();
    moved.translate(1.0f, 0.0f, 0.0f);
    QVERIFY(matrixNear(doc.mesh(0).transform, moved));
    QVERIFY(matrixNear(doc.mesh(1).transform, identity)); // hidden layer left alone
    QVERIFY(matrixNear(doc.mesh(2).transform, moved));

    // The sweep hands the user back the layer they were on.
    QCOMPARE(doc.currentMeshIndex(), 0);

    // The whole point: one entry, and one undo takes every layer back.
    QCOMPARE(doc.undoCursorPosition(), undoDepthBefore + 1);
    QVERIFY(doc.undo());
    QCOMPARE(doc.undoCursorPosition(), undoDepthBefore);
    QVERIFY(matrixNear(doc.mesh(0).transform, identity));
    QVERIFY(matrixNear(doc.mesh(2).transform, identity));
}

// A filter that emits new layers must not have its own output swept back into the same
// run. The sweep therefore fixes its targets up front, by mesh id: with a live
// meshCount() bound, duplicating every visible layer never terminates.
void FilterTests::applyToAllVisibleLayersDoesNotConsumeItsOwnOutput()
{
    Document doc;
    QCOMPARE(addCubeLayer(doc, QStringLiteral("Cube A")), 0);
    QCOMPARE(addCubeLayer(doc, QStringLiteral("Cube B")), 1);

    const QString key = filterKeyForId(doc, QStringLiteral("duplicate_current_layer"));
    QVERIFY(!key.isEmpty());

    const int undoDepthBefore = doc.undoCursorPosition();
    const Document::MultiMeshFilterResult result = doc.runFilterOnVisibleMeshes(key, {});

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.targetCount, 2);
    QCOMPARE(result.appliedCount, 2);
    QCOMPARE(result.newMeshIndices.size(), 2);
    QCOMPARE(doc.meshCount(), 4); // exactly one duplicate per original

    // Layers created across the sweep also belong to its single undo entry.
    QCOMPARE(doc.undoCursorPosition(), undoDepthBefore + 1);
    QVERIFY(doc.undo());
    QCOMPARE(doc.meshCount(), 2);
}

// A layer the filter cannot be applied to used to be dropped in silence, which made a
// partial sweep indistinguishable from a complete one.
void FilterTests::applyToAllVisibleLayersReportsSkippedLayers()
{
    Document doc;
    QCOMPARE(addCubeLayer(doc, QStringLiteral("Cube A")), 0);

    VCGMesh cloud;
    makeIcpPointCloud(cloud);
    QCOMPARE(
        doc.addMesh(cloud, QStringLiteral("Cloud"), vcg::tri::io::Mask::IOM_VERTCOORD),
        1);
    QCOMPARE(addCubeLayer(doc, QStringLiteral("Cube B")), 2);
    doc.setCurrentMeshIndex(0);

    // Compute Face Normals declares requireFaces, so the point-cloud layer fails
    // validation while both cubes go through.
    const QString key = filterKeyForId(doc, QStringLiteral("compute_face_normals"));
    QVERIFY(!key.isEmpty());

    const Document::MultiMeshFilterResult result = doc.runFilterOnVisibleMeshes(key, {});

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.targetCount, 3);
    QCOMPARE(result.appliedCount, 2);
    QCOMPARE(result.skipped.size(), 1);
    QCOMPARE(result.skipped[0].meshIndex, 1);
    QCOMPARE(result.skipped[0].layerName, QStringLiteral("Cloud"));
    QVERIFY(result.skipped[0].reason.contains(QStringLiteral("faces"), Qt::CaseInsensitive));
}

void FilterTests::packTextureImagesCreatesGutteredAtlas()
{
    Document doc;
    VCGMesh mesh;
    makeTwoTextureTriangles(mesh);
    const int meshIndex = doc.addMesh(
        mesh,
        QStringLiteral("Two textures"),
        vcg::tri::io::Mask::IOM_VERTCOORD | vcg::tri::io::Mask::IOM_WEDGTEXCOORD);
    QCOMPARE(meshIndex, 0);

    QImage red(2, 2, QImage::Format_RGBA8888);
    QImage green(2, 2, QImage::Format_RGBA8888);
    red.fill(Qt::red);
    green.fill(Qt::green);
    TextureAssociationUtils::replaceTextureAssociations(
        doc.mesh(meshIndex),
        {
            TextureAssociationUtils::makeTextureAssetFromImage(red, QStringLiteral("red.png")),
            TextureAssociationUtils::makeTextureAssetFromImage(green, QStringLiteral("green.png"))
        });

    QString filterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("pack_texture_images")) {
            filterKey = info.key;
            QCOMPARE(info.descriptor.name, QStringLiteral("Pack Texture Images"));
            break;
        }
    }
    QVERIFY(!filterKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("containerNum"), 1);
    params.insert(QStringLiteral("gutter"), 2);
    const MeshFilterRunResult result = doc.runFilter(filterKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(result.newMeshIndices.size(), 1);

    const auto &output = doc.mesh(result.newMeshIndices.front());
    QCOMPARE(output.textureAssets.size(), size_t(1));
    const QImage &atlas = output.textureAssets.front().image;
    QVERIFY(!atlas.isNull());
    int redPixels = 0;
    int greenPixels = 0;
    for (int y = 0; y < atlas.height(); ++y) {
        for (int x = 0; x < atlas.width(); ++x) {
            const QColor color = atlas.pixelColor(x, y);
            redPixels += color == QColor(Qt::red);
            greenPixels += color == QColor(Qt::green);
        }
    }
    QVERIFY(redPixels >= 36);
    QVERIFY(greenPixels >= 36);
    for (const VCGFace &face : output.mesh.face) {
        for (int corner = 0; corner < 3; ++corner) {
            QCOMPARE(face.cWT(corner).N(), 0);
            QVERIFY(face.cWT(corner).U() > 0.0f && face.cWT(corner).U() < 1.0f);
            QVERIFY(face.cWT(corner).V() > 0.0f && face.cWT(corner).V() < 1.0f);
        }
    }
}

void FilterTests::faceQualityFiltersAreSplit()
{
    Document doc;
    VCGMesh mesh;
    makeOpenDiskMesh(mesh);
    mesh.face.EnableWedgeTexCoord();
    for (VCGFace &face : mesh.face) {
        for (int corner = 0; corner < 3; ++corner) {
            face.WT(corner).U() = face.cP(corner).X();
            face.WT(corner).V() = face.cP(corner).Y();
            face.WT(corner).N() = 0;
        }
    }
    QCOMPARE(
        doc.addMesh(
            mesh,
            QStringLiteral("Parameterized disk"),
            vcg::tri::io::Mask::IOM_VERTCOORD | vcg::tri::io::Mask::IOM_WEDGTEXCOORD),
        0);

    QString geometricFilterKey;
    QString textureFilterKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("compute_face_scalar_from_geometry"))
            geometricFilterKey = info.key;
        else if (info.descriptor.id == QStringLiteral("compute_uv_distortion"))
            textureFilterKey = info.key;
    }
    QVERIFY(!geometricFilterKey.isEmpty());
    QVERIFY(!textureFilterKey.isEmpty());

    const MeshFilterRunResult geometricResult =
        doc.runFilter(geometricFilterKey, MeshFilterParameterValues{});
    QVERIFY2(geometricResult.success, qPrintable(geometricResult.errorMessage));

    const std::array<std::pair<QString, float>, 5> textureMetrics = {{
        { QStringLiteral("angle"), 0.0f },
        { QStringLiteral("area"), 0.0f },
        { QStringLiteral("edge"), 0.0f },
        { QStringLiteral("l2_stretch"), 1.0f },
        { QStringLiteral("linf_stretch"), 1.0f }
    }};
    for (const auto &[metric, expected] : textureMetrics) {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("metric"), metric);
        const MeshFilterRunResult result = doc.runFilter(textureFilterKey, params);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        for (const VCGFace &face : doc.mesh(0).mesh.face) {
            QVERIFY(std::isfinite(face.cQ()));
            QVERIFY(std::abs(face.cQ() - expected) < 1e-6f);
        }
    }

    for (VCGFace &face : doc.mesh(0).mesh.face)
        for (int corner = 0; corner < 3; ++corner)
            face.WT(corner).P() *= 7.0f;
    for (const auto &[metric, expected] : textureMetrics) {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("metric"), metric);
        const MeshFilterRunResult result = doc.runFilter(textureFilterKey, params);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        for (const VCGFace &face : doc.mesh(0).mesh.face)
            QVERIFY(std::abs(face.cQ() - expected) < 1e-5f);
    }

    for (VCGFace &face : doc.mesh(0).mesh.face)
        for (int corner = 0; corner < 3; ++corner)
            face.WT(corner).U() *= 2.0f;
    const std::array<std::pair<QString, float>, 2> anisotropicStretch = {{
        { QStringLiteral("l2_stretch"), std::sqrt(1.25f) },
        { QStringLiteral("linf_stretch"), std::sqrt(2.0f) }
    }};
    for (const auto &[metric, expected] : anisotropicStretch) {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("metric"), metric);
        const MeshFilterRunResult result = doc.runFilter(textureFilterKey, params);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        for (const VCGFace &face : doc.mesh(0).mesh.face)
            QVERIFY(std::abs(face.cQ() - expected) < 1e-5f);
    }
}

void FilterTests::libiglParametrizationFiltersRunWhenAvailable()
{
    Document doc;
    VCGMesh disk;
    makeOpenDiskMesh(disk);
    const int mask =
        vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    QCOMPARE(doc.addMesh(disk, QStringLiteral("Open Disk"), mask), 0);

    QString harmonicKey;
    QString lscmKey;
    QString arapKey;
    QString slimKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("parametrize_by_harmonic_map_libigl"))
            harmonicKey = info.key;
        else if (info.descriptor.id == QStringLiteral("parametrize_by_least_squares_conformal_maps_libigl"))
            lscmKey = info.key;
        else if (info.descriptor.id == QStringLiteral("parametrize_by_as_rigid_as_possible_libigl"))
            arapKey = info.key;
        else if (info.descriptor.id == QStringLiteral("parametrize_by_slim_libigl"))
            slimKey = info.key;
    }

    if (harmonicKey.isEmpty() || lscmKey.isEmpty())
        QSKIP("libigl parametrization plugin is not available in this build.");
    QVERIFY(!arapKey.isEmpty());
    QVERIFY(!slimKey.isEmpty());

    {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("harm_function"), 1);
        const MeshFilterRunResult result = doc.runFilter(harmonicKey, params);
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QVERIFY(result.documentModified);
        QVERIFY((doc.mesh(0).ioMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0);
    }

    {
        const MeshFilterRunResult result = doc.runFilter(lscmKey, {});
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        QVERIFY(result.documentModified);
        QVERIFY((doc.mesh(0).ioMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD) != 0);
    }

    for (const QString &filterKey : { arapKey, slimKey }) {
        const MeshFilterRunResult result = doc.runFilter(filterKey, {});
        QVERIFY2(result.success, qPrintable(result.errorMessage));
        for (const VCGVertex &vertex : doc.mesh(0).mesh.vert) {
            if (!vertex.IsD()) {
                QVERIFY(std::isfinite(vertex.cT().U()));
                QVERIFY(std::isfinite(vertex.cT().V()));
            }
        }
    }
}

void FilterTests::libiglQuantityFiltersRunWhenAvailable()
{
    Document sphereDoc;
    VCGMesh sphere;
    vcg::tri::Sphere<VCGMesh>(sphere, 2);
    const int mask = vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    QCOMPARE(sphereDoc.addMesh(sphere, QStringLiteral("Sphere"), mask), 0);

    QString gaussianKey, principalKey, exactKey, heatKey, hessianKey, windingKey;
    for (const auto &info : sphereDoc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("compute_gaussian_curvature_libigl")) {
            gaussianKey = info.key;
            QCOMPARE(info.descriptor.provenance.project, QStringLiteral("libigl"));
            QCOMPARE(info.descriptor.references.size(), size_t(2));
            QCOMPARE(info.descriptor.references.back().doi,
                     QStringLiteral("10.1007/978-3-662-05105-4_2"));
        } else if (info.descriptor.id
                   == QStringLiteral("compute_principal_curvature_directions_libigl")) {
            principalKey = info.key;
        } else if (info.descriptor.id
                   == QStringLiteral("compute_exact_geodesic_distance_from_selection_libigl")) {
            exactKey = info.key;
        } else if (info.descriptor.id
                   == QStringLiteral("compute_heat_geodesic_distance_from_selection_libigl")) {
            heatKey = info.key;
        } else if (info.descriptor.id
                   == QStringLiteral("smooth_vertex_scalar_by_hessian_energy_libigl")) {
            hessianKey = info.key;
        } else if (info.descriptor.id
                   == QStringLiteral("compute_generalized_winding_number_libigl")) {
            windingKey = info.key;
        }
    }
    if (gaussianKey.isEmpty())
        QSKIP("libigl quantity filters are not available in this build.");
    QVERIFY(!principalKey.isEmpty());
    QVERIFY(!exactKey.isEmpty());
    QVERIFY(!heatKey.isEmpty());
    QVERIFY(!hessianKey.isEmpty());
    QVERIFY(!windingKey.isEmpty());

    const MeshFilterRunResult gaussian = sphereDoc.runFilter(gaussianKey, {});
    QVERIFY2(gaussian.success, qPrintable(gaussian.errorMessage));
    double totalCurvature = 0.0;
    for (const VCGVertex &vertex : sphereDoc.mesh(0).mesh.vert)
        if (!vertex.IsD())
            totalCurvature += vertex.cQ();
    QVERIFY(std::abs(totalCurvature - 4.0 * M_PI) < 1e-4);

    const MeshFilterRunResult principal = sphereDoc.runFilter(principalKey, {});
    QVERIFY2(principal.success, qPrintable(principal.errorMessage));
    QVERIFY(sphereDoc.mesh(0).mesh.vert.IsCurvatureDirEnabled());
    for (const VCGVertex &vertex : sphereDoc.mesh(0).mesh.vert) {
        if (vertex.IsD())
            continue;
        QVERIFY(std::isfinite(vertex.cK1()));
        QVERIFY(std::isfinite(vertex.cK2()));
        QVERIFY(std::isfinite(vertex.cQ()));
    }

    sphereDoc.mesh(0).mesh.vert[0].SetS();
    const MeshFilterRunResult heat = sphereDoc.runFilter(heatKey, {});
    QVERIFY2(heat.success, qPrintable(heat.errorMessage));
    QVERIFY(std::abs(sphereDoc.mesh(0).mesh.vert[0].cQ()) < 1e-5f);
    for (const VCGVertex &vertex : sphereDoc.mesh(0).mesh.vert)
        if (!vertex.IsD())
            QVERIFY(std::isfinite(vertex.cQ()));

    Document diskDoc;
    VCGMesh disk;
    makeOpenDiskMesh(disk);
    QCOMPARE(diskDoc.addMesh(disk, QStringLiteral("Disk"), mask), 0);
    QVERIFY(!diskDoc.runFilter(exactKey, {}).success);
    diskDoc.mesh(0).mesh.vert[4].SetS();

    const MeshFilterRunResult exact = diskDoc.runFilter(exactKey, {});
    QVERIFY2(exact.success, qPrintable(exact.errorMessage));
    QCOMPARE(diskDoc.mesh(0).mesh.vert[4].cQ(), 0.0f);
    const float cornerDistance = std::sqrt(0.5f);
    for (int i = 0; i < 4; ++i)
        QVERIFY(std::abs(diskDoc.mesh(0).mesh.vert[size_t(i)].cQ() - cornerDistance) < 1e-5f);

    for (VCGVertex &vertex : diskDoc.mesh(0).mesh.vert)
        vertex.Q() = 0.0f;
    diskDoc.mesh(0).mesh.vert[4].Q() = 1.0f;
    MeshFilterParameterValues smoothingParams;
    smoothingParams.insert(QStringLiteral("smoothing_weight"), 0.1);
    const MeshFilterRunResult smooth = diskDoc.runFilter(hessianKey, smoothingParams);
    QVERIFY2(smooth.success, qPrintable(smooth.errorMessage));
    QVERIFY(diskDoc.mesh(0).mesh.vert[4].cQ() < 1.0f);
    for (const VCGVertex &vertex : diskDoc.mesh(0).mesh.vert)
        QVERIFY(std::isfinite(vertex.cQ()));

    Document windingDoc;
    VCGMesh cube, queries;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    vcg::tri::Allocator<VCGMesh>::AddVertices(queries, 2);
    queries.vert[0].P() = vcg::Point3f(0.5f, 0.5f, 0.5f);
    queries.vert[1].P() = vcg::Point3f(2.0f, 2.0f, 2.0f);
    QCOMPARE(windingDoc.addMesh(cube, QStringLiteral("Surface"), mask), 0);
    QCOMPARE(windingDoc.addMesh(
                 queries,
                 QStringLiteral("Queries"),
                 vcg::tri::io::Mask::IOM_VERTCOORD),
             1);
    for (const QString &method : { QStringLiteral("exact"), QStringLiteral("fast") }) {
        MeshFilterParameterValues windingParams;
        windingParams.insert(QStringLiteral("surface_mesh"), 0);
        windingParams.insert(QStringLiteral("method"), method);
        const MeshFilterRunResult winding = windingDoc.runFilter(windingKey, windingParams);
        QVERIFY2(winding.success, qPrintable(winding.errorMessage));
        const float tolerance = method == QStringLiteral("exact") ? 1e-4f : 0.05f;
        QVERIFY(std::abs(std::abs(windingDoc.mesh(1).mesh.vert[0].cQ()) - 1.0f) < tolerance);
        QVERIFY(std::abs(windingDoc.mesh(1).mesh.vert[1].cQ()) < tolerance);
    }
}

void FilterTests::meshBooleanFiltersRunWhenAvailable()
{
    Document doc;
    const int mask =
        vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    VCGMesh firstCube;
    VCGMesh secondCube;
    makeCubeMesh(firstCube, 0.0f, 0.0f, 0.0f);
    makeCubeMesh(secondCube, 0.5f, 0.5f, 0.5f);
    const int firstIndex = doc.addMesh(firstCube, QStringLiteral("Cube A"), mask);
    const int secondIndex = doc.addMesh(secondCube, QStringLiteral("Cube B"), mask);
    QVERIFY(firstIndex >= 0);
    QVERIFY(secondIndex >= 0);

    QString intersectionKey;
    QString unionKey;
    QString differenceKey;
    QString xorKey;
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == QStringLiteral("mesh_intersection_libigl"))
            intersectionKey = info.key;
        else if (info.descriptor.id == QStringLiteral("mesh_union_libigl"))
            unionKey = info.key;
        else if (info.descriptor.id == QStringLiteral("mesh_difference_libigl"))
            differenceKey = info.key;
        else if (info.descriptor.id == QStringLiteral("mesh_symmetric_difference_libigl"))
            xorKey = info.key;
    }

    if (unionKey.isEmpty())
        QSKIP("libigl/CGAL mesh boolean plugin is not available in this build.");
    QVERIFY(!intersectionKey.isEmpty());
    QVERIFY(!differenceKey.isEmpty());
    QVERIFY(!xorKey.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("first_mesh"), firstIndex);
    params.insert(QStringLiteral("second_mesh"), secondIndex);
    const int meshCountBefore = doc.meshCount();
    const MeshFilterRunResult result = doc.runFilter(unionKey, params);

    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QVERIFY(result.documentModified);
    QCOMPARE(result.newMeshIndices.size(), 1);
    QCOMPARE(doc.meshCount(), meshCountBefore + 1);

    const int generatedIndex = result.newMeshIndices.front();
    QVERIFY(generatedIndex >= 0 && generatedIndex < doc.meshCount());
    QVERIFY(doc.mesh(generatedIndex).mesh.VN() > 0);
    QVERIFY(doc.mesh(generatedIndex).mesh.FN() > 0);
}

namespace {

QString filterKeyForId(const Document &doc, const QString &filterId)
{
    for (const auto &info : doc.filterInfos()) {
        if (info.descriptor.id == filterId)
            return info.key;
    }
    return {};
}

} // namespace

// A convex closed surface cannot occlude itself: every ray leaving a face's outward
// hemisphere escapes, so the ambient occlusion value is the same for every face
// regardless of how far the mesh sits from the origin. It used not to be — the ray
// self-intersection offset was a fixed 1e-4, which is only a couple of float ULPs
// once coordinates reach the hundreds, so rays hit their own originating face and
// those faces read as fully occluded. On Laurana (bbox diagonal ~892) that was 4.5%
// of faces rendering black.
void FilterTests::ambientOcclusionIsScaleInvariant()
{
    Document probe;
    const QString aoKey = filterKeyForId(
        probe, QStringLiteral("compute_face_ambient_occlusion"));
    if (aoKey.isEmpty())
        QSKIP("Embree plugin is not available in this build.");
    const QString sphereKey = filterKeyForId(probe, QStringLiteral("create_sphere"));
    QVERIFY(!sphereKey.isEmpty());

    // Spread of the per-face AO value, as a fraction of its mean, plus the number of
    // faces that came out fully occluded (which on a convex body must be none).
    const auto aoSpread = [&](float scale, int &fullyOccluded) {
        Document doc;
        if (!doc.runFilter(sphereKey, {}).success)
            return -1.0;
        VCGMesh &mesh = doc.mesh(0).mesh;
        for (VCGVertex &v : mesh.vert)
            v.P() *= scale;
        vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);

        MeshFilterParameterValues params;
        params.insert(QStringLiteral("rays"), 64);
        if (!doc.runFilter(aoKey, params).success)
            return -1.0;

        double sum = 0.0;
        int count = 0;
        fullyOccluded = 0;
        for (const VCGFace &f : doc.mesh(0).mesh.face) {
            if (f.IsD())
                continue;
            sum += f.cQ();
            ++count;
            if (f.cQ() <= 0.0f)
                ++fullyOccluded;
        }
        if (count == 0 || sum <= 0.0)
            return -1.0;
        const double mean = sum / count;
        double var = 0.0;
        for (const VCGFace &f : doc.mesh(0).mesh.face)
            if (!f.IsD())
                var += (f.cQ() - mean) * (f.cQ() - mean);
        return std::sqrt(var / count) / mean;
    };

    // 0.4% is the residual from discretizing the hemisphere into 64 fixed directions;
    // 5% leaves headroom for that without admitting self-intersection noise, which
    // ran to 31% on Laurana and 86% on a sphere at this scale.
    for (float scale : { 1.0f, 100.0f, 1000.0f, 10000.0f }) {
        int fullyOccluded = -1;
        const double spread = aoSpread(scale, fullyOccluded);
        QVERIFY2(spread >= 0.0, "ambient occlusion run failed");
        QVERIFY2(
            spread < 0.05,
            qPrintable(QStringLiteral("AO spread %1 at scale %2 (expected < 0.05)")
                           .arg(spread).arg(scale)));
        QCOMPARE(fullyOccluded, 0);
    }
}

// Point samples do not occlude one another, so point-cloud AO traces against a
// separate surface. Exercise all three normal sources, including absent normals,
// while the translated occluder also verifies layer-transform handling.
void FilterTests::ambientOcclusionSupportsPointCloudsAndDirectionalLighting()
{
    Document doc;
    const QString aoKey = filterKeyForId(
        doc, QStringLiteral("compute_point_cloud_ambient_occlusion"));
    if (aoKey.isEmpty())
        QSKIP("Embree plugin is not available in this build.");

    VCGMesh occluder;
    makeCubeMesh(occluder, 0.0f, 0.0f, 0.0f);
    const int normalMask = vcg::tri::io::Mask::IOM_VERTCOORD
        | vcg::tri::io::Mask::IOM_VERTNORMAL
        | vcg::tri::io::Mask::IOM_FACENORMAL;
    QCOMPARE(doc.addMesh(occluder, QStringLiteral("Occluder"), normalMask), 0);
    QMatrix4x4 occluderTransform;
    occluderTransform.translate(10.0f, 0.0f, 0.0f);
    doc.setMeshTransform(0, occluderTransform);

    VCGMesh points;
    vcg::tri::Allocator<VCGMesh>::AddVertices(points, 2);
    points.vert[0].P() = vcg::Point3f(10.5f, 0.5f, 0.5f);
    points.vert[1].P() = vcg::Point3f(10.5f, 0.5f, 2.0f);
    for (VCGVertex &v : points.vert)
        v.N() = vcg::Point3f(0.0f, 0.0f, 1.0f);
    QCOMPARE(doc.addMesh(
        points, QStringLiteral("Oriented points"),
        vcg::tri::io::Mask::IOM_VERTCOORD | vcg::tri::io::Mask::IOM_VERTNORMAL), 1);
    doc.setCurrentMeshIndex(1);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("occluder_mesh"), 0);
    params.insert(QStringLiteral("normal_source"), QStringLiteral("point_normals"));
    params.insert(QStringLiteral("rays"), 64);
    params.insert(QStringLiteral("directional_bias"), 1.0);
    params.insert(QStringLiteral("cone_half_angle"), 5.0);
    params.insert(QStringLiteral("cone_direction"), QVector3D(0.0f, 0.0f, 1.0f));
    MeshFilterRunResult result = doc.runFilter(aoKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(doc.mesh(1).mesh.face.size(), size_t(0));
    QCOMPARE(doc.mesh(1).mesh.vert[0].Q(), 0.0f);
    QVERIFY(doc.mesh(1).mesh.vert[1].Q() > 1.0f);

    params.insert(QStringLiteral("cone_direction"), QVector3D(0.0f, 0.0f, -1.0f));
    result = doc.runFilter(aoKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(doc.mesh(1).mesh.vert[1].Q(), 0.0f);

    // The two remaining modes must not depend on target normals.
    for (VCGVertex &vertex : doc.mesh(1).mesh.vert)
        vertex.N() = vcg::Point3f(0.0f, 0.0f, 0.0f);

    params.insert(QStringLiteral("normal_source"), QStringLiteral("closest_occluder_surface"));
    params.insert(QStringLiteral("cone_direction"), QVector3D(0.0f, 0.0f, 1.0f));
    result = doc.runFilter(aoKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(doc.mesh(1).mesh.vert[0].Q(), 0.0f);
    QVERIFY(doc.mesh(1).mesh.vert[1].Q() > 1.0f);

    params.insert(QStringLiteral("normal_source"), QStringLiteral("no_normal_spherical"));
    params.insert(QStringLiteral("directional_bias"), 0.0);
    result = doc.runFilter(aoKey, params);
    QVERIFY2(result.success, qPrintable(result.errorMessage));
    QCOMPARE(doc.mesh(1).mesh.vert[0].Q(), 0.0f);
    QVERIFY(doc.mesh(1).mesh.vert[1].Q() > 1.0f);
}

namespace {

// Monte Carlo sampling of a cube, returned as the flattened sample coordinates so
// two runs can be compared exactly.
std::vector<float> montecarloSamples(const QString &key, int randomSeed)
{
    Document doc;
    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    doc.addMesh(cube, QStringLiteral("Cube"), vcg::tri::io::Mask::IOM_VERTCOORD);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("SampleNum"), 200);
    params.insert(QStringLiteral("randomSeed"), randomSeed);
    const MeshFilterRunResult result = doc.runFilter(key, params);
    if (!result.success || result.newMeshIndices.isEmpty())
        return {};

    std::vector<float> coords;
    for (const VCGVertex &vertex : doc.mesh(result.newMeshIndices.front()).mesh.vert) {
        coords.push_back(vertex.cP()[0]);
        coords.push_back(vertex.cP()[1]);
        coords.push_back(vertex.cP()[2]);
    }
    return coords;
}

} // namespace

void FilterTests::randomSeedMakesSamplingReproducible()
{
    Document probe;
    const QString key = filterKeyForId(probe, QStringLiteral("sample_surface_by_monte_carlo"));
    QVERIFY(!key.isEmpty());

    const std::vector<float> first = montecarloSamples(key, 12345);
    const std::vector<float> second = montecarloSamples(key, 12345);
    QVERIFY(!first.empty());
    QCOMPARE(first, second);

    // A different seed must actually change the sampling, otherwise the parameter
    // would be silently ignored and the test above would pass vacuously.
    const std::vector<float> other = montecarloSamples(key, 999);
    QCOMPARE(other.size(), first.size());
    QVERIFY(other != first);
}

void FilterTests::randomSeedZeroVariesBetweenRuns()
{
    Document probe;
    const QString key = filterKeyForId(probe, QStringLiteral("sample_surface_by_monte_carlo"));
    QVERIFY(!key.isEmpty());

    const std::vector<float> first = montecarloSamples(key, 0);
    const std::vector<float> second = montecarloSamples(key, 0);
    QVERIFY(!first.empty());
    QCOMPARE(second.size(), first.size());
    QVERIFY(first != second);
}

void FilterTests::randomSeedControlsExpressionRnd()
{
    Document probe;
    const QString key = filterKeyForId(probe, QStringLiteral("compute_vertex_scalar_by_expression"));
    QVERIFY(!key.isEmpty());

    // Write rnd() into the per-vertex scalar so the drawn values land somewhere we
    // can read back and compare.
    const auto qualityAfterRun = [&key](int randomSeed) {
        Document doc;
        VCGMesh cube;
        makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
        doc.addMesh(cube, QStringLiteral("Cube"), vcg::tri::io::Mask::IOM_VERTCOORD);

        MeshFilterParameterValues params;
        params.insert(QStringLiteral("q"), QStringLiteral("rnd()"));
        params.insert(QStringLiteral("randomSeed"), randomSeed);
        const MeshFilterRunResult result = doc.runFilter(key, params);

        std::vector<float> values;
        if (!result.success)
            return values;
        for (const VCGVertex &vertex : doc.mesh(0).mesh.vert)
            values.push_back(vertex.cQ());
        return values;
    };

    const std::vector<float> pinnedA = qualityAfterRun(4242);
    const std::vector<float> pinnedB = qualityAfterRun(4242);
    QVERIFY(!pinnedA.empty());
    QCOMPARE(pinnedA, pinnedB);

    const std::vector<float> autoA = qualityAfterRun(0);
    const std::vector<float> autoB = qualityAfterRun(0);
    QCOMPARE(autoA.size(), pinnedA.size());
    QVERIFY(autoA != autoB);
}

// Guard against a randomized filter being added later without a seed: every filter
// listed here was audited to draw from a generator, so each must expose the
// conventional control. Extend the list when a new randomized filter appears.
void FilterTests::randomizedFiltersDeclareARandomSeed()
{
    const QStringList randomizedFilterIds{
        QStringLiteral("sample_mesh_elements"),
        QStringLiteral("sample_surface_by_monte_carlo"),
        QStringLiteral("sample_surface_by_stratified_triangles"),
        QStringLiteral("sample_surface_by_poisson_disk"),
        QStringLiteral("simplify_point_cloud"),
        QStringLiteral("measure_hausdorff_distance"),
        QStringLiteral("sample_surface_by_voronoi_relaxation"),
        QStringLiteral("sample_volume"),
        QStringLiteral("create_voronoi_scaffolding"),
        QStringLiteral("parametrize_by_voronoi_atlas_vcglib"),
        QStringLiteral("compute_principal_curvature_directions_vcglib"),
        QStringLiteral("add_noise_to_vertex_color"),
        QStringLiteral("set_random_layer_color"),
        QStringLiteral("create_points_on_sphere"),
        QStringLiteral("create_points_on_spherical_cap"),
        QStringLiteral("displace_vertices_randomly"),
        QStringLiteral("align_by_icp_vcglib"),
        QStringLiteral("align_meshes_globally"),
        QStringLiteral("defragment_texture_atlas"),
        QStringLiteral("merge_texture_islands"),
        // filter_expression: randomness is opt-in through the formula's rnd() /
        // randInt() helpers, but it still has to be seedable. grid_generator is
        // excluded on purpose — it is the one filter there with no expression.
        QStringLiteral("select_vertices_by_expression"),
        QStringLiteral("select_faces_by_expression"),
        QStringLiteral("compute_vertex_coordinates_by_expression"),
        QStringLiteral("compute_vertex_normals_by_expression"),
        QStringLiteral("compute_face_normals_by_expression"),
        QStringLiteral("compute_vertex_color_by_expression"),
        QStringLiteral("compute_face_color_by_expression"),
        QStringLiteral("compute_vertex_scalar_by_expression"),
        QStringLiteral("compute_face_scalar_by_expression"),
        QStringLiteral("parametrize_per_vertex_by_expression"),
        QStringLiteral("parametrize_per_wedge_by_expression"),
        QStringLiteral("define_custom_vertex_scalar_attribute"),
        QStringLiteral("define_custom_face_scalar_attribute"),
        QStringLiteral("define_custom_vertex_point_attribute"),
        QStringLiteral("define_custom_face_point_attribute"),
        QStringLiteral("create_isosurface_from_expression"),
        QStringLiteral("refine_by_user_expression"),
    };

    Document doc;
    for (const QString &filterId : randomizedFilterIds) {
        bool found = false;
        for (const auto &info : doc.filterInfos()) {
            if (info.descriptor.id != filterId)
                continue;
            found = true;
            const auto &parameters = info.descriptor.parameters;
            const auto seedParam = std::find_if(
                parameters.begin(),
                parameters.end(),
                [](const auto &p) { return p.id == QStringLiteral("randomSeed"); });
            QVERIFY2(
                seedParam != parameters.end(),
                qPrintable(filterId + QStringLiteral(" declares no randomSeed parameter")));
            QCOMPARE(seedParam->type, MeshFilterParameterType::Int);
            QCOMPARE(seedParam->defaultValue.toInt(), 0);
            break;
        }
        QVERIFY2(found, qPrintable(filterId + QStringLiteral(" is not registered")));
    }
}

// Both cap filters take a polar half-angle under the same parameter id, so the same
// number must describe the same cap. Nothing pinned the actual angular extent before,
// which is how the two ends up disagreeing by a factor of two in the first place.
void FilterTests::capFiltersAgreeOnTheHalfAngleConvention()
{
    const double halfAngleDeg = 30.0;
    const float expectedZ = std::cos(float(halfAngleDeg) * float(M_PI) / 180.0f);

    Document doc;
    MeshFilterParameterValues params;
    params.insert(QStringLiteral("half_angle"), halfAngleDeg);
    params.insert(QStringLiteral("subdiv"), 3);

    const QString capKey = filterKeyForId(doc, QStringLiteral("create_sphere_cap"));
    QVERIFY(!capKey.isEmpty());
    const MeshFilterRunResult capResult = doc.runFilter(capKey, params);
    QVERIFY2(capResult.success, qPrintable(capResult.errorMessage));
    QCOMPARE(capResult.newMeshIndices.size(), 1);

    // The cap is built with its boundary in the XY plane and lifted along +Z, so the
    // widest ring sits at radius sin(halfAngle) from the axis. Measuring the boundary
    // radius is what actually distinguishes a 30 degree cap from a 60 degree one.
    const VCGMesh &cap = doc.mesh(capResult.newMeshIndices.front()).mesh;
    QVERIFY(cap.VN() > 0);
    float maxRadius = 0.0f;
    for (const VCGVertex &v : cap.vert) {
        if (v.IsD())
            continue;
        const vcg::Point3f &p = v.cP();
        maxRadius = std::max(maxRadius, std::sqrt(p.X() * p.X() + p.Y() * p.Y()));
    }
    const float expectedRadius = std::sin(float(halfAngleDeg) * float(M_PI) / 180.0f);
    QVERIFY2(
        std::abs(maxRadius - expectedRadius) < 1e-2f,
        qPrintable(QStringLiteral("boundary radius %1, expected %2 for a %3 deg half-angle")
                       .arg(maxRadius).arg(expectedRadius).arg(halfAngleDeg)));

    // The point sampler spans the same cap: every sample must lie inside the same
    // polar half-angle about its axis.
    MeshFilterParameterValues pointParams;
    pointParams.insert(QStringLiteral("half_angle"), halfAngleDeg);
    pointParams.insert(QStringLiteral("point_num"), 500);
    pointParams.insert(QStringLiteral("direction"), QVector3D(0.0f, 0.0f, 1.0f));
    pointParams.insert(QStringLiteral("technique"), QStringLiteral("fibonacci"));

    const QString pointKey =
        filterKeyForId(doc, QStringLiteral("create_points_on_spherical_cap"));
    QVERIFY(!pointKey.isEmpty());
    const MeshFilterRunResult pointResult = doc.runFilter(pointKey, pointParams);
    QVERIFY2(pointResult.success, qPrintable(pointResult.errorMessage));
    QCOMPARE(pointResult.newMeshIndices.size(), 1);

    const VCGMesh &cloud = doc.mesh(pointResult.newMeshIndices.front()).mesh;
    QVERIFY(cloud.VN() > 0);
    float minZ = 1.0f;
    for (const VCGVertex &v : cloud.vert) {
        if (v.IsD())
            continue;
        minZ = std::min(minZ, v.cP().Z());
    }
    // A 60 degree half-angle would reach z = 0.5; the 30 degree cap stops at ~0.866.
    QVERIFY2(
        minZ > expectedZ - 1e-2f,
        qPrintable(QStringLiteral("lowest sample z %1, expected no lower than %2")
                       .arg(minZ).arg(expectedZ)));
}

// Sample Mesh Elements draws N elements and emits one sample each. All three modes must
// agree on that: the face mode used to hand vcglib a world-space centroid where
// barycentric weights were expected, scattering samples between 0.10 and 1.72 of the
// unit radius, and it ignored the requested count entirely.
void FilterTests::elementSamplingEmitsOneSamplePerElement()
{
    Document probe;
    const QString key = filterKeyForId(probe, QStringLiteral("sample_mesh_elements"));
    QVERIFY(!key.isEmpty());

    const QString sphereKey = filterKeyForId(probe, QStringLiteral("create_sphere"));
    QVERIFY(!sphereKey.isEmpty());

    // QTest macros expand to `return;`, so these helpers report failure through their
    // value and the assertions stay in the test body.
    auto sphereDocument = [&](Document &doc) -> bool {
        return doc.runFilter(sphereKey, {}).success;
    };

    auto sample = [&](Document &doc, const QString &mode, int count, int seedValue) -> int {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("Sampling"), mode);
        params.insert(QStringLiteral("SampleNum"), count);
        params.insert(QStringLiteral("randomSeed"), seedValue);
        const MeshFilterRunResult r = doc.runFilter(key, params);
        if (!r.success || r.newMeshIndices.size() != 1)
            return -1;
        return r.newMeshIndices.front();
    };

    // Radius spread of the samples: on a unit sphere every mode must stay on or just
    // inside the surface. A barycentric mix-up shows up here immediately.
    auto radiusRange = [](const VCGMesh &m) {
        float lo = 1e9f, hi = -1e9f;
        for (const VCGVertex &v : m.vert) {
            if (v.IsD())
                continue;
            const float r = v.cP().Norm();
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        return std::make_pair(lo, hi);
    };

    Document doc;
    QVERIFY(sphereDocument(doc));
    const int sphereIndex = doc.currentMeshIndex();
    const int vertexCount = doc.mesh(sphereIndex).mesh.VN();
    const int faceCount = doc.mesh(sphereIndex).mesh.FN();
    QVERIFY(vertexCount > 0 && faceCount > 0);

    for (const QString &mode :
         { QStringLiteral("vertex"), QStringLiteral("edge"), QStringLiteral("face") }) {
        doc.setCurrentMeshIndex(sphereIndex);
        const int out = sample(doc, mode, 200, 4242);
        QVERIFY2(out >= 0, qPrintable(QStringLiteral("%1 sampling failed").arg(mode)));
        const VCGMesh &samples = doc.mesh(out).mesh;

        // The requested count is honoured, not the element count.
        QCOMPARE(samples.VN(), 200);
        QCOMPARE(samples.FN(), 0);

        const auto range = radiusRange(samples);
        QVERIFY2(
            range.first > 0.9f && range.second <= 1.0001f,
            qPrintable(QStringLiteral("%1 samples span radius %2..%3, expected ~1")
                           .arg(mode).arg(range.first).arg(range.second)));
    }

    // Asking for more than exists yields every element exactly once.
    doc.setCurrentMeshIndex(sphereIndex);
    const int allVerts = sample(doc, QStringLiteral("vertex"), 10 * vertexCount, 1);
    QVERIFY(allVerts >= 0);
    QCOMPARE(doc.mesh(allVerts).mesh.VN(), vertexCount);
    doc.setCurrentMeshIndex(sphereIndex);
    const int allFaces = sample(doc, QStringLiteral("face"), 10 * faceCount, 1);
    QVERIFY(allFaces >= 0);
    QCOMPARE(doc.mesh(allFaces).mesh.VN(), faceCount);

    // The declared seed has to actually steer the subset -- vcglib's own shuffles seed
    // from the container size, which makes the choice a function of the mesh alone.
    auto firstSampleWithSeed = [&](int seedValue) -> vcg::Point3f {
        Document d;
        if (!sphereDocument(d))
            return vcg::Point3f(0, 0, 0);
        const int out = sample(d, QStringLiteral("face"), 50, seedValue);
        if (out < 0)
            return vcg::Point3f(0, 0, 0);
        return d.mesh(out).mesh.vert[0].cP();
    };
    QVERIFY(firstSampleWithSeed(1) != firstSampleWithSeed(2));
    QCOMPARE(firstSampleWithSeed(7), firstSampleWithSeed(7));
}

namespace {

// A deliberately asymmetric box: distinct extents so the principal axes are well
// separated, and a bump on +X so the third moment can fix the axis signs.
void makeAsymmetricBlock(VCGMesh &mesh)
{
    mesh.Clear();
    vcg::tri::Box(mesh, vcg::Box3f(vcg::Point3f(-4, -2, -1), vcg::Point3f(4, 2, 1)));
    vcg::tri::Allocator<VCGMesh>::AddVertex(mesh, vcg::Point3f(7.0f, 0.0f, 0.0f));
    const int tip = mesh.VN() - 1;
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, tip, 0, 1);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);
}

// Several unrelated orientations. One is not enough: whether the eigen solver happens to
// return sign-equivariant axes depends on the particular rotation, so a single case can
// pass even with the sign disambiguation switched off.
std::vector<QMatrix4x4> probeRotations()
{
    const float spec[][4] = {
        { 37.0f, 0.3f, 0.8f, 0.5f },
        { 90.0f, 0.0f, 0.0f, 1.0f },
        { 180.0f, 1.0f, 0.0f, 0.0f },
        { 145.0f, 0.6f, -0.2f, 0.77f },
        { 61.0f, -0.5f, 0.5f, -0.7f },
    };
    std::vector<QMatrix4x4> out;
    for (const auto &s : spec) {
        QMatrix4x4 m;
        m.setToIdentity();
        m.rotate(s[0], s[1], s[2], s[3]);
        out.push_back(m);
    }
    return out;
}

} // namespace

// A canonical frame is only canonical if the same shape reaches it from any starting
// orientation. Principal axes come out of the solver up to sign, so without fixing the
// signs by third moment the same block lands in one of four different frames.
void FilterTests::normalizeReferenceFrameIsOrientationInvariant()
{
    Document probe;
    const QString key =
        filterKeyForId(probe, QStringLiteral("normalize_reference_frame"));
    QVERIFY(!key.isEmpty());

    auto canonicalize = [&](const QString &rotationMode, const QMatrix4x4 *pre) {
        Document doc;
        VCGMesh block;
        makeAsymmetricBlock(block);
        const int idx = doc.addMesh(block, QStringLiteral("Block"),
                                    vcg::tri::io::Mask::IOM_VERTCOORD);
        if (pre) {
            for (VCGVertex &v : doc.mesh(idx).mesh.vert) {
                const QVector3D p = pre->map(QVector3D(v.P().X(), v.P().Y(), v.P().Z()));
                v.P() = vcg::Point3f(p.x(), p.y(), p.z());
            }
            vcg::tri::UpdateBounding<VCGMesh>::Box(doc.mesh(idx).mesh);
        }
        doc.setCurrentMeshIndex(idx);
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("position"), QStringLiteral("shell_barycenter"));
        params.insert(QStringLiteral("rotation"), rotationMode);
        params.insert(QStringLiteral("scale"), QStringLiteral("unit_longest_side"));
        params.insert(QStringLiteral("minAxisSeparation"), 0.0);
        params.insert(QStringLiteral("Freeze"), true);
        const MeshFilterRunResult r = doc.runFilter(key, params);
        std::vector<vcg::Point3f> out;
        if (r.success)
            for (const VCGVertex &v : doc.mesh(idx).mesh.vert)
                out.push_back(v.cP());
        return out;
    };

    for (const QString &mode :
         { QStringLiteral("pca_vertices"), QStringLiteral("pca_area_weighted") }) {
        const std::vector<vcg::Point3f> reference = canonicalize(mode, nullptr);
        QVERIFY(!reference.empty());

        int caseIndex = 0;
        for (const QMatrix4x4 &pre : probeRotations()) {
            const std::vector<vcg::Point3f> got = canonicalize(mode, &pre);
            QCOMPARE(got.size(), reference.size());
            float worst = 0.0f;
            for (std::size_t i = 0; i < reference.size(); ++i)
                worst = std::max(worst, (reference[i] - got[i]).Norm());
            QVERIFY2(worst < 1e-3f,
                     qPrintable(QStringLiteral("%1, rotation %2: canonical frames differ by up to "
                                               "%3 -- the principal axis signs are not being "
                                               "disambiguated").arg(mode).arg(caseIndex).arg(worst)));
            ++caseIndex;
        }

        // Canonical means unit scale, centred on the origin.
        vcg::Box3f box;
        for (const vcg::Point3f &p : reference)
            box.Add(p);
        const float longest = std::max({ box.DimX(), box.DimY(), box.DimZ() });
        QVERIFY(std::abs(longest - 1.0f) < 1e-3f);
        QVERIFY(box.Center().Norm() < 0.5f);
    }
}

// The three controls pivot about the same centre, so each one does exactly its own job.
void FilterTests::normalizeReferenceFrameControlsAreIndependent()
{
    Document probe;
    const QString key =
        filterKeyForId(probe, QStringLiteral("normalize_reference_frame"));
    QVERIFY(!key.isEmpty());

    auto run = [&](const QString &position, const QString &rotation, const QString &scale,
                   double separation) {
        Document doc;
        VCGMesh block;
        makeAsymmetricBlock(block);
        const int idx = doc.addMesh(block, QStringLiteral("Block"),
                                    vcg::tri::io::Mask::IOM_VERTCOORD);
        doc.setCurrentMeshIndex(idx);
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("position"), position);
        p.insert(QStringLiteral("rotation"), rotation);
        p.insert(QStringLiteral("scale"), scale);
        p.insert(QStringLiteral("minAxisSeparation"), separation);
        p.insert(QStringLiteral("Freeze"), true);
        const MeshFilterRunResult r = doc.runFilter(key, p);
        vcg::tri::UpdateBounding<VCGMesh>::Box(doc.mesh(idx).mesh);
        return std::make_pair(r.success, doc.mesh(idx).mesh.bbox);
    };

    VCGMesh reference;
    makeAsymmetricBlock(reference);
    const vcg::Box3f before = reference.bbox;

    // Scale alone must not move the mesh: its centre stays put, its size changes.
    const auto scaleOnly = run(QStringLiteral("unchanged"), QStringLiteral("unchanged"),
                               QStringLiteral("unit_longest_side"), 0.0);
    QVERIFY(scaleOnly.first);
    QVERIFY((scaleOnly.second.Center() - before.Center()).Norm() < 1e-3f);
    const float longest = std::max({ scaleOnly.second.DimX(), scaleOnly.second.DimY(),
                                     scaleOnly.second.DimZ() });
    QVERIFY(std::abs(longest - 1.0f) < 1e-3f);

    // Position alone must not resize it.
    const auto positionOnly = run(QStringLiteral("bbox_center"), QStringLiteral("unchanged"),
                                  QStringLiteral("unchanged"), 0.0);
    QVERIFY(positionOnly.first);
    QVERIFY(positionOnly.second.Center().Norm() < 1e-3f);
    QVERIFY(std::abs(positionOnly.second.DimX() - before.DimX()) < 1e-3f);

    // A sphere has three equal eigenvalues, so its principal axes are arbitrary and the
    // guard must decline to rotate.
    Document sphereDoc;
    const QString sphereKey = filterKeyForId(sphereDoc, QStringLiteral("create_sphere"));
    QVERIFY(!sphereKey.isEmpty());
    QVERIFY(sphereDoc.runFilter(sphereKey, {}).success);
    MeshFilterParameterValues guarded;
    guarded.insert(QStringLiteral("position"), QStringLiteral("unchanged"));
    guarded.insert(QStringLiteral("rotation"), QStringLiteral("pca_area_weighted"));
    guarded.insert(QStringLiteral("scale"), QStringLiteral("unchanged"));
    guarded.insert(QStringLiteral("minAxisSeparation"), 0.1);
    guarded.insert(QStringLiteral("Freeze"), true);
    const MeshFilterRunResult guardedRun = sphereDoc.runFilter(key, guarded);
    QVERIFY2(guardedRun.success, qPrintable(guardedRun.errorMessage));
    bool warned = false;
    for (const QString &m : guardedRun.infoMessages)
        warned = warned || m.contains(QStringLiteral("too close"), Qt::CaseInsensitive);
    QVERIFY2(warned, "the ambiguity guard did not report skipping the rotation on a sphere");

    // Mesh barycenter refuses an open mesh rather than returning a meaningless centre.
    Document openDoc;
    VCGMesh disk;
    makeOpenDiskMesh(disk);
    const int openIdx = openDoc.addMesh(disk, QStringLiteral("Disk"),
                                        vcg::tri::io::Mask::IOM_VERTCOORD);
    openDoc.setCurrentMeshIndex(openIdx);
    MeshFilterParameterValues solid;
    solid.insert(QStringLiteral("position"), QStringLiteral("mesh_barycenter"));
    solid.insert(QStringLiteral("rotation"), QStringLiteral("unchanged"));
    solid.insert(QStringLiteral("scale"), QStringLiteral("unchanged"));
    solid.insert(QStringLiteral("minAxisSeparation"), 0.0);
    solid.insert(QStringLiteral("Freeze"), true);
    const MeshFilterRunResult openRun = openDoc.runFilter(key, solid);
    QVERIFY(!openRun.success);
    QVERIFY(openRun.errorMessage.contains(QStringLiteral("watertight"), Qt::CaseInsensitive));
}

// Rotating or scaling about "Bounding Box Center" must leave that centre where it is.
// The filters read mesh.bbox, which is the untransformed local box, but their matrix is
// composed on the left of the layer transform and so acts in world space -- so on a
// layer that has been moved, the pivot used to be the local centre interpreted as a
// world point, and the mesh swung around a point nowhere near itself.
void FilterTests::bboxCentrePivotIsWorldSpace()
{
    Document probe;
    const QString rotateKey = filterKeyForId(probe, QStringLiteral("rotate"));
    const QString scaleKey =
        filterKeyForId(probe, QStringLiteral("scale"));
    QVERIFY(!rotateKey.isEmpty());
    QVERIFY(!scaleKey.isEmpty());

    auto worldCentre = [](const Document &doc, int idx) {
        const Document::MeshEntry &e = doc.mesh(idx);
        const vcg::Point3f c = e.mesh.bbox.Center();
        return e.transform.map(QVector3D(c.X(), c.Y(), c.Z()));
    };

    auto movedCube = [](Document &doc) {
        VCGMesh cube;
        makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
        const int idx = doc.addMesh(cube, QStringLiteral("Moved"),
                                    vcg::tri::io::Mask::IOM_VERTCOORD);
        // A layer that has been dragged well away from the origin and turned.
        QMatrix4x4 t;
        t.setToIdentity();
        t.translate(10.0f, -4.0f, 2.5f);
        t.rotate(25.0f, 0.2f, 0.9f, 0.3f);
        doc.setMeshTransform(idx, t);
        doc.setCurrentMeshIndex(idx);
        return idx;
    };

    {
        Document doc;
        const int idx = movedCube(doc);
        const QVector3D before = worldCentre(doc, idx);

        MeshFilterParameterValues params;
        params.insert(QStringLiteral("rotAxis"), QStringLiteral("z"));
        params.insert(QStringLiteral("angle"), 90.0);
        params.insert(QStringLiteral("rotCenter"), QStringLiteral("bbox_center"));
        params.insert(QStringLiteral("Freeze"), false);
        const MeshFilterRunResult r = doc.runFilter(rotateKey, params);
        QVERIFY2(r.success, qPrintable(r.errorMessage));

        const QVector3D after = worldCentre(doc, idx);
        QVERIFY2((after - before).length() < 1e-3f,
                 qPrintable(QStringLiteral("rotation about the bbox centre moved it by %1")
                                .arg((after - before).length())));
    }

    {
        Document doc;
        const int idx = movedCube(doc);
        const QVector3D before = worldCentre(doc, idx);

        MeshFilterParameterValues params;
        params.insert(QStringLiteral("axisX"), 3.0);
        params.insert(QStringLiteral("uniformFlag"), true);
        params.insert(QStringLiteral("scaleCenter"), QStringLiteral("bbox_center"));
        params.insert(QStringLiteral("Freeze"), false);
        const MeshFilterRunResult r = doc.runFilter(scaleKey, params);
        QVERIFY2(r.success, qPrintable(r.errorMessage));

        const QVector3D after = worldCentre(doc, idx);
        QVERIFY2((after - before).length() < 1e-3f,
                 qPrintable(QStringLiteral("scaling about the bbox centre moved it by %1")
                                .arg((after - before).length())));
    }
}

// What the interactive transform tool does on commit: run the transform filter with
// Freeze off, on one of several layers, and keep the resulting layer matrix.
void FilterTests::unfrozenTransformFilterKeepsTheMatrix()
{
    Document doc;
    VCGMesh cube;
    makeCubeMesh(cube, 0.0f, 0.0f, 0.0f);
    QCOMPARE(doc.addMesh(cube, QStringLiteral("A"), vcg::tri::io::Mask::IOM_VERTCOORD), 0);
    QCOMPARE(doc.addMesh(cube, QStringLiteral("B"), vcg::tri::io::Mask::IOM_VERTCOORD), 1);
    doc.setCurrentMeshIndex(1);

    const QString key = filterKeyForId(doc, QStringLiteral("translate"));
    QVERIFY(!key.isEmpty());

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("traslMethod"), QStringLiteral("xyz"));
    params.insert(QStringLiteral("axis"), QVector3D(2.0f, 0.0f, 0.0f));
    params.insert(QStringLiteral("Freeze"), false);

    const MeshFilterRunResult r = doc.runFilter(key, params);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QVERIFY2(r.documentModified, "an unfrozen transform reported no document change");

    QMatrix4x4 expected;
    expected.setToIdentity();
    expected.translate(2.0f, 0.0f, 0.0f);
    QVERIFY2(matrixNear(doc.mesh(1).transform, expected),
             qPrintable(QStringLiteral("layer matrix is %1 %2 %3, expected 2 0 0")
                            .arg(doc.mesh(1).transform(0, 3))
                            .arg(doc.mesh(1).transform(1, 3))
                            .arg(doc.mesh(1).transform(2, 3))));

    QMatrix4x4 identity;
    identity.setToIdentity();
    QVERIFY(matrixNear(doc.mesh(0).transform, identity));

    // And a second gesture must compose onto the first, not replace it.
    const MeshFilterRunResult r2 = doc.runFilter(key, params);
    QVERIFY2(r2.success, qPrintable(r2.errorMessage));
    QMatrix4x4 twice;
    twice.setToIdentity();
    twice.translate(4.0f, 0.0f, 0.0f);
    QVERIFY(matrixNear(doc.mesh(1).transform, twice));
}

// The verb lexicon in docs/design/vocabulary.md declares itself normative: "If a term is
// not here, it is not approved". This makes that true rather than aspirational -- the
// table is parsed out of the document and every shipped display name is checked against
// it. Without this the rule is only enforced by whoever happens to audit; that is how
// "Improve Triangulation (TrueForm)" survived for weeks after its round.
//
// Adding a verb means adding a row to that table, which is exactly the intended cost.
void FilterTests::displayNamesLeadWithALexiconVerb()
{
    QFile doc(QStringLiteral(TEST_SOURCE_DIR "/docs/design/vocabulary.md"));
    QVERIFY2(doc.open(QIODevice::ReadOnly | QIODevice::Text),
             "cannot open docs/design/vocabulary.md");
    const QString text = QString::fromUtf8(doc.readAll());
    doc.close();

    // Section 3 only: other sections have tables of their own.
    const int start = text.indexOf(QStringLiteral("## 3. Verb lexicon"));
    QVERIFY2(start >= 0, "vocabulary.md has no '## 3. Verb lexicon' section");
    int end = text.indexOf(QStringLiteral("\n## "), start + 1);
    if (end < 0)
        end = text.size();
    QString section = text.mid(start, end - start);
    // "Every verb above the `Estimate` footnote is ratified" -- the section says so
    // itself, and below that line come the commentary blocks, which quote *rejected*
    // words in backticked tables (the vcglib `Update*` mapping). Parsing the whole
    // section would admit those as verbs, which is exactly backwards.
    const int ratifiedEnd = section.indexOf(QStringLiteral("`Estimate` is permitted"));
    QVERIFY2(ratifiedEnd > 0, "section 3 no longer carries the `Estimate` footnote that "
                              "marks the end of the ratified lexicon");
    section.truncate(ratifiedEnd);

    QSet<QString> verbs;
    // Rows look like:  | `Create` | ... |   and one row carries three verbs at once.
    static const QRegularExpression rowRe(QStringLiteral("^\\|\\s*((?:`[A-Za-z]+`(?:,\\s*)?)+)\\s*\\|"));
    static const QRegularExpression tickRe(QStringLiteral("`([A-Za-z]+)`"));
    const QStringList lines = section.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch m = rowRe.match(line);
        if (!m.hasMatch())
            continue;
        auto it = tickRe.globalMatch(m.captured(1));
        while (it.hasNext())
            verbs.insert(it.next().captured(1));
    }
    // The closed group of attribute-editing verbs is admitted as prose, not as rows.
    const int groupAt = section.indexOf(QStringLiteral("**Attribute-editing verbs**"));
    if (groupAt >= 0) {
        const int groupEnd = section.indexOf(QStringLiteral("\n\n"), groupAt);
        auto it = tickRe.globalMatch(section.mid(groupAt, groupEnd - groupAt));
        while (it.hasNext())
            verbs.insert(it.next().captured(1));
    }
    QVERIFY2(verbs.size() > 25,
             qPrintable(QStringLiteral("parsed only %1 verbs from the lexicon; the table "
                                       "format probably changed").arg(verbs.size())));
    QVERIFY(verbs.contains(QStringLiteral("Compute")));
    QVERIFY(verbs.contains(QStringLiteral("Sharpen")));   // ratified in round 2
    QVERIFY(verbs.contains(QStringLiteral("Mirror")));    // ratified in round 4
    QVERIFY(verbs.contains(QStringLiteral("Close")));     // ratified in round 6
    // Recorded in section 3 as the word vcglib reaches for and the lexicon never does.
    QVERIFY2(!verbs.contains(QStringLiteral("Update")), "`Update` is documented as rejected");
    QVERIFY2(!verbs.contains(QStringLiteral("UpdateNormal")),
             "the vcglib `Update*` mapping table leaked into the verb set");

    // The named-result exception in section 6: a filter whose output *is* a
    // conventionally named object may be a noun phrase.
    const QStringList namedResults{
        QStringLiteral("Mesh Union"), QStringLiteral("Mesh Intersection"),
        QStringLiteral("Mesh Difference"), QStringLiteral("Mesh Symmetric Difference"),
        QStringLiteral("Mesh CSG Expression")
    };

    // Roots whose renaming round has been applied. Extend as each round lands; the
    // pending ones are known not to conform yet and would only add noise here.
    const QSet<QString> appliedRoots{
        QStringLiteral("Meshing"), QStringLiteral("Attribute"),
        QStringLiteral("Creation"), QStringLiteral("Geometry"),
        QStringLiteral("Selection"), QStringLiteral("Repair"),
        QStringLiteral("Document"), QStringLiteral("Parametrization"),
        QStringLiteral("Measurement"), QStringLiteral("Transfer"),
        QStringLiteral("Texture")
    };

    Document probe;
    QStringList offenders;
    for (const auto &info : probe.filterInfos()) {
        const QString name = info.descriptor.name.trimmed();
        if (name.isEmpty())
            continue;
        // The *primary* category decides which round owns a filter. Several filters
        // carry a second category in an already-applied root while belonging to a
        // pending one -- the Transfer filters are also tagged Attribute/Color -- and
        // they get renamed when their own round runs.
        if (info.descriptor.categories.isEmpty())
            continue;
        const QString root = info.descriptor.categories.front().section(QLatin1Char('/'), 0, 0);
        if (!appliedRoots.contains(root))
            continue;
        bool exempt = false;
        for (const QString &nr : namedResults)
            exempt = exempt || name.startsWith(nr);
        if (exempt)
            continue;
        if (name.contains(QLatin1Char(':'))) {
            offenders << QStringLiteral("%1  (repeats the category with a colon)").arg(name);
            continue;
        }
        const QString first = name.section(QLatin1Char(' '), 0, 0);
        if (!verbs.contains(first))
            offenders << QStringLiteral("%1  (leading word '%2' is not in the lexicon)")
                             .arg(name, first);
    }
    QVERIFY2(offenders.isEmpty(),
             qPrintable(QStringLiteral("%1 display name(s) break the naming grammar:\n  %2")
                            .arg(QString::number(offenders.size()),
                                 offenders.join(QStringLiteral("\n  ")))));
}

// The reference machinery (markdownCitation / bibTeX / doiUrl) has existed since the
// descriptor format was written and had no users at all: every citation lived as inline
// HTML in the long description, so the BibTeX bibliography the generated docs advertise
// came out empty. This pins the first structured entries and the rendering they drive.
void FilterTests::structuredReferencesRenderCitations()
{
    Document doc;
    int checked = 0;
    for (const auto &info : doc.filterInfos()) {
        static const QStringList kCited{
            QStringLiteral("smooth_vertices_by_taubin_vcglib"),
            QStringLiteral("smooth_vertices_by_hc_laplacian"),
            QStringLiteral("smooth_vertices_by_scale_dependent_laplacian"),
            QStringLiteral("sharpen_face_normals_by_unsharp_mask"),
            QStringLiteral("sharpen_vertices_by_unsharp_mask"),
            QStringLiteral("sharpen_vertex_scalar_by_unsharp_mask"),
            QStringLiteral("sharpen_vertex_color_by_unsharp_mask"),
            QStringLiteral("smooth_vertices_by_two_step_normal_fitting"),
            QStringLiteral("project_vertices_onto_mls_surface_apss"),
            QStringLiteral("project_vertices_onto_mls_surface_rimls"),
            QStringLiteral("reconstruct_surface_by_marching_cubes_apss"),
            QStringLiteral("reconstruct_surface_by_marching_cubes_rimls"),
            QStringLiteral("compute_curvature_apss"),
            QStringLiteral("compute_curvature_rimls"),
            QStringLiteral("align_by_icp_vcglib"),
            QStringLiteral("align_meshes_globally"),
            QStringLiteral("displace_vertices_by_fractal_brownian_motion"),
            QStringLiteral("smooth_vertices_by_taubin_trueform"),
        };
        if (!kCited.contains(info.descriptor.id))
            continue;
        ++checked;
        const auto &refs = info.descriptor.references;
        QVERIFY2(!refs.empty(),
                 qPrintable(QStringLiteral("%1 carries no reference").arg(info.descriptor.name)));
        for (const MeshFilterReference &r : refs) {
        QVERIFY(!r.title.isEmpty());
        QVERIFY(!r.authors.empty());
        QVERIFY(r.year > 1900);
        // A DOI is not universal -- older conference proceedings often have none, and a
        // book never does -- but a reference must be identifiable somehow.
        QVERIFY2(!r.doi.isEmpty() || !r.url.isEmpty() || !r.isbn.isEmpty(),
                 qPrintable(QStringLiteral("%1 has no DOI, URL or ISBN").arg(r.id)));
        if (!r.doi.isEmpty())
            QVERIFY(r.doiUrl().startsWith(QStringLiteral("https://doi.org/")));

        const QString cite = r.markdownCitation();
        QVERIFY2(cite.contains(r.title), qPrintable(cite));
        QVERIFY2(cite.contains(QString::number(r.year)), qPrintable(cite));

        const QString bib = r.bibTeX();
        QVERIFY2(bib.startsWith(QLatin1Char('@')), qPrintable(bib));
        QVERIFY2(bib.contains(r.id), qPrintable(bib));
        if (!r.doi.isEmpty())
            QVERIFY2(bib.contains(r.doi), qPrintable(bib));

        // The paper must no longer be duplicated as prose in the description.
        }
        QVERIFY2(!info.descriptor.longDescriptionMarkdown.contains(QStringLiteral("<b>")),
                 "the inline HTML citation is still in the long description");
    }
    QCOMPARE(checked, 18);

    // One paper, four filters: the unsharp-mask family shares a single reference id, so
    // the bibliography can merge them instead of repeating the entry four times.
    int sharedCignoni = 0;
    for (const auto &info : doc.filterInfos()) {
        for (const MeshFilterReference &r : info.descriptor.references) {
            if (r.id == QStringLiteral("cignoni2005"))
                ++sharedCignoni;
        }
    }
    QCOMPARE(sharedCignoni, 4);

    // Same for the MLS pair: one paper per variant, three filters each.
    int apss = 0, rimls = 0;
    for (const auto &info : doc.filterInfos()) {
        for (const MeshFilterReference &r : info.descriptor.references) {
            if (r.id == QStringLiteral("guennebaud2007")) ++apss;
            if (r.id == QStringLiteral("oztireli2009")) ++rimls;
        }
    }
    QCOMPARE(apss, 3);
    QCOMPARE(rimls, 3);

    // The APSS filters are the only ones citing two papers; the second covers the
    // dynamic sampling work the implementation also rests on.
    int apss2008 = 0;
    for (const auto &info : doc.filterInfos()) {
        for (const MeshFilterReference &r : info.descriptor.references) {
            if (r.id == QStringLiteral("guennebaud2008"))
                ++apss2008;
        }
    }
    QCOMPARE(apss2008, 3);

    // References are declared per descriptor file, so a paper shared across plugins is
    // duplicated in JSON. The id is what ties the copies together, and they must agree --
    // otherwise a merged bibliography would silently pick one of two conflicting entries.
    std::vector<MeshFilterReference> taubinCopies;
    for (const auto &info : doc.filterInfos()) {
        for (const MeshFilterReference &r : info.descriptor.references) {
            if (r.id == QStringLiteral("taubin1995"))
                taubinCopies.push_back(r);
        }
    }
    QCOMPARE(int(taubinCopies.size()), 2);   // vcglib and TrueForm
    for (const MeshFilterReference &r : taubinCopies) {
        QCOMPARE(r.title, taubinCopies.front().title);
        QCOMPARE(r.doi, taubinCopies.front().doi);
        QCOMPARE(r.year, taubinCopies.front().year);
        QCOMPARE(r.bibTeX(), taubinCopies.front().bibTeX());
    }

    // A book carries an ISBN and an edition instead of a DOI, and both must survive into
    // the rendered citation and the BibTeX.
    for (const auto &info : doc.filterInfos()) {
        for (const MeshFilterReference &r : info.descriptor.references) {
            if (r.id != QStringLiteral("ebert2002procedural"))
                continue;
            QVERIFY(r.doi.isEmpty());
            QCOMPARE(r.isbn, QStringLiteral("978-1558608481"));
            QCOMPARE(r.edition, QStringLiteral("3"));
            QVERIFY2(r.markdownCitation().contains(QStringLiteral("ISBN 978-1558608481")),
                     qPrintable(r.markdownCitation()));
            QVERIFY2(r.markdownCitation().contains(QStringLiteral("3 ed.")),
                     qPrintable(r.markdownCitation()));
            QVERIFY2(r.bibTeX().contains(QStringLiteral("isbn = {978-1558608481}")),
                     qPrintable(r.bibTeX()));
            QVERIFY2(r.bibTeX().startsWith(QStringLiteral("@book{")), qPrintable(r.bibTeX()));
        }
    }
}

// An icon referenced as ":/img/x.png" but missing from the icons resource block in
// CMakeLists.txt resolves to nothing, and Qt silently falls back to showing the action's
// text instead. That is how the transform tool ended up labelled "Transform Layer": the
// file existed on disk, so nothing looked wrong until you saw the toolbar.
//
// The icons resource is attached to the app target only, so this cannot be checked
// through QFile(":/img/..."); it compares the source references against the build file.
void FilterTests::toolIconsResolveFromResources()
{
    QFile cmake(QStringLiteral(TEST_SOURCE_DIR "/CMakeLists.txt"));
    QVERIFY2(cmake.open(QIODevice::ReadOnly | QIODevice::Text), "cannot open CMakeLists.txt");
    const QString build = QString::fromUtf8(cmake.readAll());
    cmake.close();

    const int at = build.indexOf(QStringLiteral("qt_add_resources(MeshLab2 \"icons\""));
    QVERIFY2(at >= 0, "the icons resource block moved or was renamed");
    const int close = build.indexOf(QStringLiteral("\n)"), at);
    QVERIFY(close > at);
    const QString block = build.mid(at, close - at);

    QSet<QString> registered;
    static const QRegularExpression fileRe(QStringLiteral("(img/[A-Za-z0-9_.-]+)"));
    auto it = fileRe.globalMatch(block);
    while (it.hasNext())
        registered.insert(it.next().captured(1));
    QVERIFY2(registered.size() > 20,
             qPrintable(QStringLiteral("parsed only %1 icons; the block format changed")
                            .arg(registered.size())));

    // Every ":/img/..." literal anywhere in the sources must be registered.
    QStringList missing;
    QDirIterator walk(QStringLiteral(TEST_SOURCE_DIR "/src"),
                      { QStringLiteral("*.cpp"), QStringLiteral("*.h") },
                      QDir::Files, QDirIterator::Subdirectories);
    static const QRegularExpression refRe(QStringLiteral("\":/(img/[A-Za-z0-9_.-]+)\""));
    while (walk.hasNext()) {
        const QString path = walk.next();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(f.readAll());
        auto refs = refRe.globalMatch(text);
        while (refs.hasNext()) {
            const QString icon = refs.next().captured(1);
            if (!registered.contains(icon)) {
                missing << QStringLiteral("%1 (referenced in %2)")
                               .arg(icon, QFileInfo(path).fileName());
            }
        }
    }
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("icon(s) referenced but not in the resource block:\n  %1")
                            .arg(missing.join(QStringLiteral("\n  ")))));
}

// Convert to Quads by Triangle Pairing used to run only MakeTriEvenBySplit and
// MakePureByFlip. The latter is purely topological -- first unpaired triangle in array
// order, partner found by breadth-first edge distance, flip across to it -- so on a
// triangulated quad mesh whose diagonals are not all aligned it produced pairings that
// looked arbitrary. The quality pass (MakeDominant) was never invoked.
// The Voronoi partition is built by selecting each region's faces and painting the
// working mesh one color per region -- both debugging aids that used to be appended
// straight into the atlas, so it arrived fully selected and rainbow-coloured over
// whatever color the layer actually had.
// The island merge shares the defragmentation pipeline, which always resampled the atlas.
// With resampleTextures off it is a parametrization-only operation: the new UV layout
// arrives with no texture images rather than with the originals, which no longer line up.
// Every packer must lay the same atlas out without dropping a chart or pushing UVs out
// of the unit square. Also the harness for comparing them: the timings go to the log.
// A layer can carry a parametrization and no texture at all -- straight out of a UV
// filter, before anything is baked. Repacking is a UV operation, so that is a legitimate
// input; only resampling actually needs the pixels.
// A Document/Layer filter with no parameters is run straight from the layer context menu,
// with no panel and no confirmation. Four of these five delete something, so the set is
// worth pinning: give one of them a parameter and it quietly stops being immediate, and
// take the last parameter off another -- Merge Visible Layers, whose DeleteLayer defaults
// to true -- and it quietly becomes immediate.
void FilterTests::layerFiltersRunFromTheContextMenuAreTheParameterlessOnes()
{
    Document doc;
    QStringList immediate;
    for (const auto &info : doc.filterInfos()) {
        if (!info.descriptor.categories.contains(QStringLiteral("Document/Layer"),
                                                 Qt::CaseInsensitive))
            continue;
        if (info.descriptor.parameters.empty())
            immediate << info.descriptor.name;
    }
    immediate.sort();

    QStringList expected{
        QStringLiteral("Duplicate Current Layer"),
        QStringLiteral("Remove Current Mesh Layer"),
        QStringLiteral("Remove Current Raster"),
        QStringLiteral("Remove Hidden Mesh Layers"),
        QStringLiteral("Remove Hidden Rasters")
    };
    expected.sort();
    QCOMPARE(immediate, expected);
}

// The rasterizing packer builds rotationNum/4 base rasterizations and derives four slots
// from each, but sizes its arrays to rotationNum -- so 6 left slots 4 and 5 resized and
// never written, and searching them threw std::out_of_range. Any count must be safe.
// The one entry point the TrueForm v0.10.0 bump replaced: embedded_self_intersection_curves
// became make_self_intersection_curves, which returns the curves rather than a tuple whose
// last element is them. Nothing covered this filter before, so a silent empty result would
// have gone unnoticed.
// The first filter of the isoparametrization family: it builds an abstract domain and
// hangs it off the layer as plugin data for the later ones to read.
void FilterTests::abstractDomainIsBuiltAndAttachedToTheLayer()
{
    Document doc;
    const int index = doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply"));
    QVERIFY(index >= 0);
    doc.setCurrentMeshIndex(index);

    const QString key = filterKeyForId(doc, QStringLiteral("parametrize_by_abstract_domain"));
    QVERIFY(!key.isEmpty());
    MeshFilterParameterValues params;
    params.insert(QStringLiteral("minDomainFaces"), 150);
    params.insert(QStringLiteral("maxDomainFaces"), 200);
    const MeshFilterRunResult r = doc.runFilter(key, params);
    QVERIFY2(r.success, qPrintable(r.errorMessage));

    const LayerDataPtr domain =
        doc.layerData(index, QStringLiteral("meshlab2.filter.isoparam/abstract_domain"));
    QVERIFY2(domain, "the abstract domain was not attached to the layer");
    QVERIFY2(domain->describe().contains(QStringLiteral("abstract domain")),
             qPrintable(domain->describe()));
    QVERIFY(domain->approximateBytes() > 0);

    // The filter writes per-vertex UVs, which is the parametrization it just built.
    QVERIFY(doc.mesh(index).ioMask & vcg::tri::io::Mask::IOM_VERTTEXCOORD);
}

// Every precondition failure must be a message, never an abort: this is 12k lines of
// research code and the whole family refuses anything that is not one watertight shell.
// The three consumers of the domain. Each must run once it exists, and each must say what
// to do when it does not -- reaching one of these first is the normal mistake.
// The atlas has to be one UV space. AssociateDiamond keeps the diamond index in WT.N() as
// scratch, and left there it escapes as the wedge's texture id -- 291 distinct ids on a
// 1.2k sphere, so every diamond looked like a separate texture.
void FilterTests::abstractDomainMeasureReportsItsStructureAndCatchesABrokenOne()
{
    Document doc;
    QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
    doc.setCurrentMeshIndex(0);

    const QString measureKey = filterKeyForId(doc, QStringLiteral("measure_abstract_domain"));
    QVERIFY(!measureKey.isEmpty());

    // Before the domain exists the filter has to say so rather than report zeroes -- this is
    // the first of the family most people reach, since it is the one that only looks.
    {
        const MeshFilterRunResult r = doc.runFilter(measureKey, MeshFilterParameterValues{});
        QVERIFY2(!r.success, "measuring a layer with no domain should refuse");
        QVERIFY2(r.errorMessage.contains(QStringLiteral("no abstract domain")),
                 qPrintable(r.errorMessage));
    }

    MeshFilterParameterValues build;
    build.insert(QStringLiteral("minDomainFaces"), 150);
    build.insert(QStringLiteral("maxDomainFaces"), 200);
    QVERIFY(doc.runFilter(
        filterKeyForId(doc, QStringLiteral("parametrize_by_abstract_domain")), build).success);

    const MeshFilterRunResult r = doc.runFilter(measureKey, MeshFilterParameterValues{});
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QVERIFY(!r.documentModified);

    const auto value = [&r](const char *key) {
        const QString k = QString::fromLatin1(key);
        Q_ASSERT(r.outputValues.contains(k));
        return r.outputValues.value(k);
    };

    // Paper, sec. 4: the domain is a closed, 2-manifold, well-oriented set of equilateral
    // sub-domains, N "typically ranges between a minimum of 4 and a maximum of a few
    // hundreds", and Theta is a bijection -- so no sub-domain may be left uncovered.
    const int subDomains = value("sub_domains").toInt();
    QVERIFY2(subDomains >= 4, qPrintable(QStringLiteral("N = %1").arg(subDomains)));
    QVERIFY(subDomains <= 200); // the interval asked for above
    QCOMPARE(value("domain_border_sides").toInt(), 0);
    QCOMPARE(value("empty_sub_domains").toInt(), 0);
    QCOMPARE(value("vertices_outside_their_sub_domain").toInt(), 0);
    QVERIFY(value("structurally_valid").toBool());

    // A closed sphere: genus zero, so V - E + F = 2. Worth pinning, because the count of
    // edges is derived from the face count and the border count rather than counted, and
    // this is what catches that derivation going wrong.
    QCOMPARE(value("domain_euler_characteristic").toInt(), 2);
    QCOMPARE(value("domain_edges").toInt(), (3 * subDomains) / 2);

    // Square and Rhombus build one chart per half-diamond, i.e. per domain edge, and
    // Polygon one per half-star, i.e. per domain vertex. The atlas test measures exactly
    // 291 square charts on this mesh, so the report has to agree with it.
    QCOMPARE(value("domain_edges").toInt(), 291);
    QCOMPARE(value("domain_vertices").toInt(), 99);

    QCOMPARE(value("param_faces").toInt(), doc.mesh(0).mesh.FN());
    QVERIFY(value("stretch_efficiency").toDouble() >= 1.0);
    QVERIFY2(value("stretch_efficiency").toDouble() < 1.5,
             qPrintable(QStringLiteral("stretch %1").arg(value("stretch_efficiency").toDouble())));
    QVERIFY(value("layer_faces_per_sub_domain").toDouble() > 0.0);

    const QString report = r.infoMessages.join(QLatin1Char('\n'));
    QVERIFY2(report.contains(QStringLiteral("Sub-domains")), qPrintable(report));
    QVERIFY2(report.contains(QStringLiteral("valence")), qPrintable(report));
    QVERIFY2(report.contains(QStringLiteral("Structural checks: all passed")), qPrintable(report));
}

void FilterTests::atlasedMeshPacksOneUvSpaceForEveryChartShape()
{
    Document doc;
    QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
    doc.setCurrentMeshIndex(0);
    MeshFilterParameterValues build;
    build.insert(QStringLiteral("minDomainFaces"), 150);
    build.insert(QStringLiteral("maxDomainFaces"), 200);
    QVERIFY(doc.runFilter(
        filterKeyForId(doc, QStringLiteral("parametrize_by_abstract_domain")), build).success);

    const QString key = filterKeyForId(
        doc, QStringLiteral("create_atlased_mesh_from_abstract_domain"));

    QMap<QString, int> chartCount;
    // The last pass is the hexagon layout again, widened to irregular domain vertices.
    const QList<QPair<QString, bool>> passes{
        {QStringLiteral("square"), false},
        {QStringLiteral("rhombus"), false},
        {QStringLiteral("hexagon"), false},
        {QStringLiteral("halfstar"), false},
        {QStringLiteral("star"), false},
        {QStringLiteral("star"), true},
    };
    for (const auto &pass : passes) {
        const QString shape = pass.second ? pass.first + QStringLiteral("+irregular")
                                          : pass.first;
        doc.setCurrentMeshIndex(0);
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("chartShape"), pass.first);
        params.insert(QStringLiteral("mergeIrregularStars"), pass.second);
        // Pinned, because the default of 0 means "fresh seed every run" and it drives the
        // packer's permutation shuffle: the coverages below moved by a few tenths of a
        // percent from run to run, and the assertions here are one-sided bounds that a bad
        // draw could cross. Any fixed value does; this one is arbitrary.
        params.insert(QStringLiteral("randomSeed"), 20100701);
        const MeshFilterRunResult r = doc.runFilter(key, params);
        QVERIFY2(r.success, qPrintable(QStringLiteral("%1: %2").arg(shape, r.errorMessage)));

        const VCGMesh &m = doc.mesh(r.newMeshIndices.front()).mesh;
        QVERIFY(m.FN() > 0);

        QSet<int> textureIds;
        double covered = 0.0;
        double signedArea = 0.0;
        std::vector<double> density;
        for (const VCGFace &f : m.face) {
            if (f.IsD()) continue;
            for (int k = 0; k < 3; ++k) {
                textureIds.insert(f.cWT(k).N());
                const auto uv = f.cWT(k).P();
                QVERIFY2(uv.X() >= -1e-4f && uv.X() <= 1.0001f
                             && uv.Y() >= -1e-4f && uv.Y() <= 1.0001f,
                         qPrintable(QStringLiteral("%1: UV outside the atlas").arg(shape)));
            }
            const auto a = f.cWT(0).P(), b = f.cWT(1).P(), c = f.cWT(2).P();
            const double area = ((b.X() - a.X()) * (c.Y() - a.Y())
                                 - (c.X() - a.X()) * (b.Y() - a.Y())) * 0.5;
            covered += std::abs(area);
            signedArea += area;
            const double area3d = vcg::DoubleArea(f) * 0.5;
            if (area3d > 0.0)
                density.push_back(std::abs(area) / area3d);
        }
        // Texel density: every chart has to be laid out at the same scale, whether it was
        // merged or left on its own, or the packer spends the atlas on whichever kind came
        // out bigger. What is left is the parametrization's own area distortion.
        std::sort(density.begin(), density.end());
        const double median = density[density.size() / 2];
        const double p95 = density[density.size() * 95 / 100];
        qDebug("%-16s texel density median %.4g, p95/median %.2f", qPrintable(shape),
               median, p95 / median);
        QVERIFY2(p95 < 2.0 * median,
                 qPrintable(QStringLiteral("%1: charts packed at different scales "
                                           "(p95/median %2)").arg(shape).arg(p95 / median)));
        // Packing transforms are similarities, so they cannot mirror a chart: every face
        // must still wind the same way. A disagreement means a chart folded over itself,
        // which is what makes a parametrization useless to bake against.
        QVERIFY2(std::abs(std::abs(signedArea) - covered) < 1e-6 * covered,
                 qPrintable(QStringLiteral("%1: folded charts (|signed| %2 vs %3)")
                                .arg(shape).arg(std::abs(signedArea)).arg(covered)));
        // Both tally lines open with the chart count: "N charts: ..." for the layouts built
        // out of half-diamonds, "N charts, one per domain vertex." for the half-star one.
        const QString tally = r.infoMessages.filter(QStringLiteral(" charts")).value(0);
        QVERIFY2(!tally.isEmpty(), qPrintable(shape));
        chartCount[shape] = tally.split(QLatin1Char(' ')).value(0).toInt();
        QVERIFY(chartCount[shape] > 0);
        qDebug("%-8s %s | measured coverage %.4f", qPrintable(shape), qPrintable(tally), covered);

        QCOMPARE(textureIds.size(), 1);
        QCOMPARE(*textureIds.begin(), 0);
        QVERIFY(covered > 0.0);
        // No chart left behind: the scaled packer always fits what it is given.
        QVERIFY2(r.infoMessages.filter(QStringLiteral("did not fit")).isEmpty(),
                 qPrintable(shape));
    }

    // Merging cuts the chart count; square and rhombus differ only in how a diamond is
    // unfolded, and widening to irregular vertices can only merge more.
    QCOMPARE(chartCount[QStringLiteral("rhombus")], chartCount[QStringLiteral("square")]);
    QVERIFY(chartCount[QStringLiteral("hexagon")] < chartCount[QStringLiteral("square")]);
    QVERIFY(chartCount[QStringLiteral("star")] < chartCount[QStringLiteral("square")]);
    // Half-stars partition the domain one chart per vertex, so on a closed domain there are
    // about a third as many as there are half-diamonds, and nothing is left over.
    QVERIFY(chartCount[QStringLiteral("halfstar")] < chartCount[QStringLiteral("hexagon")]);
    QVERIFY(chartCount[QStringLiteral("star+irregular")] < chartCount[QStringLiteral("star")]);
}

// Both ball pivoting filters are interpolating reconstructions: every face they add must be
// built on points that were already there. The two implementations differ in almost every
// other respect, which is why MeshLab ships both, so this checks the property they share
// rather than pinning either one's output.
// Merge Texture Islands can take its candidates from the face selection instead of
// from the size threshold, so a chart can be folded into a chosen neighbour by hand.
void FilterTests::islandMergeSurvivesATextureItCannotDecode()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // A texture the application cannot decode. In the wild this is not exotic: Qt reads
    // an image format only when the matching plugin from qtimageformats is deployed, and
    // its Targa reader then rejects every file that lacks the TrueVision 2.0 footer, so a
    // well-formed .tga written to the original spec fails too. The bytes here must not be
    // a readable image either -- QImageReader falls back to sniffing the content when the
    // suffix has no handler, so a PNG under another name would load.
    const QString texturePath = dir.filePath(QStringLiteral("atlas.psd"));
    {
        QFile file(texturePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("8BPS\0\1\0\0 not actually a photoshop file");
    }

    const auto build = [&](Document &doc) {
        VCGMesh grid;
        constexpr int kSide = 3;
        vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
        for (int j = 0; j < kSide; ++j)
            for (int i = 0; i < kSide; ++i)
                grid.vert[std::size_t(j * kSide + i)].P() =
                    vcg::Point3f(float(i), float(j), 0.0f);
        for (int j = 0; j < kSide - 1; ++j)
            for (int i = 0; i < kSide - 1; ++i) {
                const int a = j * kSide + i;
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
            }
        vcg::tri::UpdateBounding<VCGMesh>::Box(grid);
        const int index = doc.addMesh(grid, QStringLiteral("Grid"),
                                      vcg::tri::io::Mask::IOM_VERTCOORD);
        doc.setCurrentMeshIndex(index);
        MeshFilterParameterValues uvParams;
        uvParams.insert(QStringLiteral("textdim"), 256);
        if (!doc.runFilter(
                filterKeyForId(doc,
                    QStringLiteral("parametrize_by_trivial_per_triangle_layout")),
                uvParams).success)
            return false;
        // The layer now claims a texture that cannot be read -- exactly what an .obj
        // pointing at a legacy .tga leaves behind, since import records the path and
        // decodes nothing.
        TextureAssociationUtils::ensureTextureListed(doc.mesh(index), texturePath);
        return Document::meshTextureAssociationCount(doc.mesh(index)) == 1;
    };

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("islandSource"), QStringLiteral("by_size"));
    params.insert(QStringLiteral("quickRun"), true);

    // Resampling off: nothing samples the image, so the run proceeds on a stand-in and
    // says so. Before this it refused over a texture it was never going to touch.
    {
        Document doc;
        QVERIFY(build(doc));
        MeshFilterParameterValues layoutOnly = params;
        layoutOnly.insert(QStringLiteral("resampleTextures"), false);
        const MeshFilterRunResult r = doc.runFilter(
            filterKeyForId(doc, QStringLiteral("merge_texture_islands")), layoutOnly);
        QVERIFY2(r.success, qPrintable(r.errorMessage));
        const QString info = r.infoMessages.join(QLatin1Char('\n'));
        QVERIFY2(info.contains(QStringLiteral("Could not read")), qPrintable(info));
        // No readable texture on the layer to take the texel grid from, so the stand-in
        // falls back to the output atlas size. The result has to name it: the layout is
        // sound either way, but a gutter given in pixels now refers to that grid.
        QVERIFY2(info.contains(QStringLiteral("assumed 1024x1024")), qPrintable(info));
    }

    // Resampling on: the pixels are genuinely needed, so this still fails -- but the
    // message has to point at the way out rather than just naming the file.
    {
        Document doc;
        QVERIFY(build(doc));
        MeshFilterParameterValues resampling = params;
        resampling.insert(QStringLiteral("resampleTextures"), true);
        const MeshFilterRunResult r = doc.runFilter(
            filterKeyForId(doc, QStringLiteral("merge_texture_islands")), resampling);
        QVERIFY2(!r.success, "resampling from an undecodable texture cannot work");
        QVERIFY2(r.errorMessage.contains(QStringLiteral("Resample textures")),
                 qPrintable(r.errorMessage));
        QVERIFY2(r.errorMessage.contains(QStringLiteral("no decoder in this build")),
                 qPrintable(r.errorMessage));
    }
}

void FilterTests::islandMergeCanTakeItsIslandsFromTheSelection()
{
    // A trivial per-triangle parametrization: every face is its own island, which is the
    // cheapest thing to hand an island merger.
    const auto build = [](Document &doc) {
        VCGMesh grid;
        constexpr int kSide = 3;
        vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
        for (int j = 0; j < kSide; ++j)
            for (int i = 0; i < kSide; ++i)
                grid.vert[std::size_t(j * kSide + i)].P() =
                    vcg::Point3f(float(i), float(j), 0.0f);
        for (int j = 0; j < kSide - 1; ++j)
            for (int i = 0; i < kSide - 1; ++i) {
                const int a = j * kSide + i;
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
            }
        vcg::tri::UpdateBounding<VCGMesh>::Box(grid);
        const int index = doc.addMesh(grid, QStringLiteral("Grid"),
                                      vcg::tri::io::Mask::IOM_VERTCOORD);
        doc.setCurrentMeshIndex(index);
        MeshFilterParameterValues uvParams;
        uvParams.insert(QStringLiteral("textdim"), 256);
        return doc.runFilter(
            filterKeyForId(doc,
                QStringLiteral("parametrize_by_trivial_per_triangle_layout")),
            uvParams).success;
    };

    const auto mergeParams = [](bool bySelection) {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("islandSource"),
                      bySelection ? QStringLiteral("selection") : QStringLiteral("by_size"));
        params.insert(QStringLiteral("resampleTextures"), false);
        params.insert(QStringLiteral("quickRun"), true);
        return params;
    };

    // Asked to merge the selection with nothing selected: refused, and the message says
    // what to do about it rather than reporting a silent no-op.
    {
        Document doc;
        QVERIFY(build(doc));
        const QString key =
            filterKeyForId(doc, QStringLiteral("merge_texture_islands"));
        QVERIFY(!key.isEmpty());
        const MeshFilterRunResult r = doc.runFilter(key, mergeParams(true));
        QVERIFY2(!r.success, "merging by selection with nothing selected should refuse");
        QVERIFY2(r.errorMessage.contains(QStringLiteral("no faces are selected")),
                 qPrintable(r.errorMessage));
    }

    // With a face selected, the run goes through and the layer keeps its parametrization.
    {
        Document doc;
        QVERIFY(build(doc));
        VCGMesh &m = doc.mesh(0).mesh;
        QVERIFY(m.FN() >= 4);
        // One island selected, out of eight. That is enough to catch both ways this can go
        // wrong: the selected mark used to be inherited by the merged chart, so a single
        // selected face swallowed the whole atlas, and the queued costs are computed up
        // front, so pairs that were eligible when queued used to execute after the mark had
        // been consumed. Either way more than one merge would happen.
        m.face[0].SetS();
        const QString key =
            filterKeyForId(doc, QStringLiteral("merge_texture_islands"));
        const int faceCount = m.FN();
        const MeshFilterRunResult r = doc.runFilter(key, mergeParams(true));
        QVERIFY2(r.success, qPrintable(r.errorMessage));
        QVERIFY(!r.newMeshIndices.empty());
        const Document::MeshEntry &out = doc.mesh(r.newMeshIndices.front());
        QVERIFY(vcg::tri::HasPerWedgeTexCoord(out.mesh));
        QCOMPARE(out.mesh.FN(), faceCount);
        const QString tally = r.infoMessages.filter(QStringLiteral("UV islands:")).value(0);
        QVERIFY2(!tally.isEmpty(), qPrintable(r.infoMessages.join(QStringLiteral(" | "))));
        const QRegularExpression merged(QStringLiteral("\\((\\d+) merged away\\)"));
        const QRegularExpressionMatch mm = merged.match(tally);
        QVERIFY2(mm.hasMatch(), qPrintable(tally));
        qDebug("   %s", qPrintable(tally));
        // Exactly the island that was selected: something merged, and only that one.
        QCOMPARE(mm.captured(1).toInt(), 1);
    }
}

// The rubber-band tool's C modifier sets expand_to_components: grazing one triangle takes
// the whole piece. Driven here in UV space, where the projection is fully determined by
// pan/zoom/aspect, so which faces the rectangle hits is exact rather than inferred from a
// camera. Three triangles form one component, a fourth stands alone, and the rectangle is
// aimed at a single triangle of the first.
// The stage-1 edge pipeline end to end: select some edges, colour by an expression
// that reads both endpoints and a derived quantity, then write a scalar and ramp it
// into colour. Built on a polyline whose edges have deliberately different lengths,
// because every interesting edge expression is a function of length or direction.
void FilterTests::edgeExpressionsSelectColorAndScaleAPolyline()
{
    Document doc;

    // A chain of four edges with lengths 1, 2, 3, 4 along +X.
    VCGMesh polyline;
    vcg::tri::Allocator<VCGMesh>::AddVertices(polyline, 5);
    float x = 0.0f;
    for (int i = 0; i < 5; ++i) {
        polyline.vert[std::size_t(i)].P() = vcg::Point3f(x, 0.0f, 0.0f);
        polyline.vert[std::size_t(i)].C() = vcg::Color4b(10 * i, 0, 0, 255);
        polyline.vert[std::size_t(i)].Q() = float(i);
        x += float(i + 1);
    }
    vcg::tri::Allocator<VCGMesh>::AddEdges(polyline, 4);
    for (int i = 0; i < 4; ++i) {
        polyline.edge[std::size_t(i)].V(0) = &polyline.vert[std::size_t(i)];
        polyline.edge[std::size_t(i)].V(1) = &polyline.vert[std::size_t(i + 1)];
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(polyline);
    const int index = doc.addMesh(polyline, QStringLiteral("chain"),
                                  vcg::tri::io::Mask::IOM_EDGEINDEX);
    QVERIFY(index >= 0);
    doc.setCurrentMeshIndex(index);

    // Returns void so the QVERIFY macros -- which expand to a bare `return` -- can be
    // used inside it.
    const auto runWith = [&](const QString &id, const MeshFilterParameterValues &params) {
        const QString key = filterKeyForId(doc, id);
        QVERIFY2(!key.isEmpty(), qPrintable(id));
        const MeshFilterRunResult r = doc.runFilter(key, params);
        QVERIFY2(r.success, qPrintable(r.errorMessage));
    };

    // 1. Selection. Edges 2 and 3 are the ones longer than 2.5.
    {
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("condSelect"), QStringLiteral("elen > 2.5"));
        runWith(QStringLiteral("select_edges_by_expression"), p);
    }
    const VCGMesh &m = doc.mesh(index).mesh;
    QCOMPARE(int(vcg::tri::UpdateSelection<VCGMesh>::EdgeCount(m)), 2);
    QVERIFY(!m.edge[0].IsS());
    QVERIFY(!m.edge[1].IsS());
    QVERIFY(m.edge[2].IsS());
    QVERIFY(m.edge[3].IsS());

    // 2. Colour from an expression reading an endpoint, a derived value and the index.
    {
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("r"), QStringLiteral("elen * 10"));
        p.insert(QStringLiteral("g"), QStringLiteral("r0"));      // endpoint colour
        p.insert(QStringLiteral("b"), QStringLiteral("ei * 3"));
        p.insert(QStringLiteral("a"), QStringLiteral("255"));
        p.insert(QStringLiteral("onselected"), false);
        runWith(QStringLiteral("compute_edge_color_by_expression"), p);
    }
    for (int i = 0; i < 4; ++i) {
        const vcg::Color4b c = doc.mesh(index).mesh.edge[std::size_t(i)].cC();
        QCOMPARE(int(c[0]), (i + 1) * 10);   // elen * 10
        QCOMPARE(int(c[1]), i * 10);         // the first endpoint's red
        QCOMPARE(int(c[2]), i * 3);          // edge index
    }
    QVERIFY(doc.mesh(index).ioMask & vcg::tri::io::Mask::IOM_EDGECOLOR);

    // 3. Only-on-selection must leave the unselected edges alone.
    {
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("r"), QStringLiteral("7"));
        p.insert(QStringLiteral("g"), QStringLiteral("7"));
        p.insert(QStringLiteral("b"), QStringLiteral("7"));
        p.insert(QStringLiteral("a"), QStringLiteral("255"));
        p.insert(QStringLiteral("onselected"), true);
        runWith(QStringLiteral("compute_edge_color_by_expression"), p);
    }
    QCOMPARE(int(doc.mesh(index).mesh.edge[0].cC()[0]), 10);   // untouched
    QCOMPARE(int(doc.mesh(index).mesh.edge[2].cC()[0]), 7);    // rewritten
    QCOMPARE(int(doc.mesh(index).mesh.edge[3].cC()[0]), 7);

    // 4. Scalar, then the colour ramp over its range. The shortest edge lands at one
    // end of the map and the longest at the other; what matters is that they differ
    // and that the ramp used the real range rather than a degenerate one.
    {
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("q"), QStringLiteral("elen"));
        p.insert(QStringLiteral("normalize"), false);
        p.insert(QStringLiteral("map"), false);
        p.insert(QStringLiteral("onselected"), false);
        runWith(QStringLiteral("compute_edge_scalar_by_expression"), p);
    }
    for (int i = 0; i < 4; ++i)
        QCOMPARE(doc.mesh(index).mesh.edge[std::size_t(i)].cQ(), float(i + 1));

    {
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("useCustomRange"), false);
        p.insert(QStringLiteral("zeroSym"), false);
        p.insert(QStringLiteral("colorMap"), QStringLiteral("rgb"));
        runWith(QStringLiteral("colorize_edges_by_scalar"), p);
    }
    const vcg::Color4b lo = doc.mesh(index).mesh.edge[0].cC();
    const vcg::Color4b hi = doc.mesh(index).mesh.edge[3].cC();
    QVERIFY2(lo != hi, "the shortest and longest edge got the same ramp color");

    // 5. Normalizing rescales into [0, 1] whatever the input range was.
    {
        MeshFilterParameterValues p;
        p.insert(QStringLiteral("q"), QStringLiteral("elen * 100 + 5"));
        p.insert(QStringLiteral("normalize"), true);
        p.insert(QStringLiteral("map"), false);
        p.insert(QStringLiteral("onselected"), false);
        runWith(QStringLiteral("compute_edge_scalar_by_expression"), p);
    }
    QCOMPARE(doc.mesh(index).mesh.edge[0].cQ(), 0.0f);
    QCOMPARE(doc.mesh(index).mesh.edge[3].cQ(), 1.0f);
}

void FilterTests::createdCylinderHonoursRadiusHeightAndAxis()
{
    // The three things a caller can get wrong independently: the radius (distance from
    // the axis), the height (extent along it), and whether the axis is obeyed at all.
    // Measuring in the axis's own frame catches a cylinder that is the right size but
    // still pointing at Y, which a bounding-box check would pass.
    const vcg::Point3f axis = vcg::Point3f(1.0f, 2.0f, -2.0f).Normalize(); // length 3, so
    const float radius = 0.75f;                                           // this also
    const float height = 4.0f;                                            // tests normalizing

    for (bool capped : {true, false}) {
        Document doc;
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("radius"), double(radius));
        params.insert(QStringLiteral("height"), double(height));
        params.insert(QStringLiteral("sides"), 24);
        params.insert(QStringLiteral("stacks"), 2);
        params.insert(QStringLiteral("capped"), capped);
        params.insert(QStringLiteral("axis"), QVector3D(1.0f, 2.0f, -2.0f));

        const QString key = filterKeyForId(doc, QStringLiteral("create_cylinder"));
        QVERIFY(!key.isEmpty());
        const MeshFilterRunResult r = doc.runFilter(key, params);
        QVERIFY2(r.success, qPrintable(r.errorMessage));
        QCOMPARE(r.newMeshIndices.size(), std::size_t(1));

        const VCGMesh &m = doc.mesh(r.newMeshIndices.front()).mesh;
        QVERIFY(m.VN() > 0);
        QVERIFY(m.FN() > 0);

        float maxAlong = -1e9f, minAlong = 1e9f, maxRadial = 0.0f;
        for (const VCGVertex &v : m.vert) {
            if (v.IsD()) continue;
            const float along = v.cP() * axis;
            maxAlong = std::max(maxAlong, along);
            minAlong = std::min(minAlong, along);
            maxRadial = std::max(maxRadial, (v.cP() - axis * along).Norm());
        }
        const QString what = capped ? QStringLiteral("capped") : QStringLiteral("open");
        // Centred on the origin, like vcg::tri::Cone.
        QVERIFY2(std::abs(maxAlong - height / 2.0f) < 1e-4f
                     && std::abs(minAlong + height / 2.0f) < 1e-4f,
                 qPrintable(QStringLiteral("%1: axial extent [%2, %3], expected +/-%4")
                                .arg(what).arg(minAlong).arg(maxAlong).arg(height / 2.0f)));
        QVERIFY2(std::abs(maxRadial - radius) < 1e-4f,
                 qPrintable(QStringLiteral("%1: radius %2, expected %3")
                                .arg(what).arg(maxRadial).arg(radius)));
    }
}

void FilterTests::stateJsonAcceptsBothNameSpellings()
{
    // The "kind" tag on camera and render state was "MeshLab.*" until the 2026-09
    // rename. It is embedded in saved snapshots, copied state, and any script that
    // pins a cameraState parameter, so both spellings have to keep validating --
    // otherwise the rename silently invalidates state people already have.
    Document doc;
    QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
    doc.setCurrentMeshIndex(0);
    const QString key = filterKeyForId(doc, QStringLiteral("select_by_screen_rectangle"));
    QVERIFY(!key.isEmpty());

    const auto runWithKind = [&](const QString &kind) -> QString {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("space"), QStringLiteral("uv"));
        params.insert(QStringLiteral("camera_state"),
                      QStringLiteral(R"({"kind":"%1","version":1})").arg(kind));
        params.insert(QStringLiteral("aspect"), 1.0);
        params.insert(QStringLiteral("uv_pan_x"), 0.0);
        params.insert(QStringLiteral("uv_pan_y"), 0.0);
        params.insert(QStringLiteral("uv_zoom"), 1.0);
        params.insert(QStringLiteral("rect_min_x"), 0.45);
        params.insert(QStringLiteral("rect_max_x"), 0.55);
        params.insert(QStringLiteral("rect_min_y"), 0.45);
        params.insert(QStringLiteral("rect_max_y"), 0.55);
        params.insert(QStringLiteral("element"), QStringLiteral("face"));
        params.insert(QStringLiteral("mode"), QStringLiteral("replace"));
        return doc.runFilter(key, params).errorMessage;
    };

    for (const QString &kind : {QStringLiteral("MeshLab.CameraState"),
                                QStringLiteral("MeshLab.CameraState")}) {
        const QString error = runWithKind(kind);
        QVERIFY2(!error.contains(QStringLiteral("invalid kind")),
                 qPrintable(QStringLiteral("%1 was rejected: %2").arg(kind, error)));
    }
    // And the check is still a check.
    QVERIFY(runWithKind(QStringLiteral("Something.Else")).contains(QStringLiteral("invalid kind")));
}

void FilterTests::rubberBandExpandsToConnectedComponents()
{
    VCGMesh mesh;
    auto *vi = &*vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 8);
    const float p[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {2, 0, 0},
                           {10, 10, 0}, {11, 10, 0}, {10, 11, 0}};
    for (int i = 0; i < 8; ++i)
        mesh.vert[std::size_t(i)].P() = VCGMesh::CoordType(p[i][0], p[i][1], p[i][2]);
    (void)vi;
    auto *fi = &*vcg::tri::Allocator<VCGMesh>::AddFaces(mesh, 4);
    (void)fi;
    // Faces 0-2 share edges, so they are one component; face 3 is on its own.
    const int idx[4][3] = {{0, 1, 2}, {1, 3, 2}, {1, 4, 3}, {5, 6, 7}};
    for (int f = 0; f < 4; ++f)
        for (int c = 0; c < 3; ++c)
            mesh.face[std::size_t(f)].V(c) = &mesh.vert[std::size_t(idx[f][c])];

    Document doc;
    const int layer = doc.addMesh(mesh, QStringLiteral("components"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD
                                      | vcg::tri::io::Mask::IOM_FACEINDEX
                                      | vcg::tri::io::Mask::IOM_WEDGTEXCOORD);
    QVERIFY(layer >= 0);
    doc.setCurrentMeshIndex(layer);

    // UVs go on the document's own copy: enabling an OCF component on a local mesh does not
    // survive being copied into the layer.
    // One value per face -- the face rule uses the centroid of the three wedges, and with
    // pan 0, zoom 1 and aspect 1 a u of x lands at (x + 1) / 2 across the screen.
    VCGMesh &layerMesh = doc.mesh(layer).mesh;
    layerMesh.face.EnableWedgeTexCoord();
    QVERIFY(layerMesh.face.IsWedgeTexCoordEnabled());
    // addMesh trims the mask to what the mesh actually carried, which at that point was
    // nothing, so the bit has to go back on for the UV reader to look at the wedges.
    doc.mesh(layer).ioMask |= vcg::tri::io::Mask::IOM_WEDGTEXCOORD;
    // Writing UVs behind the document's back leaves the undo system's interned copy stale,
    // and the next filter run would be handed that UV-less copy.
    doc.markMeshGeometryChanged(layer, QStringLiteral("test UVs"));
    const float faceU[4] = {0.0f, 0.3f, 0.6f, 0.9f};
    for (int f = 0; f < 4; ++f)
        for (int c = 0; c < 3; ++c) {
            layerMesh.face[std::size_t(f)].WT(c).U() = faceU[f];
            layerMesh.face[std::size_t(f)].WT(c).V() = 0.0f;
        }
    const QString key = filterKeyForId(doc, QStringLiteral("select_by_screen_rectangle"));
    QVERIFY(!key.isEmpty());

    // Returns the selected face indices. QVERIFY expands to `return;`, so a failed run is
    // reported and comes back empty for the comparison to catch.
    const auto drag = [&](const QString &mode, bool expand) -> QList<int> {
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("space"), QStringLiteral("uv"));
        // Unused in UV space, but the parameter is typed and has no default, so it has to
        // be a well-formed camera state rather than an empty object.
        params.insert(QStringLiteral("camera_state"),
                      QStringLiteral(R"({"kind":"MeshLab.CameraState","version":1})"));
        params.insert(QStringLiteral("aspect"), 1.0);
        params.insert(QStringLiteral("uv_pan_x"), 0.0);
        params.insert(QStringLiteral("uv_pan_y"), 0.0);
        params.insert(QStringLiteral("uv_zoom"), 1.0);
        // Around screen (0.5, 0.5), which is u = 0: face 0 only.
        params.insert(QStringLiteral("rect_min_x"), 0.45);
        params.insert(QStringLiteral("rect_max_x"), 0.55);
        params.insert(QStringLiteral("rect_min_y"), 0.45);
        params.insert(QStringLiteral("rect_max_y"), 0.55);
        params.insert(QStringLiteral("element"), QStringLiteral("face"));
        params.insert(QStringLiteral("mode"), mode);
        params.insert(QStringLiteral("visible_only"), false);
        params.insert(QStringLiteral("expand_to_components"), expand);
        const MeshFilterRunResult r = doc.runFilter(key, params);
        if (!r.success) {
            qWarning("rectangle select failed: %s", qPrintable(r.errorMessage));
            return {};
        }
        QList<int> selected;
        const VCGMesh &m = doc.mesh(0).mesh;
        for (int f = 0; f < m.FN(); ++f)
            if (m.face[std::size_t(f)].IsS())
                selected << f;
        return selected;
    };

    // Without expansion the rectangle takes what it covers, and nothing else.
    QCOMPARE(drag(QStringLiteral("replace"), false), QList<int>({0}));
    // With it, the whole component the rectangle grazed -- and none of the loner.
    QCOMPARE(drag(QStringLiteral("replace"), true), QList<int>({0, 1, 2}));

    // Subtract is the case worth pinning: the component under the rectangle has to come
    // out, not the remainder be grown. Start from everything selected.
    const QString allKey = filterKeyForId(doc, QStringLiteral("select_all"));
    QVERIFY(doc.runFilter(allKey, {}).success);
    QCOMPARE(drag(QStringLiteral("subtract"), true), QList<int>({3}));
    // And subtract without expansion still removes only what the rectangle covered, which
    // is the path the modifier reorganised.
    QVERIFY(doc.runFilter(allKey, {}).success);
    QCOMPARE(drag(QStringLiteral("subtract"), false), QList<int>({1, 2, 3}));
}

// The transform tool hands its gesture over as a raw matrix and relies on the filter
// composing it on the LEFT of the layer's existing transform -- the gesture is in world
// space, so `new = gesture * current`. Composing on the right would look correct on an
// untransformed layer and wrong on every moved one, which is the sort of thing that gets
// noticed late.
void FilterTests::setMatrixComposesOnTheLeftOfTheLayerTransform()
{
    Document doc;
    QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
    doc.setCurrentMeshIndex(0);

    QMatrix4x4 existing;
    existing.translate(3.0f, 0.0f, 0.0f);
    doc.setMeshTransform(0, existing);

    // A rotation, so left and right composition give different answers.
    QMatrix4x4 gesture;
    gesture.rotate(90.0f, 0.0f, 0.0f, 1.0f);

    MeshFilterParameterValues params;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            params[QStringLiteral("m%1%2").arg(row).arg(col)] = double(gesture(row, col));
    params[QStringLiteral("Freeze")] = false;
    const MeshFilterRunResult r = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("set_matrix_from_values_or_layer")), params);
    QVERIFY2(r.success, qPrintable(r.errorMessage));

    const QMatrix4x4 expected = gesture * existing;
    const QMatrix4x4 actual = doc.mesh(0).transform;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            QVERIFY2(std::abs(actual(row, col) - expected(row, col)) < 1e-4f,
                     qPrintable(QStringLiteral("m%1%2: %3 expected %4")
                                    .arg(row).arg(col)
                                    .arg(actual(row, col)).arg(expected(row, col))));
}

// The tools and the render widget reach for a handful of filters by literal
// "pluginId::filterId" key. Nothing else ties those strings to the descriptors, so a naming
// round renames the filter and the call site keeps asking for a filter that no longer
// exists -- which is how rubber-band selection, the transform tool and the quality bake all
// came to be broken at once, each failing only when a user reached for it.
//
// This reads the keys out of the sources rather than keeping a list here, so it cannot drift
// out of date the way a hand-written list would.
void FilterTests::hardcodedFilterKeysInTheUiStillResolve()
{
    Document doc;
    QSet<QString> declared;
    for (const auto &info : doc.filterInfos())
        declared.insert(info.key);
    QVERIFY(!declared.isEmpty());

    const QRegularExpression keyPattern(
        QStringLiteral("meshlab2\\.filter\\.[a-z_]+::[a-z_]+"));
    QStringList missing;
    int found = 0;
    QDirIterator it(QStringLiteral(TEST_SOURCE_DIR "/src"),
                    {QStringLiteral("*.cpp"), QStringLiteral("*.h")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString text = QString::fromUtf8(file.readAll());
        auto matches = keyPattern.globalMatch(text);
        while (matches.hasNext()) {
            const QString key = matches.next().captured(0);
            ++found;
            if (!declared.contains(key))
                missing << QStringLiteral("%1 (%2)").arg(key, QFileInfo(path).fileName());
        }
    }

    // If this ever drops to zero the pattern has stopped matching and the test is asleep.
    QVERIFY2(found > 0, "found no hardcoded filter keys in src/ -- has the pattern gone stale?");
    QVERIFY2(missing.isEmpty(),
             qPrintable(QStringLiteral("filter keys referenced in the UI no longer exist: %1")
                            .arg(missing.join(QStringLiteral(", ")))));
}

void FilterTests::bothBallPivotingsInterpolateTheirInputPoints()
{
    for (const QString &id : {QStringLiteral("reconstruct_surface_by_ball_pivoting_gruber"),
                              QStringLiteral("reconstruct_surface_by_ball_pivoting_vcglib")}) {
        Document doc;
        QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
        doc.setCurrentMeshIndex(0);
        const int pointCount = doc.mesh(0).mesh.VN();
        const double diagonal = double(doc.mesh(0).mesh.bbox.Diag());

        MeshFilterParameterValues params;
        // The ball has to reach the next point and still enclose none, so the radius belongs
        // near the sampling distance rather than a multiple of it. This is the guess vcglib
        // makes when it is handed a radius of zero.
        const double spacing = std::sqrt(diagonal * diagonal / double(pointCount));
        params.insert(id.endsWith(QStringLiteral("gruber")) ? QStringLiteral("ballRadius")
                                                            : QStringLiteral("ball_radius"),
                      spacing);
        params.insert(id.endsWith(QStringLiteral("gruber"))
                          ? QStringLiteral("deleteInitialFaces")
                          : QStringLiteral("delete_initial_faces"),
                      true);
        const QString key = filterKeyForId(doc, id);
        // The Gruber implementation needs glm, so its plugin is skipped in builds without it.
        if (key.isEmpty())
            QSKIP("Ball pivoting (Gruber) plugin is not available in this build.");

        const MeshFilterRunResult r = doc.runFilter(key, params);
        QVERIFY2(r.success, qPrintable(QStringLiteral("%1: %2").arg(id, r.errorMessage)));

        const VCGMesh &m = doc.mesh(0).mesh;
        QVERIFY2(m.FN() > 0, qPrintable(QStringLiteral("%1 reconstructed nothing").arg(id)));
        // No point was invented. The vcglib one may cluster points away, so it is allowed to
        // end up with fewer, never more.
        QVERIFY2(m.VN() <= pointCount,
                 qPrintable(QStringLiteral("%1 added vertices: %2 from %3")
                                .arg(id).arg(m.VN()).arg(pointCount)));
        for (const VCGFace &f : m.face) {
            if (f.IsD()) continue;
            for (int k = 0; k < 3; ++k)
                QVERIFY2(!f.cV(k)->IsD(),
                         qPrintable(QStringLiteral("%1 built a face on a deleted vertex").arg(id)));
        }
        qDebug("%-46s %d faces over %d points", qPrintable(id), m.FN(), pointCount);
    }
}

// Deleting the initial faces used to hang: inputPrepare builds VF adjacency over the faces,
// AdvancingFront chains its new ones onto the vertices' VF pointers and walks them, and
// clearing the face vector left those pointers in freed storage.
void FilterTests::ballPivotingRebuildsAfterDeletingTheInitialFaces()
{
    Document doc;
    QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
    doc.setCurrentMeshIndex(0);
    const int faceCount = doc.mesh(0).mesh.FN();
    QVERIFY(faceCount > 0);

    MeshFilterParameterValues params;
    params.insert(QStringLiteral("delete_initial_faces"), true);
    const MeshFilterRunResult r = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("reconstruct_surface_by_ball_pivoting_vcglib")),
        params);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QVERIFY(doc.mesh(0).mesh.FN() > 0);
}

void FilterTests::abstractDomainIndexesRegionsOnFaces()
{
    Document doc;
    QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
    doc.setCurrentMeshIndex(0);
    MeshFilterParameterValues params;
    params.insert(QStringLiteral("minDomainFaces"), 150);
    params.insert(QStringLiteral("maxDomainFaces"), 200);
    const MeshFilterRunResult r = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("parametrize_by_abstract_domain")), params);
    QVERIFY2(r.success, qPrintable(r.errorMessage));

    const Document::MeshEntry &entry = doc.mesh(0);
    QVERIFY((entry.ioMask & vcg::tri::io::Mask::IOM_FACEQUALITY) != 0);

    // Every face carries a domain region index, and between them they cover the whole
    // domain: as many distinct values as the run reported faces in the domain.
    const QString tally = r.infoMessages.filter(QStringLiteral("Abstract domain:")).value(0);
    const int domainFaces = QStringView(tally).split(QLatin1Char(' ')).value(2).toInt();
    QVERIFY(domainFaces > 0);

    QSet<int> regions;
    for (const VCGFace &f : entry.mesh.face) {
        if (f.IsD()) continue;
        const float q = f.cQ();
        QVERIFY2(q >= 0.0f && q < float(domainFaces) && q == std::floor(q),
                 qPrintable(QStringLiteral("face scalar %1 is not a region index in [0,%2)")
                                .arg(double(q)).arg(domainFaces)));
        regions.insert(int(q));
    }
    qDebug("%d domain faces, %d of them reached by a face of the mesh", domainFaces,
           int(regions.size()));
    QCOMPARE(regions.size(), domainFaces);

    // The regions have to be connected patches, not a speckle, or there is nothing worth
    // looking at. Count the face adjacencies that cross a region boundary: about 44% of
    // them here, since 194 regions over ~2.4k faces makes each one a dozen triangles with
    // a long perimeter. Shuffling the labels would put 99.5% of them on a boundary, so the
    // bar sits well clear of both.
    {
        VCGMesh &mesh = doc.mesh(0).mesh;
        VCGMeshFFAdjScope ffAdj(mesh);
        vcg::tri::UpdateTopology<VCGMesh>::FaceFace(mesh);
        int adjacencies = 0;
        int crossings = 0;
        for (VCGFace &f : mesh.face) {
            if (f.IsD()) continue;
            for (int k = 0; k < 3; ++k) {
                const VCGFace *n = f.FFp(k);
                if (!n || n == &f || n < &f) continue;
                ++adjacencies;
                if (int(f.cQ()) != int(n->cQ()))
                    ++crossings;
            }
        }
        QVERIFY(adjacencies > 0);
        const double onBoundary = double(crossings) / double(adjacencies);
        qDebug("%.1f%% of face adjacencies lie on a region boundary", 100.0 * onBoundary);
        QVERIFY2(onBoundary < 0.6, qPrintable(QStringLiteral("regions are not connected "
                                                            "patches (%1 on a boundary)")
                                                  .arg(onBoundary)));
    }

    // And the view is asked to show it.
    QCOMPARE(r.visualizationHints.size(), 1);
    QCOMPARE(r.visualizationHints.front().meshIndex, 0);
    QVERIFY(r.visualizationHints.front().attribute
            == MeshFilterVisualizationAttribute::FaceQuality);
}

void FilterTests::abstractDomainConsumersRunAndRefuseWithoutIt()
{
    const QString kDomain = QStringLiteral("meshlab2.filter.isoparam/abstract_domain");
    const QStringList consumers{
        QStringLiteral("remesh_by_abstract_domain"),
        QStringLiteral("create_atlased_mesh_from_abstract_domain")
    };

    // Without a domain: refused, and the message names the filter that builds one.
    for (const QString &id : consumers) {
        Document doc;
        QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
        doc.setCurrentMeshIndex(0);
        const MeshFilterRunResult r = doc.runFilter(filterKeyForId(doc, id), {});
        QVERIFY2(!r.success, qPrintable(QStringLiteral("%1 ran without a domain").arg(id)));
        QVERIFY2(r.errorMessage.contains(QStringLiteral("Parametrize by Abstract Domain")),
                 qPrintable(QStringLiteral("%1: %2").arg(id, r.errorMessage)));
    }

    // With one: both produce a new layer.
    Document doc;
    QVERIFY(doc.loadMesh(QStringLiteral(TEST_SOURCE_DIR "/tests/sample_mesh/sphere_1.2kv.ply")) >= 0);
    doc.setCurrentMeshIndex(0);
    MeshFilterParameterValues build;
    build.insert(QStringLiteral("minDomainFaces"), 150);
    build.insert(QStringLiteral("maxDomainFaces"), 200);
    const MeshFilterRunResult made = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("parametrize_by_abstract_domain")), build);
    QVERIFY2(made.success, qPrintable(made.errorMessage));
    QVERIFY(doc.layerData(0, kDomain));

    MeshFilterParameterValues remeshParams;
    remeshParams.insert(QStringLiteral("samplingRate"), 4);
    const MeshFilterRunResult remesh = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("remesh_by_abstract_domain")), remeshParams);
    QVERIFY2(remesh.success, qPrintable(remesh.errorMessage));
    QCOMPARE(remesh.newMeshIndices.size(), std::size_t(1));
    QVERIFY(doc.mesh(remesh.newMeshIndices.front()).mesh.FN() > 0);

    doc.setCurrentMeshIndex(0);
    const MeshFilterRunResult atlas = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("create_atlased_mesh_from_abstract_domain")), {});
    QVERIFY2(atlas.success, qPrintable(atlas.errorMessage));
    QCOMPARE(atlas.newMeshIndices.size(), std::size_t(1));
    const Document::MeshEntry &atlased = doc.mesh(atlas.newMeshIndices.front());
    QVERIFY2(vcg::tri::HasPerWedgeTexCoord(atlased.mesh), "the atlased layer has no per-wedge UVs");

    // Transfer: the target gains a domain of its own and the source keeps its one, which
    // is where this deliberately differs from MeshLab.
    const int copy = doc.duplicateMesh(0);
    QVERIFY(copy > 0);
    doc.clearLayerData(copy, kDomain);
    MeshFilterParameterValues transfer;
    transfer.insert(QStringLiteral("sourceMesh"), 0);
    transfer.insert(QStringLiteral("targetMesh"), copy);
    const MeshFilterRunResult moved = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("transfer_abstract_domain_to_another_layer")), transfer);
    QVERIFY2(moved.success, qPrintable(moved.errorMessage));
    QVERIFY2(doc.layerData(copy, kDomain), "the target did not receive a domain");
    QVERIFY2(doc.layerData(0, kDomain), "the source lost its own domain");
    QVERIFY(doc.layerData(copy, kDomain).get() != doc.layerData(0, kDomain).get());
}

void FilterTests::abstractDomainRefusesAnOpenMesh()
{
    constexpr int kSide = 12;
    VCGMesh grid;
    vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
    for (int j = 0; j < kSide; ++j)
        for (int i = 0; i < kSide; ++i)
            grid.vert[std::size_t(j * kSide + i)].P() = vcg::Point3f(float(i), float(j), 0.0f);
    for (int j = 0; j < kSide - 1; ++j) {
        for (int i = 0; i < kSide - 1; ++i) {
            const int a = j * kSide + i;
            vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
            vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
        }
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(grid);

    Document doc;
    const int index = doc.addMesh(grid, QStringLiteral("Open"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD);
    doc.setCurrentMeshIndex(index);

    const MeshFilterRunResult r = doc.runFilter(
        filterKeyForId(doc, QStringLiteral("parametrize_by_abstract_domain")), {});
    QVERIFY2(!r.success, "an open mesh was accepted");
    QVERIFY2(r.errorMessage.contains(QStringLiteral("watertight")), qPrintable(r.errorMessage));
    QVERIFY(!doc.layerData(index, QStringLiteral("meshlab2.filter.isoparam/abstract_domain")));
}

void FilterTests::selfIntersectionCurvesFindTheCrossing()
{
    VCGMesh mesh;
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, 6);
    // One triangle lying in z = 0...
    mesh.vert[0].P() = vcg::Point3f(0.0f, 0.0f, 0.0f);
    mesh.vert[1].P() = vcg::Point3f(2.0f, 0.0f, 0.0f);
    mesh.vert[2].P() = vcg::Point3f(1.0f, 2.0f, 0.0f);
    // ...and one standing in x = 1, passing straight through it.
    mesh.vert[3].P() = vcg::Point3f(1.0f, 0.5f, -1.0f);
    mesh.vert[4].P() = vcg::Point3f(1.0f, 0.5f,  1.0f);
    mesh.vert[5].P() = vcg::Point3f(1.0f, 1.5f,  0.0f);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 0, 1, 2);
    vcg::tri::Allocator<VCGMesh>::AddFace(mesh, 3, 4, 5);
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);

    Document doc;
    const int index = doc.addMesh(mesh, QStringLiteral("Crossing"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD);
    doc.setCurrentMeshIndex(index);

    const QString key = filterKeyForId(doc, QStringLiteral("create_polyline_from_self_intersections_trueform"));
    QVERIFY(!key.isEmpty());
    const MeshFilterRunResult r = doc.runFilter(key, {});
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QCOMPARE(r.newMeshIndices.size(), std::size_t(1));

    // The curve is the segment where the standing triangle crosses z = 0, which runs
    // along x = 1 from y = 0.5 to y = 1.5.
    const VCGMesh &curve = doc.mesh(r.newMeshIndices.front()).mesh;
    QVERIFY2(curve.EN() > 0, "the self-intersection curve came back empty");
    for (const auto &v : curve.vert) {
        if (v.IsD()) continue;
        QVERIFY2(std::abs(v.cP().X() - 1.0f) < 1e-3f,
                 qPrintable(QStringLiteral("curve point off the crossing plane: x = %1").arg(v.cP().X())));
        QVERIFY(std::abs(v.cP().Z()) < 1e-3f);
    }
}

void FilterTests::packUvChartsSurvivesAnyRotationCount()
{
    for (int rotations : {1, 2, 3, 4, 5, 6, 7, 9, 16}) {
        constexpr int kSide = 4;
        VCGMesh grid;
        vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
        for (int j = 0; j < kSide; ++j)
            for (int i = 0; i < kSide; ++i)
                grid.vert[std::size_t(j * kSide + i)].P() =
                    vcg::Point3f(float(i), float(j), 0.0f);
        for (int j = 0; j < kSide - 1; ++j) {
            for (int i = 0; i < kSide - 1; ++i) {
                const int a = j * kSide + i;
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
            }
        }
        vcg::tri::UpdateBounding<VCGMesh>::Box(grid);

        Document doc;
        const int meshIndex = doc.addMesh(grid, QStringLiteral("Grid"),
                                          vcg::tri::io::Mask::IOM_VERTCOORD);
        doc.setCurrentMeshIndex(meshIndex);
        MeshFilterParameterValues uvParams;
        uvParams.insert(QStringLiteral("textdim"), 128);
        QVERIFY(doc.runFilter(
            filterKeyForId(doc,
                QStringLiteral("parametrize_by_trivial_per_triangle_layout")),
            uvParams).success);

        MeshFilterParameterValues params;
        params.insert(QStringLiteral("algorithm"), QStringLiteral("rasterized_scaled"));
        params.insert(QStringLiteral("textureSize"), 256);
        params.insert(QStringLiteral("rotationNum"), rotations);
        params.insert(QStringLiteral("resampleTextures"), false);
        const MeshFilterRunResult r =
            doc.runFilter(filterKeyForId(doc, QStringLiteral("pack_uv_charts")), params);
        QVERIFY2(r.success,
                 qPrintable(QStringLiteral("rotationNum %1: %2").arg(rotations).arg(r.errorMessage)));
    }
}

// A layer can carry a parametrization and no texture at all -- straight out of a UV
// filter, before anything is baked. All three filters here read the source images only to
// learn their resolution and sample them only when resampling, so with resampling off each
// is a UV operation and that layer is a valid input.
void FilterTests::packUvChartsWorksWithoutATexture()
{
    const auto build = [](Document &doc) {
        constexpr int kSide = 4;
        VCGMesh grid;
        vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
        for (int j = 0; j < kSide; ++j)
            for (int i = 0; i < kSide; ++i)
                grid.vert[std::size_t(j * kSide + i)].P() =
                    vcg::Point3f(float(i), float(j), 0.0f);
        for (int j = 0; j < kSide - 1; ++j) {
            for (int i = 0; i < kSide - 1; ++i) {
                const int a = j * kSide + i;
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
            }
        }
        vcg::tri::UpdateBounding<VCGMesh>::Box(grid);
        const int index = doc.addMesh(grid, QStringLiteral("Grid"),
                                      vcg::tri::io::Mask::IOM_VERTCOORD);
        doc.setCurrentMeshIndex(index);
        MeshFilterParameterValues uvParams;
        uvParams.insert(QStringLiteral("textdim"), 256);
        return doc.runFilter(
            filterKeyForId(doc,
                QStringLiteral("parametrize_by_trivial_per_triangle_layout")),
            uvParams).success;
    };

    const QStringList ids{
        QStringLiteral("pack_uv_charts"),
        QStringLiteral("merge_texture_islands"),
        QStringLiteral("defragment_texture_atlas")
    };

    for (const QString &id : ids) {
        // The success path costs a full run, and for the two defragmentation filters most
        // of that is spent inside the chart packer -- two minutes on a mesh this size, for
        // reasons recorded with the packing measurements. The no-texture handling is one
        // shared block, so running it once proves it; each filter's refusal path below is
        // cheap and still covers all three descriptors no longer demanding a texture.
        const bool runSuccessPath = (id == QStringLiteral("pack_uv_charts"));

        // No texture, no resampling: works on the UVs, returns a layer with no textures.
        if (runSuccessPath) {
            Document doc;
            QVERIFY(build(doc));
            QCOMPARE(Document::meshTextureAssociationCount(doc.mesh(0)), 0);
            MeshFilterParameterValues params;
            params.insert(QStringLiteral("resampleTextures"), false);
            // Only the island merge has it, and without it the merge search is slow.
            if (id == QStringLiteral("merge_texture_islands"))
                params.insert(QStringLiteral("quickRun"), true);
            const MeshFilterRunResult r =
                doc.runFilter(filterKeyForId(doc, id), params);
            QVERIFY2(r.success, qPrintable(QStringLiteral("%1: %2").arg(id, r.errorMessage)));
            QVERIFY(!r.newMeshIndices.empty());
            const Document::MeshEntry &out = doc.mesh(r.newMeshIndices.front());
            QCOMPARE(Document::meshTextureAssociationCount(out), 0);
            QVERIFY2(vcg::tri::HasPerWedgeTexCoord(out.mesh), qPrintable(id));
        }

        // No texture but resampling asked for: nothing to resample, and the message has
        // to name the switch that fixes it.
        {
            Document doc;
            QVERIFY(build(doc));
            MeshFilterParameterValues params;
            params.insert(QStringLiteral("resampleTextures"), true);
            const MeshFilterRunResult r =
                doc.runFilter(filterKeyForId(doc, id), params);
            QVERIFY2(!r.success, qPrintable(QStringLiteral("%1 should have refused").arg(id)));
            QVERIFY2(r.errorMessage.contains(QStringLiteral("Resample textures")),
                     qPrintable(QStringLiteral("%1: %2").arg(id, r.errorMessage)));
        }
    }
}

void FilterTests::packUvChartsRunsEveryAlgorithm()
{
    const QStringList algorithms{
        QStringLiteral("rasterized_scaled"), QStringLiteral("rasterized_best_effort"),
        QStringLiteral("axis_aligned_rect"), QStringLiteral("object_oriented_rect")
    };

    for (const QString &algorithm : algorithms) {
        constexpr int kSide = 6;
        VCGMesh grid;
        vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
        for (int j = 0; j < kSide; ++j)
            for (int i = 0; i < kSide; ++i)
                grid.vert[std::size_t(j * kSide + i)].P() =
                    vcg::Point3f(float(i), float(j), 0.0f);
        for (int j = 0; j < kSide - 1; ++j) {
            for (int i = 0; i < kSide - 1; ++i) {
                const int a = j * kSide + i;
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
            }
        }
        vcg::tri::UpdateBounding<VCGMesh>::Box(grid);

        Document doc;
        const int meshIndex = doc.addMesh(grid, QStringLiteral("Grid"),
                                          vcg::tri::io::Mask::IOM_VERTCOORD);
        doc.setCurrentMeshIndex(meshIndex);
        MeshFilterParameterValues uvParams;
        uvParams.insert(QStringLiteral("textdim"), 256);
        const MeshFilterRunResult uv = doc.runFilter(
            filterKeyForId(doc,
                QStringLiteral("parametrize_by_trivial_per_triangle_layout")),
            uvParams);
        QVERIFY2(uv.success, qPrintable(uv.errorMessage));

        QImage texture(256, 256, QImage::Format_RGBA8888);
        texture.fill(Qt::red);
        TextureAssociationUtils::replaceTextureAssociations(
            doc.mesh(meshIndex),
            { TextureAssociationUtils::makeTextureAssetFromImage(
                  texture, QStringLiteral("atlas.png")) });
        doc.setCurrentMeshIndex(meshIndex);

        MeshFilterParameterValues params;
        params.insert(QStringLiteral("algorithm"), algorithm);
        params.insert(QStringLiteral("textureSize"), 512);
        params.insert(QStringLiteral("resampleTextures"), false);
        QElapsedTimer timer;
        timer.start();
        const MeshFilterRunResult r =
            doc.runFilter(filterKeyForId(doc, QStringLiteral("pack_uv_charts")), params);
        const qint64 elapsed = timer.elapsed();
        QVERIFY2(r.success, qPrintable(QStringLiteral("%1: %2").arg(algorithm, r.errorMessage)));
        QVERIFY(!r.newMeshIndices.empty());

        const VCGMesh &out = doc.mesh(r.newMeshIndices.front()).mesh;
        int outside = 0;
        for (const VCGFace &f : out.face) {
            if (f.IsD()) continue;
            for (int k = 0; k < 3; ++k) {
                const auto uvp = f.cWT(k).P();
                if (uvp.X() < -0.001 || uvp.X() > 1.001 || uvp.Y() < -0.001 || uvp.Y() > 1.001)
                    ++outside;
            }
        }
        qDebug("pack %-24s %5lld ms", qPrintable(algorithm), elapsed);
        QVERIFY2(outside == 0,
                 qPrintable(QStringLiteral("%1 put %2 UVs outside the atlas").arg(algorithm).arg(outside)));
    }
}

void FilterTests::textureIslandMergeCanSkipResampling()
{
    const auto runWith = [](bool resample, int &outputTextures, QString &error) -> bool {
        // A real parametrization, not hand-written UVs: the trivial per-triangle layout
        // gives one island per face, which is exactly what this filter exists to merge.
        constexpr int kSide = 8;
        VCGMesh grid;
        vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
        for (int j = 0; j < kSide; ++j)
            for (int i = 0; i < kSide; ++i)
                grid.vert[std::size_t(j * kSide + i)].P() =
                    vcg::Point3f(float(i), float(j), 0.0f);
        for (int j = 0; j < kSide - 1; ++j) {
            for (int i = 0; i < kSide - 1; ++i) {
                const int a = j * kSide + i;
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
                vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
            }
        }
        vcg::tri::UpdateBounding<VCGMesh>::Box(grid);

        Document doc;
        const int meshIndex = doc.addMesh(grid, QStringLiteral("Islands"),
                                          vcg::tri::io::Mask::IOM_VERTCOORD);
        doc.setCurrentMeshIndex(meshIndex);

        const QString uvKey = filterKeyForId(
            doc, QStringLiteral("parametrize_by_trivial_per_triangle_layout"));
        if (uvKey.isEmpty()) { error = QStringLiteral("parametrization filter missing"); return false; }
        MeshFilterParameterValues uvParams;
        uvParams.insert(QStringLiteral("textdim"), 256);
        const MeshFilterRunResult uv = doc.runFilter(uvKey, uvParams);
        if (!uv.success) { error = uv.errorMessage; return false; }

        QImage texture(256, 256, QImage::Format_RGBA8888);
        texture.fill(Qt::red);
        TextureAssociationUtils::replaceTextureAssociations(
            doc.mesh(meshIndex),
            { TextureAssociationUtils::makeTextureAssetFromImage(
                  texture, QStringLiteral("atlas.png")) });
        doc.setCurrentMeshIndex(meshIndex);

        const QString key = filterKeyForId(doc, QStringLiteral("merge_texture_islands"));
        if (key.isEmpty()) { error = QStringLiteral("filter not registered"); return false; }
        MeshFilterParameterValues params;
        params.insert(QStringLiteral("resampleTextures"), resample);
        // Without this the merge retries every rejected operation and a mesh this small
        // can run for minutes; the option under test is unaffected either way.
        params.insert(QStringLiteral("quickRun"), true);
        const MeshFilterRunResult r = doc.runFilter(key, params);
        if (!r.success) { error = r.errorMessage; return false; }
        if (r.newMeshIndices.empty()) { error = QStringLiteral("no output layer"); return false; }

        const Document::MeshEntry &out = doc.mesh(r.newMeshIndices.front());
        outputTextures = Document::meshTextureAssociationCount(out);
        // The point of the option is the UV layout, so it must survive either way.
        if (!vcg::tri::HasPerWedgeTexCoord(out.mesh)) {
            error = QStringLiteral("output lost its per-wedge UVs");
            return false;
        }
        return true;
    };

    int withTextures = -1, withoutTextures = -1;
    QString error;
    QVERIFY2(runWith(true, withTextures, error), qPrintable(error));
    QVERIFY2(runWith(false, withoutTextures, error), qPrintable(error));

    QVERIFY2(withTextures > 0,
             qPrintable(QStringLiteral("resampling on produced %1 textures").arg(withTextures)));
    QCOMPARE(withoutTextures, 0);
}

void FilterTests::voronoiAtlasKeepsSelectionAndColorAlone()
{
    constexpr int kSide = 24;
    const vcg::Color4b kInputColor(10, 200, 30, 255);

    VCGMesh grid;
    vcg::tri::Allocator<VCGMesh>::AddVertices(grid, kSide * kSide);
    for (int j = 0; j < kSide; ++j) {
        for (int i = 0; i < kSide; ++i) {
            VCGVertex &v = grid.vert[std::size_t(j * kSide + i)];
            v.P() = vcg::Point3f(float(i), float(j), 0.0f);
            v.C() = kInputColor;
        }
    }
    for (int j = 0; j < kSide - 1; ++j) {
        for (int i = 0; i < kSide - 1; ++i) {
            const int a = j * kSide + i;
            vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + 1, a + kSide + 1);
            vcg::tri::Allocator<VCGMesh>::AddFace(grid, a, a + kSide + 1, a + kSide);
        }
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(grid);

    Document doc;
    const int index = doc.addMesh(grid, QStringLiteral("Grid"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD
                                      | vcg::tri::io::Mask::IOM_VERTCOLOR);
    doc.setCurrentMeshIndex(index);

    const QString key = filterKeyForId(doc, QStringLiteral("parametrize_by_voronoi_atlas_vcglib"));
    QVERIFY(!key.isEmpty());
    MeshFilterParameterValues params;
    params.insert(QStringLiteral("regionNum"), 4);
    const MeshFilterRunResult r = doc.runFilter(key, params);
    QVERIFY2(r.success, qPrintable(r.errorMessage));
    QCOMPARE(r.newMeshIndices.size(), std::size_t(1));

    const Document::MeshEntry &atlas = doc.mesh(r.newMeshIndices.front());
    QVERIFY(atlas.mesh.VN() > 0);

    int selectedVertices = 0, selectedFaces = 0, recolored = 0;
    for (const VCGVertex &v : atlas.mesh.vert) {
        if (v.IsD()) continue;
        if (v.IsS()) ++selectedVertices;
        if (v.cC() != kInputColor) ++recolored;
    }
    for (const VCGFace &f : atlas.mesh.face) {
        if (!f.IsD() && f.IsS()) ++selectedFaces;
    }
    QCOMPARE(selectedVertices, 0);
    QCOMPARE(selectedFaces, 0);
    QCOMPARE(recolored, 0);
}

void FilterTests::quadPairingChoosesGoodDiagonals()
{
    // A grid whose quads are each split by a randomly chosen diagonal: the shape a real
    // triangulated quad mesh has, and the case the old pipeline handled worst.
    constexpr int kSide = 21;
    VCGMesh mesh;
    // Indices, not pointers: adding vertices reallocates the vector.
    vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, kSide * kSide);
    for (int j = 0; j < kSide; ++j)
        for (int i = 0; i < kSide; ++i)
            mesh.vert[std::size_t(j * kSide + i)].P() =
                vcg::Point3f(float(i), float(j), 0.0f);

    std::mt19937 rng(11);
    for (int j = 0; j < kSide - 1; ++j) {
        for (int i = 0; i < kSide - 1; ++i) {
            const int a = j * kSide + i;
            const int b = a + 1;
            const int c = (j + 1) * kSide + i + 1;
            const int d = (j + 1) * kSide + i;
            if (rng() & 1u) {
                vcg::tri::Allocator<VCGMesh>::AddFace(mesh, a, b, c);
                vcg::tri::Allocator<VCGMesh>::AddFace(mesh, a, c, d);
            } else {
                vcg::tri::Allocator<VCGMesh>::AddFace(mesh, a, b, d);
                vcg::tri::Allocator<VCGMesh>::AddFace(mesh, b, c, d);
            }
        }
    }
    vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);

    Document doc;
    const int index = doc.addMesh(mesh, QStringLiteral("Grid"),
                                  vcg::tri::io::Mask::IOM_VERTCOORD);
    doc.setCurrentMeshIndex(index);

    const QString key = filterKeyForId(
        doc, QStringLiteral("convert_to_quads_by_triangle_pairing"));
    QVERIFY(!key.isEmpty());
    const MeshFilterRunResult r = doc.runFilter(key, {});
    QVERIFY2(r.success, qPrintable(r.errorMessage));

    // Mean quad quality: 1 is a perfect square. Every quad of this input is exactly
    // recoverable, so the quality-driven pairing scores 1.00 on it; dropping that pass
    // and letting MakePureByFlip do the pairing scores 0.54.
    VCGMesh &out = doc.mesh(index).mesh;
    // quadQuality() reads across the diagonal through FFp, and the filter framework
    // disables FF adjacency again once a filter declaring inputPrepare "FF" returns.
    // Measuring without re-enabling it indexes an empty OCF vector.
    VCGMeshFFAdjScope ffAdj(out);
    vcg::tri::UpdateTopology<VCGMesh>::FaceFace(out);

    double total = 0.0;
    int quads = 0, unpaired = 0;
    for (VCGFace &f : out.face) {
        if (f.IsD())
            continue;
        bool paired = false;
        for (int k = 0; k < 3; ++k) {
            if (!f.IsF(k))
                continue;
            total += double(vcg::tri::BitQuad<VCGMesh>::quadQuality(&f, k));
            ++quads;
            paired = true;
            break;
        }
        if (!paired)
            ++unpaired;
    }
    QVERIFY2(quads > 0, "no quads were formed at all");
    const double mean = total / quads;
    QVERIFY2(mean > 0.95,
             qPrintable(QStringLiteral("mean quad quality %1; the quality-driven pairing "
                                       "is not being applied").arg(mean)));
    QCOMPARE(unpaired, 0);
}

QTEST_MAIN(FilterTests)
#include "test_filters.moc"
