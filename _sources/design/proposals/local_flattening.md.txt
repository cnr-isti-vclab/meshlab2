# Local Flattening

This document plans a reimplementation, as MeshLab filters, of Pietroni, Corsini, Cignoni and
Scopigno, *An Interactive Local Flattening Operator to Support Digital Investigations on Artwork
Surfaces* (IEEE TVCG 17(12), Visualization 2011; `.reference/papers/IEEEViz_local_flattening_final_v2.pdf`).
**Implemented** (stages 0 to 2, 2026-11-15 to 17; stage 3, the sketch tool, left to the annotation work); drafted 2026-11-13, scope and answers from Paolo, a co-author, 2026-11-15.

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
- **Step 2 is conformal** (confirmed by Paolo): `PoissonSolver`'s LSCM mode with the least-squares
  term of eq. 1, as reasoned below.
- **The patch keeps the surface's proportions**: the field-aligned solve is at the surface's scale
  (vcglib's `LocalFlattening` returns UVs in surface units), and the filter then scales both axes
  alike into the unit square, its longer side 0 to 1, reporting the factor as `uv_scale`. Revised
  the same day (Paolo): texture tools read UVs in [0,1], so surface units left the patch unusable
  by the transfer and photo-projection filters.
- **Names**: *Parametrize Surface along Polyline (Local Flattening)*; the height map became a data
  source of an existing filter rather than *Sample Height Map from Surface* (stage 2).
- **Step 2 constrains V too** (Paolo, after measurement): the polyline's points are pulled to
  U = 0 *and* to V equal to their arc length, with no stiff pins at its ends. Eq. 1's U alone
  leaves nearly conformal deformations free, and they rotate the field: on a developable cylinder
  a quarter-turn helix came out with 1.7% average length error, against 0.02% with both
  coordinates and no pins (the pins add a singularity around each end).

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
   **Implemented 2026-11-15**: 684 lines to 333; the system assembled from triplets and solved by
   `SimplicialLDLT` (980k faces in 19 s, conformal); fixed vertices a penalty on the diagonal
   rather than extra variables; `FixVertex` and `AddPointConstraint(face, bary, coord, value,
   weight)` added; `SolvePoisson` takes a `CallBackPos` and throws on too few fixed vertices,
   `IsFeasible` no longer prints. Three bugs fixed on the way: the field mode came out at twice the
   surface's scale (the ½ of ∇φ = n×e/2A was commented out), meshes with deleted vertices indexed
   past the system, and the fold correction mirrored the vertices after their UVs had been copied
   to the wedges. Checked on a cylinder strip: conformal and field-aligned maps isometric, a
   polyline pulled to U = 0 lands there to 1e-12.
1. **The flattening filter.** *Parametrize Surface along Polyline (Local Flattening)*, in
   `filter_texture`, a thin adapter over vcglib's new `LocalFlattening`
   (`parametrization/local_flattening.h`) and `ClipMeshWithUVLine` (`clip.h`, the texture-space
   sibling of `ClipMeshWithPlane`). Inputs: the current mesh, the guidance polyline (a `mesh`
   parameter), the half-width (`absperc`), the stiffness k_st (advanced, default 10). Output: a
   new layer, the rectangle, with per-vertex and per-wedge UVs in proportion (see the scale
   decision above), and the
   paper's distortion figures as output values. Refuses a polyline that is not one open curve and
   a region that is not a disk. **Implemented 2026-11-15**, with what the measurements forced:
   - the cotangent weights in double (float rounding excited the near-null conformal modes:
     |grad U| of 0.82 to 1.12 on a developable strip, 0.95 to 1.04 in double);
   - the polyline's ends fixed as points inside their faces (`PoissonSolver::FixPoint`), not at
     the nearest vertices, which twisted the map by up to half an edge;
   - zero-area faces skipped by the solver (one turned the whole solve into NaN), and the UV clip
     snapping at rounding level, as the plane clip does, so it makes none.

   **Measured.** Cylinder, straight polyline: 0.033% average error, ratios 0.994 to 1.003; helix:
   0.023%, 0.991 to 1.003, exact rectangles, no folds (`FilterTests::localFlatteningUnrollsA
   CylinderIsometrically`). Raw scans (`Ruth_1M`, `Shepard_1M`, `RattoA_2M`), strips 6% of the
   diagonal long and 3% wide, 0.15 to 0.4 s each: with polylines smoothed on the surface, 2.5 to
   7% average error on most strips, the paper's range (Table 1: 1.4 to 3%, on chosen regions of a
   medium-resolution model); with the raw edge paths *Create Polyline from Geodesic Path* makes,
   which zigzag and run 20 to 60% longer than the curve they stand for, 5 to 20%, U only or U and
   V alike. A few strips still collapse locally at a corner (one face pair with a ratio in the
   thousands); not yet understood.
   **Grid remeshing and smoothing** (added 2026-11-16, Paolo): the patch can be remeshed in
   parametric space as a regular grid laid exactly on the rectangle (*Remesh as a grid*, a step),
   and its shape smoothed by a Gaussian (*Smoothing*, sigma), both through one geometry image:
   the cropped surface sampled at the grid's nodes, each node at the nearest point of the patch in
   UV within a twentieth of a step, so that rounding along the crop's border loses no node while
   a hole in the scan stays one. Smoothing moves each sample only along the normal of the
   smoothed shape, P' = P - n((P - S) . n): the smoothed point S alone also slides sideways near
   the border, where the Gaussian sees one side only, and pulled the patch's edges in (edge
   ratios down to 0.36 on the test cylinder). Without the grid, each vertex moves as the grid
   samples around it did. The crop was made exact on the way: `ClipMeshWithUVLine` now puts the
   vertices it takes as lying on a line exactly on it, as `ClipMeshWithPlane` does. Measured on
   the cylinder: a complete 2 x 40 x 80 grid with ratios 0.999 to 1.003 unsmoothed, and with
   sigma 0.3 pulled in by sigma^2 / 2r as a smoothed cylinder should be; on the Ruth strip, a 1 mm
   grid smoothed by 5 mm from the raw scan, then baked from it along the normal, reads the same
   grooves as the two-copy workflow, in one step, sampling the scan evenly (0.5% against 1.4% by
   closest point).
