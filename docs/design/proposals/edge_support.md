# Edge Support

This document records the state of edge handling in MeshLab and what it would
take to make edges a first-class element. Per-edge colour storage, PLY I/O and
rendering landed in September 2026; **everything else described here is
unimplemented.**

See also: [Data Model](../data_model.md) (layers, `VCGMesh`, revisions),
[Rendering](../rendering.md) (the edge pass and the selection overlay),
[Architecture](../architecture.md), [Adding a Filter](../adding_a_filter.md),
[Vocabulary](../vocabulary.md) (filter naming).

## Status

As of 2026-09-16: per-edge colour works end to end — storage on `VCGEdge`, PLY
read/write, three render colour sources, undo and duplication. Nothing else
below is implemented or scheduled.

## Two different things are called an edge

The single most important thing to hold onto is that MeshLab has **two unrelated
notions of edge**, and almost every gap in this document comes from conflating
them.

### Edges inside meshes

A triangle mesh does not store its edges. An edge exists only as a side of a
face, and there is no object to attach data to. What per-edge information exists
is packed into **bit fields on the incident faces**, which means:

- it is limited to bit-sized facts — no colour, no scalar, no attribute;
- it is **duplicated**, since an interior edge is a side of two faces and each
  keeps its own copy, which the writer must keep consistent;
- it has no identity, so nothing can address "edge 47" of a mesh.

Two such bits are in use today:

| Bit | Meaning | Who writes it | Who reads it |
|---|---|---|---|
| `IsF(e)` — faux | Hide this edge when drawing the wireframe; marks the interior diagonals of a polygon triangulated into triangles | vcglib's polygonal importers (`import_obj`, `import_off`, `import_ply`), `filter_meshing`, `filter_instant_meshes` | the wire pass, [meshgpuresourcecache.cpp:1052](../../../src/render/meshgpuresourcecache.cpp) |
| `IsFaceEdgeS(e)` — edge selection | This side of this face is selected | `filter_meshing`, `filter_trueform` (`select_crease_edges_trueform`, `select_non_manifold_edges_trueform`), `filter_texture_defragmentation` | **nothing** |

