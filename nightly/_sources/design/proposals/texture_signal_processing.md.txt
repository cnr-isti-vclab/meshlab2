# Texture Signal Processing Filters

This document proposes which of the programs in Prada and Kazhdan's
[TextureSignalProcessing](https://github.com/mkazhdan/TextureSignalProcessing) (TSP) are
worth turning into MeshLab filters, and how to integrate them. It recommends doing so **in
process**, as Screened Poisson is, through TSP's header-only `GradientDomain` library, and
**not** as helper programs, as TextureConsolidation is. **Stage 1 implemented 2026-11-12** (the plugin
`filter_texture_signal_processing`, with *Repair Texture Seams* and *Dilate Texture across
Seams*; see its `UPSTREAM.md`); stages 2 and 3 are not. Every number below was measured for
this proposal.

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
2048² on the same 20k-face mesh). [Stage 0](#stage-0-memory-and-robustness) found where
it goes: about three quarters is the divergence operator, which these filters do not use.

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

## Stage 0: memory and robustness

Measured 2026-11-12. The method: an instrumented copy of TSP's `include/Src`, kept in the
scratchpad and placed ahead of the submodule's on the include path. It prints the process's
current and peak physical footprint (`task_info`, the figure `/usr/bin/time -l` reports)
between the steps of `GradientDomain`'s construction. AddressSanitizer builds were used for
the crashes.

### Where the memory goes

All of it is in `OperatorInitializer::Initialize`: the hierarchy before it peaks at 0.9 GB
of the 9.1 GB at 2048². Within it, the largest single step is
`InitializeBoundaryCoarseToFineBoundaryOneFormProlongation` (`EdgeIndexing.h`). At 2048² it
builds 58 million triplets over 4.5 million fine boundary edges and adds 3.6 GB. Building the
divergence matrix after it adds another 1.4 GB. Both, and the edge indexing before them,
exist **only for the divergence operator**. `GradientDomain` always builds that operator,
but `SetExteriorTexels`, `TextureDilation` and uniform smoothing never use it.
`OperatorInitializer` already has an overload without it, used by `LineIntegralConvolution`.

Switching `GradientDomain` to that overload, with `SetExteriorTexels` (images byte-identical
in every row):

| Atlas | Texture | Peak with divergence | Peak without | Time with / without |
| --- | --- | --- | --- | --- |
| xatlas gargoyle, 20k faces | 512² | 2.07 GB | 0.58 GB | — |
| same | 1024² | 4.28 GB | 1.15 GB | — |
| same | 2048² | 9.20 GB | 2.61 GB | — |
| `gargo3M_good.ply`, 3M faces, 1,961 charts (see below) | 4096² | **44.5 GB** | **15.8 GB** | 58 s / 23 s |

The memory follows the number of **fine boundary edges**: the edges of the quadratic
elements made by clipping mesh triangles to the texels a chart boundary crosses. These are
4.5 million for the 20k-face mesh at 2048², and 32.7 million for the 3M-face mesh at 4096².
That count is why memory tracks the image's side rather than its area. It also means memory
tracks how much chart boundary the atlas has, so a fragmented photogrammetry atlas costs far
more than xatlas's 300 charts.

Two smaller savings were tried and are not worth proposing. In `_Initialize` and the
one-form merge, per-chart and per-thread buffers are copied into one while the originals
stay alive. Releasing each one as it is copied keeps the order and the output, but lowers
the peak by only about 5% (9.20 → 8.75 GB, 2.61 → 2.45 GB at 2048²; 15.8 → 12.4 GB at
4096² without divergence, with the robustness fixes below).

**Conclusion:** the memory is inherent to TSP's boundary representation, not a leak. It can
be cut by about 3.5× by not building an operator these filters do not use, an upstream
change: a constructor flag on `GradientDomain`, with the current behaviour as the default.
With it, a 4096² texture on a 3M-face scan needs about 12–16 GB, which a desktop has but
a filter running inside MeshLab should not take silently. Stage 1 therefore keeps the in-process plan,
and the filter reports its estimate before it runs (see open question 1).

### TSP does not run on the real 3M-face atlas

`tests/local/COLOR_gargoyle/gargo3M_good.ply` with its 4096² texture crashes TSP before
memory becomes a question. That happens in the unmodified programs too, not only in the
instrumented copy. Under AddressSanitizer, one fault at a time:

1. **A chart-vertex indexing bug** (`AtlasCharts.inl`, `getCharts`). The map from atlas
   vertex to chart vertex is one array shared by all charts. A UV vertex used by two charts
   that touch only at that vertex gets, in the second chart, an index into the *first*
   chart's array. On this atlas, 167 vertices (249 corners) are shared that way. 129 of the
   corners read past the array, which is the crash (heap corruption, reported by the
   allocator in `InitializeGridChartsActiveNodes`). The other 120 silently take a vertex
   from the wrong chart. The xatlas atlas has none. **This is a bug independent of the
   input's validity.** The fix keeps a per-chart owner, with a map for the rare other
   charts. It is verified byte-identical on the xatlas atlas.
2. **Empty clipped polygons.** One clipping function reads index −1 of an emptied polygon:
   `ClipIndexedIntersectionPolygonToIndexedIntersectionEdge`, the sibling of the function
   fixed in mkazhdan/TextureSignalProcessing#19. Or, when the last of the triangle's
   edges empties it, `GetTriangulation` is handed a polygon with no vertices and reports
   that through `MK_ERROR_OUT`: the program **exits with status 0**, the failure mode the
   error hook above is for.

   Two inputs empty a polygon:
   - **The 30,085 zero-area UV triangles** (1%). They have no inside. With them still in,
     the run faults later, in `IndexConverter::combinedToBoundary`. Not fixed.
   - **A valid triangle that only touches a boundary cell.** Upstream's rasterizer reports
     every cell the closed triangle meets, but the clip keeps only what is strictly inside.
     So an edge through a texel centre, which float UVs can hit exactly, leaves nothing.
     Even with the zero-area triangles removed, exactly one triangle on this atlas does
     this, so stage 0's runs completed only because they carried the guards. A square
     split along its diagonal reproduces it at 64². This is upstream's bug whatever the
     input, and a pull request skips the empty cell at both call sites, as TSP's
     quadrature code already does.
3. **Flipped UV triangles** (1,211 on this atlas). These fault in `_InitializeChart`
   (`Operators.inl:1132`). TSP's README requires a valid mapping, and the programs'
   `--sanityCheck` exists to test for one.

The patches for 1 and 2 are kept in `.reference/texture-consolidation-patches/tsp-stage0/`.
They are not upstream. The 4096² figures above are on this atlas with its zero-area and
flipped faces removed (2,968,618 faces left), using the fix for 1.

What this means for the filters:

- **Fix 1 and the touching-triangle half of fix 2 belong upstream**, and both pull
  requests were prepared on 2026-11-12 (files in
  `.reference/texture-consolidation-patches/tsp-chart-vertex/` and `tsp-clip-graze/`).
  Fix 1 is #20. An earlier PR, #17, fixes the same bug by overwriting the vertex's owner.
  On this atlas that adds 89 vertices a second time to a chart that already had them,
  and the texture differs from #20's at 228 texels, by up to 54/255.
- **For the other two, the adapter should keep zero-area UV triangles out of what it
  hands TSP.** They cover no texels, so leaving them out changes nothing in texel space.
  Upstream may prefer a fix, which can follow.
- **Flipped triangles are a different matter.** They do cover texels. Left out, those
  texels would count as outside every chart, and *Repair Texture Seams* would overwrite
  them. The filter should refuse an atlas with flipped or overlapping faces and point to
  *Select Folded UV Faces* and *Select Overlapping UV Faces*, as the padding rule does for
  shared texels.

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
functors and arrays. The memory finding was the one thing that could reverse this
choice. [Stage 0](#stage-0-memory-and-robustness) showed it is inherent, and about 3.5×
smaller without the divergence operator. A helper process would not reduce it; it would
only keep it out of MeshLab's address space. So the choice stands.

### Where the sources come from

| Option | How | Cost |
| --- | --- | --- |
| **A. Reuse the existing checkouts** | Include from `external/texture-consolidation/extern/{TextureSignalProcessing,Misha}` | No new submodule. TSP's version moves only when TextureConsolidation's pin moves. Today that pin is upstream's own master, so the coupling is in *when* it moves, not *what* is used |
| B. Separate top-level submodules | `external/TextureSignalProcessing`, `external/Misha` | Independent pins, but a second checkout of the same two repositories |
| C. Vendor, as `filter_screened_poisson/Src` | Copy the 69 headers (34k lines) into the plugin | No submodule, but a copy that does not track upstream, the opposite of what the upstream pull requests were for |

**Recommendation: A.** It is the least new code and tracks upstream. If the two plugins
ever need different versions, B can be adopted then.

**Decided 2026-11-12: B for TSP, A for Misha.** The plugin needs four upstream pull requests
(#20 to #23) that are not merged yet. TextureConsolidation's pin can't carry them: its own
pull request to hliuw is opened from the fork's `main`, so moving its TSP gitlink there
would change that pull request. So TSP becomes a top-level submodule,
`external/TextureSignalProcessing`, on `cignoni/TextureSignalProcessing` at a branch merging
the four. It goes back to `mkazhdan` once they are merged. Misha needs no fix, so it still
comes from `external/texture-consolidation/extern/Misha`.

**The Stanford PLY notice (open question 3), decided the same day.** In process, it was a
licence conflict, not a packaging question. `SimpleTriangleMesh.h` included Misha's PLY
reader, and `Ply.inl`'s namespace-scope `PLY::EdgeProps` array was emitted into any object
including `GradientDomain.h`, even at `-O2`. That would have linked "do not sell" code into
GPLv3 MeshLab. #23 moves the three mesh I/O members into `MeshIO.h`, which only the programs
include. After it, an adapter-like file includes no PLY header and emits no PLY symbol.

## Staging

0. **Memory.** Done 2026-11-12; see [Stage 0](#stage-0-memory-and-robustness).
   Left from it are three upstream pull requests: the chart-vertex fix (#20), the
   touching-triangle clip (#21), and a `GradientDomain` flag, `computeDivergence`, to skip
   the divergence operator. With the flag, `SetExteriorTexels` peaks at 15.6 GB instead of
   44.5 GB on the 3M-face atlas at 4096², with byte-identical output.
1. **Plugin and adapter, two filters. Implemented 2026-11-12**, with these measured
   differences from the plan: TSP is its own submodule (above); the adapter pads only atlases
   that come within two texels of the border, since a margin changes the result where texel
   centers lie on an edge; it never writes a texel another chart covers, which
   `SetExteriorTexels` can; it fuses texture coordinates as TSP's reader does; and when the
   exterior system is singular (every island of a per-triangle layout) it adds a pull of
   relative weight 1e-10 towards each texel's value. Overlapping atlases are not refused, since
   they do not fault. In the application, on the 3M-face scan at 4096², the repair takes 20 s and
   17 GB, the dilation 7 s and 5 GB. Open: the exterior texels a triangle only grazes are barely
   determined, and about 0.15% of them come out far out of range, clamped to white or black dots
   along the chart boundaries; `SetExteriorTexels` does the same. The adapter keeps zero-area UV triangles out of
   TSP's input, and refuses atlases with flipped or overlapping faces. *Repair Texture Seams* (`SetExteriorTexels`) and
   *Dilate Texture across Seams* (`TextureDilation`). Decided 2026-11-12:
   - both names, the subcategory `Texture/Filtering`, and widening `Dilate` to images;
   - memory is documented in the descriptions, and the filters just run (open question 1);
   - the dilation samples the input image and writes a separate output. `TextureDilation`'s
     own `main` samples the image it is writing, which makes its output differ from run to
     run when threaded. Sampling a copy makes it repeatable.

   Tests:
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

1. **Memory in the filter.** Stage 0 explained the memory: it follows the fine boundary
   edge count. The filter should still say what it is about to take. Two options: estimate it
   from a cheap pass over the boundary cells and ask before running, or document the figures
   and run. Which one is the user's call.
2. **The remaining zero-area fault** (`combinedToBoundary`), for an upstream fix rather than
   the adapter's workaround, if mkazhdan prefers one.
3. **The Stanford PLY notice.** Moving it from the helpers into the application makes the
   existing open item in `UPSTREAM.md` more pressing. Alternatively, the adapter could
   avoid `PlyFile.h`, if TSP's headers can be included without it. That has not been
   tried.
4. **Reaction-diffusion's reaction term** (stage 3): keep the quadrature
   (`ScalarIntegrator`, lower-level API) or approximate it.
5. **Multigrid or direct.** At 512² the direct solve is faster than enough V-cycles to
   match it (2.5 s against 7.5 s for 100). Whether this holds at 2048² and above has not
   been measured.
