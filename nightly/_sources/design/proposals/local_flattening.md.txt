# Local Flattening

This document plans a reimplementation, as MeshLab filters, of Pietroni, Corsini, Cignoni and
Scopigno, *An Interactive Local Flattening Operator to Support Digital Investigations on Artwork
Surfaces* (IEEE TVCG 17(12), Visualization 2011; `.reference/papers/IEEEViz_local_flattening_final_v2.pdf`).
**Not implemented**; drafted 2026-11-13. The decisions marked *open* need Paolo, who co-authored the
paper.

See also: [Texture Signal Processing](texture_signal_processing.md) (per-face curvature directions
and line-integral convolution, stage 4), [Directional](directional_port.md), [TexMetro](texmetro.md),
[Adding a Filter](../adding_a_filter.md), [Vocabulary](../vocabulary.md), and the interactive tools
plan in the `interactive-tools-design` notes.

## What the paper does

A user sketches two polylines on a scanned sculpture: a **guidance polyline**, along which the
patch runs, and an **interval polyline**, whose length is the patch's half-width. The operator then:

1. **Selects** the faces whose geodesic distance from the guidance polyline is below the interval's
   length, and rejects the selection unless it is a disk.
2. **Interpolates a cross field** over the selection that follows the guidance polyline: a discrete
   harmonic (cotangent) parametrization with a least-squares term pulling the polyline's points to a
   constant U (eq. 1, stiffness k_st), the two ends of the polyline fixed; the U direction of that
   map, carried back to 3D, is the field.
3. **Parametrizes** the selection aligned to the field, as the field-aligned methods do [Bommes et
   al. 2009], but on a disk, so with no cuts or integer variables.
4. **Crops** to the rectangle spanned in UV by the ends of the two polylines.

On the resulting rectangular patch (Table 1: edge-length ratio between 0.83 and 1.17, average error
under 5%):

5. **Height map of the residual marks** (§4.1): the patch, built on a medium-resolution model (250k
   faces for the Pietà), is sampled into a geometry image at a chosen resolution (0.25 mm per pixel),
   smoothed by a Gaussian to keep only the overall shape, and each sample is cast along its normal
   onto the full-resolution scan; the distances form the image.
6. **Chisel traces** (§4.2): ridges and valleys of that image at a scale, after Lindeberg 1998: in a
   Gaussian scale space L = I * G(σ), with p and q the image's principal-curvature directions,
   ridges where L_p = 0, L_pp < 0, |L_pp| ≥ |L_qq|, valleys symmetrically; drawn colored by
   orientation, and grouped by similarity to a trace the user picks.
7. **Comparing photographs** (§5): photos registered to the model before and after a restoration are
   projected onto the same patch, giving two images of the same rectangle of surface.
8. **Marks against curvature** (§6): the principal curvature directions of a smoothed low-resolution
   model, drawn over the trace image.

## What MeshLab has for each step

