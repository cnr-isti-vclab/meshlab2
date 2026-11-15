# Filter Organization and Naming

This document covers making MeshLab filters easier to browse, name, document,
script, and maintain. The **principles in "Agreed Decisions" below are settled**.
The category ontology is implemented and validated; the large plugin-fold tables
later in this document are retained as historical design notes, not as a live
description of the current source tree.

See also: **[Vocabulary](vocabulary.md)** — the normative category ontology, verb and
noun lexicons this document applies; **[Filter Name Proposal](history/filter_names.md)** —
per-filter display/Python name proposals derived from that grammar; [Adding a Filter](adding_a_filter.md)
(practical how-to), [Architecture](architecture.md) and
[Data Model](data_model.md).

## Current Implementation Snapshot

MeshLab ships about 400 filters across about 40 filter plugins; which of them a build
has depends on its optional dependencies, and `plugins/filterpluginregistry.cpp` is the
list. Each in-tree descriptor uses the ordered
`categories` array, the loader validates entries against [Vocabulary](vocabulary.md),
and the UI lists a filter under every category it declares. Filter search also
matches categories and `provenance.project`, so backend/library provenance remains
discoverable without appearing in menu categories.

The current plugin tree is intentionally still a dependency/build layout rather
than the proposed post-fold family layout below. Recent current plugins that were
not part of the original 272-filter baseline include `filter_instant_meshes`,
`filter_quadwild`, and `filter_trueform`.

## Agreed Decisions

Settled before starting the reorganization. Everything further down is subject to
these.

### Enabling fact: plugin names are not a scripting contract

All `pythonName` values are **globally unique and flat** — Python invokes
`apply_filter("<python_name>", …)` and never qualifies by plugin. Renaming a
plugin therefore breaks no user script. The only plugin-qualified string is the
internal routing key `pluginId::filterId` (stored in `ScriptAction::filterKey`).
Plugin naming can be decided purely on engineering merit.

### 1. Plugin boundaries follow the user-facing family, split only for dependencies

Name a plugin after the family it serves (`filter_select`, `filter_texture`,
`filter_remesh`). Keep a **separate** plugin only when there is a hard reason:

- an optional external dependency that must be build-gated (embree, cgal), or
- vendored third-party code carrying its own license/provenance (xatlas, qslim,
  plymc, meshfix).

Provenance-named plugins that meet neither test should be folded into their
family. This is what retires vague units such as `filter_basic`, `filter_func`,
and the confusing `filter_sample` / `filter_sampling` pair.

### 2. Filter categories are a closed, validated ontology

Classification is defined by the two-level geometry-processing ontology in
**[Vocabulary](vocabulary.md)** (11 noun roots, each with subcategories), and the
descriptor loader **warns on an unknown category**. Before pass 1, 32 distinct
free-text `menuPath` values existed across 32 plugins; enforcement is what stops
`Parameterization` vs `Parametrization`, and one-off paths such as
`Remeshing, Smoothing and Resampling`, from coming back.

A filter carries a **set** of categories, ordered, first entry primary — the old
single `menuPath` string has been replaced by a `categories` array. See Vocabulary §1 for the model and
for why the data-touched and backend facets are *derived* rather than hand-tagged.

### 3. Backends never appear in menus; algorithm names may appear in filter names

Library and backend names are implementation details and must not appear in a
category: `Normals/Embree` → `Attribute/Normal`, `Quality/Geodesic` →
`Attribute/Scalar`, `Selection/Embree` → `Selection/by Visibility`. The backend is
carried instead on the *derived* implementation facet (see [Vocabulary](vocabulary.md)
§1), so nothing is lost.

Well-known **algorithm** names stay in the filter's own display name — *Screened
Poisson*, *Quadric Edge Collapse*, *xatlas* — because users search for them. The
library is credited in the description and structured references, not the tree.

A parenthesised backend or algorithm on a *filter name* is a different matter and is
encouraged wherever it identifies which implementation the user gets — see
[Vocabulary](vocabulary.md) §6. It does not depend on another filter sharing the base
name: several distinct filters can be routes to the same quantity, and then the suffix
is what tells them apart.

### 4. MeshLab is an algorithm archive — competing implementations are a feature

Hosting several implementations of the same or a similar algorithm is an explicit
goal, not duplication to be resolved. Simplification, for example, will legitimately
exist in vcglib, QSlim, CGAL, PlyMC and libigl flavours simultaneously.

Consequences:

