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

## Candidate filters and staging

### Phase 0 — feasibility gate

Before adding descriptors or UI, pin a release and prove a minimal headless C++
build against the project's supported platforms and toolchain. Measure configure
time, compile time, binary growth, runtime library closure and representative
scan memory use. Verify that the Open3D build can disable its visualization,
Python and unused I/O/dependency components. Check license notices and packaging
on each supported platform. Use these measurements to choose staging and
packaging; existing filters with the same operation are not a rejection gate.

### Phase 1 — RGB-D sequence reconstruction (primary)

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
layers. Phase 1 depends on a deliberate input design: either a sequence object
that the document can save/reopen, or a file-dialog workflow that records all
paths and settings in filter history. A one-shot dialog with ephemeral frame
paths is not an acceptable reproducible workflow. Check depth/color calibration,
frame ordering, units and pose conventions before integration; reject incomplete
inputs rather than silently guessing.

Open3D's documented RGB-D odometry and reconstruction-system examples are useful
starting points, but its tensor SLAM implementation is described as a prototype
with limitations. Do not present experimental dense SLAM as a reliable
general-purpose tracker. Initial support should target offline reconstruction
from recorded sequences with explicit diagnostics and a path to supplied poses.

### Phase 2 — registration implementations

| Display name | Proposed identifier | Input / output |
|---|---|---|
| Align by Colored ICP (Open3D) | `align_by_colored_icp_open3d` | Reference and moving point-cloud/mesh layers; update moving layer transform |
| Align by Point-to-Point ICP (Open3D) | `align_by_point_to_point_icp_open3d` | Reference and moving layers; update moving layer transform |
| Align by Point-to-Plane ICP (Open3D) | `align_by_point_to_plane_icp_open3d` | Reference and moving layers with target normals; update moving layer transform |
| Initialize Alignment by Feature Matching (Open3D) | `initialize_alignment_by_feature_matching_open3d` | Two point-cloud layers; set moving layer transform |

Colored ICP adds color constraints that can help with tangential motion when
geometry alone is ambiguous. Require colors and usable normals;
expose voxel scale, correspondence distance, iteration/convergence criteria and
an explicit initial transform. Return fitness and inlier RMSE so the user can
judge the result. Compare against MeshLab's current ICP and global aligner on
colored scans to document its behavior, not to decide whether an independent
implementation has a right to exist.

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

### Phase 3 — point-cloud preparation and surface reconstruction

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

### Shared-core or later candidates

- **Open3D Poisson:** the currently inspected route calls the same Kazhdan
  `PoissonRecon` core already exposed by MeshLab's Screened Poisson plugin.
  Compare vendored versions and adapter options after pinning; add a separate
  filter only if a genuinely different implementation is found. A desired
  Open3D-specific option could instead inform the existing adapter.
- **Multiway pose-graph alignment:** a separate Open3D filter is welcome if the
  document can retain and edit the resulting pose graph. The RGB-D pipeline can
  use it internally first; do not discard the implementation simply because
  MeshLab already has a global alignment filter.
- **Real-time SLAM, tensor API and GPU backends:** defer due to prototype status,
  runtime/device complexity and portability costs.
- **Open3D visualization, legacy GUI, and Python API:** out of scope for a
  headless MeshLab filter plugin.

## Validation required before implementation is considered complete

Use recorded RGB-D sequences with known calibration and reference geometry,
including clean indoor captures, low-texture/repetitive areas, loop closures,
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
