/*******************************************************************************
    Copyright (c) 2021, Andrea Maggiordomo, Paolo Cignoni and Marco Tarini

    This file is part of TextureDefrag, a reference implementation for
    the paper ``Texture Defragmentation for Photo-Reconstructed 3D Models''
    by Andrea Maggiordomo, Paolo Cignoni and Marco Tarini.

    TextureDefrag is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    TextureDefrag is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with TextureDefrag. If not, see <https://www.gnu.org/licenses/>.
*******************************************************************************/

#include "seam_remover.h"
#include "mesh.h"
#include "mesh_attribute.h"
#include "mesh_graph.h"
#include "matching.h"
#include "intersection.h"
#include "shell.h"
#include "arap.h"
#include "timer.h"
#include "logging.h"


#include <fstream>
#include <iomanip>
#include <unordered_set>

#include <vcg/complex/algorithms/clean.h>

constexpr double PENALTY_MULTIPLIER = 2.0;


struct Perf {
    double t_init;
    double t_seamdata;
    double t_alignmerge;
    double t_optimization_area;
    double t_optimize;
    double t_optimize_build;
    double t_optimize_arap;
    double t_check_before;
    double t_check_after;
    double t_accept;
    double t_reject;
    Timer timer;
};


static void InsertNewClusterInQueue(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph, const AlgoParameters& params);
static void MarkClusterDirty(ClusteredSeamHandle csh, double oldCost, CostInfo::MatchingValue oldMv, AlgoStateHandle state, GraphHandle graph);
static void RecomputeDirtyCluster(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph, const AlgoParameters& params);
static CostInfo ComputeCost(ClusteredSeamHandle csh, GraphHandle graph, const AlgoParameters& params, double penalty);
static inline double GetPenalty(ClusteredSeamHandle csh, AlgoStateHandle state);
static inline bool Valid(const WeightedSeam& ws, ConstAlgoStateHandle state);
static inline void PurgeQueue(AlgoStateHandle state);
static void ComputeSeamData(SeamData& sd, ClusteredSeamHandle csh, GraphHandle graph, AlgoStateHandle state);
static OffsetMap AlignAndMerge(ClusteredSeamHandle csh, SeamData& sd, const MatchingTransform& mi, const AlgoParameters& params);
static void ComputeOptimizationArea(SeamData& sd, Mesh& mesh, OffsetMap& om);
static std::unordered_set<Mesh::VertexPointer> ComputeVerticesWithinOffsetThreshold(Mesh& m, const OffsetMap& om, const SeamData& sd);
static CheckStatus CheckBoundaryAfterAlignment(SeamData& sd);
static CheckStatus CheckAfterLocalOptimization(SeamData& sd, AlgoStateHandle state, const AlgoParameters& params);
static CheckStatus OptimizeChart(SeamData& sd, GraphHandle graph, bool fixIntersectingEdges);
static void AcceptMove(const SeamData& sd, AlgoStateHandle state, GraphHandle graph, const AlgoParameters& params);
static void RejectMove(const SeamData& sd, AlgoStateHandle state, GraphHandle graph, CheckStatus status);
static void EraseSeam(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph);
static void InvalidateCluster(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph, CheckStatus status, double penaltyMultiplier);
static void RestoreChartAttributes(ChartHandle c, Mesh& m, std::vector<int>::const_iterator itvi,  std::vector<vcg::Point2d>::const_iterator ittc);
static CostInfo ReduceSeam(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph, const AlgoParameters& params);


Perf perf = {};

#define PERF_TIMER_RESET (perf = {}, perf.timer.Reset())
#define PERF_TIMER_START double perf_timer_t0 = perf.timer.TimeElapsed()
#define PERF_TIMER_ACCUMULATE(field) perf.field += perf.timer.TimeElapsed() - perf_timer_t0
#define PERF_TIMER_ACCUMULATE_FROM_PREVIOUS(field) perf.field += perf.timer.TimeSinceLastCheck()

//static int statsCheck[10] = {};
//static int feasibility[6] = {};

static std::vector<int> statsCheck(CheckStatus::_END, 0);
static std::vector<int> feasibility(CostInfo::MatchingValue::_END, 0);

static vcg::Color4b statusColor[] = {
    vcg::Color4b::White, // PASS=0,
    vcg::Color4b::Gray , // FAIL_LOCAL_OVERLAP,
    vcg::Color4b::Red, // FAIL_GLOBAL_OVERLAP_BEFORE,
    vcg::Color4b::Green, // FAIL_GLOBAL_OVERLAP_AFTER_OPT, // border of the optimization area self-intersects
    vcg::Color4b::LightGreen, // FAIL_GLOBAL_OVERLAP_AFTER_BND, // border of the optimzation area hit the fixed border
    vcg::Color4b::LightBlue, // FAIL_DISTORTION_LOCAL,
    vcg::Color4b::Blue, // FAIL_DISTORTION_LOCAL,
    vcg::Color4b::LightRed, // FAIL_TOPOLOGY
    vcg::Color4b::Yellow, // FAIL_NUMERICAL_ERROR
    vcg::Color4b::White, // UNKNOWN
    vcg::Color4b(176, 0, 255, 255) // FAIL_GLOBAL_OVERLAP_UNFIXABLE
};

static vcg::Color4b mvColor[] = {
    vcg::Color4b::White,    //  FEASIBLE=0,
    vcg::Color4b::Black,    //  ZERO_AREA,
    vcg::Color4b::Cyan,     //  UNFEASIBLE_BOUNDARY,
    vcg::Color4b::Magenta,  //  UNFEASIBLE_MATCHING,
    vcg::Color4b::Yellow    //  UV_THRESHOLD_EXCEEDED
};

static int accept = 0;
static int reject = 0;

static int num_retry = 0;
static int retry_success = 0;

// Lazy-recompute instrumentation
static long recompute_fused = 0; // re-clustered shared seams (charts adjacent to BOTH a and b)
static long recompute_infeasible = 0; // independent neighbors whose stale cost was already Infinity
static long recompute_visit = 0; // visit-components island-lookahead requeues
static long dirty_marks = 0; // neighbors marked dirty
static long dirty_recomputed = 0; // dirty clusters that were popped and recomputed
static double time_fused = 0;
static double time_infeasible = 0;
static double time_visit = 0;
static double time_dirty_paid = 0;

double mincost = 100000;
double maxcost = -1;

double min_energy = 10000000000;
double max_energy = 0;

/**
 * This function clears all global counters used throughout the Texture Defragmentation procedure.
 * The zeroes globals are:
 * - statsCheck: an array storing for each merge operation if it has been attempted
                successfully or if it failed (specifying why).
 * - feasibility: an array storing for each operation if it can be attempted, has been
 *                rejected, or it shouldn't be tried (and why).
 * - accept: track how many successful merges will be performed.
 * - reject: track how many failed merges will be performed.
 * - num_retry: counts the number of times we re-execute a failed merge operation.
 * - retry_success: counts the number of times the re-execution of a failed merge operation
 *                  leads to a success.
 */
static void ClearGlobals()
{
    // `statsCheck` stores for each merge operation if it has been attempted
    // successfully or if it failed (specifying why). This information is
    // represented as a `CheckStatus` value.
    //
    // We zero all entries to take into account that no operation has still been tried.
    for (unsigned i = 0; i < statsCheck.size(); ++i) {
        statsCheck[i] = 0;
    }

    // `feasibility` stores for each operation if it can be attempted, has
    // been rejected, or it shouldn't be tried (and why).
    //
    // This global array was populated by the latest call to `InsertNewClusterInQueue` and we zero it.
    //
    // ⚠️ Doesn't zero it set all entries to FEASIBLE, even ones deemed not? ⚠️
    for (unsigned i = 0; i < feasibility.size(); ++i)
        feasibility[i] = 0;

    // The global counters `accept` and `reject` track of how many successful merges will
    // be performed and how many will be canned.
    //
    // we zero them to take into account the current execution.
    accept = 0;
    reject = 0;

    // The global counters `num_retry` and `retry_success` count the total number of times we re-execute a
    // previously failed merge operation and the amount of re-evaluated merges that will finally succeed.
    //
    // The ratio `retry_success / num_retry` measures how effective the strategy of reevaluating failed merges is.
    //
    // We zero them to take into account the current execution.
    num_retry = 0;
    retry_success = 0;

    recompute_fused = 0;
    recompute_infeasible = 0;
    recompute_visit = 0;
    dirty_marks = 0;
    dirty_recomputed = 0;

    time_fused = 0;
    time_infeasible = 0;
    time_visit = 0;
    time_dirty_paid = 0;
}

/*!
 * Logs perfomance metrics regarding the current status of the Texture Defragmentation process.
 */
void LogExecutionStats()
{
    LOG_INFO    << "======== EXECUTION STATS ========";
    LOG_INFO    << "INIT       " << std::fixed << std::setprecision(3) << perf.t_init / perf.timer.TimeElapsed()                                << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_init << " secs";
    LOG_INFO    << "SEAM       " << std::fixed << std::setprecision(3) << perf.t_seamdata / perf.timer.TimeElapsed()                            << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_seamdata << " secs";
    LOG_INFO    << "MERGE      " << std::fixed << std::setprecision(3) << perf.t_alignmerge / perf.timer.TimeElapsed()                          << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_alignmerge << " secs";
    LOG_INFO    << "AREA OPT   " << std::fixed << std::setprecision(3) << perf.t_optimization_area / perf.timer.TimeElapsed()                   << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_optimization_area << " secs";
    LOG_INFO    << "OPTIMIZE   " << std::fixed << std::setprecision(3) << perf.t_optimize / perf.timer.TimeElapsed()                            << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_optimize << " secs";
    LOG_VERBOSE << "  BUILD    " << std::fixed << std::setprecision(3) << perf.t_optimize_build / perf.timer.TimeElapsed()                      << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_optimize_build << " secs";
    LOG_VERBOSE << "  ARAP     " << std::fixed << std::setprecision(3) << perf.t_optimize_arap / perf.timer.TimeElapsed()                       << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_optimize_arap << " secs";
    LOG_INFO    << "CHECK      " << std::fixed << std::setprecision(3) << (perf.t_check_before + perf.t_check_after) / perf.timer.TimeElapsed() << " , " << std::defaultfloat << std::setprecision(6)<< (perf.t_check_before + perf.t_check_after) << " secs";
    LOG_VERBOSE << "  BEFORE   " << std::fixed << std::setprecision(3) << perf.t_check_before / perf.timer.TimeElapsed()                        << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_check_before << " secs";
    LOG_VERBOSE << "  AFTER    " << std::fixed << std::setprecision(3) << perf.t_check_after / perf.timer.TimeElapsed()                         << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_check_after << " secs";
    LOG_INFO    << "ACCEPT     " << std::fixed << std::setprecision(3) << perf.t_accept / perf.timer.TimeElapsed()                              << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_accept << " secs";
    LOG_INFO    << "  count:                    " << accept;
    LOG_INFO    << "  with retry:               " << retry_success;
    LOG_INFO    << "  recompute fused:          " << recompute_fused << " , " << time_fused << " secs";
    LOG_INFO    << "  recompute infeasible:     " << recompute_infeasible << " , " << time_infeasible << " secs";
    LOG_INFO    << "  recompute visit:          " << recompute_visit << " , " << time_visit << " secs";
    LOG_INFO    << "  recompute deferred:       " << dirty_recomputed << " of " << dirty_marks << " , " << time_dirty_paid << " secs";
    LOG_VERBOSE << "  min energy:               " << min_energy;
    LOG_VERBOSE << "  max energy:               " << max_energy;
    LOG_INFO    << "REJECT     " << std::fixed << std::setprecision(3) << perf.t_reject / perf.timer.TimeElapsed()                              << " , " << std::defaultfloat << std::setprecision(6)<< perf.t_reject << " secs";
    LOG_INFO    << "  count:                    " << reject;
    LOG_INFO    << "  with retry:               " << num_retry - retry_success;
    LOG_VERBOSE << "  local overlaps            " << statsCheck[FAIL_LOCAL_OVERLAP];
    LOG_VERBOSE << "  global overlaps before    " << statsCheck[FAIL_GLOBAL_OVERLAP_BEFORE];
    LOG_VERBOSE << "  global overlaps after opt " << statsCheck[FAIL_GLOBAL_OVERLAP_AFTER_OPT];
    LOG_VERBOSE << "  global overlaps after bnd " << statsCheck[FAIL_GLOBAL_OVERLAP_AFTER_BND];
    LOG_VERBOSE << "  global overlaps unfixable " << statsCheck[FAIL_GLOBAL_OVERLAP_UNFIXABLE];
    LOG_VERBOSE << "  distortion (local)        " << statsCheck[FAIL_DISTORTION_LOCAL];
    LOG_VERBOSE << "  distortion (global)       " << statsCheck[FAIL_DISTORTION_GLOBAL];
    LOG_VERBOSE << "  topology                  " << statsCheck[FAIL_TOPOLOGY];
    LOG_VERBOSE << "  numerical error           " << statsCheck[FAIL_NUMERICAL_ERROR];
    LOG_VERBOSE << "    FEASIBILITY";
    LOG_VERBOSE << "      feasible              " << feasibility[CostInfo::FEASIBLE];
    LOG_VERBOSE << "      unfeasible boundary   " << feasibility[CostInfo::UNFEASIBLE_BOUNDARY];
    LOG_VERBOSE << "      unfeasible matching   " << feasibility[CostInfo::UNFEASIBLE_MATCHING];
    LOG_INFO    << "TOTAL      " << std::fixed << std::setprecision(3) << perf.timer.TimeElapsed() / perf.timer.TimeElapsed()          << " , " << std::defaultfloat << std::setprecision(6)<< perf.timer.TimeElapsed() << " secs";
    LOG_VERBOSE << "Minimum computed cost is " << mincost;
    LOG_VERBOSE << "Maximum computed cost is " << maxcost;
    LOG_INFO    << "===================================";
}

/*!
 * Prints the number of remaining seams, grouped by their processing and feasibility status.
 *
 * As an important side effect, for each seam still present in the parametrization, the
 * cost and feasibility are re-evaluated. This is necessary because the latest merge operation
 * may have altered the chart geometry and topology.
 * @param state: it contains all seams and metadata regarding the current execution of Texture Defragmentation.
 * @param graph: represents the current parametrization.
 * @param params: user-defined parameters for the current Texture Defragmentation.
 */
static void PrintStateInfo (
    AlgoStateHandle state,
    GraphHandle graph,
    const AlgoParameters& params) {

    // Recall that `state->chartSeamMap` maps each chart ID to the set of seams touching it.
    //
    // We scan the map to construct the data structure `moveSet`, containing all distinct seams still
    // present in the parametrization.
    std::set<ClusteredSeamHandle> moveSet;
    for (auto& entry : state->chartSeamMap) {
        for (auto csh : entry.second) {
            moveSet.insert(csh);
        }
    }

    LOG_VERBOSE << "Status of the residual " << moveSet.size() << " operations:";

    // We construct two counter arrays:
    // - `nstat`: counts seams by their processing status.
    // - `mstat`: counts seams by their feasibility category.
    //
    // We fill the arrays by scanning `moveSet`, incrementing the entries associated with
    // the current seam's processing and feasibility status.
    //
    // The cost of each seam is updated, since the latest merge operation could have changed them.
    //
    // ⚠️ Isn't it better if we update the cost for only the seams touching one of the latest merged charts? ⚠️
    int nstat[100] = {};
    int mstat[100] = {};
    for (auto csh : moveSet) {
        auto it = state->status.find(csh);
        ensure(it != state->status.end());
        ensure(it->second != PASS);
        CostInfo ci = ComputeCost(csh, graph, params, GetPenalty(csh, state));
        nstat[state->status[csh]]++;
        mstat[ci.mvalue]++;
    }

    // We print how many seams ended up in each processing and feasibility category.
    LOG_VERBOSE << "PASS                          " << nstat[CheckStatus::PASS];
    LOG_VERBOSE << "FAIL_LOCAL_OVERLAP            " << nstat[CheckStatus::FAIL_LOCAL_OVERLAP];
    LOG_VERBOSE << "FAIL_GLOBAL_OVERLAP_BEFORE    " << nstat[CheckStatus::FAIL_GLOBAL_OVERLAP_BEFORE];
    LOG_VERBOSE << "FAIL_GLOBAL_OVERLAP_AFTER_OPT " << nstat[CheckStatus::FAIL_GLOBAL_OVERLAP_AFTER_OPT];
    LOG_VERBOSE << "FAIL_GLOBAL_OVERLAP_AFTER_BND " << nstat[CheckStatus::FAIL_GLOBAL_OVERLAP_AFTER_BND];
    LOG_VERBOSE << "FAIL_GLOBAL_OVERLAP_UNFIXABLE " << nstat[CheckStatus::FAIL_GLOBAL_OVERLAP_UNFIXABLE];
    LOG_VERBOSE << "FAIL_DISTORTION_LOCAL         " << nstat[CheckStatus::FAIL_DISTORTION_LOCAL];
    LOG_VERBOSE << "FAIL_DISTORTION_GLOBAL        " << nstat[CheckStatus::FAIL_DISTORTION_GLOBAL];
    LOG_VERBOSE << "FAIL_TOPOLOGY                 " << nstat[CheckStatus::FAIL_TOPOLOGY];
    LOG_VERBOSE << "FAIL_NUMERICAL_ERROR          " << nstat[CheckStatus::FAIL_NUMERICAL_ERROR];
    LOG_VERBOSE << "UNKNOWN                       " << nstat[CheckStatus::UNKNOWN];
    LOG_VERBOSE << "  - FEASIBLE                         " << mstat[CostInfo::MatchingValue::FEASIBLE];
    LOG_VERBOSE << "  - ZERO_AREA                        " << mstat[CostInfo::MatchingValue::ZERO_AREA];
    LOG_VERBOSE << "  - UNFEASIBLE_BOUNDARY              " << mstat[CostInfo::MatchingValue::UNFEASIBLE_BOUNDARY];
    LOG_VERBOSE << "  - UNFEASIBLE_MATCHING              " << mstat[CostInfo::MatchingValue::UNFEASIBLE_MATCHING];
    LOG_VERBOSE << "  - REJECTED                         " << mstat[CostInfo::MatchingValue::REJECTED];

}

