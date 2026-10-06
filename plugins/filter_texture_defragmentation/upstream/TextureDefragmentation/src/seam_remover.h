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

#ifndef SEAM_REMOVER_H
#define SEAM_REMOVER_H

#include <vector>
#include <memory>

#include <vcg/space/point3.h>
#include <vcg/space/point2.h>

#include "types.h"
#include "mesh.h"
#include "mesh_graph.h"
#include "matching.h"
#include "arap.h"

#include "seams.h"
#include "intersection.h"

typedef std::unordered_map<Mesh::VertexPointer, double> OffsetMap;

enum FilterType {
    TextureDefrag = 0,
    SmallIslandRemover
};

/*!
 * Container holding all user-specified parameters for the Texture Defragmentation
 * algorithm and its variants. Depending on the variant executed, the user defines
 * only some parameters, while others usually remain to their default value.
 *
 * `filterType` is a special parameter (never defined by a user) identifying the
 * current variant of Texture Defragmentation being run.
 *
 *  FP_TEXTURE_DEFRAG uses: matchingThreshold, offsetFactor, boundaryTolerance,
 *  distortionTolerance, globalDistortionThreshold, UVBorderLengthReduction,
 *  timelimit.
 *
 * FP_SMALL_CHARTS_REMOVER uses: minAreaThreshold, timelimit, ignoreOnReject,
 * targetTexCount, distortionTolerance, globalDistortionThreshold, boundaryTolerance,
 * matchingThreshold, expb, UVBorderLengthReduction.
 * `reduce` is forced to be true since small charts often have irregular boundaries
 * that are only feasible for shorter sub-seams.
 * When the UNSAFE distortionMode is chosen, the internal parameter skipOverlapChecks
 * is set to true. This parameter will avoid that any merge operation is rejected
 * for the presence of overlaps and folds.
 *
 */
struct AlgoParameters {
    // `filterType` is always set by the program, never by the user
    int filterType                   = 0;

    // === FP_TEXTURE_DEFRAG parameters ===
    double matchingThreshold         = 2.0;
    double offsetFactor              = 5.0;
    double UVBorderLengthReduction   = 0.0;

    // === FP_SMALL_CHARTS_REMOVER parameters ===
    double maxThreshold          = 0.0;
    // QMeshLab: with this on, the island merger takes its candidates from the face
    // selection instead of from maxThreshold. Same rule either way -- a merge is allowed
    // when at least one of the two charts qualifies -- so a selected island dissolves into
    // its neighbour and unselected pairs are left alone.
    bool mergeSelectedIslands    = false;
    int targetTexCount               = 0;

    // === SHARED PARAMETERS ===
    double timelimit                 = 0;
    double distortionTolerance       = 0.5;
    double globalDistortionThreshold = 0.025;
    double boundaryTolerance         = 0.2;

    // === INTERNAL PARAMETERS (not exposed to the user) ===
    double reductionFactor           = 0.8;
    bool   reduce                    = false;
    bool   visitComponents           = true;
    double expb                      = 1.0;
    bool   ignoreOnReject            = false;
    // If true, all UV overlap checks are bypassed.
    // Used by UNSAFE distortion mode in FP_SMALL_ISLANDS_REMOVER.
    bool skipOverlapChecks = false;
};

struct SeamData {
    ClusteredSeamHandle csh;

    ChartHandle a;
    ChartHandle b;

    std::vector<vcg::Point2d> texcoorda;
    std::vector<vcg::Point2d> texcoordb;
    std::vector<int> vertexinda;
    std::vector<int> vertexindb;

    std::unordered_set<Mesh::VertexPointer> seamVertices;
    std::unordered_set<Mesh::FacePointer> vfTopologyFaceSet;

    std::map<Mesh::VertexPointer, Mesh::VertexPointer> mrep;
    std::map<SeamMesh::VertexPointer, std::vector<Mesh::VertexPointer>> evec;

