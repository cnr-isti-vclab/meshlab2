# Filter Name Proposal

Proposed display names aligned to the naming grammar in [Vocabulary](../vocabulary.md) §6:

```text
Verb Object [(Backend)]
```

**Status: display names APPLIED for `Meshing`** (30, 2026-07-30), **`Attribute`**
(64, 2026-08-04), **`Creation`** (38, 2026-08-05), **`Geometry`** (38, 2026-08-26)
**and `Selection`** (30, 2026-08-27).
Counts are as of each round; the archive has grown to 327 filters since, so a root's
current size exceeds what its round covered.
Any total below other than 327 names the dated registry audited during that round;
it is not a current implementation count.

Remaining roots, largest first: `Repair` (24), `Document` (22),
`Parametrization` (17), `Measurement` (14), `Transfer` (13), `Texture` (3 plus 7 filters
sitting at the bare `Texture` root, which needs a subcategory decision first).

Filters added to a root *after* its round are not automatically conformant. A sweep on
2026-08-26 found exactly one such drift (`Improve Triangulation (TrueForm)`, fixed in
round 4); eleven others flag only because they are dual-categorised into a root whose
round has not run yet, and will be renamed there.

Python names are shown too (`verb_object[_backend]`) because they should change in the
same breath, but they are **pass 2** — they are the scripting contract, so they land
separately from the display names.

## Rules being applied

1. **Lead with a canonical verb** — 61 % of current names do not.
2. **Never repeat the category** — the menu already says `Meshing/Simplification`, so
   `Simplification: Clustering Decimation` says it twice.
3. **Backend in parentheses only to disambiguate competing implementations** — required
   by the algorithm-archive model, not decoration.
4. **Title Case, no irregular capitals** — `Delete ALL Faces` → `Remove All Faces`.
5. **`Remove`, not `Delete`** — a settled ruling in the verb lexicon.

## Meshing/Boolean

Currently every name begins `Mesh Boolean:`, repeating the category. The result of a
boolean *is* a conventionally named object, so these are named for the result rather than
the action — the noun-phrase exception recorded in [Vocabulary](../vocabulary.md) §6.

| Current | Proposed | Python |
|---|---|---|
| Mesh Boolean: Union | **Mesh Union** | `mesh_union` |
| Mesh Boolean: Intersection | **Mesh Intersection** | `mesh_intersection` |
| Mesh Boolean: Difference | **Mesh Difference** | `mesh_difference` |
| Mesh Boolean: Symmetric Difference (XOR) | **Mesh Symmetric Difference** | `mesh_symmetric_difference` |

Keeping the nouns also means **no new boolean verbs are needed** — an earlier draft would
have added `Unite`/`Intersect`/`Subtract`/`Exclude` to the lexicon for these four filters
alone.

## Meshing/Deletion

Pure application of the `Remove`-not-`Delete` ruling, plus the casing rule.

| Current | Proposed | Python |
|---|---|---|
| Delete ALL Faces | **Remove All Faces** | `remove_all_faces` |
| Delete Selected Faces | **Remove Selected Faces** | `remove_selected_faces` |
| Delete Selected Vertices | **Remove Selected Vertices** | `remove_selected_vertices` |
| Delete Selected Faces and Vertices | **Remove Selected Faces and Vertices** | `remove_selected_faces_and_vertices` |

## Meshing/Remeshing

| Current | Proposed | Python |
|---|---|---|
| Remeshing: Isotropic Explicit Remeshing | **Remesh Isotropically** | `remesh_isotropically` |
| Uniform Mesh Resampling | **Remesh Uniformly by Volumetric Resampling** | `remesh_uniformly_by_volumetric_resampling` |
| Curvature flipping optimization | **Flip Edges by Curvature** | `flip_edges_by_curvature` |
| Planar flipping optimization | **Flip Edges by Planarity** | `flip_edges_by_planarity` |
| Cut mesh along crease edges | **Cut Along Crease Edges** | `cut_along_crease_edges` |

`Remesh Isotropically` drops "Explicit": there is no implicit variant to distinguish it
from, so the word only added length. It stays in the description.

*Remesh Uniformly by Volumetric Resampling* names the method because the filter really is
volumetric — it samples a signed distance field on a regular grid and reconstructs with
marching cubes. An earlier draft read *Resample Uniformly*, which broke the verb lexicon:
`Resample` is a **rejected** synonym for `Remesh` on surfaces. The canonical verb leads,
and "resampling" survives only as the method.

Despite the marching cubes, it stays `Meshing/Remeshing` rather than
`Creation/Reconstruction`: the discriminator is the *input*, and here the input is an
existing valid mesh, not unstructured data from which a surface must be inferred.

## Meshing/Quad

Moved out of `Remeshing` into their own subcategory (**applied**): converting between
triangle and quad representation is a distinct concern from improving element shape, and
this set is expected to grow.

| Current | Proposed | Python |
|---|---|---|
| Tri to Quad by 4-8 Subdivision | **Convert to Quads by 4-8 Subdivision** | `convert_to_quads_by_4_8_subdivision` |
| Tri to Quad by smart triangle pairing | **Convert to Quads by Triangle Pairing** | `convert_to_quads_by_triangle_pairing` |
| Turn into Quad-Dominant mesh | **Convert to Quad-Dominant Mesh** | `convert_to_quad_dominant_mesh` |
| Turn into a Pure-Triangular mesh | **Convert to Pure Triangles** | `convert_to_pure_triangles` |

## Meshing/Simplification

This is where the archive model bites: **three** quadric-edge-collapse implementations
coexist, so the backend suffix is load-bearing rather than decorative.

| Current | Proposed | Python |
|---|---|---|
| Simplification: Quadric Edge Collapse Decimation | **Simplify by Quadric Edge Collapse** | `simplify_by_quadric_edge_collapse` |
| Simplification: Original QSlim Quadric Edge Collapse | **Simplify by Quadric Edge Collapse (QSlim)** | `simplify_by_quadric_edge_collapse_qslim` |
| Simplification: Quadric Edge Collapse Decimation (with texture) | **Simplify by Quadric Edge Collapse with Texture** | `simplify_by_quadric_edge_collapse_with_texture` |
| Simplification: Clustering Decimation | **Simplify by Vertex Clustering** | `simplify_by_vertex_clustering` |
| Simplification: Edge Collapse for Marching Cube meshes | **Simplify Marching-Cubes Mesh by Edge Collapse** | `simplify_marching_cubes_mesh_by_edge_collapse` |
| Point Cloud Simplification | **Simplify Point Cloud** | `simplify_point_cloud` |

"Decimation" disappears throughout — it is a rejected synonym for `Simplify`, and
`Simplification: … Decimation` said the same thing twice. "Original" is dropped from the
QSlim entry: the parenthesised backend already carries that information.

## Meshing/Subdivision

| Current | Proposed | Python |
|---|---|---|
| Subdivision Surfaces: Loop | **Subdivide by Loop** | `subdivide_by_loop` |
| Subdivision Surfaces: LS3 Loop | **Subdivide by LS3 Loop** | `subdivide_by_ls3_loop` |
| Subdivision Surfaces: Butterfly Subdivision | **Subdivide by Butterfly** | `subdivide_by_butterfly` |
| Subdivision Surfaces: Midpoint | **Subdivide by Midpoint** | `subdivide_by_midpoint` |
| Subdivision Surfaces: Catmull-Clark | **Subdivide by Catmull-Clark** | `subdivide_by_catmull_clark` |
| Subdivision Surfaces: Doo Sabin | **Subdivide by Doo-Sabin** | `subdivide_by_doo_sabin` |
| Refine User-Defined | **Refine by User Expression** | `refine_by_user_expression` |

The set becomes perfectly regular: `Subdivide by <Scheme>`. *Doo Sabin* gains its
conventional hyphen. *Refine* is kept for the one genuinely adaptive, predicate-driven
case, which is exactly the exception the verb lexicon allows.

## Verbs added to the lexicon