void PrepareMesh(Mesh& m, int *vndup)
{
    int dupVert = tri::Clean<Mesh>::RemoveDuplicateVertex(m);
    if (dupVert > 0)
        LOG_INFO << "Removed " << dupVert << " duplicate vertices";

    int zeroArea = tri::Clean<Mesh>::RemoveZeroAreaFace(m);
    if (zeroArea > 0)
        LOG_INFO << "Removed " << zeroArea << " zero area faces";

    tri::UpdateTopology<Mesh>::FaceFace(m);

    // orient faces coherently
    bool wasOriented, isOrientable;
    tri::Clean<Mesh>::OrientCoherentlyMesh(m, wasOriented, isOrientable);

    tri::UpdateTopology<Mesh>::FaceFace(m);

    int numRemovedFaces = tri::Clean<Mesh>::RemoveNonManifoldFace(m);
    if (numRemovedFaces > 0)
        LOG_INFO << "Removed " << numRemovedFaces << " non-manifold faces";

    tri::Allocator<Mesh>::CompactEveryVector(m);
    tri::UpdateTopology<Mesh>::FaceFace(m);

    Compute3DFaceAdjacencyAttribute(m);

    CutAlongSeams(m);
    tri::Allocator<Mesh>::CompactEveryVector(m);

    *vndup = m.VN();

    tri::UpdateTopology<Mesh>::FaceFace(m);
    while (tri::Clean<Mesh>::SplitNonManifoldVertex(m, 0))
        ;
    tri::UpdateTopology<Mesh>::VertexFace(m);

    tri::Allocator<Mesh>::CompactEveryVector(m);
}

AlgoStateHandle InitializeState(GraphHandle graph, const AlgoParameters& algoParameters)
{

    // We measure the execution time taken by each call to `InitializeState` for performance reasons.
    PERF_TIMER_RESET;
    PERF_TIMER_START;


    // We create the AlgoStateHandle instance, denominated as `state`, which will keep any
    // useful information regarding the current progress of the Texture Defragmentation algorithm.
    //
    //
    // We compute the As-Rigid-As-Possible (ARAP for short) energy of the input parametrization.
    // The result describes the amount of distortion present before any merge happens.
    // The energy is composed of a numerator and a denominator, which are stored separately
    // in `state->arapNum` and `state->arapDenom` respectively.
    //
    // These starting values are used to measure the amount of distortion introduced by
    // each merge operation.
    AlgoStateHandle state = std::make_shared<AlgoState>();
    ARAP::ComputeEnergyFromStoredWedgeTC(graph->mesh, &state->arapNum, &state->arapDenom);
    state->inputUVBorderLength = 0;
    state->currentUVBorderLength = 0;

    // We fill the field `state->sm` with all seam edges of the parametrization.
    // Each edge stores the two faces incident to it (declared as `fa` and `fb`) and
    // its indices in the triangles (declared as `ea`, `eb`).
    BuildSeamMesh(graph->mesh, state->sm, graph);
    std::vector<SeamHandle> seams = GenerateSeams(state->sm);

    // We update `state->sm`, grouping seams into connected components that
    // consistently separate the same pair of charts. Note that there could be
    // multiple connected components for the same pair of charts.
    //
    // Each group is constructed as a SeamHandle instance containing the ordered
    // list of edges forming it and the endpoints of the chain.
    //
    // From now on we refer to a connected component as a seam, while edges are its
    // primitives members.
    std::vector<ClusteredSeamHandle> cshvec = ClusterSeamsByChartId(seams);

    // We update `state->sm` by grouping together seam chains sharing the same pair of charts.
    // Each set is stored as a single ClusteredSeamHandle, one per pair.
    //
    // While scanning the connected components, we count the number of self-cut seams
    // (i.e., those spawning a single chart) and distinct seams (i.e., those spawning two distinct charts).
    int ndisconnecting = 0;
    int nself = 0;
    for (auto csh : cshvec) {
        ChartPair charts = GetCharts(csh, graph);
        if (charts.first == charts.second) {
            nself++;
        }
        else {
            ndisconnecting++;
        }
        InsertNewClusterInQueue(csh, state, graph, algoParameters);
    }
    LOG_INFO << "Found " << ndisconnecting << " disconnecting seams";
    LOG_INFO << "Found " << nself << " non-disconnecting seams";

    // sanity check
    //for (auto& entry : state->chartSeamMap) {
    //    LOG_INFO << entry.first;
    //    ensure(entry.second.size() >= (graph->GetChart(entry.first)->adj.size()));
    //}

    // We accumulate the total UV boundary length across all charts of the parametrization.
    //
    // In `state->inputUVBorderLength` we keep the initial boundary length, while `state->currentUVBorderLength`
    // stores the current boundary, reduced each time we apply a merge operation.
    // The two fields determine how much boundary will be trimmed by the Texture Defragmentation.
    for (const auto& ch : graph->charts) {
        state->inputUVBorderLength += ch.second->BorderUV();
        state->currentUVBorderLength += ch.second->BorderUV();
    }

    // Stop measuring execution time for the current function.
    PERF_TIMER_ACCUMULATE(t_init);

    return state;
}

void GreedyOptimization(GraphHandle graph, AlgoStateHandle state, const AlgoParameters& params)
{
    // To initialize the greedy procedure, we reset all the statistical counter used by
    // the algorithm (`feasibility`, `accept`, `reject`, `num_retry`, ...) to avoid
    // having outdated values from previous executions.
    //
    // We log the initial state of all seams (i.e.their feasibility and status
    // distribution) as a baseline, via PrintStateInfo.
    //
    // The global ARAP energy of the entire atlas is logged as the pre-optimization
    // reference point. This is used as a snapshot for comparing it against our improvements.
    ClearGlobals();
    Timer timer;
    PrintStateInfo(state, graph, params);
    LOG_INFO << "Atlas energy before optimization is " << ARAP::ComputeEnergyFromStoredWedgeTC(graph->mesh, nullptr, nullptr);

    // `k` is a counter keeping track of the total number of executed iterations.
    int k = 0;
    while (state->queue.size() > 0) {

        // Recall that our greedy algorithm depends on a priority queue data structure
        // ordering merge operations from most convenient to least. When a merge
        // operation is applied, we need to re-evaluate operations which involved at
        // least one of the now merged charts. For efficiency, rather than search for
        // each of them, change their cost, and reorder the heap, we push a new entry
        // containing the updated values into the data structure. This means that the
        // heap could contain useless entries. We keep track of the number of valid
        // entries within the queue in `state->cost`.
        //
        // When the number of total entries in the queue exceeds a certain threshold
        // size, we rebuild the priority queue, clearing it from all useless members.
        if (state->queue.size() > 5 * state->cost.size())
            PurgeQueue(state);

        // ======== TERMINATION CONDITIONS FOR THE GREEDY STRAT ========
        // At the start of each iteration we check if the algorithm can be stopped.
        // We define three termination criteria:
        //
        //  * There are no more merge operations to consider (i.e., the priority
        //    queue is empty).
        //
        //  * The maximum execution time limit has elapsed.
        //
        //  * The ratio between the current UV border length and the starting UV
        //    border length quantifies how much the UV parametrization has been
        //    reduced by our optimizations.
        //    If this value is gone below a user-defined threshold, our target
        //    defragmentation has been achieved and the algorithm can stop.
        //
        //    In a Small Islands Remove execution, we aim to remove as many
        //    small islands as possible. So this last termination condition
        //    is not considered.
        if (state->queue.size() == 0) {
            LOG_INFO << "Queue is empty, interrupting.";
            break;
        }

        if (params.timelimit > 0 && timer.TimeElapsed() > params.timelimit) {
            LOG_INFO << "Timelimit hit, interrupting.";
            break;
        }

        if (params.UVBorderLengthReduction > (state->currentUVBorderLength / state->inputUVBorderLength)) {
            LOG_INFO << "Target UV border reduction reached, interrupting.";
            break;
        }

        // We pick the most convenient merge operation by popping the top of the priority queue.
        // A merge operation is identified by its chart operands, denoted as chart A and chart B.
        // We need to be sure that the selected merge operation is not outdated (via `Valid`).
        // If it is valid but has infinite cost, then we need to ensure that all remaining
        // seams are also infinite; otherwise the heap has encountered a bug.
        WeightedSeam ws = state->queue.top();
        state->queue.pop();
        if (Valid(ws, state)) {
            if (state->dirty.count(ws.first)) {
                // Stale cluster, recompute and re-queue
                RecomputeDirtyCluster(ws.first, state, graph, params);
                continue;
            }
            if (ws.second == Infinity()) {
                // sanity check
                for (auto& entry : state->cost)
                    ensure(entry.second == Infinity());
                LOG_INFO << "Queue is empty, interrupting.";
                break;
            } else {
                ++k;

                // Every 200 iterations, execution statistics are logged.
                if ((k % 200) == 0) {
                    LOG_INFO << "Logging execution stats after " << k << " iterations";
                    LogExecutionStats();
                }

                // QMeshLab: eligibility is re-checked here, at the point of action, and
                // not only where the cost was computed. Costs are worked out up front, so
                // a pair that was eligible when it entered the queue can come up for
                // execution after its selected island has already been dissolved -- and
                // executing it anyway is how a hand-picked merge quietly pulls in
                // neighbours nobody asked for. The pair is dropped rather than requeued:
                // it is not going to become eligible again.
                if (params.mergeSelectedIslands) {
                    ChartPair pair = GetCharts(ws.first, graph);
                    if (!pair.first->AnySelectedFace() && !pair.second->AnySelectedFace()) {
                        // Retired rather than merely skipped: the loop stops when the top
                        // of the queue is infinite, so dropping the seam without recording
                        // that cost leaves it churning through stale finite entries.
                        state->cost[ws.first] = Infinity();
                        LOG_DEBUG << "Retired operation: neither chart is selected";
                        continue;
                    }
                }

                // We prepare for the merge operation by keeping a snapshot of the UV
                // parametrization before the current merge operation. All backed-up
                // information is stored within the SeamData instance `sd`. If the merge
                // is rejected, we use `sd` to roll back to the previous state.
                SeamData sd;
                ComputeSeamData(sd, ws.first, graph, state);

                LOG_DEBUG << "  Chart ids are " << sd.a->id << " " << sd.b->id << " (areas = " << sd.a->AreaUV() << ", " << sd.b->AreaUV() << ")";

                // We apply an initial alignment of chart B into chart A. The merge
                // is then executed by collapsing seam vertices, update FACE-FACE and
                // VERTEX-FACE topology, and compute the UV displacement offsets (i.e.,
                // a map associating for each collapsed vertex the maximum displacement
                // introduced by our procedure). This map is stored in the variable `om`.
                OffsetMap om = AlignAndMerge(ws.first, sd, state->transform[ws.first], params);

                // We use the displacement offsets in `om` to identify which faces need
                // an As-Rigid-As-Possible (ARAP) optimization (i.e., needs to be moved
                // to decrease their distortion). For each of them we back up their
                // current UV coordinates and measure the pre-optimization folded area
                // ratio.
                ComputeOptimizationArea(sd, graph->mesh, om);

                // Before ARAP optimization, we check that the fixed region (i.e., the area
                // not to optimize) of the two charts doesn't overlap after alignment. When
                // an overlap is found, then the mesh is immediately rejected with the status
                // `FAIL_GLOBAL_OVERLAP_BEFORE`.
                //
                // When in Small Islands Remover the distortionMode is set to UNSAFE, we skip
                // this check.
                CheckStatus status = PASS;
                if (!params.skipOverlapChecks) {
                    status = (sd.a != sd.b) ? CheckBoundaryAfterAlignment(sd) : PASS;
                }

                // We run the ARAP solver onto the optimization area. The ARAP algorithm will
                // try to find the best UV coordinates that minimize distortion while
                // respecting the boundary constraints. Within the current merge operation
                // this is our first call to `OptimizeChart`, meaning there are no constraints
                // for the ARAP solver to take into account. We tell it to the algorithm by
                // setting the parameter `fixIntersectingEdges` to false.
                //
                // The ARAP solver will return a status, indicating if it succeeded or failed.
                if (status == PASS) {
                    status = OptimizeChart(sd, graph, false);
                }

                // If the ARAP solver succeeded, we then check that the proposed optimized
                // parametrization provides acceptable distortions both globally and locally
                // and that it has introduced no new overlaps.
                //
                // If not satisfied, each check returns a distinct failure status.
                //
                // When in Small Islands Remover the distortionMode is set to UNSAFE, we skip
                // this check.
                if (!params.skipOverlapChecks) {
                    if (status == PASS)
                        status = CheckAfterLocalOptimization(sd, state, params);
                }

                // Within the possible failed states, two particular cases can be fixed:
                //
                //  * The optimized boundary presents some folds over itself.
                //
                //  * The optimized boundary overlaps in some places the outer boundary
                //    of the fixed region.
                //
                // Both can be avoided by retrying the ARAP optimization, adding as
                // constraints that the vertices of the intersecting edges must stay fixed.
                //
                // The check is then repeated until either all overlaps are resolved or no
                // new vertices could be pinned. This last case is associated to the special
                // status `_END`.
                //
                // When in Small Islands Remover the distortionMode is set to UNSAFE, we skip
                // this check.
                if (!params.skipOverlapChecks) {
                    while (status == FAIL_GLOBAL_OVERLAP_AFTER_OPT || status == FAIL_GLOBAL_OVERLAP_AFTER_BND) {
                        LOG_DEBUG << "Global overlaps detected after ARAP optimization, fixing edges";
                        CheckStatus iterStatus = OptimizeChart(sd, graph, true);
                        if (iterStatus == _END)
                            break;
                        else
                            status = CheckAfterLocalOptimization(sd, state, params);
                    }
                }

                // The current status is the final one for the merge operation.
                //
                // `statsCheck` is a set of counters, keeping track for each
                // status how many times it has been reached by the merge operations.
                //
                // If the merge has succeeded, we can finally commit the merge: we
                // update the global ARAP energy, remove all merged seams from the
                // queue, set the new UV border length, and re-evaluate all neighboring
                // merge operations. Additionally, the merged boundary is colorized
                // orange as a visual sign of a successful merge operation.
                //
                // If the merge failed, we return to the UV coordinates before the
                // mesh, restore the FACE-FACE and VERTEX_FACE topologies, and
                // record the failure in order to update its penalty cost.
                statsCheck[status]++;
                if (status == PASS) {
                    AcceptMove(sd, state, graph, params);
                    // QMeshLab: see FaceGroup::ClearFaceSelection. Only once the merge is
                    // committed -- the checks above can still reject it, and a rejected
                    // merge must leave the island eligible for another neighbour.
                    if (params.mergeSelectedIslands) {
                        sd.a->ClearFaceSelection();
                        sd.b->ClearFaceSelection();
                    }
                    ColorizeSeam(sd.csh, vcg::Color4b(255, 69, 0, 255));
                    accept++;
                    LOG_DEBUG << "Accepted operation";
                } else {
                    RejectMove(sd, state, graph, status);
                    reject++;
                    LOG_DEBUG << "Rejected operation";
                }
            }
        }
    }

    // The parametrization final state is logged showing how many seams
    // were processed, how many succeeded, how many failed, and what
    // the final feasibility distribution looks like.
    //
    // The global ARAP energy is measured again, giving an idea of the
    // total distortion reduction achieved.
    PrintStateInfo(state, graph, params);
    LogExecutionStats();
    LOG_INFO << "Atlas energy after optimization is " << ARAP::ComputeEnergyFromStoredWedgeTC(graph->mesh, nullptr, nullptr);
}

