# MeshLab Vocabulary

The controlled vocabulary for MeshLab: one term per concept, used **everywhere** —
filter categories, display names, Python names, parameter ids, UI labels, and
documentation prose.

This is a normative reference. If a term is not here, it is not approved; extend
this document rather than inventing a synonym locally.

See also: [Filter Organization](filter_organization.md) (how this vocabulary is
applied to plugins and menus), [Adding a Filter](adding_a_filter.md).

## Why this exists

The initial audit covered the 272-filter registry present on 2026-07-29:

- **165 of 272 display names (61 %) did not lead with a verb**, so the reader could not
  tell what would change.
- Live synonym pairs, both spellings in active use: `remove` (10) / `delete` (8) ·
  `create` (16) / `generate` (19) · `apply` (16) / `set` (16) · `transfer` (11) /
  `project` (5) · `simplification` (6) / `decimation` (4) · `quality` (29) /
  `scalar` (7) · `texcoord` (4) / `uv` (2).
- Spelling splits: `parametrization` (8) vs `parameterization` (2); American
  `Color` alongside British `Colourisation`.
- 32 distinct free-text `menuPath` values across 32 plugins, including one-offs like
  `Remeshing, Smoothing and Resampling` and backend leakage like `Normals/Embree`.

**Current registry (2026-08-31): 327 filters**, with five renaming rounds applied —
`Meshing`, `Attribute`, `Creation`, `Geometry`, and `Selection`:

- **57 of 327 (17 %)** do not lead with a verb, and every one of them sits in a root
  whose round has not run yet.
- The same is true of every surviving rejected synonym: `delete` and `generate` in
  `Document`, `quality` in `Repair`/`Transfer`/`Measurement`/`Selection`,
  `parameterization` in `Parametrization`. The completed rounds left no synonym debt,
  so no separate sweep is needed — each round retires its own.
- Free-text `menuPath` is gone from the descriptors in favour of the `categories`
  array; the loader still reads a single `menuPath` string as a legacy fallback.
- British spellings are gone (`colour` 0 / `color` 34).

Remaining rounds, largest first: `Repair` (24), `Document` (22),
`Parametrization` (17), `Measurement` (14), `Transfer` (13), `Texture` (3, plus 7
filters still at the bare root awaiting a subcategory decision). Then pass 2: the
Python names and parameter ids.

## 1. Filter categories (closed set)

### Terminology and scope

A **filter category** classifies a *filter*, never a plugin. Two consequences, both
intended:

- **A plugin may contain filters in any number of categories.** This is structural,
  not a convention: in `filters.json`, `pluginId` is a **file-level** key while
  `categories` is a **per-filter** key. `filter_mls` spans four categories,
  `filter_igl` spans several, and `filter_basic` still spans multiple categories.
  Plugins are dependency/build units; categories are the user-facing
  classification. See [Filter Organization](filter_organization.md), decisions 1–3.
- **A filter may carry several categories.** Classification is a *set* of paths drawn
  from the ontology, not a single value — because real filters carry genuinely
  orthogonal information. *Simplification: QEC (with texture)* is
  `Meshing/Simplification` **and** `Texture`; filing it under either alone discards
  real information. Cross-listing in the tree is a feature, not a defect: the user
  finds the filter from whichever concept they thought of first.

**Ordering: the first category is primary.** The list is ordered and the first entry
is the canonical home — used for documentation grouping, the `Categories` column in
the Filter Plugins Info dialog, and the answer to "where does this filter belong?".
Later entries are cross-listings. This keeps a single canonical answer without
sacrificing the richer classification.

The term is **"filter category"**, not "menu family" and not "filter class":

- *menu family* wrongly ties the taxonomy to one presentation surface. The same
  classification also drives search, the filter panel tree, generated Python docs
  and prose grouping — menus are just one consumer.
- *filter class* collides with C++ `class` in a C++ codebase, and inherits confusion
  from MeshLab's own `FilterClass` enum.

**Descriptor field.** The authored descriptor field is the ordered array
`categories`:

```json
"categories": ["Meshing/Simplification", "Texture"]
```

Each entry must be a valid ontology path, validated by the loader. A legacy single
`menuPath` string is still accepted only as a fallback for old or out-of-tree
descriptors; in-tree descriptors should use `categories`.

### The ontology

A geometry-processing ontology: **11 roots, two levels**. Roots are **nouns**
(concepts); the verbs live in filter names. Both levels are validated by the loader —
a category must be either a root or a declared `Root/Subcategory` pair.

Two levels was chosen for growth. Eleven roots over the 272-filter migration corpus
of 2026-07-29 was ~25 each; with the current 327 filters it remains small enough that a
flat list becomes unbrowsable as the archive grows toward several hundred more
implementations, whereas the second level absorbs that.

The authoritative copy is `src/plugins/filtercategories.h`/`.cpp`, which the loader
validates against; this table mirrors it and must be updated together with it.

