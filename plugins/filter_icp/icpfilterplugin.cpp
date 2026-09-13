#include "icpfilterplugin.h"

#include "document.h"
#include "meshfilterpluginmanager.h"
#include "vcgmesh.h"

#include <wrap/io_trimesh/io_mask.h>
#include <vcg/complex/algorithms/align_pair.h>
#include <vcg/complex/algorithms/align_global.h>
#include <vcg/complex/algorithms/occupancy_grid.h>
#include <vcg/complex/algorithms/update/bounding.h>

#include <QMatrix4x4>
#include <QObject>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <vector>

std::vector<vcg::Point3d> *vcg::PointMatchingScale::fix = nullptr;
std::vector<vcg::Point3d> *vcg::PointMatchingScale::mov = nullptr;
vcg::Box3d vcg::PointMatchingScale::b;

namespace {

constexpr QLatin1StringView kTwoMeshIcp("align_by_icp_vcglib");
constexpr QLatin1StringView kGlobalIcp("align_meshes_globally");
constexpr QLatin1StringView kOverlappingMeshes("measure_layer_overlap");

using Mask = vcg::tri::io::Mask;

MeshFilterRunResult fail(const QString &message)
{
    MeshFilterRunResult result;
    result.success = false;
    result.documentModified = false;
    result.errorMessage = message;
    return result;
}

MeshFilterRunResult success(
    bool modified,
    const QStringList &info = {},
    const QVector<int> &newMeshIndices = {})
{
    MeshFilterRunResult result;
    result.success = true;
    result.documentModified = modified;
    result.infoMessages = info;
    result.newMeshIndices = newMeshIndices;
    return result;
}

vcg::Matrix44d identityMatrix()
{
    vcg::Matrix44d m;
    m.SetIdentity();
    return m;
}

vcg::Matrix44d qtToVcg(const QMatrix4x4 &m)
{
    vcg::Matrix44d r;
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            r[row][col] = double(m(row, col));
    return r;
}

QMatrix4x4 vcgToQt(const vcg::Matrix44d &m)
{
    return QMatrix4x4(
        float(m[0][0]), float(m[0][1]), float(m[0][2]), float(m[0][3]),
        float(m[1][0]), float(m[1][1]), float(m[1][2]), float(m[1][3]),
        float(m[2][0]), float(m[2][1]), float(m[2][2]), float(m[2][3]),
        float(m[3][0]), float(m[3][1]), float(m[3][2]), float(m[3][3]));
}

bool nearlyEqual(const QMatrix4x4 &a, const QMatrix4x4 &b, float eps = 1e-5f)
{
    for (int row = 0; row < 4; ++row)
        for (int col = 0; col < 4; ++col)
            if (std::abs(a(row, col) - b(row, col)) > eps)
                return false;
    return true;
}

vcg::Point3f pointToFloat(const vcg::Point3d &p)
{
    return vcg::Point3f(float(p.X()), float(p.Y()), float(p.Z()));
}

vcg::Box3d localVertexBBox(const VCGMesh &mesh)
{
    vcg::Box3d bbox;
    bbox.SetNull();
    for (const VCGVertex &vertex : mesh.vert) {
        if (!vertex.IsD())
            bbox.Add(vcg::Point3d::Construct(vertex.cP()));
    }
    return bbox;
}

vcg::Box3d transformedBBox(const VCGMesh &mesh, const vcg::Matrix44d &transform)
{
    const vcg::Box3d local = localVertexBBox(mesh);
    vcg::Box3d result;
    result.SetNull();
    if (local.IsNull())
        return result;

    for (int corner = 0; corner < 8; ++corner) {
        const vcg::Point3d p(
            (corner & 1) ? local.max.X() : local.min.X(),
            (corner & 2) ? local.max.Y() : local.min.Y(),
            (corner & 4) ? local.max.Z() : local.min.Z());
        result.Add(transform * p);
    }
    return result;
}

vcg::AlignPair::Param alignParamsFromFilter(const FilterParams &params)
{
    vcg::AlignPair::Param app;
    app.SampleNum = params.getInt(QStringLiteral("SampleNum"));
    app.MinDistAbs = params.getDouble(QStringLiteral("MinDistAbs"));
    app.TrgDistAbs = params.getDouble(QStringLiteral("TrgDistAbs"));
    app.MaxIterNum = params.getInt(QStringLiteral("MaxIterNum"));
    app.SampleMode = params.getBool(QStringLiteral("SampleMode"))
        ? vcg::AlignPair::Param::SMNormalEqualized
        : vcg::AlignPair::Param::SMRandom;
    app.ReduceFactorPerc = params.getDouble(QStringLiteral("ReduceFactorPerc"));
    app.PassHiFilter = params.getDouble(QStringLiteral("PassHiFilter"));
    app.MatchMode = params.getBool(QStringLiteral("MatchMode"))
        ? vcg::AlignPair::Param::MMRigid
        : vcg::AlignPair::Param::MMSimilarity;
    app.UseVertexOnly = params.getBool(QStringLiteral("UseVertexOnly"), false);
    app.MaxAngleRad = vcg::math::ToRad(params.getDouble(QStringLiteral("MaxAngleDeg"), 45.0));
    app.MinPointNum = params.getInt(QStringLiteral("MinPointNum"), 30);
    return app;
}

QStringList resultInfo(const vcg::AlignPair::Result &result, const QString &prefix = {})
{
    QStringList info;
    const QString label = prefix.trimmed();
    if (result.as.I.empty()) {
        info << (label.isEmpty()
                ? QObject::tr("ICP completed with no iteration statistics.")
                : QObject::tr("%1: ICP completed with no iteration statistics.").arg(label));
        return info;
    }

    const vcg::AlignPair::Stat::IterInfo &last = result.as.I.back();
    const QString message = QObject::tr(
        "ICP completed in %1 iteration(s); final median error %2; used %3/%4 samples.")
        .arg(result.as.I.size())
        .arg(last.pcl50, 0, 'g', 6)
        .arg(last.SampleUsed)
        .arg(last.SampleTested);
    info << (label.isEmpty() ? message : QObject::tr("%1: %2").arg(label, message));
    return info;
}

void prepareAlignMesh(const VCGMesh &source, vcg::AlignPair::A2Mesh &target)
{
    vcg::AlignPair aligner;
    aligner.convertMesh<VCGMesh>(const_cast<VCGMesh &>(source), target);
    if (target.fn > 0)
        target.init(identityMatrix());
    else
        target.initVert(identityMatrix());
}

bool runIcpArc(
    const VCGMesh &fixedMesh,
    const VCGMesh &movingMesh,
    const vcg::Matrix44d &movingToFixedInitial,
    const vcg::AlignPair::Param &alignParams,
    unsigned int randomSeed,
    vcg::AlignPair::Result &alignerResult,
    QString &errorMessage)
{
    vcg::AlignPair aligner;
    // AlignPair seeds itself from the wall clock, which makes the sub-sampling of
    // the moving mesh — and hence the resulting matrix — differ on every run.
    // Overwrite it so the caller decides.
    aligner.myrnd.initialize(randomSeed);
    vcg::AlignPair::A2Mesh fixed;
    vcg::AlignPair::A2Mesh moving;
    vcg::AlignPair::A2Grid faceGrid;
    vcg::AlignPair::A2GridVert vertexGrid;

    prepareAlignMesh(fixedMesh, fixed);
    if (fixed.vn <= 0) {
        errorMessage = QObject::tr("Reference mesh has no usable vertices.");
        return false;
    }

    vcg::AlignPair::Param arcParams = alignParams;
    if (fixed.fn == 0 || arcParams.UseVertexOnly)
        vcg::AlignPair::InitFixVert(&fixed, arcParams, vertexGrid);
    else
        vcg::AlignPair::initFix(&fixed, arcParams, faceGrid);

    prepareAlignMesh(movingMesh, moving);
    if (moving.vn <= 0) {
        errorMessage = QObject::tr("Source mesh has no usable vertices.");
        return false;
    }

    std::vector<vcg::AlignPair::A2Vertex> movingSamples = moving.vert;
    if (!aligner.sampleMovVert(movingSamples, arcParams.SampleNum, arcParams.SampleMode)) {
        errorMessage = QObject::tr("Could not sample the source mesh for ICP.");
        return false;
    }
    if (movingSamples.empty()) {
        errorMessage = QObject::tr("The source mesh produced no ICP samples.");
        return false;
    }

    aligner.mov = &movingSamples;
    aligner.fix = &fixed;
    aligner.ap = arcParams;

    const bool ok = aligner.align(movingToFixedInitial, faceGrid, vertexGrid, alignerResult);
    if (!ok) {
        errorMessage = QString::fromLatin1(vcg::AlignPair::errorMsg(alignerResult.status));
        return false;
    }
    return true;
}

int addIterationPointLayer(
    Document &doc,
    const QString &name,
    const std::vector<vcg::Point3d> &points,
    const std::vector<vcg::Point3d> &normals,
    const vcg::Color4b &color,
    const QMatrix4x4 &transform)
{
    VCGMesh mesh;
    auto vi = vcg::tri::Allocator<VCGMesh>::AddVertices(mesh, points.size());
    for (size_t i = 0; i < points.size(); ++i, ++vi) {
        vi->P() = pointToFloat(points[i]);
        if (i < normals.size()) {
            vi->N() = pointToFloat(normals[i]);
            if (vi->N().SquaredNorm() > 1e-20f)
                vi->N().Normalize();
        }
        vi->C() = color;
    }
    if (mesh.VN() > 0)
        vcg::tri::UpdateBounding<VCGMesh>::Box(mesh);

    const int ioMask =
        Mask::IOM_VERTCOORD
        | Mask::IOM_VERTNORMAL
        | Mask::IOM_VERTCOLOR;
    const int index = doc.addMesh(mesh, name, ioMask);
    if (index >= 0)
        doc.setMeshTransform(index, transform);
    return index;
}

QVector<int> saveLastIterationPoints(
    Document &doc,
    const vcg::AlignPair::Result &alignerResult,
    const QMatrix4x4 &referenceTransform,
    const QMatrix4x4 &sourceAlignedTransform)
{
    QVector<int> newMeshes;
    const int movingIndex = addIterationPointLayer(
        doc,
        QObject::tr("Chosen Source Points"),
        alignerResult.Pmov,
        alignerResult.Nmov,
        vcg::Color4b::Green,
        sourceAlignedTransform);
    if (movingIndex >= 0)
        newMeshes.push_back(movingIndex);

    const int fixedIndex = addIterationPointLayer(
        doc,
        QObject::tr("Corresponding Reference Points"),
        alignerResult.Pfix,
        alignerResult.Nfix,
        vcg::Color4b::Red,
        referenceTransform);
    if (fixedIndex >= 0)
        newMeshes.push_back(fixedIndex);

    return newMeshes;
}

MeshFilterRunResult runTwoMeshIcp(const FilterParams &params, Document &doc)
{
    const int referenceIndex = params.getMesh(QStringLiteral("ReferenceMesh"));
    const int sourceIndex = params.getMesh(QStringLiteral("SourceMesh"));
    if (referenceIndex < 0 || referenceIndex >= doc.meshCount())
        return fail(QObject::tr("Reference mesh index is invalid."));
    if (sourceIndex < 0 || sourceIndex >= doc.meshCount())
        return fail(QObject::tr("Source mesh index is invalid."));
    if (referenceIndex == sourceIndex)
        return fail(QObject::tr("Cannot apply ICP on the same mesh."));

    const Document::MeshEntry &referenceEntry = doc.mesh(referenceIndex);
    const Document::MeshEntry &sourceEntry = doc.mesh(sourceIndex);
    if (referenceEntry.mesh.VN() <= 0)
        return fail(QObject::tr("Reference mesh '%1' has no vertices.").arg(referenceEntry.name));
    if (sourceEntry.mesh.VN() <= 0)
        return fail(QObject::tr("Source mesh '%1' has no vertices.").arg(sourceEntry.name));

    bool referenceInvertible = false;
    const QMatrix4x4 sourceToReference =
        referenceEntry.transform.inverted(&referenceInvertible) * sourceEntry.transform;
    if (!referenceInvertible)
        return fail(QObject::tr("Reference mesh transform is not invertible."));

    doc.beginFilterProgress(QObject::tr("Align by ICP (vcglib)"));
    if (vcg::CallBackPos *cb = doc.progressCallback())
        (*cb)(5, "Preparing ICP alignment...");

    const RandomSeed seed = params.getRandomSeed();

    try {
        vcg::AlignPair::Result alignerResult;
        QString alignError;
        if (!runIcpArc(
                referenceEntry.mesh,
                sourceEntry.mesh,
                qtToVcg(sourceToReference),
                alignParamsFromFilter(params),
                seed.value,
                alignerResult,
                alignError)) {
            doc.finishFilterProgress(false, alignError);
            return fail(alignError);
        }

        alignerResult.FixName = referenceIndex;
        alignerResult.MovName = sourceIndex;

        const QMatrix4x4 alignedSourceTransform = referenceEntry.transform * vcgToQt(alignerResult.Tr);
        const bool transformChanged = !nearlyEqual(sourceEntry.transform, alignedSourceTransform);
        doc.setMeshTransform(
            sourceIndex,
            alignedSourceTransform,
            QObject::tr("ICP aligned '%1' to '%2'.")
                .arg(sourceEntry.name, referenceEntry.name));

        QVector<int> newMeshes;
        if (params.getBool(QStringLiteral("SaveLastIteration"))) {
            newMeshes = saveLastIterationPoints(
                doc,
                alignerResult,
                referenceEntry.transform,
                alignedSourceTransform);
        }

        QStringList info = resultInfo(alignerResult);
        info << QObject::tr("Updated transform of '%1' using '%2' as reference.")
                    .arg(sourceEntry.name, referenceEntry.name);
        if (!newMeshes.isEmpty())
            info << QObject::tr("Saved %1 diagnostic ICP point layer(s).").arg(newMeshes.size());
        info << seed.message();

        doc.finishFilterProgress(true, QObject::tr("ICP alignment completed."));
        return success(transformChanged || !newMeshes.isEmpty(), info, newMeshes);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("ICP failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    } catch (...) {
        const QString message = QObject::tr("ICP failed with an unknown error.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

struct GlobalAlignmentInput
{
    std::vector<int> docIndices;
    std::vector<vcg::Matrix44d> relativeTransforms;
    int baseDenseIndex = -1;
    QMatrix4x4 baseWorldTransform;
};

bool buildGlobalInput(
    const FilterParams &params,
    const Document &doc,
    GlobalAlignmentInput &input,
    QString &errorMessage)
{
    const int baseIndex = params.getMesh(QStringLiteral("BaseMesh"));
    if (baseIndex < 0 || baseIndex >= doc.meshCount()) {
        errorMessage = QObject::tr("Base mesh index is invalid.");
        return false;
    }

    const bool onlyVisible = params.getBool(QStringLiteral("OnlyVisibleMeshes"));
    if (onlyVisible && !doc.mesh(baseIndex).visible) {
        errorMessage = QObject::tr("The base mesh must be visible when aligning only visible meshes.");
        return false;
    }

    bool baseInvertible = false;
    const QMatrix4x4 baseWorld = doc.mesh(baseIndex).transform;
    const QMatrix4x4 baseInv = baseWorld.inverted(&baseInvertible);
    if (!baseInvertible) {
        errorMessage = QObject::tr("Base mesh transform is not invertible.");
        return false;
    }

    input = {};
    input.baseWorldTransform = baseWorld;
    for (int i = 0; i < doc.meshCount(); ++i) {
        const Document::MeshEntry &entry = doc.mesh(i);
        if (onlyVisible && !entry.visible)
            continue;
        if (entry.mesh.VN() <= 0)
            continue;

        if (i == baseIndex)
            input.baseDenseIndex = static_cast<int>(input.docIndices.size());
        input.docIndices.push_back(i);
        input.relativeTransforms.push_back(qtToVcg(baseInv * entry.transform));
    }

    if (input.baseDenseIndex < 0) {
        errorMessage = QObject::tr("The selected base mesh is not part of the alignment set.");
        return false;
    }
    if (input.docIndices.size() < 2) {
        errorMessage = QObject::tr("Global alignment requires at least two non-empty mesh layers.");
        return false;
    }
    if (input.docIndices.size() > OG_MAX_MCB_SIZE) {
        errorMessage = QObject::tr("Global alignment supports at most %1 layers.").arg(OG_MAX_MCB_SIZE);
        return false;
    }
    return true;
}

vcg::Box3d globalInputBBox(const GlobalAlignmentInput &input, const Document &doc)
{
    vcg::Box3d bbox;
    bbox.SetNull();
    for (size_t denseIndex = 0; denseIndex < input.docIndices.size(); ++denseIndex) {
        bbox.Add(transformedBBox(
            doc.mesh(input.docIndices[denseIndex]).mesh,
            input.relativeTransforms[denseIndex]));
    }
    return bbox;
}

MeshFilterRunResult runGlobalIcp(const FilterParams &params, Document &doc)
{
    GlobalAlignmentInput input;
    QString inputError;
    if (!buildGlobalInput(params, doc, input, inputError))
        return fail(inputError);

    const int ogSize = params.getInt(QStringLiteral("OGSize"));
    const double arcThreshold = params.getDouble(QStringLiteral("arcThreshold"));
    const vcg::AlignPair::Param alignParams = alignParamsFromFilter(params);
    const RandomSeed seed = params.getRandomSeed();

    doc.beginFilterProgress(QObject::tr("Align Meshes Globally"));
    vcg::CallBackPos *cb = doc.progressCallback();
    if (cb)
        (*cb)(5, "Computing overlap graph...");

    try {
        const vcg::Box3d bbox = globalInputBBox(input, doc);
        if (bbox.IsNull()) {
            const QString message = QObject::tr("Cannot build an occupancy grid for empty meshes.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        vcg::OccupancyGrid<VCGMesh, double> occupancyGrid;
        if (!occupancyGrid.Init(static_cast<int>(input.docIndices.size()), bbox, ogSize)) {
            const QString message = QObject::tr("Could not initialize the occupancy grid.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        for (size_t denseIndex = 0; denseIndex < input.docIndices.size(); ++denseIndex) {
            VCGMesh &mesh = doc.mesh(input.docIndices[denseIndex]).mesh;
            occupancyGrid.AddMesh(
                mesh,
                input.relativeTransforms[denseIndex],
                static_cast<int>(denseIndex));
        }
        occupancyGrid.Compute();

        std::vector<vcg::AlignPair::Result> resultList;
        const auto firstRejectedArc = std::find_if(
            occupancyGrid.SVA.begin(),
            occupancyGrid.SVA.end(),
            [arcThreshold](const auto &arc) { return arc.norm_area <= arcThreshold; });
        const size_t candidateArcCount =
            size_t(std::distance(occupancyGrid.SVA.begin(), firstRejectedArc));
        if (candidateArcCount == 0) {
            const QString message = QObject::tr("No overlapping mesh pairs passed the arc threshold.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        resultList.reserve(candidateArcCount);
        QStringList info;
        int validArcCount = 0;
        for (size_t arcIndex = 0; arcIndex < candidateArcCount; ++arcIndex) {
            const auto &arc = occupancyGrid.SVA[arcIndex];
            if (cb) {
                const int progress = 10 + int((70.0 * double(arcIndex)) / double(candidateArcCount));
                (*cb)(progress, "Computing pairwise ICP arcs...");
            }

            vcg::AlignPair::Result result;
            const vcg::Matrix44d movingToFixed =
                vcg::Inverse(input.relativeTransforms[size_t(arc.s)])
                * input.relativeTransforms[size_t(arc.t)];
            QString arcError;
            // Offset per arc so the arcs do not all sub-sample with the same
            // pattern, while the whole run still replays from one seed.
            const bool ok = runIcpArc(
                doc.mesh(input.docIndices[size_t(arc.s)]).mesh,
                doc.mesh(input.docIndices[size_t(arc.t)]).mesh,
                movingToFixed,
                alignParams,
                seed.value + unsigned(arcIndex),
                result,
                arcError);
            result.FixName = arc.s;
            result.MovName = arc.t;
            result.area = arc.norm_area;
            if (ok && result.isValid()) {
                ++validArcCount;
                info << QObject::tr("Arc %1 -> %2 accepted, overlap %3.")
                            .arg(doc.mesh(input.docIndices[size_t(arc.s)]).name,
                                 doc.mesh(input.docIndices[size_t(arc.t)]).name)
                            .arg(arc.norm_area, 0, 'f', 3);
            } else {
                info << QObject::tr("Arc %1 -> %2 rejected: %3.")
                            .arg(doc.mesh(input.docIndices[size_t(arc.s)]).name,
                                 doc.mesh(input.docIndices[size_t(arc.t)]).name,
                                 arcError);
            }
            resultList.push_back(std::move(result));
        }

        if (validArcCount == 0) {
            const QString message = QObject::tr("No pairwise ICP arc completed successfully.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        if (cb)
            (*cb)(82, "Solving global alignment...");

        std::vector<vcg::AlignPair::Result *> validResults;
        validResults.reserve(resultList.size());
        for (vcg::AlignPair::Result &result : resultList) {
            if (result.isValid())
                validResults.push_back(&result);
        }

        std::vector<int> ids(input.docIndices.size());
        for (size_t i = 0; i < ids.size(); ++i)
            ids[i] = static_cast<int>(i);

        std::map<int, std::string> names;
        for (size_t i = 0; i < input.docIndices.size(); ++i)
            names[int(i)] = doc.mesh(input.docIndices[i]).name.toStdString();

        vcg::AlignGlobal alignGlobal;
        alignGlobal.BuildGraph(validResults, input.relativeTransforms, ids);

        double globalError = 0.001;
        int guard = 0;
        while (!alignGlobal.GlobalAlign(
            names,
            globalError,
            100,
            alignParams.MatchMode == vcg::AlignPair::Param::MMRigid,
            stdout,
            cb)) {
            globalError *= 2.0;
            alignGlobal.BuildGraph(validResults, input.relativeTransforms, ids);
            if (++guard > 12) {
                const QString message = QObject::tr("Global alignment did not converge.");
                doc.finishFilterProgress(false, message);
                return fail(message);
            }
        }

        std::vector<vcg::Matrix44d> outputRelativeTransforms;
        alignGlobal.GetMatrixVector(outputRelativeTransforms, ids);
        if (outputRelativeTransforms.size() != input.docIndices.size()) {
            const QString message = QObject::tr("Global alignment produced an unexpected transform set.");
            doc.finishFilterProgress(false, message);
            return fail(message);
        }

        const vcg::Matrix44d baseCorrection =
            vcg::Inverse(outputRelativeTransforms[size_t(input.baseDenseIndex)]);
        const vcg::Matrix44d baseWorld = qtToVcg(input.baseWorldTransform);
        int touchedCount = 0;
        for (size_t denseIndex = 0; denseIndex < input.docIndices.size(); ++denseIndex) {
            const int docIndex = input.docIndices[denseIndex];
            const QMatrix4x4 nextTransform =
                vcgToQt(baseWorld * baseCorrection * outputRelativeTransforms[denseIndex]);
            if (!nearlyEqual(doc.mesh(docIndex).transform, nextTransform)) {
                ++touchedCount;
                doc.setMeshTransform(
                    docIndex,
                    nextTransform,
                    QObject::tr("Global ICP updated transform of '%1'.")
                        .arg(doc.mesh(docIndex).name));
            }
        }

        info.prepend(QObject::tr(
            "Global alignment used %1 valid ICP arc(s) over %2 candidate overlap arc(s).")
            .arg(validArcCount)
            .arg(candidateArcCount));
        info << QObject::tr("Affected layers: %1.").arg(touchedCount);
        info << seed.message();

        doc.finishFilterProgress(true, QObject::tr("Global alignment completed."));
        return success(touchedCount > 0, info);
    } catch (const std::exception &e) {
        const QString message = QObject::tr("Global ICP failed: %1").arg(QString::fromLocal8Bit(e.what()));
        doc.finishFilterProgress(false, message);
        return fail(message);
    } catch (...) {
        const QString message = QObject::tr("Global ICP failed with an unknown error.");
        doc.finishFilterProgress(false, message);
        return fail(message);
    }
}

MeshFilterRunResult runOverlappingMeshes(const FilterParams &params, Document &doc)
{
    if (doc.meshCount() <= 1)
        return success(false, { QObject::tr("At least two mesh layers are needed to report overlaps.") });
    if (doc.meshCount() > OG_MAX_MCB_SIZE)
        return fail(QObject::tr("Overlap detection supports at most %1 layers.").arg(OG_MAX_MCB_SIZE));

    vcg::Box3d bbox;
    bbox.SetNull();
    for (int i = 0; i < doc.meshCount(); ++i)
        bbox.Add(transformedBBox(doc.mesh(i).mesh, qtToVcg(doc.mesh(i).transform)));
    if (bbox.IsNull())
        return fail(QObject::tr("Cannot build an occupancy grid for empty meshes."));

    vcg::OccupancyGrid<VCGMesh, double> occupancyGrid;
    if (!occupancyGrid.Init(doc.meshCount(), bbox, params.getInt(QStringLiteral("OGSize"))))
        return fail(QObject::tr("Could not initialize the occupancy grid."));

    for (int i = 0; i < doc.meshCount(); ++i) {
        VCGMesh &mesh = doc.mesh(i).mesh;
        if (mesh.VN() > 0)
            occupancyGrid.AddMesh(mesh, qtToVcg(doc.mesh(i).transform), i);
    }
    occupancyGrid.Compute();

    QStringList info;
    for (const auto &arc : occupancyGrid.SVA) {
        info << QObject::tr("%1 overlaps %2; normalized overlap %3.")
                    .arg(doc.mesh(arc.s).name,
                         doc.mesh(arc.t).name)
                    .arg(arc.norm_area, 0, 'f', 3);
    }

    if (info.isEmpty())
        info << QObject::tr("No overlapping mesh pairs found.");
    else
        info.prepend(QObject::tr("Found %1 overlapping mesh pair(s).").arg(occupancyGrid.SVA.size()));

    return success(false, info);
}

} // namespace

QString IcpFilterPlugin::pluginId() const
{
    return QStringLiteral("meshlab2.filter.icp");
}

QString IcpFilterPlugin::name() const
{
    return QObject::tr("Alignment Filters");
}

MeshFilterRunResult IcpFilterPlugin::runFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc) const
{
    if (filterId == QString::fromLatin1(kTwoMeshIcp))
        return runTwoMeshIcp(params, doc);
    if (filterId == QString::fromLatin1(kGlobalIcp))
        return runGlobalIcp(params, doc);
    if (filterId == QString::fromLatin1(kOverlappingMeshes))
        return runOverlappingMeshes(params, doc);
    return fail(QObject::tr("Unknown ICP filter '%1'.").arg(filterId));
}

void registerIcpFilterPlugin(MeshFilterPluginManager &pluginManager)
{
    pluginManager.registerPlugin(std::make_unique<IcpFilterPlugin>());
}