2. **The height map**, decided 2026-11-15 after a first version was withdrawn: not a filter of
   its own but a **Signed Distance** data source of *Transfer Vertex Attributes from Another Layer to Texture*, which already samples a source mesh at every texel of the current mesh's UV
   layout. It writes the distance from each texel's point to the closest point of the source,
   signed by the current mesh's normals, into a 16-bit grayscale texture: 0 where nothing is
   found, 1 to 65535 across the range, given as `distance_min` and `distance_max`. That restores
   the paper's two resolutions: flatten a smoothed or simplified copy, then bake the full model
   onto the patch. The withdrawn version (`HeightMap` in vcglib, *Sample Height Map from
   Surface*) re-implemented the texel sampling, smoothed a geometry image of the patch itself and
   measured from it: single resolution, about 240 lines, and a duplicate. **Implemented
   2026-11-15**: a bump of 0.2 reads 0.2 to within 0.002, the flat part 0 to within 1e-4
   (`FilterTests::signedDistanceTransferBakesABump`). On a Ruth strip flattened from a copy
   smoothed by 3 Laplacian iterations, at 0.25 mm per texel, the carved grooves stand out over
   the facets of the 1M-face scan; flattening the smoothed copy also brought that strip's
   distortion into the paper's range (2.4% average, ratios 0.93 to 1.08, against 4.5% on the
   raw scan).
   **Along the normal** (added 2026-11-16, Paolo): both transfer-to-texture filters take a
   *Correspondence*, closest point (the default, unchanged) or along the current mesh's
   interpolated normal, both ways, outward hit first, falling back to the closest point on a
   miss, as Tarini, Cignoni and Scopigno, "Visibility based methods and assessment for
   detail-recovery" (Vis 2003) describe; their measurements found it samples the detailed model
   far more evenly than closest point. Along the normal, the signed distance is a height along
   the normal, as a displacement map means, and as the flattening paper casts its rays. On the
   Ruth strip, with the paper's sampling-uniformity measure (the mean change of distance between
   neighbouring samples): 1.4% by closest point against 0.4% along the normal over a base
   smoothed 3 times, 3.6% against 0.9% over one smoothed 10 times, no ray missing; the grooves
   read deeper (-5.5 against -3.9 mm), closest point cutting across their flanks.
3. **The sketch tool**: left out (Paolo, 2026-11-17). Tracing and editing lines on 3D surfaces
   belongs to the annotation work, which will treat them coherently for every use; the
   flattening filter takes whatever polyline layer that produces.

## Possible later extension

The interval polyline could give a second direction. `PoissonSolver`'s field mode fits ∇U to `PD1`
and ∇V to `PD2` separately, each normalized on its own, so it does not require them to be
orthogonal: a patch whose two axes follow two different polylines, a non-orthogonal frame, would
need only a second field.

## Answered questions (2026-11-17)

- **Zigzag polylines**: the filter assumes a smooth polyline, and its description says so,
  pointing the edge paths of *Create Polyline from Geodesic Path* to *Smooth Polyline on
  Surface* first.
- **The local collapses** on scan strips, replicated on `Shepard_1M` (a ratio of 6467, one fold),
  have two causes, neither in the flattening:
  1. folds of the scan itself, a triangle turned back by 170 to 180 degrees onto its neighbour,
     sometimes next to needles: no flat map can lay them out, so they collapse in UV. *Remove
     Duplicate Faces* does not catch them (it removed 29 duplicates elsewhere on that model,
     changing nothing), nor the edge-flip repair of isolated folds (these come in pairs);
     *Select Ill-Shaped Faces* finds them.
  2. regions too curved for the half-width, where the conformal map of step 2 nearly collapses
     (|grad U| down to 0.03 against about 1) and its direction is unreliable: a few faces fold
     there, on strips whose distortion is about 16% anyway.

  The filter now reports the faces folded in UV (`folded_faces`, and a warning naming *Select
  Ill-Shaped Faces*). Weighting step 3's fit by the strength of step 2's gradient might remove
  the second kind; not done.