| Root | Subcategories | Scope |
|---|---|---|
| `Meshing` | `Remeshing` · `Simplification` · `Subdivision` · `Quad` · `Boolean` · `Deletion` | Changes tessellation or connectivity for its own sake |
| `Repair` | `Duplicates` · `Topology` · `Degenerate` · `Holes and Borders` | Fixes defects: makes an invalid or damaged mesh valid |
| `Geometry` | `Transform` · `Smoothing` · `Alignment` · `Deformation` | Changes vertex positions or the layer matrix, connectivity untouched |
| `Attribute` | `Normal` · `Scalar` · `Curvature` · `Color` · `Custom` | Computes and stores per-element attribute data |
| `Selection` | `by Attribute` · `by Topology` · `by Visibility` · `Set Operations` | Changes selection state only |
| `Creation` | `Primitives` · `Reconstruction` · `Sampling` | Produces a new layer |
| `Parametrization` | `UV Creation` · `UV Conversion` · `Atlas Packing` · `Defragmentation` | Creates or edits texture coordinates and atlas layout |
| `Texture` | `Assignment` · `Conversion` · `Packing` | Creates or edits texture images |
| `Transfer` | `Within a Mesh` · `Between Layers` · `From Rasters` | Moves an attribute to another element, layer or raster. The subcategory names what supplies the correspondence: a mesh's own incidence, a computed match between two layers, or a raster's camera |
| `Measurement` | `Geometric` · `Topological` · `Statistics` | Reports values; does **not** modify the document |
| `Document` | `Layer` · `Camera` · `Render` | Document structure and view state |

`Repair` is a root, not a `Meshing` subcategory: at ~19 filters it is the single
largest branch, and "clean up this broken scan" is a top-level task users come looking
for, not a variety of meshing. Its subcategories, from the existing filters:

| Subcategory | Covers |
|---|---|
| `Duplicates` | Remove/Merge Duplicate Vertices · Duplicate Faces · Close Vertices · Unreferenced Vertices · Wedge UVs |
| `Topology` | non-manifold edges and vertices · folded faces · T-vertices · watertight repair · face orientation (Orient, Invert) |
| `Degenerate` | zero-area faces · isolated components (by face count / diameter) · removal by scalar threshold |
| `Holes and Borders` | Close Holes · Repair Mismatched Borders |

`Boolean` stays under `Meshing` — a boolean rebuilds topology to produce a new valid
mesh, which is meshing work, not a document-structure operation (it was previously
mis-filed under `Layer/Boolean`).

`Meshing/Quad` holds the triangle↔quad conversions — *Convert to Quads by 4-8
Subdivision*, *by Triangle Pairing*, *to Quad-Dominant Mesh*, *to Pure Triangles*. They
were initially filed under `Remeshing`, but converting between triangle and quad
representation is a distinct concern from improving element shape, and the set is
expected to grow.

`Meshing/Deletion` holds the filters that remove selected geometry. They are not
`Selection` (they change the mesh, not the selection) and not `Repair` (nothing was
broken).

### How category names are formed

The root column mixes two legitimate kinds of noun, and the rule is worth stating so it
stays deliberate rather than accidental:

- **Activity roots** — what you do: `Meshing`, `Repair`, `Selection`, `Creation`,
  `Parametrization`, `Transfer`, `Measurement`. Their subcategories name the *method or
  object* (`Meshing/Simplification`, `Creation/Primitives`).
- **Subject roots** — what you work on: `Geometry`, `Attribute`, `Texture`, `Document`.
  Their subcategories name the *activity or quantity* (`Texture/Conversion`,
  `Attribute/Curvature`).

Form rules:

1. **Roots are singular.** This is why the root is `Attribute`, not `Attributes` — it
   was the only plural, and inconsistent with `Geometry`, `Texture`, `Document`.
2. **Subcategories are singular** unless they name a class of countable items, where
   English forces the plural: `Repair/Duplicates`, `Creation/Primitives`,
   `Repair/Holes and Borders`.
3. `Attribute`'s four subcategories are all singular quantity names —
   `Normal` · `Scalar` · `Curvature` · `Color`. `Normals` was also a direct violation of
   the noun lexicon in §4, which makes `normal` canonical.
4. **A subcategory must not repeat a root name unqualified.** `Texture/Creation` was
   ambiguous against the root `Creation`, so texture subcategories are now
   `Assignment` · `Conversion` · `Packing`. `Parametrization/UV Creation` is fine because
   the qualifier disambiguates it.

Discriminators for the borderline cases:

- **`Attribute` vs `Measurement`** — `Attribute` *stores* data on the mesh;
  `Measurement` only *reports*. If nothing is stored, it is `Measurement`.
- **`Attribute/Scalar` vs `Attribute/Color`** — the compute-vs-colorize rule: a
  filter computing a scalar field must not bake color. It returns a visualization hint
  instead. A filter that genuinely does both carries both categories.
- **`Attribute/Scalar` vs `Attribute/Curvature`** — distinct, and not merely by
  convention: curvature has its **own dedicated storage** (the OCF curvature-direction
  component: `PD1`/`PD2`/`K1`/`K2`, guarded by `IsCurvatureDirEnabled`), so it is a
  richer quantity than a single scalar even when a derived magnitude is also written to
  the scalar slot. A curvature filter that only writes a magnitude carries both
  categories.
