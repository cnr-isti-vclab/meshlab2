# Local Flattening

This document plans a reimplementation, as MeshLab filters, of Pietroni, Corsini, Cignoni and
Scopigno, *An Interactive Local Flattening Operator to Support Digital Investigations on Artwork
Surfaces* (IEEE TVCG 17(12), Visualization 2011; `.reference/papers/IEEEViz_local_flattening_final_v2.pdf`).
**Not implemented**; drafted 2026-11-13, scope and answers from Paolo, a co-author, 2026-11-15.

See also: [Texture Signal Processing](texture_signal_processing.md) (per-face principal directions
in `VCGFace`, stage 4), [Directional](directional_port.md), [TexMetro](texmetro.md),
[Adding a Filter](../adding_a_filter.md), [Vocabulary](../vocabulary.md).

## What the paper does, and what is in scope

A user sketches a **guidance polyline**, along which a patch runs, and an **interval polyline**,
whose length is the patch's half-width. The operator then:

1. **Selects** the faces whose geodesic distance from the guidance polyline is below the interval's
   length, and rejects the selection unless it is a disk.
2. **Interpolates a cross field** over the selection that follows the guidance polyline: a
   parametrization with a least-squares term pulling the polyline's points to a constant U (eq. 1,
   stiffness k_st), the two ends of the polyline fixed; the U direction of that map, carried back
   to 3D, is the field.
3. **Parametrizes** the selection aligned to the field, as the field-aligned methods do [Bommes et
   al. 2009], but on a disk, so with no cuts or integer variables.
4. **Crops** to the rectangle spanned in UV by the ends of the two polylines.
5. **Samples a height map** of the residual marks (§4.1): the patch as a geometry image at a chosen
   resolution (0.25 mm per pixel), smoothed by a Gaussian to keep only the overall shape, each
   sample cast along its normal back onto the surface; the distances form the image.

The paper goes on to extract chisel traces as ridges and valleys of that image (§4.2), to compare
photographs on the patch (§5), and to set the traces against curvature directions (§6). **Out of
scope** (decided 2026-11-15): only steps 1 to 5 are reimplemented. Photographs onto a patch already
work with *Transfer Color from Visible Rasters to Texture* once the patch has UVs.

## Decisions (2026-11-15)

- **Single resolution.** The paper parametrizes a medium-resolution model and samples the full scan;
  here one layer is both, and a user who wants the paper's split simplifies a copy by hand.
- **A width, not an interval polyline.** The interval polyline only gives a length.
- **vcglib's `PoissonSolver` is the paper's solver**, and is cleaned up to vcglib's current rules
  rather than replaced (stage 0).

## Step 2: what eq. 1 can mean

The text is self-contradictory in two ways, and only one reading makes the system work.

**U or V does not matter.** Pulling the polyline's points to a constant U makes the polyline an
iso-line of U, a segment parallel to the V axis, while the text says it is aligned with U. But the
field built from the map is a cross field: both directions, and their opposites, are the same
field, so which axis the polyline lies along changes nothing but the labels. Step 4's crop uses the
polyline's ends in UV, so it does not care either.

**Harmonic or conformal matters.** The text says a discrete harmonic map with cotangent weights.
That energy is the Dirichlet energy of U plus that of V, the two independent of each other:

- U is then pulled to U_k at the polyline's points, and at its two fixed ends (which "should be
  correctly aligned with U"). The constant U = U_k satisfies every constraint at zero energy, so U
  is that constant, and carries no information.
- V is harmonic with only its two end values fixed and a free boundary. That is well defined, but
  its gradient runs from one end to the other with singularities at the two ends; nothing in it
  follows the polyline, since the polyline's term acts only on U.

So with a harmonic energy the alignment term has no effect, and the map collapses onto a line.

It works if the energy **couples U and V**, as the conformal (LSCM) energy does: the Dirichlet
energy minus the signed area of the map, which ties ∇U to ∇V rotated by 90° (Cauchy-Riemann). Then
making the polyline an iso-line of U forces ∇V to run along it, and conformality carries that
direction smoothly into the rest of the patch. Two things point to this reading:

- **LSCM needs exactly two pinned points** to have a unique solution, which is what the text says:
  "it is sufficient to fix the two extremes of the guidance polyline … to make the sparse system
  solvable". A harmonic map with a free boundary would also need them, but would still collapse,
  as shown above.
- **vcglib's `PoissonSolver` is that solver.** Without a direction field it adds `AddAreaTerm` to
  the cotangent matrix, the antisymmetric coupling that turns the Dirichlet energy into LSCM's, and
  pins the two farthest vertices (`FixDefaultVertices`). The paper's step 2 is that mode plus the
  least-squares term on the polyline; step 3 is the same solver's other mode, `use_direction_field`,
  which fits ∇U and ∇V to a field read from the faces' `PD1` and `PD2`, with one pinned vertex.

