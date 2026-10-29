# Texture Consolidation

**Status: Phase 1 implemented, not yet packaged (2026-10-29).** Integrating Liu, Wang, Kim and
Kazhdan, *Texture Consolidation of Generated Multi-View Renderings* (arXiv 2610.36098,
2026) — code at [hliuw/TextureConsolidation](https://github.com/hliuw/TextureConsolidation)
— as a second implementation of *Transfer Color from Visible Rasters to Texture*.

Everything under *Upstream, measured* was read from the repository and its three
dependencies on 2026-10-28, not inferred from the paper or the README.

## What it does

Given a **texture-atlased** triangle mesh and a few images with **known cameras**, it
produces one seamless texture image. Three steps:

1. **Back-projection with confidence.** Each image is projected into texture space,
   giving a color texture and a confidence texture per view. Confidence is the product
   of a normal term (clamped dot product of normal and view direction) and a depth term
   that falls off near depth discontinuities of the mesh as seen from that camera, so
   texels near silhouettes — where a generated image and the geometry disagree most —
   count less. Colors are then normalized by a local mean and scale, which the paper
   finds essential for the optical flow to be stable.
2. **Registration by optical flow on the surface.** A flow field per view, at mesh
   resolution, aligning that view's colors to a target built incrementally from the
   views already registered; hierarchical, coarse to fine. This is what removes ghosting.
3. **Gradient-domain stitching in texture space** (Prada, Kazhdan, Chuang, Hoppe 2018),
   confidence-weighted, which removes the remaining seams and color steps.

It was designed for *generated* renderings, which are geometrically inconsistent with
the mesh and with each other, but works on real photographs: the paper shows two RGB-D
scans, and the repository's own example is nine photographs of a vase that are,
according to its README, courtesy of Paolo Cignoni and Marco Callieri.

**How it will differ from the incumbent.** *Transfer Color from Visible Rasters to
Texture* (`filter_color_projection`) blends the projected rasters with angle, distance,
border, silhouette and alpha weights. It does not move any color. This method moves
colors along the surface to register the views before blending, and stitches in the
gradient domain afterwards. Expect it to remove ghosting and seams that blending
keeps, at the cost of runtime (the paper reports 71–83 s for 9–16 views at 1024², and
842 s and 1725 s for 33 and 55 views) and of the method's own failure cases: drift where
the views overlap little, and twisting on repeated patterns whose count differs between
views (paper Figs. 13, 15).

## Upstream, measured

| | |
|---|---|
| Repository | `hliuw/TextureConsolidation`, created 2026-10-09, ten commits, last push 2026-10-28 — **actively changing** |
| Licence | BSD-3-Clause |
| Language | C++20, ~120 KB; CMake |
| Programs | `BackProjection`, `TextureFlow`, `TextureDomainStitching` — three command-line tools, connected by files |
| Shared code | `include/TextureConsolidation/`, header-only, ~85 KB |
| Build needs | OpenMP (`REQUIRED`), libpng, libjpeg, zlib |
| GPU | none; the stitcher is built with `NO_OPEN_GL_VISUALIZATION` |

Its three submodules, each pinned to its upstream tip today:

| Submodule | Pinned | Licence | Activity |
|---|---|---|---|
| `mkazhdan/Misha` | `ee14cb8` (2026-10-07), tip of `main` | **No LICENSE file.** Every header carries BSD-3-Clause (Kazhdan & Bolitho, JHU) — the notice PoissonRecon uses | active |
| `mkazhdan/PhongRodriguesVF` | `fa305af` (2026-07-15, "Version 1.15"), tip | BSD-3-Clause | quiet |
| `mkazhdan/TextureSignalProcessing` | `daa6f7b` (2026-10-08, "Version 10.00"), tip | MIT | active |

Eigen arrives vendored inside PhongRodriguesVF and TextureSignalProcessing
(`third_party/include`). All licences are compatible with MeshLab's GPL.

**Input and output are files.** A binary PLY with `vertex_indices` and a per-face
`texcoord` list of six values; a `cameras.json` of OpenCV-convention pinhole cameras
(`w`, `h`, `fx`, `fy`, `cx`, `cy` in pixels, and a 4×4 world-to-camera `w2c`); one PNG
per camera. The output is a single PNG texture.

**Errors: two of the three programs report them correctly; the stitcher does not
always.** Misha's `MK_ERROR_OUT` prints a message and calls `exit(0)`
(`Misha/Exceptions.h`, lines 107 and 148), and the macro is `#ifndef`-guarded.
`BackProjection` and `TextureFlow` both define it to `MK_THROW` before including Misha,
catch in `main`, print `[ERROR] …` and return `EXIT_FAILURE`. `TextureDomainStitching` is
TextureSignalProcessing's `TextureStitching.cpp`, built unmodified: it also catches and
returns `EXIT_FAILURE`, but it does **not** redefine the macro. Its `MK_ERROR_OUT` paths
still exit with status 0, so they look like success. Three are in TextureSignalProcessing:
"Bad component" (singular, `Src/SimpleTriangleMesh.inl:152`), "Expected covering triangle"
(a sanity check, `HierarchyConstruction.inl:222`) and an index-size check (`MultiIndex.h:216`).
The rest are whichever of Misha's 36 files' calls the stitcher reaches. The README's "Bad
components" (plural, line 161) is an `MK_THROW`, and fails correctly.

**Progress is already reported.** With `--verbose`, `BackProjection` prints at the end of
each stage. `TextureFlow` prints `Level l: target …, error … -> …` for every hierarchy level,
which is where the time goes. Those lines are usable as the runner's stage markers as they
stand.

**OpenMP is required by the build, not by the code.** Every parallel loop in the three
programs goes through Misha's `ThreadPool::ParallelFor`. Its backend is the first entry of
an enum that lists OpenMP only `#ifdef _OPENMP`, and `std::thread` (`ASYNC`) otherwise. Only
the `find_package(OpenMP REQUIRED)` in TextureConsolidation's `CMakeLists.txt` makes it
mandatory.

**No symbol clash with the vendored PoissonRecon.** Misha is in `namespace MishaK`,
PoissonRecon 18.75 in `plugins/filter_screened_poisson/Src` in `namespace PoissonRecon`.
But **41 headers share a file name and include guard** with it (`Geometry.h`,
`Array.h`, `PlyFile.h`, `SparseMatrix.h`, …), so the two must never share an include
path.

## Route: unmodified submodule, run as helper processes

Three routes were considered.

| Route | For | Against |
|---|---|---|
| **A. Pinned submodule; build the three programs unmodified; run them as helpers** | Zero patches. An `exit(0)`, an assertion or a crash ends a child process, not MeshLab. Exactly the [QuadWild-BiMDF](../../plugins/filter_quadwild/UPSTREAM.md) arrangement, whose runner (`src/core/helperprocess.{h,cpp}`) already gives progress, cancellation and logging. C++20 and OpenMP stay inside the helper build. | Packaging on three platforms. File I/O round trip, small beside a minute-long solve. |
| B. Link the headers into the plugin | No files, no packaging | The stitcher is a program (`TextureStitching.cpp` with its own `main`), not a library; using it in-process means vendoring it with patches. Every `MK_ERROR_OUT` path would have to be redirected — the macro is `#ifndef`-guarded, so predefining it to throw is possible, but unverified against assertions and other exits. OpenMP and C++20 enter the application build. |
| C. Vendor a subset under `upstream/` | Freezes a moving target | It would be a patched copy of a repository three weeks old and changing daily. |

**Recommended: A.** Under [the third-party rule](../architecture.md#choosing-how-to-bring-third-party-code-in)
it is the "no change, no suitable port" case: a pinned, unmodified submodule. No vcpkg
port exists for any of the four repositories. Route B would turn that into "we patch it",
which means vendoring, for a gain that is not needed.

The pin should be advanced deliberately rather than tracked. Whether the authors will
keep the command-line interface stable is a question for them (see *Open questions*).

## The filter

| | |
|---|---|
| Display name | **Transfer Color from Visible Rasters to Texture (Texture Consolidation)** — the incumbent keeps its name; the newcomer carries the backend ([vocabulary](../vocabulary.md) §6) |
| Id / Python | `transfer_color_from_visible_rasters_to_texture_texture_consolidation` — long; worth a ruling on a shorter backend token before it is fixed |
| Categories | `Transfer/From Rasters`, `Texture` — as the incumbent |
| Domain | `SingleMesh`, `ModifyCurrentMesh`; requires vertices, faces and per-wedge texcoords (as the incumbent) |
| `outputModifies` | `TX` (texture images). The incumbent declares `WT`, which it may not deserve: worth checking separately |
| Plugin | `filter_texture_consolidation`, a thin adapter |

### Parameters

Defaults are the code's, from the README. **The paper's values differ in four places**,
and which to ship is open:

| Label (proposed) | Helper flag | Code default | Paper (App. B) | Group |
|---|---|---|---|---|
| Texture Size | `--res` | 1024 | 1024 | main |
| Output texture | — | new texture on the layer (`textureoutputref`) | — | main |
| Silhouette Falloff (px) | `BackProjection --depthRadius` | 20 | 20 | advanced |
| Confidence Cutoff | `BackProjection --cutoff` | 0.05 | **0.15** | advanced |
| Hierarchy Levels | `TextureFlow --levels` | 6 | **4** | advanced |
| Source Smoothing | `--sDiffusion` | 3e-4 | **0.002** | advanced |
| Flow Smoothness | `--sWeight` | 0.5 | **0.2** generated, 0.1 RGB-D | advanced |
| Per-level Factor | `--factor` | 0.5 | 0.5 | advanced |
| Normalization Extent | `--normalization` | 1e-3 | 1e-3 | advanced |
| Subdivisions | `--sub` | 1 | one 1-to-4 pass to ~20 K vertices | advanced |
| Gradient Weight | `TextureDomainStitching --gWeight` | 0.01 | 0.01 | advanced |

Fixed, not exposed: `--multi --wType 1`, which the README calls required, and the two
`--erode` settings, which it prescribes for this pipeline. No `randomSeed`: nothing in
the pipeline is documented as random. To be confirmed in Phase 0.

**The flow parameters are not scale-free.** The README says so: "the parameters of
`TextureFlow` depend on the scale of the mesh". The adapter can normalize the mesh before
export so that the defaults hold for any layer, but the normalization they assume is not
written anywhere. That is open question 1.

### What the adapter does

1. **Check, then refuse with a message, before anything runs.** At least one visible
   raster. Every raster is perspective (`CameraType 0`) with zero lens distortion,
   because `cameras.json` has no distortion terms; an undistortion step can come later.
   The mesh has an atlas. The mesh is connected, as the README requires; whether one
   connected component is the whole condition is open question 4.
2. **Export to a temporary directory.** The mesh as binary PLY with per-wedge texcoords,
   **with the layer matrix applied**, since the cameras are in world space. One PNG per
   visible raster from its RGBA plane. `cameras.json` from each `CameraShot`.
3. **Run the three helpers in turn** through `HelperProcess::run`, each with its own share
   of the progress bar, and with cancellation honoured between and during them.
4. **Do not trust the stitcher's exit code alone.** Its `MK_ERROR_OUT` paths can report
   success, so until change 1 below lands, a step succeeds only if its expected output files
   exist and decode at the expected size, and no `[ERROR]` line appeared on its output.
   Anything else fails the filter with the helper's last message, and the temporary
   directory is kept and named in the log. The output check is worth keeping afterwards
   too; it is cheap.
5. **Attach the result** as a new texture of the layer. The UVs are not touched.

### Camera conversion, the part that must be tested rather than reasoned

`CameraShot` holds the intrinsics in millimetres: `FocalMm`, `PixelSizeMm`, `CenterPx`,
`ViewportPx`. The pinhole terms follow as `fx = FocalMm / PixelSizeMm.x`,
`fy = FocalMm / PixelSizeMm.y`, with `w`, `h` from `ViewportPx`. On the gargoyle fixture
that is 750 × 502 pixels with focal lengths from about 1,630 to 2,490 px. The rest is
convention, and conventions differ in exactly the ways that still produce a plausible
texture: vcglib's camera looks down its own axis and its image origin may sit at the
bottom, while OpenCV looks down +z with y pointing down and the origin top-left. So
`cy`, the axis flips in `w2c` and the row order of the written PNGs are settled by **a
test, not by derivation**. Project every vertex of a mesh through `vcg::Shot` and
through the converted pinhole model, and require the two to agree to well under a pixel.
A wrong sign here does not crash anything. It shifts every projection, as a wrong
intrinsic did on the raster fixture before (see `AGENTS.md`).

## Tests

- **Descriptor tier:** automatic. The rules on `textureoutputref`, the categories and
  the name grammar apply unchanged.
- **Smoke tier:** no fixture rung has both an atlas and rasters (the `rasters` rung is
  the gargoyle without UVs, and the `parametrized` rung has no rasters), so the filter
  refuses with a message on every rung. It goes into `expectedRefusals()` with that
  reason, which keeps the slow solve out of the smoke sweep.
- **Behavioural:**
  - The camera conversion test above. Fast, no helpers needed.
  - End to end on the gargoyle fixture, atlased with xatlas first, at a small texture
    size: a texture is produced, attached, of the requested size, covering the atlas
    charts that the rasters see.
  - Against the incumbent on the same input, using *Measure Texture Seam Color
    Discrepancy*: consolidation should leave smaller seam discrepancies. That is the
    claim the method makes, and a measurement the repository already has. The paper's
    own image metrics (LPIPS, SSIM, ECD) do not all favour it, and it says why
    (App. C), so the assertion should not use them.
  - Failure path: a disconnected mesh is refused before any helper runs.
- **Dataset:** the vase from the upstream release (`example-data/vase.zip`) — your and
  Callieri's data, by the README — is `texture_consolidation_vase` in
  `tests/datasets/datasets.json`, with its sha256 and size. Its licence is `UNRESOLVED`
  and it is not CI-eligible until you settle that; no test uses it yet.

## Packaging

- **Submodule** `external/texture-consolidation`, recursive (its three submodules come
  with it), registered and pointing at the fork while its changes are proposed upstream.
- **Helper build** through `ExternalProject_Add`, as QuadWild does, targeting the three
  programs and passing our toolchain, so that its `find_package` calls find vcpkg's
  libraries.
- **vcpkg** gains `libpng` and `libjpeg-turbo`; `zlib` is already installed.
- **OpenMP** is `REQUIRED` upstream and stays so (change 2 below was not pursued). A
  missing OpenMP inside an `ExternalProject` fails the whole build at build time, so on
  macOS the plugin looks for a runtime at configure time with the hints
  `filter_screened_poisson` uses (`OpenMP_ROOT`, then Homebrew's `libomp`) and skips
  itself with a message when there is none; GCC and MSVC bring their own. On macOS the helpers then need a `libomp` at run
  time: whether the one the application already ships is reachable from
  `Contents/Helpers` is to be verified.
- **Bundling:** `meshlab2_bundle_quadwild` is QuadWild-specific, and the three release
  workflows (AppImage, Windows portable, macOS DMG) have QuadWild-specific steps. With
  a second helper package the bundling function should become a general one taking a
  name, executables and resources, rather than being copied. That is a small refactor,
  with its own mapping table when it happens.

## A fork with changes proposed upstream

You know the authors, so the friction can be removed at the source, not worked around
here. The changes below are ordered by how much integration code each saves, and each is
small enough to be a pull request on its own. **None touches Misha or
TextureSignalProcessing**: all three live in TextureConsolidation's own repository.

| # | Change | Size | What it saves MeshLab |
|---|---|---|---|
| 1 | Build the stitcher from a two-line wrapper, `src/TextureDomainStitching/TextureDomainStitching.cpp`, that defines `MK_ERROR_OUT(...)` to `MK_THROW(__VA_ARGS__)` and then includes TextureSignalProcessing's `TextureStitching.cpp`, exactly as their other two programs already do. | one new file, one CMake line | Every failure then exits non-zero, so the exit code can be trusted. |
| 2 | `find_package(OpenMP)` without `REQUIRED`; link it if found. The code already falls back to Misha's `std::thread` pool. | two CMake lines | No `libomp` to build, bundle and sign on macOS, and no configure-time check to disable the plugin. Measured in Phase 0: 27.5 s with OpenMP, 28.4 s without, on the vase. **Not pursued**: MeshLab already ships `libomp`, and the no-OpenMP build crashed in Misha (see the status below). |
| 3 | Make `TextureFlow`'s parameters independent of the mesh's scale, for example by normalizing internally to a unit bounding-box diagonal or area, *or* write down the scale the defaults assume. | small, but it changes their results | The adapter's normalization, and open question 1. It is the change most useful to other users too, since their README already warns about it. |
| 4 | A version tag once the interface settles. | none | A release to pin, not a daily commit. |

Not needed, because the code already does it: error exit codes in `BackProjection` and
`TextureFlow`, progress output (`--verbose`), and any GPU dependency. A one-file `LICENSE`
in Misha would also help a packager, since GitHub reports its licence as unknown even
though every header is BSD-3-Clause. That is the authors' own call.

**Status, 2026-10-29.** Four forks carry the changes, each on its default branch, and
TextureConsolidation's `.gitmodules` points at the other three:

| Fork | Commits beyond upstream | What they contain |
|---|---|---|
| [cignoni/TextureConsolidation](https://github.com/cignoni/TextureConsolidation) (on upstream `557829d`) | `f42e50f`, `3598fe7`, `d52ce2a`, `7ff6379` | integration-only: submodule URLs and gitlinks |
| | `4efb2e6` | `EIGEN_HAS_STD_RESULT_OF=0` (Eigen 3.3.7 under C++20 and libc++); the stitcher built through the `MK_ERROR_OUT`-to-throw wrapper (change 1: 48 instantiations of `MishaK::ErrorOut` before, none after); the guard against an all-NaN overlap in `TextureRegistration.inl` |
| | `e5104a9` | advected confidence zeroed outside the atlas; `BackProjection` variance summed in order |
| [cignoni/Misha](https://github.com/cignoni/Misha) (on `ee14cb8`) | `bcc2065` | `MishaK::Polynomial::NumCoefficients<…>` qualified, which Apple clang 21 needs. Misha PR #7 does not contain it |
| | `b8199a8` | nearest-simplex ties broken by index (`Texels.inl`) |
| [cignoni/TextureSignalProcessing](https://github.com/cignoni/TextureSignalProcessing) (on `daa6f7b`) | `b7d2d65` | the clipping guard (`PolygonClipping.h`) |
| | `8c6f35a` | scheduling-independent hierarchy, operators, quadrature and components. *Its message says "bit depth"; the content is this* |
| | `baa775b` | `TextureStitching.cpp` and `ImageIO.h`: the bit-depth refactor, the cell-confidence padding fix and the ordered scatter, together |
| [cignoni/PhongRodriguesVF](https://github.com/cignoni/PhongRodriguesVF) (on `fa305af`) | `2b96675` | deterministic system assembly and tangent frames. The block-bounded assembly is not committed yet |

**Upstream does not build on macOS** without the first two fixes (its README claims Ubuntu
with GCC 13, and MSVC): Eigen's `std::result_of`, and Misha's `Polynomial.h:105`, where
clang resolves `NumCoefficients< Dim , Degree >()` to the member being declared.

For pull requests, TextureSignalProcessing's commits mix three proposals (`baa775b` holds
a memory fix, a determinism fix and the refactor), so they want splitting first. Change 2
was not pursued: OpenMP stays `REQUIRED`, as upstream has it, since MeshLab already ships
`libomp`. Once everything lands upstream, the integration-only commits are dropped and the
submodule returns to `hliuw/TextureConsolidation`.

**How this fits the third-party rule.** The submodule would point at the fork while the
pull requests are open, and back at upstream once they merge; that is a URL change in
`.gitmodules`. MeshLab's tree still carries no patch, so it is still the "pinned,
unmodified submodule" case. `UPSTREAM.md` and `external/README.md` must say that it is a
fork, which commits it carries beyond upstream, and that the goal is to have none. A fork
that stops being proposed upstream would become a vendored tree in all but name.

## Phases

**Phase 0 — feasibility, no MeshLab code** (about a day). Add the submodule (done: it
points at the fork). Then build the three programs with our vcpkg on macOS. Run the upstream vase example and
time it. Hand-export the gargoyle, atlased by xatlas, to their format with a throwaway
script, and run it end to end. Read the helpers' output to find stage markers for the
progress bar, and confirm that nothing in the pipeline is random. *Exit:* both datasets
produce a texture, and the runtime is known.

**Phase 1 — the adapter** (two to three days): camera conversion and its projection test,
the plugin and descriptor, the checks, the error detection, attaching the texture, and the
tests above.

**Phase 2 — packaging** (one to two days): the general bundling function, the three
workflows, `libomp` on macOS, the About entry, `UPSTREAM.md`, and the `external/README.md`
paragraph saying why it is a submodule and not a port.

**Possibly later:** the paper's drift mitigation (registering to a *stationary* coarse
texture, Fig. 15b), if the programs expose it — the README lists no such option; and lens
undistortion, so that distorted rasters can be used instead of refused.

## Status, 2026-10-29 (Phase 1 done; Phase 2 not started)

**Phase 0.** The authors' vase example (`texture_consolidation_vase` in
`tests/datasets/datasets.json`, licence unresolved, not used in CI) runs end to end
through the three programs: 27.5 s with OpenMP, 28.4 s without. Building without OpenMP
exposed a crash in Misha's `std::thread` `ParallelFor`, whose futures were a `static`
local shared between concurrent calls. A fix was written but is not committed anywhere:
the helpers build with OpenMP, which does not reach that code.

**Phase 1.** `plugins/filter_texture_consolidation` provides *Transfer Color from Visible
Rasters to Texture (Texture Consolidation)*. It builds the three helpers through
`ExternalProject` with vcpkg's `libpng` and `libjpeg-turbo`, and finds them in the build
tree. What it does:

- refuses: no atlas; fewer than two visible rasters; non-perspective or distorted cameras;
  an atlas of about one chart per face (`facesPerChart < 2`). The last was found by the
  smoke tier's raster rung, whose 20,000 one-triangle charts held the stitcher for minutes
  at 9–12 GB. A real xatlas atlas of the same mesh has 300 charts, and the vase 65;
- scales the mesh and cameras to surface area 5, the scale the vase data was prepared at,
  and converts each `vcg::Shot` to an OpenCV pinhole camera (`pinholecamera.h`);
- checks every helper's outputs, keeps the intermediate files on failure (and on request
  with `MESHLAB2_TEXTURE_CONSOLIDATION_KEEP`), and turns the stitcher's "Failed to
  factorize matrix" into advice to raise the texture size: on the gargoyle, 256² usually
  fails, 512² works with warnings, and 1024² is clean.

**Every texture produced before 2026-10-29 was corrupt.** Two out-of-bounds reads in
TextureSignalProcessing gave whole charts arbitrary weights, so the same inputs came out
with all charts black, all charts red, or, occasionally, right. The test missed it: it
counted non-black texels, and the grey padding between charts alone came to 29.4%. The
"95.8% coverage" run once blamed on NaN was probably a correct one.

Tests, all on the gargoyle raster fixture:

- `textureConsolidationCameraMatchesShotProjection`: the conversion agrees with
  `Shot::Project` on every visible vertex within 0.05 px, gives the same pixel index as
  MeshLab's raster sampling, and is unchanged by world scale;
- `textureConsolidationBuildsTextureFromRasters`: xatlas atlas, then 1024² at 3 levels; a
  1024×1024 texture is attached and covers more than 20% of it (99.8% since the fixes).
  The whole filter then runs again and must give the same pixels. Also asserts the
  refusals for no atlas and for fewer than two rasters. The 20% threshold no longer
  separates a good texture from a black one, and should be raised;
- the smoke tier lists the filter in `expectedRefusals()`: no rung has both a real atlas
  and rasters.

The About box and the third-party test list TextureConsolidation, and the third-party
test classifies `libpng` and `libjpeg-turbo`. The vase opens in MeshLab through a project
written from its `cameras.json` (the inverse of `pinholecamera.h`; the round trip
reproduces every camera to float precision), but has not yet been run through the filter.

**Builds.** On macOS the plugin needs an OpenMP runtime (`OpenMP_ROOT`, or Homebrew's
`libomp`) and skips itself without one. The macOS DMG workflow turns it off: there the
stitcher took an hour on a 7 GB runner, and the DMG cannot bundle the helpers yet. Windows
(MSVC) builds all three helpers, without running ctest. The Linux AppImage moved from
Ubuntu 22.04 to 24.04, whose GCC 13 the helpers need (see
[release_process.md](../release_process.md)); there the helpers build and both tests
pass. The plugin no longer declares a dependency on the helper build, which had held every
target linking it, all the tests included, behind the stitcher.

**Compile cost.** The stitcher is one translation unit and dominated the build: precision
(2 values) and bit depth (4) were run-time options dispatched to template parameters, so
the whole `Stitching` class was compiled 8 times, and the operator assembly inside each
copy for 6 quadrature values, 2 sanity-check settings and 4 call sites, about 384 copies
of which a MeshLab run executes one. The bit depth reaches only image reading and writing,
and is now a run-time value. With Apple clang at `-O3`:

| | Time | Peak memory | Object file |
|---|---|---|---|
| Before | 151 s | 4.4 GB | 32.7 MB |
| After | 57 s | 2.6 GB | 14.9 MB |

The refactor alone changes no output: a serial run is byte-identical to the original
source with only the memory fixes, on the gargoyle and on the vase. Dropping double
precision would halve it again (32 s, 1.7 GB), but `--double` is a user option, so that is
the authors' call. GCC takes about three times as long as clang on this file.

**Memory errors fixed.** Found with AddressSanitizer and UBSan; all three programs now
run clean on both datasets, except a harmless conversion in Misha's PLY reader, which
eagerly casts every float it reads to the integer types:

- **Cell confidences not padded** (`TextureStitching.cpp`, `Init()`). When UVs come
  within half a texel of the border, the textures and per-texel confidences are padded and
  the system built at the padded size, but the per-cell confidences were not, and were
  read past their end. Needs UVs near the border, which xatlas produces.
- **Clipping an emptied polygon** (`PolygonClipping.h`). The integration clips a polygon
  against a triangle's three edges in turn; once one edge leaves nothing, the next read
  the vertex before index 0. Fails on the authors' vase too.
- **Uninitialized advected confidence** (`TextureAdvection.inl`). Only texels supported by
  the atlas were written; interpolation and erosion then read the rest. The likely cause
  of the NaN in about one `TextureFlow` run in five or six, and of the infinite loop
  `4efb2e6` guards against.

**Repeatability.** With those fixed, the schedule-dependent paths were made ordered. The
target is identical inputs, binaries and thread count on one platform, not bitwise
equality across compilers or architectures:

- `BackProjection.inl`: depth-Laplacian variance summed in pixel order, rather than by
  atomic floating-point addition, whose rounding depends on the schedule;
- Misha `Texels.inl`: equal-distance candidate simplices resolved to the lowest index;
- PhongRodriguesVF `SimplicialMeshProcessing.phong.inl`: tangent frames from the
  coordinate axis with the largest residual, instead of `RandomSpherePoint`, whose shared
  `rand()` ran inside a parallel loop;
- PhongRodriguesVF `SimplicialMeshProcessing.integration.inl`: element matrices evaluated
  in parallel and added in simplex order, in blocks of 65,536 simplices so that memory
  stays bounded (about 42 MB) whatever the mesh size; the result is the same for any
  block size (checked at 1,024);
- TextureSignalProcessing: components numbered in triangle order
  (`SimpleTriangleMesh.inl`); supporting and covering triangles assigned in triangle order
  (`HierarchyConstruction.inl`); boundary coefficients collected by chart, not worker
  (`Operators.inl`); integration samples statically scheduled before their merge
  (`QuadratureIntegration.h`, so repeatable for a given thread count only); solved colors
  scattered in node order (`TextureStitching.cpp`).

Serial stitcher runs are now byte-identical; parallel ones differ by at most 2/255 in a few
thousand texels, from floating-point reduction order. The stitcher's optional `--jitter`
stays off: `--jitter 0` seeds from the clock.

**Not done:**

- committing the block-bounded assembly in PhongRodriguesVF;
- splitting TextureSignalProcessing's commits into separable pull requests, and the pull
  requests themselves, to `hliuw/TextureConsolidation`, `mkazhdan/Misha`,
  `mkazhdan/TextureSignalProcessing` and `mkazhdan/PhongRodriguesVF`;
- bundling the helpers into the three packages (Phase 2), with `libomp` on macOS, and the
  plugin back on in the DMG;
- `UPSTREAM.md` for the plugin and the `external/README.md` paragraph;
- a coverage test that measures the charts rather than the whole image;
- running the vase through the filter itself, and the seam-discrepancy comparison test.

**For the authors:** the three memory errors and the scheduling-dependent paths above,
each with its fix on the forks; the compile cost and the bit-depth refactor; and, from
before, that `TextureFlow` looped forever on an all-NaN overlap (now an error in
`4efb2e6`). Whether a non-finite advected sample should count as zero confidence is
theirs to say, though with the advection output initialized none has been seen since.

## Open questions

These are about the algorithm's intended conventions, so they go to you, and through you
to the authors:

1. **Scale.** What mesh scale do `TextureFlow`'s defaults assume? Should the adapter
   normalize — to a unit bounding-box diagonal, unit area, or something else — so that
   one set of defaults suits every layer?
2. **Defaults.** The code and the paper disagree on four values (levels 6 vs 4, source
   smoothing 3e-4 vs 0.002, flow smoothness 0.5 vs 0.2, cutoff 0.05 vs 0.15). Which should
   MeshLab ship? Are the README's values newer?
3. **Stability.** The repository is three weeks old and changes daily. Do the authors
   intend the three-program interface to stay, or is a library API coming? That decides
   whether to pin now or wait.
4. **Preconditions.** What exactly makes the stitcher fail with "Bad components"? A
   single connected component, a manifold mesh, non-overlapping charts, no zero-area UV
   triangles? The adapter should refuse each of these with a message rather than let a
   helper exit.
5. **Real photographs.** The vase is real photographs. Should a separate default set, the
   paper's RGB-D values, be offered for photographs rather than generated renderings?
