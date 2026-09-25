# TrueForm I/O integration

This plugin is a second, independent reader and writer for OBJ and STL, built
against the header-only TrueForm library in the pinned `external/trueform`
submodule. Update it with:

```sh
git -C external/trueform fetch
git -C external/trueform checkout <reviewed-commit>
```

Currently pinned at **872775d0e** — v0.10.5 plus one upstream fix
(2026-09-22): a tube swept along a closed polyline pinched at the seam,
because the frame producer's wrap ran on the stored point count while a
closed path stores its first point twice; the seam ring now carries the
first ring's frame and *Create Tube from Polyline* is uniform around a
closed loop. Everything from v0.10.0 through
v0.10.3 was additive, so neither plugin changed to take it: v0.10.1 repaired
orientation and the Euler count; v0.10.2 reads every OBJ in parallel — 44.5 ms
to 6.5 ms on a million-triangle dragon, and 76.3 ms to 8.3 ms for the reader
that also returns normals, texture coordinates and groups — decides bundle
containment from face interiors rather than from vertices, and lets `make_cdt`
return region labels; v0.10.3 builds its internal allocator with large pages
off, so a long-lived session doing repeated CSG no longer retains memory toward
its workers' high-water marks. v0.10.4 added the volume module, a compiled C++
facade, fast winding numbers and NIfTI IO — none of which either plugin calls.

v0.10.5 removed four `tf::intersect_mode` enumerators —
`resolve_crossing_contours`, `resolve_self_crossing_contours`,
`resolve_contours` and `self_intersections`. The enum is now exactly
`{sos, primitives, within}`, and crossings between contours resolve
unconditionally. **This did not cost the build a single call site**, because
every config-taking TrueForm call here omitted the config argument and took the
library's own default; a call that left the resolution at its defaults keeps its
result byte for byte. The one artifact was a stale comment in
`runSelfIntersectionCurves`, now restated: a one-form build implies `within`.

This bump is also the first the filter plugin took new *entry points* from
rather than only new behaviour. Five filters were added on
`tf::make_non_manifold_vertices`, `tf::split_non_manifold_vertices`,
`tf::compute_face_quality` and `tf::make_boundary_rims` — none of which existed
at v0.9.17 — plus `tf::is_manifold`, `tf::is_closed`,
`tf::has_self_intersections` and `tf::euler_characteristic`, which did. All
eight are re-exported by the umbrella `<trueform/trueform.hpp>`, so no include
moved; see [TrueForm Plugin](../../docs/design/trueform_plugin.md) for which
filter calls which.

The boolean, CSG, domain and intersection-curve filters gained a
**Resolve Self-Intersections** checkbox (`resolveSelfIntersections`, default
off) that composes `primitives | within` through `intersectConfigFrom`. It is
deliberately new in this bump and not earlier: a multi-operand `within` build
**lost every operand's domain membership in v0.10.3 and v0.10.4** — every
bounded domain read "inside operand 0" and an expression naming any other
operand returned nothing. v0.10.5 is the first release where that checkbox is
sound.

The step from v0.9.17 to v0.10.0 was the breaking one: the `cut` module was
removed outright, with no compatibility shim, and its ground redistributed to
`arrangement`, `iso` and `csg`. It cost MeshLab one call site, because both
plugins include the umbrella `<trueform/trueform.hpp>`, which re-exports the new
modules — the header moves are invisible from here, and only a removed *entry
point* breaks the build. Read the release notes' "Module map" and "Removed entry
points" tables before the next bump; that is where an upgrade's real cost is
stated.

Two things changed under us that the compiler cannot catch, and neither is
covered by a test:

- **An open boolean operand is now a volume unless declared a sheet.** The
  boolean filters pass closed meshes in the covered cases, so nothing moved, but
  an open operand behaves differently from v0.9.17.
- **Tolerance is now the pitch the input's planes are quantized to.** A wall
  doubled at less than the pitch becomes one wall.
- **Domain membership counts winding as of v0.10.5, not crossing parity.** A
  region an operand covers twice now reads inside it, and a zero-thickness fold
  encloses nothing. Upstream measured no deterministic delta on a thousand
  corpus pairs, so clean input is unchanged; self-overlapping input is not.

## Licensing and permission

TrueForm is dual-licensed under the PolyForm Noncommercial License 1.0.0 or a
commercial agreement with XLAB, neither of which is GPL-compatible.

**MeshLab has explicit permission from the TrueForm owners (Polydera/XLAB) to
include the library**, obtained for this project specifically. That permission is
why `MESHLAB2_PLUGIN_IO_TRUEFORM` defaults to `ON`.

It does not travel with the source. Anyone redistributing a MeshLab binary that
contains the TrueForm components needs their own agreement with XLAB
(`info@polydera.com`). See `external/README.md` for the summary and
`external/trueform/LICENSE` and `COMMERCIAL.md` for the terms.

## Why a third OBJ reader and a second STL reader

`io_vcg` already handles OBJ and STL, and `io_obj_rapidobj` gives a second OBJ
path. This is deliberate duplication, for two reasons:

- **Fringe files.** OBJ and STL are loosely specified and the wild is full of
  variants. Independent parsers fail on *different* malformed files, so a file
  one reader rejects often opens in another. Having more reference importers is
  the point, not an accident.
- **Speed.** TrueForm parses in parallel via oneTBB.

`MeshIOPluginManager::pluginFor()` already resolves the extension collision
through the persisted per-extension preference — it had to, because `io_vcg` and
`io_obj_rapidobj` already both claim `.obj`. No new selection machinery was
needed.

## Behavioural differences from the other readers

- **STL import welds coincident vertices while loading, always.** STL is a
  triangle soup with no shared vertices. `tf::read_stl` routes through
  `tf::clean::polygon_soup`, so the mesh arrives welded. The document merges STL
  duplicates after any reader by default (`document.mergeStlDuplicateVertices`),
  so the difference shows only with that preference turned off.
- **OBJ import recovers vertex positions and faces only** — no UVs, normals or
  materials, by design in `tf::read_obj`. For a textured OBJ use `io_vcg` or
  `io_obj_rapidobj`. This is a geometry-recovery reader.
- **Export writes triangles only**, and no attributes. `tf::write_stl` requires
  triangular polygons; n-gons are fan-triangulated on the way in and out.

## Qt and oneTBB: the `emit` collision

TrueForm pulls in oneTBB, whose `tbb::profiling::event` declares an `emit()`
member. Qt defines `emit` as an empty macro, which turns that declaration into
`void () {}` and fails to compile with a message that points into a TBB header
rather than at the real cause.

`trueformioplugin.cpp` therefore wraps the TrueForm include in
`#pragma push_macro("emit")` / `#undef emit` / `#pragma pop_macro("emit")`. Any
further TrueForm-based plugin in a translation unit that also sees Qt headers
needs the same guard, or must include TrueForm before Qt.
