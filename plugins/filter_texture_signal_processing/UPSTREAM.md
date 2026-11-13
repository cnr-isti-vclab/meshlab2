# TextureSignalProcessing integration

## Provenance and boundary

*Repair Texture Seams*, *Dilate Texture across Seams*, *Smooth Texture*, *Sharpen Texture*,
*Inpaint Texture on Selected Faces*, *Transfer Texture Detail*, *Colorize Texture by Principal
Directions (Line Integral Convolution)* and *Compute Principal Curvature Directions per Face
(TextureSignalProcessing)* run the code of Prada, Kazhdan, Chuang and Hoppe, *Gradient-Domain
Processing within a Texture Atlas* (SIGGRAPH 2018), from
[`mkazhdan/TextureSignalProcessing`](https://github.com/mkazhdan/TextureSignalProcessing)
(TSP), in process. The design, and the measurements behind it, are in
[`docs/design/proposals/texture_signal_processing.md`](../../docs/design/proposals/texture_signal_processing.md).

| Repository | Licence | Where MeshLab takes it from |
|---|---|---|
| TextureSignalProcessing | MIT (`LICENSE`) | `external/TextureSignalProcessing`, on `cignoni/TextureSignalProcessing`, branch `meshlab` |
| Misha | BSD-3-Clause, in every header | `external/texture-consolidation/extern/Misha`, TextureConsolidation's checkout |

Both are compiled unmodified. **`tsp_adapter.cpp` is the only file that includes them**: it
defines `MK_ERROR_OUT` to throw, since Misha's version calls `exit(0)`, and `MK_WARN` to
nothing, since TSP's warnings are about its own bookkeeping and run to thousands of lines.
The rest of the plugin sees plain C++ (`tsp_adapter.h`).

Misha comes from TextureConsolidation's checkout because TSP's own copy, under
`third_party/include/Misha`, does not compile with Apple clang, and the two plugins should
build against the same Misha. It needs none of the fixes below.

**No Greg Turk PLY code is linked in.** Misha's PLY reader (`PlyFile.h`) carries Stanford's
notice, which forbids selling it and so cannot go into a GPL application. Fix 4 below keeps
it out of everything the adapter includes: compiled, the adapter has no `PLY::` or
`GregTurk::` symbol. A TSP update must keep it that way (`nm -C` on the adapter's object).

## The fork

The submodule points at a fork because the plugin needed changes upstream had not merged.
Each is a pull request of its own, from a branch of the fork, and `meshlab` merges them onto
upstream's `adfb89e`. Upstream merged the first five (#20 to #24) by 2026-11-13, with jdumas's
#18 (`4563bbf`, a fix in `ConstrainedTriangulation.h`) after them; the sixth is still open.

| # | Pull request | Without it |
|---|---|---|
| 1 | [#20](https://github.com/mkazhdan/TextureSignalProcessing/pull/20) Atlas charts: index a vertex shared by two charts in each of them | Heap corruption, and a crash, on any atlas where two charts touch at one vertex: 167 vertices on a 3M-face photogrammetry atlas |
| 2 | [#21](https://github.com/mkazhdan/TextureSignalProcessing/pull/21) Boundary cells: skip a triangle that only touches the cell | An out-of-bounds read, or the library calling `exit(0)`, when a triangle edge passes exactly through a texel center |
| 3 | [#22](https://github.com/mkazhdan/TextureSignalProcessing/pull/22) GradientDomain: let a caller skip the divergence operator | 3.5 times the memory: 44.5 GB instead of 15.6 GB on that atlas at 4096² |
| 4 | [#23](https://github.com/mkazhdan/TextureSignalProcessing/pull/23) Keep the PLY library out of code that does not read or write meshes | The no-sale PLY code linked into MeshLab |
| 5 | [#24](https://github.com/mkazhdan/TextureSignalProcessing/pull/24) Draw the atlas jitter from a generator of its own, not `rand()` | Every run of a filter in the application gives a different image: the jitter continued the `rand()` sequence (smoothing a 20k-face atlas at 1024², 2.2 million values differed between runs, by up to 0.73 levels) |
| 6 | (to open) Jitter every atlas vertex, not only those on a line of texel centers | A crash in `_InitializeChart`, or a coarse level failing to factor, on a layout whose vertices sit on the texel grid, such as a regular grid of UVs whose spacing is a whole number of texels: a texel center on an edge two triangles share was covered by neither, since each triangle computes the edge's crossing in Misha's rasterizer on its own (`FilterTests::textureSignalProcessingTakesUvsAlignedWithTheTexels`) |

**When the sixth is merged:** point `.gitmodules` back at `mkazhdan/TextureSignalProcessing`,
drop `branch = meshlab`, move the gitlink to upstream's merge commit, and delete this section.

## What the adapter does that the programs do not

- **Padding.** The library needs charts at least a texel or two from the image border. The
  adapter grows the grid by a margin, the edge texels repeated, only when the coordinates come
  within two texels of the border, and crops afterwards. A margin changes results at texels
  whose centers lie on an edge, so an atlas that does not need one gets none.
- **A pull towards the current values**, the one change to the method. Exterior texels that
  a triangle only grazes are barely determined by the energy, and `SetExteriorTexels` drives
  them far out of range, which shows as white and black dots along the islands (and an
  uncoupled one leaves the system singular). Each exterior texel is pulled towards its current
  value with a weight of 1e-2 of the median coupling: the dots drop from 22,220 to 2,628 on a
  3M-face scan, and the seam discrepancy on the gargoyle test comes out slightly lower (28.03
  against 28.26). Without the pull, and on an atlas needing no margin, the adapter was
  byte-identical to the program; the comment at `kPull` has the sweep.
- **Covered texels are never written.** TSP numbers texels per chart, so where charts sit
  closer than two texels one texel can be covered by one chart and outside another.
  `SetExteriorTexels` writes the nodes back in turn and can overwrite a covered texel (34 on a
  512² xatlas atlas packed with no padding); the adapter leaves it.
- **Dilation samples the input.** `TextureDilation` samples the image it is filling, so
  threaded runs differ from one another; the adapter samples a copy, and the result is the
  same on every run. A one-line upstream fix has been offered (in #23's description).
- **Texture coordinates are fused** on the key TSP's PLY reader uses (surface vertex, exact
  UV), since the library finds charts through shared texture vertices.
- **Smoothing and sharpening** (`TextureFiltering`'s uniform mode) use `HierarchicalGradientDomain`,
  TextureFiltering's four levels and three sweeps, without the divergence operator: with a uniform
  gradient scale its right-hand side is `S x0`. The direct solver is not offered: it took 63 s and
  5.7 GB to factor at 2048², against 28 s for multigrid to agree with it within one level. The
  V-cycles run until one changes no texel by more than a fiftieth of a level (the change per cycle
  understates the remaining error about 40 times on these atlases), and can be canceled.
- **Localized filtering** (only selected faces) builds the divergence operator and scales the edge
  differences by the mean of their ends' factors, a node being selected when the triangle nearest
  its texel is; only selected nodes are written. With every face selected the result is the
  uniform one exactly. The nearest triangle comes from Misha's candidate lists
  (`GetSupportedTexelInfo` with `UseVector`), whose ties are broken by index: the other path keeps
  whichever supporting triangle a thread wrote last, so the edge of a selection moved from run to
  run (20 texels of 31,000 on a 20k-face atlas).
- **Alpha is left as it was** by smoothing, sharpening, inpainting and detail transfer, as in the
  programs, which read color only. The adapter used to solve it as a fourth channel, and the
  transparent gutter of a projected texture bled into the islands: smoothing left 162,929 of
  162,932 opaque texels partly transparent. The repair and the dilation still write it, which
  makes the gutter they fill opaque.
- **Inpainting** is `SeamStitcher`'s mask mode, the mask being the texels whose nearest face is
  selected: the repair's solve, with the region freed instead of the exterior texels. The exterior
  texels outside the region are freed as well, and not written: held, the background of the gutter
  bled into the fill as a boundary value. Only the texels coupled less than 1e-3 of the median take
  the repair's pull towards their current value; on every exterior texel, it added up along the
  seams and held a large region back towards what was there (the comment at `kWeak` has the sweep).
- **Detail transfer** is `TextureFiltering --inLow`: the values held to the base texture, the
  gradients taken from the detail texture, which the plugin resamples to the base's size when it
  has another (the program refuses it).
- **Object-space normal maps** (`TextureFiltering --colorAsNormal`) need no decoding, since the
  solves commute with an affine change of the values; the texels written are renormalized.
- **Line-integral convolution** takes its field from the mesh's principal directions, the faces'
  if they have them, the vertices' otherwise, rather than computing curvature itself, scaled by
  k1 - k2 as the program scales its own. The program's metric construction is reproduced (the
  embedding metric at unit area, the field at unit root-mean-square length,
  `InitializeVectorFieldMetric`) and passed to `GradientDomain`'s metric constructor. The streaks are
  **solved directly**, as with `--useDirectSolver`: under the 1e5 anisotropy multigrid does not
  converge (300 V-cycles left texels up to 862 levels from the direct solution on a 20k-face atlas
  at 1024², still changing by 9 per cycle), and the direct solve took less time. Without that
  flag, the program's batch output runs ten V-cycles of each solve (two sweeps per level) and
  stops there. The program's weight w is set from the filter's *Streak Length* L, in the
  mesh's units: w = A √(1 + 1e5) (3.357 / L)², A the area (`lineIntegralConvolution` derives it,
  and the proposal records the measurement). The noise comes from a `std::mt19937` seeded by the
  filter's `randomSeed`, as the program's random hue or as black and white or random gray. The
  sharpening is `filter()`, the stage-2 solve.
- **Curvature per face** is `InitializePrincipalCurvatureDirection`, after `LineIntegralConvolution`'s
  normal smoothing, which is in the program rather than the library and is ported into the adapter
  (`principalCurvature`), on Eigen matrices. TSP measures on the surface scaled to unit area, so its
  curvatures are multiplied back by the square root of the area.
- **Refused or left out:** faces folded over their own UV island are refused for the repair
  (they fault in `_InitializeChart`); faces with no UV area are left out of both.

## Updating

1. Move the submodule, rebuild, and run the plugin's tests (the `textureSignalProcessing` and
   `principalCurvatureDirections` rows of `FilterTests`, and the smoke rows).
2. Re-run the symbol check above.
3. If TSP changed `SetExteriorTexels.cpp`, `TextureDilation.cpp`, `TextureFiltering.cpp`,
   `SeamStitcher.cpp` or `LineIntegralConvolution.cpp`, compare the adapter's functions with them: the adapter reproduces their
   computation, with the differences listed above, and nothing else.
