# Plugin Reorganization (pass 1)

The plan of 2026-08 to fold the filter plugins into user-facing families (`filter_remesh`, `filter_transfer`, `filter_smooth`, ...), with its goals, mapping table and rulings. **It was not carried out**: the plugin tree is still the one `plugins/` shows. The decisions that did take effect are in [Filter Organization](../filter_organization.md). Moved out of it on 2026-11-15.

## 5. First pass = plugins + categories only

The initial coordinated change renamed plugin directories/ids where needed, replaced
the single `menuPath` string with a validated `categories` array, and reclassified
the 272-filter baseline into the ontology. The baseline `pythonName`s, descriptor ids, and
MeshLab-style parameter ids (`TargetFaceNum`, `QualityThr`, …) are deliberately
**deferred to a second pass**. Rationale: the first pass then breaks no scripts,
so it can land quickly and be judged on the tree alone.

This supersedes the single-pass approach in *Migration Strategy* step 4 below.

## Goals

- Make the filter browser predictable: users should know where to look for an
  operation before they know the exact filter name.
- Make filter names describe the observable result of the operation.
- Keep Python names explicit, coherent, and readable.
- Avoid hiding persistent mesh changes behind names that sound like temporary
  visualization changes.
- Separate framework concerns from filter-specific algorithms.
- Prefer a clean, coherent taxonomy over preserving legacy script names.

## Remaining Issues

The current filter set is partly inherited from MeshLab naming and partly shaped
by incremental MeshLab ports. That gives us working coverage, but not always a
coherent vocabulary.

Common issues:

- Some filters named `Colorize` actually compute quality/scalar values.
- Some filters both compute data and bake colors, which makes their side effects
  harder to predict. **Now a hard rule — see below.**
- Similar parameters use slightly different names across plugins.
- Some plugin names mirror implementation provenance rather than user intent.
- Some Python names are algorithm-family names while others are action names.
- Menu categories mix operation families, data domains, and plugin provenance.
- Some defaults are document-dependent, so script generation and documentation
  need to be clear about when defaults are evaluated.

## Migration Strategy

1. Document the taxonomy and agree on the vocabulary.
2. Audit all existing filters and assign each one a proposed family.
3. Choose canonical display names, Python names, descriptor ids, and parameter
   ids together.
4. Update menu paths and descriptors in one coordinated pass.
5. Update generated docs to show the canonical names.
6. Add temporary aliases only for cases where we explicitly decide the short-term
   release-management benefit is worth the extra complexity.
7. Remove stale names from user-facing docs once the new taxonomy is in place.

## Moves since pass 1

One row per filter that changed plugin after the pass-1 layout settled, with the measured
evidence that justified it.

| Filter | From | To | Evidence | Applied |
|---|---|---|---|---|
| `trim_surface_by_plane` | `filter_screened_poisson` | `filter_meshing` | It first shipped built on PoissonRecon's SurfaceTrimmer, which is what put it there. Reimplemented on `vcg::tri::ClipMeshWithPlane`, its dispatch references no `ScreenedPoisson::` symbol and the plugin's own headers are not included by it; `filter_meshing` already holds `create_polyline_from_planar_section`, whose plane vocabulary it shares, and `close_holes`. | 2026-09-24 |

## Plugin Mapping Table (historical pass-1 proposal)

> **Status (2026-08-31): historical design record.** Applied pieces include the
> category migration, `filter_func` → `filter_expression`, removal of the empty
> `filter_intrinsic_simplification`, and the libigl merge into **`filter_igl`**.
> Since then the implementation has grown again to **33 filter plugin directories**
> with new dependency/algorithm archives such as `filter_instant_meshes`,
> `filter_quadwild`, and `filter_trueform`.
>
> **Deferred — the splits and family folds** (`filter_meshing` 37 filters → 10 targets,
> `filter_colorproc`, `filter_unsharp`, `filter_sampling`, and the folds that would create
> `filter_compute` / `filter_color` / `filter_smooth`). They were designed *before*
> categories became a first-class axis, so they now largely re-implement the category
> organization at the plugin level, while moving hundreds of lines of filter
> implementation between 1500-line files with no automated behaviour check. The rows
> below therefore describe the *intended* end state, not the current tree.

