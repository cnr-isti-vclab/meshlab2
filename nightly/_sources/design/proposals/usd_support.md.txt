# USD Support

This document proposes reading and writing Pixar's Universal Scene Description (`.usda`,
`.usdc`, `.usdz`) in MeshLab, with its materials, and exporting a set of layers to one USD
file. Stage 1, the flattened import and the one-layer export, is implemented (`plugins/io_usd`);
stages 2 and 3 are not. Decided 2026-11-23 (Paolo): the library is LightUSD, and import starts
flattened, as glTF's does.

See also: [Data Model](../data_model.md) (layers, materials, units),
[Project persistence](../history/project_persistence.md) (how saving works, and why a save is
not a filter), [Third-Party Code](../third_party.md), [Rendering](../rendering.md) (the
material channels the renderer reads), `plugins/io_gltf` (the precedent for materials and
for flattening a scene).

## Status

| Stage | What | State |
|---|---|---|
| 0 | LightUSD as a submodule; read and write the stage-0 fixtures; check against OpenUSD | done 2026-11-27 (§3.1): go; `external/lightusd` at v1.0.0 |
| 1 | `io_usd`: flattened import, single-layer export, four material channels, units and up axis | done 2026-11-27 (§4.4), with the departures from the plan listed there |
| 2 | Scene I/O: export visible layers to one file, import as separate layers | not started; needs approval of the interface |
| 3 | The material model, extended once for USD and glTF | not started; needs its own mapping table |

## 1. What USD asks of MeshLab

A USD file is a **scene**, not a mesh: a tree of prims with transforms, many meshes,
instances, materials bound per face subset, cameras, and composition arcs (references,
payloads, variants, sublayers) that assemble a stage from several files. Five properties
matter for us:

- **Geometry** is `UsdGeomMesh` with `faceVertexCounts` and `faceVertexIndices`: polygons of
  any size, not triangles. Attributes are **primvars** with an interpolation each: `constant`,
  `uniform` (per face), `vertex`, `varying` and `faceVarying` (per corner). Texture
  coordinates are usually `primvars:st`, faceVarying, with v up as in MeshLab and OBJ. A mesh
  can be `leftHanded`, which reverses the winding.
- **Materials** are bound to a whole mesh, or to the faces of a `UsdGeomSubset` (family
  `materialBind`). The portable surface is `UsdPreviewSurface`, fed by `UsdUVTexture` nodes,
  each of which reads one or more channels of an image, with a scale and a bias.
- **Units and orientation** are stage metadata: `metersPerUnit`, whose fallback by the
  specification is 0.01 (centimeters), and `upAxis`, whose fallback is **Y**. MeshLab is
  Z-up.
- **Packaging**: `.usdz` is an uncompressed zip with the stage and its textures inside, the
  form Apple's Quick Look, Blender, Omniverse and Houdini exchange.
- **Composition**: a file can be a root layer that references others; what the user sees is
  the composed stage.

## 2. The library: LightUSD

Decided 2026-11-23. LightUSD is lighttransport's C++ USD library, called TinyUSDZ until its
1.0 (by the author of tinygltf, which `io_gltf` uses).

