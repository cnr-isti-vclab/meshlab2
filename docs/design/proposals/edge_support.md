# Edge Support

What remains of making edges a first-class element. Every stage this proposal
planned has shipped; what is left are the open questions at the end, and the
document is ready to move to history.

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
On the same day *Select All*, *Select None* and *Invert Selection* gained an edge
option (`allEdges`, `InvEdges`), defaulting like the face and vertex ones
(`@hasSelectedEdges` for invert); the selection tool's A/N/I keys pass it off.
Also on 2026-11-27, `Rubber-band Select` gained an *Edges* mode (`E`), below. Later
that day *Dilate Selection* and *Erode Selection* gained `element: edge` (the
tool's `+` and `-` in edge mode), and their vertex mode steps along polylines; edges
are drawn by a per-vertex or a per-edge scalar through the color map, as surfaces
and points are ([Rendering](../rendering.md)), the histogram can chart a per-edge
scalar, and the layer panel counts only the kinds a layer has.

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

`Rubber-band Select` has an *Edges* mode (`E`), committed as `element: edge` of
`select_by_screen_rectangle` and `select_by_screen_polygon`. An edge is taken when
its midpoint projects inside the region, as a face is by its barycenter, so no new
rule had to be chosen between "both endpoints in" and "the segment crosses it". One
mode serves both kinds: a polyline edge sets `VCGEdge::IsS()`, a triangle side the
face-edge bit of each face it bounds, and the two faces agree because they test
the same point, with no adjacency. *Visible Only* casts its ray to the midpoint;
growing to components or islands is refused for edges. All on the CPU, in the
filter: the GPU depth-mask path this section once proposed was not needed.

In edge mode the tool's A, N, I, + and - run the set-operation filters' edge option
(`dilate_selection` and `erode_selection` take `element: edge`), which covers polyline
edges only, so they are disabled on a layer without them; nothing selects, clears,
inverts, grows or shrinks the triangle sides of a mesh except a drag over the whole
view. Vertex-mode dilate and erode step along polyline edges as well as faces.

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
5. Done 2026-11-27: the rubber band's edge mode, on the CPU (see above).

## Open questions

- **Triangle sides in the set-operation filters.** Select All, None and Invert
  could clear, set and flip the face-edge bits too, so the tool's A/N/I work on a
  mesh in edge mode; whether `allEdges` should mean both kinds is undecided.
- **The edge option of the set-operation filters shows on triangle meshes too**,
  where it does nothing: the parameter schema cannot hide a row by what the layer
  holds. The flag was chosen over separate polyline filters (2026-11-27) as less
  UI; a schema-level `appliesTo` would remove the dead rows everywhere at once.
- **Per-edge quality on mesh edges** would need a genuinely different design —
  probably a named attribute keyed by a canonical (vertex, vertex) pair rather
  than anything stored on faces.

## Non-goals

- Making triangle meshes store explicit edge elements. The face-bit
  representation is a deliberate memory trade-off and is not in question.
- Half-edge structures. vcglib has `EHAdj`; nothing here needs it.
- Edge-based remeshing or editing operations. This document is about carrying
  and showing per-edge information, not about changing topology.