Every pre-baseline plugin classified against decision 1. Evidence columns were
measured from each plugin's
`CMakeLists.txt`, `.gitmodules`, and directory contents — not inferred from names.
The uniform `option(MESHLAB2_PLUGIN_FILTER_*)` present in every plugin is build
plumbing, not a dependency gate, and does not justify a separate plugin.

Verdicts: **KEEP** = passes a split test as-is · **RENAME** = passes a test but
the name is unclear · **FOLD** = fails both tests, merge into family plugin(s) ·
**DROP** = not a filter plugin.

### Stays separate — real external dependency

| Current | Filters | Dependency evidence | Verdict | Target |
|---|---|---|---|---|
| `filter_cgal` | 1 | `find_package(CGAL)` | KEEP | `filter_cgal` |
| `filter_embree` | 5 | embree + OpenMP via `MeshLab2Embree` | KEEP | `filter_embree` |
| `filter_mesh_booleans` | 4 | `find_package(libigl)` | MERGE | `filter_igl` |
| `filter_parametrization` | 2 | libigl — gated on `MeshLab2PluginFilterIglCommon`, includes `igl/lscm.h`, `igl/harmonic.h` | MERGE | `filter_igl` |
| `filter_func` | 18 | `find_package(muparser)` | RENAME | `filter_expression` — dep is real, but "func" says nothing |

**One dependency → one plugin.** In the historical proposal, the two libigl plugins
merge into a single `filter_igl` containing the 6 baseline filters, and
`filter_igl_common`'s adapter code becomes its
internal implementation rather than a separate gate target. This is the shape that
scales as many more libigl filters arrive: one `find_package`, one build gate, one
provenance story.

`filter_igl` will span several menu families (parametrization, booleans, and later
decimation, smoothing, curvature, deformation…) and that is **not** a repeat of the
`filter_meshing` grab-bag. The difference is which axis carries the meaning: a
plugin is a *dependency/build* unit, while categories do all the user-facing
organizing. `filter_meshing` was broken because its name described nothing and its
filters were mis-filed; `filter_igl` names its dependency exactly and its filters
file into the correct families.

Naming convention for this group: **name the plugin after the dependency**
(`filter_cgal`, `filter_embree`, `filter_igl`) so the reason it is separate is
visible in the name. `filter_expression` is the deliberate exception — muparser is
an implementation choice, whereas "expression filters" is what the user is looking
for (see open ruling 5).

### Stays separate — vendored third-party code

| Current | Filters | Vendoring evidence | Verdict | Target |
|---|---|---|---|---|
| `filter_qslim` | 1 | submodule `external/qslim` + `UPSTREAM.md` | KEEP | `filter_qslim` |
| `filter_meshfix` | 1 | submodule `external/meshfix` + `UPSTREAM.md` | KEEP | `filter_meshfix` |
| `filter_xatlas` | 1 | `upstream/` + `UPSTREAM.md` | KEEP | `filter_xatlas` |
| `filter_texture_defragmentation` | 2 | `upstream/` | KEEP | `filter_texture_defragmentation` |
| `filter_screened_poisson` | 3 | vendored source tree + OpenMP | KEEP | `filter_screened_poisson` |

Current dependency/vendored additions outside the original table include
`filter_instant_meshes`, `filter_quadwild`, and `filter_trueform`; each remains a
separate plugin because its external algorithm code is the build/provenance unit.

### Folds into family plugins

