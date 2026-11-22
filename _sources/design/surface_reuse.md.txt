# Surface reuse Python experiment

`python/examples/surface_reuse.py` implements a fixed-panel baseline inspired by
Baas, Bommes and Bousseau, *Shape Approximation by Surface Reuse* (SGP 2025,
[paper and project](https://repo-sam.inria.fr/circled/surface-reuse/)). It tests
whether existing sampling, alignment and distance filters can recover useful
rigid panels. It does not reproduce the paper's partition optimization.

## Running in MeshLab

Load the source bottle as layer **0** and the target as layer **1**, in the same
physical units. Open the script in the Python editor and run it. Its final call
is the configuration: initially 24 cells, two RANSAC restarts per piece, and a
distance tolerance of 0.5% of the target bounding-box diagonal. Prefer an explicit
absolute tolerance, e.g. `tolerance=0.5` for half a millimeter when coordinates
are in millimeters. There is no scale fitting or independent normalization.

The script uses NumPy and the existing Voronoi, geodesic, sampling, layer and
vcglib ICP filters; global placement also needs the Open3D plugin. It checks
availability first. `trials=0` skips Open3D and only refines the input pose,
which is useful for already aligned data but cannot discover remote placements.
In standalone pymeshlab2, import the file and call `surface_reuse(meshset, ...)`.

Every source piece is appended to the document as a `Source piece_...` layer
immediately after partitioning, including pieces that later find no acceptable
fit. These layers start hidden to avoid coincident faces over the original bottle;
toggle their visibility in the Layers panel to inspect them. Accepted placements
are appended as visible `Reuse ...` layers. Originals retain their geometry
and transforms. Taking world-space snapshots temporarily duplicates and freezes
each input in the document, so these operations appear in its action history.
The inputs' colors, scalar fields, materials and textures are not transferred.

The printed output directory holds `source.off`, `target.off`, every original
`piece_*.off`, the candidate `placed_*.off` files, and `report.json`. Only candidates
listed under `accepted` are chosen; other placed files are retained for diagnosis.
An explicit `output_dir` must be a new directory, preventing accidental overwrite.
The default is a newly created temporary directory, which is left on disk.

## Implemented pipeline

1. Require one connected source surface and generate relaxed Voronoi seeds.
   The existing filter exposes seeds through vertex selection, but not region
   labels. Run vcglib's geodesic-distance filter for each seed and assign each
   triangle to the seed with the lowest mean corner distance. This partitions
   the original triangles along their edges, without replacing curved cells
   with the coarse polygon mesh returned by the Voronoi filter. Split disconnected
   cell fragments through the existing connected-component filter.
2. For each piece, try its input pose and several seeded Open3D FPFH/RANSAC
   placements against the complete target. Refine global guesses with Open3D
   point-to-plane ICP, then finish with **rigid** vcglib ICP. No new registration
   implementation is included. Pieces with fewer
   than 30 vertices are reported as rejected: use finer input triangulation or
   fewer pieces. Use reasonably uniform source and target triangulations; FPFH
   operates on their vertices, and this experiment does not remesh automatically.
3. Measure unsigned point-to-triangle distances from **all panel vertices** and
   area-uniform Monte Carlo panel samples to the target. Reject a placement if
   any tested point exceeds tolerance. Use the per-vertex distance filter rather
   than a thresholded Hausdorff mean: the latter discards failed queries, which
   can make a mostly unmatched panel appear to fit. Misses from the chosen filter
   remain above tolerance. Rejected-trial distances can be censored at the search
   limit; accepted-trial RMS values are uncensored.
4. Measure target-to-panel distances at a common area-uniform target sample set.
   Greedily pick the candidate adding the most uncovered samples, breaking ties
   by lower panel RMS, subject to a sampled-overlap limit. A physical source piece
   is used at most once. Return sampled target coverage separately from fit error.
   This prevents multiple excellent fits to the same region from masquerading as
   good reconstruction, but is only a greedy heuristic.

## Relationship to the paper and limitations

The paper uses multiscale extrinsic descriptors, boundary-based registrations,
and a target labeling with fitting, boundary smoothness and label-count costs.
Panel boundaries are discovered after candidate registration; the method can
grow larger panels wherever the source continues to fit. Source regions may be
reused, requiring multiple copies of a bottle. This experiment fixes the source
panels first and uses each once, so it solves a stricter, simpler stock problem.

Fixed small cells are an inexpensive implementation of the local-reuse idea,
but too-small panels make almost any smooth surface match and hide fragmentation
cost. Smooth cylindrical or planar panels can give FPFH very weak evidence;
restarts do not guarantee distinct placements or the best match. Failed alignment
is not proof that a suitable placement does not exist. ICP is local, and its
inlier fit alone does not validate the whole panel. Normals should be consistent;
flipping panels is not explicitly searched.

Coverage and overlap are sampling estimates, not an exact tessellation or a
collision test. Near-boundary samples can count as overlapping even for adjacent
panels. Passing every tested point is not a continuous Hausdorff guarantee.
The output can contain gaps, geometric overlaps, awkward boundaries and many
small pieces. It does not trim, merge or grow panels, optimize cutting patterns,
or construct connectors. It is a matching experiment, not a fabrication result.

The code is cheap; execution need not be. Partitioning runs one geodesic solve
per seed, and matching runs `(trials + 1) * actual_piece_count` ICP attempts plus
RANSAC and sampling. Start with 8–24 cells and modest meshes. The script runs
synchronously in the embedded interpreter. Random seeds are passed throughout,
but backend/platform differences can still change registration results.

A next step closer to the paper would keep overlapping candidates at several
scales, then assign target regions by new covered area per panel, with boundary
and panel-count penalties. General-purpose new geometry operations should live
in vcglib; the Python script should continue to orchestrate filters.

## Validation

`python/tests/test_surface_reuse.py` checks recovery on identical surfaces,
preservation of input coordinates and total source face count, rejection of
out-of-range distance queries, and coverage selection without reusing a piece.
Run it against a built pymeshlab2 package with
`python -m pytest python/tests/test_surface_reuse.py -q`.

In the local Release build (2026-11-16), eight cells on identical subdivided
spheres passed the distance test; seven were selected, covering 90.2% of 500
target samples at tolerance 0.01. A deformed sphere with a displaced target,
eight cells, one global restart, 500 samples and tolerance 0.015 yielded four
distance-valid placements; three were selected, covering 39.4%. These are
synthetic sanity checks, not bottle-to-freeform reconstruction benchmarks. The
second example has exact matches available and still misses many of them, so
the registration search remains the main limitation.