void Finalize(GraphHandle graph, int *vndup) {

    // Recall that at the beginning of the Texture Defragmentation procedure, for
    // individualizing and fixing the seams present on the mesh, we duplicated the
    // vertices such that each chart had its own unique copy.
    // Since we are returning the optimized final mesh, these extra vertices
    // are not needed anymore and must be removed.
    //
    // We copy all vertices in the final mesh into the set `vset` (the duplicates are
    // present). The current number of vertices is written into the counter
    // `vndup`
    //
    // First, we remove all duplicate vertices via `RemoveDuplicateVertex`. The procedure
    // consists in recognizing vertices whose 3D positions coincide, collapsing them into
    // one.
    //
    // We then clean up the mesh data structures by marking all vertices no longer
    // referenced by any face as `DELETED`.
    //
    // Finally, the VERTEX-FACE adjacency topology is rebuilt from scratch, since
    // the available vertices have changed drastically.
    std::unordered_set<Mesh::ConstVertexPointer> vset;
    for (const MeshFace& f : graph->mesh.face)
        for (int i = 0; i < 3; ++i)
            vset.insert(f.cV(i));

    *vndup = (int) vset.size();

    tri::Clean<Mesh>::RemoveDuplicateVertex(graph->mesh);
    tri::Clean<Mesh>::RemoveUnreferencedVertex(graph->mesh);
    tri::UpdateTopology<Mesh>::VertexFace(graph->mesh);
}

// -- static functions ---------------------------------------------------------

/*!
 * Given an unvisited seam, updates the Texture Defragmentation algorithm state, making it available for processing.
 *
 * @param csh: the seam to register.
 * @param state: the current state of the Texture Defragmentation procedure. It holds the priority queue
 *               and all auxiliary lookup structures.
 * @param graph: the parametrization graph.
 * @param params: the user-defined parameters for the defragmentation run.
 */
static void InsertNewClusterInQueue(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph, const AlgoParameters& params)
{
    // We color all faces adjacent to the seam in white.
    // This is done as a preliminary step just to
    // visualize the faces which will be evaluated
    // in this algorithm.
    ColorizeSeam(csh, vcg::Color4b::White);

    // Compute the cost of merging the two charts separated by the seam.
    //
    // The returned CostInfo instance carries three pieces of information:
    // - `cost`: a finite scalar quantifying how much the operation is feasible.
    // - `mvalue`: the feasibility status (FEASIBLE, UNFEASIBLE_BOUNDARY, UNFEASIBLE_MATCHING, or ZERO_AREA), describing
    //              why the operation is accepted or rejected.
    // - `matching`: the rigid transformation to apply if this merge is eventually selected.
    CostInfo ci = ComputeCost(csh, graph, params, GetPenalty(csh, state));

    // When `ComputeCost` determines that the two charts' UV boundaries are too geometrically
    // incompatible to align under a rigid transformation (`UNFEASIBLE_MATCHING`), and the
    // reduce flag is set, we iteratively trim the seam until a feasible matching is found.
    //
    // Each call to `ReduceSeam` modifies `csh` in place, updating the entry with the computed
    // shorter prefix or suffix. The cost is also updated.
    //
    // The loop continues until the result is deemed acceptable.
    //
    // Note that `UNFEASIBLE_BOUNDARY` and `ZERO_AREA` are not handled: reduction only addresses
    // geometric incompatibility.
    if (params.reduce) {
        while (ci.mvalue == CostInfo::UNFEASIBLE_MATCHING) {
            ci = ReduceSeam(csh, state, graph, params);
        }
    }

    // Replace the provisional white color (assigned at the start of this function) with the
    // final color corresponding to the seam's feasibility status (mvalue). Each category
    // (FEASIBLE, ZERO_AREA, UNFEASIBLE_BOUNDARY, UNFEASIBLE_MATCHING) maps to a distinct
    // color in `mvColor`, giving the user a visual overview of the feasibility landscape
    // directly in the MeshLab viewport.
    ColorizeSeam(csh, mvColor[ci.mvalue]);

    // We now replace the color of adjacent faces with one corresponding to the seam's feasibility status.
    // The colors are:
    // - White for FEASIBLE.
    // - Black for ZERO_AREA.
    // - Cyan for UNFEASIBLE_BOUNDARY.
    // - Magenta for UNFEASIBLE_MATCHING.
    //
    // We do it to provide a visual overview for the computed costs.
    ColorizeSeam(csh, mvColor[ci.mvalue]);

    // `feasibility` is a global array of counters, indexed by `maalue category (FEASIBLE,
    // ZERO_AREA, UNFEASIBLE_BOUNDARY, UNFEASIBLE_MATCHING). Each entry contains the number
    // of seams belonging each class.
    feasibility[ci.mvalue]++;

    // Update the global minimum cost and maximum cost with the value of the current seam. Obviously, seams whose cost
    // is set to `Infinity()` are excluded, since they can make `maxcost` meaningless (i.e., set to infinite).
    if (ci.cost != Infinity()) {
        mincost = std::min(mincost, ci.cost);
        maxcost = std::max(maxcost, ci.cost);
    }

    // We update the priority queue (i.e., a min-heap ordered by cost), with the current seam.
    //
    // Each entry of the data structure contains the following four fields:
    // - `cost`: kept here for a quick look-up.
    // - `transform`: the rigid transformation to apply if the merge operation is selected.
    // - `status`: the processing state of this seam. It is always initialized to `UNKNOWN`, then
    //             it could be updated to either `DONE` or `SKIPPED` by the Texture Defragmentation.
    // - `mvalue`: the feasibility category. Used to decide if the seam should be attempted after
    //             picking it from the queue.
    state->queue.push(std::make_pair(csh, ci.cost));
    state->cost[csh] = ci.cost;
    state->transform[csh] = ci.matching;
    state->status[csh] = UNKNOWN;
    state->mvalue[csh] = ci.mvalue;

    // We update the `chartSeamMap` with the current seam. The data structure maps each chart ID to the
    // set of clustered seams touching it.
    //
    // This allows easily detecting which seams need to be recomputed after a chart has been merged.
    ChartPair p = GetCharts(csh, graph);
    state->chartSeamMap[p.first->id].insert(csh);
    state->chartSeamMap[p.second->id].insert(csh);

    // We update `emap` with the current seam's endpoints. The data structure maps each
    // seamMesh vertex index to the set of seams that have that vertex as an endpoint.
    //
    // It is used to detect topological conflicts: if two seams share an endpoint
    // vertex, merging one of them may invalidate the other.
    std::set<int> endpoints = GetEndpoints(csh);
    for (auto vi : endpoints)
        state->emap[vi].insert(csh);
}

/*!
 * Lazy variant of `InsertNewClusterInQueue`: re-registers a seam whose adjacent charts changed after a merge,
 * without recomputing its cost. The cost is computed by `RecomputeDirtyCluster` only if the seam ever reaches
 * the top of the queue, so a seam that is dirtied many times, or never popped, pays `ComputeCost` at most once.
 *
 * @param csh: the seam to register.
 * @param oldCost: the seam's cost before the merge, used as a stale priority.
 * @param oldMv: the seam's feasibility status before the merge.
 * @param state: the current state of the Texture Defragmentation procedure. It holds the priority queue
 *               and all auxiliary lookup structures.
 * @param graph: the parametrization graph.
 */
static void MarkClusterDirty(ClusteredSeamHandle csh, double oldCost, CostInfo::MatchingValue oldMv, AlgoStateHandle state, GraphHandle graph)
{
    // We color all faces adjacent to the seam in white, as `InsertNewClusterInQueue` does
    // before evaluating a seam. The final color is assigned by `RecomputeDirtyCluster`.
    ColorizeSeam(csh, vcg::Color4b::White);

    // We push the seam in the priority queue with its stale pre-merge cost, so that the queue
    // ordering still approximates the greedy one.
    //
    // The `transform` is left as identity: a dirty seam is always recomputed before it can be
    // evaluated, so the value is never read.
    state->queue.push(std::make_pair(csh, oldCost));
    state->cost[csh] = oldCost;
    state->transform[csh] = MatchingTransform::Identity();
    state->status[csh] = UNKNOWN;
    state->mvalue[csh] = oldMv;

    // We update `chartSeamMap` and `emap` exactly as `InsertNewClusterInQueue` does, since the
    // membership of the seam does not depend on its cost.
    ChartPair p = GetCharts(csh, graph);
    state->chartSeamMap[p.first->id].insert(csh);
    state->chartSeamMap[p.second->id].insert(csh);

    std::set<int> endpoints = GetEndpoints(csh);
    for (auto vi : endpoints)
        state->emap[vi].insert(csh);

    // Finally, we flag the seam as dirty, so that the main loop recomputes it when popped.
    state->dirty.insert(csh);
    dirty_marks++;
}

/*!
 * Pays the deferred `ComputeCost` for a dirty seam that reached the top of the queue, and re-pushes it with
 * its true cost.
 *
 * The caller must not act on the pop that triggered this call: it re-enters the loop, so that the refreshed
 * entry competes for the top on equal footing with the others.
 *
 * @param csh: the dirty seam to recompute.
 * @param state: the current state of the Texture Defragmentation procedure. It holds the priority queue
 *               and all auxiliary lookup structures.
 * @param graph: the parametrization graph.
 * @param params: the user-defined parameters for the defragmentation run.
 */
static void RecomputeDirtyCluster(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph, const AlgoParameters& params)
{
    double t_d = perf.timer.TimeElapsed();
    state->dirty.erase(csh);

    // We compute the cost of merging the two charts separated by the seam, trimming it
    // via `ReduceSeam` when the matching is unfeasible, as in `InsertNewClusterInQueue`.
    CostInfo ci = ComputeCost(csh, graph, params, GetPenalty(csh, state));

    if (params.reduce) {
        while (ci.mvalue == CostInfo::UNFEASIBLE_MATCHING) {
            ci = ReduceSeam(csh, state, graph, params);
        }
    }

    // We replace the provisional white color assigned by `MarkClusterDirty` with the
    // color corresponding to the seam's feasibility status, and update the statistics.
    ColorizeSeam(csh, mvColor[ci.mvalue]);

    feasibility[ci.mvalue]++;

    if (ci.cost != Infinity()) {
        mincost = std::min(mincost, ci.cost);
        maxcost = std::max(maxcost, ci.cost);
    }

    // We push the seam in the priority queue with its true cost. The stale entry was already
    // popped by the caller.
    //
    // `chartSeamMap` and `emap` were already updated by `MarkClusterDirty`, and `ComputeCost`
    // does not modify the charts, so only the per-seam values need to be refreshed.
    state->queue.push(std::make_pair(csh, ci.cost));
    state->cost[csh] = ci.cost;
    state->transform[csh] = ci.matching;
    state->status[csh] = UNKNOWN;
    state->mvalue[csh] = ci.mvalue;

    dirty_recomputed++;
    time_dirty_paid += perf.timer.TimeElapsed() - t_d;
}

// this function returns true if there exists a sequence of at most maxSteps operations
// that results in a UV-island being formed in the graph AND involves a and b
static bool IslandLookahead(ChartHandle a, ChartHandle b, int maxSteps)
{
    // if there are too many candidates, exit early
    if (a->adj.size() > (unsigned) maxSteps || b->adj.size() > (unsigned) maxSteps)
        return false;

    std::unordered_set<ChartHandle> nab;
    nab.insert(a->adj.begin(), a->adj.end());
    nab.insert(b->adj.begin(), b->adj.end());

    for (auto c : nab) {
        std::stack<ChartHandle> s;
        s.push(a);

        std::set<ChartHandle> visited = {c, a}; // prevent the visit from reaching c
        int steps = 0;

        // start the visit
        while (!s.empty()) {
            ChartHandle sc = s.top();
            s.pop();
            visited.insert(sc);
            for (auto ac : sc->adj) {
                if (visited.find(ac) == visited.end()) {
                    s.push(ac);
                    steps++;
                }
                if (steps > maxSteps)
                    break;
            }
            if (steps > maxSteps)
                break;
        }

        // if the stack is empty we could not advance the visit
        // this means that the visited component would only be adjacent to c
        //   => the visited component is an island whose only adjacency is c
        if (s.empty())
            return true;
    }

    return false;
}

/*!
 * Given a potential merge operation, represented as a cluster of seams sharing the same pair of charts, it computes
 * a cost describing how convenient is to merge them.
 *
 * A merge is considered never convenient in three cases:
 * - either chart has zero area in UV or 3D space (ZERO_AREA);
 * - the seam covers too small a fraction of either chart's UV boundary (UNFEASIBLE_BOUNDARY);
 * - the UV boundaries of the two charts are too geometrically incompatible to align under
 *   a rigid transformation within the allowed error threshold (UNFEASIBLE_MATCHING).
 *
 * The cost is proportional to the alignment error of the best-fit rigid transformation between the two UV boundaries,
 * penalized by how little of each chart's boundary the seam covers, and rewarded by the area of the smaller chart.
 *
 * Some particular properties regarding the cost can depend on the filter's used.
 *
 * FP_SMALL_ISLANDS_REMOVER sets all merge operations employing two charts having UV areas major or equal
 * than the minAreaThreshold to plus infinity.
 *
 * @param csh: the cluster of seams.
 * @param graph: the parametrization.
 * @param params: the parameters set by the user for the current texture defragmentation run.
 * @param penalty: a weight that inflates the score of merge operations that have already been attempted and failed.
 *
 * @return a CostInfo struct containing: the computed cost (finite if feasible, infinite otherwise),
 *         the feasibility status (mvalue), and the best-fit rigid matching transform to apply if this merge is selected
 *         for processing.
 */
