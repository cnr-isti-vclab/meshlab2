# Open3D Scan Processing and Reconstruction

This document proposes a scoped Open3D integration for RGB-D scan processing,
multi-view registration and reconstruction in MeshLab. **Not implemented.** It
includes Open3D's independent implementations even where another MeshLab filter
performs the same operation. The RGB-D sequence pipeline is the largest new
workflow; point-cloud preparation, registration and reconstruction filters also
belong in MeshLab's algorithm archive.

See also: [Adding a Filter](../adding_a_filter.md),
[Filter Organization](../filter_organization.md),
[Data Model](../data_model.md), [Geogram Port](../history/geogram_port.md).

## Status and evidence

Surveyed on **2026-10-14** against the current Open3D documentation, selected
upstream C++ source files and this checkout's filter descriptors and adapters.
The documentation cited below describes Open3D 0.20-era APIs; the source links
currently target upstream `main`, so the precise release and source revision
must be pinned before implementation. This is a design and provenance study,
not a benchmark or build-compatibility result. No Open3D code has been compiled
or exercised in MeshLab.

A second pass on **2026-10-14** read `CMakeLists.txt`, `vcpkg.json` and
`3rdparty/find_dependencies.cmake` at tag **v0.20.0** (released 2026-09-16) and
measured this checkout's current dependency footprint. It found one fact that
dominates the cost of the whole plan and was absent from the first draft — see
*The dependency cost is VTK* — and one that removes most of the risk from the
RGB-D phase. Both are recorded below.

## Why Open3D, given current MeshLab filters

