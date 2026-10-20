# TrueForm Plugin

How the TrueForm-backed plugins (`plugins/io_trueform`, `plugins/filter_trueform`)
relate to the rest of the codebase, and what to read before changing them. For the
plugin scaffolding itself see [Adding a Filter](adding_a_filter.md).

## The computation layer

Inside these two plugins, geometry work goes through `tf::` entry points, not
vcglib. The repository-wide "always use vcglib" rule stops at their directory
boundary; vcglib appears only at the conversion edge, where layer meshes become
TrueForm buffers and results come back.

TrueForm ships its own agent/contributor documentation in the submodule:
`external/trueform/AGENTS.md` is the entry point, and it routes caller-side work
to `agents/usage_cpp.md` (how the public API composes: views, tagging, reusable
structures) and `agents/cpp_modules.md` (the API inventory). Read those before
adding or changing TrueForm calls; prefer a direct entry point over rebuilding a
result the library already returns.

## Local rules

- The single `#include <trueform/trueform.hpp>` sits inside the Qt keyword-macro
  fence (`push_macro("emit")` ...): Qt's `emit` collides with oneTBB's
  `event::emit` otherwise. Do not add TrueForm includes outside the fence.
- Filters bake the layer matrix into world coordinates before TrueForm calls and
  map results back through the inverse. That is the filter semantics — for
  anything involving two layers, the relative transform is the geometry — so do
  not move computation into layer-local space.
- A filter that builds a multi-operand arrangement — the booleans, the CSG
  expression, the solid domains, the intersection curves — takes its
  intersection run from `intersectConfigFrom(params)`, which composes
  `primitives | within` from that filter's *Resolve Self-Intersections*
  checkbox. Crossings between contours resolve unconditionally as of TrueForm
  v0.10.5, so `within` is the only request a call site has left to make. A
  one-form entry implies it and takes no checkbox.
- Parallel loops over vcg elements are sound only for inline components
  (`Coord`, `Normal`, `Quality`, flags). OCF components derive from
  `std::vector` and must not be written concurrently.
- **Angles cross the boundary in degrees.** TrueForm carries them as
  `tf::rad<T>`; every angle this plugin takes from a user or writes into a
  channel is in degrees, so a parameter converts on the way in
  (`tf::deg<float>(value)`) and a result converts on the way out. There is no
  filter that shows a radian.
- A filter that answers several structural questions about one layer builds the
  connectivity once and tags the form with it — `MeshHealthContext` beside
  `SignedDistanceContext` and `VertexConnectivity`. Every `topology/` entry
  point takes the tagged form and uses what it finds, so an untagged call is a
  silent rebuild per question, not a compile error.
- MSVC needs `/bigobj` for the filter TU.

## The diagnostics and repair family

These read or repair a layer's structure rather than its shape, and each is a
thin call onto one TrueForm entry point. The table is the map from filter to
entry point; `filters.json` is the full inventory.

| Filter | TrueForm entry point |
|---|---|
| Select Non-Manifold Edges (TrueForm) | `tf::make_non_manifold_edges` |
| Select Non-Manifold Vertices (TrueForm) | `tf::make_non_manifold_vertices` |
| Measure Mesh Health (TrueForm) | `tf::is_manifold`, `tf::is_closed`, `tf::has_self_intersections`, `tf::euler_characteristic`, `tf::make_boundary_rims`, `tf::make_non_manifold_edges`, `tf::make_non_manifold_vertices` |
| Split Non-Manifold Vertices (TrueForm) | `tf::split_non_manifold_vertices` |
| Compute Face Quality (TrueForm) | `tf::compute_face_quality` |
| Create Polyline from Boundary Rims (TrueForm) | `tf::make_boundary_rims` |
| Repair Self-Intersections (TrueForm) | `tf::make_polygon_arrangements` |
| Extract Outer Shell (TrueForm) | `tf::make_outer_shell` |

Two contracts in that table are the library's, not the plugin's, and the help
text has to state them because a caller cannot see them:

- `tf::split_non_manifold_vertices` separates only the fans that come apart
  without cutting an edge. An edge three faces carry is crossed by no fan, so a
  vertex holding one is left untouched and `tf::make_non_manifold_vertices`
  still names it afterwards. The filter is a partial repair by construction.
- `tf::compute_face_quality`'s `quality` is a *triangle* measure. A face that is
  not a triangle reads `-1`; the angles and the aspect ratio hold at any arity.
