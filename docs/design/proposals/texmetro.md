# TexMetro: Reference Values

**Not implemented: the comparison with TexMetro itself.** MeshLab reports every key of
[TexMetro](https://github.com/cnr-isti-vclab/texmetro)'s JSON, but no number of ours has yet
been checked against a JSON that TexMetro wrote, and the questions below are still open with its
authors. This is Stage 0 of the original proposal, the one stage that needs no code.

**What shipped** (2026-10-23 to 26): the metrics as CPU algorithms in vcglib
(`parametrization/{distortion,uv_utils,texel_coverage,seam_discrepancy}.h`) with thin filters in
`filter_measure`, `filter_select` and `filter_meshing`. The filters, the key-by-key table and where
the numbers knowingly differ from TexMetro's are in [`docs/texture_metrics.md`](../../texture_metrics.md);
the reasoning, the measurements and the options rejected (vendoring TexMetro, a QRhi raster) are in
the [history record](../history/texmetro.md).

See also: [Test Data](../test_data.md), [Vocabulary](../vocabulary.md).

## What Stage 0 is

1. Build TexMetro (at `cfc35548`, 2020-04-24) on Linux or Windows. Its raster shaders are GLSL
   4.30, and a Mac stops at OpenGL 4.1, so it cannot run here.
2. Run it on the two gargoyle atlases (`tests/local/COLOR_gargoyle/gargo3M_{good,bad}.ply`, a
   local-only dataset) and on models of the *Real-World Textured Things* benchmark; keep the JSON.
3. Compare with `docs/examples/texmetro_report.py model --compare reference.json`, which lists the
   keys that differ by more than a tolerance. Its starting tolerances are guesses from what is
   known of where the two differ; set them from the real comparison.
4. Settle the open questions with the authors, and decide which TexMetro quirks stay MeshLab's
   default.

The tooling has been ready since 2026-10-26. What is missing is the reference JSON, from a machine
that can run TexMetro.

## What the comparison should expect

**Rare texel counters move with the rasterizer's sub-pixel precision.** The texel counters were
measured with the CPU rasterizer at `S` = 4 (the OpenGL minimum, and what this Mac's driver
reports), 8 and 16 sub-pixel bits, on the same triangles. At level 0 the large counters move by a
few parts per million; overwritten and lost texels by about 0.1 % between 16 and 8 bits and about
1 % at 4 (good atlas: lost 8,652 → 8,666 → 8,752). At coarse levels, where counts are small, the
effect is larger (lost texels at 512² on the good atlas: 122 at 16 bits, 161 at 4). The benchmark's
numbers came from whatever drivers ran them, so **bit-for-bit agreement is not the target**; the
8-bit-against-4-bit spread is the floor of any tolerance. MeshLab defaults to 8 bits (*Subpixel
Bits*).

**Known, deliberate differences**, all listed in `docs/texture_metrics.md`: `nfolds` does not count
zero-UV-area faces (20 fewer of 1,231 on the good atlas, 22 of 1,475 on the bad); no UV recentring;
oversized textures are counted, not zeroed; `orientable` is not reported. With the defaults
(*Clash Neighbours* diagonal, *Pairing* edge order) the clash and seam keys follow TexMetro's code.

## Open questions

For the authors of TexMetro (quirk numbers as in the [history record](../history/texmetro.md#quirks-found-in-texmetros-code)):

1. **The clash rule.** `Clash3x3` compares only the four diagonal neighbours, while bilinear
   sampling touches the four edge neighbours first. All eight changes `texelClashes` by +3.3 %
   (good) and +4.0 % (bad). Intended? MeshLab offers both and defaults to TexMetro's.
2. **`totalTexels_bilinear`** counts a 3 × 3 footprint where bilinear sampling reads 2 × 2. A
   deliberately conservative bound, or an approximation?
3. **UV recentring and wrapping.** Why a 10⁻³ UV-unit shift for a UV box centred outside [0,1], and
   should UVs outside it be wrapped (as the seam sampler does) or clipped (as the raster does)?
   Tiled textures are the case.
4. **The outlier rule.** Is it intended that `sf`'s cutoff depends on `occupancy`, and so on the
   texture resolution?
5. **The seam pairing.** Each side's i-th sample is taken from the vertex its own face lists first,
   so a point is compared with the *mirrored* point of the other side, reading about 16 % high on
   both atlases (12.31 against 10.63 on the good one). MeshLab offers *Same point*; which should be
   the default?
6. **TexMetro's own future.** Should TexMetro move onto the vcglib code, dropping OpenGL and GLEW
   so that it runs on a Mac?

For this repository:

7. **The gargoyle atlases**: can they be published as a test dataset
   (`tests/datasets/datasets.json`, per [Test Data](../test_data.md))? Then the comparison could
   run in CI against a committed reference JSON.