| Current | Filters | Why it fails both tests | Target(s) |
|---|---|---|---|
| `filter_basic` | 3 | no dep; three unrelated filters | `filter_measure` (Measure Mesh Summary), `filter_transform` (Normalize To Unit Box), `filter_create` (Create Isosurface from Perlin Noise) |
| `filter_create` | 13 | already a family name | `filter_create` (unchanged) |
| `filter_clean` | 15 | already a family name | `filter_clean` (14), `filter_reconstruct` (1 — its lone non-Cleaning filter is *Reconstruct Surface by Ball Pivoting*) |
| `filter_select` | 25 | already a family name | `filter_select` (unchanged) |
| `filter_colorproc` | 31 | no dep; name is provenance-ish | `filter_color` (24), `filter_compute` (7 Quality — per the compute-vs-colorize rule) |
| `filter_measure` | 8 | already a family name | `filter_measure` (unchanged) |
| `filter_layer` | 14 | already a family name | `filter_layer` (unchanged) |
| `filter_camera` | 10 | already a family name | `filter_camera` (unchanged) |
| `filter_texture` | 11 | already a family name | `filter_texture` (unchanged) |
| `filter_sampling` | 14 | already a family name | `filter_sampling` (unchanged) |
| `filter_meshing` | 37 | no dep; **the single worst grab-bag** — its 37 filters span nine families, see breakdown below | 9 targets — **see open ruling 2** |
| `filter_unsharp` | 21 | no dep; algorithm-named | `filter_smooth` (14), `filter_compute` (7 normals) |
| `filter_trioptimize` | 3 | no dep; algorithm-named | `filter_remesh` (2), `filter_smooth` (1) |
| `filter_sample` | 1 | **Resolved:** random vertex displacement moved to the functional `filter_vertex_displacement` plugin, together with the five MeshLab fractal displacement methods | `filter_vertex_displacement` |
| `filter_voronoi` | 4 | no dep; algorithm-named | `filter_remesh` (1), `filter_sampling` (3) |
| `filter_geodesic` | 4 | no dep; algorithm-named | `filter_compute` |
| `filter_icp` | 3 | no dep; **mis-filed** — ICP/Global Align/Overlapping are registration, currently under Remeshing+Measure | `filter_align` (new family) |
| `filter_plymc` | 2 | no dep — uses vcglib's own `create/plymc/plymc.h` | `filter_reconstruct` (1 *Reconstruct Surface by Volumetric Merging*), `filter_remesh` (1 *Edge Collapse for Marching Cube meshes*) |
| `filter_img_patch_param` | 4 | no dep | `filter_parametrize` (2), `filter_compute` (2 — *Compute Vertex Scalar from Raster Coverage* and *Compute Face Scalar from Rasters* are Compute, not Raster ops) |
| `filter_color_projection` | 3 | no dep | `filter_transfer` (raster→mesh/texture projection) |

### `filter_meshing` breakdown (37 filters → 9 families)

This one plugin is the largest source of misfiling in the tree. Notably it holds
**11 transform/matrix filters**, which is what makes `filter_transform` a real
family rather than a one-filter stub.

| Target | N | Filters |
|---|---|---|
| `filter_transform` | 11 | Flip/swap axis · Rotate · Rotate to Fit a plane · Align to Principal Axis · Scale, Normalize · Translate, Center, set Origin · Matrix: Reset / Freeze / Invert / Set from TRS / Set-Copy |
| `filter_remesh` | 14 | *remeshing* — Isotropic Explicit · Tri to Quad (4-8) · Quad-Dominant · Pure-Triangular · Tri to Quad smart pairing · *subdivision* — Loop · Butterfly · Midpoint · LS3 Loop · Catmull-Clark · Doo Sabin · *simplification* — Clustering Decimation · Quadric Edge Collapse · QEC with texture |
| `filter_clean` | 4 | Re-Orient faces coherently · Invert Face Orientation · Close Holes · Split Vertices by Attribute Seam |
| `filter_create` | 3 | Create Polyline from Selected Edges · Create Polyline from Selection Perimeter · Create Polyline from Planar Section |
| `filter_compute` | 2 | Compute Point Cloud Normals · Compute Principal Curvature Directions |
| `filter_smooth` | 1 | Smooth Point Cloud Normals |
| `filter_parametrize` | 1 | Parametrize by Cylindrical Projection |
| `filter_select` | 1 | Select Crease Edges |

### Judgment call

| Current | Filters | Situation | Proposed |
|---|---|---|---|
| `filter_mls` | 8 | No external dep and no vendored provenance, so it fails both tests — but its 8 filters span **four** families (projection→Smooth, marching cubes→Reconstruct, curvature quality + radius→Compute, small-component→Select) over ~15 shared source files (`apss.tpp`, `rimls.tpp`, `mlssurface`, `balltree`). Folding would scatter a shared algorithm core. | KEEP as an **algorithm-suite exception** — see open ruling 1 |

### Not filter plugins

| Current | Situation | Action |
|---|---|---|
| `filter_igl_common` | No `filters.json`. It is the **libigl dependency gate**: a static adapter library whose existence as a CMake target is what the libigl-backed plugins are conditioned on. | MERGE into `filter_igl` as internal implementation code, since that becomes its only consumer. No separate gate target needed. |
| `filter_intrinsic_simplification` | Directory is **empty** and not referenced in `plugins/CMakeLists.txt` | DELETE |