Two, both now recorded in [Vocabulary](../vocabulary.md) §3:

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Flip` | Reconnect by flipping edges; connectivity changes, vertices do not move | A precise, well-known mesh operation; `Remesh` is too coarse for it |
| `Cut` | Split a surface along a curve, introducing a boundary | `Split` is claimed by layer operations, so reusing it here would be ambiguous |

## Rulings

1. ~~Should the triangle↔quad conversions have their own subcategory?~~ **Yes —
   `Meshing/Quad`, applied.** The ontology and the four descriptors are updated; the set
   is expected to grow.
2. ~~Is `Simplify by Quadric Edge Collapse with Texture` too long?~~ **Kept as proposed.**
3. ~~Verb or noun for the booleans?~~ **Noun**: `Mesh Union`, `Mesh Intersection`,
   `Mesh Difference`, `Mesh Symmetric Difference`. Recorded in the vocabulary as the
   named-result exception so they are not "corrected" back to verb-first.

Applied: all 30 display names. Verified against the 272-filter registry present on
2026-07-30: no old name survived as a filter name, no display names were duplicated,
JSON remained intact, and the build and tests were clean.

Note for whoever regenerates the docs: `docs/api/filters.md` is generated from the
descriptors; fix stale filter entries by running `--generate-docs`, not by hand-editing
individual generated sections.

---

# Round 2 — `Attribute` (65 filters → 64)

**Status: APPLIED** (2026-08-04). 56 names changed, 8 already conformed, 1 filter
removed as a duplicate. Verified mechanically against the 277-filter registry present
on 2026-08-04: JSON valid across all 31 descriptor files, no old name survived as a
filter name, no display name was duplicated, every proposed name was present, the
build was clean, and test results were unchanged from the pre-change
baseline (`FilterTests` 22/1, `FilterCreationTests` 19/0, `DocumentTests` 15/6 — the
failures are pre-existing and were confirmed identical with the changes stashed).

The largest root and the least regular: 61 of the 65 names do not lead with a verb, and
the same operation is spelled three different ways across plugins (`Re-Compute`,
`Compute`, bare noun). It is also where the `scalar`-not-`quality` ruling
([Vocabulary](../vocabulary.md) §4) first bites — 17 names say *Quality* today.

Three cross-cutting patterns do most of the work:

| Pattern | Applies to | Example |
|---|---|---|
| `Compute <Object> by Expression` | the 8 `filter_expression` entries | `Per Vertex Color Function` → **Compute Vertex Color by Expression** |
| `Sharpen <Object> by Unsharp Mask` | the 3 unsharp entries here (a 4th is in `Geometry`) | `UnSharp Mask Color` → **Sharpen Vertex Color by Unsharp Mask** |
| `Compute <Object> (<Backend>)` | competing curvature / geodesic implementations | `Discrete Curvatures` → **Compute Curvature (Discrete)** |

## Attribute/Normal (13)

| Current | Proposed | Python |
|---|---|---|
| Re-Compute Vertex Normals | **Compute Vertex Normals** | `compute_vertex_normals` |
| Re-Compute Face Normals | **Compute Face Normals** | `compute_face_normals` |
| Re-Compute Per-Polygon Face Normals | **Compute Polygon Face Normals** | `compute_polygon_face_normals` |
| Compute normals for point sets | **Compute Point Cloud Normals** | `compute_point_cloud_normals` |
| Per Vertex Normal Function | **Compute Vertex Normals by Expression** | `compute_vertex_normals_by_expression` |
| Per Face Normal Function | **Compute Face Normals by Expression** | `compute_face_normals_by_expression` |
| Normalize Vertex Normals | **Normalize Vertex Normals** | `normalize_vertex_normals` |
| Normalize Face Normals | **Normalize Face Normals** | `normalize_face_normals` |
| Re-Orient vertex normals using cameras | **Orient Vertex Normals by Cameras** | `orient_vertex_normals_by_cameras` |
| Reorient Face Normals by Geometry | **Orient Face Normals by Ray Casting** | `orient_face_normals_by_ray_casting` |
| Smooth Face Normals | **Smooth Face Normals** | `smooth_face_normals` |
| Smooth normals on point sets | **Smooth Point Cloud Normals** | `smooth_point_cloud_normals` |
| UnSharp Mask Normals | **Sharpen Face Normals by Unsharp Mask** | `sharpen_face_normals_by_unsharp_mask` |

`Re-Compute` → `Compute` is the casing rule in [Vocabulary](../vocabulary.md) §5 applied
literally; the "Re-" carried no information, since every one of these overwrites whatever
was there.

*Orient Face Normals by Ray Casting* keeps its method in the name because it competes
with `Repair/Topology`'s coherent-orientation filter, which works purely topologically.
The two are now visibly a pair rather than two unrelated names.

## Attribute/Scalar (20)

The `Quality` → `Scalar` root-and-branch pass. Note that in the geodesic family the word
disappears entirely rather than being replaced: the object of the sentence is the
*distance*, and the category already says the result is a scalar.

| Current | Proposed | Python |
|---|---|---|
| Compute Border Distance Quality | **Compute Geodesic Distance from Border** | `compute_geodesic_distance_from_border` |
| Compute Geodesic Distance Quality from Point | **Compute Geodesic Distance from Point** | `compute_geodesic_distance_from_point` |
| Compute Geodesic Distance Quality from Selection | **Compute Geodesic Distance from Selection** | `compute_geodesic_distance_from_selection` |
| Compute Heat Geodesic Distance Quality from Selection | **Compute Geodesic Distance from Selection (Heat Method)** | `compute_geodesic_distance_from_selection_heat` |
| Compute Ambient Occlusion | **Compute Face Ambient Occlusion** | `compute_face_ambient_occlusion` |
| — | **Compute Point Cloud Ambient Occlusion** | `compute_point_cloud_ambient_occlusion` |
| Compute Obscurance | **Compute Obscurance** | `compute_obscurance` |
| Compute Shape Diameter Function | **Compute Shape Diameter Function** | `compute_shape_diameter_function` |
| Generate Scalar Harmonic Field | **Compute Harmonic Scalar Field** | `compute_harmonic_scalar_field` |
| Per Face Geometric Quality | **Compute Face Scalar from Geometry** | `compute_face_scalar_from_geometry` |
| Per Face Texture Distortion | **Compute UV Distortion** | `compute_uv_distortion` |
| Quality from raster coverage (Vertex) | **Compute Vertex Scalar from Raster Coverage** | `compute_vertex_scalar_from_raster_coverage` |
| Quality from raster coverage (Face) | **Compute Face Scalar from Raster Coverage** | `compute_face_scalar_from_raster_coverage` |
| Vertex Quality from Camera | **Compute Vertex Scalar from Camera** | `compute_vertex_scalar_from_camera` |
| Per Vertex Quality Function | **Compute Vertex Scalar by Expression** | `compute_vertex_scalar_by_expression` |
| Per Face Quality Function | **Compute Face Scalar by Expression** | `compute_face_scalar_by_expression` |
| Clamp Vertex Quality | **Clamp Vertex Scalar** | `clamp_vertex_scalar` |
| Saturate Vertex Quality | **Clamp Vertex Scalar Gradient** | `clamp_vertex_scalar_gradient` |
| Smooth Vertex Quality | **Smooth Vertex Scalar** | `smooth_vertex_scalar` |
| UnSharp Mask Quality | **Sharpen Vertex Scalar by Unsharp Mask** | `sharpen_vertex_scalar_by_unsharp_mask` |

*Generate* → *Compute*: `Generate` is a rejected synonym for `Create`, and `Create` means
*produce a new layer*. This filter writes an attribute on the current mesh, so the verb
is `Compute`.

*Saturate Vertex Quality* is the one outright translation: nothing about it saturates
anything: it bounds the spatial gradient of the field so values cannot change faster than
a given rate. **Clamp Vertex Scalar Gradient** says what it does, and pairs it with the
filter directly above:

| | Bounds |
|---|---|
| `Clamp Vertex Scalar` | the values |
| `Clamp Vertex Scalar Gradient` | the rate of change of the values |

Caveat recorded so the name is not "corrected" later: the implementation is one-sided,
only ever decreasing values, so it is not a symmetric two-sided clamp. It does enforce
|∇q| ≤ threshold, which is a clamp of the gradient, and the description spells out the
conservative behaviour.

## Attribute/Curvature (4)

Three competing curvature estimators plus the directions filter. Backend in parentheses
is load-bearing here, exactly as in `Meshing/Simplification`.

| Current | Proposed | Python |
|---|---|---|
| Discrete Curvatures | **Compute Curvature (Discrete)** | `compute_curvature_discrete` |
| Compute APSS Curvature Quality | **Compute Curvature (APSS)** | `compute_curvature_apss` |
| Compute RIMLS Curvature Quality | **Compute Curvature (RIMLS)** | `compute_curvature_rimls` |
| Compute curvature principal directions | **Compute Principal Curvature Directions** | `compute_principal_curvature_directions` |

The three estimators become directly comparable in the menu, which is the point of the
archive model. Today they sort apart (`Compute APSS…`, `Compute RIMLS…`, `Discrete…`) and
read as unrelated filters.

## Attribute/Custom (4)

| Current | Proposed | Python |
|---|---|---|
| Define New Per Vertex Custom Scalar Attribute | **Define Custom Vertex Scalar Attribute** | `define_custom_vertex_scalar_attribute` |
| Define New Per Vertex Custom Point Attribute | **Define Custom Vertex Point Attribute** | `define_custom_vertex_point_attribute` |
| Define New Per Face Custom Scalar Attribute | **Define Custom Face Scalar Attribute** | `define_custom_face_scalar_attribute` |
| Define New Per Face Custom Point Attribute | **Define Custom Face Point Attribute** | `define_custom_face_point_attribute` |

Only `New` and `Per` are dropped, both redundant. `Define` is kept rather than converted
to the lexicon's `Set` — see ruling 2.

## Attribute/Color (24)

> **One filter removed, not renamed.** *Quality Mapper applier* was a duplicate of
> *Colorize by vertex Quality*: identical parameters, identical categories and
> input/output declarations, and literally the same code path (`kFilterMapVQuality ||
> kFilterQualityMapper` in one `if`). It survived as a MeshLab compatibility shim — in
> MeshLab it was driven by the Quality Mapper *dialog*, an editable transfer-function
> curve that QMeshLab never ported, so the filter was wired to the plain ramp and became
> a clone. Descriptor entry and dispatch branch deleted; no alias kept, since a
> consistent Python API for QMeshLab outranks pymeshlab script compatibility (ruling 5).
> A real transfer-function editor remains a legitimate future feature, and the name
> *Colorize Vertices by Scalar Transfer Function* is reserved for it.

The `Colorize` / `Set` split is the organising idea, and it resolves the *Colorize vs
Compute* confusion flagged at the end of round 1:

- **`Colorize`** — colour *derived from other data* (a scalar field, a distance, a noise
  field). Reading the mesh tells you something.
- **`Set`** — colour *assigned*, carrying no information about the geometry (a fixed
  colour, a random one per component).
- **`Adjust` / `Invert` / …** — colour *edited*, an image-processing operation on colour
  that is already there.

| Current | Proposed | Python |
|---|---|---|
| Colorize by vertex Quality | **Colorize Vertices by Scalar** | `colorize_vertices_by_scalar` |
| Colorize by face Quality | **Colorize Faces by Scalar** | `colorize_faces_by_scalar` |
| Perlin color | **Colorize Vertices by Perlin Noise** | `colorize_vertices_by_perlin_noise` |
| Disk Vertex Coloring | **Colorize Vertices by Disk Distance** | `colorize_vertices_by_disk_distance` |
| Voronoi Vertex Coloring | **Colorize Vertices by Voronoi Regions** | `colorize_vertices_by_voronoi_regions` |
| Per Vertex Color Function | **Compute Vertex Color by Expression** | `compute_vertex_color_by_expression` |
| Per Face Color Function | **Compute Face Color by Expression** | `compute_face_color_by_expression` |
| Vertex Color Filling | **Set Vertex Color** | `set_vertex_color` |
| Set Mesh Color | **Set Mesh Color** | `set_mesh_color` |
| Random Face Color | **Set Random Face Color** | `set_random_face_color` |
| Random Component Color | **Set Random Component Color** | `set_random_component_color` |
| PerMesh Color Scattering | **Set Random Layer Color** | `set_random_layer_color` |
| Vertex Color Brightness Contrast Gamma | **Adjust Vertex Color Brightness/Contrast/Gamma** | `adjust_vertex_color_brightness_contrast_gamma` |
| Vertex Color Levels Adjustment | **Adjust Vertex Color Levels** | `adjust_vertex_color_levels` |
| Vertex Color White Balance | **Adjust Vertex Color White Balance** | `adjust_vertex_color_white_balance` |
| Vertex Color Invert | **Invert Vertex Color** | `invert_vertex_color` |
| Vertex Color Desaturation | **Desaturate Vertex Color** | `desaturate_vertex_color` |
| Vertex Color Thresholding | **Threshold Vertex Color** | `threshold_vertex_color` |
| Vertex Color Colourisation | **Tint Vertex Color** | `tint_vertex_color` |
| Equalize Vertex Color | **Equalize Vertex Color** | `equalize_vertex_color` |
| Color noise | **Add Noise to Vertex Color** | `add_noise_to_vertex_color` |
| Smooth: Laplacian Vertex Color | **Smooth Vertex Color** | `smooth_vertex_color` |
| Smooth: Laplacian Face Color | **Smooth Face Color** | `smooth_face_color` |
| UnSharp Mask Color | **Sharpen Vertex Color by Unsharp Mask** | `sharpen_vertex_color_by_unsharp_mask` |

`PerMesh Color Scattering` → **Set Random Layer Color** applies the `mesh` vs `layer`
ruling: it iterates over document layers and gives each a distinct colour, so it is a
layer operation even though what it writes is a mesh attribute.

`Smooth: Laplacian …` drops the method: Laplacian is the only smoothing offered, and if a
second scheme ever lands it comes back as `Smooth Vertex Color (Taubin)`.

## Verbs added to the lexicon

Two new general verbs, plus a group of standard attribute-editing operations that are
better recorded as a set than as ten separate lexicon rows.

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Orient` | Make normals point consistently, or toward a reference | Distinct from `Compute` (values are flipped, not recalculated); also the canonical form of `Re-Orient`/`Reorient` |
| `Sharpen` | Enhance local variation in an attribute | The four unsharp-mask filters had no verb at all; `Sharpen X by Unsharp Mask` matches `Simplify by …` |

