# Open3D Scan Processing and Reconstruction

This document proposes what is left of the Open3D integration: **RGB-D sequence
reconstruction (phase 3) — odometry, pose refinement and TSDF integration of recorded
color and depth frames into a mesh. Not implemented**, and not yet costed on the vendored
route. It also keeps the Open3D candidates that were deliberately not taken.

What shipped, phases 0 to 2 (2026-10-14 to 2026-10-30), is `plugins/filter_open3d`: a
vendored 116-file subset of Open3D v0.20.0, four point-cloud filters, *Reconstruct Surface
by Ball Pivoting (Open3D)* and nine registration filters, from pairwise ICP to *Align
Meshes Sequentially* and *Align Meshes Globally*. How that subset is built and patched is in
`plugins/filter_open3d/UPSTREAM.md`; how each filter behaves is in its description. The
proposal those phases were built from, with the rejected build route, the mapping table
and the registration measurements, is the record
[history/open3d_scan_processing.md](../history/open3d_scan_processing.md).

See also: [Data Model](../data_model.md), [Test Data](../test_data.md),
[Third-Party Code](../third_party.md).

## What the build already settles for this phase

Open3D is **vendored, not a package**: vcpkg has no `open3d` port, and building the
library pulls VTK unconditionally (the reasons, re-checked 2026-10-16, are in
`UPSTREAM.md`). So phase 3 enters the build the way phases 1 and 2 did — by extending the
subset — and its cost is the size of that extension, which has **not been measured**:

- TSDF integration lives in `cpp/open3d/pipelines/integration/` and RGB-D odometry in
  `cpp/open3d/pipelines/odometry/`; neither is in `files.txt` today, and neither has been
  traced. The subset holds `geometry/`, `pipelines/registration/`, `utility/` and
  `camera/` (the last already there because `geometry/ImageFactory.cpp` reaches it; it
  includes `PinholeCameraIntrinsic` and `PinholeCameraTrajectory`, which phase 3 needs).
- Trace by **building**, not by following includes: the first include-only count of the
  current subset was less than half the real one, because a C++ library also closes over
  symbol definitions no `#include` names. Then confirm with `nm -u` on the archive that
  nothing reaches `open3d::core::`, `open3d::t::` or `open3d::visualization::`, as was
  done for the three existing cuts.
- The pose graph and its optimizer (`GlobalOptimization`) are already vendored and used
  by *Align Meshes Globally (Open3D)*, whose comments record Open3D's frame convention and
  why it needs a certain-edge backbone. A fragment pose graph can reuse both.
- Any new external library goes into `vcpkg.json`, as `nanoflann`, `fmt`, `qhull` and
  `jsoncpp` did, and adds to the CI cache, which GitHub limits to 10 GB per repository with
  least-recently-used eviction; past it, entries evict each other and every run rebuilds
  cold. Measured locally on 2026-10-14, before Open3D: 1.4 GB `vcpkg_installed`, 2.2 GB
  binary cache. A heavy addition is measured against that, not assumed.

## Phase 3 — RGB-D sequence reconstruction

Last on purpose: the largest new workflow, and the one the build and adapter questions
were not to be answered through.

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

This differs from *Reconstruct Surface by Volumetric Merging*, which integrates
already-placed visible layers: this one needs image and depth inputs, calibration, frame
poses and a trajectory.

**The representation already exists.** Checked 2026-10-14:

- `Document::RasterEntry` (`src/core/document.h`) carries a `CameraShot` — pose *and*
  intrinsics — alongside `std::vector<RasterPlane>` and a current plane index.
- `RasterPlaneSemantic` (`src/core/rasterplane.h`) already declares **`DepthFloat`**,
  beside `RGBA`, the mask semantics and four spare float and RGBA planes.
- Planes are parsed from and written back to `.mlp` projects
  (`src/core/document_project.cpp`), so they persist across save and reopen.

So an RGB-D frame **is** a raster layer with an `RGBA` plane and a `DepthFloat` plane, an
ordered sequence is an ordered set of raster layers, and the whole thing already
round-trips through a project file. A one-shot dialog with ephemeral frame paths is not an
acceptable reproducible workflow. Check depth/color calibration, frame ordering, units
and pose conventions before integration; reject incomplete inputs rather than silently
guessing.