MeshLab already has a substantial scan-reconstruction toolbox. Its existing
filters estimate and smooth point-cloud normals, orient normals with CGAL,
align two layers with ICP, globally align overlapping layers, reconstruct by
Ball Pivoting (two implementations), Screened Poisson, CGAL Poisson, APSS/RIMLS,
and integrate visible layers through volumetric merging. Under the
[algorithm-archive rule](../filter_organization.md#4-meshlab-is-an-algorithm-archive--competing-implementations-are-a-feature),
overlap in *operation* does not disqualify a filter. A separate entry is
appropriate when Open3D supplies a distinct implementation; name it with an
`(Open3D)` suffix and explain how it differs. Omit an entry that only wraps the
same underlying implementation already exposed in MeshLab.

| Capability | Current MeshLab | Potential Open3D contribution |
|---|---|---|
| Point-cloud preparation | Sampling, simplification, normal estimation/smoothing/orientation and outlier selection exist | Open3D's voxel downsampling, radius/statistical outlier removal and local normal estimation are independent implementations worth separate entries |
| Pairwise alignment | ICP and global multi-layer alignment exist | Open3D point-to-point, point-to-plane and colored ICP, plus feature-based global registration, are alternative implementations and should be identifiable by name |
| Surface reconstruction from points | Two BPA implementations, CGAL alpha shape, Screened Poisson and CGAL Poisson exist | Open3D's own Ball Pivoting and Qhull-based alpha-shape implementations are archive candidates; its Poisson route uses Kazhdan's `PoissonRecon`, which the Screened Poisson plugin already embeds |
| Multiple RGB-D frames | Volumetric merging integrates visible layers, but does not expose the documented RGB-D odometry → pose graph → TSDF sequence workflow | A scan-sequence pipeline that estimates poses, refines loop/fragment alignment and integrates calibrated depth frames into a mesh is the clearest distinct feature |

Open3D's RGB-D reconstruction examples assume synchronized color/depth images
with known registration and camera intrinsics. They use frame odometry and
multiway registration to build/refine poses, then integrate depth into a
`ScalableTSDFVolume` and extract a triangle mesh. This differs from the existing
filter's use of already-loaded visible layers: it needs image/depth inputs,
calibration, frame poses and a trajectory/pose-graph representation.

This implementation distinction is visible in source. Open3D's
[`CreateFromPointCloudBallPivoting`][open3d-bpa] invokes its own `BallPivoting`
class with a list of radii. Its [`CreateFromPointCloudAlphaShape`][open3d-alpha]
starts from Qhull Delaunay tetrahedra and extracts the qualifying boundary;
the existing alpha-shape filter uses CGAL. By contrast, Open3D's
[`CreateFromPointCloudPoisson`][open3d-poisson] includes
`PoissonRecon/Src/*`; MeshLab's `filter_screened_poisson` also uses
`PoissonRecon::Reconstructor`. That is a shared core implementation, despite
different adapters and possibly different vendored revisions. Confirm exact
provenance after pinning Open3D, but do not propose a second Poisson menu entry
merely for the Open3D adapter.

## Proposed integration shape

Use one optional `filter_open3d` plugin, following the dependency-per-plugin
pattern. Keep Open3D types behind private adapters and translate at the plugin
boundary between MeshLab's vertex-only `TriMesh` point clouds / raster layers
and Open3D point clouds, RGB-D images, camera intrinsics and triangle meshes.
Preserve source layers; write alignment as layer transforms where possible and
produce reconstruction results as new tagged layers.

The plugin should not take a dependency on Python bindings. Expose a bounded,
reviewable set of C++ filters through the normal MeshLab registry and descriptor
schema. Use Open3D's CPU path first. CUDA/SYCL variants are not implied by the
presence of Open3D and should not be made a build or runtime requirement.

### The dependency cost is VTK

Measured at v0.20.0:

| | |
|---|---|
| Bundled third-party components in the source tree | **48** |
| Upstream's own `vcpkg.json` dependencies | **21** |
| Of those, already in this project's `vcpkg.json` | about 4 (`embree`, `tbb`, Eigen and zlib transitively) |
| This project's current dependency count | 15 |

Upstream 0.20 has a **vcpkg mode**: `if(OPEN3D_USE_VCPKG)` flips roughly twenty
`USE_SYSTEM_*` options ON, and the repository ships a top-level `vcpkg.json`.
That largely answers this document's own requirement that configure-time
dependency fetching be disallowed — the mechanism exists upstream and does not
have to be invented.

**But VTK is unconditional.** `3rdparty/find_dependencies.cmake` has exactly two
branches for it — `if(USE_SYSTEM_VTK)` finds it, `if(NOT USE_SYSTEM_VTK)`
imports it — and no third. There is no no-VTK build. Yet the only consumer in
the tree is `cpp/open3d/t/geometry/VtkUtils.cpp`, part of the **tensor API this
document explicitly defers**. Adopting Open3D therefore means adding one of the
largest C++ libraries in existence to this project's dependency set, to support
code the plan has already decided not to call.

Nothing else in the plan comes close to this in cost, and no filter choice
changes it. It is the question Phase 0 exists to answer.

Open3D's core project is MIT-licensed, but that does not settle the notices,
availability, binary size or redistribution conditions of its dependency graph
and optional components. Before choosing a source submodule or package route,
compare a pinned upstream build against MeshLab's vcpkg baseline, identify which
third-party components are enabled, and audit licenses for shipped binaries.
Do not allow configure-time dependency fetching. Record the exact upstream
revision, local patches, enabled options and notices in
`plugins/filter_open3d/UPSTREAM.md` if the integration proceeds.

Suggested structure:

```text
plugins/filter_open3d/
  CMakeLists.txt
  filters.json
  UPSTREAM.md
  open3dfilterplugin.h/.cpp
  open3dmeshadapter.h/.cpp       # point clouds and triangle meshes
  open3dregistration.h/.cpp      # ICP and feature initialization
  open3dpointcloud.h/.cpp        # sampling, outliers and normals
  open3drgbd.h/.cpp              # RGB-D inputs, calibration, frame poses
  open3dreconstruction.h/.cpp    # TSDF integration and extraction
```

Plugin id: `meshlab2.filter.open3d`; display name: `Open3D Scan Processing`.
Build gate: `MESHLAB2_PLUGIN_FILTER_OPEN3D`. These identifiers are proposals and
must be checked against repository conventions when implementation begins.

## ⚠ There is no Open3D package, and the build costs VTK — the mapping table

Measured 2026-10-14 against tag **v0.20.0**, unpacked locally and traced with a
script rather than read off directory names.

**`ports/open3d` does not exist in vcpkg** — not in our pinned baseline
(`37bb045f`) and not upstream either; `contents/ports/open3d` returns 404. The
route this document assumed, "pin a release and add it to `vcpkg.json`", was
never available. That leaves building Open3D from source, or vendoring part of
it.

### Why not build it

All 21 of Open3D's own vcpkg dependencies exist in our baseline, but only three
are already built:

| | |
|---|---|
| Already in our manifest | `embree`, `tbb`, `tinygltf` (3 of 21) |
| Already built in `vcpkg_installed` | `eigen3`, `openssl`, `zlib` (3) |
| Would have to be built | **15**, headlined by **VTK** |

The vcpkg `vtk` port is `9.3.0-pv5.12.1` with **26 dependencies**, and its
default features pull HDF5, NetCDF, PROJ, SQLite, libharu, libtheora and seacas,
plus `vtk-compile-tools` — so VTK is built essentially twice. And it cannot be
patched out cheaply: VTK is not confined to `VtkUtils.cpp`, with 28 references
in `t/geometry/TriangleMesh.cpp` and 3 in `t/geometry/PointCloud.cpp`. Removing
it means excising parts of the tensor API, which is owning a fork of their build.

### What we actually need: the mapping table

Entry points are the six public headers behind the Phase 1 and 2 filters. The
set was then **built**, and building corrected it twice — the figures below are
the ones that compile, link and run, not the ones first traced.

| Current (upstream v0.20.0) | Target | Measured evidence | Verdict |
|---|---|---|---|
| `cpp/open3d/geometry/` | `plugins/filter_open3d/upstream/open3d/geometry/` | 54 files, 18,304 lines (of 56 in the directory) | **vendor** |
| `cpp/open3d/pipelines/registration/` | `…/upstream/open3d/pipelines/registration/` | 27 files, 5,583 lines (of 27) | **vendor** |
| `cpp/open3d/utility/` | `…/upstream/open3d/utility/` | 26 files, 3,832 lines (of 37) | **vendor** |
| `cpp/open3d/camera/` | `…/upstream/open3d/camera/` | 6 files, 401 lines; reached from `geometry/ImageFactory.cpp` | **vendor** |
| `cpp/open3d/Macro.h` | `…/upstream/open3d/Macro.h` | 1 file, 110 lines | **vendor** |
| `3rdparty/tomasakeninemoeller/` | `…/upstream/3rdparty/tomasakeninemoeller/` | 2 headers, 522 lines; Möller tri-tri test for `geometry/IntersectionTest.cpp` | **vendor** |
| `cpp/open3d/core/` | — | 173 files; reached **only** through the 3 cut edges below | **exclude** |
| `cpp/open3d/t/` | — | 142 files; the VTK consumer | **exclude** |
| `cpp/open3d/visualization/` | — | 17 files; reached only via `t/geometry/TriangleMesh.h` → `rendering/Model.h` | **exclude** |
| **Total vendored** | | **116 files, 28,752 lines** (1.3 MB source, 1.6 MB archive) | |

Four files in the excluded-by-choice category, all of which would compile but
each of which costs something we do not want:

| Excluded file | Cost |
|---|---|
| `geometry/SurfaceReconstructionAlphaShape.cpp` | includes `core/Tensor.h`; the **only** file of the 38 candidates that reopens the `core/` cascade |
| `geometry/SurfaceReconstructionPoisson.cpp` | needs the vendored PoissonRecon, and this document already rules out a second Poisson entry |
| `utility/Download.cpp` | adds libcurl |
| `utility/Extract.cpp`, `utility/ExtractZIP.cpp` | add zlib/minizip plumbing |

#### ⚠ The first count was wrong, and building is what found it

The table originally said **51 files, 12,859 lines**. That figure was derived by
following `#include` edges, and it was never buildable. Two flaws, both found by
the compiler and the linker rather than by reading:

1. **A C++ library closes over symbol *definitions*, not only includes.**
   `PointCloud.h` declares `EstimateNormals`; the body is in
   `geometry/EstimateNormals.cpp`, a file no `#include` anywhere names. Likewise
   `CreateFromPointCloudBallPivoting` → `SurfaceReconstructionBallPivoting.cpp`,
   `Image::CreateFloatImage` → `ImageFactory.cpp`, and
   `TetraMesh::CreateFromPointCloud` → `TetraMeshFactory.cpp`. Pairing each
   header with its same-named `.cpp` misses every one.
2. **Relative includes were not matched.** `geometry/IntersectionTest.h` includes
   `"Line3D.h"`, not `"open3d/geometry/Line3D.h"` — which in turn pulled in
   Möller's triangle-triangle headers from Open3D's own `3rdparty/`.

The shape of the decision did not change: the three cut edges hold, and `core/`,
`t/` and `visualization/` stay out. The price roughly doubled.

#### The three cuts, verified at link time

| Cut edge | Reached from | What is lost |
|---|---|---|
| `core/EigenConverter.h` | `geometry/BoundingVolume.cpp:13` | feeding points to the two below |
| `t/geometry/kernel/MinimumOBB.h` | `BoundingVolume.cpp:17`, used at line 290 | `ComputeMinimumOBBJylanki` |
| `t/geometry/kernel/MinimumOBE.h` | `BoundingVolume.cpp:18`, used at line 115 | `ComputeMinimumOBEKhachiyan` |

Together they implement exactly `GetMinimalOrientedBoundingBox()`, which MeshLab
does not need. `nm -u` on the built archive reports **zero** undefined symbols
into `open3d::core::`, `open3d::t::` or `open3d::visualization::`, so the cuts
hold in the linker and not merely in the include graph.

A fourth apparent edge, `Macro.h` → `core/CUDAUtils.h`, is **not** a cut: it sits
behind `#if defined(__CUDA_ARCH__)` and never fires in a CPU build.

### The subset needs five external libraries, two of which we had

| Library | Used by | Added to `vcpkg.json` |
|---|---|---|
| Eigen | throughout | already present |
| TBB | `geometry/PointCloud.cpp`, `registration/Registration.cpp` | already present |
| nanoflann | `geometry/KDTreeFlann.cpp:15` | **added** |
| fmt | `utility/Logging.{h,cpp}`, `Helper.cpp`, `ProgressBar.cpp` | **added** |
| Qhull | `geometry/Qhull.cpp`, `ComputeConvexHull` in several files | **added** |
| jsoncpp | `utility/IJsonConvertible.h`, via `camera/PinholeCameraIntrinsic.h` | **added** |

Four new packages, against fifteen including VTK for the build route.

### Phase 0 result — it builds, links and computes

`MeshLab2Open3DUpstream` compiles as a static library on arm64 macOS against the
project's own Eigen and TBB. A standalone harness against the archive, on a
sampled unit sphere:

```
points                        : 7080
VoxelDownSample(0.05)         : 4696
EstimateNormals               : 4696 normals
RemoveStatisticalOutliers     : kept 4528
BallPivoting                  : 4696 verts, 9059 tris
ICP                           : fitness 1.0000  rmse 0.00000  dx -0.0500
```

ICP recovers exactly the −0.05 undoing an applied +0.05. Ball pivoting yields
9,059 triangles against the closed-surface Euler ideal of ~9,388 — slightly
fewer, consistent with the small holes the algorithm genuinely leaves.

### What this changes elsewhere in this document

- **Alpha shape leaves Phase 1.** `SurfaceReconstructionAlphaShape.cpp` includes
  `core/Tensor.h` and `core/EigenConverter.h`, so it reopens the 173-file `core/`
  cascade the cuts close. We already ship a CGAL alpha shape and an alpha
  wrapping; revisit only if Open3D's specific behaviour is wanted.
- **Phase 3 is not costed on this route.** TSDF integration lives in
  `pipelines/integration/`, which has not been traced. It must not be promised
  until it is.
- **"Proposed integration shape" above is superseded** in its dependency
  assumptions: Open3D becomes a vendored `upstream/` tree like
  `filter_instant_meshes` and `filter_xatlas`, with `UPSTREAM.md` recording the
  exact revision, the cut edges and their justification — not an external package.

Open3D is MIT, so vendoring into this GPL project is unproblematic.

## Candidate filters and staging

### Phase 0 — feasibility gate

**Answered 2026-10-14, and the answer was not one of the expected ones.** The
blocking question was:

> **Can Open3D be built on arm64 macOS against a dependency set we are willing
> to ship — and does it link statically?**

It is moot: there is no Open3D package to build against, and building from
source costs VTK. The route is to **vendor 51 files** instead — see the mapping
table above, which supersedes the questions below. They are kept because they
record what was checked.

The remaining Phase 0 work on the vendored route is much smaller: compile the 51
files against our Eigen and TBB, add `nanoflann`, `fmt` and `qhull` to
`vcpkg.json`, and confirm the three cut edges really do keep `core/` and `t/`
out of the build rather than only out of the include graph.

| # | Question | Why it blocks |
|---|---|---|
| 1 | Does `OPEN3D_USE_VCPKG` mode configure against our baseline, and what does it force us to add? | Upstream's manifest lists 21 packages; we have ~4 of them |
| 2 | Is VTK genuinely unavoidable? | Only `t/geometry/VtkUtils.cpp` uses it, and the tensor API is deferred — but `find_dependencies.cmake` offers no no-VTK branch. A patch may be cheaper than the dependency |
| 3 | Static or shared? | `BUILD_SHARED_LIBS` defaults ON. geogram's single dylib already cost an rpath fix, a bundling block, a signing loop and 64 stale rpath entries; Open3D plus VTK is that problem multiplied |
| 4 | What does it cost the CI cache budget? | See below — this is a measurement, not a guess |
| 5 | Can visualization, Python, examples, CUDA/SYCL, RealSense and Kinect all be disabled? | `BUILD_GUI`, `BUILD_PYTHON_MODULE`, `BUILD_EXAMPLES`, `WITH_IPP` and the sensor toggles all exist; confirm the resulting closure |
| 6 | Licence notices for everything actually linked | 48 bundled components upstream; vcpkg mode shrinks but does not eliminate the audit |

Existing filters with the same operation are not a rejection gate. Dependency
weight is.

#### The CI cache budget is a real constraint

The first local build is close to a one-time cost: vcpkg keys its binary cache
on an ABI hash, so a package is rebuilt only when the baseline, the triplet, the
feature set or **the compiler** changes. An Xcode upgrade invalidates everything.

CI is already set up for this —
[`macos-dmg.yml`](../../../.github/workflows/macos-dmg.yml) caches
`~/Library/Caches/vcpkg/archives`, `$VCPKG_ROOT/downloads` and
`vcpkg_installed`, keyed on `hashFiles('vcpkg.json', 'ci/vcpkg-triplets/*.cmake')`
with prefix `restore-keys`. Adding `open3d` changes that hash, but the
restore-keys still recover the archive directory, so only the new packages
compile. That part is fine.

The limit is size. Measured in this checkout on 2026-10-14:

| | |
|---|---|
| `vcpkg_installed` | 1.4 GB |
| local binary cache | 2.2 GB |
| `vcpkg/buildtrees` | 5.9 GB |

GitHub Actions allows **10 GB of cache per repository**, evicted least-recently-used,
and the cache key deliberately includes `github.run_id` so each rebuild writes a
fresh entry. Open3D plus VTK could plausibly double the cached set. If the total
crosses the limit, entries begin evicting *each other* and the steady state
becomes a cold rebuild on every run — which the workflow's own comments record
having happened to the Windows job for three weeks, from a different cause. No
job in this repository sets `timeout-minutes`, so the ceiling is the GitHub
default of six hours; a cold build including VTK should be measured against it.

### Phase 1 — point-cloud preparation and surface reconstruction

The cheapest filters, scheduled first **on purpose**: they are the smallest set
that exercises the whole adapter — point cloud in, attributes across, mesh out —
so the build, packaging and bundling story is proven before anything expensive
depends on it. The geogram port used the same reasoning and it held.

| Display name | Proposed identifier | Input / output |
|---|---|---|
| Sample Point Cloud by Voxel Grid (Open3D) | `sample_point_cloud_by_voxel_grid_open3d` | Point-cloud layer; new point-cloud layer tagged `voxel sampled` |
| Select Point Cloud Outliers by Radius (Open3D) | `select_point_cloud_outliers_by_radius_open3d` | Point-cloud layer; select rejected vertices |
| Select Point Cloud Outliers Statistically (Open3D) | `select_point_cloud_outliers_statistically_open3d` | Point-cloud layer; select rejected vertices |
| Compute Point Cloud Normals (Open3D) | `compute_point_cloud_normals_open3d` | Point-cloud layer; write vertex normals |
| Reconstruct Surface by Ball Pivoting (Open3D) | `reconstruct_surface_by_ball_pivoting_open3d` | Oriented point cloud; new mesh layer tagged `ball pivoting` |
| Reconstruct Surface by Alpha Shape (Open3D) | `reconstruct_surface_by_alpha_shape_open3d` | Point cloud; new mesh layer tagged `alpha shape` |

The outlier filters select rejected input vertices using the indices returned by
Open3D; they do not destroy the input cloud. Radius filtering needs a minimum
neighbor count and search radius. Statistical filtering needs neighbor count and
standard-deviation threshold. Voxel sampling should expose voxel size in mesh
units, preserve supported averaged colors/normals and document which vertex
attributes cannot be propagated. Normal estimation should expose Open3D's
neighborhood search and numerical mode; it estimates *unoriented* normals, so
orientation remains a separate step. These filters may look similar to existing
ones, but each uses Open3D's implementation and should carry its own provenance.

**Two incumbents would have to be renamed, which is the maintainer's call.**
[Vocabulary](../vocabulary.md) §6 — *a backend family names itself completely* —
means an unsuffixed filter cannot stay unsuffixed once a second implementation
of the same operation arrives:

| Proposed | Incumbent, unsuffixed today | Becomes |
|---|---|---|
| Compute Point Cloud Normals (Open3D) | `Compute Point Cloud Normals` | `… (vcglib)` or whichever backend it is |
| Reconstruct Surface by Alpha Shape (Open3D) | `Reconstruct Surface by Alpha Shape` (CGAL) | `… (CGAL)` |

This is the same situation `Pack UV Charts` hit during the geogram port, where
it became decision 5: renaming a shipped filter changes its `pythonName`, which
is a breaking API change and not a side effect a plugin may take on its own.
Ball Pivoting needs nothing — `(Gruber)` and `(vcglib)` are already suffixed.

Open3D Ball Pivoting accepts multiple ball radii and requires consistently
oriented input normals. Expose a nonempty ordered radii list, preserve input
colors where supported, and report coverage, disconnected components and
unreferenced points. Describe its multi-radius search separately from the
vcglib and Gruber implementations. Open3D alpha shape takes a radius-like
`alpha` in mesh units and uses Qhull tetrahedralization. Record whether the
result is empty, disconnected or non-manifold; do not imply watertightness.
Unlike the current CGAL alpha-shape filter, this path may pull in Open3D's
tensor/VTK normal-orientation code, so Phase 0 must confirm its actual build
dependencies before scheduling it.

### Phase 2 — registration implementations

| Display name | Proposed identifier | Input / output |
|---|---|---|
| Align by Colored ICP (Open3D) | `align_by_colored_icp_open3d` | Reference and moving point-cloud/mesh layers; update moving layer transform |
| Align by Point-to-Point ICP (Open3D) | `align_by_point_to_point_icp_open3d` | Reference and moving layers; update moving layer transform |
| Align by Point-to-Plane ICP (Open3D) | `align_by_point_to_plane_icp_open3d` | Reference and moving layers with target normals; update moving layer transform |
| Align by Feature Matching (Open3D) | `align_by_feature_matching_open3d` | Two point-cloud layers; set moving layer transform |

Colored ICP adds color constraints that can help with tangential motion when
geometry alone is ambiguous. Require colors and usable normals;
expose voxel scale, correspondence distance, iteration/convergence criteria and
an explicit initial transform. Return fitness and inlier RMSE so the user can
judge the result. Compare against MeshLab's current ICP and global aligner on
colored scans to document its behavior, not to decide whether an independent
implementation has a right to exist.

**Implemented 2026-10-14.** All four ship, with `Geometry/Alignment` as their
category, a single `referenceMesh` parameter (the current layer is the one that
moves — first declared as `sourceMesh`/`referenceMesh` after the TrueForm
aligner, and changed 2026-10-15; see
[filter organization](../filter_organization.md)), and `outputModifies: ["TM"]` — the result lands on the layer matrix,
composed onto what was already there, and the coordinates are never touched.
Each refuses with the filter that supplies the missing input named: point-to-plane
and colored ICP need reference normals, colored ICP needs color on both layers,
and feature matching needs normals on both.

*Initialize Alignment by Feature Matching* was renamed **Align by Feature
Matching (Open3D)**: `Initialize` is not in the verb lexicon, and
[vocabulary](../vocabulary.md) §3 defines `Align` as *deriving a registration
transform between layers*, which is exactly what it does. That it returns a
coarse pose belongs in the description, not in a new verb.

**Two feature-based initializers ship, not one.** Both compute the same FPFH
descriptors and differ only in what they do with them: *Align by Feature
Matching* samples candidate poses with RANSAC, *Align by Fast Global
Registration* optimizes a robust objective over all correspondences directly,
annealing the scale parameter (Zhou et al. 2016). The second is usually faster
and has no iteration budget to guess at; the first is the more conservative
choice on low-overlap pairs. Shipping both is the archive rule working as
intended — the shared FPFH stage makes them directly comparable.

**Both needed `randomSeed`, and the test found it.** RANSAC samples poses and
FGR samples tuples; both draw from `open3d::utility::random`, which the plugin
was not seeding, so the same input gave a different pose on every run. The
sequential-chain test below caught it by disagreeing with itself. The three
randomized filters — feature matching, fast global registration and the multiway
aligner — now declare `randomSeed` per the project convention, resolve it with
`FilterParams::getRandomSeed()` and pass it to `open3d::utility::random::Seed()`.
Zero still means a fresh seed; the value used is reported in the result.

**Sequential initial registration works, and the initializer has to be RANSAC.**
The eight `faccia*.ply` range maps, loaded raw and aligned pair by pair along
the sequence — each scan onto the one already placed, which is what Open3D's
`register_fragments` documentation describes and what a user does by hand — land
within **0.09%, 0.29%, 0.54%, 0.86%, 0.95%, 0.82% and 0.82%** of the diagonal, at
fitness 0.81 to 0.92. Drift is bounded rather than cumulative, because each ICP
pulls the new scan onto real surface.

*Align by Feature Matching* is the one to chain; *Fast Global Registration* is
not, and the difference is structural. FGR subtracts each cloud's mean and then
optimizes the rotation from the identity with Gauss-Newton
(`NormalizePointCloud` and the loop after it in `FastGlobalRegistration.cpp`),
so it is translation-invariant but **not orientation-invariant**. A chain is
precisely the case that defeats it: the reference has already been rotated into
the global frame while the incoming scan is still raw, so the relative rotation
grows along the sequence — roughly 45 degrees at the first link, and by
construction (the scans are named for their turntable angle; not measured
directly) far more by the fourth, where FGR collapses to fitness 0.24 and the
chain is lost. The same
faccia135/faccia180 pair registers at fitness 0.81 in isolation, where both
scans are still raw. RANSAC samples a correspondence triple and solves for the
pose outright, so it has no such dependency.

This is the practical content of "the first is the more conservative choice on
low-overlap pairs" above, and it is worth stating as orientation rather than
overlap: the pair that failed had ample overlap. The FGR filter's help says so.

Two measurement traps are recorded with it, because both produced confident
wrong conclusions here first. **Comparing translation columns is not a pose
error**: these scans sit some 570 units from the origin with a diagonal of 261,
so a few degrees of rotation moves the translation column further than the
object is wide, and a pair registered to within 0.34% read as 208% off. The
tests use the RMS displacement of the mesh's own bounding-box corners instead.
And **a chain hides which link failed**: every scan after a bad one registers at
good fitness onto a misplaced predecessor, so per-link fitness has to be read
alongside per-link error, never the endpoint alone.

Feature-based global registration (for example FPFH descriptors with RANSAC or
another supported robust estimator) supplies a coarse pairwise pose that can
initialize local ICP. Keep it as a separately callable alignment initializer;
the existing global layer alignment solves a different multi-layer task. The
adapter must honor
visible layer transforms and must define whether the result is applied to the
layer matrix or baked into coordinates; prefer the current layer-transform
contract.

Point-to-point and point-to-plane ICP merit named Open3D entries alongside the
vcglib and TrueForm variants. Expose the same core convergence and maximum
correspondence controls where feasible, and state the objective and required
normals in each description. Keep the result matrix contract consistent across
all ICP filters.

**Implemented 2026-10-15: *Align Meshes Sequentially (Open3D)***
(`align_meshes_sequentially_open3d`), the one-step form of the chain just
described, added after the by-hand version proved to work. It is orchestration
only: each pair is a call to the *Align by Feature Matching* code followed by the
*Point-to-Plane ICP* code, and the final stage is the *Align Meshes Globally*
code, so there is no second implementation of any of them.

Behaviour, as the maintainer specified it:

- the user names a reference layer; the chain is that layer followed by every
  later layer in the layer list (visible and non-empty ones by default), each
  aligned onto the one before it. Layers listed before the reference are not part
  of it;
- every pair attempted is written to the log as it starts, and a pair that
  registered is reported by name with its inlier RMSE and fitness as it ends;
- the progress bar belongs to the filter and covers the whole run, advancing by one
  pair's share as each layer is aligned (to 90%, then the global optimization, then
  100%). The pair steps and the global stage run inside it without opening or
  closing a bar of their own — when they did, each inner finish unblocked the
  interface in the middle of the run and rewound the percentage;