**Attribute-editing verbs** (standard image/signal operations, admitted as a closed
group): `Normalize`, `Adjust`, `Clamp`, `Invert`, `Equalize`, `Desaturate`, `Threshold`,
`Tint`. Each keeps its ordinary meaning; none may be used where `Compute` (derive from
geometry) or `Set` (assign a constant) is accurate.

`Define` is also admitted, but **narrowly**: declaring a new named custom attribute, and
nothing else. It is otherwise still a rejected synonym for `Set`.

## Duplicate sweep (2026-08-04)

Run before applying round 2, prompted by finding that *Quality Mapper applier* was a
clone. Five independent tests covered the 277 filters present on 2026-08-04:

| Test | What it looks for | Hits | True positives |
|---|---|---|---|
| A | Identical behavioural signature (parameters + domains + requirements + `outputModifies` + categories) | 12 groups | 0 |
| B | Two or more filter ids sharing one dispatch branch | 9 groups | 0 |
| C | Shared branch where the ids are **never re-tested inside** — the *Quality Mapper* signature | 1 | 0 |
| D | Cross-plugin name-token overlap ≥ 0.6 | 12 pairs | 0 (1 naming defect) |
| E | Duplicate `id`, `pythonName`, or display name | 0 | 0 |

**Result: no further clones. *Quality Mapper applier* was the only one.**

Test A is weak alone — filters with no parameters collide trivially (the five Platonic
solids, the four igl booleans, three no-parameter selections), as do families that differ
only in an internal constant (`Subdivide by Butterfly` / `by Midpoint`). Test C is the
sharp one, and its single hit is the APSS-vs-RIMLS surface factory in `filter_mls`, where
the three ids legitimately share "use APSS" and are discriminated at three later sites.

### Not duplicates, but defects found on the way

1. **`Create Selection Perimeter Polyline` has a wrong description.** It says the polyline
   is "composed by the selected edges of the current mesh"; the code walks *selected
   faces* and emits each edge whose neighbour is unselected. Its sibling
   `Build a Polyline from Selected Edges` is the one that reads the edge selection
   (`BuildFromFaceEdgeSel`). Two genuinely different filters whose descriptions claim the
   same input.
2. **Two indistinguishable names for two different algorithms** — the only real find of
   test D, and one to fix in the `Transfer` round rather than here:

   | Name | Plugin | Mechanism |
   |---|---|---|
   | *Transfer Color: Texture to Vertex* | `colorproc` | exact per-wedge UV lookup, same mesh only |
   | *Transfer: Texture to Vertex Color* | `texture` | closest-point resampling in world space, across two layers, with a distance bound |

   Neither is redundant: the first is the exact fast path, the second the only one that
   crosses layers. But the names differ by word order alone, which is unusable. The
   second needs its mechanism in the name.
3. **No filter can declare "requires a selection".** Both polyline filters check for a
   selection at runtime and fail with a message; the `inputRequirements` schema has keys
   for vertices, edges, faces, colour, texcoords, textures and quality, but none for
   selection state, so the UI cannot grey them out. A framework gap, noted not fixed.

## Rulings

All settled (2026-08-04).

1. ~~Are the slashes in `Adjust Vertex Color Brightness/Contrast/Gamma` acceptable?~~
   **Yes, kept.** The filter really does all three.
2. ~~`Define` or `Set` for custom attributes — or `Create`?~~ **`Define`, admitted
   narrowly.** `Create` was considered and rejected: it carries the invariant *a new
   layer appears in the layer list*, which holds exactly today — every filter named
   `Create *` declares `outputDomain: NewMeshes`, while all four custom-attribute
   filters are `ModifyCurrentMesh`. Spending `Create` here would break a rule that makes
   the menu readable, for the sake of four filters. `Set` was rejected because these
   declare a new named attribute rather than assign to an existing one.
3. ~~`Saturate` → `Limit … Gradient`?~~ **`Clamp Vertex Scalar Gradient`** — better than
   the proposal, since it reuses a lexicon verb instead of adding `Limit` and pairs the
   filter with `Clamp Vertex Scalar` directly above it.
4. ~~`Compute Curvature (Discrete)` demotes the familiar name into a parenthesis — worth
   the regularity?~~ **Yes, proceed.** Verified that the three are genuinely competing
   implementations of one quantity rather than different quantities sharing a word:

   | Filter | Method | Writes | Offers |
   |---|---|---|---|
   | Discrete Curvatures | discrete differential operators (Meyer et al.) | `VQ` | Mean / Gaussian / RMS / ABS |
   | Compute APSS Curvature Quality | algebraic point set surfaces (MLS) | `VQ` | Mean / Gauss / K1 / K2 / ApproxMean |
   | Compute RIMLS Curvature Quality | robust implicit MLS | `VQ` | Mean / Gauss / K1 / K2 |

   Same output slot, same quantity, overlapping type enums — exactly what the
   parenthesised backend exists for, and the same trade already accepted across
   `Meshing`. Search still finds "discrete" via the description.

   *Compute curvature principal directions* stays apart on merit: it is the only one
   writing `VA`, and its `Method` enum is five further algorithms (Taubin, PCA, Normal
   Cycles, Quadric Fitting, Scale-Dependent Quadric Fitting). Its unique output is the
   directions, so it is named for them.
5. ~~Keep MeshLab `pythonName`s as aliases when renaming?~~ **No.** A consistent Python
   API for QMeshLab outranks pymeshlab script compatibility. Pass 2 therefore renames all
   `pythonName`s outright, with no alias mechanism and no deprecation window. Recorded
   here because it removes a prerequisite that pass 2 would otherwise have needed.

---

# Round 3 — `Creation` (38 filters)

**Status: APPLIED** (2026-08-05). 37 names changed, 1 already conformed. Verified
mechanically against the 277-filter registry present on that date: JSON valid across
all descriptor files, no old name survived, no display name was duplicated, every
proposed name was present, the build was clean, and tests were unchanged
from baseline (`FilterTests` 22/1, `FilterCreationTests` 19/0, `DocumentTests` 15/6 —
all pre-existing).

Structurally the easiest root — three subcategories, each with an obvious canonical verb
(`Create`, `Reconstruct`, `Sample`) — but it contains the one genuinely contested
decision of the whole naming effort. See ruling 1.

## Creation/Primitives (22)

| Current | Proposed | Python |
|---|---|---|
| Annulus | **Create Annulus** | `create_annulus` |
| Box/Cube | **Create Box** | `create_box` |
| Cone | **Create Cone** | `create_cone` |
| Dodecahedron | **Create Dodecahedron** | `create_dodecahedron` |
| Dodecahedron (symmetric) | **Create Symmetric Dodecahedron** | `create_symmetric_dodecahedron` |
| Icosahedron | **Create Icosahedron** | `create_icosahedron` |
| Octahedron | **Create Octahedron** | `create_octahedron` |
| Sphere | **Create Sphere** | `create_sphere` |
| Sphere Cap | **Create Sphere Cap** | `create_sphere_cap` |
| Tetrahedron | **Create Tetrahedron** | `create_tetrahedron` |
| Torus | **Create Torus** | `create_torus` |
| Points on a Sphere | **Create Points on a Sphere** | `create_points_on_a_sphere` |
| — | **Create Points on a Spherical Cap** | `create_points_on_a_spherical_cap` |
| Grid Generator | **Create Grid** | `create_grid` |
| Implicit Surface | **Create Isosurface from Expression** | `create_isosurface_from_expression` |
| Noisy Isosurface | **Create Isosurface from Perlin Noise** | `create_isosurface_from_perlin_noise` |
| Fit Plane to Selection | **Create Plane from Selection** | `create_plane_from_selection` |
| Compute Planar Section | **Create Polyline from Planar Section** | `create_polyline_from_planar_section` |
| Build a Polyline from Selected Edges | **Create Polyline from Selected Edges** | `create_polyline_from_selected_edges` |
| Create Selection Perimeter Polyline | **Create Polyline from Selection Perimeter** | `create_polyline_from_selection_perimeter` |
| Create Solid Wireframe | **Create Solid Wireframe** | `create_solid_wireframe` |
| Voronoi Scaffolding | **Create Voronoi Scaffolding** | `create_voronoi_scaffolding` |

The **three polyline filters become one family**, distinguished by the input at the end
of the name. This is the direct fix for the confusion the duplicate sweep turned up: two
of them previously read as near-identical (*Build a Polyline from Selected Edges* /
*Create Selection Perimeter Polyline*) and their descriptions claimed the same input.

*Compute Planar Section* changes verb: it produces a new layer, so `Compute` — which the
lexicon defines as *calculate and store an attribute* — was simply the wrong word.

*Fit Plane to Selection* → *Create Plane from Selection* because `Fit` is a rejected
synonym for `Align`, and the filter's observable result is a new quad layer.