| Step | Existing in MeshLab or vcglib | Missing | Verdict |
|---|---|---|---|
| Sketching polylines on the surface | Polyline layers; *Create Polyline from Geodesic Path*, *from Planar Section*; *Embed Polyline in Surface* (vcglib `CoMEmbed`); the GPU surface pick of the interactive tools | A tool that draws a polyline on the surface | Filter first, with the guidance polyline as a layer; the sketch tool later, as a front end that runs the filter |
| 1. Selection by geodesic distance | *Compute Geodesic Distance from Selection* (vcglib `Geodesic`, heat), *Select by Scalar*; disk test `PoissonSolver::IsFeasible` | Seeds on the polyline rather than on selected vertices | Embed the polyline (its vertices become seeds) in a copy, or seed the geodesic at the polyline's points |
| 2. Cross field from the polyline | vcglib `CrossField`: `PropagateFromSelF`, `SmoothIterative` with fixed faces, face `PD1`/`PD2` (in `VCGFace` since stage 4 of the TSP work) | The paper's harmonic map with the alignment term (eq. 1); `vcg::tri::Harmonic` solves scalar fields with hard vertex constraints only | *Open* (question 1): the paper's construction, or seeding the polyline's tangent on the faces it crosses and propagating it |
| 3. Field-aligned disk parametrization | vcglib `PoissonSolver` (Pietroni and Cignoni, October 2011), with `use_direction_field` reading the faces' `PD1`/`PD2` | Modern idioms: it reports with `printf`, checks with `assert`, keeps state in `std::map`s | Very likely the paper's own solver (question 2). Bring it to vcglib's current rules rather than write another |
| 4. Crop to a rectangle | vcglib `clip.h` (exact plane clipping, *Trim Surface by Plane*) | Clipping in UV | Clip a copy laid out in UV by the rectangle's four lines, exactly, then map back by barycentrics |
| Distortion (Table 1) | vcglib `Distortion`, the TexMetro measures | The paper's edge-length ratios as one result | Report min/max ratio and average error from the filter |
| 5. Height map | vcg `GridStaticPtr` ray casting (as `select_by_rectangle` uses), Embree optionally; raster layers (`QImage`, `Grayscale16`) | Sampling a geometry image, Gaussian smoothing, casting along normals | New, a few hundred lines in a plugin |
| 6. Ridges and valleys | Nothing for images | Gaussian derivative scale space, Lindeberg's conditions, tracing into polylines | New; the larger image-processing piece (question 5) |
| 7. Photos on the patch | *Transfer Color from Visible Rasters to Texture* on the patch layer | None | Works once the patch has UVs; document the workflow |
| 8. Curvature against the marks | Four curvature methods, per vertex and (stage 4) per face; *Colorize Texture by Principal Directions (Line Integral Convolution)* on the patch's texture | None once stage 4 lands | Document the workflow |

## Proposed stages

1. **The flattening filter.** *Parametrize Surface along Polyline (Local Flattening)*, name to be
   decided. Inputs: the current mesh, the guidance polyline (a `mesh` parameter naming a polyline
   layer), the patch's half-width (`absperc`), and the cross-field construction if question 1 keeps
   both. Output: a **new layer**, the rectangular patch, with per-wedge UVs in the surface's units
   (one UV unit is one mesh unit, so lengths read off the image), and the distortion figures as
   output values. Refuses a region that is not a disk. Mostly vcglib: geodesic, cross field,
   `PoissonSolver`, clipping. Tests: a cylinder (developable, so the patch must be isometric up to
   discretization), then a scan.
2. **The height map.** A filter from the patch layer and the full-resolution layer to a raster layer
   holding a 16-bit height image at a given resolution, with the smoothing scale σ; the height range
   as output values. Tests: a known bump function added to a smooth surface must come back.
3. **Chisel traces.** Ridges and valleys at a scale, as polyline layers: in the image's space, or
   carried back onto the 3D surface through the patch, colored by orientation. Grouping by similarity
   to a user-picked trace is left for later.
4. **Workflows** on existing filters: photos before and after a restoration, curvature directions
   against the traces; a page in the user documentation.
5. **The sketch tool.** Drawing the two polylines on the surface, as the paper does, built as a front
   end that runs stage 1's filter (one gesture, one filter run, one undo step).

## Open questions for Paolo

1. **The cross field.** Eq. 1 pulls the polyline's points to a constant U, which maps the polyline
   to a line parallel to the V axis, while the text says the patch is aligned with U. Which was meant,
   and was the field the gradient of U or of V? Would seeding the faces the polyline crosses with
   its tangent and propagating with `CrossField` give the same patches in practice? It needs no new
   solver.
2. **`PoissonSolver`.** Is vcglib's the solver of the paper? It fixes one vertex when following a
   field and two otherwise; was the patch's scale left to the field (unit-length directions, so
   lengths come out isometric), as `fieldScale` suggests?
3. **The interval polyline.** In a filter, a width is simpler than a second polyline. Was the
   interval ever used for anything but its length?
4. **Which model for which step.** The paper parametrizes a medium-resolution model and samples the
   full-resolution one. Two layers in MeshLab, or one layer simplified on the fly?
5. **Image processing.** Ridges and valleys need Gaussian derivatives on images, which MeshLab has
   nowhere. Write the small separable convolution needed, or bring in an image library? Is OpenCV
   acceptable as an optional dependency?
6. **Data.** The paper used the Pietà di Palestrina, the St Matthew and the David. Are any of those
   scans available for tests? `.reference/2007Delos_Afrodite` holds 1 GB of range maps of a
   sculpture, an alternative.