- **`Creation/Primitives` vs `Reconstruction` vs `Sampling`** — all produce a new
  layer; the discriminator is the **input**: parameters, unstructured data from which
  a surface is inferred, or samples drawn from existing geometry.
- **`Parametrization` vs `Texture`** — UVs versus pixels. A filter doing both carries
  both, primary first.

  **Defragmentation is `Parametrization`, not `Texture`.** *Texture Map
  Defragmentation* is described as "reduce texture atlas fragmentation by **merging
  compatible charts** and resampling the texture map", and *Merge Texture Islands*
  merges "islands … with neighbors sharing a common seam". Charts, islands and seams are
  parametrization concepts: the operation is chart surgery, and resampling the image is
  a consequence. So both filters get `Parametrization/Defragmentation` as primary and
  `Texture` as a secondary category — a textbook case for multi-valued categories.
- **`Geometry/Transform` vs `Geometry/Alignment`** — a matrix the *user* supplies is
  `Transform`; one the *algorithm* derives from correspondence is `Alignment`.
- **`Repair` vs `Meshing/Remeshing`** — `Repair` makes an **invalid** mesh valid;
  `Remeshing` rebuilds an already-valid mesh for element-shape or resolution reasons.
  "Is the input broken?" is the test.
- **Backends never appear as categories.** `Normals/Embree` → `Attribute/Normal`,
  with `embree` carried on the derived implementation facet instead. See
  [Filter Organization](filter_organization.md), decision 3.

All three shape questions raised during review are now settled: `Repair` is a root ·
`Boolean` stays in `Meshing` · `Curvature` is distinct from `Scalar`.

### Migration: today's 32 values → the ontology

| Current `menuPath` | N | Target `categories` |
|---|---|---|
| `Camera` | 10 | `Document/Camera` |
| `Cleaning` | 15 | `Repair/*` by defect kind · `Creation/Reconstruction` (Ball Pivoting) |
| `Color` | 24 | `Attribute/Color` |
| `Compute/Attributes` | 4 | `Attribute` |
| `Compute/Color` | 2 | `Attribute/Color` — it writes color |
| `Compute/Geometry` | 1 | `Attribute` |
| `Compute/Normals` | 9 | `Attribute/Normal` |
| `Compute/Quality` | 2 | `Attribute/Scalar` |
| `Compute/Texture` | 2 | `Texture/Assignment` |
| `Create` | 16 | `Creation/Primitives` |
| `Geometry/Transform` | 1 | `Geometry/Transform` |
| `Inspection` | 1 | `Measurement` |
| `Layer` | 14 | `Document/Layer` |
| `Layer/Boolean` | 4 | `Meshing/Boolean` — mis-filed under `Layer` today |
| `MLS` | 8 | *split by result* — `Geometry/Smoothing` (projection) · `Creation/Reconstruction` (marching cubes) · `Attribute/Curvature` · `Measurement/Statistics` (radius) · `Selection/by Topology` (small components) |
| `Measure` | 5 | `Measurement/Geometric`, `Measurement/Topological` |
| `Measure/Quality` | 4 | `Measurement/Statistics` |
| `Meshing` | 38 | *split ten ways* — see the `filter_meshing` breakdown in [Filter Organization](filter_organization.md) |
| `Normals/Embree` | 1 | `Attribute/Normal` — backend dropped |
| `Parameterization` | 2 | `Parametrization/UV Creation` — spelling normalized |
| `Quality` | 7 | `Attribute/Scalar` |
| `Quality/Embree` | 3 | `Attribute/Scalar` — backend dropped |
| `Quality/Geodesic` | 4 | `Attribute/Scalar` — backend dropped |
| `Raster` | 5 | `Transfer/From Rasters` (color projection) · `Attribute/Scalar` (coverage count) |
| `Remeshing` | 8 | `Meshing/Remeshing` |
| `Remeshing, Smoothing and Resampling` | 2 | `Creation/Reconstruction` · `Meshing/Simplification` |
| `Remeshing/Surface Reconstruction` | 3 | `Creation/Reconstruction` |
| `Sampling` | 17 | `Creation/Sampling` |
| `Selection` | 27 | `Selection/*` by criterion |
| `Selection/Embree` | 1 | `Selection/by Visibility` — backend dropped |
| `Smoothing` | 16 | `Geometry/Smoothing` |
| `Texture` | 16 | *split* — `Parametrization/UV Creation` (Voronoi Atlas, Flat Plane, Trivial Per-Triangle) · `Parametrization/UV Conversion` (PerWedge↔PerVertex) · `Parametrization/Defragmentation` (+`Texture` secondary) · `Texture/Assignment` (Set Texture) · `Texture/Conversion` (normal-map) · `Texture/Packing` (Pack Texture Images) · `Transfer/Between Layers` (the three Transfer filters) |

Notable corrections this surfaces:

- `Compute/Color` belongs to **`Attribute/Color`** — it writes color.
- `Compute/Texture` belongs to **`Texture`**.
- `Quality`, `Compute/Quality`, `Quality/Embree` and `Quality/Geodesic` were **four**
  names for one concept — now the single `Attribute/Scalar`.