*Implicit Surface* / *Noisy Isosurface* become a visible pair: both build a scalar field
on a grid and run marching cubes, differing only in where the field comes from.

## Creation/Reconstruction (8)

`Surface Reconstruction:` repeats the category, so it goes; `Reconstruct` leads.

| Current | Proposed | Python |
|---|---|---|
| Surface Reconstruction: Screened Poisson | **Reconstruct Surface by Screened Poisson** | `reconstruct_surface_by_screened_poisson` |
| Surface Reconstruction: SSD | **Reconstruct Surface by Smooth Signed Distance** | `reconstruct_surface_by_smooth_signed_distance` |
| Surface Reconstruction: Ball Pivoting | **Reconstruct Surface by Ball Pivoting (vcglib)** | `reconstruct_surface_by_ball_pivoting_vcglib` |
| Surface Reconstruction: VCG | **Reconstruct Surface by Volumetric Merging** | `reconstruct_surface_by_volumetric_merging` |
| Alpha Wrap | **Reconstruct Surface by Alpha Wrapping** | `reconstruct_surface_by_alpha_wrapping` |
| Marching Cubes (APSS) | **Reconstruct Surface by Marching Cubes (APSS)** | `reconstruct_surface_by_marching_cubes_apss` |
| Marching Cubes (RIMLS) | **Reconstruct Surface by Marching Cubes (RIMLS)** | `reconstruct_surface_by_marching_cubes_rimls` |
| Surface Reconstruction: Surface Trimmer | **Trim Surface by Scalar Isovalue** | `trim_surface_by_scalar_isovalue` |

*Surface Reconstruction: VCG* was named for its **library**, which decision 3 forbids in
a category and which tells the user nothing here either. The algorithm is volumetric
merging of range maps, so that is what the name says; the backend parenthesis is dropped
because nothing competes with it.

*Surface Reconstruction: Surface Trimmer* is not a reconstruction at all — it is the
post-processing companion to Poisson. See ruling 3.

## Creation/Sampling (9)

Becomes fully regular: `Sample <what> by <method>`.

| Current | Proposed | Python |
|---|---|---|
| Montecarlo Sampling | **Sample Surface by Monte Carlo** | `sample_surface_by_monte_carlo` |
| Poisson-disk Sampling | **Sample Surface by Poisson Disk** | `sample_surface_by_poisson_disk` |
| Stratified Triangle Sampling | **Sample Surface by Stratified Triangles** | `sample_surface_by_stratified_triangles` |
| Voronoi Sampling | **Sample Surface by Voronoi Relaxation** | `sample_surface_by_voronoi_relaxation` |
| Clustered Vertex Sampling | **Sample Vertices by Clustering** | `sample_vertices_by_clustering` |
| Mesh Element Sampling | **Sample Mesh Elements** | `sample_mesh_elements` |
| Regular Recursive Sampling | **Sample Offset Surface Recursively** | `sample_offset_surface_recursively` |
| Texel Sampling | **Sample Texels** | `sample_texels` |
| Volumetric Sampling | **Sample Volume** | `sample_volume` |

`Montecarlo` gains its conventional space. Nothing else here is contentious: the noun
after `Sample` says what is sampled, the method follows `by`.

## Verbs added to the lexicon