- **Never** collapse or reject a filter because an equivalent one already exists.
- The category is the *operation* (`Meshing/Simplification`), and the competing
  entries sit side by side inside it.
- This makes **disambiguation in the filter display name mandatory** whenever more
  than one implementation of an operation exists — the backend or algorithm name is
  what tells them apart. It refines decision 3: a backend name is banned from the
  category, but *required* in the filter name when it is the distinguishing
  information.
- Expect the number of dependency-gated plugins to grow over time. Decision 1
  scales to that: one optional dependency → one plugin.

## Guiding Principle

A filter name should answer this question:

> What will be different in the document after this operation completes?

If a filter only computes a scalar attribute, the name says `Compute`. If it writes
vertex colors, `Colorize` or `Bake`. If it produces a new layer, `Create` or
`Reconstruct` (`Generate` is a rejected synonym — see the verb lexicon). If it changes
only the selection, `Select`.

## Rule: a Compute filter never bakes color

A filter that computes a per-element scalar **must not** map it to vertex or face
color. The render pass already maps scalar to color, and the filter signals that it
wants that mapping through `visualizationHints` (see
[Filter Visualization Hints](../../README.md) and `MeshFilterRunResult`). Baking is
both redundant and **destructive** — it silently overwrites any colour the user had.

Consequences, and the current state of each:

| Case | Filters | Status |
|---|---|---|
| Unconditional bake inside a `Compute` filter | *Compute Obscurance*, *Compute Ambient Occlusion* | **Fixed.** Both wrote vertex *and* face colour while their descriptors declared only `outputModifies: [FQ, VQ]` — the code violated its own contract. The bake is removed; the `FaceQuality` visualization hint they already returned now does the job. |
| Opt-in `map to colour` parameter on a compute filter | *Compute Vertex Scalar from Camera* (`map`), *Per Vertex/Face Quality Function* (`mapToColor`), *Compute Harmonic Scalar Field* (`colorize`) | **Deferred to pass 2.** These are opt-in rather than automatic, but the option is now redundant. Removing a parameter is script-visible, so it belongs with the parameter-id pass. |
| Colorizing is the filter's actual purpose | *Colorize by vertex/face Quality* | **Correct as-is** — these are `Attribute/Color` filters by definition. |
| Color is the main use of the scalar | *Compute Mesh Ambient Occlusion*, *Compute Point Cloud Ambient Occlusion* | **Reversed 2026-11-14.** Ambient occlusion is wanted as shading far more often than as a number, so both are `Attribute/Color` first and `Attribute/Scalar` second, write the value as a gray vertex color (0 is black, the largest value white) and declare `VC` in `outputModifies`. They ask for the `VertexColor` visualization hint. *Compute Obscurance* stays a pure scalar. |
| Colorizing a newly created layer | *Sample Surface by Voronoi Relaxation* / *Sample Volume* volume meshes | **Acceptable** — writes to a new layer, destroys nothing. Low priority. |

## Deferred: document-dependent defaults

The problem described in *Defaults and Script Generation* below — that some defaults are
computed from the current document (face count, bbox diagonal, quality range) and so must
be captured at invocation time — is **acknowledged and deliberately deferred**. It needs
to be made more evident in the descriptors and the UI, but it is independent of the
naming/classification work and should not block it.

## Naming Rules

### Display and Python names

The grammar is normative in [Vocabulary](vocabulary.md) §6: display names are
`Verb Object [(Backend)]`, Python names `verb_object[_backend]`, both built from the
verb and noun lexicons there.

Note this **supersedes** the `Verb: Object / Method` and
`family_object_method` patterns previously drafted here. The category prefix is no
longer repeated in the name — the category already carries it, and repeating it reads
redundantly under a `Meshing/Simplification` branch. So *Simplify by Quadric Edge
Collapse*, not *Simplification: Quadric Edge Collapse*;
`simplify_by_quadric_edge_collapse`, not `simplification_quadric_edge_collapse`.

Still to avoid:

- implementation-only names when a user-facing action is clearer
- `Colorize` for filters that only compute quality
- vague verbs such as `Process`, `Apply` or `Filter` without an object
- names that hide whether the filter creates a new layer or modifies the current one

Policy on renaming (unchanged): choose the canonical name from the vocabulary, not
from MeshLab legacy; rename when materially clearer; never keep two canonical names
for one operation. **No aliases** — MeshLab has no legacy callers to protect, so a
renamed filter or parameter is renamed outright and a stale call fails loudly with
*Unknown parameter* rather than silently doing something else.

