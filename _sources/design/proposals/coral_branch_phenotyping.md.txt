# Coral Branch Phenotyping (curve-skeleton port)

Porting Yuri Andraccio's `filter_curvature_skeleton` from his fork of classic MeshLab
into meshlab2. The plugin implements the workflow in the preprint *Leveraging 3D
geometric processing for the phenotyping of branching corals* (Scientific Reports draft,
2026): mean-curvature skeleton → pruning → Hack-order branch hierarchy → cutting into
branches → per-branch measurement.

**Status: not implemented. This is the mapping table, awaiting approval.** Nothing has
been moved or written yet.

See also: [Vocabulary](../vocabulary.md), [Filter Organization](../filter_organization.md),
[Adding a Filter](../adding_a_filter.md), [Edge Support](edge_support.md) (the skeleton
is a polyline layer).

## Source, measured

`github.com/Yurand2000/meshlab`, branch `devel` @ `b7d86b2` (2025-03-08), directory
`src/meshlabplugins/filter_curvature_skeleton`. Line counts are `wc -l` over `.h`/`.cpp`.

| Part | Lines |
|---|---:|
| 6 user filters (`filters/*`) | 3 350 |
| 11 debug filters (`test_filters/*`, built only with `-DCURVATURE_SKELETON_TEST`) | 3 020 |
| Shared algorithms (`common/`) | 2 944 |
| CGAL adapter (`cgalAdapter/`) | 326 |
| Plugin shell (`filter_curvature_skeleton.{h,cpp}`) | 293 |
| CMakeLists | 373 |
| **Total** | **10 306** |

It builds against a **forked vcglib**, `Yurand2000/vcglib` @ `99d054a` (26 commits
ahead of `cnr-isti-vclab/vcglib` `devel`, 95 behind). Net diff against upstream, from
the GitHub compare API:

| File | Diff | Adds | Called by the plugin |
|---|---|---|---|
| `vcg/complex/algorithms/stat.h` | +169 | `Stat::ComputeHackOrderNumbers`, `Stat::ComputeStrahlerNumbers` on an edge-mesh tree | Hack: `TreeSegmentationFilter_apply.cpp:118`. Strahler: never |
| `vcg/simplex/edge/topology.h` | +35 | `edge::VEEdgeCollapseNonManifold` | `common/SimplifySkeleton.imp.h:139` |
| `vcg/complex/algorithms/outline_support.h` | +1 −1 | Fixes a call to a renamed function (`Convert3DOutlinesToEdgeMesh` → `ConvertOutline3VecToEdgeMesh`) | indirectly |

Every other vcglib header the plugin includes exists in our `vcglib/` checkout.

## Filter mapping

17 source filters → **5 ported, 1 folded into another, 11 dropped** (5 + 1 + 11 = 17).

Names follow `Verb Object [(Backend)]` (vocabulary §6). Categories follow the closest
shipped precedent, cited in the evidence column.