One, and it is contested — see ruling 3.

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Trim` | Cut a surface along an isovalue of a scalar field and discard one side | Neither `Remove` (which deletes whole elements matching a predicate) nor `Cut` (which introduces a boundary but keeps both sides) describes it |

## Rulings

All settled (2026-08-05).

1. ~~Does every primitive need the word `Create`?~~ **Yes.** Fourteen filters were bare
   nouns, and rule 2 (*never repeat the category*) plus the named-result exception both
   argued for leaving them. `Create` wins because it carries an invariant — *a new layer
   appears in the layer list* — that held across the 277-filter registry audited on
   2026-08-05 and was the deciding argument in round 2 ruling 2, where `Create` was
   **refused** to the
   custom-attribute filters precisely to protect it. Dropping the verb from the filters
   that actually create layers would make the invariant invisible where it matters most.
   Two supporting reasons: the subcategory is not uniformly noun-able (7 of 21 are not
   named objects), and filters are reached by flat search and Python, where a bare
   `Sphere` does not say what will happen.
2. ~~Spell out `SSD`?~~ **Yes — `Reconstruct Surface by Smooth Signed Distance`.** Long,
   but parallel with *Screened Poisson* and it keeps the no-unexplained-abbreviations
   rule intact for the Python name.
3. ~~`Trim` as a new verb — and is the filter's behaviour really what the description
   claims?~~ **Verified in code, and `Trim` is admitted.**
   `runSurfaceTrimmerImpl` in `poissonrecon_backend.cpp`:

   - `splitPolygon(..., trimValue)` splits **every** polygon along the isovalue of vertex
     quality, creating **new vertices interpolated on the cut edges**;
   - only `gtPolygons` — the side above the threshold — is kept;
   - small disconnected islands are optionally dropped (`islandAreaRatio`,
     `removeIslands`);
   - the result is triangulated and the mesh rebuilt in place.

   So `Remove Faces by Scalar Threshold` would be actively wrong: vertices appear that
   were not in the input, and no whole face is tested against a predicate. `Remove`
   cannot describe this; `Cut` keeps both sides. `Trim` is the term of art and earns its
   lexicon entry.

   The filter stays in `Creation/Reconstruction`: it is meaningless except as the
   post-processing companion to Poisson-family reconstruction, and moving it away from
   the filters it exists to serve would help nobody.

## Reconstruction output domain: interpolating vs approximating

An earlier draft of this section flagged `Surface Reconstruction: Ball Pivoting` as
inconsistent for declaring `ModifyCurrentMesh` where every other reconstruction declares
`NewMeshes`. **That was wrong**, and the distinction it missed is worth recording as a
rule.

| Mechanism | Filters | `outputDomain` |
|---|---|---|
| **Approximating** — fit an implicit field and extract it, or build an offset surface. Output vertices are new. | Screened Poisson, Smooth Signed Distance, Volumetric Merging, Alpha Wrapping, Marching Cubes (APSS), Marching Cubes (RIMLS) | `NewMeshes` — 6/6 |
| **Interpolating** — the input points *are* the output vertices; only connectivity is added. | Ball Pivoting | `ModifyCurrentMesh` — 1/1 |

`vcg::tri::BallPivoting<VCGMesh> pivot(mesh, ...)` runs on the layer's own mesh and adds
faces to the existing vertex set, so `ModifyCurrentMesh` is not an oversight but the
honest declaration: nothing is destroyed, the point cloud is still there and now carries
a surface. Emitting a new layer would duplicate every point for no gain.

**Alpha Wrapping is approximating, despite the name.** Worth stating outright, because
alpha *shapes* are the opposite: an alpha complex (Edelsbrunner-Mucke) is a subcomplex of
the Delaunay triangulation of the input points, so its vertices *are* the input points — a
textbook interpolating reconstruction. CGAL's `alpha_wrap_3` is a different and much newer
algorithm (*Alpha Wrapping with an Offset*, Portaneri et al. 2022) that shares only the
Greek letter: it refines a 3D Delaunay triangulation with Steiner points and carves it,
and its `Offset` parameter **must be strictly positive**, so the output surface cannot
pass through the input points. Same letter, opposite side of the line.

### Alpha Wrap now accepts point clouds (fixed)

Found while checking the above. `cgalfilterplugin.cpp` called only the **triangle soup**
overload, and `buildTriangleSoup` failed with *"requires at least one valid triangular
face"* whenever the face list was empty; the descriptor declared `requireFaces: true` to
match. CGAL also provides a **point set** overload (`alpha_wrap_3(points, alpha, offset,
out)`, routing to `oracle.add_point_set`) which needs **no normals** — unlike Poisson,
the strictly positive offset defines the envelope by itself. The face requirement was
QMeshLab's wiring, not the algorithm's.

**Fixed** (2026-08-05): `buildTriangleSoup` now tolerates an empty face list, the call
site branches to the point-set overload, `requireFaces` is dropped, the info message
reports "Input point set: N points", and the description explains both paths. Covered by
`FilterTests::cgalAlphaWrapAcceptsPointClouds`, which strips faces through the real
`Remove All Faces` filter and then wraps the resulting cloud.

**The two paths are not equivalent, and the description now says so.** They select
structurally different CGAL oracles:

| | Triangle soup | Point set |
|---|---|---|
| Oracle | `Triangle_soup_oracle` | `Point_set_oracle` |
| AABB tree over | **triangles** (`AABB_triangle_primitive_3`) | **points** (`AABB_primitive` over `vector<Point_3>`) |
| `alpha` at construction | **yes** | no |
| Subdivides the input | **yes** — `AABB_tree_oracle_splitter`, `Splitter_base(alpha)` | no |

CGAL states the alpha coupling outright in `Triangle_soup_oracle`: *"the oracle will be
adapted to this particular 'alpha', and so when calling again AW3(other_alpha) the oracle
might not have performed a split that is adapted to this other alpha value."*

So with faces, triangle **interiors** are solid to the wrap and oversized faces are split
to the alpha scale; without faces the input is isolated points and the envelope comes from
the offset around each one. Where a triangle spans a wide gap the point-set path sees only
its three corners, and the ball can roll between them and dent or hole the result. The
point-cloud path is sound only when sampling is dense relative to `Alpha` and `Offset` —
if a layer has faces, keep them.

This also settles a classification doubt raised earlier in this document: an earlier note
here suggested Alpha Wrap might be mis-filed, since "its input is a mesh, not a point
cloud" — but that was an accident of the wiring. With point clouds accepted it is
unambiguously an unstructured-input-to-surface reconstruction, so
`Creation/Reconstruction` is correct and there is no case for moving it next to
`Repair Watertight Mesh (MeshFix)`. It remains **approximating** either way.

Ball Pivoting is currently the **only** interpolating reconstruction in the tree — there
is no Delaunay, convex hull, or advancing-front filter in `filter_cgal`, `filter_igl`,
`filter_meshfix` or `filter_clean`. The rule is recorded because that is exactly the
family an igl plugin would add:

> **Ball Pivoting is the only reconstruction that works *in place*, on the current
> mesh, optionally discarding the original face set. Every other reconstruction emits a
> new layer.**

That is the whole of it, and it is a **design choice about where the result goes**, not a
property of the algorithm. Two earlier drafts of this section tried to derive it from the
mathematics and both were wrong:

- *"interpolating -> `ModifyCurrentMesh`, approximating -> `NewMeshes`"* — MeshLab's
  convex hull breaks this immediately: interpolating, yet it must emit a new layer.
- *"preserves the entire input vertex set -> `ModifyCurrentMesh`"* — Ball Pivoting does
  not preserve it in any useful sense. It deletes nothing, but it does not connect
  everything either: `clustering` is passed to vcglib's `minr`, i.e. `min_edge`, a
  **minimum edge length**, so points nearer than that to an existing vertex are skipped,
  as are points the ball cannot reach. They survive as unreferenced vertices.

The interpolating/approximating distinction is still worth knowing (it is why Alpha
Wrapping never touches the input points, and it is the family the qhull filters belong
to), but it does **not** determine `outputDomain`. Nothing algorithmic does. A convex hull
could be computed in place too, by replacing the mesh; MeshLab simply chose not to.

The open design question, then, is a UX one rather than a classification one: should
reconstruction be uniform — always a new layer, so the input survives for comparison — or
is in-place Ball Pivoting worth keeping as the "give my point cloud a surface" workflow?
Left as-is for now; recorded so the inconsistency is a decision rather than an accident.

### Filters MeshLab has here and QMeshLab does not

MeshLab's `filter_qhull` provides four filters with no QMeshLab counterpart:
`FP_QHULL_CONVEX_HULL` (`generate_convex_hull`), `FP_QHULL_VORONOI_FILTERING`
(Amenta-Bern reconstruction), `FP_QHULL_ALPHA_COMPLEX_AND_SHAPE`, and
`FP_QHULL_VISIBLE_POINTS`. The first three are the interpolating-reconstruction family
whose absence is noted above; the fourth is a viewpoint-visibility filter and belongs with
`Selection/by Visibility` and `ViewpointOccluder`, not here.

MeshLab computes the hull with **Qhull**, but `vcglib/vcg/complex/algorithms/convex_hull.h`
already exists, so a port needs no new external dependency.

---

# Round 4 — `Geometry` (38 filters)

**Applied 2026-08-26.** 33 renamed, 5 already conformant. Python names are recorded for
pass 2 and were **not** applied.

## Geometry/Transform (12)

Ten of the twelve led with a colon prefix that restated the category.

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Matrix: Freeze Current Matrix | **Freeze Matrix** | `freeze_matrix` |
| Matrix: Invert Current Matrix | **Invert Matrix** | `invert_matrix` |
| Matrix: Reset Current Matrix | **Set Matrix to Identity** | `set_matrix_to_identity` |
| Matrix: Set from translation/rotation/scale | **Set Matrix from Translation/Rotation/Scale** | `set_matrix_from_trs` |
| Matrix: Set/Copy Transformation | **Set Matrix from Values or Layer** | `set_matrix_from_values_or_layer` |
| Transform: Align to Principal Axis | **Align to Principal Axes** | `align_to_principal_axes` |
| Transform: Flip and/or swap axis | **Mirror or Swap Axes** | `mirror_or_swap_axes` |
| Transform: Rotate | **Rotate** | `rotate` |
| Transform: Rotate to Fit to a plane | **Rotate to Fitted Plane** | `rotate_to_fitted_plane` |
| Transform: Scale, Normalize | **Scale** | `scale` |
| Transform: Translate, Center, set Origin | **Translate** | `translate` |
| Normalize To Unit Box | **Normalize to Unit Box** | `normalize_to_unit_box` |

## Geometry/Smoothing (13)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Laplacian Smooth | **Smooth Vertices by Laplacian** | `smooth_vertices_by_laplacian` |
| Laplacian Smooth (surface preserving) | **Smooth Vertices by Laplacian (Surface Preserving)** | `smooth_vertices_by_laplacian_surface_preserving` |
| HC Laplacian Smooth | **Smooth Vertices by HC Laplacian** | `smooth_vertices_by_hc_laplacian` |
| ScaleDependent Laplacian Smooth | **Smooth Vertices by Scale-Dependent Laplacian** | `smooth_vertices_by_scale_dependent_laplacian` |
| Taubin Smooth | **Smooth Vertices by Taubin** | `smooth_vertices_by_taubin` |
| TwoStep Smooth | **Smooth Vertices by Two-Step Normal Fitting** | `smooth_vertices_by_two_step_normal_fitting` |
| Depth Smooth | **Smooth Vertices along One Direction** | `smooth_vertices_along_one_direction` |
| Directional Geometry Preservation | **Project Vertices onto the Line of Sight** | `project_vertices_onto_line_of_sight` |
| UnSharp Mask Geometry | **Sharpen Vertices by Unsharp Mask** | `sharpen_vertices_by_unsharp_mask` |
| MLS projection (APSS) | **Project Vertices onto MLS Surface (APSS)** | `project_vertices_onto_mls_surface_apss` |
| MLS projection (RIMLS) | **Project Vertices onto MLS Surface (RIMLS)** | `project_vertices_onto_mls_surface_rimls` |
| Smooth Vertices by Laplacian (TrueForm) | *unchanged* | |
| Smooth Vertices by Taubin (TrueForm) | *unchanged* | |

Renaming the two vcg smoothers completes two competing sets whose TrueForm halves were
already in final form, under the convention the archive has settled into: **the incumbent
stays unsuffixed, the challenger carries the backend** (`Remove Duplicate Vertices` +
`… (TrueForm)`, `Simplify by Quadric Edge Collapse` + `… (QSlim)`).

## Geometry/Deformation (8)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Displace Vertices Randomly | *unchanged* | |
| Displace by Fractal Brownian Motion | **Displace Vertices by Fractal Brownian Motion** | `displace_vertices_by_fractal_brownian_motion` |
| Displace by Heterogeneous Multifractal Noise | **Displace Vertices by Heterogeneous Multifractal Noise** | `displace_vertices_by_heterogeneous_multifractal_noise` |
| Displace by Hybrid Multifractal Noise | **Displace Vertices by Hybrid Multifractal Noise** | `displace_vertices_by_hybrid_multifractal_noise` |
| Displace by Ridged Multifractal Noise | **Displace Vertices by Ridged Multifractal Noise** | `displace_vertices_by_ridged_multifractal_noise` |
| Displace by Standard Multifractal Noise | **Displace Vertices by Standard Multifractal Noise** | `displace_vertices_by_standard_multifractal_noise` |
| Per Vertex Geometric Function | **Compute Vertex Coordinates by Expression** | `compute_vertex_coordinates_by_expression` |
| Vertex Linear Morphing | **Displace Vertices toward Target Mesh** | `displace_vertices_toward_target_mesh` |

`Compute … by Expression` matches the six names ratified in round 2.

## Geometry/Alignment (5)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Align by Bounding Box | *unchanged* | |
| Align to Corresponding Points | *unchanged* | |
| Align by ICP (TrueForm) | *unchanged* | |
| ICP Between Meshes | **Align by ICP** | `align_by_icp` |
| Global Align Meshes | **Align Meshes Globally** | `align_meshes_globally` |

## Verbs added to the lexicon

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Mirror` | Negate one or more axes | `Flip` was given a single meaning in round 1 — reconnect by flipping edges, vertices do not move — which is the opposite of this |
| `Project` | Move vertices onto existing geometry, or onto a line | Admitted **narrowly**, for geometric projection only. It remains a rejected synonym for `Transfer` wherever attributes move between domains |

`Swap` is used as an ordinary word inside *Mirror or Swap Axes*, not as a lexicon verb.

## Rulings

All settled (2026-08-26).

1. ~~`Flip` for axis mirroring?~~ **No** — `Mirror` and `Swap` are better and leave
   `Flip` to edges.
2. ~~How much of a variant list belongs in a name?~~ **Trim to the bare verb.** The
   `Transform`/`Matrix` group is expected to be split later into pure `Translate`,
   `Rotate` and `Scale` plus a targeted canonicalizing filter, and the bare verbs are the
   foundation that split needs.
3. ~~`Normalize To Unit Box` overlaps `Scale`'s `unitFlag`.~~ **Both kept**, case fixed.
   It already centres and uniformly scales, so it is the seed of the future
   "normalize / make canonical" filter; the split moves `unitFlag` out of `Scale` into it.
4. ~~`Directional Geometry Preservation` — what is it?~~ Reading the implementation
   settled it: with `o` the stored position, `p` the current one and `d` the unit vector
   from the viewpoint to `o`, it computes `o + d·((p−o)·d)`. That is an orthogonal
   projection of the vertex onto its own sight line, keeping the along-view part of a
   previous smoothing and discarding the lateral part. **Project Vertices onto the Line of
   Sight.** The old description was wrong twice over — it said "blend" (there is no mixing
   factor) and "preservation" (every vertex moves); it has been rewritten.
5. ~~`Project` or route around it?~~ **Admitted**, narrowly — see the lexicon table.
6. ~~`Morph` as a verb?~~ **No.** `Displace` covers it, and "morphing" stays in the
   description so search still finds it.
7. ~~`Meshes` or `Layers`?~~ **Meshes**.
8. ~~Drift: `Improve Triangulation (TrueForm)`?~~ **Remesh by Edge Flipping (TrueForm)**,
   folded into this round. It was the only filter added after its own round that broke
   rule 1.

## Fixes made on the way

- `Project Vertices onto the Line of Sight` — description rewritten (see ruling 4).
- Both `viewPoint` parameters in `filter_unsharp` gained
  `"point3fDefaultPreset": "cameraEye"`. Both filters are defined by the viewer's
  position and neither offered the preset.
- Progress labels in `filter_icp`, `filter_mls` and `filter_trueform` followed their
  filters' new names; they are user-visible.

`docs/design/filter_classification.md` and `filter_organization.md` still carry the old
names. Both are dated snapshots of an audit, like the "Current" column above, and are left
alone on purpose. `docs/api/filters.md` is generated from the descriptors.

---

# Round 5 — `Selection` (30 filters)

**Applied 2026-08-27.** 12 renamed, 18 already conformant.

## Selection/Set Operations (5)

All five kept. `Dilate Selection`, `Erode Selection` and `Invert Selection` were already
right; the round's work here was admitting the two verbs (below) so the guard accepts
them.

