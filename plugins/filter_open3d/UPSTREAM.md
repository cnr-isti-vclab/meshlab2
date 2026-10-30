# Open3D provenance

- Project: [Open3D](https://github.com/isl-org/Open3D)
- License: MIT (see `upstream/LICENSE`)
- Vendored release: **v0.20.0**, published 2026-09-16
- Source archive: `https://github.com/isl-org/Open3D/archive/refs/tags/v0.20.0.tar.gz`
  SHA-256 `66a8ff94cf3f641d77a2b885ef7bdc8e7a4e6c0fa689458d559d3f6d771feef4`
- Vendored file list: `files.txt` (116 paths)

**There is no Open3D package to depend on.** `ports/open3d` does not exist in
vcpkg — not in this project's pinned baseline and not upstream — so the choice
was to build Open3D from source or to vendor part of it. Building it pulls VTK
unconditionally (`3rdparty/find_dependencies.cmake` offers a system VTK or an
imported VTK and no third branch), and the vcpkg `vtk` port has 26 dependencies
whose default features reach HDF5, NetCDF, PROJ and SQLite, plus
`vtk-compile-tools`, so VTK is built roughly twice. For a plugin that wants
point-cloud preparation, Ball Pivoting and ICP, that is the wrong trade.

**Re-checked 2026-10-16**, because a package appearing later would change the
choice. Still no port: the pinned baseline (`37bb045`, 2026-08-15) has no
`ports/open3d`, neither does vcpkg `master` or its `versions/` database, and the
only other mentions are `-DWITH_OPEN3D=OFF` in `rtabmap` and an unrelated codec
in `assimp`. One is in progress, [microsoft/vcpkg#53211](https://github.com/microsoft/vcpkg/pull/53211)
"[open3d]: add new port" — a **draft** opened 2026-08-04, *changes requested*, last
updated 2026-09-11, after earlier attempts (#12199, #17423, #41844) were closed
unmerged. Its manifest builds all of Open3D and depends on `vtk` (default features
off), `assimp[draco]`, `embree`, `glfw3`, `glew`, `opengl`, `curl`, `openssl`,
`zeromq`/`cppzmq`, `msgpack` and `minizip` beside the ones this plugin already
takes. So even once merged it is the build route rejected above, with VTK built
once rather than not at all, for a plugin that uses 116 files. Revisit only if
that port lands *and* gains a feature that builds the legacy
`geometry`/`registration` core alone.

So this is a **subset**: 116 of Open3D's files, 28,752 lines, being the closure
of the legacy `geometry` and `pipelines/registration` entry points. The whole
tensor API (`cpp/open3d/t/`, 142 files), the tensor core (`cpp/open3d/core/`,
173 files) and `cpp/open3d/visualization/` are excluded, along with the Python
bindings, the GUI, the I/O layer and the build system. MeshLab converts to and
from its own mesh model in the plugin's adapters, outside `upstream/`.

The measured closure, the rejected build route and the reasoning are in
[docs/design/proposals/open3d_scan_processing.md](../../docs/design/proposals/open3d_scan_processing.md).

## Local changes inside `upstream`

Two files are patched.

### `open3d/geometry/BoundingVolume.cpp`

Three includes removed and the two functions that used them replaced with
`utility::LogError`.

```
#include "open3d/core/EigenConverter.h"            // line 13
#include "open3d/t/geometry/kernel/MinimumOBB.h"   // line 17, used at line 290
#include "open3d/t/geometry/kernel/MinimumOBE.h"   // line 18, used at line 115
```

These are the **only** edges from Open3D's legacy geometry into `core/` and
`t/`. Following them pulls 189 further files — the entire tensor API and,
through `t/geometry/TriangleMesh.h`, `visualization/rendering` — into a plugin
that needs none of it: the same six entry points close over 240 files with these
edges and 116 without.

What is lost is exactly `OrientedBoundingBox::CreateFromPointsMinimal` and
`OrientedBoundingEllipsoid::CreateFromPoints`, i.e. minimum-volume oriented
bounding boxes and ellipsoids. MeshLab computes oriented bounding boxes itself
and nothing in `filter_open3d` calls these. The stubs raise rather than return a
wrong answer. **Verified in the linker, not only in the include graph:** `nm -u`
on `libMeshLab2Open3DUpstream.a` reports zero undefined symbols into
`open3d::core::`, `open3d::t::` or `open3d::visualization::`.

A fourth apparent edge, `open3d/Macro.h` → `open3d/core/CUDAUtils.h`, is left
untouched and is **not** a cut: it sits behind `#if defined(__CUDA_ARCH__)` and
never fires in a CPU build. A grep for excluded subsystems will report it.

### `open3d/pipelines/registration/Registration.cpp`: deterministic RANSAC

`RegistrationRANSACBasedOnCorrespondence` was not reproducible with a fixed seed
(added 2026-10-29). The samples are drawn up front from the seeded generator and
the reduction is `tbb::parallel_deterministic_reduce`, but the early exit was an
atomic bound every thread lowered as it found a better pose and checked before
each iteration, so which iterations were validated -- and now and then the pose --
depended on thread timing. That broke the plugin's `randomSeed` contract (a logged
seed replays the run), and `FilterTests::open3dFeatureMatchingCanEstimateItsQualityWithIcp`
failed about one run in three, comparing two runs with the same seed.

The patch takes the early exit between batches of iterations instead: each batch
is validated whole and in parallel, every chunk keeps the bound its own best
result asks for, `join()` reduces it by min, and the next batch stops at it. The
batches start at 64 and double to 1024, with a grain of 4; nothing depends on the
thread count, so the result is the same on any machine. Measured on the test
range maps: a pair validates 192 iterations in ~110 ms against ~85 ms for the
racing loop, giving exactly the sequential result; a first batch of 1024 took
~470 ms, since the bound is often met within a hundred or so iterations.
The 20 Open3D filter tests take ~40 s patched against ~42 s before. The
`atomic_min` helper, used only by the old bound, is removed. A candidate to send
upstream.

Separately, the reducer's split constructor sizes `ransac_corres` ("Must be sized
here too"). That line came in with the original import and was not compared
against a v0.20.0 checkout; `check_upstream_status.sh` will tell.

No other file differs from upstream.

## Deliberately omitted implementation files

These compile cleanly but each costs something unwanted:

| File | Why not |
|---|---|
| `geometry/SurfaceReconstructionAlphaShape.cpp` | includes `core/Tensor.h`; the only file of 38 candidates that reopens the `core/` cascade |
| `geometry/SurfaceReconstructionPoisson.cpp` | needs the vendored PoissonRecon, and `filter_screened_poisson` already exposes the same Kazhdan core |
| `utility/Download.cpp` | adds libcurl |
| `utility/Extract.cpp`, `utility/ExtractZIP.cpp` | add zlib/minizip plumbing |

## Third-party code inside `upstream/3rdparty`

`tomasakeninemoeller/` — Tomas Möller's triangle/triangle intersection test
(JGT 2(2), 1997), two headers, required by `geometry/IntersectionTest.cpp`.
Carried from Open3D's own `3rdparty/`. Its licence file
(`3rdparty/tomasakeninemoeller-LICENSE`) is an MIT template whose
`[year]` and `[fullname]` placeholders upstream never filled in; the headers
themselves carry only the author's name and the paper reference.

## Build requirements

`vcpkg.json` gained four packages for this subset: `nanoflann`
(`geometry/KDTreeFlann.cpp`), `fmt` (`utility/Logging`), `qhull`
(`geometry/Qhull.cpp` and `ComputeConvexHull`) and `jsoncpp`
(`utility/IJsonConvertible.h`, reached through `camera/PinholeCameraIntrinsic.h`).
Eigen and TBB were already present.

`CMakeLists.txt` supplies `OPEN3D_STATIC`, which empties the visibility macros in
`Macro.h`, and the three `OPEN3D_CXX_*` defines that `utility/CompilerInfo.cpp`
expects from Open3D's own CMake. Upstream sources are compiled with warnings off:
a vendored tree that must be re-patched on every update is a tree nobody updates.

## Updating

```bash
plugins/filter_open3d/check_upstream_status.sh [path-to-open3d-checkout]
```

It reports which vendored files differ from the given upstream tree — the two
patched files above should be the only hits — and which are missing or newly added. The file
set itself is **not** derivable by following includes alone: a C++ library closes
over symbol definitions too, and `EstimateNormals.cpp`,
`SurfaceReconstructionBallPivoting.cpp`, `ImageFactory.cpp` and
`TetraMeshFactory.cpp` are each reached by no `#include` anywhere. That is why
`files.txt` records the result rather than a script re-deriving it.