static CostInfo ComputeCost (
    ClusteredSeamHandle csh,
    GraphHandle graph,
    const AlgoParameters& params,
    double penalty)
{

    // Retrieves the two charts associated with the cluster.
    // The swapped flag tells us if the pair was returned in reversed order relative to how the seam stores them.
    // The order matters, since it tells us how to interpret which UV coordinates belong to which chart downstream.
    bool swapped;
    ChartPair charts = GetCharts(csh, graph, &swapped);
    ChartHandle a = charts.first;
    ChartHandle b = charts.second;


    // ============ INVALIDATING MERGE OPERATION ===========
    // We check if the merge operation should be skipped, assigning to it an infinite cost.
    // Depending on the variation of Texture Defragmentation being executed, the condition
    // can change. The motivation is reflected in the CostInfo status.
    //
    // Universal (all variants), if one of the charts has zero area in UV or 3D space, the
    // merge is meaningless and immediately rejected with CostInfo::ZERO_AREA.
    //
    // FP_SMALL_CHART_REMOVER: if both charts have UV area > maxAreaThreshold, neither
    // qualifies as a small chart, and the merge is rejected with CostInfo::UV_THRESHOLD_EXCEEDED.
    // The check is skipped when maxAreaThreshold <= 0 (i.e., no area restriction).
    const bool zeroArea = a->AreaUV() == 0 || b->AreaUV() == 0 ||
                          a->Area3D() == 0 || b->Area3D() == 0;
    if (zeroArea) {
        return { Infinity(), {}, CostInfo::ZERO_AREA };

    }
    // ========= VARIANT USING AVERAGE AREA =========
    // const bool smallIslandCond =  params.filterType == FilterTextureDefragPlugin::FP_SMALL_ISLANDS_REMOVER  &&
    //                           params.maxThreshold > 0                                                       &&
    //                           a->AreaUV() > params.maxThreshold                                             &&
    //                           b->AreaUV() > params.maxThreshold;
    // ========= VARIANT USING MEDIAN BORDER =========

    ///////////////////// CHANGE FILTERTYPE AFTER SETTING UP AN ENUM FOR THE VARIANT OF TEXTURE DEFRAG /////////////////
    // ========= QMESHLAB VARIANT USING THE FACE SELECTION =========
    // Same shape as the size rule below it: reject only when *neither* chart qualifies.
    const bool selectionCond   =  params.filterType == FilterType::SmallIslandRemover                       &&
                                  params.mergeSelectedIslands                                              &&
                                  !a->AnySelectedFace()                                                    &&
                                  !b->AnySelectedFace();

    const bool smallIslandCond =  params.filterType == FilterType::SmallIslandRemover                       &&
                                  !params.mergeSelectedIslands                                             &&
                                  params.maxThreshold > 0                                                   &&
                                  a->BorderUV() > params.maxThreshold                                       &&
                                  b->BorderUV() > params.maxThreshold;

    if (selectionCond || smallIslandCond) {
        return { Infinity(), {}, CostInfo::OVER_UV_AREA };
    }

    // While computing the final cost, some conditions could immediately reject the merge operation.
    // Most reasons involve that the merge could introduce too much distortion to even try doing it.
    //
    // When executing Small Islands Remover with distortionMode set to `LOOSE`, we skip any check
    // regarding the distortion introduced.

    // We construct two parallel arrays, each containing the UV positions of the seam vertices as seen from the two
    // charts' sides:
    // - `bpa` contains the UV positions of the seam vertices as seen from chart A's side.
    // - `bpb` contains the same vertices as seen from chart B's side.
    // Since each seam edge has two UV-space representations (one per chart), these two arrays capture the same
    // geometric seam boundary in two different UV spaces.
    std::vector<vcg::Point2d> bpa;
    std::vector<vcg::Point2d> bpb;
    ExtractUVCoordinates(csh, bpa, bpb, {a->id});

    // Compute the best-fit rigid transformation matrix (i.e., a roto-translation matrix) that aligns bpa onto bpb.
    //
    // Note that, for self-cut seams (a == b), no matching is needed (we set `mi` to the identity matrix).
    MatchingTransform mi = MatchingTransform::Identity();
    if (a != b) {
        mi = ComputeMatchingRigidMatrix(bpa, bpb);
    }

    // We build a map data structure, referred to as `bmap`, accumulating for each chart the total UV space length of
    // the seam boundary as seen from that chart's side. it is computed by summing the UV-edge lengths over all seam
    // edges on said sides.
    //
    // Note the two sides measure the same 3D edges but in their respective UV spaces, meaning the entries between the
    // two charts can differ if the parametrization is distorted.
    //
    // We also accumulate within `seamLength3D` the actual 3D length, which is computed for debug reason.
    std::map<RegionID, double> bmap;
    double seamLength3D = 0;
    int ne = 0;
    SeamMesh& seamMesh = csh->sm;
    for (SeamHandle sh : csh->seams) {
        for (int iedge : sh->edges) {
            SeamEdge& edge = seamMesh.edge[iedge];
            bmap[edge.fa->id] += (edge.fa->V0(edge.ea)->T().P() - edge.fa->V1(edge.ea)->T().P()).Norm();
            bmap[edge.fb->id] += (edge.fb->V0(edge.eb)->T().P() - edge.fb->V1(edge.eb)->T().P()).Norm();
            seamLength3D += (edge.fa->P0(edge.ea) - edge.fa->P1(edge.ea)).Norm();
            ne++;
        }
    }

    CostInfo ci;
    ci.matching = mi;
    ci.mvalue = CostInfo::FEASIBLE;

    // In Texture Defragmentation we measure how large the seam is compared to the total UV boundary of each chart.
    // It is computed as the maximum between the two ratios (one per chart). If this ratio is below our boundary
    // tolerance (passed as a parameter), the seam is too small to make the merge convenient.
    // If so, we mark the seam as UNFEASIBLE_BOUNDARY, telling the algorithm to skip it.
    //
    // In Small Islands Remover the aim of the filter is to remove small islands, so we should consider them always
    // convenient. For this reason we skip this check entirely.
    if (a != b) {
        double maxSeamToBoundaryRatio = std::max(bmap[a->id] / a->BorderUV(), bmap[b->id] / b->BorderUV());
        if (maxSeamToBoundaryRatio < params.boundaryTolerance && (!params.visitComponents || !IslandLookahead(a, b, 5))) {
            ci.cost = Infinity();
            ci.mvalue = CostInfo::UNFEASIBLE_BOUNDARY;
            return ci;
        }
    }

    // We measure how well our roto-translation matrix actually aligns bpa onto bpb, computing the total residual
    // error after applying the best-fit rigid transform.
    //
    // The result is then normalized by the number of seam vertices as `avgErr`. If `avgErr` exceeds a threshold
    // proportional to the average UV seam length. the two charts' UV boundaries are too geometrically incompatible
    // to merge without unacceptable distortion — the seam is marked UNFEASIBLE_MATCHING.
    //
    // In Small Islands Remover when the distortionMode is set to `LOOSE`, we skip this check.
    double totErr = MatchingErrorTotal(mi, bpa, bpb);
    double avgErr = totErr / (double) bpa.size();
    if (avgErr > params.matchingThreshold * ((bmap[a->id] + bmap[b->id]) / 2.0)) {
        ci.cost = Infinity();
        ci.mvalue = CostInfo::UNFEASIBLE_MATCHING;
        return ci;
    }

    // The final cost is obtained by also considering three additional factors:
    //
    // - lossgain: the matching error (avgErr), scaled by a boundary ratio penalty. The ratio BorderUV / bmap is the
    // inverse of what was checked above — it's large when the seam is a small fraction of the chart's boundary.
    // Raising this to params.expb (a tunable exponent) controls how aggressively small-contact seams are penalized.
    // Intuitively: a seam that covers most of a chart's boundary is cheaper to process (low lossgain) than one that
    // barely touches it (high lossgain).
    //
    // - sizebonus: A more convenient score is given to operations merging a really small chart.
    //
    // - penalty: parameter passed as input, it discourages merge operations that have already been tried and failed.
    double lossgain = avgErr * std::pow(std::min(a->BorderUV() / bmap[a->id], b->BorderUV() / bmap[b->id]), params.expb);
    double sizebonus = std::min(a->AreaUV(), b->AreaUV());
    ci.cost = lossgain * sizebonus;
    if (ci.cost == 0 && penalty > 1.0)
        ci.cost = 1;
    ci.cost *= penalty;

    return ci;
}

static inline double GetPenalty(ClusteredSeamHandle csh, AlgoStateHandle state)
{
    if (state->penalty.find(csh) == state->penalty.end())
        state->penalty[csh] = 1.0;
    return state->penalty[csh];
}

static inline bool Valid(const WeightedSeam& ws, ConstAlgoStateHandle state)
{
    auto it = state->cost.find(ws.first);
    return (it != state->cost.end() && it->second == ws.second);
}

/*!
 * Rebuilds the priority queue, removing all seams that have become invalid after the latest merge operation.
 *
 * A seam is considered if and only if:
 * 1) It is still present in the parametrization.
 * 2) Its cost hasn't been updated by the latest merge operation.
 *
 * @param state: contains all major data regarding the current iteration of Texture Defragmentation.
 */
static inline void PurgeQueue(AlgoStateHandle state)
{
    // We clear the priority queue, collecting all valid seams into the `valid` temporary set.
    // A seam is deemed valid if it satisfies the following properties:
    // - It is still present in the parametrization.
    // - Its cost hasn't been updated by the latest merge operation.
    std::unordered_set<ClusteredSeamHandle> valid;
    while (!state->queue.empty()) {
        WeightedSeam ws = state->queue.top();
        if (Valid(ws, state) && ws.second != Infinity())
            valid.insert(ws.first);
        state->queue.pop();
    }

    ensure(state->queue.empty());

    // All seams deemed valid are pushed back into the priority queue.
    for (ClusteredSeamHandle csh : valid)
        state->queue.push(std::make_pair(csh, state->cost[csh]));
}

/*!
 * Copies into parameter `sd` a snapshot of the current state of the paired
 * charts, before applying a merge operation over them.
 *
 * This information will be used for both evaluating the operation and restore
 * the original state in case of a reject.
 * @param sd: a SeamData instance in which all information regarding the pair charts will be stored.
 * @param csh: The set of seams that will be merged.
 * @param graph: the current UV parametrization.
 * @param state: all necessary info regarding the current moment in time for the Texture Defragmentation procedure.
 */
static void ComputeSeamData(SeamData& sd, ClusteredSeamHandle csh, GraphHandle graph, AlgoStateHandle state)
{
    // We record the time elapsed for profiling reasons.
    PERF_TIMER_START;

    sd.csh = csh;

    // We construct the SeamData instance, named `sd`, for keeping a snapshot
    // of charts `a` and `b` before attempting a merge operation over them.
    //
    // We copy the shared clustered seams and the charts' parametrization.
    ChartPair charts = GetCharts(csh, graph);
    sd.a = charts.first;
    sd.b = charts.second;

    // If we already attempted this merge, increment by one the global counter
    // `num_retry` (it keeps the total number of merge operations attempted multiple times).
    if (state->failed[sd.a->id].count(sd.b->id) > 0)
        num_retry++;

    // For both charts, we copy their original per vertex and per wedge texture coordinates.
    // This data will be necessary for both evaluating the merge operation and for restoring
    // the original state in case the operation is rejected.

    // Note that, if we are merging a self-cut, we just need to copy one chart (both are the same).
    Mesh& m = graph->mesh;
    sd.texcoorda.reserve(3 * sd.a->FN());
    sd.vertexinda.reserve(3 * sd.a->FN());
    for (auto fptr : sd.a->fpVec) {
        sd.texcoorda.push_back(fptr->V(0)->T().P());
        sd.texcoorda.push_back(fptr->V(1)->T().P());
        sd.texcoorda.push_back(fptr->V(2)->T().P());
        sd.vertexinda.push_back(tri::Index(m, fptr->V(0)));
        sd.vertexinda.push_back(tri::Index(m, fptr->V(1)));
        sd.vertexinda.push_back(tri::Index(m, fptr->V(2)));
    }

    if (sd.a != sd.b) {
        sd.texcoordb.reserve(3 * sd.b->FN());
        sd.vertexindb.reserve(3 * sd.b->FN());
        for (auto fptr : sd.b->fpVec) {
            sd.texcoordb.push_back(fptr->V(0)->T().P());
            sd.texcoordb.push_back(fptr->V(1)->T().P());
            sd.texcoordb.push_back(fptr->V(2)->T().P());
            sd.vertexindb.push_back(tri::Index(m, fptr->V(0)));
            sd.vertexindb.push_back(tri::Index(m, fptr->V(1)));
            sd.vertexindb.push_back(tri::Index(m, fptr->V(2)));
        }
    }

    // We compute the upper bound of the UV boundary length spawning across the charts.
    // This value will be compared with the final border length to measure how much
    // of it is trimmed by our merge.
    sd.inputUVBorderLength = sd.a->BorderUV();
    if (sd.a != sd.b)
        sd.inputUVBorderLength += sd.b->BorderUV();

    PERF_TIMER_ACCUMULATE(t_seamdata);
}

/*!
 * For all faces in the chart, updates their UV Wedge coordinates
 * to the current UV Texture positions.
 *
 * This is a necessary preliminary step in the re-optimization procedure.
 * @param c: the chart instance.
 */
static void WedgeTexFromVertexTex(ChartHandle c)
{
    // Recall that for a chart subject to a merge operation, we store in its
    // UV Wedge coordinates the original positions before the merge, while
    // in its UV Vertex coordinates we store the position after applying it.
    //
    // For the re-optimization we now want to update the UV Wedge coordinates
    // by setting them equal to the UV Vertex ones.
    //
    // For each face `f` in the chart we set all its wedges UV coords to
    // the associated vertex UV coords.
    for (auto fptr : c->fpVec)
        for (int i = 0; i < 3; ++i)
            fptr->WT(i).P() = fptr->V(i)->T().P();
}

/*!
 * Merges the two pairs of UV charts, eliminating all seams across them.
 * If the merge doesn't involve a self-cut, this procedure involves an
 * initial alignment of the charts. Finally the FACE-FACE and VERTEX-FACE
 * topology are reconstructed in order to take into account the updated
 * parametrization.
 *
 * Since our alignment is applied through a rigid transformation, we resolve
 * possible small inconsistencies between the two charts by considering the
 * average of the collapsed vertices. However, the averaging could introduce small
 * overlaps. For this reason, we return a map associating for each collapsed
 * vertex the maximum displacement introduced by our procedure.
 * @param csh: the seams along the pair of charts to merge.
 * @param sd: a snapshot of the UV parametrization along the two charts
 *            before our procedure.
 * @param mi: the rigid transformation aligning the two charts.
 * @param params: User-defined parameters of the Texture Defragmentation filter.
 *                For this function we only use `offsetFactor` as a weight for the
 *                 displacements we return.
 * @return a map data structure, associating for each collapsed vertex the
 *         maximum displacement introduced by our procedure.
 */
