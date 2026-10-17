#include "open3dregistration.h"

#include "document.h"
#include "filterparam.h"
#include "open3dmeshadapter.h"

#include "open3d/geometry/KDTreeSearchParam.h"
#include "open3d/geometry/PointCloud.h"
#include "open3d/pipelines/registration/ColoredICP.h"
#include "open3d/pipelines/registration/FastGlobalRegistration.h"
#include "open3d/pipelines/registration/Feature.h"
#include "open3d/pipelines/registration/GlobalOptimization.h"
#include "open3d/pipelines/registration/GlobalOptimizationConvergenceCriteria.h"
#include "open3d/pipelines/registration/GlobalOptimizationMethod.h"
#include "open3d/pipelines/registration/PoseGraph.h"
#include "open3d/utility/Random.h"
#include "open3d/pipelines/registration/Registration.h"
#include "open3d/pipelines/registration/TransformationEstimation.h"

#include <QObject>
#include <QStringList>
#include <algorithm>
#include <exception>
#include <functional>
#include <utility>
#include <vector>

namespace {

constexpr QLatin1StringView kPointToPoint("align_by_point_to_point_icp_open3d");
constexpr QLatin1StringView kPointToPlane("align_by_point_to_plane_icp_open3d");
constexpr QLatin1StringView kColored("align_by_colored_icp_open3d");
constexpr QLatin1StringView kFeature("align_by_feature_matching_open3d");
constexpr QLatin1StringView kFeatureSets("align_sets_by_feature_matching_open3d");
constexpr QLatin1StringView kGlobal("align_meshes_globally_open3d");
constexpr QLatin1StringView kSequential("align_meshes_sequentially_open3d");
constexpr QLatin1StringView kFastGlobal("align_by_fast_global_registration_open3d");

namespace Adapter = meshlab::open3d;
namespace reg = open3d::pipelines::registration;

// The progress bar of one filter run. A filter that is called by another -- the
// sequential aligner drives the pairwise registrations and the global one --
// must not open and close the bar itself: each inner finish would unblock the
// interface in the middle of the outer run and rewind the percentage. So the
// outer filter owns it, and the inner ones are handed a quiet Progress.
class Progress
{
public:
    Progress(Document &doc, bool owner) : m_doc(doc), m_owner(owner) {}

    void begin(const QString &label) const
    {
        if (m_owner)
            m_doc.beginFilterProgress(label);
    }
    void finish(bool success, const QString &message) const
    {
        if (m_owner)
            m_doc.finishFilterProgress(success, message);
    }
    void report(int percent, const QString &message) const
    {
        if (!m_owner)
            return;
        if (vcg::CallBackPos *cb = m_doc.progressCallback())
            (*cb)(percent, qPrintable(message));
    }

private:
    Document &m_doc;
    bool m_owner;
};

MeshFilterRunResult fail(const QString &message)
{
    return { false, false, message };
}

// A length in the layer's own units has no sensible fixed default, so these filters
// declare theirs as absperc with a default scaled to the layer, and every call that
// comes through the framework carries a positive value. This fallback is for the one
// caller that does not: the sequential aligner runs the pairwise steps with a partial
// parameter set -- it names the pairing distance and leaves the matching voxel and the
// feature radius out -- and wants each pair scaled to its own reference. Zero or absent
// means derive it, from the diagonal, with the fraction the manifest uses as its default.
double resolveLength(double diagonal, double given, double fractionOfDiagonal)
{
    if (given > 0.0)
        return given;
    return diagonal > 0.0 ? diagonal * fractionOfDiagonal : 0.0;
}

double resolveLength(const VCGMesh &mesh, double given, double fractionOfDiagonal)
{
    return resolveLength(double(mesh.bbox.Diag()), given, fractionOfDiagonal);
}

QMatrix4x4 toQMatrix(const Eigen::Matrix4d &m)
{
    QMatrix4x4 out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            out(r, c) = float(m(r, c));
    }
    return out;
}

Eigen::Matrix4d toEigen(const QMatrix4x4 &m)
{
    Eigen::Matrix4d out;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c)
            out(r, c) = double(m(r, c));
    }
    return out;
}

// World-space bounding boxes, used only to decide which pairs are worth
// registering. Every pair is O(points) to register, so on a dozen scans the
// difference between all pairs and overlapping ones is the difference between
// a minute and a few seconds.
vcg::Box3f worldBox(const Document::MeshEntry &entry)
{
    vcg::Box3f box;
    const vcg::Box3f &local = entry.mesh.bbox;
    for (int corner = 0; corner < 8; ++corner) {
        const vcg::Point3f p(
            (corner & 1) ? local.max.X() : local.min.X(),
            (corner & 2) ? local.max.Y() : local.min.Y(),
            (corner & 4) ? local.max.Z() : local.min.Z());
        const QVector3D q = entry.transform.map(QVector3D(p.X(), p.Y(), p.Z()));
        box.Add(vcg::Point3f(q.x(), q.y(), q.z()));
    }
    return box;
}

