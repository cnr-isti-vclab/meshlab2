# Texture Signal Processing Filters

This document proposes which of the programs in Prada and Kazhdan's
[TextureSignalProcessing](https://github.com/mkazhdan/TextureSignalProcessing) (TSP) are
worth turning into MeshLab filters, and how to integrate them. It recommends doing so **in
process**, as Screened Poisson is, through TSP's header-only `GradientDomain` library, and
**not** as helper programs, as TextureConsolidation is. **Not implemented.** Every number
below was measured for this proposal. None of the code has been committed.

See also: [TextureConsolidation history](../history/texture_consolidation.md),
`plugins/filter_texture_consolidation/UPSTREAM.md`, `plugins/filter_screened_poisson/UPSTREAM.md`,
[Adding a Filter](../adding_a_filter.md), [Vocabulary](../vocabulary.md),
[Filter Organization](../filter_organization.md), [TexMetro](texmetro.md).

## What TSP is

TSP solves gradient-domain problems **on the texels of a texture atlas**. It builds mass
and stiffness matrices over the active texels and couples texels across chart seams
through the surface, so a filtered signal is continuous on the surface even where it
jumps in the image. This is the method of [Prada, Kazhdan, Chuang and Hoppe, SIGGRAPH
2018](http://www.cs.jhu.edu/~misha/MyPapers/SIG18.pdf). It is MIT-licensed and builds on
Kazhdan's `Misha` utility library (BSD-3-Clause). MeshLab already builds both, as
submodules of `external/texture-consolidation`, at upstream commits that include our
fixes (mkazhdan/TextureSignalProcessing#19, mkazhdan/Misha#8).

The repository has nine programs, plus a header-only library,
`include/Src/GradientDomain.h`. The library's constructor takes **functors**: corner to
surface vertex, vertex to position, corner to texture vertex, texture vertex to UV, and
the image size. In return it gives the active texels, the texel graph's edges and whether
an edge crosses a chart, and the mass, stiffness and divergence operators, both as Eigen
matrices and as direct operators on a texel array. `HierarchicalGradientDomain` adds a
multigrid solver.

## Evidence

All on 2026-11-12, on this Mac (16 cores: 12 performance, 4 efficiency), Release builds,
Apple clang, OpenMP from Homebrew's libomp. The probes and builds were kept in the session
scratchpad, not committed. The test mesh is `tests/sample_mesh/gargoyle_small/gargoyle.ply`
(20k faces) parametrized with *Parametrize by Atlas (xatlas)*. The real texture is
1024², made from the fixture's four photos by *Transfer Color from Visible Rasters to
Texture*.

### Building

| What | Compile time | Peak compiler memory | Notes |
| --- | --- | --- | --- |
| `GradientDomain.example.cpp` | 14 s | 0.9 GB | the library's own direct-solver example |
| `HierarchicalGradientDomain.example.cpp` | 16 s | 1.0 GB | the multigrid example |
| `SetExteriorTexels.cpp`, `SeamStitcher.cpp` | 14 s each | — | both use the library |
| `TextureDilation.cpp` | 5 s | — | uses `Misha/Texels.h`, not the library |
| `ReactionDiffusion.cpp` | 35 s | — | with its GLUT viewer; it has no switch to build without one |

- **Eigen.** MeshLab compiles against `vcglib/eigenlib`, Eigen 5.0.1, and TSP vendors
  Eigen 3.3.7. Both library examples compile against Eigen 5.0.1, and **their output
  images are byte-identical** to the builds with TSP's own Eigen. One Eigen in the
  binary is therefore possible, and it has to be one: two versions of Eigen's templates
  in one process would break C++'s one-definition rule.
- **Misha.** TSP carries its own copy of Misha under `third_party/include/Misha`, and **it
  does not compile with Apple clang** (`Polynomial.h`: "declaration of 'Degree' shadows
  template parameter"). The Misha repository does compile. TSP therefore needs both
  repositories, as TextureConsolidation already arranges.
- **Names.** Misha's code is in `namespace MishaK`, and no plugin in the application uses
  it today. Screened Poisson's vendored copy is in `namespace PoissonRecon`. The two can
  link into one binary.
- **What an adapter would include.** A file including only `Src/PreProcessing.h`,
  `Src/GradientDomain.h` and `Misha/Texels.h` pulls in **28 TSP headers (11,874
  lines) and 41 Misha headers (22,121 lines)**, nothing from `third_party/`. Among them
  is Misha's `PlyFile.h`, which carries the Stanford PLY notice. Its no-sale clause is
  already an open item in `UPSTREAM.md`, and in process it would be in the application
  itself, not only in a helper.

### Running

| Program | Input | Time | Peak memory | Result |
| --- | --- | --- | --- | --- |
| `SetExteriorTexels` | photo texture, 1024² | 2.8 s | 4.5 GB | 61,916 texels changed |
| `SetExteriorTexels` | same texture, rescaled to 512² / 2048² | 1.6 s / 5.6 s | 2.1 / 9.1 GB | — |
| `SeamStitcher` (default) | photo texture, 1024² | 2.7 s | 4.5 GB | **no texels changed** (0 degrees of freedom) |
| `SeamStitcher --exterior` | photo texture, 1024² | > 10 min, one core | 3.5 GB | stopped by hand |
| `TextureDilation --radius 8` | photo texture, 1024² | 0.64 s | 0.23 GB | 421,800 gutter texels rewritten |
| Library example, direct solver (smoothing) | stripes, 512² | 2.5 s | 2.1 GB | — |
| Library example, multigrid, 5 / 20 / 100 V-cycles | stripes, 512² | 2.1 / 3.0 / 7.5 s | 2.1 GB | 36 / 16 / 0.3 levels (of 255) from the direct solve |
| `ReactionDiffusion`, stripes, 1000 steps | 512² / 1024² | 44 s / 88 s | 1.3 / 2.5 GB | continuous across seams (below) |

**Memory is the main cost, and it is all in setup.** In `SetExteriorTexels`, building the
`GradientDomain` takes the time and the memory. Factoring and solving take under 0.05 s.
Memory grows with the image's side, not its area (2.1, 4.5, 9.1 GB for 512², 1024²,
2048² on the same 20k-face mesh). Extrapolating, a 4096² texture would need about 18 GB.
That figure is extrapolated, not measured. Where the memory goes is not yet known, and
it should be found before any filter ships (see [Open questions](#open-questions)). It
may be an upstream fix like the ones already merged.

**Seams.** Two checks:

- **Reaction-diffusion.** Sampling the 512² stripe texture on both sides of all 3,515
  seam edges gives a mean difference of 0.004, against 0.075 for one texel's step
  inside a chart. Rendered on the mesh, no seam shows.
- **Real texture.** *Measure Texture Seam Color Discrepancy* on the photo texture drops
  from a mean of 26.4 (max 164) to 20.5 (max 120) after `SetExteriorTexels`. What remains
  is real color disagreement between the photos on the two sides of a seam, which this
  program does not touch by design. It only makes bilinear lookups agree.

### Two preconditions of the library

1. **No chart may come near the image border.** On the xatlas atlas, whose charts come
   within 0.4 texel of the edge, the library throws "Could not factor node index"
   (`BoundaryQuadraticElements.inl`, `GetBoundaryPolygons`). With the UVs pulled 4 texels
   in from the edge, it runs. The *programs* avoid this by enlarging the grid with
   `Src/Padding.h` and cropping afterwards. The adapter has to do the same, rather than
   change the user's UVs.
2. **Some errors call `exit()`.** Misha's `MK_ERROR_OUT` prints a message and calls
   `exit(0)`. Three library headers use it: `SimpleTriangleMesh.inl`,
   `HierarchyConstruction.inl` and `MultiIndex.h`. In process, that would close MeshLab.
   The fix is the one-line redefinition TextureConsolidation's wrapper already uses,
   `#define MK_ERROR_OUT( ... ) MK_THROW( __VA_ARGS__ )` placed before anything
   includes Misha. It turns the exit into a `MishaK::Exception` for the filter to catch.

### Padding is a separate, user-facing condition

xatlas's default `padding` is 0, so at 512² neighbouring charts can share texels. With
the default, 148 of the 3,515 seam edges disagree by more than 0.1; with `padding 2`, 2
do. Any texel-space filter should refuse such an atlas, or warn about it. *Select
Overlapping UV Faces* already finds the faces involved.

## Which programs are worth a filter

Overlap with an existing filter is context, not an objection (see `AGENTS.md`). The
proposed names are drafts. Four of them need words the vocabulary does not have yet; see
[Naming](#naming).

| TSP program | What it does | In MeshLab today | Proposed filter | Verdict |
| --- | --- | --- | --- | --- |
| `SetExteriorTexels` (203 lines) | Sets the texels just outside each chart to the values that minimize the Dirichlet energy, given the covered ones, so bilinear lookups agree across seams | Nothing. *Measure Texture Seam Color Discrepancy* only measures the problem | *Repair Texture Seams* | **Stage 1.** Small, fast, useful on any atlas, measurable with a filter we ship |
| `TextureDilation` (155 lines) | Fills gutter texels up to a radius by following the surface across the seam and sampling the neighbouring chart | The `pullpush` option of the transfer filters, which fills in image space with a blur | *Dilate Texture across Seams* | **Stage 1.** Does not need `GradientDomain`: 5 s to compile, 0.23 GB to run |
| `TextureFiltering` (1,184 lines; the library example does the same in 120) | Smooths or sharpens a texture with a screened Poisson solve, across seams | Nothing for textures; *Smooth Vertex Color* and the unsharp-mask filters work per vertex | *Smooth Texture*, *Sharpen Texture* | **Stage 2.** Per-edge gradient scaling is computed by the caller, so a face selection could drive a localized version, which the program does only through its interactive viewer |
| `TextureStitching` (1,520 lines) | Blends several partial textures, or one composite with a source-id mask, in the gradient domain | Ships inside *Transfer Color from Visible Rasters to Texture (Texture Consolidation)*, as a helper | An option of the stage-2 smoothing: keep gradients only between texels from the same source | **Stage 3**, only if *Transfer Color from Visible Rasters to Texture* is changed to output, per texel, which raster its color came from |
| `LineIntegralConvolution` (1,025 lines) | Draws a vector field as a streak texture by anisotropic diffusion along it | Field filters exist (principal curvature directions, two backends; the [Directional](directional_port.md) proposal), but nothing draws a field into a texture | *Render Vector Field to Texture (LIC)*, name to be decided | **Stage 3.** Uses the per-triangle metric form of the constructor, which the library supports |
| `ReactionDiffusion` (918 lines) | Gray–Scott stripes or dots grown in texture space, optionally aligned to a vector field | Nothing | Name to be decided | **Stage 3.** Its reaction term is integrated with `ScalarIntegrator`, which is not part of `GradientDomain.h`. Either the adapter uses the lower-level API, or the term is approximated by applying the mass matrix to a per-texel value, which is a change in the method and needs Paolo's ruling |
| `Geodesics` (816 lines) | Heat-method distance from a clicked point, at texel resolution | Five geodesic filters (vcglib, libigl, heat and exact), per vertex | — | **No.** The only new thing would be texel resolution |
| `SeamStitcher` (260 lines) | Re-solves the texels at chart crossings, or the exterior texels, or masked ones, to minimize the Dirichlet energy | — | — | **No.** On a padded atlas its default mode has nothing to do: 0 degrees of freedom, output identical. `--exterior` overlaps `SetExteriorTexels` and ran for over 10 minutes. The mask mode is the stage-2 solve with a different right-hand side |
| `TextureMasking` (320 lines) | Draws active, boundary and gutter texels and triangle IDs as an image | *Measure Texel Coverage*, *Select Overlapping UV Faces* ([TexMetro](texmetro.md)) | — | **No.** Diagnostic, and covered |

## Integration: in process, as Screened Poisson is

TextureConsolidation runs as helper programs because its programs are whole command-line
pipelines (back-projection, optical flow, stitching) that read and write files. TSP's
library is a different case. It is built for being called, and it is called with
functors that a `CMeshO` with wedge UVs answers directly.

| | Helper programs (as TextureConsolidation) | In process (as Screened Poisson) |
| --- | --- | --- |
| Data | Write a PLY and a PNG, run a program, read a PNG back | Functors over `CMeshO` wedge UVs, the texture as a `QImage`; the result returned through `textureoutputref` |
| Per filter | One executable each, plus its install, signing, packaging and helper-cache entry. `ReactionDiffusion`, `LineIntegralConvolution` and `Geodesics` need a patch even to build without their GLUT viewer | One adapter file with a `filters.json` entry |
| Compile | Not part of the main build; cached by `.github/actions/helper-cache` | About 15 s per adapter file, about 1 GB of compiler memory. For comparison, before its fix the stitcher alone took 68 minutes on the macOS runner |
| Memory (GB, measured above) | In the helper's process, freed when it exits | In MeshLab's process. Freed when the `GradientDomain` is destroyed, but the peak is shared with whatever the document holds |
| Errors | Exit status, plus the existing `MK_ERROR_OUT` wrapper | The same `#define`, then `catch (const MishaK::Exception &)` |
| Cancel | Kill the process | Not possible during setup and solve. Acceptable for the stage-1 and stage-2 filters (seconds); reaction-diffusion's step loop belongs to the adapter, so it can check for cancellation between steps |
| Threads | The program's own thread pool and OpenMP | The same, inside the app; OpenMP is already linked for Screened Poisson |

**Recommendation: in process**, in a new plugin `filter_texture_signal_processing`
(*Texture Signal Processing Filters*: one dependency, one plugin, as in
[Filter Organization](../filter_organization.md)). The adapter does three things: it
pads the grid, it wraps errors, and it converts between `CMeshO`/`QImage` and TSP's
functors and arrays. The memory finding is the one thing that could reverse this choice:
if 4096² really needs about 18 GB, a helper process would at least keep that out of
MeshLab's own memory. So the memory investigation comes before stage 1.

### Where the sources come from

| Option | How | Cost |
| --- | --- | --- |
| **A. Reuse the existing checkouts** | Include from `external/texture-consolidation/extern/{TextureSignalProcessing,Misha}` | No new submodule. TSP's version moves only when TextureConsolidation's pin moves. Today that pin is upstream's own master, so the coupling is in *when* it moves, not *what* is used |
| B. Separate top-level submodules | `external/TextureSignalProcessing`, `external/Misha` | Independent pins, but a second checkout of the same two repositories |
| C. Vendor, as `filter_screened_poisson/Src` | Copy the 69 headers (34k lines) into the plugin | No submodule, but a copy that does not track upstream, the opposite of what the upstream pull requests were for |

**Recommendation: A.** It is the least new code and tracks upstream. If the two plugins
ever need different versions, B can be adopted then.

## Staging

0. **Memory.** Find where `GradientDomain` construction spends 4.5 GB at 1024² on a
   20k-face mesh, and whether it is inherent. Measure 4096² on `tests/local/COLOR_gargoyle/
   gargo3M_good.ply` (3M faces, a real 4096² atlas). Upstream any fix. This decides
   whether the recommendation stands.
1. **Plugin and adapter, two filters.** *Repair Texture Seams* (`SetExteriorTexels`) and
   *Dilate Texture across Seams* (`TextureDilation`). Tests:
   - a smoke row for each;
   - a behavioural test asserting that *Measure Texture Seam Color Discrepancy* drops
     on the gargoyle photo texture;
   - the xatlas gargoyle at its default settings, whose charts touch the image border:
     the adapter pads, so the filter must run rather than throw.
2. ***Smooth Texture*, *Sharpen Texture***, with an optional face selection to localize
   them.
3. **Line-integral convolution, reaction-diffusion, source-aware stitching.** Each needs
   its own decision first: the vocabulary for the first two, Paolo's ruling on the
   reaction term, and a change to color projection for the third.

## Naming

[Vocabulary](../vocabulary.md) has no subcategory for processing an existing image.
`Texture` has `Assignment`, `Conversion` and `Packing`. Proposed:

- **A subcategory `Texture/Filtering`**, for filters that change a texture's texels from
  its own texels. All stage-1 and stage-2 filters go there.
- ***Repair Texture Seams***: `Repair` ("fix a defect while preserving intent") fits as
  defined.
- ***Smooth Texture*, *Sharpen Texture***: `Sharpen` is admitted for attributes;
  `Smooth` is defined for positions and normals but already used for colors (*Smooth
  Vertex Color*). The vocabulary should say so.
- ***Dilate Texture across Seams***: `Dilate` is admitted only for selections. Widening
  it to images keeps its standard morphological meaning. The alternative, `Fill`, is
  rejected in the lexicon as a synonym of `Close`.
- **Line-integral convolution and reaction-diffusion** produce a new texture from no
  input texture. `Create` is reserved for layers, and `Render` means an image of the
  scene. To be decided with the stage-3 filters.

## Open questions

1. **Memory** (stage 0). The cause of the 2.1/4.5/9.1 GB, and whether Misha's thread
   pool, quadrature buffers or the boundary polygons account for it.
2. **The Stanford PLY notice.** Moving it from the helpers into the application makes the
   existing open item in `UPSTREAM.md` more pressing. Alternatively, the adapter could
   avoid `PlyFile.h`, if TSP's headers can be included without it. That has not been
   tried.
3. **Reaction-diffusion's reaction term** (stage 3): keep the quadrature
   (`ScalarIntegrator`, lower-level API) or approximate it.
4. **Multigrid or direct.** At 512² the direct solve is faster than enough V-cycles to
   match it (2.5 s against 7.5 s for 100). Whether this holds at 2048² and above has not
   been measured.