static OffsetMap AlignAndMerge(ClusteredSeamHandle csh, SeamData& sd, const MatchingTransform& mi, const AlgoParameters& params)
{

    // Record the execution time of this function, for profiling.
    PERF_TIMER_START;

    OffsetMap om;

    // ================ ALIGNMENT FOR DISTINCT CHARTS, DENOTED AS `a` AND `b`. ================
    // Recall that we have computed the rigid matching transformation as a roto-translation
    // matrix called `mi`.
    //
    // This step applies the rigid transformation `mi` onto chart `b` to align its
    // UV boundary onto chart A's. In practice, we multiply `mi` on all vertices
    // of `b`, marking them as visited along to way to avoid applying the
    // transformation more than once.
    //
    // At the end of this phase, chart `b`'s UV layout will be rigidly repositioned
    // to sit adjacent to chart `a` along the seam, guaranteeing that their shared
    // boundary vertices nearly coincide.
    //
    // Note that a self-cut seam avoids this step: we don't need to align the same chart!
    if (sd.a != sd.b) {
        std::unordered_set<Mesh::VertexPointer> visited;
        for (auto fptr : sd.b->fpVec) {
            for (int i = 0; i < 3; ++i) {
                if (visited.count(fptr->V(i)) == 0) {
                    visited.insert(fptr->V(i));
                    fptr->V(i)->T().P() = mi.Apply(fptr->V(i)->T().P());
                }
            }
        }
    }

    // Recall that each seam edge has two pairs of vertices, one per chart.
    // We retrieve these vertices and store them as VertexPointer such that:
    // * chart a's has `v0a` and `v1a`.
    // * chart b's has `v0b` and `v1b`
    //
    // In 3D the two pairs geometrically coincide, but in texture space they
    // point to distinct UV coordinates. Our merge procedure aims to collapse
    // the two pairs into one.
    //
    // Recall that a clustered seam is composed of multiple edges. For each
    // edge we retrieve its vertices, denoted as `edge.V(0)` and `edge.V(1)`.
    // These two vertices in 3D correspond to either `v0a`/`v1a` or
    // `v0b`/`v1b` respectively. We enforce a canonical ordering such that:
    // * `edges.V(0)` matches `v0a` and `v0b`.
    // * `edges.V(1)` matched `v1a` and `v1b`.
    // To guarantee the defined arrangement we swap the pointers where necessary.
    //
    // While visiting the edges, we build two map data structures:
    // * `evec`: maps each seam's endpoint to the list of all mesh vertex pointers equal
    //           to it.
    //           For example evec[e.V(0)] = [v0a, v0b, ...]
    // * `mrep`: maps each mesh vertex `vi` to its representative. The representative
    //           will be the vertex that will substitute all the ones mapped to it.
    //           We always set the representative to the top of the list in `evec[vi]`
    // Extra care must be taken to avoid adding the same vertex twice.
    SeamMesh& seamMesh = csh->sm;
    for (SeamHandle sh : csh->seams) {
        for (int iedge : sh->edges) {
            SeamEdge& edge = seamMesh.edge[iedge];

            Mesh::VertexPointer v0a = edge.fa->V0(edge.ea);
            Mesh::VertexPointer v1a = edge.fa->V1(edge.ea);
            Mesh::VertexPointer v0b = edge.fb->V0(edge.eb);
            Mesh::VertexPointer v1b = edge.fb->V1(edge.eb);

            sd.seamVertices.insert(v0a);
            sd.seamVertices.insert(v1a);
            sd.seamVertices.insert(v0b);
            sd.seamVertices.insert(v1b);

            if (v0a->P() != edge.V(0)->P())
                std::swap(v0a, v1a);
            if (v0b->P() != edge.V(0)->P())
                std::swap(v0b, v1b);

            if (sd.mrep.count(v0a) == 0)
                sd.evec[edge.V(0)].push_back(v0a);
            sd.mrep[v0a] = sd.evec[edge.V(0)].front();

            if (sd.mrep.count(v1a) == 0)
                sd.evec[edge.V(1)].push_back(v1a);
            sd.mrep[v1a] = sd.evec[edge.V(1)].front();

            if (sd.mrep.count(v0b) == 0)
                sd.evec[edge.V(0)].push_back(v0b);
            sd.mrep[v0b] = sd.evec[edge.V(0)].front();

            if (sd.mrep.count(v1b) == 0)
                sd.evec[edge.V(1)].push_back(v1b);
            sd.mrep[v1b] = sd.evec[edge.V(1)].front();
        }
    }

    // For every vertex within a seam (a seam is made of at least one edge, and
    // each edge has two endpoint vertices), we collect the faces adjacent to
    // it by using VF topology (i.e., computing its VF star). These faces are
    // added into `vfTopologyFaceSet`.
    //
    // We need to keep track of these faces because, after a vertex has been
    // collapsed, we need to update the topology of these faces to redirect
    // to the representative vertex.
    for (auto vp : sd.seamVertices) {
        std::vector<Mesh::FacePointer> faces;
        std::vector<int> indices;
        face::VFStarVF(vp, faces, indices);
        sd.vfTopologyFaceSet.insert(faces.begin(), faces.end());
    }

    // We now update each face (on both charts) to point to the representative
    // vertex. We do so by setting the pointed vertex V(i) to the representative
    // pointed by mrep[V(i)].
    //
    // At the end of this step, both charts' faces reference the same single
    // object at each seam position.
    for (auto fptr : sd.a->fpVec) {
        for (int i = 0; i < 3; ++i)
            if (sd.mrep.count(fptr->V(i)))
                fptr->V(i) = sd.mrep[fptr->V(i)];
    }
    if (sd.a != sd.b) {
        for (auto fptr : sd.b->fpVec) {
            for (int i = 0; i < 3; ++i)
                if (sd.mrep.count(fptr->V(i)))
                    fptr->V(i) = sd.mrep[fptr->V(i)];
        }
    }

    // ================ UPDATE FACE-FACE TOPOLOGY ================
    // Now that the two charts have been aligned, we need to update the
    // FACE-FACE (FF) adjacency properties along each edge of the seams.
    // Consider an edge `e` composing a cut along the chart pairs: we need
    // to link the two faces, one per chart, sharing it.
    // We call these triangles `fa` and `fb`and:
    //  * `fa` needs to add `fb` as its adjacent face through the edge `e`.
    //  * `fb` needs to add `ab` as its adjacent face through the edge `e`.
    for (SeamHandle sh : csh->seams) {
        for (int iedge : sh->edges) {
            SeamEdge& edge = seamMesh.edge[iedge];
            edge.fa->FFp(edge.ea) = edge.fb;
            edge.fa->FFi(edge.ea) = edge.eb;
            edge.fb->FFp(edge.eb) = edge.fa;
            edge.fb->FFi(edge.eb) = edge.ea;
        }
    }

    // ================ UPDATE VERTEX-FACE TOPOLOGY ================
    // We now need to reconstruct the VERTEX_FACE (VF) adjacency, since all
    // vertices on a seam have a new face neighbor, the one from the other
    // merged chart.
    //
    // First, we clear the previous VF list since it is no longer valid.
    //
    // Second, recall that we pre-collected all faces touching a seam vertex
    // in the set `vfTopologyFaceSet`. We scan it such that each face `f` is
    // added to the VF list entry associated to its corners.
    {
        for (Mesh::VertexPointer vp : sd.seamVertices) {
            vp->VFp() = 0;
            vp->VFi() = 0;
        }

        for (Mesh::FacePointer fptr : sd.vfTopologyFaceSet) {
            for (int i = 0; i < 3; ++i) {
                if (sd.seamVertices.find(fptr->V(i)) != sd.seamVertices.end()) {
                    (*fptr).VFp(i) = (*fptr).V(i)->VFp();
                    (*fptr).VFi(i) = (*fptr).V(i)->VFi();
                    (*fptr).V(i)->VFp() = &(*fptr);
                    (*fptr).V(i)->VFi() = i;
                }
            }
        }
    }

    // After the rigid alignment, v0a and v0b (and also v1a and v1b) could slightly
    // differ due to the residual error present in the rigid transformation.
    //
    // We resolve it by setting the UV coordinates of their representative vertices to
    // the average of all its associated collapsed vertices UV coordinates. This works
    // because the parametrization now references only the representative vertices.
    //
    // While averaging, we compute for all representative vertices its `maxOffset`: the
    // maximum distance between the associated collapsed vertices and the average, scaled
    // by `params.offsetFactor`. The value can be interpreted as the largest displacement
    // introduced by the averaging.
    //
    // This offset will be stored in the map `om`, which is returned by the function, and
    // will be used to detect and handle potential UV overlaps that could have been
    // introduced by the averaging.
    for (auto& entry : sd.evec) {
        vcg::Point2d sumpos = vcg::Point2d::Zero();
        for (auto vp : entry.second) {
            sumpos += vp->T().P();
        }
        vcg::Point2d avg = sumpos / (double) entry.second.size();

        double maxOffset = 0;
        for (auto& vp : entry.second)
            maxOffset = std::max(maxOffset, params.offsetFactor * (vp->T().P() - avg).Norm());

        om[entry.second.front()] = maxOffset;
        entry.second.front()->T().P() = avg;
    }

    PERF_TIMER_ACCUMULATE(t_alignmerge);

    return om;
}

/*!
 * We compute the UV re-optimization area for the latest merge operation.
 * This region is represented as the face set composing it and is stored
 * in the field sd.optimizationArea`.
 *
 * We keep a snapshot of the current and original UV coordinates in
 * `sd.texcoordoptVert` and `sd.textcoordoptWedge`, which will be used
 * during the optimization procedure.
 *
 * Finally, we also store the extension of area to re-optimize and how much of
 * it contains folds/overlaps in `sd.inputAbsoluteArea` and `sd.inputNegativeArea`.
 * These final values will be used at the end of the optimization to compare if
 * the result improved or worsened the UV quality.
 * @param sd: stores all temporary data necessary by the greedy optimization.
 * @param mesh: the original input mesh.
 * @param om: a map containing for for each representative vertex of the optimization
 *            its maximum displacement.
 */
static void ComputeOptimizationArea(SeamData& sd, Mesh& mesh, OffsetMap& om)
{
    // Start measuring execution time, for performance reasons.
    PERF_TIMER_START;

    // We identify all faces that can be part of the UV re-optimization.
    // These are all faces inside the applied merge operation (i.e., belonging to
    // the pair of charts). We store them in the vector `fpVec`.
    //
    // Note that if the operation dissolved a self-cut, then we add to the list
    // only the faces from one chart (since the other is identical).
    std::vector<Mesh::FacePointer> fpvec;
    fpvec.insert(fpvec.end(), sd.a->fpVec.begin(), sd.a->fpVec.end());
    if (sd.a != sd.b)
        fpvec.insert(fpvec.end(), sd.b->fpVec.begin(), sd.b->fpVec.end());

    // We compute the potential UV re-optimization region in respect to the vertices.
    // This procedure is handled by the `ComputeVerticesWithinOffsetThreshold`
    // function, whose result (the set of vertices of the optimized area) is passed to
    // the field `sd.verticesWithinThreshold`.
    sd.verticesWithinThreshold = ComputeVerticesWithinOffsetThreshold(mesh, om, sd);

    // We have the vertices of the UV re-optimization area, we now need to determine its faces.
    //
    // These are found checking for each face `f`, coming from the vectors of faces of
    // the charts, the following properties:
    // * At least one of the corners of f belongs in the UV re-optimization area (i.e.,
    //   is contained in `sd.verticesWithinThreshold`).
    // * All three edges of `f` are manifold within the original 3D mesh. We check this
    //   property by employing FACE-FACE adjacency.
    // If both properties are satisfied, then `f` is part of the re-optimization region
    // and is added to the field `sd.optimizationArea`.
    //
    // An edge case occurs when the face has a corner inside the re-optimization area, but
    // one of its edges is non-manifold. Our re-optimization procedure assumes that the
    // input region is manifold. To guarantee this requirement, we remove all three corners
    // of `f` from `sd.verticesWithinThreshold`.
    sd.optimizationArea.clear();
    auto ffadj = Get3DFaceAdjacencyAttribute(mesh);
    for (auto fptr : fpvec) {
        bool addFace = false;
        bool edgeManifold = true;
        for (int i = 0; i < 3; ++i) {
            edgeManifold &= IsEdgeManifold3D(mesh, *fptr, i, ffadj);
            if (sd.verticesWithinThreshold.find(fptr->V(i)) != sd.verticesWithinThreshold.end())
                addFace = true;
        }
        if (addFace && edgeManifold)
            sd.optimizationArea.insert(fptr);
        if (addFace && !edgeManifold) {
            sd.verticesWithinThreshold.erase(fptr->V(0));
            sd.verticesWithinThreshold.erase(fptr->V(1));
            sd.verticesWithinThreshold.erase(fptr->V(2));
        }
    }

    // We backup the current values of the UV coordinates per-vertex and
    // per-wedge within the fields `sd.texcoordoptVert` and `sd.texcoordoptWedge`.
    //
    // Recall that the per-vertex UV coordinates contain the values already
    // displaced by `AlignAndMerge`, while the per-wedge UV coordinates contain
    // the original ones.
    for (auto fptr : sd.optimizationArea) {
        sd.texcoordoptVert.push_back(fptr->V(0)->T().P());
        sd.texcoordoptVert.push_back(fptr->V(1)->T().P());
        sd.texcoordoptVert.push_back(fptr->V(2)->T().P());

        sd.texcoordoptWedge.push_back(fptr->WT(0).P());
        sd.texcoordoptWedge.push_back(fptr->WT(1).P());
        sd.texcoordoptWedge.push_back(fptr->WT(2).P());

    }


    // We compute the extension of the optimization area, plus
    // the portion within containing overlaps/folds that could have
    // been introduced by the merge operation.
    //
    // These results are stored within the fields `sd.inputAbsoluteArea`
    // and `sd.inputNegativeArea`.
    //
    // The two values are computed by doing a face scan over the found
    // optimization area. For each face `f` we compute its original
    // signed UV area through the cross-produce of its wedges.
    // If the computed area is negative we update both `sd.inputAbsoluteArea`
    // and `sd.inputNegativeArea`, otherwise just the first.
    //
    // These two values will be used after the optimization procedure to compare
    // if the result improves or worsens the UV quality.
    {
        sd.inputNegativeArea = 0;
        sd.inputAbsoluteArea = 0;
        for (auto fptr : sd.optimizationArea) {
            vcg::Point2d uv0in = fptr->WT(0).P();
            vcg::Point2d uv1in = fptr->WT(1).P();
            vcg::Point2d uv2in = fptr->WT(2).P();

            double inputAreaUV = ((uv1in - uv0in) ^ (uv2in - uv0in)) / 2.0;
            if (inputAreaUV < 0)
                sd.inputNegativeArea += inputAreaUV;
            sd.inputAbsoluteArea += std::abs(inputAreaUV);
        }
    }

    PERF_TIMER_ACCUMULATE(t_optimization_area);
}

/* Visit vertices starting from the merged ones, subject to the distance budget
 * stored in the OffsetMap object. */

/*!
 * Finds the portion of the merged charts where to apply UV re-optimization.
 * This region is returned as the set of vertices spawning it.
 * @param m: the general mesh.
 * @param om: a map matching each representative vertex with its maximum displacement
 *            introduced by the (align and) merge operation.
 * @param sd: a snapshot of the parameterization before the two charts have been merged.
 * @return The UV re-optimization region, represented as the set of vertices
 *         belonging to it.
 */
static std::unordered_set<Mesh::VertexPointer> ComputeVerticesWithinOffsetThreshold(Mesh& m, const OffsetMap& om, const SeamData& sd)
{

    // For determining the maximum area of coverage to apply our ARAP
    // re-optimization, we visit the UV layout similarly to Dijkstra.
    // Our seeds are the representative vertices created during the
    // `AlignAndMerge` function (retrieved from the `om` parameter).
    // For each seed we visit the graph identifying its "influence area",
    // the portion of neighboring vertices influenced by its transformation.
    // The extent of a seed influence is represented by its displacement,
    // stored as the associated `om` value. This data is interpreted as the
    // seed's budget. The aim of our visit consists in maximizing the coverage
    // of each seed before their budget runs out. In this way we will find the
    // maximum area of coverage.
    //
    //
    // We construct the following data structures:
    // * vset: the set of vertices influenced by our seeds.
    //         This vector is initialized with the representative vertices
    //         themselves and will be returned as output to represent the
    //         extent of the re-optimization area.
    // * dist: a map tracking for each seed its "remaining budget" for the visit.
    // * h: a max heap ordering seeds by their budget. Larger budgets are placed
    //      first. For sorting the heap we define a custom comparison function
    //      called `cmp`.
    typedef std::pair<Mesh::VertexPointer, double> VertexNode;
    auto cmp = [] (const VertexNode& v1, const VertexNode& v2) { return v1.second < v2.second; };
    std::unordered_set<Mesh::VertexPointer> vset;
    OffsetMap dist;
    std::vector<VertexNode> h;


    // In the beginning we fill the heap with the representative vertices
    // paired to their maximum displacement. These values are retrieved
    // from `om`, the map returned by `AlignAndMerge`.
    for (const auto& entry : om) {
        h.push_back(std::make_pair(entry.first, entry.second));
        dist[entry.first] = entry.second;
    }
    std::make_heap(h.begin(), h.end());

    // ============== DIJKSTRA VISIT ==============
    // We visit the heap until it has been cleared, popping at each
    // iteration the element at the top. The popped head is the seed
    // that currently has the largest remaining budget, suggesting us
    // to expand its visit.
    //
    // Note that the heap can contain multiple entries for the same seed.
    // This happens because when a seed is updated by getting a higher
    // budget (it can never decrease), we push the new pair into the heap
    // without removing the old entry. Extra care must be taken to ensure
    // that the current element refers to the most recent iteration of
    // the said seed.
    //
    // For expanding the range of a seed we first retrieve all its incident
    // faces by computing the VF star. Extra care must be taken to make sure
    // that all retrieved faces belong to either chart `a` or chart `b`. If
    // there is an exception, a bug occurred.
    //
    // For each (valid) incident face, we denote the seed as V(0) (i.e., its
    // first corner). We consider the other two corners, denoted as V(1) and
    // V(2). We compute their distance D(i) as the remaining budget of the seed
    // passing through vertex V(i), minus the UV space length of the edge connecting
    // the current vertex to the neighbor.
    //
    // We then push D(i) as the new budget for vertex V(i) if either it isn't present
    // in the heap, or its larger than the previous iteration.
    while (!h.empty()) {
        std::pop_heap(h.begin(), h.end(), cmp);
        VertexNode node = h.back();
        h.pop_back();
        if (node.second == dist[node.first]) {
            std::vector<Mesh::FacePointer> faces;
            std::vector<int> indices;
            face::VFStarVF(node.first, faces, indices);

            for (unsigned i = 0; i < faces.size(); ++i) {
                if(faces[i]->id != sd.a->id && faces[i]->id != sd.b->id){
                    LOG_ERR << "issue at face " << tri::Index(m, faces[i]);
                }
                ensure(faces[i]->id == sd.a->id || faces[i]->id == sd.b->id);

                // if either neighboring vertex is seen with more spare distance,
                // update the distance map

                int e1 = indices[i];
                Mesh::VertexPointer v1 = faces[i]->V1(indices[i]);
                double d1 = dist[node.first] - EdgeLengthUV(*faces[i], e1);

                if (d1 >= 0 && (dist.find(v1) == dist.end() || dist[v1] < d1)) {
                    dist[v1] = d1;
                    h.push_back(std::make_pair(v1, d1));
                    std::push_heap(h.begin(), h.end(), cmp);
                }

                int e2 = (indices[i]+2)%3;
                Mesh::VertexPointer v2 = faces[i]->V2(indices[i]);
                double d2 = dist[node.first] - EdgeLengthUV(*faces[i], e2);

                if (d2 >= 0 && (dist.find(v2) == dist.end() || dist[v2] < d2)) {
                    dist[v2] = d2;
                    h.push_back(std::make_pair(v2, d2));
                    std::push_heap(h.begin(), h.end(), cmp);
                }
            }
        }
    }

    // The area of influence where to apply the ARAP optimization is returned
    // as the set of vertices reached by the traversal. This portion is returned
    // as the vertex set `vset`.
    for (const auto& entry : dist)
        vset.insert(entry.first);

    LOG_DEBUG << "vset.size() == " << vset.size();

    return vset;
}

static std::vector<HalfEdge> ExtractHalfEdges(const std::vector<ChartHandle>& charts, const vcg::Box2d& box, bool internalOnly)
{
    std::vector<HalfEdge> hvec;
    for (auto ch : charts)
        for (auto fptr : ch->fpVec)
            for (int i = 0; i < 3; ++i)
                if ((!internalOnly || !face::IsBorder(*fptr, i)) && SegmentBoxIntersection(Segment(fptr->V0(i)->T().P(), fptr->V1(i)->T().P()), box))
                    hvec.push_back(HalfEdge{fptr, i});
    return hvec;
}