- `Raster` was doing two unrelated jobs: projecting color (a `Transfer`) and computing
  coverage counts (an `Attribute/Scalar` write).
- `Layer/Boolean` moves out of `Layer` to **`Meshing/Boolean`** — booleans rebuild
  topology; they are not document-structure operations.
- Four backend-suffixed paths disappear entirely.

Filters that now legitimately carry **more than one** category include *Simplification:
QEC (with texture)* → `Meshing/Simplification` + `Texture`; *Parameterization +
texturing from registered rasters* → `Parametrization/UV Creation` +
`Texture/Assignment` + `Transfer/From Rasters`; *Transfer Vertex Color to Texture* →
`Transfer/Between Layers` + `Attribute/Color`.

### Categories, and what should never be hand-tagged

Categories are the **authored** classification: a set of ontology paths expressing
what the filter *is about*. Everything mechanically knowable should be **derived**
instead of typed by hand, because hand-maintained metadata drifts — which is exactly
what happened to `tags`.

Two derivable facets, both already present in the descriptor in structured form:

| Facet | Derived from | Gives |
|---|---|---|
| **Data touched** | `outputModifies` (`VG` 68, `VN` 61, `FV` 47, `VC` 31, `VQ` 30, `WT` 12, `TM`/`TX`, `FS`/`VS`) and `inputRequirements` | "writes UVs", "reads vertex color", "changes topology" — and the read/write distinction, which no hand tag currently captures |
| **Implementation** | `provenance.project` and the plugin | "every QSlim filter", "everything backed by CGAL" — already searched today |

So `tags` should become **generated** (categories + derived facets, flattened for
search) rather than authored. That kills the drift structurally and cancels the
manual retagging of the original 272-filter migration baseline.

For reference, the hand-written tags measured on 2026-07-29 — all 272 filters in that
baseline carried 1–6 each — show the three failure modes this avoids:

- **Category echoes** — `meshing` (38), `selection` (36), `smoothing` (19),
  `sampling` (18), `cleaning` (16), `create` (16). Redundant, and `meshing` names a
  root we are restructuring.
- **Element and data terms** — `vertex` (44), `face` (36), `quality` (36),
  `color` (31), `texture` (26), `uv` (19). Useful, but exactly what `outputModifies`
  already encodes, so hand-typing them is duplicated effort that can disagree with
  the truth.
- **Rejected vocabulary** — `delete` (8), a verb rejected in favour of `Remove`;
  `inspection` (9), `info` (9) and `measure` (9) as three tags for one concept.

Should a hand tag ever be needed for something genuinely not derivable (e.g.
`point cloud` as an input expectation), it must come from the controlled vocabulary
in sections 3–4, singular and lower case (`normal`, not `normals`).

## 2. Representation

*What* the filter operates on and produces — orthogonal to the category, which says
what it *does*. *Simplify Quadric Edge Collapse* is `Meshing/Simplification` acting on
`Mesh/Triangle`; *Reconstruct Screened Poisson* is `Creation/Reconstruction` taking
`Point Cloud` and producing `Mesh/Triangle`. Neither fact implies the other.

With this, the classification model has four facets:

| Facet | Answers | Source |
|---|---|---|
| **Category** (§1) | what does it do | authored, multi-valued |
| **Representation** (§2) | what does it work on / produce | authored, directional |
| **Data touched** | which attributes does it read/write | derived from `outputModifies`, `inputRequirements` |
| **Implementation** | which library/algorithm | derived from `provenance.project` |

### Terminology: `representation`, not "domain"

`inputDomain` and `outputDomain` already exist in the descriptor and mean **document
scope** — `SingleMesh` / `WholeDocument` / `None`, and `ModifyCurrentMesh` /
`NewMeshes` / `Information`. Applying this document's own one-term-per-concept rule,
the new facet is **`representation`**, and those existing fields should be renamed
**`inputScope` / `outputScope`** in the same pass, since scope is what they describe.

### The values

| Representation | Meaning |
|---|---|
| `Mesh` | Any face-based surface; use when the filter does not care |
| `Mesh/Triangle` | Requires triangles |
| `Mesh/Quad` | Quad or quad-dominant |
| `Mesh/Polygonal` | General n-gons |
| `Point Cloud` | Vertices only, no faces |
| `Polyline` | Edge-based curves (the mesh edge container) |
| `Volume` | Voxel grid or 3D scalar field |
| `Raster` | Registered image layer |

### Direction matters

Input and output are declared separately:

```json
"inputRepresentation": ["Point Cloud"],
"outputRepresentation": ["Mesh/Triangle"]
```

This is what answers *"what turns a point cloud into a surface?"* and *"what converts
triangles to quads?"* — questions the current tree cannot express at all, even though
the filters exist (*Tri to Quad by 4-8 Subdivision*, *Turn into a Pure-Triangular
mesh*, *Reconstruct Ball Pivoting*).

### Declared, not derived

Unlike the data-touched and implementation facets, representation cannot be inferred:

- **Quad-ness is invisible in the data structure.** vcglib stores quad meshes as
  *triangle* meshes with faux edges (`FaceClearF`, `IsFaceFauxConsistent`), so the
  intent lives in bit flags, not in the type.