## Selection/by Attribute (9)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Conditional Face Selection | **Select Faces by Expression** | `select_faces_by_expression` |
| Conditional Vertex Selection | **Select Vertices by Expression** | `select_vertices_by_expression` |
| Select Faces with Edges Longer Than... | **Select Faces by Edge Length** | `select_faces_by_edge_length` |
| Select by Face Quality | **Select Faces by Scalar** | `select_faces_by_scalar` |
| Select by Vertex Quality | **Select Vertices by Scalar** | `select_vertices_by_scalar` |
| Select Vertices Inside Mesh | **Select Vertices Inside Mesh (TrueForm)** | `select_vertices_inside_mesh_trueform` |
| Select Faces by Color · by View Angle · Select Outliers | *unchanged* | |

`by Expression` matches the six `Compute … by Expression` names ratified in round 2 —
the same muparser machinery, and the descriptor ids already said
`select_faces_by_condition`. The trailing `...` was a MeshLab dialog marker.

## Selection/by Topology (13)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Select non Manifold Edges | **Select Non-Manifold Edges (vcglib)** | `select_non_manifold_edges_vcglib` |
| Select non Manifold Vertices | **Select Non-Manifold Vertices** | `select_non_manifold_vertices` |
| Select Self Intersecting Faces | **Select Self-Intersecting Faces** | `select_self_intersecting_faces` |
| Select small disconnected component | **Select Small Disconnected Components** | `select_small_disconnected_components` |
| Select 'Problematic' Faces | **Select Ill-Shaped Faces** | `select_ill_shaped_faces` |
| the other 8 | *unchanged* | |

`Select non Manifold Edges` was the last bare incumbent in the archive — it escaped the
`(vcglib)` sweep only because its casing differed from its `(TrueForm)` counterpart.

## Selection/by Visibility (3)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Select by Rectangle (Screen) | **Select by Screen Rectangle** | `select_by_screen_rectangle` |
| Select Visible Faces · Select Visible Vertices | *unchanged* | |

`(Screen)` is neither a backend nor an algorithm, so under rule 3 it does not belong in
parentheses: it says what the filter *is*, and belongs in the name.