/*!
 * Given two charts A and B, considered for a merge operation, checks if their condensation does not
 * cause any overlap.
 *
 * If an overlap is found, the merge is rejected. Note that any overlaps within the
 * optimization area is ignored, as they are hadneld by the As-Rigid-As-Possible (ARAP)
 * optimizer.
 *
 * @param sd: the `SeamData` object representing the merge operation.
 *
 * @return `PASS` if no overlap is found, otherwise `FAIL_GLOBAL_OVERLAP_BEFORE`.
 */
static CheckStatus CheckBoundaryAfterAlignmentInner(SeamData& sd)
{

    // Guarantee that the function is checking a merge operation between two distinct charts.
    ensure(sd.a != sd.b);

    // We identify for both charts the boundary of their fixed region. These portions of the
    // area are represented as two vectors of edges on the border, which are denoted as `aVec`
    // and `bVec` (for chart A and chart B respectively).
    //
    // The process for computing both vectors is analogous: we iterate over all faces of one
    // chart, considering for our construction only those within the fixed region (i.e., not
    // in the optimization area).
    //
    // For each fixed region face check for edges satisfying one of these two properties:
    //
    // * The edge is on the outer boundary of the chart. This can be verified by seeing that
    //   the edge is incident to only one face.
    //
    // * The edge delimits the fixed region from the optimization area. This can be verified
    //   by determining that its two incident faces belong each to a different region.
    //
    // For each valid edge, we add its half-edge to the vector.
    std::vector<HalfEdge> aVec;
    for (auto fptr : sd.a->fpVec)
        if (sd.optimizationArea.find(fptr) == sd.optimizationArea.end())
            for (int i = 0; i < 3; ++i)
                if (face::IsBorder(*fptr, i) || (sd.optimizationArea.find(fptr->FFp(i)) != sd.optimizationArea.end()))
                    aVec.push_back(HalfEdge{fptr, i});

    std::vector<HalfEdge> bVec;
    for (auto fptr : sd.b->fpVec)
        if (sd.optimizationArea.find(fptr) == sd.optimizationArea.end())
            for (int i = 0; i < 3; ++i)
                if (face::IsBorder(*fptr, i) || (sd.optimizationArea.find(fptr->FFp(i)) != sd.optimizationArea.end()))
                    bVec.push_back(HalfEdge{fptr, i});

    // Check for possible overlaps between the two charts.
    //
    // If there exists at least one half-edge shared across the two borders, our alignment
    // has placed B's fixed region in a position that overlaps A's fixed region. This issue
    // cannot be resolved, the merge must be rejected.
    //
    // Otherwise, the merge can proceed.
    //
    // These two cases are associated to the status enum `FAIL_GLOBAL_OVERLAP_BEFORE` and `PASS`.
    if ((aVec.size() > 0) && (bVec.size() > 0)) {
        std::vector<HalfEdgePair> heVec = CrossIntersection(aVec, bVec);
        if (heVec.size() > 0)
            return FAIL_GLOBAL_OVERLAP_BEFORE;
    }

// Old implementation for checking possible overlaps. It also checks for overlaps within the
// optimization Area. Now deprecated because the intersection between optimization areas is
// handled by the ARAP optimizer.
#if 0
    vcg::Box2d box;
    for (auto fptr : sd.b->fpVec)
        for (int i = 0; i < 3; ++i)
            box.Add(fptr->V(i)->T().P());

    // also check if the edges of b overlap the edges of a (only check the edges inside the bbox of b)
    aVec = ExtractHalfEdges({sd.a}, box, false);
    bVec = ExtractHalfEdges({sd.b}, box, false);
    if ((aVec.size() > 0) && (bVec.size() > 0)) {
        std::vector<HalfEdgePair> heVec = CrossIntersection(aVec, bVec);
        if (heVec.size() > 0)
            return FAIL_GLOBAL_OVERLAP_BEFORE;
    }
#endif

    return PASS;
}

static CheckStatus CheckBoundaryAfterAlignment(SeamData& sd)
{
    PERF_TIMER_START;
    LOG_DEBUG << "Running CheckBoundaryAfterAlignment()";
    CheckStatus status = CheckBoundaryAfterAlignmentInner(sd);
    PERF_TIMER_ACCUMULATE(t_check_before);
    return status;
}

/*!
 * This algorithm checks that the UV parametrization after applying the merge operation
 * (and possible re-optimizations), does not introduce either too much distortion or
 * new overlaps/fold. If not, the merge is rejected and a status enum describing the
 * found problem is returned.
 *
 * If new overlaps are found, usually the Texture Defragmentation will try a new
 * re-optimization of the merge operation, trying to fix the particular problem.
 *
 * @param sd: contains all data regarding the merge operation.
 * @param state: contains all information regarding the current phase of the Texture Defragmentation procedure.
 * @param params: user-defined parameters.
 *
 * @return a status enum indicating if the merge has been rejected and why.
 */
static CheckStatus CheckAfterLocalOptimizationInner(SeamData& sd, AlgoStateHandle state, const AlgoParameters& params)
{
    // =============================== CHECK GLOBAL DISTORTION ===============================
    // Recall that we have stored the starting ARAP energy of the whole mesh, split between
    // numerator and denominator, in `state->arapNum` and `state->arapDenom`.
    //
    // Recall also that our re-optimization stored the optimization area ARAP energy before
    // and after the optimization in `sd.inputArapNum`/`sd.inputArapDenom` and
    // `sd.outputArapNum`/`sd.outputArapDenom` respectively (we will consider only the numerator).
    //
    // The difference between `sd.outputArapNum` and `sd.inputArapNum` represents the change of
    // the ARAP energy introduced by the current merge within the optimization area.
    //
    // Adding the difference to the global ARAP energy numerator gives us an upper-bound estimate
    // of the global ARAP energy if the merge is accepted. If this estimation exceeds our global
    // distortion threshold, then the merge introduces too much distortion and will be rejected
    // with the status `FAIL_DISTORTION_GLOBAL`.
    double newArapVal = (state->arapNum + (sd.outputArapNum - sd.inputArapNum)) / state->arapDenom;
    if (newArapVal > params.globalDistortionThreshold)
        return FAIL_DISTORTION_GLOBAL;

    // =============================== CHECK LOCAL DISTORTION ===============================
    // `sd.outputArapNum` and `sd.outputArapDenom` store the optimization area ARAP energy
    // after the merge operation.
    //
    // If this local energy exceeds the local distortion threshold, then the merge introduces
    // too much distortion and will be rejected with status `FAIL_DISTORTION_LOCAL`.
    double localDistortion = sd.outputArapNum / sd.outputArapDenom;
    if (localDistortion > params.distortionTolerance) {
        return FAIL_DISTORTION_LOCAL;
    }

    // =============================== CHECK FOLDED AREA ===============================
    // Recall that we have stored for the pre-merge UV parametrization its absolute
    // area and signed area as `inputAbsoluteArea` and `inputNegativeArea`.
    // For the signed area we only consider it if negative, because it means that
    // there are flipped or folded triangles within the UV space.
    //
    // We compute the absolute and signed UV areas also for the post-merge
    // parametrization, storing them in outputAbsoluteArea` and `outputNegativeArea`.
    //
    // The ratio between the total area and folded/flipped area measures how much
    // folding is present. We compute this ration both for the pre-merge and
    // post-merge parametrization in `inputRatio` and `outputRatio respectively.
    //
    // If the post-merge ratio is bigger than the pre-merge one, it means that
    // our merge operation introduces more folding, so it is rejected with
    // status `FAIL_LOCAL_OVERLAP`.
    double outputNegativeArea = 0;
    double outputAbsoluteArea = 0;
    for (auto fptr : sd.optimizationArea) {
        double areaUV = AreaUV(*fptr);
        if (areaUV < 0)
            outputNegativeArea += areaUV;
        outputAbsoluteArea += std::abs(areaUV);
    }
    double inputRatio = std::abs(sd.inputNegativeArea / sd.inputAbsoluteArea);
    double outputRatio = std::abs(outputNegativeArea / outputAbsoluteArea);
    if (outputRatio > inputRatio) {
        return FAIL_LOCAL_OVERLAP;
    }

    // Recall that, during the UV optimization procedure (i.e., `OptimizeChart`), when
    // an intersection is detected, its vertices are added to the set
    // `fixedVerticesFromIntersectingEdges`. During a retry of the optimization
    // procedure, the incriminated vertices will be pinned, such that the new solution
    // is more likely to not present the found overlaps. Still, if an intersection is
    // found involving already-pinned vertices, we are forced to keep it.
    //
    // When checking for overlaps introduced by the merge, these intersections
    // should be ignored. To do so we define two filter functions as lambdas:
    //
    //  * FixedPair: check whether all four vertices of both edges in an intersecting
    //               pair are already fixed. Used for intersections within the
    //               optimization area.
    //
    //  * FixedFirst: check whether the two vertices of the first edge are fixed.
    //                Used for intersections between the optimization boundary
    //                and the fixed region.
    auto FixedPair = [&] (const HalfEdgePair& hep) -> bool {
        return /*hep.first.fp->id == hep.second.fp->id
                &&*/ sd.fixedVerticesFromIntersectingEdges.find(hep.first.V0()) != sd.fixedVerticesFromIntersectingEdges.end()
                && sd.fixedVerticesFromIntersectingEdges.find(hep.first.V1()) != sd.fixedVerticesFromIntersectingEdges.end()
                && sd.fixedVerticesFromIntersectingEdges.find(hep.second.V0()) != sd.fixedVerticesFromIntersectingEdges.end()
                && sd.fixedVerticesFromIntersectingEdges.find(hep.second.V1()) != sd.fixedVerticesFromIntersectingEdges.end();
    };

    auto FixedFirst = [&] (const HalfEdgePair& hep) -> bool {
        return /*hep.first.fp->id == hep.second.fp->id
                &&*/ sd.fixedVerticesFromIntersectingEdges.find(hep.first.V0()) != sd.fixedVerticesFromIntersectingEdges.end()
                && sd.fixedVerticesFromIntersectingEdges.find(hep.first.V1()) != sd.fixedVerticesFromIntersectingEdges.end();
    };


    // The merge could introduce overlaps from the optimized area either over the
    // fixed region or to itself.
    //
    // For checking the presence of overlaps we construct the data structure `sVec`
    // holding the UV half-edges of the optimized area outer boundary or belonging
    // to the interface between the optimization area and the surrounding fixed region.
    std::vector<HalfEdge> sVec;
    for (auto fptr : sd.optimizationArea)
        for (int i = 0; i < 3; ++i)
            if (face::IsBorder(*fptr, i) || (sd.optimizationArea.find(fptr->FFp(i)) == sd.optimizationArea.end()))
                sVec.push_back(HalfEdge{fptr, i});

    // =============================== CHECK INNER OVERLAP ===============================
    // We check if the optimization area crosses itself in UV space. We remove already
    // fixed pairs by using the lambda `FixedPair`. If after the filtering there are
    // still some intersections, it means that the optimization introduces some UV
    // folds that cannot be resolved. We reject the merge with the status
    // `FAIL_GLOBAL_OVERLAP_AFTER_OPT`.
    //
    // The incriminated intersection pairs will be within `sd.intersectionOpt` such that
    // a new iteration of `OptimizeChart` could fix them and compute a tighter solution.
    if (sVec.size() > 0) {
        sd.intersectionOpt = Intersection(sVec);
        sd.intersectionOpt.erase(std::remove_if(sd.intersectionOpt.begin(), sd.intersectionOpt.end(), FixedPair), sd.intersectionOpt.end());
        if (sd.intersectionOpt.size() > 0) {
            return FAIL_GLOBAL_OVERLAP_AFTER_OPT;
        }
    }

    // =============================== CHECK FIXED AREA OVERLAP ===============================
    // We check if the optimization area's boundary intersects the outer boundary of the
    // fixed region. The fixed region border is constructed as the set `nopVecBorder`.
    //
    // After the intersection is computed, already fixed pairs are removed by the
    // lambda `FixedFirst`. If after the filtering there are still some intersections, it
    // means that the optimization introduces some UV folds that cannot be resolved.
    // We reject the merge with the status `FAIL_GLOBAL_OVERLAP_AFTER_BND`.
    //
    // Note that this check is just an approximation. Finding all the overlaps between
    // the two regions would require to rasterize the triangles and check all intersections
    // between them.
    std::vector<HalfEdge> nopVecBorder;
    for (auto fptr : sd.a->fpVec)
        if (sd.optimizationArea.find(fptr) == sd.optimizationArea.end())
            for (int i = 0; i < 3; ++i)
                if (face::IsBorder(*fptr, i) /* || (sd.optimizationArea.find(fptr->FFp(i)) != sd.optimizationArea.end()) */)
                    nopVecBorder.push_back(HalfEdge{fptr, i});
    if (sd.a != sd.b) {
        for (auto fptr : sd.b->fpVec)
            if (sd.optimizationArea.find(fptr) == sd.optimizationArea.end())
                for (int i = 0; i < 3; ++i)
                    if (face::IsBorder(*fptr, i) /* || (sd.optimizationArea.find(fptr->FFp(i)) != sd.optimizationArea.end()) */)
                        nopVecBorder.push_back(HalfEdge{fptr, i});
    }

    if (sVec.size() > 0 && nopVecBorder.size() > 0) {
        sd.intersectionBoundary = CrossIntersection(sVec, nopVecBorder);
        sd.intersectionBoundary.erase(std::remove_if(sd.intersectionBoundary.begin(), sd.intersectionBoundary.end(), FixedFirst), sd.intersectionBoundary.end());
        if (sd.intersectionBoundary.size() > 0) {
            return FAIL_GLOBAL_OVERLAP_AFTER_BND;
        }
    }

    // =============================== CHECK INTERNAL EDGES OVERLAP ===============================
    // As of now we have checked for overlap only boundary edges, we need to ensure
    // that the optimization border does not fold over any internal one, both inside
    // or outside the optimization area.
    //
    // To optimize the search, a bounding box is constructed over the optimization area.
    // By limiting our search over the bounding box we guarantee a linear complexity.
    // Otherwise, an explicit check between the interior edges with `sVec` would take
    // quadratic time.
    //
    // If an intersection is found, we reject the merge with the status
    // `FAIL_GLOBAL_OVERLAP_AFTER_BND`. All found intersections are stored in
    // `sd.intersectionInternal`.
    vcg::Box2d optBox;
    for (auto fptr : sd.optimizationArea)
        for (int i = 0; i < 3; ++i)
            optBox.Add(fptr->V(i)->T().P());

    std::vector<HalfEdge> internal = ExtractHalfEdges({sd.a, sd.b}, optBox, true); // internal only

    if (sVec.size() > 0 && internal.size() > 0) {
        sd.intersectionInternal = CrossIntersection(sVec, internal);
        sd.intersectionInternal.erase(std::remove_if(sd.intersectionInternal.begin(), sd.intersectionInternal.end(), FixedFirst), sd.intersectionInternal.end());
        if (sd.intersectionInternal.size() > 0) {
            return FAIL_GLOBAL_OVERLAP_AFTER_BND;
        }
    }

    // =============================== CHECK CHARTS BOUNDARY OVERLAP ===============================
    // Recall that we refer to the charts operand of the merge operation as chart A and chart B.
    // We now check for intersections between the entire boundary of chart A with the entire
    // boundary of chart B. For efficiency, we restrict the search over the bounding box
    // of chart B.
    //
    // If an overlap is found, we reject the merge operation with the status `FAIL_GLOBAL_OVERLAP_UNFIXABLE`.
    // Note this kind of overlap cannot be fixed, the two charts fold in a way that cannot be resolved
    // by pinning more vertices. For this reason no intersection is recorded.
    vcg::Box2d box;
    for (auto fptr : sd.b->fpVec)
        for (int i = 0; i < 3; ++i)
            box.Add(fptr->V(i)->T().P());

    std::vector<HalfEdge> aVec = ExtractHalfEdges({sd.a}, box, false);
    std::vector<HalfEdge> bVec = ExtractHalfEdges({sd.b}, box, false);
    if ((aVec.size() > 0) && (bVec.size() > 0)) {
        std::vector<HalfEdgePair> heVec = CrossIntersection(aVec, bVec);
        if (heVec.size() > 0)
            return FAIL_GLOBAL_OVERLAP_UNFIXABLE;
    }

    // If all checks are passed, the merge is accepted with status `PASS`.
    return PASS;
}

static CheckStatus CheckAfterLocalOptimization(SeamData& sd, AlgoStateHandle state, const AlgoParameters& params)
{
    PERF_TIMER_START;
    LOG_DEBUG << "Running CheckAfterLocalOptimization()";
    CheckStatus status = CheckAfterLocalOptimizationInner(sd, state, params);
    PERF_TIMER_ACCUMULATE(t_check_after);
    return status;
}