- **at the first pair that fails** — no correspondences, or ICP fitness below the
  minimum (default 0.5; registered range maps read 0.8 and above, an unregistered
  pair read 0.24) — it stops. The failed layer is put back where it was found,
  along with every layer after it, and the layers placed before it are optimized
  together as a pose graph with their current poses as the starting point. A stop
  is a successful run that says so; only a first pair that fails is an error, so
  that a run which moved nothing never reports success;
- *Global Refinement* can be switched off, and is skipped below three aligned
  layers, where there is a single pair and nothing to distribute.

Measured on the eight raw `faccia*.ply`: the whole chain registers, the worst
scan 0.91% of the diagonal off with the chain alone and 0.84% with the global
refinement, so the refinement is a small gain here and the assertion is only that
it does not hurt. With an unrelated sphere listed among them the chain stops at
the sphere, leaves it and the scan behind it untouched, and keeps the three that
registered.

**Starting alignment is stated first in every alignment filter's help.** What a
registration must be handed is the thing a user most needs and least thinks to
check — ICP on far-apart layers returns a confident wrong pose — so each Open3D
aligner opens with a `**Starting alignment:**` paragraph, "none" included, and
`alignmentFiltersStateTheirStartingAlignment` in the descriptor tier enforces it.
It is scoped to the Open3D filters because their requirements were measured; the
vcglib and TrueForm aligners predate the rule and have not been characterised.