- **Polylines** use the edge container, which a surface mesh may also populate
  incidentally.
- Only the point-cloud case is partly derivable: `requireVertices` without
  `requireFaces` implies a filter that tolerates points.

So it is declared — but the loader should **cross-check** it: a filter declaring
`Point Cloud` input while listing `requireFaces` is contradictory and should warn. That
catches drift the same way category validation does.

### The need is already visible

These concepts are being expressed informally today, where nothing can search or
validate them: across current descriptors `quad` appears **48** times, `point cloud`
**33**, and `polyline` **12** — scattered through display names, descriptions and tags.

## 3. Verb lexicon

Canonical verbs for the leading word of a name. One meaning each.

| Verb | Means | Rejected synonyms |
|---|---|---|
| `Create` | Produce a new layer | `Generate`, `Make`, `Build`, `Add` (for layers) |
| `Reconstruct` | Infer a surface from unstructured input | `Rebuild` |
| `Sample` | Draw samples/points from existing data | `Resample` (unless resolution really changes) |
| `Remove` | Delete data matching a predicate or the selection | **`Delete`**, `Erase`, `Discard`, `Purge` |
| `Repair` | Fix a defect while preserving intent | `Fix`, `Heal`, `Clean` (as a verb) |
| `Merge` | Weld or combine equivalent data | `Weld`, `Unify`, `Join` |
| `Select` | Change selection state | `Mark`, `Pick` |
| `Compute` | Calculate and store an attribute | `Estimate`\*, `Calculate`, `Re-Compute`, `Recompute` |
| `Measure` | Report values without modifying | `Inspect`, `Report`, `Analyze`, `Info` |
| `Colorize` | Write color derived from other data | `Color` (as a verb), `Paint`, `Colourise` |
| `Transfer` | Move data between domains/layers/rasters, including baking an attribute into a texture | **`Project`**, `Copy`, `Map`, `Push`, `Pull`, `Bake`, `Flatten` |
| `Transform` | Apply an affine change to positions | `Move` (use `Translate`), `Deform` |
| `Translate`, `Rotate`, `Scale` | The specific affine operations | `Shift`, `Turn`, `Resize` |
| `Displace` | Move vertices by a computed scalar or vector field while preserving connectivity | `Perturb`, `Deform` |
| `Freeze` | Write the layer matrix into the vertex coordinates and reset it to the identity | `Apply` (to a stored transform), `Bake` (of a matrix), `Collapse matrix`, `Commit` |
| `Align` | Derive a registration transform between layers | `Register`, `Fit` |
| `Simplify` | Reduce element count | `Decimate`, `Reduce` |
| `Subdivide` | Increase element count by splitting | `Refine`\* |
| `Remesh` | Rebuild tessellation | `Retriangulate`, `Resample` (for surfaces) |
| `Flip` | Reconnect by flipping edges; connectivity changes, vertices do not move | *(`Remesh` is too coarse for it)* |
| `Cut` | Split a surface along a curve, introducing a boundary | **`Split`** (claimed by layer and element operations) |
| `Smooth` | Reduce noise in positions/normals | `Denoise`, `Fair`, `Blur`, `Relax` |
| `Parametrize` | Create or edit UVs | `Unwrap`, `Parameterize`, `Flatten` |
| `Pack` | Arrange UV charts, or whole texture images, into an atlas | `Layout`, `Bin` |
| `Set` | Assign a value or state | *(prefer over `Define`, `Assign`)* |
| `Convert` | Change representation, same information | `Translate`, `Cast` |
| `Duplicate`, `Split`, `Extract` | Layer-structure operations. `Split` additionally covers separating *mesh elements* that were sharing storage (*Split Vertices by Attribute Seam*) | `Clone`, `Separate`, `Detach` |
| `Orient` | Make normals point consistently, or toward a reference | `Re-Orient`, `Reorient` — the values are flipped, not recalculated, so not `Compute` |
| `Sharpen` | Enhance local variation in an attribute | *(the unsharp-mask filters had no verb at all)* |
| `Trim` | Cut a surface along an isovalue of a scalar field and discard one side | *(neither `Remove`, which deletes whole elements, nor `Cut`, which keeps both sides)* |
| `Mirror` | Negate one or more axes | **`Flip`** — reserved above for edge flipping, which is its opposite: connectivity changes and vertices do not |
| `Project` | Move vertices onto existing geometry, or onto a line | Admitted **narrowly**, for geometric projection. Still a rejected synonym for `Transfer` wherever attributes move between domains |
| `Define` | Declare a new named custom attribute | Admitted **narrowly**, for that alone; otherwise still a rejected synonym for `Set` |
| `Refine` | Subdivide adaptively, where which elements split depends on the data | Admitted **narrowly**; plain uniform splitting is `Subdivide` |
| `Add` | Add a quantity to an existing attribute (*Add Noise to Vertex Color*) | Admitted **narrowly**; still rejected for producing a layer, where `Create` wins |
| `Dilate`, `Erode` | Grow or shrink the selection by one ring of adjacent elements | The standard morphological pair; `Select` names the act of selecting, not these two operations on an existing selection |
| `Close` | Fill a boundary loop with new faces | `Fill`, `Patch`, `Cap` — *close a hole* is the established term of art, and `Repair` is too coarse to separate it from the other `Repair/*` filters |
| `Import`, `Export` | Read or write data in a file outside the document | `Load`, `Save` — those are the File menu's document-level operations, and a filter that writes a camera rig or a multiresolution build beside the mesh is not saving the document; also `Read`, `Write` |
| `Rename` | Change the label of a layer or raster | `Relabel`. `Set` would have covered it and matches the ids, but *rename* is the word a user searches for |
| `Render` | Produce an image from the scene | `Draw`, `Snapshot`, `Screenshot` — and not `Create`, which produces a layer |
| `Defragment` | Reduce the fragmentation of a texture atlas by merging compatible charts and resampling | Admitted **narrowly**, for that alone. It is the term of the paper the filter implements (Maggiordomo et al. 2021); `Merge` describes only half of it, and `Pack` means arranging charts rather than merging them |
| `Estimate` | Report a result that is explicitly statistical or approximate, where the approximation is something the user needs to know about | Admitted **narrowly**. `Compute` wins wherever the result is exact; the footnote below states the same rule and is where the ratified set ends |