/*!
 * After an align and merge operation, it applies a local ARAP re-optimizaton procedure
 * to decrease the distortion introduced by the merge.
 *
 * It is designed to be called multiple times over the same merged area with progressively
 * tighter constraints, until either a valid result is found or all options are exhausted.
 *
 * If a solution is reached, the graph's mesh will be updated with the found UV coordinates.
 *
 * @param sd: a list of all seams in the current parameterization.
 * @param graph: the parameterization to optimize.
 * @param fixIntersectingEdges: a flag indicating if the current call is the first attempt or not.
 *
 * @return a status value indicating the outcome of the procedure.
 */
static CheckStatus OptimizeChart(SeamData& sd, GraphHandle graph, bool fixIntersectingEdges)
{
    // Records execution time, profiling each sub-phase separately.
    // The sub-phases are:
    //  * t_optimize_build
    //  * t_optimize_arap
    //  * t_optimize
    PERF_TIMER_START;

    // We backed up the UV per-Vertex and per-Wedge UV coordinates of the mesh, right
    // after the initial align and merge operation. These snapshots were stored in
    // `sd.texcoordoptVert` and `texcoordoptWedge`.
    //
    // Our optimization procedure should be executed over those snapshots, so we restore
    // them to overwrite any UVs modifications made by a previous Optimize Chart attempt
    // (e.g., this could be a second attempt to optimize the charts after a failure).
    //
    // We define the working domain (i.e., the area to optimize) as the FaceGroup called
    // `support`. It will contain all faces belonging to the optimized area.
    auto itV = sd.texcoordoptVert.begin();
    auto itW = sd.texcoordoptWedge.begin();
    FaceGroup support(graph->mesh, INVALID_ID);
    for (auto fptr : sd.optimizationArea) {
        support.AddFace(fptr);
        fptr->V(0)->T().P() = *itV++; fptr->WT(0).P() = *itW++;
        fptr->V(1)->T().P() = *itV++; fptr->WT(1).P() = *itW++;
        fptr->V(2)->T().P() = *itV++; fptr->WT(2).P() = *itW++;
    }


    // Before starting the optimization, we compute the ARAP contribution of the base optimization
    // area.
    //
    // The ARAP energy quantifies how much the transformation for moving from the original UVs to
    // the target ones differs from a rigid transformation (i.e., one involving only rotations and
    // translations).
    //
    // WARNING: it is critical for computing the energy that the UV Wedge coordinates HAVE NOT YET BEEN UPDATED.
    // Otherwise, the Wedge UVs would no longer reflect the pre-optimization state.
    ARAP::ComputeEnergyFromStoredWedgeTC(support.fpVec, graph->mesh, &sd.inputArapNum, &sd.inputArapDenom);

    // As of now we had in the UV Wedge coordinates the positions before the align and merge operation.
    // Since the actual optimization procedure needs to work only on the coordinates after the merge
    // operation, we no longer need these old values.
    //
    // For this reason we copy the current per-Vertex UVs into the per-Wedge UVs (via WedgeTexFromVertexTex).
    WedgeTexFromVertexTex(sd.a);
    if (sd.a != sd.b)
        WedgeTexFromVertexTex(sd.b);

    LOG_DEBUG << "Building shell...";

    // We now construct the shell mesh, a temporary model in which we will apply the optimization procedure.
    // The shell mesh is built over the faces in the optimization area. The function managing the
    // construction will also compute the per-face target shapes of the optimization.
    //
    // Since we are computing a merge optimization, the downscaling factor is set to 1.0, guaranteeing that the
    // target shapes represent the ideal UV configurations to minimize ARAP distortion after the merge.
    sd.shell.Clear();
    sd.shell.ClearAttributes();
    bool singleComponent = BuildShellWithTargetsFromUV(sd.shell, support, 1.0);

    if (!singleComponent)
        LOG_DEBUG << "Shell is not single component";

    // We use the existing texture coordinates as a starting point for the ARAP optimization.
    // For this reason we set the per-Wedge and per-Vertex UV coordinates of the shell mesh to
    // the values in the `support` face group (i.e., the optimization area).
    for (unsigned i = 0; i < support.FN(); ++i) {
        auto& sf = sd.shell.face[i];
        auto& f = *(support.fpVec[i]);
        for (int j = 0; j < 3; ++j) {
            sf.WT(j) = f.V(j)->T();
            sf.V(j)->T() = sf.WT(j);
        }
    }

    // The previous align and merge operation could have introduced non-manifold vertices.
    //
    // Recall that a non-manifold vertex is one incident to at least two distinct sheets of faces.
    // To fix a non-manifold vertex, we need to create a copy for each sheet, displacing them from
    // one another.
    //
    // This process is handled via `SplitNonManifoldVertex`. Note that the function splits a
    // non-manifold vertex only for a pair of sheets, meaning that if the vertex is shared among
    // N sheets, we need to call the function N times. For this reason the function is enclosed
    // within a loop.
    while (tri::Clean<Mesh>::SplitNonManifoldVertex(sd.shell, 0.3))
        ;
    ensure(tri::Clean<Mesh>::CountNonManifoldEdgeFF(sd.shell) == 0);

    // The shell is cut along its UV seams, duplicating vertices at seam boundaries.
    CutAlongSeams(sd.shell);

    // We check that the shell mesh guarantees the following topological properties:
    //
    //  * its number of holes is major than zero. A shell mesh must have at least one boundary.
    //    Otherwise, it is a closed surface, which cannot be parameterized without cuts.
    //
    //  * its genus must be zero. A shell with handles (genus > 0) has a non-trivial topology
    //    that the ARAP solver cannot handle correctly.
    //
    // If at least one of the two properties does not hold, the process is aborted with the
    // status `FAIL_TOPOLOGY`.
    int nholes = tri::Clean<Mesh>::CountHoles(sd.shell);
    int genus = tri::Clean<Mesh>::MeshGenus(sd.shell);
    if (nholes == 0 || genus != 0) {
        return FAIL_TOPOLOGY;
    }

    // If the shell mesh has more than one boundary, we fill all the inner holes (via CloseHoles3D).
    // We only keep the outer border, which is recognized as the longest.
    if (singleComponent && nholes > 1)
        CloseHoles3D(sd.shell);

    // We set each shell vertex's 3D position to its UV coordinates.
    // The z-component of each point is always set to zero.
    SyncShellWithUV(sd.shell);

    PERF_TIMER_ACCUMULATE(t_optimize_build);

    LOG_DEBUG << "Optimizing...";
    ARAP arap(sd.shell);
    arap.SetMaxIterations(100);

    // Recall that `verticesWithinThreshold` contains all vertices close enough to the seam
    // (i.e., those that will be re-optimized).
    //
    // Any vertex in the optimization area that isn't in `verticesWithinThreshold` sits at
    // the boundary between the optimization area and the fixed region. These vertices will
    // be marked as SELECTED, pinned and set as constraints for the ARAP solver.
    for (unsigned i = 0; i < support.FN(); ++i) {
        for (int j = 0; j < 3; ++j) {
            if (sd.verticesWithinThreshold.find(support.fpVec[i]->V(j)) == sd.verticesWithinThreshold.end()) {
                ensure(sd.shell.face[i].IsHoleFilling() == false);
                sd.shell.face[i].V(j)->SetS();
            }
        }
    }

    // If the flag `fixIntesectingEdges` is set to true, then our current ARAP optimization
    // is a re-attempt after a failure caused by the presence of overlaps in the previous
    // solution. The vertices of the overlapped edges are stored in `sd.intersectionOpt`,
    // `sd.intersectionBoundary`, and `sd.intersectionInternal`. All of them are copied into
    // `fixedVerticesFromIntersectingEdges` and marked as `SELECTED`.
    //
    // If no new vertices are added compared to the previous attempt, there are no new
    // constraints, so re-trying the same identical ARAP procedure will be futile. We
    // return immediately with the status `_END`.
    if (fixIntersectingEdges) {
        unsigned fixedBefore = sd.fixedVerticesFromIntersectingEdges.size();
        for (auto hep : sd.intersectionOpt) {
            sd.fixedVerticesFromIntersectingEdges.insert(hep.first.fp->V0(hep.first.e));
            sd.fixedVerticesFromIntersectingEdges.insert(hep.first.fp->V1(hep.first.e));
            sd.fixedVerticesFromIntersectingEdges.insert(hep.second.fp->V0(hep.second.e));
            sd.fixedVerticesFromIntersectingEdges.insert(hep.second.fp->V1(hep.second.e));
        }
        for (auto hep : sd.intersectionBoundary) {
            HalfEdge he = hep.first;
            sd.fixedVerticesFromIntersectingEdges.insert(he.fp->V0(he.e));
            sd.fixedVerticesFromIntersectingEdges.insert(he.fp->V1(he.e));
        }
        for (auto hep : sd.intersectionInternal) {
            HalfEdge he = hep.first;
            sd.fixedVerticesFromIntersectingEdges.insert(he.fp->V0(he.e));
            sd.fixedVerticesFromIntersectingEdges.insert(he.fp->V1(he.e));
        }
        if (fixedBefore == sd.fixedVerticesFromIntersectingEdges.size())
            return _END;

        for (unsigned i = 0; i < support.FN(); ++i) {
            for (int j = 0; j < 3; ++j) {
                if (sd.fixedVerticesFromIntersectingEdges.find(support.fpVec[i]->V(j)) != sd.fixedVerticesFromIntersectingEdges.end())
                    sd.shell.face[i].V(j)->SetS();
            }
        }
    }


    // We have marked as SELECTED all pinned vertices that will be interpreted as constraints
    // by the ARAP solver. These will be explicitly set as constraints of the solver via  the
    // functon `FixSelectedVertices`.
    //
    // If the number of SELECTED vertices is minor than two, the optimization
    // procedure becomes impractical. We need at least two fixed vertices such that the ARAP system
    // removes both the translational and rotational degrees of freedom.
    //
    // In this case we search for a convenient edge, whose distance between its original position and
    // target one is within the tolerance (via `FixRandomEdgeWithinTolerance`). Found, we pick its
    // endpoints as constraints.
    int nfixed = arap.FixSelectedVertices();
    LOG_DEBUG << "Fixed " << nfixed << " vertices";
    double tol = 0.02;
    while (nfixed < 2) {
        LOG_DEBUG << "Not enough selected vertices found, fixing random edge with tolerance " << tol;
        nfixed += arap.FixRandomEdgeWithinTolerance(tol);
        tol += 0.02;
    }
    ensure(nfixed > 0);

    // The ARAP solver will manipulate all the UV coordinates from non-fixed vertices to minimize
    // the ARAP energy relative to the target shape. The result will be stored in as a SolveInfo
    // struct in `sd.si`. Among the fields in the struct, `numericalError` indicates whether the
    // linear system solver failed numerically or not.
    LOG_DEBUG << "Solving...";
    sd.si = arap.Solve();

    PERF_TIMER_ACCUMULATE_FROM_PREVIOUS(t_optimize_arap);

    // After founding the solution, the optimized per-Vertex and per-Vertex UV coordinates are
    // synced back to the original mesh's faces.
    //
    // Note that we update only non-hole filling faces, since the other where only constructed
    // for the optimization procedure.
    SyncShellWithUV(sd.shell);

    LOG_DEBUG << "Syncing chart...";

    // The ARAP energy of the optimization area is measured again (if no numerical error occurred).
    // This new value is compared against the pre-optimization baseline to determine whether the
    // optimization improved or worsened the UV quality.
    //
    // If a deterioration happened, the operation failed with status `FAIL_DISTORTION_LOCAL`.
    ensure(HasFaceIndexAttribute(sd.shell));
    auto ia = GetFaceIndexAttribute(sd.shell);
    for (auto& sf : sd.shell.face) {
        if (!sf.IsHoleFilling()) {
            auto& f = (graph->mesh).face[ia[sf]];
            for (int k = 0; k < 3; ++k) {
                f.WT(k).P() = sf.V(k)->T().P();
                f.V(k)->T().P() = sf.V(k)->T().P();
            }
        }
    }

    if (!sd.si.numericalError)
        ARAP::ComputeEnergyFromStoredWedgeTC(support.fpVec, graph->mesh, &sd.outputArapNum, &sd.outputArapDenom);

    PERF_TIMER_ACCUMULATE(t_optimize);

    // If the solver encountered a numerical error, `FAIL_NUMERICAL_ERROR` is returned, otherwise
    // the optimization completed successfully.
    return sd.si.numericalError ? FAIL_NUMERICAL_ERROR : PASS;
}

static bool SeamInterceptsOptimizationArea(ClusteredSeamHandle csh, const SeamData& sd)
{
    const SeamMesh& sm = csh->sm;
    for (auto sh : csh->seams) {
        for (int i : sh->edges) {
            const SeamEdge& edge = sm.edge[i];
            if ((sd.optimizationArea.find(edge.fa) != sd.optimizationArea.end()) || (sd.optimizationArea.find(edge.fb) != sd.optimizationArea.end()))
                return true;
        }
    }
    return false;
}