**Implemented 2026-10-15: *Use All Visible Layers as Reference*** on *Align by
Feature Matching* (`useAllVisibleAsReference`; `referenceMesh` is ignored while it
is on). The reference becomes every other visible layer, concatenated in world
space; everything downstream — the voxel downsampling the feature path already
does, FPFH, RANSAC, fitness — is unchanged, because none of it cares how many
layers made the target. Derived lengths come from the extent of the combined
points. It is offered on feature matching only: the ICP objectives refine from a
pose already near, where one named reference is the meaningful thing.

It exists because a scan can overlap the surface already built up without
overlapping any one scan — in the range maps, the first scan of the second loop
does not overlap the last of the first. Measured on the eight `faccia` scans
placed at their `partial.mlp` poses as the reference and the raw `lato` scans
registered against them, as coarse pose error in % of the diagonal:

| scan | one `faccia` as reference | the whole ring as reference |
|---|---|---|
| `lato135` | 0.8 and 2.1 (`faccia135`, `faccia180`); the other six 62–78 | 0.5–1.4 over three seeds |
| `lato180` | 2.5 (`faccia225`); the other seven 62–71 | 1.7–3.3 over three seeds |
| `lato225` | 1.5 and 2.0 (`faccia270`, `faccia315`); the other six 74–81 | 1.7–3.3 over three seeds |