**Attribute-editing verbs**, admitted as a closed group of standard image and signal
operations, each keeping its ordinary meaning: `Normalize`, `Adjust`, `Clamp`, `Invert`,
`Equalize`, `Desaturate`, `Threshold`, `Tint`. None may be used where `Compute` (derive
from geometry) or `Set` (assign a constant) is accurate.

\* `Estimate` is permitted **only** when the result is explicitly statistical or
approximate and that matters to the user (e.g. *Estimate Radius from Density*).

**`Apply` and `Bake` were removed as verbs** (2026-08-27). `Apply` was defined as
"apply an existing stored transform to geometry" and `Freeze` as "bake the layer matrix
into vertex coordinates" — the same operation twice, and the two entries defined each
other in a circle. `Apply`'s only user was a *smoothing* filter, using it as exactly the
generic verb its own entry forbade. `Bake` had no users at all: baking an attribute into
a texture is already `Transfer`, which won on the domain-to-domain framing.

Both words stay in the **descriptions** of the filters they belonged to, so searching for
*freeze* or *bake* still finds them — the same reason *morphing* survives in the text of
`Displace Vertices toward Target Mesh`. A word being rejected as a verb does not mean it
should stop being findable.

Two rulings worth stating outright, because both rejected terms were in heavy use
before the renaming rounds began:

- **`Remove`, not `Delete`.** One verb; the object says what goes
  (`Remove Selected Faces`).
- **`Transfer`, not `Project`.** `Project` described only the raster sub-case;
  `Transfer` covers all domain-to-domain movement, and the raster appears in the
  object (`Transfer Raster Color to Vertex Color`). `Project` was later admitted for
  the unrelated geometric sense — moving vertices onto a surface — and that is the
  only sense it may carry.

**`Update` is not a verb here.** It is the most common word in vcglib's algorithm API —
`UpdateNormal`, `UpdateTopology`, `UpdateQuality`, `UpdateColor`, `UpdateSelection` and
eight more — so it is the first word a contributor arriving from that codebase reaches
for. It is also the least informative: every one of those calls either derives a value,
assigns one, moves something, or rebuilds a cache, and the lexicon already has a precise
verb for each. No shipped display name or `pythonName` contains it, and none should. The
mapping is recorded here so that looking for *update* lands somewhere useful:

| vcglib | What it actually does | Canonical verb |
|---|---|---|
| `UpdateNormal`, `UpdateCurvature`, `UpdateCurvatureFitting` | Derive an attribute from geometry | `Compute` |
| `UpdateQuality` | Derive a per-element scalar, or assign one | `Compute`, or `Set` for a constant |
| `UpdateColor` | Derive color, assign it, or edit it in place | `Colorize`, `Set`, or the attribute-editing group (`Normalize`, `Invert`, `Equalize`, …) |
| `UpdatePosition` | Move vertices | `Translate`/`Rotate`/`Scale`, `Transform`, `Displace`, `Smooth`, `Freeze` |
| `UpdateSelection` | Change selection state | `Select`, `Dilate`, `Erode` |
| `UpdateTexture` | Create or edit UVs | `Parametrize` |
| `UpdateTopology`, `UpdateBounding`, `UpdateFlags`, `UpdateHalfEdges`, `UpdateComponentEP` | Rebuild a derived cache the next algorithm needs | *none* — see below |

That last row is the important one: five of the thirteen `Update*` classes are
bookkeeping the user never asks for. They are prerequisites, not operations, and MeshLab
declares them per filter as `inputPrepare` (`FF`, `VF`, `BorderFF`, `FNorm`, `BBox`, …)
for the plugin manager to run and tear down around the call. A filter named
*Update Topology* would be exposing an implementation detail as a feature. Normals sit on
both sides of this line — a prerequisite when a filter needs them, and a result the user
asks for as *Compute Normals*.