### Third axis: plugin display names

`MeshFilterPlugin::name()` is what the user actually reads — first column of the
**Filter Plugins Info** dialog. It is a separate axis from the directory/`pluginId`
and needs the same pass. Current state is inconsistent in four ways:

- **A `MeshLab ` prefix on 19 of 31, absent on 12.** The prefix is pure noise:
  every plugin in the application is a MeshLab plugin.
- **Inconsistent suffix** — mostly `… Filters`, but `Create Meshes`,
  `Texture Tools`, `VCG Surface Reconstruction`.
- **Editorial qualifiers** — `Original QSlim Filters`.
- **Family invisible or wrong** — `MeshLab Sample Filters` (its one filter is a
  smoothing filter), `MeshLab Basic Filters`, `MeshLab Function Filters`.

Rules:

1. Drop the `MeshLab ` prefix everywhere.
2. Family plugins: `<Family> Filters`.
3. Dependency/vendored plugins: `<Library or Algorithm> <Family> Filters` — here
   the library **is** the identity, so naming it is informative, not provenance
   leakage. (Decision 3 restricts backends in categories, not in the name of a
   plugin that exists *because* of that backend.)
4. No editorial adjectives.

| Current display name | Target |
|---|---|
| `MeshLab Basic Filters` | *(dissolved — see fold table)* |
| `Camera Filters` | `Camera Filters` |
| `CGAL Mesh Filters` | `CGAL Remeshing Filters` |
| `MeshLab Cleaning Filters` | `Cleaning Filters` |
| `Color Projection Filters` | `Transfer and Projection Filters` |
| `MeshLab Color Processing Filters` | `Color Filters` |
| `Create Meshes` | `Create Filters` |
| `MeshLab Embree Filters` | `Embree Filters` |
| `MeshLab Function Filters` | `Expression Filters` |
| `MeshLab Geodesic Filters` | *(dissolved → Compute Filters)* |
| `MeshLab ICP Filters` | `Alignment Filters` |
| `Image Patch Parameterization Filters` | *(dissolved → Parametrization / Compute)* |
| `MeshLab Layer Filters` | `Layer Filters` |
| `MeshLab libigl Boolean Filters` | `libigl Filters` *(merged into `filter_igl`)* |
| `MeshLab libigl Parametrization Filters` | `libigl Filters` *(merged into `filter_igl`)* |
| `MeshLab Measure Filters` | `Measure Filters` |
| `MeshFix Filters` | `MeshFix Cleaning Filters` |
| `MeshLab Meshing Filters` | *(dissolved across nine families)* |
| `MeshLab MLS Filters` | `MLS (APSS/RIMLS) Filters` |
| `VCG Surface Reconstruction` | *(dissolved → Reconstruction / Simplification)* |
| `Original QSlim Filters` | `QSlim Simplification Filters` |
| `MeshLab Sample Filters` | *(dissolved → Smoothing Filters)* |
| `MeshLab Sampling Filters` | `Sampling Filters` |
| `MeshLab PoissonRecon Filters` | `Screened Poisson Reconstruction Filters` |
| `MeshLab Selection Filters` | `Selection Filters` |
| `Texture Tools` | `Texture Filters` |
| `Texture Defragmentation Filters` | `Texture Defragmentation Filters` |
| `MeshLab TriOptimize Filters` | *(dissolved → Remeshing / Smoothing)* |
| `MeshLab Smoothing and Normal Filters` | *(split → Smoothing Filters / Compute Filters)* |
| `MeshLab Voronoi Filters` | *(dissolved → Remeshing / Sampling)* |
| `MeshLab xatlas Filters` | `xatlas Parametrization Filters` |

New family plugins need display names too: `Compute Filters`,
`Transform Filters`, `Subdivision Filters`, `Simplification Filters`,
`Reconstruction Filters`, `Parametrization Filters`, `Smoothing Filters`.

### Proposed post-fold plugin set, not the current tree

Family plugins and their post-fold populations (totals verified against the
historical 272-filter baseline):