Open3D's documented RGB-D odometry and reconstruction-system examples are useful
starting points, but its tensor SLAM implementation is described as a prototype
with limitations, and the tensor API is outside the subset in any case. Do not present
experimental dense SLAM as a reliable general-purpose tracker. Initial support should
target offline reconstruction from recorded sequences with explicit diagnostics and a
path to supplied poses.

## Candidates not taken

- **Reconstruct Surface by Alpha Shape (Open3D):** left out of phase 1 because
  `geometry/SurfaceReconstructionAlphaShape.cpp` includes `core/Tensor.h` and
  `core/EigenConverter.h`, reopening the 173-file `core/` cascade the cuts close. MeshLab
  ships a CGAL alpha shape and an alpha wrapping; revisit only if Open3D's Qhull-based
  behaviour is specifically wanted. If it ever ships, the incumbent *Reconstruct Surface
  by Alpha Shape* takes a `(CGAL)` suffix under [Vocabulary](../vocabulary.md) §6, a
  `pythonName` change that is the maintainer's call. (The other incumbent this raised,
  *Compute Point Cloud Normals*, is already `(vcglib)`.)
- **Open3D Poisson:** `SurfaceReconstructionPoisson.cpp` calls the same Kazhdan
  `PoissonRecon` core that `filter_screened_poisson` exposes. Add a separate filter only if
  a genuinely different implementation is found; a wanted Open3D option could instead
  inform the existing adapter.
- **Persisting the pose graph** of *Align Meshes Globally (Open3D)* for inspection or
  re-optimization: today it is built, optimized and discarded. `LayerData` is the
  mechanism if it is wanted; a phase-3 trajectory would want the same.
- **Real-time SLAM, the tensor API and GPU backends:** deferred for prototype status,
  runtime and device complexity, and portability.
- **Open3D visualization, legacy GUI and Python API:** out of scope for a headless filter
  plugin.

## Test datasets

`redwood_rgbd_sample` in `tests/datasets/datasets.json` (see [Test Data](../test_data.md)):
five synchronised colour and depth frames from the Redwood indoor dataset with PrimeSense
intrinsics, an odometry log and a trajectory, 3.77 MB, public domain with attribution
(Park, Zhou and Koltun, ICCV 2017), `ciEligible: true`. It is the smallest input that
exercises a full RGB-D sequence, so phase 3 has its fixture already.

## Validation required before phase 3 is complete

Recorded RGB-D sequences with known calibration and reference geometry, including clean
indoor captures, low-texture or repetitive areas, loop closures, motion blur, depth holes,
thin structures and disconnected fragments. Compare pose error, drift, completeness,
surface accuracy, runtime and peak memory with the existing volumetric integration where
inputs can be made comparable. Verify deterministic behaviour where the algorithm permits,
correct units and transform conventions, graceful handling of missing frames, and no
mutation of source layers. Do not ship until the RGB-D input representation is
reproducible and the licence notices of any newly vendored file are recorded in
`UPSTREAM.md`.

## Open questions

1. What does `pipelines/integration/` plus `pipelines/odometry/` add to the subset, in
   files, lines and external libraries, and does either reach `core/` or `t/`?
2. Should the per-frame trajectory be kept on the document (as `LayerData`, or as the
   raster cameras themselves), and with what undo semantics?
3. *Align Layer to Reference by Colored ICP* still cannot refuse an uncolored layer:
   `VCGMesh` always allocates vertex colors and an unset color is a valid one (see
   `open3dmeshadapter.cpp`). Normals were fixed by testing for a non-zero normal; colors
   have no equivalent test, and phase 3's color frames would meet the same question.

## Official references

- [Open3D project](https://www.open3d.org/) and [source repository](https://github.com/isl-org/Open3D)
- [RGB-D integration](https://www.open3d.org/docs/latest/tutorial/pipelines/rgbd_integration.html)
- [Reconstruction system](https://www.open3d.org/docs/latest/tutorial/reconstruction_system/index.html)
- [Dense RGB-D SLAM](https://www.open3d.org/docs/latest/tutorial/t_reconstruction_system/dense_slam.html)
- [Open3D license](https://github.com/isl-org/Open3D/blob/main/LICENSE)
