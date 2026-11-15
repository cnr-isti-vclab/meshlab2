# Category Migration

Two records from the move to the category ontology of [Vocabulary](../vocabulary.md) §1: the mapping of the 32 old `menuPath` values, and the search change that made dropping category prefixes from filter names safe. Moved out of Vocabulary on 2026-11-15; the rules they led to are there.

## Migration: today's 32 values → the ontology

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

## Discoverability: prerequisite (done)

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

Ranking is unaffected: name matches sort above category-only matches (since 2026-10-30
by `FilterPresentation::rank`, which replaced the name-only `titleMatchesAllTerms`; see
[Filter Search](proposals/filter_search.md)).

The implementation loops over the `categories` array; a single legacy `menuPath` string
is still accepted by the loader for descriptors that predate it.

Worth noting `provenance.project` was *already* searched, which is exactly what the
algorithm-archive model needs — typing *qslim* finds the QSlim implementations.
