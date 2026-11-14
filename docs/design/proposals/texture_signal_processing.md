# Texture Signal Processing Filters

This document proposes which of the programs in Prada and Kazhdan's
[TextureSignalProcessing](https://github.com/mkazhdan/TextureSignalProcessing) (TSP) are
worth turning into MeshLab filters, and how to integrate them. It recommends doing so **in
process**, as Screened Poisson is, through TSP's header-only `GradientDomain` library, and
**not** as helper programs, as TextureConsolidation is. **Stages 1 and 2 implemented 2026-11-12** (the
plugin `filter_texture_signal_processing`, with *Repair Texture Seams*, *Dilate Texture across
Seams*, *Smooth Texture* and *Sharpen Texture*; see its `UPSTREAM.md`); **stage 3 the same day**
(*Inpaint Texture on Selected Faces*, *Transfer Texture Detail* and object-space normal maps); **stage 4's
line-integral convolution and reaction-diffusion 2026-11-13**, with per-face principal curvature in
MeshLab's data model; its source-aware stitching is not. Every number below was measured for
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
| `TextureFiltering` (1,184 lines; the library example does the same in 120) | Smooths or sharpens a texture with a screened Poisson solve, across seams | Nothing for textures; *Smooth Vertex Color* and the unsharp-mask filters work per vertex | *Smooth Texture*, *Sharpen Texture* | **Stage 2.** Per-edge gradient scaling is computed by the caller, so a face selection could drive a localized version, which the program does only through its interactive viewer. **Stage 3** adds two of its options: `--inLow`, values from a second texture (*Transfer Texture Detail*), and `--colorAsNormal`, object-space normal maps |
| `TextureStitching` (1,520 lines) | Blends several partial textures, or one composite with a source-id mask, in the gradient domain | Ships inside *Transfer Color from Visible Rasters to Texture (Texture Consolidation)*, as a helper | An option of the stage-2 smoothing: keep gradients only between texels from the same source | **Stage 3**, only if *Transfer Color from Visible Rasters to Texture* is changed to output, per texel, which raster its color came from |
| `LineIntegralConvolution` (1,025 lines) | Draws a vector field as a streak texture by anisotropic diffusion along it | Field filters exist (principal curvature directions, two backends; the [Directional](directional_port.md) proposal), but nothing draws a field into a texture | *Render Vector Field to Texture (LIC)*, name to be decided | **Stage 3.** Uses the per-triangle metric form of the constructor, which the library supports |
| `ReactionDiffusion` (918 lines) | Gray–Scott stripes or dots grown in texture space, optionally aligned to a vector field | Nothing | *Colorize Texture by Reaction-Diffusion* | **Stage 4, implemented 2026-11-13.** Its reaction term is integrated with `ScalarIntegrator`, which is not part of `GradientDomain.h`; the adapter uses the lower-level API, as the program does (Paolo, 2026-11-13: keep the quadrature) |
| `Geodesics` (816 lines) | Heat-method distance from a clicked point, at texel resolution | Five geodesic filters (vcglib, libigl, heat and exact), per vertex | — | **No.** The only new thing would be texel resolution |
| `SeamStitcher` (260 lines) | Re-solves the texels at chart crossings, or the exterior texels, or masked ones, to minimize the Dirichlet energy | — | — | **Mask mode: stage 3**, as *Inpaint Texture on Selected Faces*, with the selection as the mask. It frees the masked texels and holds the rest, which is the stage-1 repair's solve with a different set of free texels. The other two modes, no: on a padded atlas the default mode has nothing to do (0 degrees of freedom, output identical), and `--exterior` overlaps `SetExteriorTexels` and ran for over 10 minutes |
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
   `SetExteriorTexels` can; it fuses texture coordinates as TSP's reader does; and it pulls
   each exterior texel towards its current value with a weight of 1e-2 of the median coupling
   (decided 2026-11-12). Without it, the exterior texels a triangle only grazes are barely
   determined and come out far out of range, white and black dots along the chart boundaries,
   as `SetExteriorTexels` gives them; with it the dots drop from 22,220 to 2,628 on the 3M-face
   scan, the seam discrepancy on the gargoyle test is slightly lower (28.03 against 28.26), and
   a per-triangle layout no longer makes the system singular. Overlapping atlases are not
   refused, since they do not fault. In the application, on the 3M-face scan at 4096², the
   repair takes 20 s and 17 GB, the dilation 7 s and 5 GB. The adapter keeps zero-area UV triangles out of
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
   them. **Implemented 2026-11-12.** Decided the same day: the two parameters are TSP's own
   (*Fidelity*, its interpolation weight, default 1000; *Gradient Scale*, its modulation, 0 to
   smooth and 2 to sharpen); the selection builds the divergence operator only when used; the
   solver is multigrid until converged (open question 5: the direct solver took 63 s and 5.7 GB to
   factor at 2048²). On the gargoyle test, smoothing takes the seam discrepancy from 33.9 to 2.9.
   On the 3M-face scan at 4096²: 3 minutes and 19 GB, 4 minutes and 47 GB with a selection. Found
   on the way: TSP jittered with `rand()`, so in the application every run differed from the last;
   #24 gives the jitter a generator of its own.
3. **Inpainting, detail transfer and normal maps.** Proposed 2026-11-12; Paolo approved them
   and their names the same day; **implemented the same day**, with the measured differences from
   this plan listed after the three items. Three uses of what stages 1 and 2
   built, each a few dozen lines in the adapter:
   - ***Inpaint Texture on Selected Faces*** (`SeamStitcher --mask`). The texels whose nearest
     face is selected, by stage 2's rule, exterior ones included, are freed; every other texel
     is held. The freed texels take the values that minimize the Dirichlet energy on the
     surface, which is the repair's solve, $(P^\top S P)\,y = -P^\top S\,x$, with a different $P$. The
     fill is harmonic: smooth, continuous across seams, and meeting the surrounding colors at
     the edge of the selection. That is right for markers, stains and small projection errors,
     and blurry over large regions. The repair's pull towards the current values cannot be
     reused: at 1e-2 of the median coupling it reaches about ten texels, so a stain would show
     through. The system is only regularized, at 1e-8, so that a region with no held texel
     around it (a whole connected piece of the surface selected) keeps its mean. The solve is
     direct, like the repair's; its cost on a large selection is open question 6.
   - ***Transfer Texture Detail*** (`TextureFiltering --inLow`). Two textures of one mesh,
     both read through the UVs of the first. The result keeps the gradients of the *Detail
     Texture*, scaled by *Gradient Scale* (default 1), while its values are held to the
     *Base Texture* with weight *Fidelity*: $(w\,M + S)\,x = w\,M\,x_b + s\,S\,x_d$. For example, a sharp
     texture from one capture and a color-correct but soft one from another. The result
     replaces the base texture's image, the one the faces show. A detail texture of another size
     is resampled to the base's size (the program refuses it). No selection: what scaling the gradients
     outside it should mean has no obvious answer when they come from another texture.
   - **Object-space normal maps** (`TextureFiltering --colorAsNormal`): an option on the
     smoothing, sharpening, detail-transfer and inpainting filters. The colors are decoded as
     vectors ($2c-1$), solved for, renormalized and encoded again. Object-space maps only:
     a tangent-space map's frame changes across a seam, so treating its two sides as one
     signal mixes frames. The help says so.

   Tests: inpainting a patch overwritten with noise on the gargoyle leaves every unselected
   texel unchanged and comes back close to the original; detail transfer with the same
   texture as base and detail at scale 1 returns the input, and from a smoothed base restores
   most of the original roughness; a normal-map run gives unit-length normals.

   Measured while implementing, and changed from the plan:
   - **The inpainting's pull.** Regularizing the region alone, as planned, sent the exterior
     texels the energy barely reaches up to 4,300 levels out of range; the repair's pull on all
     of them added up along the seams of a large region and held it back towards what was there
     (15,022 of 34,090 texels of a region painted magenta stayed near magenta). Only the texels
     coupled less than 1e-3 of the median take the pull: 89 stay near magenta, the error against
     the photo is 27.8 levels against 26.4 with no pull at all, and 31 values come out of range
     on the 3M-face scan, against 45.
   - **The gutter around the region is freed**, and not written: held, its background bled into
     the fill as a boundary value.
   - **Alpha is left alone** by the four filters that solve for colors, as in the programs, which
     read color only. This fixes stage 2 as well: solved as a fourth channel, the transparent gutter
     of a projected texture left 162,929 of its 162,932 opaque texels partly transparent after
     smoothing.
   - **Which face is nearest** to a texel now comes from Misha's candidate lists, whose ties are
     broken by index. The other path keeps whichever supporting triangle a thread wrote last, so
     the edge of a selection moved from run to run; this fixes stage 2's localized filtering too.
   - On the gargoyle test, inpainting a region of 34,090 texels painted magenta brings it back to
     27.8 levels rms of the photo (157.6 painted); detail transfer from a smoothed base brings the
     roughness from 5.08 back to 14.28 (the photo's is 14.17), and the error against the photo
     from 31.2 to 5.6.
4. **Line-integral convolution, reaction-diffusion, source-aware stitching.** Line-integral
   convolution first, planned 2026-11-13 below; reaction-diffusion the same day, after it
   (below). The stitching still needs a change to color projection.

   **What `LineIntegralConvolution` does.** It takes a direction per triangle, by default the
   maximal (or, with `--minimal`, the minimal) principal curvature direction scaled by
   k_max − k_min, computed from vertex normals it first smooths twice. It normalizes the field
   to unit root-mean-square length and makes the metric anisotropic along it
   (`InitializeAnisotropicMetric`, the orthogonal direction weighted by |v|^aExp · 1e5, aExp 0 by
   default). Random-hue noise, one color per texel, is diffused under that metric,
   (w M_a + S_a) x = w M_a n with w = 1e4, which smears it into streaks along the field, and the
   streaks are then sharpened by the stage-2 solve with fidelity 1e4 and gradient scale 100. The
   library's constructor takes a 2×2 metric per triangle in place of positions, so the adapter
   builds the anisotropic system without the lower-level interface. Two systems are built,
   anisotropic and isotropic (the measured cost is below).

   **Implemented 2026-11-13**, as planned below, with these measured differences:
   - **The streaks are solved directly.** Multigrid does not converge under the 1e5 anisotropy: on
     the 20k-face gargoyle atlas at 1024², 300 V-cycles left texels up to 862 levels from the direct
     solution, still changing by 9 per cycle. The direct solve was faster too: 14 s and 3 GB in all
     at 1024², against 49 s; 85 s and 8 GB at 2048². The anisotropic operators are released before
     the sharpening builds its own, so the two never coexist.
   - **vcglib's conversion returns the two directions in either order** (two thirds of the vertices
     of an ellipsoid came back with the labels swapped), while carrying each curvature with its own
     direction (2% off after vertex → face → vertex). So the transfer filters put each pair back
     with the maximal curvature first, MeshLab's convention, and the line-integral convolution takes
     the direction of the larger curvature, not `PD1`. The vcglib change factors the magnitude
     matching out of `SetFaceCrossVectorFromVert` into `CrossMagnitudes`, uses it in
     `SetVertCrossVectorFromFace`, and makes that skip a vertex with no face, which it used to read
     past the end of an empty list for.
   - **The streaks' length is a length.** The program's weight w (default 1e4) has no unit of
     its own: the surface is scaled to unit area, and the anisotropic metric, rescaled to unit area
     too, shortens lengths along the field by (1 + 1e5)^(1/4) ≈ 17.8, so the noise is averaged
     along the field over a decay length l = 17.8 √A / √w. The filter asks instead for a *Streak
     Length* L in the mesh's units (`absperc`, half the bounding box diagonal by default), the
     whole width at which the correlation along a streak falls to half, 2 · 1.678 l, and sets
     w = A √(1 + 1e5) (3.357 / L)². Measured on a cylinder whose field runs around it: half the
     correlation at 1.70 l (w = 1e4) and 1.88 l (w = 1e5) against 1.678 in theory, and streaks of
     1.07 and 2.10 for 1 and 2 asked; the default sharpening shortens them by about a fifth
     (1.30 l and 1.38 l). Across the field they are a texel wide. The program's default is a
     length of 0.60 √A, and √A is close to the bounding box diagonal on a compact mesh (212
     against 198 on the gargoyle), hence the default. Asked by Paolo, 2026-11-13, with a single
     `textureSize` ([vocabulary](../vocabulary.md#texture-size-2026-11-13)) and a choice of
     noise.
   - On an ellipsoid three times as long as wide, TSP's per-face curvatures come out 1.001 and 0.120
     around the middle (exactly 1 and 1/9; the normal smoothing rounds the small one up), with the
     minimal direction along the axis; through the vertices and back they move by 0.3%.

   **Decided 2026-11-13** (Paolo): the filter takes its directions from the mesh, not from a
   computation of its own; MeshLab gains **per-face** principal curvature directions, with
   conversions both ways; TSP's per-face curvature becomes a curvature method of its own; the
   result goes into an existing texture or a new one; the name is *Colorize Texture by Principal
   Directions (Line Integral Convolution)*. In order:

   a. **Per-face curvature directions in the data model.** vcglib already has the component,
      `vcg::face::CurvatureDirfOcf` (PD1, PD2, K1, K2), and memory accounting already counts its
      vector (`vcgFaceOcfBytes`). `VCGFace` gains it as a storable OCF component, like the vertex
      one: `deepCopyMesh` copies it, so undo keeps it; the layer panel lists it. Left for later:
      Python accessors, drawing it in the curvature decorator, and file formats (none carries it).
   b. **vcglib: the magnitudes from faces to vertices.** `CrossField<Mesh>` already converts
      both ways, treating the two directions as a cross field, which is right for principal
      directions: `SetFaceCrossVectorFromVert` carries the curvatures K1 and K2 with them, but
      `SetVertCrossVectorFromFace` sets only the directions. It is extended to carry them too,
      the same way, so that PD1 stays the direction of K1. Commit in vcglib, push, and gitlink
      bump by Paolo.
   c. ***Transfer Curvature Directions from Vertex to Face*** and ***from Face to Vertex***
      (`Transfer/Within a Mesh` and `Attribute/Curvature`, in `filter_colorproc` beside the color
      and scalar transfers), thin adapters over (b).
   d. ***Compute Principal Curvature Directions per Face (TextureSignalProcessing)***, in the
      TSP plugin: `InitializePrincipalCurvatureDirection`, the shape operator of each triangle
      from the differences of its vertex normals, after the program's normal smoothing (two
      iterations, interpolation 1e3, ported from the program, since the library does not have
      it). Stored per face, in the mesh's units: TSP computes on the mesh scaled to unit area.
   e. ***Colorize Texture by Principal Directions (Line Integral Convolution)***: the maximal or
      minimal direction, from the faces when they have directions, otherwise from the vertices
      through (b)'s conversion to faces, on a copy. Parameters: the program's streak and
      sharpening weights (the streaks' as a length, below) and its anisotropy exponent, the noise (the program's random hue, or
      black and white or random gray, the classic noises), and `randomSeed` for it. The output
      is always a new square texture of one side asked (`textureSize`, as most filters that
      make a texture take it), one per texture the faces use, appended and
      bound to the faces, as *Transfer Color from Vertex to Texture* does: there is no texture
      parameter at all. (A first version took a `textureoutputref` and a separate texture
      choosing the faces; Paolo found the two parameters confusing. The code that writes a
      `textureoutputref` result still moved from *Convert Object-Space Normal Map to Tangent
      Space* to `TextureAssociationUtils::storeOutputTexture`, and the slot helpers of that
      transfer moved there too.)

### Reaction-diffusion (implemented 2026-11-13)

**Decided 2026-11-13** (Paolo): keep the program's quadrature for the reaction term. The
adapter's `reactionDiffusion` therefore cannot use `GradientDomain`, which builds the mass and
stiffness but not the `ScalarIntegrator` of the same call, and builds the system the way the
program's `InitializeSystem` does: the hierarchy (the program's multigrid blocks, 128 × 16, padded
2 × 0), the metric, the surface's or anisotropic along a field, scaled to an area of half the
number of nodes, the operators with 6 quadrature points per triangle and the integrator with 3,
exact. Each step is `UpdateApproximateSolution`: the mass of each concentration plus the
integrated reaction, one V-cycle of two sweeps per level for each, clamped at 0. The output is
the program's gray, 0.75 (1 − 2b) clamped, 204 off the nodes. What differs:

- **The seeds** are drawn by area from a `std::mt19937` seeded by `randomSeed`; the program
  draws them with `rand()` on several threads, so its own output changes from run to run.
- **Double precision.** The program solves in `float` unless given `--double`.
- **The field** is the mesh's principal directions, as for line-integral convolution, through
  the same conversion (`fieldInTriangleBases`, which LIC now shares), instead of a file.
- **No viewer**, and so no seed picked with the mouse; the run can be canceled between steps.

**Compared with the program** on the 20k-face gargoyle atlas at 512², 1000 steps, stripes
(statistics over the 202,214 node texels; the seeds differ, so the texels cannot):

| | Program | Filter |
|---|---|---|
| Mean gray | 112.8 | 112.1 |
| Share darker than 96 | 0.437 | 0.444 |
| 10th / 50th / 90th percentile | 42 / 114 / 182 | 42 / 112 / 182 |
| Stripe period (power-weighted, texels) | 11.22 (another run 11.23) | 11.22 |
| Time, peak memory | 38 s, 0.97 GB (float) | 51 s, 1.49 GB (double) |

Dots grow from a thousandth of the texels and have not covered the surface by step 1000: the
share left plain was 0.40 and 0.17 in two runs of the program, 0.30 for the filter.

As first ported, the pattern's width was a number of texels, about 11 for the stripes, because of
the program's metric scaling: a larger texture gave more stripes, not finer ones. `FilterTests::textureSignal-
ProcessingGrowsReactionDiffusion` holds the filter to the program's dark share, to the period (below,
since the *Pattern Size* replaced the program's *Diffusion Scale*), to the stripes lining up with a field (128 crossings around a cylinder against 3,664 along
it), and to the pattern running on across a seam. Writing that test showed that the test grid's
cylinder was not closed: its last column of vertices sat at `cos(2π)`, `sin(2π)` in float, never
merged with its first, so the cut was a slit (70 levels across it, against 5 once closed).

**Parameters**: *Texture Size* (512, the program's), *Pattern* (stripes or dots, the program's
two rate pairs and seed shares), *Direction* (none, maximal, minimal), *Anisotropy Scale* (1),
*Steps* (1000), *Pattern Size* (3% of the diagonal; below), and as advanced *Anisotropy Exponent*
(0), *Time Step* (10, the program's speed) and `randomSeed`.

### Reaction-diffusion's scale as a length (approved and applied 2026-11-14)

The filter's size parameter, *Diffusion Scale*, is the program's, and the pattern it gives is a
number of texels, not a length on the mesh: the program scales the surface to an area of half
the number of texel nodes. Measured on the flat 2π × 2π grid laid out over 0.9 of the texture,
stripes, 2000 steps, period from the power-weighted spectrum:

| Texture | Diffusion Scale | Period, texels | Period, mesh units |
|---|---|---|---|
| 128² | 1 | 12.66 | 0.691 |
| 128² | 4 | 26.33 | 1.436 |
| 256² | 1 | 12.87 | 0.351 |
| 256² | 4 | 26.53 | 0.723 |

So doubling *Texture Size* halves the pattern on the mesh, and the scale is in no unit a user
sees. The fix is the one the Detail Size made. Scaling the metric by s is the same as dividing
the diffusion by s (the steps are (s M + d D S) a' = s M a + d s R), so the adapter can fix the
diffusion at the program's and choose s = A (c / L)² for a period L on the surface, A its area:
c, the period in the metric, measured 9.11 and 9.18 at the two sizes above; the adapter takes 9.15
(`kPeriodInMetric`). On the gargoyle at
512² the program's default is a period of about 6.1 mesh units, 3.1% of the diagonal.

The seeds are a share of the texels too (1% for stripes, 0.1% for dots), so with the pattern a
length they would grow denser as the texture grows. They follow it: a number per period cell,
the program's at its default (about 1.2 for stripes and 0.12 for dots per L²), so that how
many steps fill the surface does not depend on the texture either. (The proposal said 1.2 and 0.12;
with c = 9.15 the program's density is 0.02 c², 1.7 and 0.17.)

**Applied, with one finding.** Each seed must also keep its size in the metric. The program seeds
single texels, each of area 1/2 in its metric; with the metric set by the Pattern Size, a texel of a
finer texture is smaller, and at 256² on the grid where 128² grew stripes, nothing grew: the seeds
diffused away before reacting. Each seed is now a disk of nodes / (2 s) texels, one texel when that
is less, which is the program's seed wherever its scaling and this one agree. Measured after the
change:

| Case | Pattern Size | Period measured |
|---|---|---|
| Flat grid, 128² | 0.7 | 0.691 (12.8 texels) |
| Flat grid, 256² | 0.7 | 0.689 (25.7 texels) |
| Gargoyle, 512², default 3% of the diagonal | 5.95 | 11.0 texels (program: 11.22); dark share 0.443 (0.437) |

`FilterTests::textureSignalProcessingGrowsReactionDiffusion` holds the period to the Pattern Size
within 10% at both sizes; the period is 4 / (π ρ), ρ the crossings between dark and light per unit
length, which agreed with the spectrum's within 1.5%.

| Current id · label · default | Target id · label · default | Evidence | Verdict |
|---|---|---|---|
| `diffusionScale` · `Diffusion Scale` · 1 | `patternSize` · `Pattern Size` · `@bboxDiag003` (absperc): the distance from one stripe or dot to the next | measured above; 3% is the program's default on the gargoyle at 512² | id, label, type |
| seeds: 1% / 0.1% of the texels | about 1.7 / 0.17 per L² of surface, each a disk of the program's seed area | the program's density and seed size at its default | no parameter |
| `textureSize` | unchanged; now only the resolution. The log says the period in texels, and warns below 6, where the grid no longer resolves the pattern | — | help |
| `anisotropyScale`, `anisotropyExponent`, `steps`, `timeStep`, `pattern`, `direction`, `randomSeed` | unchanged | dimensionless already: a ratio of diffusions, an exponent, counts and Gray–Scott's own time | none |

*Pattern Size* is in the vocabulary beside *Detail Size*: a period on the surface.

## Fidelity as a length (approved and applied 2026-11-13)

Every *Fidelity* the TSP filters take is the program's screening weight w, with no unit of its
own: the surface is scaled to area 1, so the same w acts at the same relative scale on any
mesh, and nobody can say what scale that is. The solve is (w M + S) x = w M x₀ + s S x₀ (a
detail texture in place of x₀ on the right for *Transfer Texture Detail*), so a pattern of
wavenumber k on the surface keeps (w + s k²) / (w + k²) of its amplitude: the input's below the
**crossover wavelength λ = 2π √(A / w)**, A the surface's area, and s times it above. It is the
one length that says what the weight does in every filter: details smaller than λ are the ones
smoothed away, sharpened or transferred.

**Measured** on a flat 2π × 2π grid laid out over 0.9 of a 512² texture, with sinusoids along
u, amplitude fitted over the middle half (the gain, measured against theory):

| Filter (s) | w | λ / 2 | λ | 2 λ |
|---|---|---|---|---|
| Smooth (0) | 1e3 | 0.202 / 0.200 | 0.477 / 0.500 | 0.788 / 0.800 |
| Smooth (0) | 1e4 | 0.198 / 0.200 | 0.496 / 0.500 | 0.795 / 0.800 |
| Sharpen (2) | 1e3 | 1.816 / 1.800 | 1.430 / 1.500 | 1.182 / 1.200 |
| Sharpen (2) | 1e4 | 1.798 / 1.800 | 1.487 / 1.500 | 1.191 / 1.200 |

The gap grows with the wavelength against the 460-texel layout (at w = 100, 145 texels for λ/2,
smoothing gave 0.184 against 0.200): the layout's border, where the solve has no neighbors,
is near. The test that now pins the Detail Size, a 74-texel wavelength fitted over three whole
periods, measures 0.502 and 1.501 at λ; the probe fitted over the middle half of the layout,
not whole periods, which probably accounts for its larger gap there. The program's default w = 1000 is λ = 0.199 √A, close to 20% of the bounding box
diagonal on a compact mesh (√A is 212 against a diagonal of 198 on the gargoyle).

**Mapping**, each a length (`absperc`), w = A (2π / λ)² computed in the adapter
(`weightForDetailSize`). The ids are pymeshlab2's keyword arguments, renamed with no alias. The
two rows marked derived were approved on the shared equation, without a measurement of their own:

| Filter | Current id · label · default | Target id · label · default | Evidence | Verdict |
|---|---|---|---|---|
| *Smooth Texture* | `fidelity` · `Fidelity` · 1000 | `detailSize` · `Detail Size` · 20% of the diagonal | measured above | id, label, type |
| *Sharpen Texture* | same | same | measured above | id, label, type |
| *Transfer Texture Detail* | `fidelity` · `Fidelity` · 1000 | `detailSize` · `Detail Size` · 20%: details smaller come from the detail texture | the same operator, gain s k² / (w + k²) on the detail texture; derived, not measured | id, label, type |
| *Colorize Texture by Principal Directions (LIC)* | `sharpenFidelity` · `Sharpening Fidelity` · 1e4 (advanced) | `sharpenDetailSize` · `Sharpening Detail Size` · 5% (λ = 0.063 √A at the program's 1e4) | the stage-2 solve, measured above | id, label, type |
| *Compute Principal Curvature Directions per Face (TSP)* | `smoothingWeight` · `Normal Smoothing Fidelity` · 1000 (advanced) | `smoothingSize` · `Normal Smoothing Size` · 20%, per round | the same screened operator on the normals; derived, not measured | id, label, type |
| *Smooth*, *Sharpen*, *Transfer Texture Detail* | `gradientScale` | unchanged; the help says it is the gain on details well below the Detail Size, and the Detail Size where it is half applied | dimensionless already | help only |
| *Dilate Texture across Seams* · *LIC* · *Curvature* | `radius` (texels) · `anisotropyExponent` · `smoothingIterations` | unchanged | a length in texels, an exponent, a count | none |

Totals: 5 filters change an id, 3 more a help; 3 parameters stay. The default 20% is a new bound
token, `@bboxDiagFifth`, named like `@bboxDiagTenth` and `@bboxDiagHalf`: `@bboxDiag02` would
read as 2% beside `@bboxDiag002`. 5% is `@bboxDiag005`. *Detail Size* is in the
[vocabulary](../vocabulary.md): the wavelength at which a gradient-domain filter is halfway
between the input and its effect. `FilterTests::textureSignalProcessingDetailSizeIsTheCrossover`
holds Smooth and Sharpen Texture to it.

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
- ***Inpaint Texture on Selected Faces*** (stage 3, approved 2026-11-12): admits `Inpaint` narrowly, for
  filling a region of an image from its surroundings, as the image-processing term of art,
  the way `Close` was admitted for holes. `Repair` would also fit its definition, but would
  not tell this filter from *Repair Texture Seams*; `Fill` is rejected under `Close`.
- ***Transfer Texture Detail*** (stage 3, approved 2026-11-12): `Transfer` as defined, data moving
  between textures. Its parameters are *Base Texture* and *Detail Texture*.
- **The normal-map option** (stage 3, approved 2026-11-12): *Texture Holds Object-Space Normals*.
- **Line-integral convolution** (stage 4, decided 2026-11-13): *Colorize Texture by Principal
  Directions (Line Integral Convolution)*, `Colorize` as defined ("write color derived from other
  data"), as *Colorize Vertices by Perlin Noise*. `Rasterize` was considered and not admitted:
  it suggests scan-converting the field's lines, and the filters that do scan-convert attributes
  into textures are `Transfer`.
- **Reaction-diffusion** (stage 4, 2026-11-13): *Colorize Texture by Reaction-Diffusion*, `Colorize` as
  for line-integral convolution, the hyphen of the term of art.

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
4. **Reaction-diffusion's reaction term.** Answered 2026-11-13 (Paolo): keep the quadrature,
   through the lower-level API.
5. **Multigrid or direct.** Answered with stage 2: at 512² the direct solve is faster than
   enough V-cycles to match it (2.5 s against 7.5 s for 100), but at 2048² it takes 63 s and
   5.7 GB to factor against 28 s of multigrid, and grows faster from there. Multigrid.
6. **Inpainting cost** (stage 3). Answered: the direct solve is enough. Most of the cost is
   building the operators, as for the repair: a second and 1.4 GB for 5% of a 20k-face mesh at
   1024², 3 s and 1.7 GB for half; on the 3M-face scan at 4096², 22 s and 18 GB for 5% of the
   faces, 96 s and 22 GB for half of them, far more than inpainting is for.