Every verb above the `Estimate` footnote is ratified. The rounds that introduced the
later ones are recorded in [Filter Names](history/filter_names.md), which is the history; this
table is the authority. `tests/test_filters.cpp` parses it and fails if any shipped
display name leads with a word that is not here.

## 4. Element and data nouns

| Concept | Canonical | Rejected |
|---|---|---|
| Mesh element | `vertex`, `face`, `edge`, `wedge` | `point` (for vertex), `triangle`, `corner` |
| Per-element qualifier | `per-vertex`, `per-face`, `per-wedge`, `per-mesh` | `vertex-wise`, `PerVertex` in prose |
| Per-element scalar attribute | `scalar` / `scalar field` (user-facing) · `quality` (code/API only) | `quality` in user-facing text, `value` |
| Texture coordinates | `UV` (user-facing), `texcoord` (code/API) | `tex coords`, `UVW`, `st` |
| UV creation | `parametrization` | `parameterization`, `unwrapping` |
| Texture image | `texture` | `image`, `map` (except `normal map`) |
| Registered photo layer | `raster` | `image`, `photo`, `camera image` |
| Geometric object | `mesh` | `model`, `object`, `shape` |
| Document slot | `layer` | `entry`, `item`, `slot` |
| Colour | `color` (American) | `colour`, `Colourisation` |
| Selection | `selection` | `marked set`, `active set` |
| Normal vector | `normal` | `normals direction` |
| Layer matrix | `matrix` | `transformation matrix` (verbose), `xform` |

`mesh` vs `layer`: filters that operate on geometry say **mesh**; operations on
document structure say **layer**. *Duplicate Layer*, but *Remove Duplicate
Vertices*.

### Why `scalar`, not `quality`

`quality` is a **storage-slot name masquerading as a concept**. The slot (`cQ()`,
`IOM_VERTQUALITY`) actually holds curvature magnitudes, geodesic distances, ambient
occlusion, shape diameter, raster coverage counts — none of which is a "quality". At
the level of abstraction of a category, the honest concept is a **per-element scalar
field**; "quality" tells the reader nothing about what was computed and misleads anyone
who has not learned the MeshLab convention.

This follows the same split already used for texture coordinates: **`UV` user-facing,
`texcoord` in code**. Likewise **`scalar` user-facing, `quality` in code** — vcglib's
API is not ours to rename, and no code churn is implied.

Scope warning: user-facing `quality` is not confined to filters. It also appears in the
histogram, the colormap-by-scalar UI, the layer panel's `VQ`/`FQ` data flags, the
decorator info panel, and parameter ids such as `qualityThreshold`. Filters and
categories are renamed in **pass 1**; the UI labels and parameter ids follow in a later
pass so the two stay consistent. Until then, expect `quality` to persist in the UI.

## 5. Spelling and casing

- Display names: **Title Case**, no trailing period.
- No irregular internal capitals: `Unsharp`, not `UnSharp`; `Re-Orient` →
  `Reorient`.
- No hyphenated pseudo-verbs: `Re-Compute` → `Compute`.
- American spelling throughout (`color`, `normalize`, `parametrize`).
- Python names: `lower_snake_case`, no abbreviations that are not in this document.
- Parameter ids: `lowerCamelCase` (see the parameter table in
  [Filter Organization](filter_organization.md)).

## 6. Naming grammar

Display name:

```text
Verb Object [(Backend)]
```

- Lead with a canonical verb (§3).
- The category is **not** repeated in the name: the menu already carries it. So under
  `Meshing/Simplification`, the name is *Simplify by Quadric Edge Collapse*, not
  *Simplification: Quadric Edge Collapse*.
- Add the backend or algorithm in parentheses whenever it tells the reader **which
  implementation they are getting** and that could reasonably matter: *Simplify by
  Quadric Edge Collapse (QSlim)*.

  This does **not** require two filters sharing a base name. What competes is the set of
  routes to a result, not the set of identical labels. Gaussian curvature is reachable
  through *Compute Curvature (Discrete)*, *(APSS)*, *(RIMLS)*, *(TrueForm)* and
  *Compute Gaussian Curvature (libigl)* — five routes, no two of which share a base
  name, and the suffix is the only thing separating them. Requiring a name collision
  before allowing a suffix would strip exactly the information a user needs to choose.

  It is still not decoration. Omit it where the filter is the only meaningful route to
  its result, and never add it merely to record which plugin the code happens to live
  in — that belongs in the description and the structured references.

**Connectors.** The object often needs a preposition, and three are in regular use —
`by` for the method (*Smooth Vertices by Laplacian*, *Compute Vertex Scalar by
Expression*), `from` for the source (*Compute Geodesic Distance from Border*), `to` for
the destination (*Convert to Pure Triangles*). Prefer `by` for "how", and keep the
phrase readable rather than mechanically short.

`for` names the class of input a specialised variant is built for, distinguishing it
from the general filter: *Measure Topological Properties for Quad Mesh* beside
*Measure Topological Properties* (2026-09-01).

