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

As of 2026-09-16, **stage 1 of the staging below is implemented**: per-edge
colour and scalar storage on `VCGEdge`, PLY colour read/write, three render
colour sources, undo and duplication, and five filters —
`select_edges_by_expression`, `compute_edge_color_by_expression`,
`compute_edge_scalar_by_expression` and `colorize_edges_by_scalar`, plus the
`EdgeColor` visualization hint that makes them visible on an existing layer.
The smoke sweep gained a `polyline` fixture, which also gave
`create_tube_from_polyline_trueform` its first real input.

**Stage 2 is also implemented**: selected edges now draw in the selection
overlay, for both kinds of edge, behind an *Edges* checkbox beside *Vertices*
and *Faces*.

**Stage 3 is implemented** for the isocontours: each edge of a multi-level
contour layer now carries the value of the contour it belongs to, so the levels
can be told apart.

Stages 4-5 are not implemented, and nothing in [Interactive selection of
edges](#interactive-selection-of-edges) has changed — no tool can select an edge
by pointing at it, in either sense.

A limitation that surfaced during stage 1 — edge scalars having no `ioMask` bit,
so they were memory-only and lost on save — was **fixed on 2026-09-16** by adding
`IOM_EDGEQUALITY = 0x40000` to VCGLib, taking the one bit that was still free
under `IOM_ALL`. Per-edge scalars now round-trip through PLY as a `quality`
property on the edge element, in both ASCII and binary and whether the file
stores it as `float` or `double`, and filters can declare
`requireEdgeQuality` — `colorize_edges_by_scalar` does.

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
    vcg::edge::Qualityf,    // added 2026-09
    vcg::edge::BitFlags> {};
```

`vcg::edge::Qualityf` joined it in stage 1, so an edge now carries colour and a
scalar; there is still no normal and no adjacency. By comparison `VCGVertex`
carries coord, normal, colour, quality and flags as fixed components plus four
OCF ones.

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

**Implemented 2026-09-16.** The selection overlay -- the last pass of the frame,
which drew semi-transparent red triangles for selected faces and red points for
selected vertices -- now also draws red lines for selected edges, controlled by
an *Edges* checkbox beside the existing *Vertices* and *Faces* ones.

One checkbox covers **both** kinds of edge, because from the user's side there is
only one question, "show me what is selected". The buffer is filled from the
per-face edge bits when the layer is a triangle mesh and from `VCGEdge::IsS()`
when it is a polyline; a layer is one or the other. The per-face path has to
de-duplicate, since an interior edge carries the bit on both of its faces and
would otherwise be drawn twice.

It lives in the **selection** cache rather than the decorator cache, which
matters: selection buffers are keyed on `selectionRevision` as well as
`geometryRevision`, while every decorator is keyed on geometry alone. A
selected-edge decorator would have gone stale the moment the user changed the
selection without touching geometry, which is the normal case.

The rest of this section describes what that has fixed, and is kept because the
reasoning still applies to the decorator-shaped gaps below.

The decorator pass is the other place edges get drawn, and it covers boundary
edges, texture seams, non-manifold edges, non-manifold vertices, normals and
curvature directions. There is **no decorator for selected edges and none for
crease edges**.

The practical consequence, before the change above: running
`select_crease_edges_trueform` on a model appeared to do nothing. The bits are set, the log says how many, and the viewport
is unchanged. The user has to run `create_polyline_from_selected_edges` and
inspect the resulting layer to confirm the filter worked at all — and the
filter's own help says so, telling the reader to build a polyline "to see
exactly where the trouble is". A filter documenting a workaround for the fact
that its output is invisible is the clearest statement of the gap.

What is *not* covered, and would still be separate work: a **crease-edge
decorator**. Crease filters currently write to the same per-face selection bits
they share with everything else, so their output now shows up in the selection
overlay -- but that means a crease selection and a rectangle selection are
indistinguishable, and running one clears the other. Giving creases their own
bits or their own decorator is a real design question, not a rendering one.

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
| `define_custom_edge_scalar_attribute`, `define_custom_edge_point_attribute` | Completes the custom-attribute trio. **Not implemented** — not part of stage 1. |

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

**Implemented 2026-09-16 for the isocontours**, which was the case that
motivated it. `create_polyline_from_scalar_isocontour_trueform` takes a
`contourCount` and puts every level into a single layer; it now stamps each edge
with the value of the contour it belongs to, so `colorize_edges_by_scalar` turns
the result into readable isolines with no new UI at all.

The awkward part is that TrueForm's multi-value `make_isocontours` merges every
level into one `curves_buffer` and keeps no record of which path came from which
level, and the per-point provenance is not exposed by the public API either. So
the filter now calls the **single-value** overload once per level and labels the
segments as it appends them. That trades one mesh walk for N, which sounded
expensive and measured as nothing: on a 327k-face sphere, 50 levels cost 43 ms
total, and going from 5 levels to 20 on an 82k-face sphere moved 13.5 ms to
18.5 ms — the fixed setup dominates, not the per-level extraction.

The shared `addPolylineLayer` helper split into `appendCurvesToMesh` (which
takes the scalar to stamp) and `finishPolylineLayer`, with `addPolylineLayer`
kept as a one-line wrapper so the other five producers are untouched.

**The other producers were considered and deliberately left alone.**
`create_polyline_from_planar_section` cuts at one offset, so a per-edge value
would be the same number on every edge; it would become worth doing if the
filter ever cut at several offsets at once. The intersection polylines
(`create_polyline_from_self_intersections_trueform`,
`create_polyline_from_mesh_intersection_trueform`) lie on *both* input surfaces
by construction, so "which surface it came from" has no answer to record.

### One distinctive idea, deliberately deferred

**Arc length along the chain** — `es`, normalised 0→1 from the start of the
polyline, and `eidx`, position in the chain. It is the one quantity edges have
that vertices and faces do not, and colouring by it shows direction and flow.
It needs the chain *ordered*, not merely component-labelled, so it means walking
the union–find result and handling open versus closed chains. Worth doing, not
in the first slice.

## Staging

1. **Done (2026-09-16).** `vcg::edge::Qualityf`; `select_edges_by_expression`;
   `compute_edge_color_by_expression`; `compute_edge_scalar_by_expression`;
   `colorize_edges_by_scalar`. The smallest set that closes the loop — select,
   compute a number, map it to colour — and makes the feature usable end to end.
2. **Done (2026-09-16).** Selected edges in the selection overlay, so
   `select_crease_edges_trueform` and friends stop being invisible. Landed as
   one *Edges* checkbox covering both kinds of edge rather than as the
   mesh-only decorator originally sketched here.
3. **Done (2026-09-16).** Producers stamping quality (Tier 3) — the isocontours,
   which were the only producer with a per-curve value worth recording.
4. The remaining colorize filters.
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
