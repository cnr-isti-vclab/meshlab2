# OpenSCAD CSG

A single filter exposing geogram's OpenSCAD-subset language compiler, so a mesh can be
built from a script rather than from geometry already in the document. **Not
implemented.**

Split out of the geogram port, where it was decision 8 — deferred, not dropped. See
[Geogram Port](../history/geogram_port.md) for the port that made it available, and
[Vocabulary](../vocabulary.md) for the naming below.

## Status

As of 2026-09-20: not implemented, and not scheduled. `filter_geogram` ships and the
capability is already linked into the application — nothing needs building to reach
it, which is why this is worth keeping on the list rather than forgetting.

## What geogram provides

`mesh/mesh_CSG.h`, part of the `geogram` vcpkg package `filter_geogram` already
depends on:

- **`CSGCompiler::compile_string(source)`** and `compile_file(path)` — a complete
  OpenSCAD-subset language, returning a `CSGMesh_var`.
- **`CSGBuilder`**, the same operations driven from C++: the primitives `square`,
  `circle`, `cube`, `sphere`, `cylinder`; the transforms `multmatrix`,
  `linear_extrude`, `rotate_extrude`, `projection`; `hull`; and n-ary
  `union_instr` / `intersection` / `difference` over a `CSGScope` of meshes.
- Tessellation controls in the OpenSCAD idiom — `set_fn`, `set_fs`, `set_fa`.

## Shape of the filter

One filter, because the language is the interface:

| | |
|---|---|
| Display name | *Create Mesh from OpenSCAD Script (geogram)* |
| `id` / `pythonName` | `create_mesh_from_openscad_script_geogram` |
| Categories | `Creation/Primitives` |
| `inputDomain` | `None` — it consumes no layer |
| `outputDomain` | `NewMeshes` |
| `outputTag` | stands alone as the whole layer name, per [vocabulary](../vocabulary.md) §7 for a filter with no input |
| Parameters | a `string` holding the script, plus the three tessellation knobs |

The verb is `Create`, which §3 reserves for producing a new layer, and the object names
what it is made from. It belongs in `filter_geogram`: one dependency, one plugin.

## Why it was deferred

It is cheap to build and unlike anything MeshLab ships — but it is a **different kind
of feature** from the other twelve geogram filters. Those are operations on the current
document; this is a modelling language that happens to be in the same library. Adding
it during the port would have widened a plugin that was already four families wide,
on a capability nobody had asked for.

That reasoning is about *timing*, not merit. The case for doing it later is unchanged.

## Open questions

1. **Is a script a parameter?** A multi-line program in a `string` parameter is a poor
   editing experience, and the parameter UI has no code editor. The alternatives are a
   `fileopen` parameter pointing at a `.csg`/`.scad` file — which `compile_file` takes
   directly, and which lets the user keep the script in their own editor — or both.
   The file route is probably the honest default.
2. **How much of OpenSCAD does the subset actually cover?** Unmeasured. A filter that
   silently mis-parses a real OpenSCAD file is worse than one that refuses it, so the
   descriptor needs to state the boundary — which means someone has to find it.
3. **Does it belong in `Creation/Primitives` at all?** The category's discriminator is
   the *input*: parameters, unstructured data, or samples of existing geometry. A
   script is arguably a fourth kind. `Primitives` is the closest fit and probably good
   enough, but it is worth a moment's thought rather than an assumption.
4. **Error reporting.** A compiler has syntax errors with line numbers. `errorMessage`
   is a single string, and the log is the only other channel — enough, but the mapping
   from a parse failure to something a user can act on needs deciding.
