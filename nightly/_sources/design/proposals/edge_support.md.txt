# Edge Support

What remains of making edges a first-class element. **Not implemented:** any way
to select an edge interactively, in either sense of *edge* below.

Stages 1-3 shipped on 2026-09-16 and their proposal is kept as a record in
[history/edge_support.md](../history/edge_support.md): per-edge colour and scalar
on `VCGEdge` ([Data Model](../data_model.md)), PLY edge scalars
(`IOM_EDGEQUALITY`), the `EdgeColor` visualization hint, *Select Edges by
Expression*, *Compute Edge Color by Expression*, *Compute Edge Scalar by
Expression* and *Colorize Edges by Scalar*, selected edges of both kinds in the
selection overlay ([Rendering](../rendering.md)), and isocontour levels stamped
on their edges. Stage 4 shipped on 2026-11-26: *Set Random Polyline Color*,
*Transfer Color from Edge to Vertex*, and *Define Custom Edge Scalar/Point
Attribute*, whose attributes the edge expressions read by name
([expressions.md](../../expressions.md)). Arc length along a chain shipped on
2026-11-27: the edge expressions read `es`, `earc`, `eidx` and `ech`, from vcglib's
`Clean::EdgeMeshChains`, which splits a polyline at its ends and branch points.

## Two different things are called an edge

MeshLab has **two unrelated notions of edge**, and every remaining gap depends on
which one is meant.

- **Edges inside meshes.** A triangle mesh stores no edges. Per-edge facts are
  bits on the incident faces, duplicated on both faces of an interior edge, with
  no identity and no room for a colour or a scalar. Two bits are in use: faux
  (`IsF`, hides the interior diagonals of a triangulated polygon from the
  wireframe) and edge selection (`IsFaceEdgeS`, written by the crease,
  non-manifold-edge and defragmentation filters, and drawn by the selection
  overlay).
- **Edges inside polylines.** An edge mesh (a polyline layer) has real
  `VCGEdge` elements with colour, scalar and flags, and the *Create Polyline*
  filters produce them. *Create Polyline from Selected Edges* is the bridge: it
  turns per-face edge bits into edge elements in a new layer.

## Interactive selection of edges

Nothing can select an edge by pointing at it.

**For meshes.** `Rubber-band Select` runs `select_by_screen_rectangle` (or
`select_by_screen_polygon`) with one `bool m_selectFaces`; `F`/`V` toggle faces
and vertices. An edge mode means deciding what "an edge is inside the rectangle"
means — both endpoints in, or the segment crossing it — and then writing the
per-face edge bits of both incident faces.

**For polylines.** `VCGEdge::IsS()` is written only by *Select Edges by
Expression*, by layer splitting and by undo. `select_all`, `select_none` and
`invert_selection` take `allFaces`/`allVerts` and have no edge option, and
`select_non_manifold_edges_vcglib` selects the faces and vertices *incident* on
non-manifold edges rather than the edges. vcglib's `UpdateSelection` already has
`EdgeAll`, `EdgeClear`, `EdgeInvert` and `EdgeCount`.

**What to build on.** The current-layer outline pass already rasterises polyline
edges into an offscreen depth buffer (`m_currentMaskEdgesDepthOnlyPipeline`,
`m_currentMaskFatEdgesDepthOnlyPipeline`), so a picking or rubber-band path for
edges would extend GPU code that is in use; `ViewpointOccluder` backs the
visible-only option as it does for faces.

**A crease-edge decorator** is separate work. Crease filters write the same
per-face selection bits as everything else, so their output shows in the
selection overlay, but a crease selection and a rectangle selection are then
indistinguishable and running one clears the other. Giving creases their own bits
or their own decorator is a data-model question, not a rendering one.

## Filters

Two filters this proposal listed were not built, because the edge expressions
already do their work: `compute_edge_scalar_from_geometry` is *Compute Edge Scalar
by Expression* with `elen`, `emz` or `acos(abs(edz))`, and the vertex-to-edge color
transfer is *Compute Edge Color by Expression* with `(r0+r1)/2` and its siblings, as
the description of *Transfer Color from Edge to Vertex* says.

## Staging

4. Done 2026-11-26 (see the opening).
5. An edge mode for `Rubber-band Select`, on top of the existing depth-mask
   machinery.

## Open questions

- **Should mesh edge selection and polyline edge selection share a UI?** They are
  different storage with different semantics, but a user drawing a rectangle over
  a model does not care. A single tool mode writing to whichever the current layer
  supports is probably right, but it makes the tool's state harder to describe.
- **Does `invert_selection` / `select_all` grow an `allEdges` flag**, or do
  polyline layers get their own selection filters? The first is less UI, the
  second avoids three dead checkboxes on every triangle mesh.
- **Per-edge quality on mesh edges** would need a genuinely different design —
  probably a named attribute keyed by a canonical (vertex, vertex) pair rather
  than anything stored on faces.

## Non-goals

- Making triangle meshes store explicit edge elements. The face-bit
  representation is a deliberate memory trade-off and is not in question.
- Half-edge structures. vcglib has `EHAdj`; nothing here needs it.
- Edge-based remeshing or editing operations. This document is about carrying
  and showing per-edge information, not about changing topology.