A scan overlaps only its one or two neighbours in the ring, so a single reference
is right or wrong by luck of which was chosen, and **the wrong ones report fitness
0.55–0.79 while being most of the object away** — the fitness number cannot be
what tells them apart. The set always registers. `lato180` takes about 15 s a run
where the others take about one second: its match is weaker, so RANSAC runs to its
100000-iteration cap. That cost is the existing one, not the set's.

**Found while testing this: the "needs normals" refusals in this plugin had never
fired.** `VCGMesh` always allocates its normals and colors, so
`HasPerVertexNormal` is true for every layer and `hadNormals` was too. A point
cloud with no normals — all zeros — went through feature matching and
point-to-plane ICP as if it had them, and reported a fitness. This contradicted the
"each refuses with the filter that supplies the missing input named" statement made
under Phase 2 above, which was true of the code and false of its behaviour. The
adapter now takes "has normals" to mean that some live vertex carries a non-zero
one, and the refusals fire; the refusal for a reference *set* names the layer
without them, since concatenation would otherwise drop them silently. Colors have
the same flaw and are **not** fixed: an unset color is indistinguishable from a
white one, so *Align by Colored ICP* and its "needs color" refusal still cannot
tell a colored layer from an uncolored one. And because the loader computes smooth
normals for any layer with faces, only point clouds can lack normals; earlier
advice to run *Compute Vertex Normals* on range maps first was unnecessary.