    std::unordered_set<Mesh::VertexPointer> verticesWithinThreshold;
    std::unordered_set<Mesh::FacePointer> optimizationArea;
    std::vector<vcg::Point2d> texcoordoptVert;
    std::vector<vcg::Point2d> texcoordoptWedge;

    double inputNegativeArea;
    double inputAbsoluteArea;

    double inputUVBorderLength;

    double inputArapNum;
    double inputArapDenom;

    double outputArapNum;
    double outputArapDenom;

    ARAPSolveInfo si;

    Mesh shell;

    std::vector<HalfEdgePair> intersectionOpt;
    std::vector<HalfEdgePair> intersectionBoundary;
    std::vector<HalfEdgePair> intersectionInternal;

    std::unordered_set<Mesh::VertexPointer> fixedVerticesFromIntersectingEdges;

    SeamData() : a{nullptr}, b{nullptr}, inputNegativeArea{0}, inputAbsoluteArea{0} {}
};

// enum of the possible outcomes for safety checks when performing merge operations
enum CheckStatus {
    PASS=0,
    FAIL_LOCAL_OVERLAP,
    FAIL_GLOBAL_OVERLAP_BEFORE,
    FAIL_GLOBAL_OVERLAP_AFTER_OPT, // border of the optimization area self-intersects
    FAIL_GLOBAL_OVERLAP_AFTER_BND, // border of the optimzation area hit the fixed border
    FAIL_DISTORTION_LOCAL,
    FAIL_DISTORTION_GLOBAL,
    FAIL_TOPOLOGY,  // shell genus is > 0 or shell is closed
    FAIL_NUMERICAL_ERROR,
    UNKNOWN,
    FAIL_GLOBAL_OVERLAP_UNFIXABLE,
    _END
};

/*!
 * Describes the outcome of a merge operation as evaluated by ComputeCost.
 *
 * Only FEASIBLE produces a finite cost and allows the merge to proceed.
 * All other entries explain why the operation was assigned infinite cost
 * and excluded from the greedy procedure.
 *
 * ZERO_AREA applies across all variants: if either chart has zero area
 * in UV or 3D space, no meaningful merge can be computed.
 *
 * Depending on the variant there are separate cases:
 *
 * FP_TEXTURE_DEFRAG uses:
 *
 *   - UNFEASIBLE_BOUNDARY: the seam covers too small a fraction of either
 *     chart's UV boundary, meaning the charts barely touch and merging them
 *     would produce poorly shaped charts.
 *
 *   - UNFEASIBLE_MATCHING: the UV boundaries of the two charts are too
 *     geometrically incompatible to align under a rigid transformation within
 *     the allowed error threshold. If `reduce` is active, ReduceSeam attempts
 *     to find a shorter sub-seam that resolves this before giving up.
 *
 *   - REJECTED: the merge was attempted but reverted due to post-optimization
 *     failures (overlap or distortion). Set by the main loop, not ComputeCost.
 *
 * FP_SMALL_CHARTS_REMOVER uses:
 *   - OVER_UV_AREA: both charts have UV area >= minAreaThreshold, meaning
 *     neither qualifies as a small chart, thus they should not be merged
 */
struct CostInfo {
    enum MatchingValue {
        FEASIBLE = 0,
        ZERO_AREA,
        UNFEASIBLE_BOUNDARY,
        UNFEASIBLE_MATCHING,
        OVER_UV_AREA,
        REJECTED,
        _END
    };

    double cost;
    MatchingTransform matching;
    MatchingValue mvalue;
};

struct AlgoState {

    struct WeightedSeamCmp {
        bool operator()(const WeightedSeam& a, const WeightedSeam& b)
        {
            return a.second > b.second;
        }
    };