static void AcceptMove(const SeamData& sd, AlgoStateHandle state, GraphHandle graph, const AlgoParameters& params)
{
    PERF_TIMER_START;

    if (min_energy > sd.si.finalEnergy)
        min_energy = sd.si.finalEnergy;
    if (max_energy < sd.si.finalEnergy)
        max_energy = sd.si.finalEnergy;

    state->changeSet.insert(sd.optimizationArea.begin(), sd.optimizationArea.end());

    std::vector<SeamHandle> shared;
    std::set<ClusteredSeamHandle> sharedClusters; // clusters that can be aggregated after the merge
    std::set<ClusteredSeamHandle> independentClusters; // clusters not directly impacted by the merge

    std::set<ClusteredSeamHandle> selfClusters;

    if (sd.a != sd.b) {
        // ``disjoint'' seams, i.e. seams between B and C with C not in N(a)
        // are inherited by A
        for (auto csh : state->chartSeamMap[sd.b->id]) {
            ChartPair p = GetCharts(csh, graph);
            ChartHandle c = (p.first == sd.b) ? p.second : p.first;
            if (c == sd.a || c == sd.b) {
                selfClusters.insert(csh);
            } else if (sd.a->adj.find(c) == sd.a->adj.end()) {
                independentClusters.insert(csh);
            } else {
                ensure(c->adj.find(sd.a) != c->adj.end());
                ensure(c->adj.find(sd.b) != c->adj.end());
                ensure(sharedClusters.count(csh) == 0);
                sharedClusters.insert(csh);
                for (auto sh : csh->seams)
                    shared.push_back(sh);
            }
        }

        // we also need to recompute the cost of seams between A and C with C not in N(b)
        for (auto csh : state->chartSeamMap[sd.a->id]) {
            ChartPair p = GetCharts(csh, graph);
            ChartHandle c = (p.first == sd.a) ? p.second : p.first;
            if (c == sd.a || c == sd.b) {
                selfClusters.insert(csh);
            } else if (sd.b->adj.find(c) == sd.b->adj.end()) {
                independentClusters.insert(csh);
            } else {
                ensure(c->adj.find(sd.a) != c->adj.end());
                ensure(c->adj.find(sd.b) != c->adj.end());
                ensure(sharedClusters.count(csh) == 0);
                sharedClusters.insert(csh);
                for (auto sh : csh->seams)
                    shared.push_back(sh);
            }
        }

        /*
        for (auto x : std::set<ChartHandle>{sd.a, sd.b}) {
            for (auto csh : state->chartSeamMap[x->id]) {
                ChartPair p = GetCharts(csh, graph);
                ChartHandle c = (p.first == x) ? p.second : p.first;
                if ((sd.a->adj.find(c) != sd.a->adj.end()) && (sd.b->adj.find(c) != sd.b->adj.end())) {
                    ensure(c != sd.a);
                    ensure(c != sd.b);
                    ensure(sharedClusters.count(csh) == 0);
                    sharedClusters.insert(csh);
                    for (auto sh : csh->seams)
                        shared.push_back(sh);
                }
            }
        }
        */

        // update the MeshGraph object
        for (auto fptr : sd.b->fpVec)
            fptr->id = sd.a->Fp()->id;
        sd.a->fpVec.insert(sd.a->fpVec.end(), sd.b->fpVec.begin(), sd.b->fpVec.end());

        sd.a->adj.erase(sd.b);
        for (auto c : sd.b->adj) {
            if (c != sd.a) { // chart a is now (if it wasn't already) adjacent to c
                c->adj.erase(sd.b);
                c->adj.insert(sd.a);
                sd.a->adj.insert(c);
            }
        }
        graph->charts.erase(sd.b->id);

        // update state
        state->chartSeamMap.erase(sd.b->id);
        std::set<RegionID>& failed_b = state->failed[sd.b->id];
        state->failed[sd.a->id].insert(failed_b.begin(), failed_b.end());
        state->failed.erase(sd.b->id);
    } else {
        // if removing a non-disconnecting seam then all the clusters are independent
        independentClusters.insert(state->chartSeamMap[sd.b->id].begin(), state->chartSeamMap[sd.b->id].end());
        independentClusters.erase(sd.csh);
    }

    // invalidate cache
    sd.a->ParameterizationChanged();

    // update current UV border length
    double deltaUVBorderLength = sd.a->BorderUV() - sd.inputUVBorderLength;
    state->currentUVBorderLength += deltaUVBorderLength;

    // update atlas energy
    state->arapNum += (sd.outputArapNum - sd.inputArapNum);
    state->arapDenom += (sd.outputArapDenom - sd.inputArapDenom);

    if (state->failed[sd.a->id].count(sd.b->id) > 0)
        retry_success++;

    // Erase seam
    EraseSeam(sd.csh, state, graph);
    state->penalty.erase(sd.csh);

    for (auto csh : independentClusters) {
        auto it = state->status.find(csh);
        ensure(it != state->status.end());

        CheckStatus clusterStatus = it->second;
        ensure(clusterStatus != PASS);

        CostInfo::MatchingValue mv = state->mvalue[csh];
        double oldCost = state->cost[csh];

        EraseSeam(csh, state, graph);

        bool invalidate = (clusterStatus == CheckStatus::FAIL_GLOBAL_OVERLAP_BEFORE)
                || (clusterStatus == CheckStatus::FAIL_GLOBAL_OVERLAP_AFTER_OPT)
                || (clusterStatus == CheckStatus::FAIL_GLOBAL_OVERLAP_AFTER_BND)
                || (clusterStatus == CheckStatus::FAIL_GLOBAL_OVERLAP_UNFIXABLE && !SeamInterceptsOptimizationArea(csh, sd))
                || (clusterStatus == CheckStatus::FAIL_TOPOLOGY);

        if (invalidate || (params.ignoreOnReject && mv == CostInfo::REJECTED))
            InvalidateCluster(csh, state, graph, clusterStatus, 1.0);
        else if (oldCost != Infinity())
            MarkClusterDirty(csh, oldCost, mv, state, graph); // defer ComputeCost to pop time
        else {
            recompute_infeasible++;
            double t_i = perf.timer.TimeElapsed();
            InsertNewClusterInQueue(csh, state, graph, params); // infeasible-stale: recompute now
            time_infeasible += perf.timer.TimeElapsed() - t_i;
        }
    }

    for (auto csh : sharedClusters)
        EraseSeam(csh, state, graph);

    std::vector<ClusteredSeamHandle> cshvec = ClusterSeamsByChartId(shared);
    for (auto csh : cshvec) {
        recompute_fused++;
        double t_f = perf.timer.TimeElapsed();
        InsertNewClusterInQueue(csh, state, graph, params);
        time_fused += perf.timer.TimeElapsed() - t_f;
    }

    if (params.visitComponents) {
        // if potential islands are allowed to ignore the boundary length limit,
        // then check if any chart adjacent to a or b becomes a potential island
        // after the merge and activate the corresponding seams below threshold

        std::set<ClusteredSeamHandle> unfeasibleBoundaryAdj;
        for (ChartHandle c : sd.a->adj)
            for (auto csh : state->chartSeamMap[c->id])
                if (state->mvalue[csh] == CostInfo::MatchingValue::UNFEASIBLE_BOUNDARY)
                    unfeasibleBoundaryAdj.insert(csh);

        for (ClusteredSeamHandle csh : unfeasibleBoundaryAdj) {
            recompute_visit++;
            double t_v = perf.timer.TimeElapsed();
            EraseSeam(csh, state, graph);
            InsertNewClusterInQueue(csh, state, graph, params);
            time_visit += perf.timer.TimeElapsed() - t_v;
        }
    }

    PERF_TIMER_ACCUMULATE(t_accept);
}

static void RejectMove(const SeamData& sd, AlgoStateHandle state, GraphHandle graph, CheckStatus status)
{
    PERF_TIMER_START;

    Mesh& m = graph->mesh;

    // restore texture coordinates and indices
    RestoreChartAttributes(sd.a, m, sd.vertexinda.begin(), sd.texcoorda.begin());
    if (sd.a != sd.b)
        RestoreChartAttributes(sd.b, m, sd.vertexindb.begin(), sd.texcoordb.begin());

    sd.a->ParameterizationChanged();
    if (sd.a != sd.b)
        sd.b->ParameterizationChanged();

    // restore face-face topology
    SeamMesh& seamMesh = sd.csh->sm;
    for (SeamHandle sh : sd.csh->seams) {
        for (int iedge : sh->edges) {
            const SeamEdge& edge = seamMesh.edge[iedge];
            edge.fa->FFp(edge.ea) = edge.fa;
            edge.fa->FFi(edge.ea) = edge.ea;
            edge.fb->FFp(edge.eb) = edge.fb;
            edge.fb->FFi(edge.eb) = edge.eb;
        }
    }

    // restore vertex-face topology
    // iterate over emap, and split the lists according to the original topology
    // recall that we never touched any vertex topology attribute
    {
        for (Mesh::VertexPointer vp : sd.seamVertices) {
            vp->VFp() = 0;
            vp->VFi() = 0;
        }

        for (Mesh::FacePointer fptr : sd.vfTopologyFaceSet) {
            for (int i = 0; i < 3; ++i) {
                if (sd.seamVertices.find(fptr->V(i)) != sd.seamVertices.end()) {
                    (*fptr).VFp(i) = (*fptr).V(i)->VFp();
                    (*fptr).VFi(i) = (*fptr).V(i)->VFi();
                    (*fptr).V(i)->VFp() = &(*fptr);
                    (*fptr).V(i)->VFi() = i;
                }
            }
        }
    }

    EraseSeam(sd.csh, state, graph);

    InvalidateCluster(sd.csh, state, graph, status, PENALTY_MULTIPLIER);
    if (sd.a != sd.b)
        state->failed[sd.a->id].insert(sd.b->id);

    PERF_TIMER_ACCUMULATE(t_reject);
}

static void EraseSeam(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph)
{
    ensure(csh->size() > 0);

    state->dirty.erase(csh); // any pending dirty flag is void once the seam is erased

    std::size_t n = state->cost.erase(csh);
    ensure(n > 0);

    n = state->transform.erase(csh);
    ensure(n > 0);

    n = state->status.erase(csh);
    ensure(n > 0);

    n = state->mvalue.erase(csh);
    ensure(n > 0);

    ChartPair charts = GetCharts(csh, graph);

    // the following check are needed because AcceptMove() may erase seams after
    // fiddling with the chartSeamMap...
    if (state->chartSeamMap.find(charts.first->id) != state->chartSeamMap.end())
        state->chartSeamMap[charts.first->id].erase(csh);

    if (state->chartSeamMap.find(charts.second->id) != state->chartSeamMap.end())
        state->chartSeamMap[charts.second->id].erase(csh);

    // erase seam from endpoint map
    std::set<int> endpoints = GetEndpoints(csh);
    for (auto vi : endpoints) {
        unsigned n = state->emap[vi].erase(csh);
        ensure(n > 0);
    }
}

static void InvalidateCluster(ClusteredSeamHandle csh, AlgoStateHandle state, GraphHandle graph, CheckStatus status, double penaltyMultiplier)
{
    ColorizeSeam(csh, statusColor[status]);

    CostInfo ci;
    ci.cost = Infinity();
    ci.mvalue = CostInfo::REJECTED;
    ci.matching = MatchingTransform::Identity();

    state->queue.push(std::make_pair(csh, ci.cost));
    state->cost[csh] = Infinity();
    state->transform[csh] = ci.matching;
    state->status[csh] = status;
    state->mvalue[csh] = ci.mvalue;

    ChartPair p = GetCharts(csh, graph);
    state->chartSeamMap[p.first->id].insert(csh);
    state->chartSeamMap[p.second->id].insert(csh);

    // add penalty if the cluster is later re-evaluated
    double penalty = GetPenalty(csh, state);
    state->penalty[csh] = penalty * penaltyMultiplier;

    // add cluster to endpoint map
    std::set<int> endpoints = GetEndpoints(csh);
    for (auto vi : endpoints)
        state->emap[vi].insert(csh);
}

static void RestoreChartAttributes(ChartHandle c, Mesh& m, std::vector<int>::const_iterator itvi,  std::vector<vcg::Point2d>::const_iterator ittc)
{
    for (auto fptr : c->fpVec) {
        for (int i = 0; i < 3; ++i) {
            fptr->V(i) = &m.vert[*itvi++];
            fptr->V(i)->T().P() = *ittc;
            fptr->WT(i).P() = *ittc++;
        }
    }
}

/*!
 * Given a clustered seam whose UV boundaries are too geometrically incompatible to be aligned
 * under a rigid transformation (i.e. ComputeCost returned UNFEASIBLE_MATCHING), attempts to
 * find a shorter contiguous seam that achieves a feasible alignment.
 *
 * Two candidate reductions are constructed:
 *  - FORWARD REDUCTION: retains the first edges of the seam, starting from its beginning.
 *  - BACKWARD REDUCTION: retains the last edges of the seam, starting from its end.
 *
 * The candidates' length must not exceed `reductionFactor * totlen`
 *
 * Finally, among the computed candidates we return the lower cost one.
 *
 * Note that as a side effect, `csh` is modified in place: its seam chain list is replaced with the
 * winning reduction.
 *
 * This function is designed to be called iteratively: if the returned CostInfo still has
 * mvalue == UNFEASIBLE_MATCHING, the caller will invoke it again on the already-reduced `csh`,
 * progressively shortening the seam until a satisfiable alignment is found.
 *
 * @param csh: the clustered seam to reduce. Modified in place to contain the winning reduction.
 * @param state: the current state of the Texture Defragmentation procedure, used to retrieve
 *               the penalty associated with this seam.
 * @param graph: the parametrization graph, providing access to chart geometry and adjacency.
 * @param params: user-defined parameters for the Texture Defragmentation procedure. We will use the
 *                parameter `reductionFactor`, which determines the maximum 3D length of each
 *                candidate as a fraction of the full seam's total 3D length.
 * @return the CostInfo of the winning reduced seam.
 */
static CostInfo ReduceSeam (
    ClusteredSeamHandle csh,
    AlgoStateHandle state,
    GraphHandle graph,
    const AlgoParameters& params) {

    // Basic data initialization.
    // We compute the total 3D length of the full seam cluster. It will be used together with `params.reductionFactor`
    // to determine the maximum 3D length for our candidate reduction.
    //
    // `sm` is a SeamMesh instance keeping the raw seam edges. It will be used during the candidates construction for
    // accessing edges' data (geometry and topology).
    //
    // The candidates are declared as `fwd` and `bwd`: `fwd` visits edges from the start of the seam chain list; `bwd`
    // visit edges from the end.
    double totlen = ComputeSeamLength3D(csh);
    SeamMesh& sm = csh->sm;
    ClusteredSeamHandle reduced = nullptr;
    ClusteredSeamHandle fwd, bwd;

    // ================= FORWARD REDUCTION =================
    // Constructs a candidate reduction by considering edges from the beginning of the seam cluster.
    // The outer loop iterates over seam chains in forward order, such that for each chain, edges are
    // visited in forward order and their length is accumulated into `lenfwd`.
    // The construction stops as soon as `lenfwd` reaches the maximum allowed length `reductionFactor * totlen`.
    {
        fwd = std::make_shared<ClusteredSeam>(sm);
        double lenfwd = 0;
        auto seamHandleIt = csh->seams.begin();
        while (lenfwd < params.reductionFactor * totlen && seamHandleIt != csh->seams.end()) {
            SeamHandle sh  = *seamHandleIt;
            SeamHandle shnew = std::make_shared<Seam>(sm);

            // `visited` is a map data structure counting for each vertex in the truncated chain the number
            // of edges incident to it.
            //
            // Recall that the polyline structure guarantees that internal vertices have exactly two incident edges, while
            // edpoints appear in exactly one.
            //
            // This property is exploited at the end to identify within the map the two endpoints of the chain.
            std::map<SeamMesh::VertexPointer, int> visited;

            for (int e : sh->edges) {
                if (lenfwd >= params.reductionFactor * totlen)
                    break;
                shnew->edges.push_back(e);
                visited[sm.edge[e].V(0)]++;
                visited[sm.edge[e].V(1)]++;
                lenfwd += (sm.edge[e].P(0) - sm.edge[e].P(1)).Norm();
            }

            if (shnew->edges.size() == sh->edges.size()) {
                shnew->endpoints = sh->endpoints;
            } else {
                for (auto& entry : visited) {
                    if (entry.second == 1) {
                        shnew->endpoints.push_back(tri::Index(sm, entry.first));
                    }
                }

                // This final check ensures that `endpoints[0]` corresponds to the start of the edge sequence, i.e. it
                // matches one of the two vertices of the first edge in the short chain.
                //
                // If `endpoint[0]` does not match either vertex of the first edge, the two endpoints are swapped.
                if (tri::Index(sm, sm.edge[shnew->edges.front()].V(0)) != (unsigned) shnew->endpoints.front()
                        && tri::Index(sm, sm.edge[shnew->edges.front()].V(1)) != (unsigned) shnew->endpoints.front()) {
                    std::reverse(shnew->endpoints.begin(), shnew->endpoints.end());
                }
            }

            fwd->seams.push_back(shnew);
        }
    }

    // ================= BACKWARD REDUCTION =================
    // Constructs a candidate reduction by considering edges from the end of the seam cluster.
    // The outer loop iterates over seam chains in forward order, such that for each chain, edges are
    // visited in forward order and their length is accumulated into `lenbwd`.
    // The construction stops as soon as `lenbwd` reaches the maximum allowed length `reductionFactor * totlen`.
    {
        bwd = std::make_shared<ClusteredSeam>(sm);
        double lenbwd = 0;
        auto seamHandleIt = csh->seams.rbegin();
        while (lenbwd < params.reductionFactor * totlen && seamHandleIt != csh->seams.rend()) {
            SeamHandle sh  = *seamHandleIt;
            SeamHandle shnew = std::make_shared<Seam>(sm);

            // `visited` is a map data structure counting for each vertex in the truncated chain the number
            // of edges incident to it.
            //
            // Recall that the polyline structure guarantees that internal vertices have exactly two incident edges, while
            // edpoints appear in exactly one.
            //
            // This property is exploited at the end to identify within the map the two endpoints of the chain.
            std::map<SeamMesh::VertexPointer, int> visited;

            for (auto ei = sh->edges.rbegin(); ei != sh->edges.rend(); ++ei) {
                if (lenbwd >= params.reductionFactor * totlen)
                    break;
                shnew->edges.push_back(*ei);
                visited[sm.edge[*ei].V(0)]++;
                visited[sm.edge[*ei].V(1)]++;
                lenbwd += (sm.edge[*ei].P(0) - sm.edge[*ei].P(1)).Norm();
            }
            std::reverse(shnew->edges.begin(), shnew->edges.end());

            if (shnew->edges.size() == sh->edges.size()) {
                shnew->endpoints = sh->endpoints;
            } else {
                for (auto& entry : visited) {
                    if (entry.second == 1) {
                        shnew->endpoints.push_back(tri::Index(sm, entry.first));
                    }
                }

                // This final check ensures that `endpoints[0]` corresponds to the start of the edge sequence, i.e. it
                // matches one of the two vertices of the first edge in the short chain.
                //
                // If `endpoint[0]` does not match either vertex of the first edge, the two endpoints are swapped.
                if (tri::Index(sm, sm.edge[shnew->edges.front()].V(0)) != (unsigned) shnew->endpoints.front()
                        && tri::Index(sm, sm.edge[shnew->edges.front()].V(1)) != (unsigned) shnew->endpoints.front()) {
                    std::reverse(shnew->endpoints.begin(), shnew->endpoints.end());
                }
            }

            bwd->seams.push_back(shnew);
        }
        std::reverse(bwd->seams.begin(), bwd->seams.end());
    }

    // Finally, we compute the cost of the two candidates, selecting the one with the lower cost.
    // As a side effect, `csh` is replaced with the chosen candidate.
    // The CostInfo of the winning candidate is returned.
    CostInfo cfwd = ComputeCost(fwd, graph, params, GetPenalty(csh, state));
    CostInfo cbwd = ComputeCost(bwd, graph, params, GetPenalty(csh, state));
    if (cfwd.cost < cbwd.cost) {
        csh->seams = fwd->seams;
        return cfwd;
    } else {
        csh->seams = bwd->seams;
        return cbwd;
    }
}

