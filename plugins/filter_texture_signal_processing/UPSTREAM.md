# TextureSignalProcessing integration

## Provenance and boundary

*Repair Texture Seams* and *Dilate Texture across Seams* run the code of Prada, Kazhdan,
Chuang and Hoppe, *Gradient-Domain Processing within a Texture Atlas* (SIGGRAPH 2018), from
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

The submodule points at a fork because the plugin needs four changes upstream has not merged.
Each is a pull request of its own, from a branch of the fork; `meshlab` merges all four onto
upstream's `adfb89e`.

| # | Pull request | Without it |
|---|---|---|
| 1 | [#20](https://github.com/mkazhdan/TextureSignalProcessing/pull/20) Atlas charts: index a vertex shared by two charts in each of them | Heap corruption, and a crash, on any atlas where two charts touch at one vertex: 167 vertices on a 3M-face photogrammetry atlas |
| 2 | [#21](https://github.com/mkazhdan/TextureSignalProcessing/pull/21) Boundary cells: skip a triangle that only touches the cell | An out-of-bounds read, or the library calling `exit(0)`, when a triangle edge passes exactly through a texel center |
| 3 | [#22](https://github.com/mkazhdan/TextureSignalProcessing/pull/22) GradientDomain: let a caller skip the divergence operator | 3.5 times the memory: 44.5 GB instead of 15.6 GB on that atlas at 4096² |
| 4 | [#23](https://github.com/mkazhdan/TextureSignalProcessing/pull/23) Keep the PLY library out of code that does not read or write meshes | The no-sale PLY code linked into MeshLab |

#17 (jdumas) fixes the same bug as #20 by overwriting the owner, which adds 89 vertices a second
time to a chart on that atlas; either one going in closes 1.

**When all four are merged:** point `.gitmodules` back at `mkazhdan/TextureSignalProcessing`,
drop `branch = meshlab`, move the gitlink to upstream's merge commit, and delete this section.
If upstream merges #17 instead of #20, check the 3M-face atlas first (see the proposal).

## What the adapter does that the programs do not

- **Padding.** The library needs charts at least a texel or two from the image border. The
  adapter grows the grid by a margin, the edge texels repeated, only when the coordinates come
  within two texels of the border, and crops afterwards. A margin changes results at texels
  whose centers lie on an edge, so an atlas that does not need one gets none and is
  byte-identical to `SetExteriorTexels`.
- **Covered texels are never written.** TSP numbers texels per chart, so where charts sit
  closer than two texels one texel can be covered by one chart and outside another.
  `SetExteriorTexels` writes the nodes back in turn and can overwrite a covered texel (34 on a
  512² xatlas atlas packed with no padding); the adapter leaves it.
- **Dilation samples the input.** `TextureDilation` samples the image it is filling, so
  threaded runs differ from one another; the adapter samples a copy, and the result is the
  same on every run. A one-line upstream fix has been offered (in #23's description).
- **Texture coordinates are fused** on the key TSP's PLY reader uses (surface vertex, exact
  UV), since the library finds charts through shared texture vertices.
- **Refused or left out:** faces folded over their own UV island are refused for the repair
  (they fault in `_InitializeChart`); faces with no UV area are left out of both.

## Updating

1. Move the submodule, rebuild, and run the plugin's tests (`FilterTests` rows for both
   filters, and the smoke rows).
2. Re-run the symbol check above.
3. If TSP changed `SetExteriorTexels.cpp` or `TextureDilation.cpp`, compare the adapter's two
   functions with them: the adapter reproduces their computation and nothing else.