**Implemented 2026-10-16: lengths are `absperc`, and downsampled normals are unit
length.** Every length parameter in the Open3D filters had been a plain number where
`0` meant "derive it from the diagonal". That hid the real value from the user and from
any script reading it back, and differed from the 61 parameters elsewhere that use
`absperc`. All fifteen are now `absperc`: the default is a token, the editor shows the
absolute value and its percentage of the diagonal, and a positive minimum
(`@bboxDiag00001`) makes zero impossible to enter. The maximum is `@bboxDiag`, which is
also what fixes what 100% means.

| Filter(s) | Parameter | Was (0 meant) | Now |
|---|---|---|---|
| Sample by Voxel Grid | `voxelSize` | 0.5% of diag | `@bboxDiag0005` |
| Select Outliers by Radius; Compute Normals | `searchRadius` | 2% | `@bboxDiag002` |
| P2P, P2Plane, Colored ICP | `maxCorrespondenceDistance` | 2% | `@bboxDiag002` |
| Feature Matching, Fast Global Reg. | `maxCorrespondenceDistance`, `registrationVoxelSize`, `featureRadius` | 2%, 1%, 5% | `@bboxDiag002`, `@bboxDiag01`, **`@bboxDiag005`** (new) |
| Align Meshes Globally | `maxCorrespondenceDistance`, `registrationVoxelSize` | 3%, 1% | `@bboxDiag003`, `@bboxDiag01` |
| Align Meshes Sequentially | `maxCorrespondenceDistance` | 2% (pairs), 3% (its global stage) | `@bboxDiag002` for both |

Three behaviours differ, and are the price of the change:

- **The basis is the current layer's diagonal**, which is what the tokens resolve
  against. It was the reference layer's for the pairwise filters, the first layer's for
  global alignment, and the combined extent when a set was the reference. For scans of
  similar size the difference is small; in `partial.mlp`, with `faccia000` current, the
  default voxel for global alignment moved from 2.40 to 2.61.