### Descriptor IDs

Descriptor `id` values are internal identifiers, but they should still be
coherent and aligned with the canonical operation name.

Recommended:

- rename ids when the current id is misleading or blocks a cleaner taxonomy
- use ids for implementation routing, not user education
- keep ids unchanged only when the existing id is already clear enough

## Parameter Naming Rules

Parameters should be consistent across filters, especially for script users.

Recommended common names:

| Concept | Preferred id |
|---|---|
| source mesh | `sourceMesh` |
| target mesh | `targetMesh` |
| reference mesh | `referenceMesh` |
| control mesh | `controlMesh` |
| proxy mesh | `proxyMesh` |
| source texture | `sourceTexture` |
| target texture | `targetTexture` |
| output texture | `outputTexture` |
| name of a texture the filter makes | `textureName` (see below) |
| selected-only toggle | `selectedOnly` — injected from `selectionScope`, never declared by hand ([Adding a Filter](adding_a_filter.md)) |
| preserve boundary | `preserveBoundary` |
| preserve topology | `preserveTopology` |
| update normals | `updateNormals` |
| random seed | `randomSeed` (see below) |
| iteration count | `iterations` |
| quality threshold | `qualityThreshold` |
| distance threshold | `distanceThreshold` |

### A single-mesh filter works on the current layer

A filter that works on one mesh takes it from the current layer, chosen in the layer panel,
never from a mesh parameter; a mesh parameter names a *second* input. A lone `sourceMesh`
that defaults to the current layer is the telltale of a filter taking its own mesh as a
parameter, and `FilterDescriptorTests::singleMeshFiltersUseTheCurrentLayer` refuses it; one
that defaults to another layer is the source a two-layer filter reads (next section).

Migrated 2026-09-29 (measured from the code, not the manifests: each of these read its
`sourceMesh` and never touched the current layer otherwise): the ten TrueForm filters
*Create Tube from Polyline*, *Split into Connected Components*, *Extract Outer Shell*,
*Create Polyline from Self-Intersections*, *Create Polyline from Scalar Isocontour*,
*Create Polyline from Boundary Rims*, *Measure Mesh Health*, *Repair Self-Intersections*,
*Split Non-Manifold Vertices* and *Cut Along Scalar Isocontour*. The requirement checks had
been validating the current layer while the filter worked on another.

Kept, because the parameter is a real second input beside the current layer: the occluder
of *Compute Point Cloud Ambient Occlusion*, the surface of *Compute Generalized Winding
Number (libigl)* and the target of *Displace Vertices Toward Target Mesh* (both refuse the
current layer), the projection reference of *Remesh Isotropically*, the samples to refine of
*Sample Surface by Poisson Disk*, the fixed base of *Align Meshes Globally*, and the optional
fit layer of *Create Grid*. That is all 17 filters with a single mesh parameter; the other
30 with mesh parameters take two meshes.

**Open3D pairwise aligners, 2026-10-15.** The five pairwise registrations dropped
`sourceMesh` and take only `referenceMesh`: the first filters migrated to the general rule
below.

### A filter that reads two layers changes the current one

A filter that reads two layers and changes one changes the **current** layer, as a
single-mesh filter does. The other layer is one mesh parameter, defaulting to
`@otherMeshIndex`, named for its role: `sourceMesh` when it is read from, `referenceMesh`
when the current layer is matched, measured or projected onto it. There is no parameter for
the layer that changes. Such a filter declares `outputDomain: ModifyCurrentMesh`.

So the verb's object is always the current layer: for a transfer, make current the layer
that receives and pick where it comes from; for an alignment, the layer that moves and what
it moves onto; for a distance or a selection, the layer that gets the values and what it is
measured against. A filter that changes neither layer -- it creates one, or only reports --
is not bound by this.

`FilterDescriptorTests::twoLayerFiltersChangeTheCurrentLayer` refuses a `ModifyCurrentMesh`
filter with two mesh parameters, or with one that defaults to the current layer: both are
how a filter names the layer it changes. Its exception table holds the parameters that may
default to the current layer without naming the changed one -- a surface to project onto,
by default the layer's own (*Remesh Isotropically*, the two MLS projections), and the layer
held fixed by the whole-document aligners -- and fails the run in both directions. The
migration that brought 15 filters under the rule is recorded in
[two_layer_filters.md](history/two_layer_filters.md).