The field of step 2 is then, per face, the gradient of U carried to 3D and its rotation in the face's
plane. In a conformal map the two are orthogonal and of equal length, so they make a cross field
directly.

The alternative needs no map at all: give the faces the polyline crosses its tangent as `PD1`, and
spread it over the selection with vcglib's `CrossField::PropagateFromSelF` and `SmoothIterative`
(faces under the polyline fixed). It is simpler, but it is not the paper's method, and the field
away from the polyline would be smoothed rather than conformal. **Proposed:** the conformal reading
as the method, and the propagation only if the first proves fragile.

## What MeshLab and vcglib have for each step

| Step | Existing | Missing | Plan |
|---|---|---|---|
| Polyline on the surface | Polyline layers; *Create Polyline from Geodesic Path*, *from Planar Section*; *Embed Polyline in Surface* (vcglib `CoMEmbed`) | A sketch tool | The filter takes the guidance polyline as a layer. A tool may follow as a front end that runs the filter |
| 1. Selection | vcglib geodesic distance (`Geodesic`, heat), *Select by Scalar*; `PoissonSolver::IsFeasible` (a disk) | Seeds on the polyline instead of on vertices | Embed the polyline in a working copy, so its points become vertices and seeds; or seed the geodesic at the polyline's points directly |
| 2. Cross field | `PoissonSolver`'s conformal mode | Soft linear constraints on points inside faces (eq. 1: one row per polyline point, its three corners weighted by its barycentric coordinates) | Add to `PoissonSolver`, which already assembles least-squares rows for its fixed vertices |
| 3. Field-aligned parametrization | `PoissonSolver` with `use_direction_field`, reading face `PD1`/`PD2` (in `VCGFace` since the TSP stage 4) | Nothing beyond stage 0's clean-up | Reuse |
| 4. Crop to a rectangle | vcglib `clip.h`, exact plane clipping (*Trim Surface by Plane*) | Clipping in UV | Clip a working copy laid out in UV by the rectangle's four lines, exactly, and carry the positions with the cuts |
| Distortion (Table 1) | vcglib `Distortion`, the TexMetro measures | The paper's edge-length ratios as one result | Report min and max ratio and the average error from the filter |
| 5. Height map | vcg `GridStaticPtr` ray casting (as *Select by Rectangle* uses); raster layers (`QImage`, `Grayscale16`) | Sampling the geometry image, Gaussian smoothing, casting along normals | New, in the plugin |

## Stages

0. **`PoissonSolver` cleaned up, with point constraints.** To vcglib's rules (`vcglib/AGENTS.md`):
   throw on a region that is not a disk or a singular system instead of `assert` and `printf`;
   report through a `CallBackPos`; vectors instead of `std::map` indexing, since a patch of a scan
   can have a million faces; the dead commented code removed. Then the new soft point constraint.
   The sample `trimesh_disk_parametrization` stays a short sample. A vcglib commit, push and gitlink
   bump by Paolo.
1. **The flattening filter.** *Parametrize Surface along Polyline (Local Flattening)*, name to be
   approved. Inputs: the current mesh, the guidance polyline (a `mesh` parameter naming a polyline
   layer), the half-width (`absperc`), the alignment stiffness k_st. Output: a **new layer**, the
   rectangular patch, with per-wedge UVs in the surface's units, so that lengths read off the image
   are lengths on the surface; the distortion figures as output values. Refuses a region that is not
   a disk. Tests: a cylinder, developable, so the patch must come out isometric up to discretization;
   then a strip of a scan (`TestDataset/Ruth_1M.ply`, `StatuaPisa.ply`, `Shepard_1M.ply`), against
   the paper's distortion figures (edge-length ratio within about 0.83 to 1.17, average error under
   5%).
2. **The height map.** A filter from the patch layer to a raster layer holding a 16-bit height
   image, at a resolution in pixels per unit and with the smoothing scale σ; the height range as
   output values, to read heights back. Rays are cast from the smoothed geometry image onto the
   patch itself (single resolution). Tests: a known bump added to a smooth surface must come back.
3. **The sketch tool**, later and only if wanted: drawing the guidance polyline on the surface as a
   front end that runs stage 1's filter, one gesture to one undo step.

## Possible later extension

The interval polyline could give a second direction. `PoissonSolver`'s field mode fits ∇U to `PD1`
and ∇V to `PD2` separately, each normalized on its own, so it does not require them to be
orthogonal: a patch whose two axes follow two different polylines, a non-orthogonal frame, would
need only a second field.

## Open questions

1. **The conformal reading of step 2** (above): is that how the original worked?
2. **The patch's scale.** In field mode the directions are normalized, so ∇U and ∇V have unit length
   and the patch comes out at the surface's scale (`fieldScale` 1). Is that how lengths were kept, or
   was the patch rescaled afterwards?
3. **The name** of the stage 1 filter, and of stage 2's (*Sample Height Map from Surface*?).
