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
- MSVC needs `/bigobj` for the filter TU.
