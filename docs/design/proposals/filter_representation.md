# Filter Representation

A proposed fourth classification facet, beside the category: what a filter works on and
what it produces. **Not implemented**: the descriptor loader reads no `inputRepresentation` or `outputRepresentation`, and `inputDomain`/`outputDomain` have not been renamed. Moved out of [Vocabulary](../vocabulary.md) §2 on 2026-11-15.

## The facet

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