- **The sequential aligner's global stage now uses 2%, not 3%**, because it has one
  parameter for both.
- **Scripts that passed `0` to mean "derive" now get an error** naming the minimum. They
  were the only way to reach the old behaviour, so none is expected in the wild.

The fallback is not gone: the sequential aligner still runs the pairwise steps with a
partial parameter set, naming the pairing distance and leaving the matching voxel and
feature radius to be derived, so that each pair is scaled to its own reference. That is
the one caller `resolveLength` in `open3dregistration.cpp` still serves. The point cloud
filters had none and lost theirs.

**The token names are not percentages.** `@bboxDiag01` is 1%, `@bboxDiag001` is 0.1%, and
`@bboxDiag005` — added for the feature radius, as none existed for 5% — is 5%. The digits
are those of the fraction. A previous version of this note, and the first draft of this
change, read `001` as 1%.

*Downsampling Cell Size* — it was *Matching Voxel Size*, which named the stage the number
serves and not what it controls, and "voxel" implies a volume; see the ruling in
[vocabulary](../vocabulary.md) — has a written meaning at last: the edge of an axis-aligned cubic
cell, in the layer's units, anchored half a cell below the cloud's minimum corner; each
occupied cell becomes one point at the average of its points, with normals and colors
averaged too. Open3D leaves the averaged normals un-normalized — its own comment says so —
so a cell whose normals disagree gets one shorter than 1, which FPFH and the point-to-plane
term both assume is 1. It is now renormalized at all three downsampling sites. On a sphere
at a coarse cell the shortest normal was **0.93** before; the registration result moved
both ways, within what three seeds can tell apart (mean coarse error, same pair: 2.1 → 1.3 at
a 1% voxel, 1.8 → 1.2 at 2%, 2.9 → 4.2 at 4%), so the change rests on correctness and
not on a measured gain.

On one pair of range maps (`faccia045` onto `faccia000`, coarse pose, three seeds) the
cost and the outcome by voxel, in % of the diagonal: 0.25% took 76 s; 0.5% took 1 s;
1% (the default) took 0.15 s and kept about 4,500 points of 80,000; 2% was as accurate;
4% was slower and worse; 8% kept about 110 points and lost the pose. Scaling the feature
radius and correspondence distance with the voxel (5× and 2×, as Open3D's own tutorial
does) helped at 8% and made 2% and 4% clearly **worse**, so they are deliberately *not*
tied to it.

**A test that misreported global alignment, again.** `open3dGlobalAlignmentOnRealRangeMaps`
counted a layer as "relocated" when its matrix's translation column moved 5% of the
diagonal. That is the metric that already misled the sequential tests: the scans sit far
from the origin, so a small rotation moves the column by more than the object is wide.
The same physical result counted between one and eight relocated layers depending on
voxel size. By the layers' own bounding-box corners, global alignment moves ten of the
eleven non-reference layers 0.6–1.5% of the diagonal in every configuration tried,
renormalization on or off, and moves only the deliberately misplaced `lato000` far.
The test now measures that way.

### Phase 3 — RGB-D sequence reconstruction

The largest new workflow, and therefore **last** rather than first. It was
scheduled first in the original draft as the "primary" feature; that ordering
asked the heaviest dependency in the project to prove itself through its most
complicated consumer. Phases 1 and 2 answer the build and adapter questions on
filters that can be thrown away cheaply if Phase 0 goes badly.

| Display name | Proposed identifier | Input / output |
|---|---|---|
| Reconstruct Surface from RGB-D Sequence (Open3D) | `reconstruct_surface_from_rgbd_sequence_open3d` | Ordered RGB-D frame set plus calibration; new mesh layer tagged `rgbd reconstruction` |

The filter should accept explicitly ordered, synchronized color/depth frames,
camera intrinsics, color-to-depth registration assumptions, depth scale/truncation,
and either an initial pose per frame or a documented odometry mode. It should
estimate/refine poses, integrate into a scalable TSDF volume, and extract a
triangle mesh. It must report frames rejected or disconnected, pose-graph
quality, voxel size, and memory-relevant volume parameters. Keep the trajectory
and per-frame poses available as a sidecar/result attribute or a follow-on
document object only if the existing data model can represent them safely; do
not hide essential alignment state in plugin globals.

This is a multi-image input, whereas current filters primarily consume document
layers — but **the representation already exists and the original draft missed
it.** Checked in this checkout on 2026-10-14:

- `Document::RasterEntry` (`src/core/document.h:93`) carries a `CameraShot` —
  pose *and* intrinsics — alongside `std::vector<RasterPlane>` and a current
  plane index.
- `RasterPlaneSemantic` (`src/core/rasterplane.h`) already declares
  **`DepthFloat`**, beside `RGBA`, the mask semantics and four spare float and
  RGBA planes.
- Planes are parsed from and written back to `.mlp` projects
  (`src/core/document_project.cpp:73-99`), so they persist across save and
  reopen.

So an RGB-D frame **is** a raster layer with an `RGBA` plane and a `DepthFloat`
plane; an ordered sequence is an ordered set of raster layers; and the whole
thing already round-trips through a project file. The warning below stands, but
it is satisfied by what the document model can already express rather than by a
new sequence object:

A one-shot dialog with ephemeral frame paths is not an acceptable reproducible
workflow. Check depth/color calibration,
frame ordering, units and pose conventions before integration; reject incomplete
inputs rather than silently guessing.

Open3D's documented RGB-D odometry and reconstruction-system examples are useful
starting points, but its tensor SLAM implementation is described as a prototype
with limitations. Do not present experimental dense SLAM as a reliable
general-purpose tracker. Initial support should target offline reconstruction
from recorded sequences with explicit diagnostics and a path to supplied poses.

### Shared-core or later candidates

- **Open3D Poisson:** the currently inspected route calls the same Kazhdan
  `PoissonRecon` core already exposed by MeshLab's Screened Poisson plugin.
  Compare vendored versions and adapter options after pinning; add a separate
  filter only if a genuinely different implementation is found. A desired
  Open3D-specific option could instead inform the existing adapter.