| # | Current (display / python) | Target name · python | Category | Scope | Evidence | Verdict |
|---|---|---|---|---|---|---|
| 1 | *Skeletonize* · `skeletonize_mesh` | **Create Curve Skeleton by Mean Curvature Flow (CGAL)** · `create_curve_skeleton_by_mean_curvature_flow_cgal` | `Creation/Primitives` | SingleMesh → NewMeshes, tag `skeleton` | Wraps `CGAL::Mean_curvature_flow_skeletonization` (`cgalAdapter/CGALMeshSkeletonizer.h`). Output is an edge mesh. Precedent: every *Create Polyline from …* filter is `Creation/Primitives` + NewMeshes. Also writes a per-vertex `skeleton_index` onto the **source** mesh (`AlgorithmSkeletonize.cpp:28`) — see issue A | Port |
| 2 | *Prune Skeleton* · `prune_skeleton` | **Remove Curve Skeleton Branches** · `remove_curve_skeleton_branches` | `Meshing/Deletion` | SingleMesh (skeleton) → ModifyCurrentMesh | `Prune` is not in the lexicon; `Remove` is. Two modes in one filter: shorter than a length, or selected leaves (`PruneSkeletonFilter_params.cpp`). Rewrites `skeleton_index` on the original mesh (`PruneSkeleton.cpp:27`), so it modifies **two layers** — issue A | Port |
| 3 | *Mesh Segmentation* · `mesh_segmentation` | **Compute Branch Hierarchy from Curve Skeleton** · `compute_branch_hierarchy_from_curve_skeleton` | `Attribute/Scalar` | SingleMesh (surface) + `skeleton` mesh param → ModifyCurrentMesh, `FQ FA` | Stores per-face Hack order and branch tag (`hack_order`, `segmentation_tag`). Today it bakes face color (`F_POSTCONDS MM_FACECOLOR`), which breaks the "Compute never bakes color" rule. Target writes the order to face scalar and returns a `FaceQuality` hint. Root is marked by vertex quality == 0 (`TreeSegmentationFilter_params.cpp`) and branch swaps come from the selection | Port |
| 4 | *Polyline Cutting* · `polyline_cutting` | **Cut Mesh into Branches** · `cut_mesh_into_branches` | `Meshing/Remeshing` | SingleMesh → NewMeshes, tag `branch %1` | 1 011 lines, the largest filter: refines the label boundaries (smoothing, projection, fit-plane and separation weights), cuts, and optionally closes the holes. Precedent: *Cut Along Scalar Isocontour (TrueForm)* is `Meshing/Remeshing` + NewMeshes | Port |
| 5 | *Restore Hack Order Data* · `restore_hack_order_data` | — | — | — | Finds the pieces by regex on their layer labels (`Part\s+#(\d+);\s+Tag\s+(\d+)`), then relabels them and paints them by order (`RestoreHackOrderDataFilter_apply.cpp`). Under vocabulary §7 the framework owns layer names, so this cannot survive as written. #4 already knows every piece's tag, order and parent when it creates it | **Fold into #4**: write `branch_id`, `branch_order` and `parent_branch_id` as per-face attributes on each piece |
| 6 | *Measure Branches* · `branches_measure` | **Measure Branches** · `measure_branches` | `Measurement/Geometric` | WholeDocument → Information | Recomputes an MCF skeleton per piece, extends it to the tip (`BranchExtender.cpp`), and reports curved length, linear length, border length, area and volume (CSV header at `BranchMeasureFilter_apply.cpp:85`). Its *Save skeletons as meshes* option creates layers, which a Measurement filter may not do | Port. Drop the layer option, return `outputValues`, keep the CSV as a `filesave` parameter |
| 7–17 | 11 debug filters (Find Path, Compute Polylines, Refine Polyline, Cut on Polylines, Close Holes, …) | — | — | — | Compiled only with `-DCURVATURE_SKELETON_TEST`; they expose intermediate stages of #3 and #4 | **Drop**. Their checks become rows in `tests/test_filters.cpp` |

## Shared code mapping