// ---------------------------------------------------------------------------
// Multiway registration.
//
// Open3D's pose graph convention, read out of the residual in
// GlobalOptimization.cpp (X_inv * Tt_inv * Ts ~ I):
//
//     node.pose_            maps that node's LOCAL frame to world
//     edge.transformation_  = T_target^-1 * T_source
//
// which is exactly what RegistrationICP(source, target, ...) returns when both
// clouds are given in their own local frames. So the clouds are NOT moved into
// world space here -- unlike every other filter in this file -- and a layer's
// matrix is its node pose directly, before and after. Getting this backwards
// yields a plausible-looking optimisation that moves everything somewhere wrong.
//
// `onlyLayers`, when given, replaces the visibility selection: the sequential
// aligner hands over exactly the layers it managed to place.
MeshFilterRunResult runGlobalAlignment(
    const FilterParams &params, Document &doc, const std::vector<int> *onlyLayers = nullptr,
    bool ownsProgress = true)
{
    const Progress progress(doc, ownsProgress);
    const bool onlyVisible = params.getBool(QStringLiteral("onlyVisibleLayers"), true);

    std::vector<int> layers;
    if (onlyLayers) {
        layers = *onlyLayers;
    } else {
        for (int i = 0; i < doc.meshCount(); ++i) {
            if (onlyVisible && !doc.mesh(i).visible)
                continue;
            if (doc.mesh(i).mesh.VN() > 0)
                layers.push_back(i);
        }
    }
    if (layers.size() < 2) {
        return fail(QObject::tr(
            "Global alignment needs at least two layers with vertices; found %1.")
                .arg(layers.size()));
    }

    const VCGMesh &first = doc.mesh(layers.front()).mesh;
    const double voxelSize = resolveLength(
        first, params.getDouble(QStringLiteral("registrationVoxelSize"), 0.0), 0.01);
    const double maxCorrespondenceDistance = resolveLength(
        first, params.getDouble(QStringLiteral("maxCorrespondenceDistance"), 0.0), 0.03);
    if (!(maxCorrespondenceDistance > 0.0))
        return fail(QObject::tr("The layers have no extent, so no distances can be derived."));

    // Same reason as the pairwise filters: the feature-matching path samples
    // randomly, so the run has to be reproducible.
    RandomSeed seed;
    if (params.getEnum(QStringLiteral("pairInitialization"), QStringLiteral("currentPose"))
            == QLatin1StringView("featureMatching")) {
        seed = params.getRandomSeed();
        open3d::utility::random::Seed(int(seed.value));
    }

    progress.begin(QObject::tr("Global alignment"));

    // Local-frame clouds, downsampled once and reused by every pair.
    std::vector<open3d::geometry::PointCloud> clouds(layers.size());
    std::vector<Eigen::Matrix4d> poses(layers.size());
    QString error;
    for (size_t i = 0; i < layers.size(); ++i) {
        const Document::MeshEntry &entry = doc.mesh(layers[i]);
        Adapter::CloudMapping mapping;
        if (!Adapter::meshToCloud(entry.mesh, clouds[i], mapping, error)) {
            progress.finish(false, error);
            return fail(QObject::tr("Layer '%1': %2").arg(entry.name, error));
        }
        if (voxelSize > 0.0) {
            if (const auto down = clouds[i].VoxelDownSample(voxelSize)) {
                if (down->points_.size() >= 10) {
                    // Averaged normals are shorter than one where a cell's normals disagree,
                    // and the point-to-plane term scales with their length.
                    down->NormalizeNormals();
                    clouds[i] = *down;
                }
            }
        }
        if (clouds[i].normals_.size() != clouds[i].points_.size()) {
            clouds[i].EstimateNormals(
                open3d::geometry::KDTreeSearchParamHybrid(maxCorrespondenceDistance * 2.0, 30));
        }
        poses[i] = toEigen(entry.transform);
    }

    // Open3D's register_fragments pipeline distinguishes two kinds of pair: a
    // consecutive one, whose relative pose is already known from odometry and
    // only needs refining, and every other one, whose pose is unknown and has
    // to be *found* -- their compute_initial_registration, which is global
    // registration on FPFH features.
    //
    // Layers in a document carry no odometry, so which case applies depends on
    // whether the user has already placed them. Trusting the layer matrices is
    // right after a rough manual placement and useless before one, which is
    // what this choice is.
    const bool matchFeatures =
        params.getEnum(QStringLiteral("pairInitialization"), QStringLiteral("currentPose"))
            == QLatin1StringView("featureMatching");

    // A bounding-box overlap test asks where the layers are, so it is only
    // meaningful when their poses are trusted. With feature matching the poses
    // are exactly what is unknown, and the test would reject the pairs that
    // most need computing.
    const bool allPairs = matchFeatures
        || params.getEnum(QStringLiteral("candidatePairs"), QStringLiteral("overlapping"))
               == QLatin1StringView("all");

    // FPFH is computed once per layer and reused by every pair that layer takes
    // part in, as Open3D's own pipeline does in preprocess_point_cloud.
    std::vector<std::shared_ptr<reg::Feature>> features;
    if (matchFeatures) {
        const double featureRadius = maxCorrespondenceDistance * 5.0;
        features.resize(layers.size());
        for (size_t i = 0; i < layers.size(); ++i) {
            progress.report(5 + int(5.0 * double(i) / double(layers.size())),
                QStringLiteral("Computing features for layer %1 of %2...")
                                     .arg(i + 1).arg(layers.size()));
            features[i] = reg::ComputeFPFHFeature(
                clouds[i],
                open3d::geometry::KDTreeSearchParamHybrid(featureRadius, 100));
        }
    }
    std::vector<vcg::Box3f> boxes;
    boxes.reserve(layers.size());
    for (int index : layers)
        boxes.push_back(worldBox(doc.mesh(index)));

    reg::PoseGraph graph;
    for (const Eigen::Matrix4d &pose : poses)
        graph.nodes_.push_back(reg::PoseGraphNode(pose));

    const double minimumFitness = params.getDouble(QStringLiteral("minimumFitness"), 0.3);
    int connectedComponents = 1;
    int spanningEdges = 0;
    const int maxIterations = std::max(1, params.getInt(QStringLiteral("maxIterations"), 30));
    const int pairBudget = int(layers.size() * (layers.size() - 1) / 2);
    int considered = 0;
    int accepted = 0;

    struct Candidate {
        int source;
        int target;
        double fitness;
        Eigen::Matrix4d transformation;
        Eigen::Matrix6d information;
    };
    std::vector<Candidate> candidates;

    try {
        for (size_t t = 0; t < layers.size(); ++t) {
            for (size_t srcIdx = t + 1; srcIdx < layers.size(); ++srcIdx) {
                if (!allPairs) {
                    vcg::Box3f overlap = boxes[t];
                    overlap.Intersect(boxes[srcIdx]);
                    if (overlap.IsEmpty())
                        continue;
                }
                ++considered;
                progress.report(10 + int(70.0 * double(considered) / double(std::max(1, pairBudget))),
                    QStringLiteral("Registering pair %1 of at most %2...")
                                         .arg(considered).arg(pairBudget));

                // X = T_target^-1 * T_source. Either seeded from where the
                // layers already are, or found from scratch by matching
                // features and then refined -- Open3D's two cases.
                Eigen::Matrix4d init = poses[t].inverse() * poses[srcIdx];
                if (matchFeatures) {
                    const auto coarse = reg::FastGlobalRegistrationBasedOnFeatureMatching(
                        clouds[srcIdx], clouds[t], *features[srcIdx], *features[t],
                        reg::FastGlobalRegistrationOption(
                            1.4, /*use_absolute_scale=*/true, true,
                            maxCorrespondenceDistance));
                    if (coarse.fitness_ <= 0.0)
                        continue;
                    init = coarse.transformation_;
                }
                const auto result = reg::RegistrationICP(
                    clouds[srcIdx], clouds[t], maxCorrespondenceDistance, init,
                    reg::TransformationEstimationPointToPlane(),
                    reg::ICPConvergenceCriteria(1e-6, 1e-6, maxIterations));
                if (result.fitness_ < minimumFitness)
                    continue;

                const Eigen::Matrix6d information = reg::GetInformationMatrixFromPointClouds(
                    clouds[srcIdx], clouds[t], maxCorrespondenceDistance,
                    result.transformation_);
                candidates.push_back({ int(srcIdx), int(t), result.fitness_,
                                       result.transformation_, information });
                ++accepted;
            }
        }

        if (accepted == 0) {
            error = QObject::tr(
                        "No pair of layers registered above the minimum fitness (%1). They may "
                        "not overlap, or the correspondence distance (%2) may be too small.")
                        .arg(minimumFitness).arg(maxCorrespondenceDistance);
            progress.finish(false, error);
            return fail(error);
        }

        // Open3D wants a connected subset of *certain* edges -- its own pipeline
        // gets them from scan order, as odometry between consecutive frames.
        // Layers in a document have no such order, and marking every edge
        // uncertain leaves the certain subset empty, which the optimiser warns
        // about and then handles badly.
        //
        // So the backbone is built here: a maximum spanning tree over the
        // accepted pairs, strongest fitness first, marked certain. Everything
        // else stays a loop closure and keeps the benefit of the line process,
        // which is what lets one bad pair be discounted rather than believed.
        std::sort(candidates.begin(), candidates.end(),
                  [](const Candidate &a, const Candidate &b) { return a.fitness > b.fitness; });

        std::vector<int> parent(layers.size());
        for (size_t i = 0; i < parent.size(); ++i)
            parent[i] = int(i);
        const std::function<int(int)> findRoot = [&](int x) {
            while (parent[size_t(x)] != x)
                x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
            return x;
        };

        int certainEdges = 0;
        for (const Candidate &c : candidates) {
            const int rs = findRoot(c.source);
            const int rt = findRoot(c.target);
            const bool joinsComponents = rs != rt;
            if (joinsComponents) {
                parent[size_t(rs)] = rt;
                ++certainEdges;
            }
            graph.edges_.push_back(reg::PoseGraphEdge(
                c.source, c.target, c.transformation, c.information, !joinsComponents));
        }

        int components = 0;
        for (size_t i = 0; i < parent.size(); ++i) {
            if (findRoot(int(i)) == int(i))
                ++components;
        }
        connectedComponents = components;
        spanningEdges = certainEdges;

        progress.report(85, QStringLiteral("Optimizing the pose graph (no progress available)..."));

        const int referenceLayer =
            params.getMesh(QStringLiteral("referenceMesh"), layers.front());
        int referenceNode = 0;
        for (size_t i = 0; i < layers.size(); ++i) {
            if (layers[i] == referenceLayer)
                referenceNode = int(i);
        }

        const reg::GlobalOptimizationOption option(
            maxCorrespondenceDistance,
            params.getDouble(QStringLiteral("edgePruneThreshold"), 0.25),
            params.getDouble(QStringLiteral("preferenceLoopClosure"), 2.0),
            referenceNode);
        reg::GlobalOptimization(
            graph, reg::GlobalOptimizationLevenbergMarquardt(),
            reg::GlobalOptimizationConvergenceCriteria(), option);
    } catch (const std::exception &e) {
        error = QObject::tr("Open3D global alignment failed: %1")
                    .arg(QString::fromLocal8Bit(e.what()));
        progress.finish(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("Open3D global alignment failed with an unknown error.");
        progress.finish(false, error);
        return fail(error);
    }

    if (graph.nodes_.size() != layers.size()) {
        error = QObject::tr("The optimizer returned %1 poses for %2 layers.")
                    .arg(graph.nodes_.size()).arg(layers.size());
        progress.finish(false, error);
        return fail(error);
    }

    // A node's pose IS the layer matrix, so it replaces rather than composes.
    //
    // The movement reported is how far the layer's own bounding-box corners
    // travel, not the change in the matrix's translation column: scans sit far
    // from the origin, so a small rotation shifts that column by more than the
    // object is wide and the number would overstate the move several times over.
    double largestMove = 0.0;
    for (size_t i = 0; i < layers.size(); ++i) {
        const QMatrix4x4 before = doc.mesh(layers[i]).transform;
        const QMatrix4x4 after = toQMatrix(graph.nodes_[i].pose_);
        doc.setMeshTransform(layers[i], after);
        const vcg::Box3f &box = doc.mesh(layers[i]).mesh.bbox;
        for (int corner = 0; corner < 8; ++corner) {
            const vcg::Point3f p = box.P(corner);
            const QVector3D local(p.X(), p.Y(), p.Z());
            largestMove = std::max(largestMove, double((after.map(local) - before.map(local)).length()));
        }
    }

    progress.finish(true, QObject::tr("Aligned the layers."));

    MeshFilterRunResult out;
    out.success = true;
    out.documentModified = true;
    out.infoMessages
        << QObject::tr("Layers: %1").arg(layers.size())
        << QObject::tr("Pairs: %1 accepted of %2 registered (minimum fitness %3)")
               .arg(accepted).arg(considered).arg(minimumFitness)
        << QObject::tr("Graph: %1 backbone edge(s), %2 loop closure(s)")
               .arg(spanningEdges).arg(accepted - spanningEdges)
        << QObject::tr("Largest layer movement: %1").arg(largestMove, 0, 'f', 6)
        << QObject::tr("Correspondence distance: %1, matching voxel size: %2")
               .arg(maxCorrespondenceDistance).arg(voxelSize)
        << (matchFeatures ? seed.message() : QString())
        << (matchFeatures
                ? QObject::tr("Pair poses found by feature matching; the layers' existing "
                              "matrices were not used as a starting point, and every pair was "
                              "attempted.")
                : QObject::tr("Pair poses refined from the layers' existing matrices. Switch to "
                              "feature matching if they are not already roughly placed."));
    if (connectedComponents > 1) {
        out.infoMessages << QObject::tr(
            "The graph has %1 disconnected groups: layers in different groups share no "
            "usable overlap, so their poses are not related to each other. Only the group "
            "containing the reference layer is anchored.").arg(connectedComponents);
    }
    return out;
}

// What a caller that chains pairwise registrations needs to judge each link,
// without parsing the log text.
struct PairQuality
{
    double fitness = 0.0;     // fraction of source points with a correspondence
    double inlierRmse = 0.0;  // RMS distance of those correspondences, layer units
};

// One pairwise registration: the layers of `sourceLayers` -- one, or the whole of a set --
// are moved, all by the same correction, onto the layer the `referenceMesh` parameter names,
// or onto `referenceOverride` when given (a set). Called with the current layer by the
// filters themselves, and with each chain layer in turn by the sequential aligner, which is
// also why it takes the layers as an argument rather than reading a parameter.
MeshFilterRunResult runPairwise(
    const QString &filterId,
    const FilterParams &params,
    Document &doc,
    const std::vector<int> &sourceLayers,
    bool ownsProgress = true,
    PairQuality *qualityOut = nullptr,
    const std::vector<int> *referenceOverride = nullptr)
{
    const Progress progress(doc, ownsProgress);
    const bool isPointToPoint = filterId == QString::fromLatin1(kPointToPoint);
    const bool isPointToPlane = filterId == QString::fromLatin1(kPointToPlane);
    const bool isColored = filterId == QString::fromLatin1(kColored);
    // Aligning two sets is the feature-matching registration with a set on each side.
    const bool isFeature = filterId == QString::fromLatin1(kFeature)
        || filterId == QString::fromLatin1(kFeatureSets);
    const bool isFastGlobal = filterId == QString::fromLatin1(kFastGlobal);
    if (!isPointToPoint && !isPointToPlane && !isColored && !isFeature && !isFastGlobal)
        return fail(QObject::tr("Unknown filter id: %1").arg(filterId));

    if (sourceLayers.empty())
        return fail(QObject::tr("Registration requires a valid current layer."));
    for (const int layer : sourceLayers) {
        if (layer < 0 || layer >= doc.meshCount())
            return fail(QObject::tr("Registration requires a valid current layer."));
    }
    const auto isSource = [&sourceLayers](int layer) {
        return std::find(sourceLayers.begin(), sourceLayers.end(), layer) != sourceLayers.end();
    };

    // The reference is one layer, named by the parameter -- or, for feature
    // matching only, every other visible layer taken together. Features describe
    // shape, not position, so a pose can be found against any surface the layer
    // overlaps; that is what lets the layer join a scan sequence whose previous
    // member does not overlap it. The ICP objectives refine from a pose already
    // near, where a single named reference is the meaningful thing, so they do
    // not offer it.
    const bool useAllVisible =
        isFeature && params.getBool(QStringLiteral("useAllVisibleAsReference"), false);
    std::vector<int> referenceLayers;
    if (referenceOverride) {
        referenceLayers = *referenceOverride;
        for (const int layer : referenceLayers) {
            if (layer < 0 || layer >= doc.meshCount())
                return fail(QObject::tr("Registration requires two valid layers."));
            if (isSource(layer))
                return fail(QObject::tr("A layer cannot be in both sets: '%1' is.")
                                .arg(doc.mesh(layer).name));
        }
        if (referenceLayers.empty())
            return fail(QObject::tr("The reference set has no layer with vertices."));
    } else if (useAllVisible) {
        for (int i = 0; i < doc.meshCount(); ++i) {
            if (!isSource(i) && doc.mesh(i).visible && doc.mesh(i).mesh.VN() > 0)
                referenceLayers.push_back(i);
        }
        if (referenceLayers.empty()) {
            return fail(QObject::tr(
                "Use All Visible Layers as Reference is on, but no layer other than the current "
                "one is visible."));
        }
    } else {
        const int referenceIndex =
            params.getMesh(QStringLiteral("referenceMesh"), doc.currentMeshIndex());
        if (referenceIndex < 0 || referenceIndex >= doc.meshCount())
            return fail(QObject::tr("Registration requires two valid layers."));
        if (isSource(referenceIndex))
            return fail(QObject::tr("Registration requires two different layers."));
        referenceLayers.push_back(referenceIndex);
    }

    // Every cloud is taken in world space, through its layer matrix, so the
    // result is the correction to apply on top of what the source already has
    // rather than a transform from its local frame. The reference set is simply
    // those clouds concatenated: nothing downstream cares how many layers made
    // the target, and the downsampling the feature path already does is what
    // keeps overlapping layers from over-weighting the surface they share.
    QString error;
    open3d::geometry::PointCloud source;
    open3d::geometry::PointCloud target;
    Adapter::CloudMapping sourceMapping;
    Adapter::CloudMapping targetMapping;
    // The source is built as the reference is: the layers' points together, each through its
    // own matrix, so that one correction moves them all as the rigid group they are.
    QString sourceWithoutNormals;
    for (const int layer : sourceLayers) {
        const Document::MeshEntry &entry = doc.mesh(layer);
        open3d::geometry::PointCloud part;
        if (!Adapter::meshToCloud(entry.mesh, part, sourceMapping, error, &entry.transform))
            return fail(QObject::tr("Source layer '%1': %2").arg(entry.name, error));
        if (!sourceMapping.hadNormals && sourceWithoutNormals.isEmpty())
            sourceWithoutNormals = entry.name;
        source += part;
    }
    // What a group of layers is called in messages: their names when they are few, else a count.
    const auto groupLabel = [&doc](const std::vector<int> &layers) {
        if (layers.size() > 3)
            return QObject::tr("%1 layers").arg(layers.size());
        QStringList names;
        for (const int layer : layers)
            names << doc.mesh(layer).name;
        return names.join(QStringLiteral(", "));
    };
    const QString sourceLabel = groupLabel(sourceLayers);
    QString layerWithoutNormals;
    for (const int layer : referenceLayers) {
        const Document::MeshEntry &entry = doc.mesh(layer);
        open3d::geometry::PointCloud part;
        if (!Adapter::meshToCloud(entry.mesh, part, targetMapping, error, &entry.transform))
            return fail(QObject::tr("Reference layer '%1': %2").arg(entry.name, error));
        // Concatenation drops normals unless every part has them, so which layer
        // is missing them has to be noted here, not rediscovered afterwards.
        if (!targetMapping.hadNormals && layerWithoutNormals.isEmpty())
            layerWithoutNormals = entry.name;
        target += part;
    }

    // What the reference is called in messages, and how big it is: the one
    // layer's own bounding-box diagonal as before, or for a set the extent of
    // the points together, since the layers sit at different places in the world.
    QString referenceLabel;
    double referenceDiagonal = 0.0;
    if (referenceLayers.size() == 1) {
        referenceLabel = doc.mesh(referenceLayers.front()).name;
        referenceDiagonal = double(doc.mesh(referenceLayers.front()).mesh.bbox.Diag());
    } else {
        referenceLabel = referenceOverride
            ? groupLabel(referenceLayers)
            : QObject::tr("%1 visible layers").arg(referenceLayers.size());
        Eigen::Vector3d low = target.points_.front();
        Eigen::Vector3d high = low;
        for (const Eigen::Vector3d &point : target.points_) {
            low = low.cwiseMin(point);
            high = high.cwiseMax(point);
        }
        referenceDiagonal = (high - low).norm();
    }

    // Each objective needs something the plain point-to-point one does not, and
    // Open3D's behaviour without it ranges from an assertion to a silently poor
    // result. Refuse with the filter that supplies it named.
    if ((isPointToPlane || isColored) && target.normals_.size() != target.points_.size()) {
        return fail(QObject::tr(
            "This objective needs normals on the reference layer '%1', which has none. "
            "Run Compute Point Cloud Normals on it first.").arg(layerWithoutNormals));
    }
    if (isColored) {
        if (source.colors_.size() != source.points_.size()
            || target.colors_.size() != target.points_.size()) {
            return fail(QObject::tr(
                "Colored ICP needs per-vertex color on both layers. '%1' or '%2' has none.")
                    .arg(sourceLabel, referenceLabel));
        }
        if (source.normals_.size() != source.points_.size()) {
            return fail(QObject::tr(
                "Colored ICP needs normals on the source layer '%1' as well.")
                    .arg(sourceWithoutNormals));
        }
    }
    if ((isFeature || isFastGlobal)
        && (source.normals_.size() != source.points_.size()
            || target.normals_.size() != target.points_.size())) {
        const QString missing = source.normals_.size() != source.points_.size()
            ? sourceWithoutNormals : layerWithoutNormals;
        return fail(QObject::tr(
            "Feature matching describes each point by its neighbours' orientations, so "
            "every layer involved needs normals; '%1' has none. Run Compute Point Cloud "
            "Normals on it first.").arg(missing));
    }

    const double maxCorrespondenceDistance = resolveLength(
        referenceDiagonal,
        params.getDouble(QStringLiteral("maxCorrespondenceDistance"), 0.0),
        0.02);
    if (!(maxCorrespondenceDistance > 0.0)) {
        return fail(QObject::tr(
            "The reference layer has no extent, so no correspondence distance can be "
            "derived. Give one explicitly."));
    }

    const int maxIterations = std::max(1, params.getInt(QStringLiteral("maxIterations"), 30));

    // Set by the feature-matching branch so the log can say what it actually ran on.
    std::pair<size_t, size_t> downsampledFrom{ 0, 0 };

    // Both feature-based routes sample randomly: RANSAC draws correspondence
    // triples, and FGR's tuple test draws triples too
    // (Registration.cpp and FastGlobalRegistration.cpp both use
    // utility::random::UniformIntGenerator). Open3D seeds that generator
    // globally and once, so without this two identical runs give different
    // poses -- which is how this was found, by a test that passed and then
    // failed on the same input.
    RandomSeed seed;
    if (isFeature || isFastGlobal) {
        seed = params.getRandomSeed();
        open3d::utility::random::Seed(int(seed.value));
    }

    progress.begin(QObject::tr("Registering layers"));
    // Open3D offers no progress callback anywhere in the registration path:
    // ComputeFPFHFeature, RegistrationICP and the RANSAC functions take no hook
    // and never touch its own utility::ProgressBar. So the bar can only move
    // between the calls, and the label is the honest part -- it names the phase
    // that is about to block. Keeping the phases short is the real fix, which
    // is what the downsampling below is for.
    progress.report(20, isFeature ? QStringLiteral("Preparing clouds...")
                                  : QStringLiteral("Running ICP (no progress available)..."));

    reg::RegistrationResult result;
    try {
        const reg::ICPConvergenceCriteria criteria(1e-6, 1e-6, maxIterations);
        if (isPointToPoint) {
            result = reg::RegistrationICP(
                source, target, maxCorrespondenceDistance, Eigen::Matrix4d::Identity(),
                reg::TransformationEstimationPointToPoint(
                    params.getBool(QStringLiteral("estimateScale"), false)),
                criteria);
        } else if (isPointToPlane) {
            result = reg::RegistrationICP(
                source, target, maxCorrespondenceDistance, Eigen::Matrix4d::Identity(),
                reg::TransformationEstimationPointToPlane(), criteria);
        } else if (isColored) {
            result = reg::RegistrationColoredICP(
                source, target, maxCorrespondenceDistance, Eigen::Matrix4d::Identity(),
                reg::TransformationEstimationForColoredICP(), criteria);
        } else if (isFeature || isFastGlobal) {
            // Global registration is defined on a *downsampled* cloud, and
            // feeding it a raw scan is not merely slow, it is quadratic-ish
            // misuse: every RANSAC iteration copies and transforms the whole
            // source cloud and then runs a nearest-neighbour query per point
            // (Registration.cpp, the RANSAC worker), and there are up to
            // `ransacIterations` of them. At 150k points that is twenty minutes;
            // at a few thousand it is seconds, and the pose is the same because
            // a rigid transform does not care how densely the surface was
            // sampled. Open3D's own pipeline downsamples first and its defaults
            // -- 100 feature neighbours, 100000 RANSAC iterations -- are chosen
            // for that size.
            const double registrationVoxelSize = resolveLength(
                referenceDiagonal,
                params.getDouble(QStringLiteral("registrationVoxelSize"), 0.0), 0.01);
            const size_t sourceBefore = source.points_.size();
            const size_t targetBefore = target.points_.size();
            if (registrationVoxelSize > 0.0) {
                progress.report(30, QStringLiteral("Downsampling for feature matching..."));
                const auto sourceDown = source.VoxelDownSample(registrationVoxelSize);
                const auto targetDown = target.VoxelDownSample(registrationVoxelSize);
                if (sourceDown && targetDown
                    && sourceDown->points_.size() >= 3 && targetDown->points_.size() >= 3) {
                    // FPFH assumes unit normals; a cell's averaged one is shorter than that
                    // wherever the normals it averaged disagree.
                    sourceDown->NormalizeNormals();
                    targetDown->NormalizeNormals();
                    source = *sourceDown;
                    target = *targetDown;
                }
            }
            downsampledFrom = { sourceBefore, targetBefore };

            const double featureRadius = resolveLength(
                referenceDiagonal,
                params.getDouble(QStringLiteral("featureRadius"), 0.0), 0.05);
            const int featureNeighbours =
                std::max(3, params.getInt(QStringLiteral("featureNeighbours"), 100));
            const open3d::geometry::KDTreeSearchParamHybrid featureSearch(
                featureRadius, featureNeighbours);

            progress.report(40, QStringLiteral(
                    "Computing FPFH features on %1 + %2 points...")
                        .arg(source.points_.size()).arg(target.points_.size()));
            const auto sourceFeatures = reg::ComputeFPFHFeature(source, featureSearch);
            const auto targetFeatures = reg::ComputeFPFHFeature(target, featureSearch);

            progress.report(60, QStringLiteral(
                    "RANSAC, up to %1 iterations (no progress available)...")
                        .arg(params.getInt(QStringLiteral("ransacIterations"), 100000)));
            if (isFastGlobal) {
                progress.report(60, QStringLiteral("Fast global registration (no progress available)..."));
                // Graduated non-convexity rather than sampling: the objective is
                // optimised directly, with the scale parameter mu annealed down
                // by division_factor each round. use_absolute_scale is true
                // because every distance this plugin exposes is already in the
                // layers' own units -- letting Open3D rescale by model diameter
                // would silently reinterpret the value the user typed.
                reg::FastGlobalRegistrationOption option(
                    params.getDouble(QStringLiteral("divisionFactor"), 1.4),
                    /*use_absolute_scale=*/true,
                    params.getBool(QStringLiteral("decreaseMu"), true),
                    maxCorrespondenceDistance,
                    std::max(1, params.getInt(QStringLiteral("fgrIterations"), 64)),
                    params.getDouble(QStringLiteral("tupleScale"), 0.95),
                    std::max(1, params.getInt(QStringLiteral("maximumTupleCount"), 1000)),
                    params.getBool(QStringLiteral("tupleTest"), true));
                result = reg::FastGlobalRegistrationBasedOnFeatureMatching(
                    source, target, *sourceFeatures, *targetFeatures, option);
            } else {
                result = reg::RegistrationRANSACBasedOnFeatureMatching(
                    source, target, *sourceFeatures, *targetFeatures,
                    params.getBool(QStringLiteral("mutualFilter"), true),
                    maxCorrespondenceDistance,
                    reg::TransformationEstimationPointToPoint(false),
                    3, {},
                    reg::RANSACConvergenceCriteria(
                        std::max(1, params.getInt(QStringLiteral("ransacIterations"), 100000)),
                        params.getDouble(QStringLiteral("ransacConfidence"), 0.999)));
            }
        }
    } catch (const std::exception &e) {
        error = QObject::tr("Open3D registration failed: %1").arg(QString::fromLocal8Bit(e.what()));
        progress.finish(false, error);
        return fail(error);
    } catch (...) {
        error = QObject::tr("Open3D registration failed with an unknown error.");
        progress.finish(false, error);
        return fail(error);
    }

    if (qualityOut)
        *qualityOut = { result.fitness_, result.inlier_rmse_ };
    if (result.fitness_ <= 0.0) {
        error = QObject::tr(
                    "No correspondence was found between '%1' and '%2'. The maximum "
                    "correspondence distance (%3) is in the layers' own units; if the two "
                    "start far apart, align them roughly first or raise it.")
                    .arg(sourceLabel, referenceLabel)
                    .arg(maxCorrespondenceDistance);
        progress.finish(false, error);
        return fail(error);
    }

    // The result is a world-space correction, so it is composed onto whatever
    // the layer already carried rather than replacing it. Coordinates are not
    // touched: the layer-transform contract is what every other aligner here
    // follows.
    const QMatrix4x4 correction = toQMatrix(result.transformation_);
    for (const int layer : sourceLayers)
        doc.setMeshTransform(layer, correction * doc.mesh(layer).transform);

    progress.finish(true, QObject::tr("Registered the layers."));

    MeshFilterRunResult out;
    out.success = true;
    out.documentModified = true;
    out.infoMessages
        << QObject::tr("Moved '%1' onto '%2'.").arg(sourceLabel, referenceLabel)
        << QObject::tr("Fitness: %1 (fraction of source points with a correspondence)")
               .arg(result.fitness_, 0, 'f', 4)
        << QObject::tr("Inlier RMSE: %1").arg(result.inlier_rmse_, 0, 'f', 6)
        << QObject::tr("Maximum correspondence distance: %1").arg(maxCorrespondenceDistance);
    if (referenceLayers.size() > 1) {
        QStringList names;
        for (const int layer : referenceLayers)
            names << doc.mesh(layer).name;
        out.infoMessages << QObject::tr("Reference layers: %1").arg(names.join(QStringLiteral(", ")));
    }
    if (sourceLayers.size() > 1) {
        QStringList names;
        for (const int layer : sourceLayers)
            names << doc.mesh(layer).name;
        out.infoMessages << QObject::tr("Moved layers: %1").arg(names.join(QStringLiteral(", ")));
    }
    if (isFeature || isFastGlobal)
        out.infoMessages << seed.message();
    if (downsampledFrom.first > 0) {
        out.infoMessages << QObject::tr(
            "Matched on %1 and %2 point(s), downsampled from %3 and %4. The pose applies "
            "to the full layers.")
                .arg(source.points_.size()).arg(target.points_.size())
                .arg(downsampledFrom.first).arg(downsampledFrom.second);
    }
    if (result.fitness_ < 0.3) {
        out.infoMessages << QObject::tr(
            "Fitness below 0.3 usually means the two layers barely overlap at this "
            "correspondence distance. Treat the result as unreliable.");
    }
    return out;
}

// Align Sets by Feature Matching: the feature-matching registration with a set of layers on each
// side. Each set is taken as one rigid group -- its layers' points together, each through its
// own matrix -- the moving group is registered onto the reference group, and every layer of the
// moving set gets the same correction. For two sets of scans that are each already aligned among
// themselves and need placing relative to each other, which pairwise registration of single
// layers cannot do when no one scan of one set overlaps enough of the other.
MeshFilterRunResult runAlignSets(const FilterParams &params, Document &doc)
{
    const int movingSet = doc.indexOfVisibilitySet(params.getString(QStringLiteral("movingSet")));
    const int referenceSet =
        doc.indexOfVisibilitySet(params.getString(QStringLiteral("referenceSet")));
    if (movingSet < 0 || referenceSet < 0)
        return fail(QObject::tr("Align Sets needs two existing visibility sets."));
    if (movingSet == referenceSet)
        return fail(QObject::tr("Align Sets needs two different sets: '%1' was given twice.")
                        .arg(doc.visibilitySets()[size_t(movingSet)].name));

    // A layer with no vertices has nothing to register and is left out of the group.
    const auto layersWithPoints = [&doc](int set) {
        std::vector<int> layers;
        for (const int layer : doc.visibilitySetMeshIndices(set)) {
            if (doc.mesh(layer).mesh.VN() > 0)
                layers.push_back(layer);
        }
        return layers;
    };
    const std::vector<int> moving = layersWithPoints(movingSet);
    const std::vector<int> reference = layersWithPoints(referenceSet);
    if (moving.empty() || reference.empty()) {
        return fail(QObject::tr("The set '%1' has no layer with vertices.")
                        .arg(doc.visibilitySets()[size_t(moving.empty() ? movingSet : referenceSet)].name));
    }
    return runPairwise(
        QString::fromLatin1(kFeatureSets), params, doc, moving, true, nullptr, &reference);
}

// Align Meshes Sequentially: the chain Open3D's register_fragments tutorial
// describes and a user otherwise does by hand. Starting from the reference, each
// next layer (in document order) is moved onto the one before it -- a feature
// match for the coarse pose, then point-to-plane ICP -- and the walk stops at the
// first pair that does not register. Whatever was placed up to there is then
// optimized together, so the drift a chain accumulates is shared out.
//
// It is built from runPairwise and runGlobalAlignment rather than beside them:
// the filter is the orchestration, and each pair is exactly what running the two
// pairwise filters by hand would have done.
//
// The progress bar belongs to this filter and covers the whole run: it advances
// by one pair's share each time a layer is aligned. Open3D has no progress hook,
// so within a pair the bar cannot move, and the figure is honest only at pair
// boundaries -- which is where the log line for the pair is written too. The
// inner steps are run quiet so that none of them rewinds or closes it.
MeshFilterRunResult runSequentialAlignment(const FilterParams &params, Document &doc)
{
    const Progress progress(doc, true);
    const bool onlyVisible = params.getBool(QStringLiteral("onlyVisibleLayers"), true);
    const int referenceIndex = params.getMesh(QStringLiteral("referenceMesh"), doc.currentMeshIndex());
    if (referenceIndex < 0 || referenceIndex >= doc.meshCount())
        return fail(QObject::tr("Sequential alignment needs a valid reference layer."));

    // The reference, then every later layer in document order. Layers listed
    // before the reference are not part of the chain and are left alone.
    std::vector<int> chain;
    for (int i = referenceIndex; i < doc.meshCount(); ++i) {
        const Document::MeshEntry &entry = doc.mesh(i);
        if (i != referenceIndex && ((onlyVisible && !entry.visible) || entry.mesh.VN() == 0))
            continue;
        chain.push_back(i);
    }
    if (chain.size() < 2) {
        return fail(QObject::tr(
            "Sequential alignment needs the reference layer and at least one layer after it "
            "in the layer list; found %1.").arg(chain.size()));
    }

    // Resolved once, so that every pair of a run with seed 0 shares the one
    // value the result reports and the whole run can be replayed.
    const RandomSeed seed = params.getRandomSeed();
    const double minimumFitness = params.getDouble(QStringLiteral("minimumFitness"), 0.5);
    const double correspondenceDistance =
        params.getDouble(QStringLiteral("maxCorrespondenceDistance"), 0.0);
    const int maxIterations = std::max(1, params.getInt(QStringLiteral("maxIterations"), 60));
    const int pairCount = int(chain.size()) - 1;
    const bool refine = params.getBool(QStringLiteral("globalRefinement"), true);

    // The pairs take the bar up to 90% when a global optimization will follow
    // and to 100% when it will not. With fewer than three layers the refinement
    // is skipped, so the larger share is only an upper bound -- harmless, since
    // the bar finishes at 100 either way.
    const int pairShare = (refine && pairCount >= 2) ? 90 : 100;
    progress.begin(QObject::tr("Aligning meshes sequentially"));

    size_t placed = 1; // chain[0] is the reference and is never moved
    QString stoppedAt;
    QString stopReason;
    for (size_t k = 1; k < chain.size(); ++k) {
        const QString sourceName = doc.mesh(chain[k]).name;
        const QString referenceName = doc.mesh(chain[k - 1]).name;
        doc.writeLog(QObject::tr("Pair %1 of %2: aligning '%3' onto '%4'")
                         .arg(k).arg(pairCount).arg(sourceName, referenceName));
        progress.report(pairShare * int(k - 1) / pairCount,
                        QObject::tr("Pair %1 of %2: %3 onto %4")
                            .arg(k).arg(pairCount).arg(sourceName, referenceName));

        const QMatrix4x4 before = doc.mesh(chain[k]).transform;
        MeshFilterParameterValues values;
        values.insert(QStringLiteral("referenceMesh"), chain[k - 1]);
        values.insert(QStringLiteral("maxCorrespondenceDistance"), correspondenceDistance);
        values.insert(QStringLiteral("randomSeed"), int(seed.value));

        PairQuality coarse;
        PairQuality refined;
        QString reason;
        MeshFilterRunResult step =
            runPairwise(kFeature, FilterParams(values), doc, { chain[k] }, false, &coarse);
        if (step.success) {
            values.insert(QStringLiteral("maxIterations"), maxIterations);
            step = runPairwise(kPointToPlane, FilterParams(values), doc, { chain[k] }, false, &refined);
        }
        if (!step.success)
            reason = step.errorMessage;
        else if (refined.fitness < minimumFitness)
            reason = QObject::tr("fitness %1 is below the minimum %2")
                         .arg(refined.fitness, 0, 'f', 3).arg(minimumFitness, 0, 'f', 3);

        if (!reason.isEmpty()) {
            while (reason.endsWith(QLatin1Char('.')))
                reason.chop(1);
            // A failed pair leaves the layer where it was found rather than
            // wherever the last attempt happened to put it.
            doc.setMeshTransform(chain[k], before);
            doc.writeLog(QObject::tr("Failed to align '%1' onto '%2': %3")
                             .arg(sourceName, referenceName, reason),
                         Document::LogSource::Application, Document::LogLevel::Warning);
            stoppedAt = sourceName;
            stopReason = reason;
            break;
        }
        // The error is the ICP inlier RMSE -- how far, on average, the source's
        // matched points lie from the reference surface -- in the layers' units and
        // as a share of the reference's diagonal, which is the figure comparable
        // from one set of scans to the next.
        const double diagonal = double(doc.mesh(chain[k - 1]).mesh.bbox.Diag());
        doc.writeLog(QObject::tr("Aligned '%1' onto '%2': inlier RMSE %3 (%4% of the diagonal), "
                                 "fitness %5 (feature matching alone %6)")
                         .arg(sourceName, referenceName)
                         .arg(refined.inlierRmse, 0, 'g', 4)
                         .arg(diagonal > 0.0 ? 100.0 * refined.inlierRmse / diagonal : 0.0, 0, 'f', 2)
                         .arg(refined.fitness, 0, 'f', 3).arg(coarse.fitness, 0, 'f', 3));
        ++placed;
        progress.report(pairShare * int(k) / pairCount,
                        QObject::tr("Aligned %1 of %2 layers").arg(placed).arg(chain.size()));
    }

    if (placed == 1) {
        const QString message = QObject::tr(
            "'%1' did not register onto the reference '%2' (%3). Nothing was moved.")
                .arg(stoppedAt, doc.mesh(chain[0]).name, stopReason);
        progress.finish(false, message);
        return fail(message);
    }

    MeshFilterRunResult out;
    out.success = true;
    out.documentModified = true;
    out.infoMessages << seed.message();

    // Optimizing the aligned group together is the point of stopping early: the
    // layers that did register are not held hostage by the one that did not.
    // With two layers there is a single pair and nothing to distribute.
    if (refine && placed >= 3) {
        const std::vector<int> aligned(chain.begin(), chain.begin() + long(placed));
        MeshFilterParameterValues values;
        values.insert(QStringLiteral("referenceMesh"), chain[0]);
        values.insert(QStringLiteral("pairInitialization"), QStringLiteral("currentPose"));
        values.insert(QStringLiteral("maxCorrespondenceDistance"), correspondenceDistance);
        values.insert(QStringLiteral("minimumFitness"), minimumFitness);
        values.insert(QStringLiteral("maxIterations"), maxIterations);
        doc.writeLog(QObject::tr("Optimizing the %1 aligned layers together").arg(placed));
        progress.report(pairShare + 5, QObject::tr("Optimizing the %1 aligned layers together "
                                                   "(no progress available)").arg(placed));
        const MeshFilterRunResult global =
            runGlobalAlignment(FilterParams(values), doc, &aligned, false);
        if (global.success) {
            out.infoMessages << QObject::tr("Optimized the %1 aligned layers together.").arg(placed);
        } else {
            doc.writeLog(QObject::tr("  global optimization failed: %1; the sequential "
                                     "alignment is kept.").arg(global.errorMessage),
                         Document::LogSource::Application, Document::LogLevel::Warning);
        }
    }

    out.infoMessages << QObject::tr("Aligned %1 of %2 layers onto '%3'.")
                            .arg(placed).arg(chain.size()).arg(doc.mesh(chain[0]).name);
    // The layers that registered, with the reference they were registered onto, as a visibility
    // set: one click in the layer panel shows what the run aligned and hides what it left behind.
    // Each run adds its own ("aligned", "aligned 2", ...). Untranslated, like a layer name.
    MeshFilterVisibilitySet alignedSet;
    alignedSet.name = QStringLiteral("aligned");
    for (size_t k = 0; k < placed; ++k)
        alignedSet.meshIndices.push_back(chain[k]);
    out.visibilitySets.push_back(alignedSet);
    if (!stoppedAt.isEmpty()) {
        out.infoMessages << QObject::tr(
            "Stopped at '%1': %2. It and the layers after it were not moved.")
                .arg(stoppedAt, stopReason);
    }
    progress.report(100, QObject::tr("Done"));
    progress.finish(true, out.infoMessages.isEmpty() ? QObject::tr("Aligned the layers.")
                                                     : out.infoMessages.back());
    return out;
}

} // namespace

MeshFilterRunResult runOpen3DRegistrationFilter(
    const QString &filterId,
    const FilterParams &params,
    Document &doc)
{
    if (filterId == QString::fromLatin1(kGlobal))
        return runGlobalAlignment(params, doc);
    if (filterId == QString::fromLatin1(kSequential))
        return runSequentialAlignment(params, doc);
    if (filterId == QString::fromLatin1(kFeatureSets))
        return runAlignSets(params, doc);
    // The layer that moves is the current one; the reference is the parameter.
    return runPairwise(filterId, params, doc, { doc.currentMeshIndex() });
}

bool isOpen3DRegistrationFilter(const QString &filterId)
{
    return filterId == QString::fromLatin1(kPointToPoint)
        || filterId == QString::fromLatin1(kPointToPlane)
        || filterId == QString::fromLatin1(kColored)
        || filterId == QString::fromLatin1(kFeature)
        || filterId == QString::fromLatin1(kFeatureSets)
        || filterId == QString::fromLatin1(kGlobal)
        || filterId == QString::fromLatin1(kSequential)
        || filterId == QString::fromLatin1(kFastGlobal);
}