That last row is a real gap and is discussed under [Rendering of selected
edges](#rendering-of-selected-edges).

### Edges inside polylines

An *edge mesh* — what the UI calls a polyline layer — has real `VCGEdge`
elements. They have identity, they can carry components, and they are what
per-edge colour was added to. Today:

```cpp
class VCGEdge : public vcg::Edge<VCGUsedTypes,
    vcg::edge::VertexRef,
    vcg::edge::Color4b,     // added 2026-09
    vcg::edge::BitFlags> {};
```

No quality, no normal, no adjacency. By comparison `VCGVertex` carries coord,
normal, colour, quality and flags as fixed components plus four OCF ones.

Polyline layers are produced by a growing family of filters:

| Plugin | Filter |
|---|---|
| `filter_meshing` | `create_polyline_from_selected_edges`, `create_polyline_from_selection_perimeter`, `create_polyline_from_planar_section` |
| `filter_trueform` | `create_polyline_from_self_intersections_trueform`, `create_polyline_from_mesh_intersection_trueform`, `create_polyline_from_scalar_isocontour_trueform` |

**All of them write positions and connectivity and nothing else.** The shared
`addPolylineLayer` helper sets `ioMask = IOM_EDGEINDEX` and leaves quality and
colour untouched. This matters more than it sounds — see
[Give the producers something worth colouring](#tier-3--give-the-producers-something-worth-colouring).

### The bridge between the two

`create_polyline_from_selected_edges` is the only crossing point: it turns
per-face edge selection bits into real edge elements in a new layer. In practice
it is also the *only way to see* a face-edge selection at all.

## Rendering of selected edges

**Neither kind of edge selection is rendered.**

The selection overlay is the last pass of the frame and draws exactly two
things: semi-transparent red triangles for selected faces, and red points for
selected vertices. `PerMeshRenderSettings` has `showSelection`,
`showSelectionFaces` and `showSelectionVertices` — there is no edge equivalent,
and the mesh GPU cache builds no selected-edge buffer.

The decorator pass is the other place edges get drawn, and it covers boundary
edges, texture seams, non-manifold edges, non-manifold vertices, normals and
curvature directions. There is **no decorator for selected edges and none for
crease edges**.

The practical consequence: running `select_crease_edges_trueform` on a model
appears to do nothing. The bits are set, the log says how many, and the viewport
is unchanged. The user has to run `create_polyline_from_selected_edges` and
inspect the resulting layer to confirm the filter worked at all — and the
filter's own help says so, telling the reader to build a polyline "to see
exactly where the trouble is". A filter documenting a workaround for the fact
that its output is invisible is the clearest statement of the gap.

Two separate pieces of work follow, and they should not be confused:

1. **A selected-edge decorator for meshes.** Reads `IsFaceEdgeS(e)`, builds a
   line buffer, draws it like the boundary decorator does. The boundary
   decorator is the template: it already walks faces, collects edges by a
   predicate, and has its own colour and width settings. This is the cheaper and
   more valuable of the two.
2. **Selected edges in the selection overlay for polylines.** Reads
   `VCGEdge::IsS()`, keyed on `selectionRevision` like the existing selection
   buffers. Pointless until something can select polyline edges — see below.

## Interactive selection of edges

Nothing can select an edge interactively, in either sense.

**For meshes.** The `Rubber-band Select` tool drags a rectangle and runs
`select_by_screen_rectangle`; the tool carries a single `bool m_selectFaces`,
and `F`/`V` toggle between faces and vertices. There is no third mode. Adding
one means deciding what "an edge is inside the rectangle" means — both endpoints
in, or the segment intersecting the rectangle — and then writing the result to
the per-face edge bits of both incident faces.

**For polylines.** Worse: `VCGEdge::IsS()` is never set by anything in the
codebase. [layerwidget.cpp:274](../../../src/ui/layerwidget.cpp) counts selected
edges and shows the count in the layer tree, so the UI is already prepared to
report a number that is always zero. `select_all`, `select_none` and
`invert_selection` take `allFaces`/`allVerts` and have no edge option;
`select_non_manifold_edges_vcglib` selects the faces and vertices *incident* on
non-manifold edges rather than edges themselves. vcglib is ready —
`UpdateSelection` has `EdgeAll`, `EdgeClear`, `EdgeInvert` and `EdgeCount` — and
nothing calls them.

**What already exists to build on.** The current-layer outline pass rasterises
polyline edges into an offscreen depth buffer, through both
`m_currentMaskEdgesDepthOnlyPipeline` and
`m_currentMaskFatEdgesDepthOnlyPipeline`. So the GPU machinery for "which edge
pixels are where, and which are occluded" is already written and in use; a
picking or rubber-band path for edges would extend it rather than start from
nothing. The same applies to `ViewpointOccluder`, which backs the visible-only
option of rectangle selection.

**Suggested order.** Filter-driven selection first (`select_edges_by_expression`
below), because it needs no UI work and immediately makes `IsS()` mean
something; then the polyline selection overlay so the result is visible; then
the interactive tool mode, which is the only part needing tool-state and
keybinding design.

## Filters

The natural home for expression-driven ones is `filter_expression`, whose
per-element pattern (`setPerVertexVariables` / `setPerFaceVariables` and the
matching runtime setters) an edge context would mirror. The plain colorize ones
belong in `filter_colorproc` beside their vertex and face twins, though keeping
the whole family together is a defensible alternative.

### Tier 0 — two enablers

Not filters, but everything below is thinner without them.

**Add `vcg::edge::Qualityf` to `VCGEdge`.** One line, exactly parallel to the
`Color4b` line. vcglib already ships `UpdateColor::PerEdgeQualityRamp` and
`Stat::ComputePerEdgeQualityMinMax`, and both call `RequirePerEdgeQuality` — so
today they cannot compile against `VCGMesh`. This one line unlocks the existing
scalar→colormap path instead of every colorize filter hand-rolling a ramp. Cost
is 4 bytes per edge, on layers that have orders of magnitude fewer edges than a
mesh has faces.

**Add an `EdgeColor` case to `MeshFilterVisualizationAttribute`** and its branch
in `MainWindow`. `defaultRenderModeForMesh` picks `PerEdge` automatically for
*newly created* layers, but a filter that colours an existing polyline changes
nothing on screen without a hint.

### Tier 1 — expression filters

| Filter | Notes |
|---|---|
| `select_edges_by_expression` | The keystone. Gives `IsS()` its first writer and makes `onselected` meaningful on everything else. |
| `compute_edge_color_by_expression` | `r`/`g`/`b`/`a`, twin of the vertex and face pair. |
| `compute_edge_scalar_by_expression` | Needs Tier 0. |
| `define_custom_edge_scalar_attribute`, `define_custom_edge_point_attribute` | Completes the custom-attribute trio. |

The design work is the **edge variable context**, where edges should earn their
own vocabulary rather than copy the face one. Following `setPerFaceVariables`:

- endpoints — `x0,y0,z0` / `x1,y1,z1`, `nx0`…`nz1`, `r0,g0,b0,a0` / `r1`…`a1`,
  `q0,q1`, `vi0,vi1`, `vsel0,vsel1`
- the edge itself — `er,eg,eb,ea`, `eq`, `ei`, `esel`
- **derived conveniences** — `elen` (length), `emx,emy,emz` (midpoint),
  `edx,edy,edz` (unit direction). All derivable from the endpoints, but they are
  what anyone will actually reach for, and the face context already sets the
  precedent of precomputing this kind of thing. Length and a direction dot
  product cover most useful expressions on a polyline.
- bbox variables come free from `defineCommonBBoxVars`.

Cost: one `setPerEdgeVariables`/`setEdgeRuntime` pair (~60 lines, mirroring the
vertex pair) plus roughly 40 lines per filter in the existing `runFilter` chain.

### Tier 2 — colorize filters

- **`colorize_edges_by_scalar`** — twin of `colorize_vertices_by_scalar`;
  essentially one call to `PerEdgeQualityRamp`. Needs Tier 0.
- **`compute_edge_scalar_from_geometry`** — twin of
  `compute_face_scalar_from_geometry`: length, angle to an axis, midpoint
  height. Feeds the above.
- **`set_random_polyline_color`** — a different colour per connected chain.
  Visually the most useful of the set, and it needs no new vcglib component:
  `edge::EEAdj` and `vertex::VEAdj` are both absent, but a union–find over
  endpoint vertex indices gives the components in about fifteen lines.
- **`transfer_color_from_vertex_to_edge`** and the reverse — twins of the
  existing vertex↔face transfer pair.

### Tier 3 — give the producers something worth colouring

This is what turns the feature from a toy into something that earns its place.

`create_polyline_from_scalar_isocontour_trueform` takes a `contourCount` and
puts **every level into a single layer**. A twenty-contour extraction is
currently one uniform hairball, and the scalar that generated it is discarded.
Have it stamp the isovalue into per-edge quality and `colorize_edges_by_scalar`
turns it into properly coloured isolines with no new UI at all.

The same argument applies to `create_polyline_from_planar_section` (section
height, once it can cut at several offsets) and to the trueform intersection
polylines (which surface each piece came from).

### One distinctive idea, deliberately deferred

**Arc length along the chain** — `es`, normalised 0→1 from the start of the
polyline, and `eidx`, position in the chain. It is the one quantity edges have
that vertices and faces do not, and colouring by it shows direction and flow.
It needs the chain *ordered*, not merely component-labelled, so it means walking
the union–find result and handling open versus closed chains. Worth doing, not
in the first slice.

## Staging

1. `vcg::edge::Qualityf`; `select_edges_by_expression`;
   `compute_edge_color_by_expression`; `compute_edge_scalar_by_expression`;
   `colorize_edges_by_scalar`. The smallest set that closes the loop — select,
   compute a number, map it to colour — and makes the feature usable end to end.
2. A selected-edge decorator for meshes, so `select_crease_edges_trueform` and
   friends stop being invisible. Independent of everything else here and
   arguably the highest value-per-line item in the document.
3. Producers stamping quality (Tier 3), starting with the isocontours.
4. The remaining colorize filters, and the polyline selection overlay.
5. An edge mode for `Rubber-band Select`, on top of the existing depth-mask
   machinery.

## Open questions

- **Should mesh edge selection and polyline edge selection share a UI?** They
  are different storage with different semantics, but a user drawing a rectangle
  over a model does not care. A single tool mode writing to whichever the
  current layer supports is probably right, but it makes the tool's state harder
  to describe.
- **Does `invert_selection` / `select_all` grow an `allEdges` flag**, or do
  polyline layers get their own selection filters? The first is less UI, the
  second avoids three dead checkboxes on every triangle mesh.
- **Per-edge quality on mesh edges** is not addressed here at all and would need
  a genuinely different design — probably a named attribute keyed by a canonical
  (vertex, vertex) pair rather than anything stored on faces.

## Non-goals

- Making triangle meshes store explicit edge elements. The face-bit
  representation is a deliberate memory trade-off and is not in question.
- Half-edge structures. vcglib has `EHAdj`; nothing here needs it.
- Edge-based remeshing or editing operations. This document is about carrying
  and showing per-edge information, not about changing topology.
