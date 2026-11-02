# Face scalar from rasters, applied 2026-11-01

How *Compute Face Scalar from Raster Coverage* became *Compute Face Scalar from Rasters*,
a measure of how well an aligned photo set covers each face. Asked for and approved on
2026-11-01. The filter's behaviour is documented in its own description in
`plugins/filter_img_patch_param/filters.json`; this records what there was, the decisions
and what was measured.

## 1. What there was

Three filters touched the question, none answered it:

| Filter | Gave | Missing |
|---|---|---|
| *Compute Face Scalar from Raster Coverage* | how many rasters see each face | a face counted as seen when **any one** of its three vertices was visible; only a count |
| *Compute Vertex Scalar from Camera* | angle or distance to the camera | the **current** raster only, no occlusion |
| *Transfer Color from Visible Rasters…* | per-raster weights by angle, distance, border | used to blend, never output |

Nothing gave the best resolution or the best angle over all the rasters that see a face.

## 2. Decisions

1. **Visibility.** A raster sees a face when at least two of three interior points pass the
   test (front side of the face, inside the image, depth buffer within `depthEpsilon`). The
   points are (2/3, 1/6, 1/6) and its permutations, the three-point quadrature rule of a
   triangle, halfway between the centroid and each vertex. Rejected: the vertices (they
   carry the silhouette's errors); the centroid alone (one sample decides a half-hidden face
   by chance); (0.8, 0.1, 0.1), which sits close to the vertices. Points are projected
   through the camera, not interpolated in the image, which would be wrong under
   perspective.
2. **Face only.** The vertex filter keeps its vertex test and its count.
3. **Rename** to mirror *Compute Face Scalar from Geometry*, which also picks its metric by
   a parameter. `quality` is not user-facing ([vocabulary](../vocabulary.md)).
4. **Measures.** Count; best resolution, √(projected area in px² / area on the mesh), a
   linear density in pixels per document unit that falls with distance and with
   foreshortening; best angle, in degrees, between the face normal and the direction to the
   camera. Unseen faces get 0, 0 and 180.
5. **Summary.** `outputValues`: `measure_min`, `measure_median`, `measure_max`,
   `measure_worst_5_percent` (the low tail of a count or a resolution, the high tail of an
   angle) over the seen faces, and `unseen_faces`, `faces_seen_once`.

## 3. Mapping table, as approved

| # | Current | Target | Evidence | Verdict |
|---|---|---|---|---|
| 1 | *Compute Face Scalar from Raster Coverage*, `compute_face_scalar_from_raster_coverage` | *Compute Face Scalar from Rasters*, `compute_face_scalar_from_rasters`, no alias | `filter_img_patch_param/filters.json`, `kCoverageFace`; `git grep`: no test or reference doc used the id | Rename |
| 2 | — | `measure`: `count` (default) / `bestResolution` / `bestAngle` | new | Add |
| 3 | `normalizeQuality` on the face filter | — | meaningful for a count only | Drop |
| 4 | any of 3 vertices visible | 2 of 3 interior points visible | `imgpatchparamfilterplugin.cpp`, face branch | Change |
| 5 | `depthEpsilon` | unchanged | — | Keep |
| 6 | *Compute Vertex Scalar from Raster Coverage* | unchanged | face only | Keep |
| 7 | `filter_organization.md`, plugin table row | new name | reference doc | Update |
| 8 | `filter_classification.md`, `filter_names.md`, `pass2_identifier_map.md` | untouched | history | Keep |

## 4. Measured on the gargoyle fixture

`tests/sample_mesh/gargoyle_small`, 20 000 faces, four photos at 750 × 502, bounding box
diagonal 198 units. `FilterTests::faceScalarFromRastersMeasuresTheViews`.

| Measure | min | median | max | worst 5% |
|---|---|---|---|---|
| Count | 1 | 1 | 3 | 1 |
| Best angle | 1.1° | 44.4° | 90.0° | 75.5° |
| Best resolution | 0.04 px/unit | 1.86 px/unit | 2.43 px/unit | 1.09 px/unit |

2 369 faces are seen by no photo and 13 222 by only one: no face is in all four, which
surround the statue. Scaling the scene and its cameras by 3 divides every resolution by 3.

**The depth tolerance is absolute, and the result depends on it strongly:**

| `depthEpsilon` | 0 | 0.1 | 0.5 (default) | 2 | 5 | 20 |
|---|---|---|---|---|---|---|
| unseen faces | 10 069 | 4 619 | 2 369 | 2 088 | 1 875 | 961 |

At 0 half the faces fail against their own depth buffer: the buffer holds depth
interpolated linearly in the image at pixel centres, and the point tested lies elsewhere in
the pixel, on a surface that is often slanted. The default 0.5 happens to be about one
pixel's footprint here (1 / 1.86 units). On a scan in meters it would be half the object.
Left unchanged, as decided; a tolerance in pixel footprints would make it scale-free.
