# TrueForm I/O integration

This plugin is a second, independent reader and writer for OBJ and STL, built
against the header-only TrueForm library in the pinned `external/trueform`
submodule. Update it with:

```sh
git -C external/trueform fetch
git -C external/trueform checkout <reviewed-commit>
```

Currently pinned at **v0.10.2** (2026-09-03). Everything since v0.10.0 is
additive, so neither plugin changed to take it: v0.10.1 repaired orientation and
the Euler count, and v0.10.2 reads every OBJ in parallel — 44.5 ms to 6.5 ms on a
million-triangle dragon, and 76.3 ms to 8.3 ms for the reader that also returns
normals, texture coordinates and groups — decides bundle containment from face
interiors rather than from vertices, and lets `make_cdt` return region labels.

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

- **STL import welds coincident vertices while loading.** STL is a triangle soup
  with no shared vertices, so every other reader yields a mesh that needs
  *Remove Duplicate Vertices* afterwards. `tf::read_stl` routes through
  `tf::clean::polygon_soup`, so the mesh arrives welded.
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