    std::priority_queue<WeightedSeam, std::vector<WeightedSeam>, WeightedSeamCmp> queue;
    std::unordered_map<ClusteredSeamHandle, double> cost;
    std::unordered_map<ClusteredSeamHandle, double> penalty;
    std::unordered_map<RegionID, std::set<ClusteredSeamHandle>> chartSeamMap;

    std::map<ClusteredSeamHandle, CheckStatus> status;

    std::map<int, std::set<ClusteredSeamHandle>> emap; // endpoint -> seams map

    std::unordered_map<ClusteredSeamHandle, MatchingTransform> transform; // the rigid matching computed for each currently active move
    std::unordered_map<ClusteredSeamHandle, CostInfo::MatchingValue> mvalue;

    // Lazy recompute set: clusters whose neighbor geometry changed but whose cost has not yet been refreshed.
    // They remain in the queue at their old stale cost and are recomputed only if/when they are popped.
    std::unordered_set<ClusteredSeamHandle> dirty;

    std::unordered_map<RegionID, std::set<RegionID>> failed;

    SeamMesh sm;
    std::set<Mesh::FacePointer> changeSet;

    double arapNum;
    double arapDenom;

    double inputUVBorderLength;
    double currentUVBorderLength;
};

void PrepareMesh(Mesh& m, int *vndup);

/*!
 * It constructs an AlgoStateHandle instance, containing all starting information
 * regarding the Texture Defragmentation process.
 *
 * The function computes the following fields of the object:
 * - `sm`: stores the seams present in the parametrization, grouped by their pair of charts.
 * - `inputUVBorderLength`: stores the amount of boundary length of the initial UV parametrization.
 * - `currentUVBorderLength`: will store the optimized amount of boundary length of the parametrization
 *                            during the execution of Texture Defragmentation. At the start is set equal
 *                            to `inputUVBorderLength`.
 * - `ndisconnecting`: counts the total number of seams placed over two distinct charts.
 * - `nself`: counts the total number of seams placed over only one chart (also called self-seams).
 *
 * @param graph: the UV-layout of the parametrization.
 * @param algoParameters: the user-defined parameters of Texture Defragmentation.
 * @return the constructed AlgoStateHandle instance.
 */
AlgoStateHandle InitializeState(GraphHandle graph, const AlgoParameters& algoParameters);

/*!
 * Implements a greedy best-fit search over the space of possible chart merges.
 * As a side-effect it updates the UV coordinates of the input mesh with the
 * computed optimized values.
 *
 * For each merge operation S we define a cost, declared as Appeal, computed as:
 *          Appeal(S) = ( Benefit(S) / Cost(S) ) * SizeBonus(S) * Backoff(S)
 * where:
 * - Benefit(S): determines how convenient is our operation in respect to
 *               to the overall number of seams decreased by it. This is
 *               measured by considering the size of the boundaries of the
 *               charts to be fused. We declare `L_A` and `L_N` as the
 *               summation of the lengths of the linked edges' length in
 *               both charts.
 *
 * - Cost(S): after rigidly aligning the two chart, if their linked vertices
 *            are distant, their displacement will be big, leading to a
 *            considerable distortion.
 *
 * - SizeBonus(S): gives priority to merge operations having charts with small area.
 *
 * - Backoff(S): penalizes merge operations that have failed multiple times.
 *
 * @param graph: the UV parametrization.
 * @param state: contains all data regarding the current execution of Texture Defragmentation.
 * @param params: the user-defined parameters of Texture Defragmentation.
 */
void GreedyOptimization(GraphHandle graph, AlgoStateHandle state, const AlgoParameters& params);

/*!
 * After all merge operations are done, this function prepares the final mesh
 * for being returned to the user. All data added to the mesh to ease
 * the optimization procedure is now removed.
 *
 * @param graph: the UV parametrization.
 * @param vndup: a pointer that will contain the total number
 *               of vertices **before** the cleaning procedure.
 */
void Finalize(GraphHandle graph, int *vndup);

#endif // SEAM_REMOVER_H