| Current | Lines | Target | Verdict |
|---|---:|---|---|
| `cgalAdapter/CGALMeshConverter.h` | 111 | `filter_cgal` already has `copyCgalMeshToVcg` (`cgalfilterplugin.cpp:181`) and `buildTriangleSoup` | Reuse ours; delete theirs |
| `cgalAdapter/CGALMeshSkeletonizer.{h,cpp}` | 215 | Stays as the MCF wrapper | Port |
| `common/TemplateFilter.*`, `filter_curvature_skeleton.*` | 437 | Replaced by `filters.json` and our dispatch | Drop |
| `common/Utils.*` (`tryGetSkeletonMeshIndex` finds any layer whose label contains `"skel"`; `tryGetOriginalMeshIndex` works only when the document has exactly 2 layers) | 207 | Explicit `mesh` parameters | Drop; issue B |
| `common/SkeletonMesh.h` (its own vertex/edge types with `VEAdj`/`EEAdj`) | 70 | Check whether our `VCGMesh` edge side can carry the adjacency it needs, otherwise keep it as a private working type | Decide during port |
| `ComputeBranches`, `BranchTagger`, `PruneSkeleton`, `SimplifySkeleton`, `BranchExtender`, `MeshBorderPolyline`, `EdgeMeshUtils`, `PolylineMesh` | 2 230 | Plugin-private algorithms | Port. Any piece that is generic edge-mesh machinery is a candidate for vcglib |
| vcglib `Stat::ComputeHackOrderNumbers` / `ComputeStrahlerNumbers` | 169 | Upstream vcglib `devel` | Upstream through our process (commit, push, gitlink bump). Strahler costs nothing to expose as an option on #3; the paper's Discussion names it for species that grow by bisection |
| vcglib `edge::VEEdgeCollapseNonManifold` | 35 | Upstream vcglib | Upstream |
| vcglib `outline_support.h` fix | 1 | Upstream vcglib | Still broken upstream: `outline_support.h:290` calls the nonexistent `Convert3DOutlinesToEdgeMesh`, which goes unnoticed only because nothing instantiates that overload. Upstream the fix |

**Size estimate after the port:** about 4.5k lines of plugin code (the 5 filters minus
their boilerplate, plus the algorithms in `common/`), and about 200 lines in vcglib.

## Open issues (need a decision before code)

**A. State shared across layers.** The mesh-to-skeleton correspondence lives on the
surface mesh as a per-vertex float holding a skeleton vertex **index**. Two framework
invariants break it:

- `outputModifies` is honored only for `ModifyCurrentMesh`
  (`meshfilterplugin.h:244`). #1 (NewMeshes) and #2 (current = skeleton) both write to
  the *other* layer, so undo would not capture that write.
- The framework compacts the current mesh after every filter. Pruning deletes skeleton
  vertices, the survivors are renumbered, and every stored index goes stale.

Candidate fix: give each skeleton vertex a stable `int` id attribute (the fork already
has one, `original_index`). The surface stores ids instead of indices, and #2 stops
touching the surface: #3 resolves a pruned id to its surviving ancestor. **`int`
attributes now survive undo, Duplicate Layer and addMesh** (added 2026-09-27; before that
`deepCopyMesh` copied only `float` and `Point3f`), and the expression filters can read
them. That still leaves #1 annotating its source layer, which needs either framework
support for "NewMeshes + modifies source", or storing the correspondence on the skeleton
side.

**B. Finding the layers.** Replace the label heuristic with explicit `mesh` parameters
(77 of our descriptors already use `"type": "mesh"`), defaulted to the current layer and
to the most recent layer tagged `skeleton`.

**C. Named attributes are not saved.** Per-face labels live in memory and survive undo,
but `io_vcg` does not write custom attributes to PLY. A saved-and-reopened project loses
the hierarchy and has to be recomputed. That is acceptable if #4's pieces are the saved
artifact, since they carry their identity in their layer names.

**D. Plugin placement.** Decision 1 splits plugins only by dependency. Only #1 and #6
need CGAL; #2–#4 need only vcglib. Options:
(a) put all five in `filter_cgal`, which already gates on CGAL and holds the converter;
(b) create a new family plugin `filter_skeleton`, gated on CGAL, with #2–#4 in it too.
Option (a) is less code; option (b) is the better home if more skeleton algorithms
arrive (e.g. a vcglib or libigl skeleton alongside CGAL's).

**E. Vocabulary extension.** `curve skeleton` was **approved on 2026-09-27** (vocabulary
§4, with its rejected synonyms and the reasons for each). `branch` is still open.

**F. Paper vs code.** Things the paper describes that the code does not:

- Branching angle, geotropic angle, order and parent are not in the CSV, which has only
  lengths, area and volume.
- Handle/loop removal before skeletonization (the paper's Methods, after Alderighi et
  al.) is not in the plugin.

The first belongs in #6. For the second, ask Yuri where it lives.