A scoped filter (`selectionScope`) confines itself on the current layer's selection, which
under this rule is always the layer it changes.

### `textureName`

A filter that makes a texture writes no file: it returns an image with a name, and saving
the mesh writes it under that name ([Adding a Filter](adding_a_filter.md)). The name is a
`textureName` parameter -- `string`, default empty -- resolved through
`TextureAssociationUtils::outputTextureNames`:

- **empty** (the default) names it after the layer and what it holds, `<layer>_<role>.png`
  (`scan_color.png`, `scan_tangent_normal.png`), so two layers' textures do not share a name;
- **a name** is used as given, with `.png` added when it has no suffix;
- with **several texture slots**, `_0`, `_1` ... go before the suffix.

It is a name and not a path: where the file goes is decided by the save. *Rename Texture*
changes it afterwards. Python passes it as `textureName`, the parameter id.

### `randomSeed`

Every filter whose algorithm draws from a random generator declares exactly one
`randomSeed` parameter — `int`, default `0`, minimum `0` — and resolves it through
`FilterParams::getRandomSeed()`:

- **`0`** (the default) draws a fresh seed, so repeated applications differ. The
  drawn value is reported in the filter's result messages, so a result the user
  likes can be pinned afterwards.
- **any other value** is used verbatim, making the run exactly reproducible.

Do not read the parameter with `getInt()` and hand-roll the fallback: the helper is
what keeps "0 means surprise me" identical across filters, and it draws from
`QRandomGenerator` rather than `time(0)` so two filters run in the same second do
not share a seed.

Where randomness is conditional — a `pca` curvature method, a `montecarlo` point
technique, an opt-in `Random` toggle — declare the parameter anyway and report the
seed only on the branch that actually uses it.

`FilterTests::randomizedFiltersDeclareARandomSeed` pins the list of randomized
filters; extend it when you add one.

Current naming issue:

- Many ported filters use MeshLab-style ids such as `TargetFaceNum`,
  `QualityThr`, or `PreserveBoundary`. These should be replaced by coherent
  MeshLab parameter ids when we do the naming pass.

Open question:

- Do we want descriptor support for temporary parameter aliases during a
  transition, or should the cleanup be a direct rename? Since script
  compatibility is not a priority, direct rename should be the default answer
  unless a specific problem appears.

## Defaults and Script Generation

Some defaults are static, such as `0`, `false`, or `"none"`. Others are dynamic
tokens resolved from the current document state, such as:

- current mesh index
- other mesh index
- bounding-box diagonal
- face count
- selected face count
- quality min/max
- hardware thread count

For compact Python generation, default comparison must use the descriptor state
from the moment the filter was invoked. Recomputing defaults after the filter has
run can produce wrong compact scripts. For example, quadric simplification uses a
target face count default derived from the current face count; after the filter
modifies the mesh, that default is no longer the same.

Policy:

- The filter panel's `Copy Python call` and the action-history compact script
  export must use the same formatting function.
- Action history should store both the full Python call and compact Python call
  at filter execution time.
- Full export should prefer the stored full call.
- Compact export should prefer the stored compact call.
- Reconstructing a call later should be treated as a fallback for old history
  entries only.

## Filter Side Effects

Every filter descriptor should make side effects explicit through:

- input domain
- output domain
- output-modifies codes
- input preparation requirements
- parameter-specific mesh preparation
- cleanup policy
- visualization hints

Suggested side-effect categories:

- `InformationOnly`
- `SelectionOnly`
- `ModifyCurrentMesh`
- `ModifyNamedMesh`
- `NewMesh`
- `NewTexture`
- `NewRaster`
- `ModifyDocumentStructure`
- `ModifyViewState`

Open question:

- Should these categories be formalized beyond the existing input/output domain
  and output-modifies fields?

## Open Questions

- ~~Should `Registration` be a top-level family?~~ Settled by the plugin table:
  `filter_align` exists as its own family (the three ICP/global-align filters were
  mis-filed under Remeshing + Measure).
- ~~Should `Sampling` remain top-level?~~ Settled: `filter_sampling` is kept as a
  family (17 filters).
- ~~Should `Reconstruction` be top-level?~~ Settled by ruling 6: kept as its own
  family rather than folded into `Create`.
- Should view-only actions be filters, commands, or both?
- Are there any old names that must remain as temporary hidden aliases for a
  specific release-management reason?
- Should generated Python docs show only canonical names by default?
- How aggressive should we be in splitting the large `filter_meshing` family?