| Plugin | N | | Plugin | N |
|---|---|---|---|---|
| `filter_select` | 26 | | `filter_sampling` | 17 |
| `filter_color` | 24 | | `filter_layer` | 14 |
| `filter_compute` | 22 | | `filter_transform` | 12 |
| `filter_clean` | 18 | | `filter_texture` | 11 |
| `filter_remesh` | 18 | | `filter_camera` | 10 |
| `filter_create` | 17 | | `filter_measure` | 9 |
| `filter_smooth` | 17 | | `filter_align` | 3 |
| `filter_transfer` | 3 | | `filter_parametrize` | 3 |
| `filter_reconstruct` | 2 | | | |

17 family plugins (226 filters) + 9 dependency/vendored plugins + 1 algorithm suite
(46 filters) = **27 filter plugins, from 32**. These proposal totals were verified
against the historical 272-filter baseline, not the registry as it grew afterwards.

Historical dependency/vendored group: `filter_igl` (6), `filter_embree` (5),
`filter_expression` (18), `filter_cgal` (1), `filter_screened_poisson` (3),
`filter_texture_defragmentation` (2), `filter_xatlas` (1), `filter_qslim` (1),
`filter_meshfix` (1); plus the `filter_mls` suite (8).

Expect this group — not the family group — to grow as more libraries are archived.

The headline is not the count: it is that no plugin name is vague or
provenance-only any more, and the 37-filter `filter_meshing` grab-bag is gone.
New families introduced: `filter_compute`, `filter_color`, `filter_smooth`,
`filter_transform`, `filter_reconstruct`, `filter_transfer`, `filter_align`.

### Rulings

**All settled.** `filter_mls` stays as an algorithm suite (1) · remeshing,
subdivision and simplification share one **`filter_remesh`** (2) ·
`filter_transform` is a real family (3) · the raster-projection family is
**`filter_transfer`** (4) · the muparser plugin is **`filter_expression`** (5) ·
**`filter_reconstruct` is kept** despite holding only 2 filters (6).

The plugin mapping is therefore complete and ready to review as a whole. The
remaining prerequisite before code moves is the **menu vocabulary table**.

1. ~~**`filter_mls` exception.**~~ **Accepted.** Under the algorithm-archive
   framing (decision 4) an algorithm suite keeping its own plugin is normal rather
   than exceptional. Proposed third split test:

   > A plugin may stay separate if it owns **shared private implementation code**
   > used by filters in more than one family.

   The qualifier matters: the test is shared *code*, not shared topic. `filter_mls`
   passes clearly — ~15 source files (`apss.tpp`, `rimls.tpp`, `mlssurface`,
   `balltree`) behind 8 filters spanning Smooth/Reconstruct/Compute/Select.
   `filter_voronoi`, `filter_trioptimize` and `filter_unsharp` do **not** pass:
   they are thin wrappers over vcglib with no private core, so they still fold.
   Without this qualifier "algorithm family" would justify keeping everything.
2. ~~Split `filter_meshing`'s tessellation filters three ways or one?~~ **One
   `filter_remesh` (18 filters)** — remeshing, subdivision and simplification share
   a single plugin. `filter_subdivide` and `filter_simplify` are *not* created.

   Note this constrains only the plugin axis. The menu vocabulary is decided
   separately, so `Remeshing`, `Subdivision` and `Simplification` may still be
   distinct user-facing families served by this one plugin — which is exactly the
   plugin-is-a-build-unit / menu-does-the-organizing split from decisions 1–3.
3. ~~`filter_transform` with a single filter.~~ **Resolved by the breakdown**: it
   inherits 11 transform/matrix filters from `filter_meshing` plus *Normalize To
   Unit Box*, so it is a genuine 12-filter family — and the natural home for the
   interactive move/rotate/scale tools' commit filters.
4. ~~Name for the raster-projection family.~~ **`filter_transfer`** — chosen over
   `filter_project` because the family also covers non-projection transfers between
   domains (texture↔vertex colour, mesh↔mesh attributes), which `project` would
   misdescribe.
5. ~~Name for the muparser plugin.~~ **`filter_expression`** — the deliberate
   exception to naming dependency-gated plugins after their dependency, since
   "expression" is what the user is looking for and muparser is an implementation
   choice that could change.
6. ~~Is a 2-filter `filter_reconstruct` worth keeping?~~ **Kept.** Surface
   reconstruction is a task users go looking for by name, so the family earns its
   place on discoverability even while thin — and under the algorithm-archive
   framing it is an obvious growth point, with Screened Poisson, PlyMC and the MLS
   marching cubes already filing into its `Reconstruction` menu from their own
   plugins.
