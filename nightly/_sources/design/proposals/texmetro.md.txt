# TexMetro: Texture and Parametrization Quality Metrics

This document evaluates bringing [TexMetro](https://github.com/cnr-isti-vclab/texmetro)'s
metrics into MeshLab, either by vendoring the tool as a subrepo or by reimplementing
it on QRhi, and recommends a third route: **reimplement the metrics on the CPU, with a
deterministic texel rasterizer, as algorithms in vcglib and thin `Measurement` filters in
MeshLab**. **Stages 1 to 4 are implemented (2026-10-24), but for the optional seam polyline.** The recommendation
rests on measurements made for this proposal, listed under [Evidence](#evidence).

See also: [Edge Support](edge_support.md), [Adding a Filter](../adding_a_filter.md),
[Filter Organization](../filter_organization.md), [Vocabulary](../vocabulary.md),
[Rendering](../rendering.md), [Test Data](../test_data.md), [Data Model](../data_model.md).

## Status and evidence

Evaluated on **2026-10-23** against TexMetro at commit
`cfc35548dad7526a1c6936293a213d52b5fab7a0` (2020-04-24; the first commit is 2019-07-19;
the authors are Andrea Maggiordomo and Paolo Cignoni). Every source file was read:
`src/` is 1,701 lines in nine files. It is GPL-3.0, as MeshLab is, so nothing in it is
blocked by licence. The vcglib checkout surveyed for what already exists is at
`d5f5ae997`.

Two kinds of statement follow, kept apart:

- **Read** from the code: what each metric computes, and its quirks.
- **Measured** on an Apple M3 Max (macOS, Qt 6.11.2), with throw-away probes in the
  session scratchpad (not committed), compiled `-O2` against this checkout's vcglib:
  the OpenGL version the machine offers, whether TexMetro's shaders compile, TexMetro's
  quasi-conformal formula against vcglib's, and a CPU texel rasterizer. The inputs were
  two real photogrammetry atlases of the same gargoyle, `gargo3M_good.ply` and
  `gargo3M_bad.ply` (2,999,914 triangles each, a 4096 × 4096 texture each, in
  `tests/local/COLOR_gargoyle/`, which is a local-only dataset).

Not measured: TexMetro itself was **neither built nor run**; the part of it that does the
texel metrics cannot work on this machine (below). So nothing here compares our numbers
to its numbers, and the validation that does so is [Stage 0](#staging). Other GPUs and
operating systems were not measured either.

## Recommendation

| | Option | Verdict |
|---|---|---|
| **A** | Vendor TexMetro as a subrepo (or call it as an external tool) | **Reject.** Its raster shaders are GLSL 4.30, so an OpenGL 4.3-class driver, and a Mac stops at 4.1; it has its own mesh loader and works on files, not on the document. Keep it as the reference to validate against. |
| **B** | Reimplement the texel raster on QRhi | **Defer.** Possible (B1 needs no compute), but the GPU can only speed up half of a step that costs 0.35 s, its counts depend on the device, and it cannot be tested in `ctest` or run headless. Worth doing later, for *visualization* only. |
| **C** | CPU metrics and a deterministic CPU texel rasterizer, written **into vcglib**; thin `Measurement` filters in MeshLab | **Recommended.** No new dependency or plugin; runs on every platform, in `ctest`, in `pymeshlab` without a GPU; 0.9 s for a full 13-level pyramid on a 3 M-face atlas. The same vcglib code can serve TexMetro itself, which would then no longer need OpenGL. |

Of TexMetro's 68 top-level report keys, 14 need nothing built (MeshLab already reports
them, or the document already knows them), 1 is derivable, 1 (`orientable`) is dropped by
decision, and **52 are new**. 34 of the 52 need no rasterization at all. They are plain mesh and UV arithmetic, and vcglib
already holds most of the pieces (see [Where the code lives](#where-the-code-lives)).

## Decisions taken

Settled by the maintainer on **2026-10-23**, after the first version of this proposal:

- **Option C, with the algorithms in vcglib** and thin filters in `filter_measure`
  ([Where the code lives](#where-the-code-lives)).
- **`orientable` is dropped** from the report. `oriented` stays, as it needs nothing new.
- **The category `Measurement/UV Mapping` is approved.** The vocabulary table and
  `src/plugins/filtercategories.{h,cpp}` change together, in the stage whose first filter
  uses it (Stage 2), not before: an approved term with no filter would be dead code.
- **Percentiles use `vcg::Histogram`** at TexMetro's own bin counts, which reproduces the
  published values; the sorted-pairs alternative is not built.

## What TexMetro reports, against what MeshLab has

The keys are those of TexMetro's `README.md` JSON table. "Needs raster" means the value
depends on the texel raster, the one part TexMetro does with OpenGL.

**Mesh metrics (20 keys).**

| TexMetro key | In MeshLab today | Plan |
|---|---|---|
| `fn` `vn` `en` `en_b` | `faces_number` `vertices_number` `edges_number` `boundary_edges` (*Measure Topological Properties*) | none |
| `area` | `surface_area` (*Measure Geometric Properties*) | none |
| `boundary_loops` `nonmanif_edge` `nonmanif_vert` `connected_components` `genus` `vnunref` | `number_holes` `non_two_manifold_edges` `non_two_manifold_vertices` `connected_components_number` `genus` `unreferenced_vertices` | none |
| `area_normalized` | derivable from `surface_area` and `bbox_dim_*` | `normalized_surface_area` (**Stage 1, done**) |
| `boundary_len`, `boundary_len_normalized` | **missing.** The only `perimeter` output belongs to the *selection* measure, not the whole mesh | `boundary_length`, `normalized_boundary_length` in *Measure Geometric Properties* (**Stage 1, done**) |
| `fndup` `fnzero` `fndegen` | **missing from the filters.** vcglib counts two of them already: `Clean::RemoveZeroAreaFace(m, false)` and `RemoveDegenerateFace(m, false)` count without deleting (TexMetro's `CountDegenerateFace` is the same test); `RemoveDuplicateFace` has no counting mode | `duplicate_faces`, `zero_area_faces`, `degenerate_faces` in *Measure Topological Properties*, with the flag on `RemoveDuplicateFace` (**Stage 1, done**) |
| `oriented` | **missing** (`is_mesh_two_manifold` is a different property). vcglib's `IsCoherentlyOrientedMesh` gives it without touching the mesh | `is_mesh_oriented` in *Measure Topological Properties* (**Stage 1, done**); it is absent when the mesh has non-manifold edges, where an orientation means nothing |
| `orientable` | exists in vcglib only inside `OrientCoherentlyMesh`, which flips faces, so a report cannot use it | **dropped** (decision) |
| `mesh` | a file name, not a metric | none |

Totals: 11 covered + 1 derivable + 6 missing + 1 dropped + 1 not a metric = 20.

**UV and texture metrics (48 keys).**

| TexMetro keys | Count | In MeshLab today | Needs raster | Plan |
|---|---|---|---|---|
| `num_charts` `en_uv` `en_uv_b` `area_uv` `boundary_len_uv` `num_null_charts` `mapped_fraction` `nfolds` | 8 | none reported (charts exist inside the defragmentation code, not as a report) | no | *Measure UV Mapping* (**Stage 2, done**) |
| `ntex` `textures` | 2 | known to the document | no | none |
| `qcd_perc1` … `qcd_perc99` (11), `qcd_min` `qcd_max` `qcd_avg` `qcd_stddev` `qcd_discardedArea` | 16 | **none.** *Compute UV Distortion* has angle, area, edge, L2 and L∞ stretch, not σmax/σmin | no | metric in *Compute UV Distortion* (**Stage 1, done**); statistics in *Measure UV Mapping* (**Stage 2, done**). TexMetro's JSON also carries `qcd_rms` and `qcd_var`, which its README omits; both are reported |
| `sf_perc1` … `sf_perc99` (11), `sf_min` `sf_max` `sf_avg` `sf_stddev` `sf_discardedArea` | 16 | **close.** *Compute UV Distortion → Area distortion* is `\|sf − 1\|` (shown below), minus the sign and TexMetro's outlier rule | **yes**, for the outlier cutoff | *Measure UV Mapping* (**Stage 3, done**). The JSON also carries `sf_rms` and `sf_var`; both are reported |
| `seam_discrepancy_avg` `_min` `_max` `_stddev` | 4 | none | no (needs the texture images) | *Measure Texture Seam Color Discrepancy* (**Stage 4, done**), with `_rms` and `_var`, which the JSON carries and the README omits, and a count, `seam_edges_number` |
| `occupancy`, `texture_N` | 2 | none | **yes** | `occupancy` in *Measure UV Mapping*, `texture_N` (and `ntex`) in *Measure Texel Coverage* (**Stage 3, done**) |

Totals: 8 + 2 + 16 + 16 + 4 + 2 = 48. `texture_N` is a list of eight raster counters per
MIP level, listed below, so the whole JSON is 68 top-level keys plus those eight.

**Raster counters, per texture and MIP level (8 keys, all need the raster).** `rw` and
`rh` (the level's size), `totalTexels`, `totalTexels_bilinear`, `overwrittenTexels`,
`lostTexels`, `texelClashes`, `boundaryTexels`. None exists in MeshLab.

**Why `sf` is nearly there already.** With the existing global scale
`s_A = ΣA₃D / ΣA_UV`, *Area distortion* is `|s_A · A_UV / A₃D − 1|`. TexMetro's
per-face `sf` is `(A_UV / A₃D) / (ΣA_UV / ΣA₃D)`, which is `s_A · A_UV / A₃D`. So the
existing metric is `|sf − 1|`: the sign (compression against expansion) is lost, and
the sums run over every face with oriented UV area, where TexMetro's skip faces beyond
its outlier cutoff and use unsigned area.

## Evidence

### TexMetro's raster path cannot run on a Mac

The machine offers OpenGL **4.1** (`4.1 Metal - 90.5`, whatever version is asked for:
4.3, 4.2 and 4.1 were each requested and each returned 4.1). TexMetro asks for a 4.2 core
context and its shaders start with `#version 430`. Both were extracted verbatim and
compiled on this machine's driver:

```
ERROR: 0:1: '' :  version '430' is not supported
ERROR: 0:3: 'uimage2D' : Reserved word.
```

Neither shader compiles, so the texel metrics (the 8 counters, `occupancy`, and through
the outlier cutoff all of `sf`) cannot be computed here. The same driver reports
`GL_SUBPIXEL_BITS = 4`, the minimum the specification allows, and
`GL_MAX_TEXTURE_SIZE = 16384`.

### vcglib already computes the quasi-conformal distortion

TexMetro builds a local isometry, a 2 × 2 Jacobian and a closed-form singular-value
ratio, `qcd = σmax/σmin`. vcglib's `Distortion::mappingTransform2D` followed by
`svd2x2` produces the same two singular values, since a rotation of the source frame does
not change them. Both were run on all 3 M faces of each atlas, with the UVs scaled to
texels as TexMetro does:

| | good | bad |
|---|---|---|
| faces TexMetro would count (non-zero 3D and UV area) | 2,968,650 | 2,903,156 |
| largest relative difference, TexMetro vs vcglib, over those faces | 8.1 × 10⁻¹² | 6.4 × 10⁻¹² |
| faces finite and below the cutoff of 100 for only one of the two | 0 | 0 |
| faces TexMetro skips for zero UV area | 30,085 (1.0 %) | 94,754 (3.2 %) |
| time for 3 M faces: TexMetro's formula / vcglib | 0.10 s / 0.04 s | 0.10 s / 0.04 s |

On the skipped faces the two formulas disagree (infinite on one, a huge number on the
other), which is why the zero-area guard is part of the metric and not an optimisation.
A face is *not* zero-area in 3D in either atlas; the zero-UV faces are degenerate UV
triangles, and in the bad atlas they are three percent of the mesh.

The conclusion for the design: **QCD is one call to vcglib plus the guard**, so it is a
few lines added to `Distortion`, next to the metrics already there, and costs almost
nothing.

### vcglib already partitions the mesh into UV charts

TexMetro splits vertices at the seams and floods across face adjacency (`GenerateAtlas`).
vcglib has the same thing without touching the mesh's vertices:
`UpdateTopology::FaceFaceFromTexCoord` makes two faces adjacent only across an edge whose
UVs agree on both sides, so a connected-component count of the result is the number of
charts. On the two atlases it gives **1,961** and **1,716** charts in 0.48 s, the same
numbers as an independent union-find over shared edges. What is missing is only the
per-face chart id, which the existing `ConnectedComponentIterator` provides.

### A CPU rasterizer is fast, and it is the more reproducible choice

A prototype of about 55 lines rasterizes UV triangles on a fixed-point grid with a chosen
number of sub-pixel bits `S`, using the top-left rule (so a texel on a shared edge
belongs to exactly one triangle), a saturating 16-bit coverage count and a 32-bit
last-writer chart id per texel. It then runs TexMetro's counting passes verbatim. On a 4096² texture and
3 M triangles, single-threaded:

| step, level 0 | time |
|---|---|
| rasterize (clearing the two buffers included) | 0.17 s |
| TexMetro's per-texel 3 × 3 passes (bilinear footprint, boundary, clash) | 0.18 s |
| **one MIP level** | **0.35 s** |
| the whole 13-level pyramid, 4096² down to 1 × 1 | **≈ 0.9 s** |

The pyramid figure is derived, not timed on its own: the whole probe run takes 3.6 s,
which also loads the mesh, labels the charts and computes QCD, and holds three full
pyramids, one per sub-pixel precision. Coarser levels cost less (0.06 s at 512²). Only
the first row is something a GPU could do, and it is half of a step that already takes a
third of a second. The prototype's buffers needed 6 bytes per texel (a 16-bit count and a
32-bit id). The implementation that landed counts in 32 bits too, so that a texel covered
by more than 65,535 triangles (a small virtual texture under a dense mesh) is exact: **8
bytes per texel**, 134 MB at 4096² and 2.1 GB at 16384², which the bands of Stage 3 keep
under a gigabyte. TexMetro keeps two 32-bit GPU textures and two 32-bit CPU copies, 16
bytes per texel, 4.3 GB at 16384². The landed version reproduces the prototype's counts
to the texel at levels 0, 3, 6 and 9, gives the same counts in bands of any height, and
counts the whole 13-level pyramid in 1.09 s.

### The published counters depend on the rasterizer's sub-pixel precision

The same prototype was run with `S` = 4 (what this machine's GL driver reports, and the
specification's minimum), 8 and 16, on the same triangles. The **level 0** counters, in
texels (`clash` is TexMetro's rule, diagonal neighbours only, see the quirks below):

| atlas, `S` | total | overwritten | lost | clash | boundary |
|---|---|---|---|---|---|
| good, 16 | 6,380,465 | 8,077 | 8,652 | 9,102 | 425,195 |
| good, 8 | 6,380,479 (+14) | 8,087 (+10, 0.12 %) | 8,666 (+14, 0.16 %) | 9,097 (−5) | 425,171 (−24) |
| good, 4 | 6,380,585 (+120, 0.002 %) | 8,127 (+50, 0.62 %) | 8,752 (+100, 1.2 %) | 9,089 (−13) | 425,087 (−108) |
| bad, 16 | 6,454,993 | 11,962 | 12,628 | 9,522 | 351,499 |
| bad, 8 | 6,454,976 (−17) | 11,966 (+4) | 12,629 (+1) | 9,519 (−3) | 351,507 (+8) |
| bad, 4 | 6,455,051 (+58) | 11,991 (+29, 0.24 %) | 12,696 (+68, 0.54 %) | 9,525 (+3) | 351,302 (−197) |

The large counters move by a few parts per million. The rare ones (overwritten, lost)
move by about 0.1 % at 8 bits and about 1 % at 4. At coarse levels, where the counts
themselves are small, the effect is larger: on the good atlas at 512², going from 16 to
4 bits changes *lost texels* from 122 to 161 (+32 %).

The consequence for the design is that **bit-for-bit agreement with a GPU is not a
meaningful goal**: the numbers in the published benchmark were produced by whichever
drivers ran them, and a different driver would have printed slightly different rare
counts. A CPU rasterizer pins the precision (a parameter, default 8) and the tie rule,
and returns the same numbers on every machine. How closely it must match the benchmark is
a Stage 0 question.

### What QRhi offers

From the Qt 6.12 documentation (the project requires Qt 6.11 or later):

- `QRhi::Compute` (compute shaders, image load/store, storage buffers) is absent on
  OpenGL older than 4.3. The project's OpenGL floor is **3.2 core** (shaders are baked as
  GLSL 150), and Linux runs on OpenGL, so a compute path needs a fallback there. Metal and
  Direct3D have compute.
- `QRhi::ReadBackAnyTextureFormat` is typically false on OpenGL, where only `RGBA8`,
  `BGRA8` and (non-ES) `R8` are guaranteed readable. `R32UI`, TexMetro's format, exists
  since Qt 6.9 but need not be readable back on the OpenGL backend.
- `QRhi::TextureSizeMax` is "typically 4096 – 16384", and a larger texture "is expected to
  fail", so a 16 K texture, or a bigger one, needs tiling. This machine reports 16384.
- QRhi "does not at the moment allow freely creating and submitting command buffers": all
  GPU work is inside a frame, with readbacks completing after it.
- The render path lives in the `MeshLab` executable and needs a window server and a GPU:
  `tests/` cannot link it, and `HeadlessRenderContext` needs both, which is why the render
  probes are run by hand (see [Rendering](../rendering.md#verifying-the-render-path)). A
  texel metric on QRhi would have no `ctest` row and could not run in `pymeshlab` on a
  machine without a display.

### Quirks found in TexMetro's code

Read, not run. They matter because a reimplementation either reproduces them or
deliberately does not, and the published numbers include them. Questions for the authors
are collected in [Open questions](#open-questions).

| # | Where | What it does |
|---|---|---|
| 1 | `Clash3x3` | The test is `i != row && j != col`, which skips the centre **and the four edge neighbours**, so only the four diagonal neighbours are ever compared. Counting all eight changes `texelClashes` by +3.3 % (good: 9,102 → 9,403) and +4.0 % (bad: 9,522 → 9,907). |
| 2 | `ComputeTexImageInfo` | `checkMask &= BitClear` assigns, but the mask is not read again, so it behaves as a plain test. Harmless. |
| 3 | the id buffer | Each fragment does `imageStore(idbuf, …, chartId)` with no ordering. Which chart "wins" a texel written twice is up to the GPU, so `texelClashes` is not deterministic wherever a texel is overwritten. The coverage *count* is (it is an atomic add). |
| 4 | `ComputeAtlasInfo` | `sf`'s outlier cutoff, `thresholdUV = perc3D · realTextureArea · 100`, uses `realTextureArea = occupancy × texel count`, so `sf` cannot be computed without the raster. |
| 5 | `occupancy` | Taken from the **first** MIP level with any texels, per texture, as `(totalTexels − lostTexels) / (w·h)`. |
| 6 | `totalTexels` | Counts fragments (`Σ n`, overwrites included), not distinct texels. `totalTexels_bilinear` counts covered texels plus every uncovered texel with a covered one in its 3 × 3 neighbourhood. |
| 7 | MIP levels | Each level re-rasterizes the triangles at the smaller size; none is downsampled from the one above. |
| 8 | units | UVs are scaled to texels (`texmetro.cpp`) before every UV metric, so `area_uv` and `boundary_len_uv` are in texels, and QCD sees the texel anisotropy of a non-square texture. |
| 9 | `LoadMesh` | If the UV box of a texture is centred outside [0,1] the UVs are shifted by an integer plus 10⁻³ **UV units** (4 texels at 4096). The raster clips to [0,1] while the seam sampler wraps, so the two disagree for UVs outside the unit square. |
| 10 | texture too large | If a side exceeds `GL_MAX_TEXTURE_SIZE` the level returns all zeros with a warning on `stderr`, and the JSON carries the zeros. |
| 11 | percentiles | Binned `vcg::Histogram<double>`: 500,000 bins for QCD, **20,000,000** for `sf`. Each histogram stores two arrays of that length, so `sf` costs about 320 MB of memory, and a percentile is only as exact as the bin width (2 × 10⁻⁴ and 5 × 10⁻⁶). |
| 12 | seams | Every seam edge is visited from both sides, so each is counted twice; the average is unaffected. Sampling is 5 samples per texel of UV length, bilinear, with `%` wraparound, and the discrepancy is the Euclidean distance of two 8-bit sRGB triples (0 – 441.7), unweighted by perception, weighted in the histogram by 3D edge length. |
| 14 | seams | **The two sides' samples are paired from opposite ends.** The i-th sample of a face's edge is compared with the i-th sample of its neighbour's edge, each taken from the vertex its own face lists first. Two consistently oriented faces list a shared edge in opposite directions, so a point is compared with the *mirrored* point of the other side. It holds for every seam of both atlases (192,582 and 153,018 seam sides, each with the neighbour running the other way). Comparing the same point instead lowers the mean from 12.31 to 10.63 on the good atlas and from 12.12 to 10.48 on the bad one: TexMetro reads about 16 % high, and the two atlases stay as close to each other as before. |
| 15 | seams | The bilinear sampler takes the fractional part with `modf`, which is negative for a negative coordinate while its texel is the floor, so the weights are wrong within half a texel of the left or bottom of the texture. The mean is the same to six decimals with the right fraction. |
| 13 | `nfolds` | A face is a fold when `signbit(its UV area) != signbit(its chart's net area)`. A face with **no** UV area has a zero of either sign, so it counts as a fold or not by the sign of that zero. The native count does not count a zero-area face as a fold: on the two atlases that is **20** faces fewer of 1,231 (good) and **22** of 1,475 (bad), measured by running both rules on the same files. |

Stage 3 reproduces quirks 4 to 7 as they are (the `sf` cutoff from the occupancy, the occupancy
from the first level with texels, `totalTexels` as fragments, and each level rasterized again),
and makes quirk 1 an option, **Clash Neighbours**, defaulting to TexMetro's diagonals. Quirk 3
is settled as the last face in face order. Quirk 10 is gone: nothing is too large for the CPU
rasterizer, which counts in bands.

## Options

### A. Vendor TexMetro (subrepo or external tool)

Brought in as a submodule it would follow the pattern of the other vendored plugins
(an `upstream/` tree, a `UPSTREAM.md`). Against it, measured and read:

- The texel metrics cannot run on a Mac: the shaders do not compile (above). On Windows
  TexMetro needs GLEW, on Linux `-lGL -lGLEW`; none is a MeshLab dependency (MeshLab
  renders through QRhi). It is a qmake project and the build is separate.
- Its `Mesh` type, OBJ/PLY/FBX importers and UV recentring duplicate what the document
  already holds. A vendored copy would work on a file path, not on the layer in front of
  the user, so it could neither respect the document's textures nor produce per-face
  results to look at or select.
- Run as an external tool from a filter (export, spawn, parse the JSON), it keeps the same
  platform limit and adds a process boundary, a temporary export of the model and a
  failure mode for every error message.
- Nothing in it uses `Document`, selection, undo, `outputValues` or the Python API.

What survives is its value as a *reference*: the Stage 0 validation runs it on a Linux or
Windows machine.

### B. Reimplement the raster on QRhi

Two ways, both constrained by the facts under [What QRhi offers](#what-qrhi-offers).

**B1, portable, no compute.** One `RGBA8` render target the size of the MIP level;
colour replaced by the chart id (24 bits), alpha *added* (a blend with different colour
and alpha operations; count saturates at 255); read back with `readBackTexture`, the one
format guaranteed readable on every backend, including OpenGL. The 3 × 3 passes then run
on the CPU anyway, since TexMetro has them there too. Pros: works wherever QRhi does, no
new shader stage. Cons: a render target per level, tiled above `TextureSizeMax`; the UVs
are quantized to 32-bit floats on the way in (about 10⁻³ texel at 16384, finer than the
precision difference above, but not the same as double); the fill rule and sub-pixel
precision are the device's, with the spread measured above; and no `ctest`, no headless
`pymeshlab`.

**B2, compute.** A storage-buffer `atomicAdd` per texel, as TexMetro does with
`imageAtomicAdd`. Needs `QRhi::Compute` (so a fallback on OpenGL < 4.3, the Linux
default), a compute shader baked for GLSL 430 (the project bakes 150 today), and it is the
first compute use in the project. It removes the readback of the counters but not that of
the buffers the CPU passes use.

Both give a number that changes with the machine, in a path no test can reach. B is
worth building **for what only a GPU does well**: drawing the overdraw count and the
clash texels live in the UV view (`renderwidget_uv.cpp` already exists), as a diagnostic.
That is a separate, optional stage ([Stage 5](#staging)) and its numbers would not be
the reported ones.

### C. CPU algorithms in vcglib, thin filters in MeshLab (recommended)

The precedent is in the tree: when *Defragment Texture Atlas* was ported, its OpenGL
atlas rasterizer was replaced by a QImage CPU renderer behind the same API
(`plugins/filter_texture_defragmentation/UPSTREAM.md`). TexMetro's raster is the same
kind of problem, by overlapping authors. The measurements above are what make C safe:
0.35 s a level, deterministic, eight bytes a texel.

What this proposal adds is *where* the code goes. Nothing here is MeshLab-specific: UV
charts, distortion and texel coverage are mesh and UV processing, with no document, GUI
or Qt in them. The repository's rule is that such code is **written into vcglib** when
there is no easy path to it (the Reeb graph and handle/tunnel loops, the planar polygon
tessellation and the plane clipping are the precedents), and TexMetro is such a case: the
only candidate source is a tool whose raster cannot run on a Mac. Hosting it in vcglib
also lets TexMetro itself adopt it later and drop OpenGL and GLEW, so there is one
implementation and two consumers.

## Proposed design (Option C)

### Where the code lives

**vcglib holds the algorithms; MeshLab holds adapters.** The adapters, all in
**`filter_measure`** (the plugin of the `Measurement` family; [Filter
Organization](../filter_organization.md), decision 1: plugins follow the family and split
only for a dependency, and there is none), are: the descriptors, the parameters, the
`QImage` access (`TextureAssociationUtils::loadAssociatedTextureImage`, wrapped in a
texel functor), and the `outputValues`.

What goes into vcglib, checked against what is already there (the library's own rules
are no duplication and less code):

| # | vcglib file | Addition | What it reuses, and why nothing existing will do | Lines (estimate) |
|---|---|---|---|---|
| V1 | `parametrization/distortion.h` | `QuasiConformalDistortion(f)`, `σmax/σmin` of the face's Jacobian; `UVScaling(f, scale)`, the *signed* area ratio | `mappingTransform2D` and `svd2x2`, already there and measured above to agree with TexMetro to 10⁻¹¹. `AreaDistortion` becomes `\|UVScaling − 1\|`, so it is not duplicated | ~15 |
| V2 | `clean.h` | a `DeleteFacesFlag` on `RemoveDuplicateFace`, as `RemoveZeroAreaFace` and `RemoveDegenerateFace` already have | the counting modes that exist | ~5 |
| V3 | `parametrization/uv_utils.h` | UV charts: `PerWedgeCharts` (a per-face chart id and the chart count), `CountUVEdgeNum`, and the chart-relative `Distortion::FoldedNum(m, faceChart)` behind `nfolds`. **Landed in Stage 2**, with an optional per-face label on `Clean::ConnectedComponents` and `AreaUV` taken in double | `UpdateTopology::FaceFaceFromTexCoord` and `ConnectedComponents` do the partition (measured above). `Distortion::IsFolded` tests the sign against zero, not against the chart | ~110 |
| V4 | `parametrization/texel_coverage.h` (new) | `TexelCoverage`: a scan converter on a fixed-point grid with the top-left fill rule, and TexMetro's counters for one MIP level, in bands for large textures. **Landed in Stage 3** | `SurfaceSampler::Texture` / `SingleFaceRaster` is a *sampler*: it tests edges inclusively, so a texel centre exactly on a shared edge is produced by both triangles, which a coverage count would report as overwritten, and it adds border samples. Different job | 262 with its comments (the prototype is ~55) |
| V5 | `parametrization/texel_coverage.h` or a sibling | `SeamDiscrepancy` in `seam_discrepancy.h`: finds the seams and measures one, **templated on a texture object** (its size and the color of a texel) so vcglib keeps no image library. **Landed in Stage 4**, with `UpdateTopology::IsTexCoordSeam` extracted from `FaceFaceFromTexCoord` so they share one test | the seam edges are those the geometric `FaceFace` joins and the texture coordinates cut | 190 with its comments |

Statistics need no addition: area-weighted percentiles use `vcg::Histogram` as it is (see
[Engine](#engine)). The line counts are estimates, not measurements.

Following `vcglib/AGENTS.md`: algorithms are static members of a class templated on the
mesh type; long runs take a `CallBackPos *`, defaulting to null; bad input throws through
`MeshAssert`, it does not `assert`; and there are no regression tests in vcglib, since its
samples are documentation. The behavioural tests live in MeshLab's `tests/test_filters.cpp`
and drive the vcglib code through the filters.

**Each vcglib piece lands as the four-part task** (commit inside `vcglib/`, push to
`origin/devel`, bump the gitlink, land the MeshLab code that depends on it in the same
bump), so [Staging](#staging) is organised around that.

### Filters

Naming follows [Vocabulary](../vocabulary.md): `Measure` reports without modifying,
`Compute` stores an attribute, `Select` changes selection. **The category
`Measurement/UV Mapping` is approved (2026-10-23):** it extends the closed ontology (the
current subcategories are Geometric, Topological, Statistics), and `Measurement/Texture`
was excluded by form rule 4, which forbids a subcategory that repeats another root's
name.

| Filter (display name) | Id | Kind | Reports or writes |
|---|---|---|---|
| **Measure UV Mapping** | `measure_uv_mapping` | Information | `num_charts`, `en_uv`, `en_uv_b`, `area_uv`, `boundary_len_uv`, `num_null_charts`, `mapped_fraction`, `nfolds`, the 16 `qcd_*` and the 16 `sf_*` keys, `occupancy` |
| **Measure Texel Coverage** | `measure_texel_coverage` | Information | per texture and MIP level: `rw`, `rh`, `totalTexels`, `totalTexels_bilinear`, `overwrittenTexels`, `lostTexels`, `texelClashes`, `boundaryTexels` |
| **Measure Texture Seam Color Discrepancy** | `measure_texture_seam_discrepancy` | Information | `seam_discrepancy_avg`, `_min`, `_max`, `_rms`, `_var`, `_stddev`, `seam_edges_number` |
| **Compute UV Distortion** (existing) | `compute_uv_distortion` | writes face quality | two new metrics, **Quasi-conformal distortion** (`σmax/σmin`) and **UV scaling** (signed `sf`) — done in Stage 1 |
| **Select Folded UV Faces** | `select_folded_uv_faces` | selection | faces whose UV orientation disagrees with their chart's |
| *Measure Topological Properties*, *Measure Geometric Properties* (existing) | | Information | the 6 missing mesh keys of the first table |

`Compute UV Distortion` is the cheapest slice of the whole proposal: two enumerators and
about thirty lines, with a visualization (a face-quality map of where the distortion is)
that TexMetro never had. It needs no raster. The sign matters for UV scaling: the existing
*Area distortion* cannot tell a compressed face from a stretched one, which is exactly
what a texel-density map is for.

**Outputs.** The keys keep TexMetro's names where they do not collide, since they are the
ones in the published benchmark and already lower-case and underscored like MeshLab's
(`qcd_perc50`, `seam_discrepancy_avg`). The raster counters are nested (texture, then
level). `FilterRunRecord.output_values` in the Python binding converts only scalars and
`QVector3D` today: **a list or map becomes `None`**. The binding needs a recursive
conversion of `QVariantList` and `QVariantMap` (about fifteen lines in
`src/python/bindings/module.cpp`, useful to any filter), and then
`json.dumps(result.output_values)` writes a TexMetro-compatible file with no file output
in the filter. A table of old key to new key goes in the filter documentation.

**Parameters** (advanced, with these defaults):

| Parameter | Default | Meaning |
|---|---|---|
| Texture size | from the layer's texture images | or a virtual size `W × H`, for a model with UVs and no image (TexMetro's `--texsize`) |
| Sub-pixel bits | 8 | the rasterizer's fixed-point grid, the one source of device difference above |
| MIP levels | all | or the base level only |
| Outlier cutoffs | 100 for QCD, 100 for UV scaling | TexMetro's constants |
| TexMetro-compatible clash rule | on | reproduces quirk 1; off compares all eight neighbours |

### Engine

Three pieces, the first two in vcglib (V3 and V4 above).

1. **Charts (V3).** A UV chart is a set of faces connected across edges whose two endpoint
   UVs agree on both sides: `FaceFaceFromTexCoord` followed by the component iterator,
   0.48 s for 3 M faces. The geometric `FaceFace` is built first and kept, since the seam
   test needs to tell a UV seam (joined in 3D, detached in UV) from a real border. A
   non-manifold edge, shared by more than two faces, is never joined across.
2. **Texel raster (V4).** The prototype above, grouped by texture index (`WT(i).N()`),
   with the chart id as the last-writer value so the clash count is defined. Memory is
   eight bytes a texel. A texture whose buffers exceed a limit (default one gigabyte) is
   rasterized in horizontal bands, one row of overlap each side, each band taking the
   triangles that touch it. Bucketing the triangles by band is not done: a band scans every
   face, which costs a few hundred milliseconds a band at 3 M faces and is nothing at the
   two or three bands a 16 K texture needs.
3. **Statistics.** `vcg::Histogram` as it is, with TexMetro's bin counts (500,000 for QCD,
   20,000,000 for `sf`). That is no new code and it **reproduces the published
   percentiles exactly**, at the cost TexMetro already pays: two arrays of that length, so
   about 320 MB transient for `sf`. The alternative, exact percentiles from a sorted array
   of (value, weight) pairs, is 48 MB for 3 M faces and not binned, but it is new code and
   it differs from the published values by up to a bin (5 × 10⁻⁶ for `sf`, 2 × 10⁻⁴ for
   QCD). The histogram was chosen (2026-10-23); the alternative is not built, and stays in
   reserve for the day the memory matters.

Concurrency: none to begin with; a full pyramid is under a second. Charts, bands and
textures are independent if that ever changes.

### Where it connects to the rest of MeshLab

- **Seams as polylines, with a value per edge.** Edges now carry colour and a scalar
  ([Edge Support](edge_support.md), `IOM_EDGEQUALITY`). The seam discrepancy is a value
  per seam edge, so a later filter can create the UV-seam polyline and store each edge's
  discrepancy in it, to be coloured by the existing edge colorizers. Stage 4 did not build it: it
  is a *Create* filter, so it belongs in `filter_meshing` beside *Create Polyline from Selection
  Perimeter*, and it needs the UV layer builder and the texture reader that now sit in
  `filter_measure` (about 120 lines, to be moved into core, not written again). It is offered, not built.
- **The UV view** already fills triangles by quality, with the same colour map and range
  controls as the 3D view ([Rendering](../rendering.md), *UV fill*), so the face quality
  that *Compute UV Distortion* writes is visible on the UV layout without further work.
- **Texture images** come from `TextureAssociationUtils::loadAssociatedTextureImage`, as
  *Sample Texels* does, and a layer with several textures is handled per texture.

## Verification

Follows the three tiers of the testing plan.

1. **Descriptor conformance**: one row per new filter, in `test_filter_descriptors.cpp`;
   the new category is added to `filtercategories.{h,cpp}` and to the vocabulary table
   together.
2. **Smoke**: rows for the Information filters on the existing ladder. The texel and
   seam filters need texture images, so they are driven on the `textured` rung and
   recorded in `expectedRefusals()` for the rungs without one.
3. **Behaviour**, with exact expected values:
   - an isometric mapping has QCD = 1 and UV scaling = 1 on every face;
   - a uniform scale of the UVs leaves QCD unchanged and `sf` unchanged;
   - one UV triangle of known area covers a known texel count; two coincident triangles
     give `overwritten = n`, `lost = n`; a triangle flipped against its chart is counted
     once in `nfolds`;
   - two triangles sharing an edge cover every texel on it exactly once (the top-left
     rule), at every `S` from 4 to 16;
   - QCD agrees with an independent computation to 10⁻⁹;
   - two runs on the same input give identical output.
4. **Datasets**: if the two gargoyle atlases can be published they become a dataset
   (`tests/datasets/datasets.json`, with a licence, per [Test Data](../test_data.md)).
   Otherwise a small synthetic atlas with a known answer is committed.

## Staging

Stages 1 to 4 each end in one vcglib bump, with the MeshLab code that uses it.

| Stage | vcglib (four-part landing) | MeshLab | Needs |
|---|---|---|---|
| **0** | none | **Reference values.** Build TexMetro on Linux or Windows; run it on the two gargoyle atlases and on benchmark models; keep the JSON. Decide the tolerance between our rare counters and the GL ones (the measured 8-bit-vs-4-bit spread, about 0.1 % to 1 %, is the floor) and which quirks to keep. Settle the [open questions](#open-questions) with the authors. | no code |
| **1** | V1 (`QuasiConformalDistortion`, `UVScaling`); V2 (the counting flag) | QCD and UV scaling in *Compute UV Distortion*; the six missing mesh keys; the recursive Python conversion | nothing; it ships alone. **Implemented 2026-10-23** |
| **2** | V3 (charts, signed chart area, folds) | *Measure UV Mapping* (charts, folds, QCD percentiles through `vcg::Histogram`), and *Select Folded UV Faces*. `sf` and `occupancy` wait for the raster | Stage 1, and approval of the category. **Implemented 2026-10-23** (`sf` and `occupancy` still wait for the raster) |
| **3** | V4 (the rasterizer, banding) | *Measure Texel Coverage*, the full pyramid; `occupancy` and `sf` added to *Measure UV Mapping* | Stage 2, and Stage 0 for the `sf` outlier rule. **Implemented 2026-10-23**, the rule as TexMetro has it |
| **4** | V5 (seam discrepancy, templated on a texel functor) | *Measure Texture Seam Color Discrepancy*; optionally the seam polyline with per-edge values | Stage 2, edge support. **Implemented 2026-10-24**; the polyline is not (see below) |
| **5** *(optional)* | none | An overdraw and clash overlay in the UV view, on QRhi (B1), as a diagnostic that reports no numbers | Stage 3 |

The `Compute UV Distortion` change (Stage 1) is the cheapest slice of the proposal and
ships alone; it needs no raster. The line counts in the table of V1 to V5, about 230 in
vcglib, and a few hundred more for the adapters, descriptors and tests, are estimates. No
new plugin or dependency is needed.

## Open questions

For the authors of TexMetro, since several of its choices are undocumented
(see [Quirks](#quirks-found-in-texmetros-code); the numbers are those measured above):

1. **The clash rule (quirk 1).** Is checking only the diagonal neighbours intended?
   Bilinear sampling at a texel touches its four edge neighbours first. Checking all eight
   changes the published `texelClashes` by +3 to +4 %. Reproduce it (the default
   proposed), or fix it?
2. **`totalTexels_bilinear`.** It counts a 3 × 3 footprint, where bilinear sampling reads
   2 × 2. A deliberately conservative bound, or an approximation?
3. **Overwritten texels' chart id (quirk 3).** The CPU rasterizer must choose one. The
   last triangle in face order is proposed; any other choice changes which chart a
   clash is attributed to.
4. **UV recentring and wrapping (quirk 9).** Why a 10⁻³ UV-unit shift, and should UVs
   outside [0,1] be wrapped (as the sampler does) or clipped (as the raster does)? Models
   with a tiled texture are the case.
5. **Outlier rule (quirk 4).** Is it intended that `sf` depends on `occupancy`, and so on
   the texture resolution? Two textures of different size would give slightly different
   `sf` for the same UVs.
6. **Texel units (quirk 8).** QCD in texel space includes a non-square texture's
   anisotropy; in normalized UV space it does not. *Compute UV Distortion* works in
   normalized UVs today. Which should the new metric use, or both as an option?
7. **Oversized textures (quirk 10).** Return zeros silently, as now, or the real values?
   The CPU rasterizer has no such limit, so the latter would be a visible difference.
8. **The seam pairing (quirk 14).** Is comparing the i-th sample of each side's own edge
   intended? It pairs mirrored points and reads about 16 % high on both atlases. Stage 4
   keeps it as the default so that the numbers can be checked against the benchmark's, and
   offers *Same point* as an option. Which should be the default?
9. **TexMetro's own future.** Once V1 to V5 are in vcglib, should TexMetro be moved onto
   them? Its OpenGL, GLEW and its `-lGL` build would go, and it would run on a Mac.

For this repository:

10. **`Select Folded UV Faces`.** Stage 2 gave it `Selection/by Topology` and
   `Parametrization`, as *Select Vertex Texture Seams* has. Say if it should be otherwise.
11. **Key names.** For the mesh keys added to the existing measure filters, Stage 1
    used MeshLab-style names (`duplicate_faces`, `zero_area_faces`, `degenerate_faces`,
    `is_mesh_oriented`, `boundary_length`, `normalized_surface_area`,
    `normalized_boundary_length`), since they sit beside `faces_number` and
    `unreferenced_vertices` and TexMetro's `fndup` or `fnzero` would not. For the UV
    reports Stage 2 kept TexMetro's names (`num_charts`, `qcd_perc50`, …), as the
    proposal said: they preserve the benchmark's JSON, and they fit the existing suffixes
    (`_min`, `_avg`, `_stddev`, `_perc95`). The vocabulary's word for a chart is *UV island*,
    which the descriptors say next to the key. The raster and seam reports kept TexMetro's
    names too, with `seam_edges_number` added. Open for a veto.
12. **The gargoyle atlases** can be published as a test dataset?

## Non-goals

- Porting the TexMetro command line, or its OBJ, PLY and FBX importers; MeshLab's
  readers feed the filters.
- Any use of OpenGL or QRhi to compute a reported number. B1 is for drawing, in Stage 5.
- Algorithm code in the MeshLab plugin. The filters stay adapters; anything that needs no
  document, Qt or GPU is written into vcglib.
- Changing the meaning of the five existing *Compute UV Distortion* metrics. Expressing
  *Area distortion* as `|UVScaling − 1|` inside vcglib is a refactor that keeps its values.
- Metrics of the texture's *content* (sharpness, blur, exposure) or of the benchmark's
  other tools.
- Reproducing a GPU's rounding bit for bit; the target is agreement within the measured
  device spread.
- A benchmark site, a database, or collecting the results of a run.