| | LightUSD | OpenUSD (Pixar) |
|---|---|---|
| Licence | Apache 2.0 (some helpers MIT) | Tomorrow Open Source Technology License 1.0: Apache 2.0 with a changed trademark clause |
| Dependencies | none; C++17 | TBB (already ours) |
| Reads | USDA, USDC, USDZ | all; the reference implementation |
| Writes | USDA, USDC, USDZ | all |
| Composition | sublayers, references, payloads, variants, inherits, specializes, by eager flattening; prototypes are copied, not shared | complete |
| How it comes in | no vcpkg port: a **pinned submodule** under `external/`, by the rule of [Third-Party Code](../third_party.md#choosing-how-to-bring-third-party-code-in) | vcpkg `usd` 26.8, which builds **shared libraries only** ("a true static usd build is left as a future port improvement") |
| Packaging | compiled into the plugin | its plugin registry finds `plugInfo.json` files at run time, and the shared libraries must be bundled and signed on three platforms; a static build needs the monolithic library linked whole-archive |
| Maturity | 1.0.0, "first stable release", November 2026 | the industry reference |

OpenUSD's packaging is at odds with a statically linked application; LightUSD is the kind of
dependency MeshLab already takes. Its risk is youth: a rename and a 1.0 weeks old. Stage 0
measures it before anything depends on it, and the tests check what it writes with OpenUSD's
own `usdchecker` (section 7), used as a tool, never linked.

**Not confirmed from its documentation**, and the first thing stage 0 tests: per-face material
binding through `GeomSubset`, and faceVarying primvars (texture coordinates, normals) in its
render-scene conversion (Tydra). Its schema table marks `UsdGeom` (Mesh and GeomSubset
included), `UsdPreviewSurface`, `UsdUVTexture`, `UsdTransform2d` and the primvar readers done.

## 3. Stage 0: measuring LightUSD

- The submodule at a release tag, built into a throwaway target; build time and the size it
  adds to the application.
- Read: the fixtures of section 7, and a few public USDZ assets (Apple's AR Quick Look
  gallery, Pixar's Kitchen Set), through Tydra's render scene; check subsets, faceVarying
  texture coordinates, `leftHanded`, instancing and a referenced file.
- Write a mesh with two materials and faceVarying coordinates as `.usda`, `.usdc` and
  `.usdz`; `usdchecker` and `usdcat` must accept all three, and `.usdz` must open in Quick
  Look.
- Outcome: go, or go with patches upstreamed first, or reconsider.

### 3.1 Results (2026-11-27)

Measured on LightUSD v1.0.0 (source tarball, not yet a submodule), built as a subdirectory of a
throwaway CMake project with Release flags on this 16-core Mac, checked with
OpenUSD 26.11 (`usd-core` from PyPI). The trial program and its outputs stayed outside the
repository; the written `.usda`, `.usdz` and texture are in the git-ignored `tests/local/usd_stage0/`.

**The library to use is not the one this proposal named.** LightUSD 1.0 ships two
implementations. The default, `src/next` (`lightusd::next`, with the render conversion
`tydra_next`), is a rewrite. The older core, with the Tydra described in its README and
`lightusd_static` in its integration notes, is deprecated (`LIGHTUSD_NATIVE_PRODUCT=legacy`).
Stage 1 targets the new one: link `lightusd::next` and `tydra_next`, include
`next/lightusd-next.hh` and `tydra/next/render-converter.hh`.

| Check | Result |
|---|---|
| Build | 17.5 s wall, 180 s CPU, 283 build steps, library and Tydra only (no tools, tests, examples, C API, OBJ/FBX import, audio, EXR). Static libraries 14 MB |
| Size added | the trial executable, loader, converter and three writers, 4.3 MB stripped (5.4 MB unstripped) |
| GeomSubset materials | read: `material_subsets` per mesh (3 subsets covering the 9 faces of `geomsubset-material-001.usda`); written: OpenUSD binds each subset to its material and `ValidateFamily` accepts the `materialBind` partition |
| faceVarying primvars | kept as authored, with an interpolation flag per attribute: normals (`facevarying-normal-test.usda`), texture coordinates (`parity-material-uv-subset.usda`, `reference-geom-001.usda`) |
| Polygons | kept with `triangulate = false` (`reference-geom-001.usda`: 32 triangles and 468 quads) |
| `leftHanded` | reported per mesh (`RenderMesh::left_handed`) |
| References | resolved (`reference-geom-001.usda`) |
| Instancing | expanded: two instances of one prototype become two meshes, each at its own world transform (`blastest-instanced-static.usda`) |
| Textures | asset path, resolved path, output channel (`r`, `rgb`...) and UV set name per texture; images are not decoded with `load_textures = false`, so MeshLab reads them with `QImage` |
| `metersPerUnit` | Tydra reports 1.0 when the stage does not author it (`render-converter-convert.cc`), but `Stage::GetMeta().metersPerUnit_set` tells whether it was authored |
| Writing | a cube with two materials, one textured through `UsdUVTexture` and `UsdPrimvarReader_float2`, faceVarying `st`, GeomSubsets, Z up, millimeters, written as `.usda`, `.usdc` and `.usdz`: OpenUSD reads all three as written, and its 30 registered validators report nothing. The `.usdz` stores its entries uncompressed with `root.usdc` first, and the texture resolves inside the package |
| Scale | a 2.25 M vertex, 4.5 M triangle mesh: write USDC 23 ms (27 MB), load 8 ms (arrays decode lazily), convert 54 ms; peak resident memory 508 MB including the test's own arrays. OpenUSD reads it back in 20 ms with the same counts and values |
| Quick Look | not checked: `qlmanage` produced no thumbnail from the session's sandbox. To be opened by hand |

**Authoring is low-level.** The writer builds prims with `StageBuilder`/`LayerBuilder`,
values with `Value::MakeFloatCompArray(..., TypeId::Point3f, 3)` and the like, and records the
USD type name of each property itself. A shader output (`outputs:surface`) is declared with
`add_property_slot` on an interned name: a type name alone writes nothing, and the first
attempt produced connections to undeclared outputs. The `materialBind` family must declare its
type (`subsetFamily:materialBind:familyType = "partition"`): OpenUSD's validator flagged its
absence. The exporter is therefore some 300 lines of careful code, not a call. Both pitfalls
belong in its comments and its round-trip test.

**Verdict: go.** Everything stage 1 needs works, at a cost in build time and size MeshLab
already pays for comparable libraries.

## 4. Stage 1: the `io_usd` plugin

A `MeshIOPlugin`, as `io_gltf` is, for `usda`, `usdc`, `usd` and `usdz`.

### 4.1 Import, flattened

The composed stage becomes **one layer**, as a glTF scene does: every mesh's world transform is
baked into its positions and normals, instances are expanded, and the meshes are appended.

| USD | MeshLab | Notes |
|---|---|---|
| `UsdGeomMesh` under the default prim (or the root) | faces of the one layer | `purpose` `default` and `render` only; `proxy` and `guide` are skipped, as are prims made `invisible` |
| polygons (`faceVertexCounts`) | triangles, with faux edges marking each polygon (`IOM_BITPOLYGONAL`), the form `io_vcg`'s PLY and OBJ paths carry | |
| `leftHanded` | winding reversed | |
| `points`, world transform | vertex positions | |
| `normals` / `primvars:normals` | vertex normals; faceVarying ones as wedge normals (`IOM_WEDGNORMAL`, as `io_vcg`'s OBJ path writes them) | |
| `primvars:st` (or the name the material's primvar reader asks for) | vertex or wedge texture coordinates, by interpolation | v up already; no flip |
| `primvars:displayColor` / `displayOpacity` | vertex or face color, by interpolation; constant ones to vertex color | |
| `GeomSubset` (`materialBind`) and the mesh's own binding | each face's texture slot index (wedge `N()`), one slot per material | as glTF's primitives |
| `UsdGeomPoints` | vertices with no faces | a point cloud |
| `UsdGeomBasisCurves`, cameras, lights, skinning, blend shapes | not read in stage 1; the log says what was skipped | curves and cameras come in stage 2 |
| `metersPerUnit` | the document unit, through `outUnit` | see question 2 |
| `upAxis` `Y` | a rotation of +90° about X on the **layer matrix**; the coordinates are kept | nothing is baked, and Freeze Matrix bakes it when the user wants |

### 4.2 Materials, stage 1

Only what `MeshIOMaterialSlot` can hold today, the four channels glTF fills:

| UsdPreviewSurface input | MeshLab slot field |
|---|---|
| `diffuseColor` from a `UsdUVTexture` | `baseColorTexture` |
| `diffuseColor` constant | vertex color, as glTF does with its base color factor |
| `normal` from a texture (`sourceColorSpace` raw, scale 2, bias −1) | `normalTexture`; `normalScale` 1 |
| `occlusion` | `occlusionTexture`; `occlusionStrength` 1 |
| `roughness`, texture or constant | `roughnessTexture` / `roughnessFactor` |
| `metallic`, `emissiveColor`, `opacity`, `opacityThreshold`, `clearcoat`, `ior`, the specular workflow | dropped, with a log line naming them |
| other surfaces (MaterialX, renderer-specific shaders) | not interpreted; the mesh keeps `displayColor` |

Textures come from files beside the stage or from inside the `.usdz`; the latter load as
in-memory assets with no file, as a `.glb`'s do, and Save Mesh to `.usdz` keeps them inside.
UDIM paths (`<UDIM>`) are not supported in stage 1, and say so.

**A known limit that stage 1 inherits.** `UsdUVTexture` reads named channels: roughness is
often the green channel of a packed image whose blue is metallic, as in glTF. The renderer
reads the roughness texture as the **mean of its three channels** (`fill_smooth.frag`,
`roughSample`), so a packed texture already renders wrong roughness from glTF today. Stage 3
adds the channel; stage 1 records the image and logs the channel it ignored.

### 4.3 Export, one layer

| MeshLab | USD |
|---|---|
| the layer | one `UsdGeomMesh` under `/World`, named from the file |
| faces (polygons where faux edges mark them) | `faceVertexCounts`, `faceVertexIndices` |
| vertex normals | `normals`, vertex interpolation |
| vertex or wedge texture coordinates | `primvars:st`, vertex or faceVarying |
| vertex or face color | `primvars:displayColor` (and `displayOpacity`), vertex or uniform |
| vertex scalar | `primvars:scalar`, float, vertex (by the vocabulary's user-facing word) |
| texture slots | one `UsdShadeMaterial` per slot with a `UsdPreviewSurface` and a `UsdUVTexture` per channel present (base color, normal, occlusion, roughness), bound by one `GeomSubset` per slot |
| document unit | `metersPerUnit`; nothing authored while the unit is not set |
| | `upAxis` `Z` |

`.usda` is text, `.usdc` binary, `.usdz` packs the textures. Textures follow the Save Mesh
rule: stored and unchanged ones are referred to or copied byte for byte, new ones written
next to the file, or inside it for `.usdz`. Unlike glTF's saver, which writes only the base
color texture, all four channels are written.

### 4.4 As implemented (2026-11-27)

`plugins/io_usd/usdplugin.cpp`, about 1,150 lines (glTF's is 2,100), builds LightUSD as a subproject with its
tools, tests, C API, bundled importers, audio and EXR off. The plugin compiles with
`QT_NO_KEYWORDS`, because LightUSD names members `slots`.

**Departures from 4.1–4.3**, each forced by what MeshLab or LightUSD offers:

| Planned | Implemented | Why |
|---|---|---|
| faceVarying normals as wedge normals | averaged per vertex, with a log line | `VCGFace` has no per-corner normal (`src/core/vcgmesh.h`); the OBJ path's `IOM_WEDGNORMAL` is a mask bit with no storage behind it in MeshLab's mesh |
| `upAxis` Y as a rotation on the layer matrix (question 1) | the rotation baked into the coordinates, with a log line | `MeshIOPlugin::load` returns no matrix; carrying one is the request-struct change of [Load Options §6.1](load_options.md#6-open-questions), not a stage 1 change. glTF, also Y-up, is not rotated at all today, so the two formats now disagree |
| unfallbacked `metersPerUnit` (question 2) | centimeters, by the specification, with a log line | the specification's value; one line to change if Unknown is preferred |
| texture coordinates as vertex or wedge by interpolation | always wedge, as glTF's importer does | one path; a vertex-interpolated `st` gives the same wedges |
| one slot per material | one slot per material whose diffuse color is a texture; a material with a constant diffuse color gives its faces a face color, and their slot index is −1 | a slot is a texture group to the renderer (`MeshGpuResourceCache`), and `mesh.textures[i]` must pair with slot `i` for `Document::loadMesh` |
| polygons triangulated by MeshLab | triangulated by Tydra (ear clipping), which also maps each triangle back to its authored face and corners; the diagonals become faux edges | the same map indexes faceVarying data, GeomSubsets and `leftHanded` |

Also: a prim whose world transform mirrors (negative determinant) has its winding reversed, as
`leftHanded` does; invisible prims are skipped; `UsdGeomPoints` become vertices; textures inside
a `.usdz` are decoded with `QImage` from the package (LightUSD's `USDZReader`), into in-memory
assets; on export a mesh with one slot on every face binds its material on the mesh, otherwise
through GeomSubsets whose family is a `partition`, or `nonOverlapping` when some faces have no
slot; edges are not written, with a log line; texture paths not copied are written relative to
the file, as PLY's and OBJ's are.

**A save with no mask** (a project save, Python's `save_current_mesh`) writes what the layer
holds: since 2026-11-27 `Document::saveMesh` fills it in with `Document::defaultSaveMask`, the
default the Save dialog proposes. Before, every exporter took it as everything the format can
hold, and in USD the white vertex colors that came with it displaced the layer's face colors.

**Measured** on build-release, this Mac:

- `tests/test_document.cpp`: four USD tests (the fixtures in `tests/data/usd/`, and a
  usda/usdc/usdz round trip of polygons, wedge coordinates, face colors, a material, an
  in-memory texture and the unit) and the decimal-comma locale test extended to `usda`.
- OpenUSD 26.11 (`usd-core`, outside the tree) on 12 files MeshLab wrote (the fixtures, a
  1,280-face sphere with face colors and the stage-0 cube, each as usda, usdc and usdz): all
  read as written, all 30 validators report nothing, the `.usdz` textures resolve inside the
  package. Not automated: `usd-core` is not a test dependency.
- All 1,272 `.usd*` files in LightUSD's repository opened one by one through `--script`: no
  crash, no hang; 508 load, 764 are refused with a message (no visible mesh or points, or
  invalid on purpose). The two refused `models/` files with a `Mesh` have no points, or
  reference an `.obj`, which needs LightUSD's OBJ plugin.
- 4.5 M triangles: load 0.27 s, save 0.66 s, peak resident memory 2.2 GB.
- Not checked: Quick Look and Blender on a written `.usdz`; the GUI's open and save dialogs.

## 5. Stage 2: scenes

### 5.1 The interface

A plugin returns one mesh. A scene needs the layers, each with its matrix, label, visibility
and material set. The proposal is the smallest addition that carries that: two optional calls
on `MeshIOPlugin`, `loadScene` and `saveScene`, taking a list of layer views
(mesh, matrix, label, visible, material set and texture context), and a capability flag.
`Document` drives them as it drives `load` and `save`. glTF can implement them later; nothing
else changes. **This is new framework code and needs approval before it is written.**

### 5.2 Export Visible Layers as USD

A document operation beside Save Project, not a filter: saving changes the disk, not the
document, by the project-persistence rules. One self-contained file, `.usdz` to send, `.usdc`
or `.usda` to work with.

| MeshLab | USD |
|---|---|
| each visible layer | a prim under `/World`: `UsdGeomMesh`; a point cloud as `UsdGeomPoints`; a polyline layer as linear `UsdGeomBasisCurves` |
| the layer matrix | the prim's `xformOp:transform`, **not baked**: moving a layer in another tool moves it as in MeshLab |
| the label | the prim name, made a valid identifier and unique, and the label itself as `displayName` |
| visibility | only visible layers are written, as the name says |
| materials and textures | as in 4.3, under `/World/Looks`, shared textures written once |
| rasters | `UsdGeomCamera` prims from the shot and lens; the images have no standard place and are not written |
| document unit | `metersPerUnit` |
| what USD has no place for | undo history, selection, render settings, the raster images: the `.mlp` keeps them |

The `.mlp` stays MeshLab's project. A second form, a root file referencing one USD file per
layer (what a `.mlp` does), is possible and not proposed: it would be a second project format
to keep in step.

### 5.3 Import as Separate Layers

The other half: each `UsdGeomMesh`, `UsdGeomPoints` and `UsdGeomBasisCurves` prim becomes a
layer, its world transform the layer matrix and its prim name (or `displayName`) the label;
cameras become rasters without images. A file written by 5.2 then comes back as the layers
that wrote it. A menu entry beside Import Mesh; plain Open keeps flattening.

### 5.4 Other formats that would use the same interface

The interface in 5.1 is not USD-specific. Three formats MeshLab already reads and writes can
hold several layers, and each is flattened into one mesh today (measured 2026-11-23: glTF
bakes every node transform, 3MF every build item and component, E57 merges every scan).

| Format | Layers and matrices | Materials | Rasters | Unit | Use |
|---|---|---|---|---|---|
| glTF / GLB | named nodes with transforms | metallic-roughness, as USD | cameras carry a field of view and aspect only; intrinsics and images only through `extras` | meters by definition | the first one after USD: same material work, cross-check of the two exports |
| E57 | one scan per point-cloud layer, with its pose | color only | **yes**: images with pinhole or spherical cameras and their poses | meters | scan projects: point clouds plus calibrated photos; points only, no meshes |
| 3MF | build items with transforms, named objects | base materials, color groups, textures | no | stated in the file | 3D printing: several parts with their placement |

Considered and not proposed: X3D/VRML and COLLADA (scene graphs, but superseded by glTF and
inconsistent across tools), FBX (writing needs Autodesk's proprietary SDK; a ufbx-based import
is possible), OBJ (named objects but no transforms). Bundler, NVM, COLMAP and Agisoft camera
files are photo alignments rather than scenes: an import of rasters onto existing layers.

## 6. Stage 3: the material model

Extended once, for USD and glTF together, since both carry what MeshLab drops today:

| Channel | glTF | USD |
|---|---|---|
| base color factor | `baseColorFactor` (now baked into vertex color) | `diffuseColor` constant |
| metallic, texture and factor | `metallicFactor`, blue of `metallicRoughnessTexture` | `metallic` |
| emissive, texture and factor | `emissiveTexture`, `emissiveFactor` | `emissiveColor` |
| opacity and its mode | `alphaMode` (`OPAQUE`, `MASK` with `alphaCutoff`, `BLEND`) | `opacity`, `opacityThreshold`, `opacityMode` |
| the channel a texture is read from | fixed by the specification (roughness green, metallic blue, occlusion red) | the `UsdUVTexture` output connected |

This touches `MeshIOMaterialSlot`, undo snapshots, the GPU cache, the fill shaders and the
glTF plugin, so by the mapping-table rule it gets its own table, field by field, approved
before code. It also fixes the roughness averaging of 4.2.

## 7. Validation

- **Fixtures written by us**, small `.usda` files committed under `tests/data/usd/`: a
  quad and two triangles with faceVarying coordinates and two materials by subsets; a
  `leftHanded` mesh on a Y-up stage with no unit and an invisible prim; a referenced mesh,
  once translated and once as a mirrored instance. Ours, so no licence
  question; public assets only in the local dataset, by [test_data.md](../test_data.md).
- **Round trips** in `tests/test_document.cpp`, beside the glTF ones: save a layer, load it
  back, compare positions, coordinates, slot indices and the four material channels.
- **OpenUSD as a checker**: when `usdchecker` (pip `usd-core`) is on the path, every file the
  tests write must pass it; skipped otherwise, as the datasets are.
- By hand, once per stage: the `.usdz` opens in Quick Look and in Blender.

## 8. Cost

`io_gltf` is about 2,000 lines; `io_usd` is expected to be similar, plus about 150 for the scene
interface of stage 2 and the two document operations. Stage 3 is a change across five places
whose size its mapping table will tell.

## 9. Open questions

1. **Up axis.** A rotation on the layer matrix (proposed), or baked into the coordinates as
   glTF's node transforms are? *Stage 1 bakes it (4.4): the layer matrix needs the load request
   of Load Options. And should glTF, Y-up by definition, be rotated too?*
2. **Unit fallback.** A stage that does not author `metersPerUnit` is in centimeters by the
   specification. Report centimeters (by definition, as glTF's meters), or Unknown (as for a
   format that does not say)? Stage 0 showed the importer can tell the two apart
   (`metersPerUnit_set`); Tydra's own fallback, 1.0, is used by neither choice. *Stage 1
   reports centimeters.*
3. **Scene interface** (5.1): approve the two optional calls, or another shape?
4. **Stage 3 timing**: right after stage 1, or after stage 2?