- **Multiway pose-graph alignment: implemented 2026-10-15** as *Align Meshes
  Globally (Open3D)*, and the incumbent took a suffix under decision 5
  (`align_meshes_globally` → `align_meshes_globally_vcglib`). The pose graph is
  built, optimised and discarded; persisting it for inspection or re-optimisation
  remains open, and `LayerData` is the mechanism if it is wanted.

  Two things were settled by building it. **Open3D's frame convention** is
  `node.pose_` = local→world and `edge.transformation_` = `T_target⁻¹ · T_source`,
  read out of the residual `X_inv * Tt_inv * Ts ≈ I` in `GlobalOptimization.cpp`;
  that is exactly what `RegistrationICP` returns for clouds in their own local
  frames, so this filter alone does **not** move clouds into world space, and a
  node pose *is* the layer matrix. **Edge certainty cannot be all-uncertain**:
  Open3D expects a connected subset of certain edges, which its own pipeline gets
  from scan order. Document layers have none, and marking every edge uncertain
  produces `Certain-edge subset of PoseGraph is not connected` and a poor result.
  A maximum spanning tree over the accepted pairs, strongest fitness first, is the
  backbone; everything else stays a loop closure and keeps the line process.
- **Real-time SLAM, tensor API and GPU backends:** defer due to prototype status,
  runtime/device complexity and portability costs.
- **Open3D visualization, legacy GUI, and Python API:** out of scope for a
  headless MeshLab filter plugin.

## Test datasets

**Built, 2026-10-14** — see [Test Data](../test_data.md) for the mechanism.
`tests/datasets/datasets.json` is the manifest, `tests/datasets/fetch.py` fetches
and verifies, and `MESHLAB2_REQUIRE_DATASET` in `tests/testdatasets.h` gives a
test an absolute path or skips it with an actionable message, and
`.github/workflows/tests.yml` runs the suite on request.

It was built before any Open3D work on purpose: it is not Open3D-specific, and
the first thing it did was repair two long-standing failures that had nothing to
do with this proposal — `loadMeshLabProjectLoadsMeshesAndTransforms` and
`loadMeshLabProjectLoadsRastersAndCamera` hard-coded paths under `tests/` for
data that only ever existed under `tests/local/`.

The manifest is seeded with `redwood_rgbd_sample`: five synchronised colour and
depth frames from the Redwood indoor dataset with PrimeSense intrinsics, an
odometry log and a trajectory, 3.77 MB. That is the smallest input that
exercises a full RGB-D sequence, so Phase 3 has its fixture already.

Its licence was initially recorded as `UNRESOLVED` — GitHub reports
`isl-org/open3d_downloads` as NOASSERTION and the hosting repository says
nothing about redistribution. It is now **public domain**, confirmed against
[redwood-data.org](http://redwood-data.org/indoor_lidar_rgbd/license.html):
*"The entire dataset, including both RGB-D scans and reconstructed models, is in
the public domain. Any part of the dataset can be used for any purpose with
proper attribution."* The entry is `ciEligible: true` and carries the citation
the page asks for (Park, Zhou and Koltun, ICCV 2017).

Worth noting how that went, because it is the argument for the manifest: the
licence was not discoverable from the download URL, the hosting repository or
the Open3D source — all three were silent or said NOASSERTION — and was settled
by someone who knew which project the data came from. A `license` field that can
hold `UNRESOLVED` is what kept the question visible until then, instead of it
being assumed either way.

## Validation required before implementation is considered complete

Use the fetched corpus described in *Test datasets* above: recorded RGB-D
sequences with known calibration and reference geometry, including clean indoor
captures, low-texture/repetitive areas, loop closures,
motion blur, depth holes, thin structures and disconnected fragments. Compare
pose error, drift, completeness, surface accuracy, runtime and peak memory with
the existing multi-layer volumetric reconstruction workflow where inputs can
be made comparable. Verify deterministic behavior where the algorithm permits,
correct units and transform conventions, graceful handling of missing frames,
and no mutation of source geometry. Compare ICP and Ball Pivoting variants on
the same clouds to document their distinct behavior, including color-informative
and color-uninformative scans, sparse/noisy samples, varying densities and
multiple components. Such comparisons inform users and defaults; they are not
an admission test based on whether another filter performs the same operation.

Do not ship until the RGB-D input representation is reproducible, source layers
remain intact, the dependency and license audit is complete, and packaging works
without an Open3D installation when the plugin is disabled.

## Official references

- [Open3D project](https://www.open3d.org/) and [source repository](https://github.com/isl-org/Open3D)
- [Surface reconstruction](https://www.open3d.org/docs/latest/tutorial/geometry/surface_reconstruction.html)
- [Point-cloud outlier removal](https://www.open3d.org/docs/latest/tutorial/geometry/pointcloud_outlier_removal.html)
- [ICP registration](https://www.open3d.org/docs/latest/tutorial/pipelines/icp_registration.html)
- [Colored point-cloud registration](https://www.open3d.org/docs/latest/tutorial/pipelines/colored_pointcloud_registration.html)
- [Global registration](https://www.open3d.org/docs/latest/tutorial/pipelines/global_registration.html)
- [RGB-D integration](https://www.open3d.org/docs/latest/tutorial/pipelines/rgbd_integration.html)
- [Reconstruction system](https://www.open3d.org/docs/latest/tutorial/reconstruction_system/index.html)
- [Dense RGB-D SLAM](https://www.open3d.org/docs/latest/tutorial/t_reconstruction_system/dense_slam.html)
- [Open3D license](https://github.com/isl-org/Open3D/blob/main/LICENSE)

[open3d-bpa]: https://github.com/isl-org/Open3D/blob/main/cpp/open3d/geometry/SurfaceReconstructionBallPivoting.cpp
[open3d-alpha]: https://github.com/isl-org/Open3D/blob/main/cpp/open3d/geometry/SurfaceReconstructionAlphaShape.cpp
[open3d-poisson]: https://github.com/isl-org/Open3D/blob/main/cpp/open3d/geometry/SurfaceReconstructionPoisson.cpp