`with` is admitted as a fourth connector, for an *additional output* a filter also
produces: *Parametrize from Registered Rasters with Texture* beside *Parametrize from
Registered Rasters*. It is not a fifth way of saying `by` — reach for it only when the
alternative is a second verb (2026-09-01).

**The incumbent stays unsuffixed.** When a second implementation of an existing filter
arrives, the newcomer carries the backend and the existing name does not change:
*Remove Duplicate Vertices* alongside *Remove Duplicate Vertices (TrueForm)*. This keeps
the archive from churning names every time a competitor is added, and it means the
unsuffixed name is always the long-standing one.

The two rules answer different questions and do not conflict: the paragraph above says
**when a suffix is allowed at all**, this one says **whose name changes** once a direct
competitor turns up. A filter may perfectly well carry a suffix from birth without any
same-named sibling ever existing.

**Superseded 2026-08-27.** Leaving the incumbent bare turned out to hide exactly what
the reader needs: faced with *Remove Duplicate Vertices* and *Remove Duplicate Vertices
(TrueForm)*, nothing tells you the first one is vcglib. Every implementation in a
competing family now names itself, the original vcglib ones included — so
**Simplify by Quadric Edge Collapse (vcglib)** beside **(QSlim)**. The churn this rule was
protecting against is a one-off cost, paid once per family, and worth it.

**A backend family names itself completely.** "Omit it where the filter is the only
meaningful route to its result" reads well per filter and fails across a set: with 19 of
31 TrueForm filters suffixed and 12 bare, the suffix stopped meaning "this is TrueForm"
and started meaning "this one happened to have a sibling". Every TrueForm filter now
carries `(TrueForm)`, including the ones nothing competes with, so its *absence* is
information too (2026-09-01). The omission clause still governs a backend that is not a
named family the user chooses between.

**Algorithm in the name, library in the suffix.** When a family holds two
implementations from the *same* library they cannot both be `(vcglib)`, and the answer is
already in the archive: the distinguishing algorithm belongs in the name.
*Compute Heat Geodesic Distance from Selection (vcglib)* beside
*Compute Geodesic Distance from Selection (vcglib)*; *Smooth Vertices by
Surface-Preserving Laplacian (vcglib)* beside *Smooth Vertices by Laplacian (vcglib)* — the
same shape as *Smooth Vertices by HC Laplacian* and the libigl geodesic pair.

**Exception — a well-known named result may be a noun phrase.** Where a filter's output
*is* a conventionally named object, naming the result reads better than naming the
action: the boolean filters are **`Mesh Union`**, `Mesh Intersection`,
`Mesh Difference`, `Mesh Symmetric Difference` rather than *Unite Meshes* etc. Use this
sparingly — it applies when the noun is the established term of art, not merely when a
noun is available. Recorded so the names are not "corrected" back to verb-first later.

Python name:

```text
verb_object[_backend]
```

The display name, in snake_case: punctuation collapses to `_`, a `(Backend)` suffix
becomes a trailing `_backend`, and the articles `the`/`a`/`an` drop. Nothing else is
dropped -- connectors included -- so the menu name and the API name are one string in two
spellings, and neither has to be looked up to know the other. **The `id` is the same
string**, so a filter has one name, not three.

Examples: *Compute Vertex Normals* → `compute_vertex_normals`; *Simplify by Quadric Edge
Collapse (QSlim)* → `simplify_by_quadric_edge_collapse_qslim`; *Transfer Color from Vertex
to Face* → `transfer_color_from_vertex_to_face`.

An earlier draft of this section dropped `by` while keeping `from` and `to`, which no
reader could have predicted; pass 2 settled it by keeping every word (2026-09-03).

### Discoverability: prerequisite (done)

Dropping the category prefix from names is only safe if filter search can still find a
filter by its category. It could not: `MeshFilterPanel::matchesSearch` matched `name`,
`shortDescription`, `longDescriptionMarkdown` and `provenance.project`, but **not** the
category.

**Implemented** in `src/ui/meshfilterpanel.cpp` — `matchesSearch` now also matches the
category. The historical before/after benchmark below used the 272 descriptors in the
2026-07-29 category migration:

| Query | Matches before | After |
|---|---|---|
| `cleaning` | 1 | 16 |
| `selection` | 17 | 36 |
| `sampling` | 22 | 28 |
| `simplification` | 7 | 7 |

The `cleaning` row shows how real the gap was: 15 of the 16 filters in that category
were unfindable by its name, because they are called *Remove Duplicate Vertices*,
*Repair Non-Manifold Edges* and so on — never "cleaning".

The unchanged rows matter just as much. `simplification` gained nothing **because the
category is currently repeated in those names** (*Simplification: Quadric Edge
Collapse*). Once the naming pass removes that redundancy, those queries would have lost
their matches without this fix — so this is the prerequisite that makes the new grammar
safe, not merely an improvement.

Ranking is unaffected: sorting uses `titleMatchesAllTerms`, which is name-only, so name
matches still sort above category-only matches.

The implementation loops over the `categories` array; a single legacy `menuPath` string
is still accepted by the loader for descriptors that predate it.

Worth noting `provenance.project` was *already* searched, which is exactly what the
algorithm-archive model needs — typing *qslim* finds the QSlim implementations.