## Verbs added to the lexicon

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Dilate`, `Erode` | Grow or shrink the selection by one ring of adjacent elements | The standard morphological pair. `Select` names the act of selecting, not these two operations on a selection that already exists |

## Rulings

All settled (2026-08-27).

1. ~~Admit `Dilate`/`Erode`, or rename to `Expand`/`Shrink Selection`?~~ **Admit both.**
   The three existing names were already good; renaming them to avoid two lexicon rows
   would have been the wrong trade.
2. ~~`Select Vertices Inside Mesh` carried no backend suffix, alone among the TrueForm
   filters.~~ **Suffix added.**
3. ~~`Select 'Problematic' Faces`~~ → **Select Ill-Shaped Faces**. The scare quotes were
   doing the work of a definition; the filter selects elongated, flipped or folded faces.
   *Select Degenerate Faces* was rejected as confusable with the `Repair/Degenerate`
   subcategory, which means zero-area.

## Enforcement

`Selection` was added to `appliedRoots` in `displayNamesLeadWithALexiconVerb`, so the
round is now checked on every build.

# Round 6 — `Repair` (24 filters)

**Applied 2026-09-01.** 14 renamed, 10 already conformant. All 24 carry `Repair/*` as
their *primary* category, so nothing here was owned by another round.

## Repair/Degenerate (4)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Remove Isolated Pieces (wrt Diameter) | **Remove Isolated Components by Diameter** | `remove_isolated_components_by_diameter` |
| Remove Isolated Pieces (wrt Face Num.) | **Remove Isolated Components by Face Count** | `remove_isolated_components_by_face_count` |
| Remove Vertices wrt Quality | **Remove Vertices by Scalar** | `remove_vertices_by_scalar` |
| Remove Zero Area Faces | **Remove Zero-Area Faces** | *unchanged* |

`wrt` is an abbreviation the vocabulary does not carry, and rule 3 reserves parentheses
for the backend, so both `(wrt …)` become `by` connectors. `Pieces` → `Components`
follows round 5's *Select Small Disconnected Components*; `Quality` → `Scalar` is §4;
`Zero-Area` hyphenates as a compound adjective, like round 5's `Non-Manifold`.

## Repair/Duplicates (7)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Merge Wedge Texture Coord | **Merge Close Wedge UVs** | `merge_close_wedge_uvs` |
| Vertex Attribute Seam | **Split Vertices by Attribute Seam** | `split_vertices_by_attribute_seam` |
| the other 5 | *unchanged* | |

§4 makes `UV` the user-facing term and `Coord` was a bare abbreviation; the description
says "merge very close per-wedge texture coordinates", so restoring `Close` also puts the
name beside its sibling *Merge Close Vertices*.

## Repair/Holes and Borders (2)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Close Holes | *unchanged* — `Close` admitted to the lexicon | *unchanged* |
| Snap Mismatched Borders | **Repair Mismatched Borders** | `repair_mismatched_borders` |

The snap filter splits faces so two borders coincide and only welds vertices when
`unify_vertices` is set, so `Merge` would have over-promised. *Snap* stays in the
description.

## Repair/Topology (11)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Extract Outer Shell | **Extract Outer Shell (TrueForm)** | `extract_outer_shell_trueform` |
| Invert Faces Orientation | **Invert Face Orientation** | `invert_face_orientation` |
| Orient Faces Coherently (TrueForm) | **Orient Faces Consistently (TrueForm)** | `orient_faces_consistently_trueform` |
| Re-Orient all faces coherently | **Orient Faces Consistently (vcglib)** | `orient_faces_consistently_vcglib` |
| Repair Self-Intersections | **Repair Self-Intersections (TrueForm)** | `repair_self_intersections_trueform` |
| Repair non Manifold Edges | **Repair Non-Manifold Edges** | *unchanged* |
| Repair non Manifold Vertices by Splitting | **Repair Non-Manifold Vertices by Splitting** | `repair_non_manifold_vertices_by_splitting` |
| the other 4 | *unchanged* | |

`Re-Orient` is struck by §5 by name and listed as a rejected synonym in §3.
*Coherently* → *Consistently* is §3's own wording for `Orient`, and the vcglib filter's
`shortDescription` already said "Orient faces consistently" — only the display name
lagged. Neither non-manifold repair takes a suffix: nothing competes with them.

## Verbs added to the lexicon

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Close` | Fill a boundary loop with new faces | *Close a hole* is the term of art; `Repair` is too coarse to separate it from the four other `Repair/*` filters |

`Split` was **widened**, not added: it was scoped to layer-structure operations, and
`Cut`'s row claimed it outright for them. It now also covers separating mesh elements
that were sharing storage, which is what *Split Vertices by Attribute Seam* does.

## Rulings

All settled (2026-09-01).

1. ~~`Close Holes`: admit `Close`, or rename to `Repair Holes`?~~ **Admit `Close`.**
2. ~~`Vertex Attribute Seam` leads with no verb at all.~~ **Split Vertices by Attribute
   Seam**, with `Split` widened as above. *Convert to Seam-Independent Vertices* would
   have needed no lexicon change but read worse.
3. ~~Do the two unsuffixed TrueForm filters here take `(TrueForm)`?~~ **Every TrueForm
   filter takes it** — see below.

## The TrueForm suffix sweep

Ruling 3 came back wider than the round: 19 of 31 TrueForm filters were suffixed and 12
were not, so the suffix had stopped meaning "this is TrueForm" and started meaning "this
one happened to have a sibling". The other 10 were renamed with this round, across
already-applied roots (`Attribute`, `Creation`, `Geometry`, `Meshing`) and one pending
one (`Measurement`, whose names are still open to its own round):

*Compute Signed Distance to Mesh* · *Create Polyline from Self-Intersections* ·
*Create Polyline from Mesh Intersection* · *Create Polyline from Scalar Isocontour* ·
*Create Tube from Polyline* · *Align by Bounding Box* · *Align to Corresponding Points* ·
*Measure Chamfer Distance* · *Mesh CSG Expression* · *Cut Along Scalar Isocontour*

§6 records the amendment: the omission clause still governs a backend that is not a named
family the user chooses between.

## Fixes made on the way

- Two descriptions cross-referenced filters this round renamed (*Orient Faces Outward*
  pointed at *Orient Faces Coherently*, *Repair Self-Intersections* at *Extract Outer
  Shell*); both now name the current filter, suffix included.
- `beginFilterProgress` labels in the TrueForm plugin repeat the display name verbatim,
  so all 12 were updated with it; likewise the undo label and failure message of
  *Split Vertices by Attribute Seam* in the meshing plugin.
- Description vocabulary: five `Delete …` openings → `Remove` (§3 rejects `Delete`
  outright), two `quality` → `scalar`, three `texture coord(inate)s` → `UVs`, two
  British `neighbouring` → `neighboring`, one `non Manifold` → `non-manifold`, and the
  typo `trheshold` in *Close Holes*.

## Enforcement

`Repair` was added to `appliedRoots` in `displayNamesLeadWithALexiconVerb`. Verified to
discriminate: restoring *Snap Mismatched Borders* fails the guard with
`leading word 'Snap' is not in the lexicon`.

# Round 7 — `Document` (22 filters)

**Applied 2026-09-01.** 18 renamed, 4 already conformant. All 22 carry `Document/*` as
their primary category. 17 of the 22 led with a word outside the lexicon — by some margin
the least conformant root, because none of it had been touched since the MeshLab import.

## Document/Camera (10)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Export active rasters cameras to file | **Export Cameras from Visible Rasters** | `export_cameras_from_visible_rasters` |
| Import cameras for active rasters from file | **Import Cameras to Visible Rasters** | `import_cameras_to_visible_rasters` |
| Generate Camera from Direction | **Set Camera from Direction** | *unchanged* |
| Generate Camera to View Selection | **Set Camera to View Selection** | *unchanged* |
| Transform camera extrinsics | **Transform Camera Extrinsics** | `transform_camera_extrinsics` |
| Transform: Rotate Camera or set of cameras | **Rotate Cameras** | `rotate_cameras` |
| Transform: Scale Camera or set of cameras | **Scale Cameras** | `scale_cameras` |
| Transform: Translate Camera or set of cameras | **Translate Cameras** | `translate_cameras` |
| Set Mesh Camera · Set Raster Camera | *unchanged* | |

`Generate` is a rejected synonym for `Create`, but neither verb applied: these assign the
current view rather than producing a layer, and the ids already said `set_camera_…`. The
three `Transform:` names carried the colon form the guard rejects outright;
`Geometry/Transform` ships bare `Rotate`/`Scale`/`Translate`, so the camera set needs only
the object its own category does not supply.

## Document/Layer (11)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Delete Current Mesh | **Remove Current Mesh Layer** | `remove_current_mesh_layer` |
| Delete Current Raster | **Remove Current Raster** | `remove_current_raster` |
| Delete all non visible Mesh Layers | **Remove Hidden Mesh Layers** | `remove_hidden_mesh_layers` |
| Delete all Non Selected Rasters | **Remove Hidden Rasters** | `remove_hidden_rasters` |
| Duplicate Current layer | **Duplicate Current Layer** | *unchanged* |
| Flatten Visible Layers | **Merge Visible Layers** | `merge_visible_layers` |
| Move selected faces to another layer | **Extract Selected Faces** | `extract_selected_faces` |
| Move selected vertices to another layer | **Extract Selected Vertices** | `extract_selected_vertices` |
| Rename Current Mesh | **Rename Current Mesh Layer** | `rename_mesh_layer` |
| Rename Current Raster | *unchanged* | |
| Split in Connected Components | **Split into Connected Components** | `split_into_connected_components` |

`Delete` → `Remove` is settled by §3. "Mesh Layer" appears only where a raster
counterpart exists, so each pair disambiguates itself; `Duplicate`, `Merge` and `Split`
have no raster twin and stay plain "Layer".

`Flatten` is a rejected synonym (for `Transfer` and `Parametrize`) and the operation is a
merge. `MergeVisible` defaults to true, so *Merge Visible Layers* names the default
honestly while the parameter widens it; *flatten* stays in the description.

`Move` is rejected in favour of `Translate` and would read as moving geometry through
space. These produce a new layer from part of an existing one, which is `Extract` — and
because `Extract` implies the new layer, "to another layer" drops out entirely. The
`DeleteOriginal` parameter still chooses move-versus-copy.

## Document/Render (1)

*Render from Render-State JSON* — unchanged; `Render` admitted below.

## Verbs added to the lexicon

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Import`, `Export` | Read or write data in a file outside the document | Nothing covered file I/O. `Transfer` is defined over domains and layers; `Load`/`Save` are the File menu's document-level operations, and a filter writing a camera rig — or a multiresolution build — beside the mesh is not saving the document |
| `Rename` | Change the label of a layer or raster | `Set Mesh Name` would have needed no new verb and matched the ids, but *rename* is the word a user searches for |
| `Render` | Produce an image from the scene | `Create` produces a layer, not an image; nothing else was close |

## Rulings

All settled (2026-09-01).

1. ~~File I/O has no verb: `Export`/`Import`, or `Save`/`Load`?~~ **`Export`/`Import`**,
   which also covers the planned Nexus generation filters.
2. ~~`Rename`: admit it, or use `Set … Name`?~~ **Admit `Rename`.**
3. ~~`Render`.~~ **Admitted narrowly.**

## Fixes made on the way

- *Delete all Non Selected Rasters* did not delete unselected rasters: it tests
  `!doc.raster(i).visible`, so it removes **hidden** ones. The name, the description and
  both of its runtime messages all repeated the same wrong claim; all four corrected.
- Nine descriptions still opened with `Delete …` or described the old verb (*move or
  duplicate*, *flatten*); rewritten to match the filters they document, keeping *flatten*
  as a searchable synonym.
- Two prose references in code — a comment in `Document::runFilterOnVisibleMeshes` and one
  in the layer-index invariant test — named filters this round renamed.

## Enforcement

`Document` was added to `appliedRoots`. Verified to discriminate: restoring
*Flatten Visible Layers* fails the guard with
`leading word 'Flatten' is not in the lexicon`.

# Round 8 — `Parametrization` (17 filters)

**Applied 2026-09-01.** All 17 renamed — the only round in which nothing was already
conformant. Two further filters carry `Parametrization` as a *secondary* category
(*Select Vertex Texture Seams*, *Pack Texture Images*) and belong to their own rounds.

The round's shape is one decision: **`Parametrize` owns UV creation outright.** The verb
had been in the lexicon since the ontology was written and had no users at all — thirteen
filters produced UVs under five different leading words. All thirteen now lead with it.

## Parametrization/UV Creation (13)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Compute As-Rigid-As-Possible Parametrization (libigl) | **Parametrize by As-Rigid-As-Possible (libigl)** | `parametrize_by_as_rigid_as_possible_libigl` |
| Compute Locally Injective Parametrization (SLIM) | **Parametrize by SLIM (libigl)** | `parametrize_by_slim_libigl` |
| Harmonic Parametrization | **Parametrize by Harmonic Map (libigl)** | `parametrize_by_harmonic_map_libigl` |
| Least Squares Conformal Maps Parametrization | **Parametrize by Least Squares Conformal Maps (libigl)** | `parametrize_by_least_squares_conformal_maps_libigl` |
| Geometric Cylindrical Unwrapping | **Parametrize by Cylindrical Projection** | `parametrize_by_cylindrical_projection` |
| Parameterization from registered rasters | **Parametrize from Registered Rasters** | `parametrize_from_registered_rasters` |
| Parameterization + texturing from registered rasters | **Parametrize from Registered Rasters with Texture** | `parametrize_from_registered_rasters_with_texture` |
| Parametrization: Flat Plane | **Parametrize by Flat Plane** | `parametrize_by_flat_plane` |
| Parametrization: Trivial Per-Triangle | **Parametrize by Trivial Per-Triangle Layout** | `parametrize_by_trivial_per_triangle_layout` |
| Parametrization: Voronoi Atlas | **Parametrize by Voronoi Atlas (vcglib)** | `parametrize_by_voronoi_atlas_vcglib` |
| Parametrization: xatlas | **Parametrize by Atlas (xatlas)** | `parametrize_by_atlas_xatlas` |
| Per Vertex Texture Function | **Parametrize per Vertex by Expression** | `parametrize_per_vertex_by_expression` |
| Per Wedge Texture Function | **Parametrize per Wedge by Expression** | `parametrize_per_wedge_by_expression` |

Four `Parametrization:` colon names, one rejected `Unwrap`, two rejected `Parameterization`
spellings (§4), two verbless noun phrases, and four leading with `Compute` — legal in
itself, but not when a verb exists for exactly this attribute.

`Voronoi Atlas` takes `(vcglib)`: it is `vcg/…/parametrization/voronoi_atlas.h`, and it
competes with xatlas as a route to an atlas, which is when rule 3 says both name
themselves.

Method names follow the literature rather than a house style: *As-Rigid-As-Possible* and
*Least Squares Conformal Maps* read naturally expanded, *SLIM* does not — nobody writes
*Scalable Locally Injective Mappings* in full.

## Parametrization/UV Conversion (2)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Convert PerVertex UV into PerWedge UV | **Convert Per-Vertex UV to Per-Wedge UV** | `convert_per_vertex_uv_to_per_wedge_uv` |
| Convert PerWedge UV into PerVertex UV | **Convert Per-Wedge UV to Per-Vertex UV** | `convert_per_wedge_uv_to_per_vertex_uv` |

`PerVertex` is the irregular internal capital §5 names by example; `per-vertex` is the §4
qualifier. `into` → `to`, the ratified connector.

## Parametrization/Defragmentation (2)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Small Islands Remover | **Merge Texture Islands** | `merge_texture_islands` |
| Texture Map Defragmentation | **Defragment Texture Atlas** | `defragment_texture_atlas` |

Both were verbless noun phrases. *Small Islands Remover* removes nothing: its own
description says it merges small islands into neighbours that share a seam.

**Revised 2026-09-08.** *Merge Small Texture Islands* dropped the *Small*: the filter now
takes the islands to merge either from a size threshold or from the face selection, so which
islands go is a parameter and no longer part of what the filter is. `Merge Texture Islands`.

## Verbs added to the lexicon

| Verb | Means | Why the existing lexicon was not enough |
|---|---|---|
| `Defragment` | Reduce the fragmentation of a texture atlas by merging compatible charts and resampling | The term of the paper the filter implements (Maggiordomo et al. 2021, already cited in the descriptor). `Merge` covers only half of it — it also resamples — and `Pack` means arranging charts, not merging them |

`Parametrize` was **not** added: it was already ratified and simply unused.

## A connector added

`with` joins `by`, `from` and `to`, for an *additional output* a filter also produces:
*Parametrize from Registered Rasters with Texture* beside *Parametrize from Registered
Rasters*. §6 records that it is not a fifth way of saying `by` — it is for the case where
the alternative is a second verb.

## Rulings

All settled (2026-09-01).

1. ~~Does `Parametrize` own UV creation, or may `Compute` keep the four libigl filters?~~
   **`Parametrize` owns it outright**, the two expression filters included.
2. ~~Admit `Defragment`?~~ **Yes.**
3. ~~*Parametrize and Transfer Texture from Registered Rasters* or *… with Texture*?~~
   **`with Texture`**, which is what promoted `with` to a connector.

## Fixes made on the way

- Two libigl messages had the display name substituted into them where the *method* name
  was the correct noun ("Parametrize by Least Squares Conformal Maps (libigl) failed.");
  reworded to name the method.
- Progress labels in the libigl, texture, xatlas and defragmentation plugins repeat the
  display name verbatim and were updated with it.
- `plugins/filter_texture_defragmentation/upstream/` is a vendored third-party tree and
  was deliberately left untouched, though it contains six occurrences of the old name.

## Enforcement

`Parametrization` was added to `appliedRoots`. Verified to discriminate: restoring
*Small Islands Remover* fails the guard with
`leading word 'Small' is not in the lexicon`.

# Round 9 — `Measurement` (14 filters)

**Applied 2026-09-01.** 13 renamed, 1 already conformant. One further filter carries
`Measurement` as a *secondary* category (*Create Plane from Selection*) and belongs to
its own round.

The round turns on a distinction §3 already draws and nothing had enforced: **`Measure`
reports values without modifying, `Compute` calculates and stores an attribute**.
`outputModifies` settles it filter by filter, and exactly one of the fourteen stores
anything.

## Measurement/Geometric (6)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Compute Area/Perimeter of Selection | **Measure Selection Area and Perimeter** | `measure_selection_area_and_perimeter` |
| Compute Geometric Measures | **Measure Geometric Properties** | `measure_geometric_properties` |
| Hausdorff Distance | **Measure Hausdorff Distance** | `measure_hausdorff_distance` |
| Overlapping Meshes | **Measure Layer Overlap** | `measure_layer_overlap` |
| Distance from Reference Mesh | **Compute Distance from Reference Mesh** | `compute_distance_from_reference_mesh` |
| Measure Chamfer Distance (TrueForm) | *unchanged* | |

*Distance from Reference Mesh* is the one `Compute`: it declares `outputModifies: ["VQ"]`
and writes the distance into vertex quality. *Hausdorff Distance* reports statistics and
produces sample layers only as an optional by-product, so it stays `Measure`.

The `/` in *Area/Perimeter* was never a connector, and rewriting that name removed the
last real use of `of` in any shipped name — the only other occurrence sits inside the
fixed phrase *line of sight*. No new connector was needed for it.

## Measurement/Statistics (5)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Estimate radius from density | **Estimate Radius from Density** | `estimate_radius_from_density` |
| Per Face Quality Histogram | **Measure Face Scalar Histogram** | `measure_face_scalar_histogram` |
| Per Face Quality Stat | **Measure Face Scalar Statistics** | `measure_face_scalar_statistics` |
| Per Vertex Quality Histogram | **Measure Vertex Scalar Histogram** | `measure_vertex_scalar_histogram` |
| Per Vertex Quality Stat | **Measure Vertex Scalar Statistics** | `measure_vertex_scalar_statistics` |

`Quality` → `Scalar` is §4; `Stat` is an abbreviation §5 does not carry.

## Measurement/Topological (3)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Compute Topological Measures | **Measure Topological Properties** | `measure_topological_properties` |
| Compute Topological Measures for Quad Meshes | **Measure Topological Properties for Quad Mesh** | `measure_topological_properties_for_quad_mesh` |
| Current Mesh Info | **Measure Mesh Summary** | `measure_mesh_summary` |

`Info` is a rejected synonym of `Measure`. "Measures" could not survive beside the verb,
so the pair became "Properties"; keeping the two parallel is what promoted `for` to a
connector rather than splitting them into differently shaped names.

## A defect the round exposed

**The vocabulary permitted `Estimate` and the guard rejected it.** §3 admitted it in the
footnote below the table — *"permitted only when the result is explicitly statistical or
approximate"* — and that footnote's own worked example is *Estimate Radius from Density*,
this exact filter. But the ratified set is defined as everything **above** that footnote,
which is where round 6's guard truncates, so the parser saw 55 verbs and `Estimate` was
not one of them.

Doc and enforcement had disagreed since the truncation landed, and nothing caught it
because no shipped filter led with `Estimate`. It now has a table row of its own, above
the footnote; the footnote stays, because it is also the marker the guard truncates on.

## Connector added

`for` names the class of input a specialised variant is built for, distinguishing it from
the general filter: *Measure Topological Properties for Quad Mesh* beside *Measure
Topological Properties*. Like `with` in round 8, it arrived with exactly one user.

## Rulings

All settled (2026-09-01).

1. ~~"Properties" or "Geometry"/"Topology" as the object?~~ **Properties.**
2. ~~The quad variant breaks the parallel with its sibling.~~ **`for Quad Mesh`**, which
   admitted `for`.

## Enforcement

`Measurement` was added to `appliedRoots`. Verified to discriminate: restoring
*Current Mesh Info* fails the guard with
`leading word 'Current' is not in the lexicon`.

# Round 10 — `Transfer` (13 filters)

**Applied 2026-09-01.** All 13 renamed, and the three subcategories renamed and
remembered with them. Nine names carried the `Verb:` colon form the guard rejects
outright, three led with `Project`, and one was a verbless noun phrase.

Every name collapses onto one shape: **`Transfer <attribute> from <source> to <target>`**.

## Transfer/From Rasters (3, was `Raster to Mesh`)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Project Current Raster Color to Current Mesh | **Transfer Color from Current Raster to Vertex** | `transfer_color_from_current_raster_to_vertex` |
| Project Active Rasters Color to Current Mesh | **Transfer Color from Visible Rasters to Vertex** | `transfer_color_from_visible_rasters_to_vertex` |
| Project Active Rasters Color to Current Mesh Texture | **Transfer Color from Visible Rasters to Texture** | `transfer_color_from_visible_rasters_to_texture` |

No ruling was needed: §3 settles it outright — *"`Transfer`, not `Project`. `Project`
described only the raster sub-case."* `Project` was later readmitted, but only for moving
*vertices* onto geometry, and these move colour. "Active" → "Visible" follows the
descriptions, which already said "all visible rasters".

## Transfer/Within a Mesh (7, was `Mesh to Mesh`)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Transfer Color: Vertex to Face | **Transfer Color from Vertex to Face** | `transfer_color_from_vertex_to_face` |
| Transfer Color: Mesh to Face | **Transfer Color from Mesh to Face** | `transfer_color_from_mesh_to_face` |
| Transfer Color: Face to Vertex | **Transfer Color from Face to Vertex** | `transfer_color_from_face_to_vertex` |
| Transfer Color: Texture to Vertex | **Transfer Color from Texture to Vertex** | `transfer_color_from_texture_to_vertex` |
| Transfer Quality: Vertex to Face | **Transfer Scalar from Vertex to Face** | `transfer_scalar_from_vertex_to_face` |
| Transfer Quality: Face to Vertex | **Transfer Scalar from Face to Vertex** | `transfer_scalar_from_face_to_vertex` |
| Transfer: Vertex Color to Texture | **Transfer Color from Vertex to Texture** | `transfer_color_from_vertex_to_texture` |

`Quality` → `Scalar` is §4; the ids already said `compute_scalar_transfer_…`, so only the
label had lagged.

## Transfer/Between Layers (3, was `Attribute to Texture`)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Vertex Attribute Transfer | **Transfer Vertex Attributes by Closest Point** | `transfer_vertex_attributes_by_closest_point` |
| Transfer: Texture to Vertex Color | **Transfer Color from Texture to Vertex by Closest Point** | `transfer_color_from_texture_to_vertex_by_closest_point` |
| Transfer: Vertex Attributes to Texture | **Transfer Vertex Attributes to Texture by Closest Point** | `transfer_vertex_attributes_to_texture_by_closest_point` |

## The subcategories now name what supplies the correspondence

The old split mixed axes — `Mesh to Mesh` beside `Attribute to Texture` — and six of the
seven filters under `Mesh to Mesh` never went mesh to mesh at all: they moved an attribute
between elements of a *single* mesh. The distinction that actually matters is whether a
correspondence has to be constructed:

| Subcategory | What supplies the correspondence |
|---|---|
| `Within a Mesh` | The mesh's own incidence. A face already knows its vertices; nothing is chosen and nothing can be wrong |
| `Between Layers` | A computed match between two layers — closest point on the source surface, bounded by `upperBound` |
| `From Rasters` | The raster's camera |

Membership follows the parameter signature, not the name: exactly three filters carry
`sourceMesh`/`targetMesh`/`upperBound`, and those three are `Between Layers`. Two of them
had been filed under `Attribute to Texture`, and *Transfer: Vertex Color to Texture* — same
mesh, no source/target pair — had been filed with them; all three moved.

`by Closest Point` names that mechanism on the three cross-layer filters. It is the
standard term for what they do, it explains what `upperBound` bounds, and it avoids
overloading `Project`, which §3 keeps for moving vertices onto geometry.

"Domain" was considered and rejected for the subcategory names: §2 already rules that
`inputDomain`/`outputDomain` mean *document scope*, so the word is spoken for. "Between
Elements" fails too, because a texture is not one of §4's elements.

## Rulings

All settled (2026-09-01).

1. ~~How to mark the cross-layer three?~~ **`by Closest Point`**, over `from Another
   Layer`: less short, more true, and it names the mechanism rather than the arity.
2. ~~`Mesh to Mesh` misdescribes its contents.~~ **Subcategories renamed** to
   `Within a Mesh` / `Between Layers` / `From Rasters`, and three filters re-homed.
   `Same Mesh` was the runner-up to `Within a Mesh`; the prepositional form pairs with
   `Between Layers` as an inside/outside pair, which `Same Mesh` does not.

## Enforcement

`Transfer` was added to `appliedRoots`. Verified to discriminate: restoring
*Vertex Attribute Transfer* fails the guard with
`leading word 'Vertex' is not in the lexicon`.

# Round 11 — `Texture` (3 filters)

**Applied 2026-09-01.** 1 renamed, 2 already conformant. The last round of pass 1.

Eight further filters carry a bare `Texture` as a *secondary* category — the raster
transfers, the defragmentation pair, *Pack UV Charts*, *Simplify by Quadric Edge Collapse
with Texture (vcglib)* — and were renamed by their own rounds.

## Texture/Conversion (1)

| Current | Proposed | Python (pass 2) |
|---|---|---|
| Convert: Object-Space Normal Map to Tangent-Space | **Convert Object-Space Normal Map to Tangent Space** | `convert_object_space_normal_map_to_tangent_space` |

The last `Verb:` colon name in the archive. `Convert` was already the right verb and `to`
already the right connector; only the colon had to go. The trailing hyphen goes with it:
*object-space* is a compound adjective adjacent to the noun it modifies and keeps its
hyphen, while *to tangent space* is a prepositional phrase and never needed one.

## Texture/Assignment and Texture/Packing (2)

*Set Texture* and *Pack Texture Images* were already conformant.

## A lexicon entry corrected

`Pack` was defined as "Arrange UV charts in an atlas", but *Pack Texture Images* arranges
whole images, not charts — the entry under-described its own shipped user, and had done
since it was written. It now reads "Arrange UV charts, or whole texture images, into an
atlas". The two filters now form a pair distinguished only by their object, which is what
the grammar is for: **Pack UV Charts** beside **Pack Texture Images**.

## Enforcement

`Texture` was added to `appliedRoots`, completing the set: all eleven roots are now
checked on every build. Verified to discriminate: restoring
*Convert: Object-Space Normal Map to Tangent-Space* fails the guard with
`repeats the category with a colon` — the colon branch of the check, which no earlier
round had exercised on its own.

# Pass 1 complete

All 328 filters across 11 roots carry a display name that leads with a ratified verb and
follows `Verb Object [(Backend)]`. The lexicon closed at 56 verbs and 5 connectors
(`by`, `from`, `to`, `with`, `for`).

Ids and `pythonName`s are untouched by design and are pass 2: **250 of 328 `pythonName`s
and 280 of 328 ids** no longer match their display name, plus parameter ids on top.
