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
| 2 | Scene I/O: export visible layers to one file, import as separate layers | done 2026-11-28 (5.5); cameras left out (5.3) |
| 3 | The material model, extended once for USD and glTF: data only, nothing new drawn | done 2026-11-28 (§6.1) |

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
  specification is 0.01 (centimeters), and `upAxis`, whose fallback is **Y**. MeshLab keeps
  a file's own frame ([Data Model](../data_model.md), "Frames are kept"); its default view
  is Y-up.
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
| `metersPerUnit` | the document unit, through `MeshIOLoadContext::unit` | see question 2 |
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
read the roughness texture as the **mean of its three channels**, so a packed texture rendered
wrong roughness from glTF; stage 3 added the channel (§6.1).

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
| | `upAxis` `Y`, as MeshLab's default view shows the coordinates (decided 2026-11-28; stage 1 wrote `Z`). A Y-up stage read as it is comes back as it was; a Z-up stage read as it is comes back declared Y-up, until a layer records the frame it came from (5.3) |

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
| `upAxis` Y as a rotation on the layer matrix (question 1) | the coordinates kept as the file has them; the `rotateYUpToZUp` load option, off by default, rotates them, and the log says which (since 2026-11-28; stage 1 first rotated every Y-up stage automatically) | MeshLab keeps a file's own frame (question 1). glTF, Y-up by definition, has the same option |
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
layer, its world transform the layer matrix and its prim name (or `displayName`) the label.
A file written by 5.2 then comes back as the layers that wrote it. A menu entry beside Import
Mesh; plain Open keeps flattening.

**Cameras are left out of stage 2** (decided 2026-11-28), both ways: a camera with no image is
not a raster, and the place for it is the annotations framework to come, which will attach
geometric information of several kinds to layers.

**Where an imported layer comes from is an open problem** (recorded 2026-11-28). For now a
layer imported from a scene has no file of its own: it is not reloadable, and *Save* asks for a
new file. A layer could instead record the scene file and its prim (as `package.usdz[entry]`
names a texture inside a package), so that *Reload* reads that prim again and the stored-state
mark knows the layer still holds it; that needs provenance below the file level in
`MeshEntry`, in the stored-state record and in the project, and is worth doing once more than
one format imports scenes.

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

### 5.5 As implemented (2026-11-28)

**Interface** (`src/plugins/meshioplugin.h`): `holdsScenes(filename)`, `loadScene(filename,
layers, cb, context)` filling `MeshIOSceneLayer`s (mesh, load mask, material set, matrix,
label, visibility), and `saveScene(filename, views, options, cb)` taking
`MeshIOSceneLayerView`s (mesh, mask, matrix, label, texture context). **Document**:
`importLayers(filename, error, options)` and `exportVisibleLayers(filename, error)`, *File >
Import Layers...* and *File > Export Visible Layers...* (offered for the formats whose plugin
holds scenes), Python `ms.import_layers(path, **options)` and `ms.export_visible_layers(path)`.
An import is one undo step, recorded as `import_layers` with its load options; an export changes
no layer and no layer's file.

**Import.** Each `Mesh`, `Points` and `BasisCurves` prim is a layer in the prim's own
coordinates; its world transform is the layer matrix, its `displayName` (or name) the label;
an invisible prim is a hidden layer. Curves come from Tydra's tessellation, one polyline per
curve, its vertices shared; a polyline whose last point repeats its first closes on it. The
`rotateYUpToZUp` option turns a Y-up stage by a rotation in front of every layer matrix, the
coordinates untouched. Plain *Open* flattens as before, now with curve prims as edges too.

**Export.** Each visible layer is a prim under `/World`, named from its label and unique,
with the label as `displayName` and the matrix as `xformOp:transform`, not baked. A layer with
faces is a `Mesh`, without a `Points`; its edges are linear `BasisCurves` beside it (alone for a
polyline), one curve per chain of `Clean::EdgeMeshChains`, so a polyline's vertices stay shared
when read back. Materials of every layer go under `/World/Looks`; a texture file or image used
by several layers is written once. A one-layer *Save* goes through the same writer, without a
transform or `displayName`, and now writes edges too (stage 1 dropped them).

**Shared with load and save** (the table approved before the move): `Document::prepareImportedMesh`
and `logImportedMesh` are the post-import steps of `loadMesh`, `reloadMesh` and `importLayers`
(reload used to skip registering the PBR channel textures, so a reloaded layer lost its normal,
occlusion and roughness maps); `texturesToSave` and `maskToSave` the save preparation of
`saveMesh` and `exportVisibleLayers`; `Document::CallbackScope` the progress-callback setup
each of them repeated.

**Not done**: cameras (5.3); provenance of an imported layer (5.3); per-layer up axis (a Z-up
stage read as it is comes back declared Y-up); a branching polyline (a vertex of degree 3 or
more) comes back with that vertex once per chain; the mirrored-matrix winding that the
flattened import reverses is left to the layer matrix.

**Measured**: `usdScenesRoundTripVisibleLayers` (usda, usdc, usdz: three visible layers and a
hidden one; labels, matrices, local coordinates, a polyline and a closed loop back, one undo
step, the flattened Open placing each prim, a polyline saved alone keeping its edges);
`reloadKeepsMaterialChannelTextures`.

## 6. Stage 3: the material model (data only)

**Scope, decided 2026-11-28.** Stage 3 is about what MeshLab *keeps*: every material input that
glTF and USD share is read, kept on the layer and written back by both, so that neither format
loses material data on a round trip. It draws nothing new. The view's PBR material mode is not
physically based today -- Blinn-Phong, roughness choosing the highlight's exponent and strength,
a constant ambient, no Fresnel and no environment lighting (`shaders/fill_smooth.frag:150-185`)
-- and metallic and transparency only mean something under a real metallic-roughness shading
with image-based lighting. That is a rendering proposal of its own; it will also decide what the
current mode becomes. The Plain material mode reads none of these fields.

glTF 2.0 (core metallic-roughness model) and USD's `UsdPreviewSurface` share one set of inputs:
base color, metallic, roughness, normal, occlusion, emissive, opacity with a cutoff. They differ
in that glTF packs roughness and metallic into the green and blue of one image where USD
connects each channel by name, and in what each adds beyond them (USD: specular workflow,
clearcoat, ior; glTF: extensions). One slot model carries both.

**Measured 2026-11-28** (every read and write of `MeshIOMaterialSlot`, with the evidence below):
the glTF importer indexes its slots by material and its faces' texture index by base-color
image, while every consumer reads the face index as the slot; the renderer reads a roughness
texture as the mean of its channels; the glTF exporter writes the base color only, and
tinygltf's omitted defaults make every exported material fully metallic; USD reads and writes
neither normal scale nor occlusion strength; undo copies the material set whole, and `.mlp`
stores no material.

| # | Current | Target | Evidence | Verdict |
|---|---|---|---|---|
| 0 | glTF: slots by material, a face's texture index by base-color image, numbered in visiting order; a face whose material has no base texture gets 0 | one slot per glTF material, each face's texture index its material's slot | `gltfimportplugin.cpp:1249-1295`, `:1463`; the face index read as the slot at `meshgpuresourcecache.cpp:486-501`, glTF export `:1798`, USD `writeLayer`, `texturefilterplugin.cpp:1555` | **fix first** |
| 0b | a slot with no base-color texture is skipped by the GPU cache, its other maps unloaded | white albedo, the slot's other maps loaded | `meshgpuresourcecache.cpp:579-605`; needed once row 0 gives such materials slots | **change** |
| 1 | roughness drawn from the mean of a texture's three channels | `MeshIOMaterialTextureRef::channel` (RGB, R, G, B, A), set by the importers, written by the exporters, and the one existing roughness read uses it | `fill_smooth.frag:156-161`, `fill_flat.frag:166-169`; glTF packs roughness in green, USD names the output it connects (`usdplugin.cpp:565-568`) | **add**; the one draw change, as a fix |
| 2 | USD reads and writes neither `normalScale` nor `occlusionStrength` | the normal scale through `UsdUVTexture` scale and bias, both ways; occlusion strength, which USD has no input for, logged | `usdplugin.cpp:502-515`, `:814-877` | **complete** |
| 3 | glTF export: base color only; every material fully metallic by omitted defaults | the whole slot: normal, occlusion, metallic-roughness (one image packed from two when they differ, as glTF requires), factors, alpha | `gltfimportplugin.cpp:1781-1855`; `tiny_gltf.h:733-759` | **complete** |
| 4 | glTF `baseColorFactor` baked into vertex colors when there is no `COLOR_0`, ignored otherwise; USD's constant `diffuseColor` dropped on textured materials | `MeshIOMaterialSlot::baseColorFactor` (RGBA, default 1); untextured materials keep today's face or vertex colors (question 5) | `gltfimportplugin.cpp:1297-1311`, `:1406-1410`; `usdplugin.cpp:502-521` | **add**, kept, not drawn |
| 5 | metallic: not read by glTF, dropped with a log line by USD | `metallicTexture` (glTF: the blue channel of the metallic-roughness image) and `metallicFactor` | `gltfimportplugin.cpp:1283-1284`; `usdplugin.cpp:535-550` | **add**, kept, not drawn |
| 6 | emissive: not read by glTF, dropped by USD | `emissiveTexture`, `emissiveFactor` | as row 5 | **add**, kept, not drawn |
| 7 | alpha: glTF `alphaMode`/`alphaCutoff` not read; USD `opacity` kept only on untextured faces, `opacityThreshold` not even logged (4.2 says it is) | `alphaMode` (opaque, mask, blend), `alphaCutoff`, `opacity` | `usdplugin.cpp:521`, `:535-550`; this document 4.2 | **add**, kept, not drawn |
| 8 | the four texture references walked by hand in about eight places | one `forEachTextureRef(slot, fn)` over all six (with metallic and emissive), used by every walk: texture registration on load, removal, the layer widget's list, the GPU cache | `document_io.cpp:291-305`, `textureassociationutils.cpp:516-558`, `layerwidget.cpp:666-679`, `meshgpuresourcecache.cpp:515-541` | **merge** |
| 9 | two near-identical batch-fill blocks in the GPU cache | one | `meshgpuresourcecache.cpp:758-830`, `:853-941` | **merge** |
| 10 | `MeshIOMaterialSet::textureAssets` always empty inside the document (`normalizeMaterialSet` moves images to `MeshEntry::textureAssets`), yet memory accounting walks it | those reads removed | `document_internal.cpp:248-281`; `document_memory.cpp:61`; `documentundomanager.cpp:91,112` | **remove** |

Totals: one fix and one change it needs (0, 0b), one added field drawn as a fix (1), two
completions (2, 3), four added fields kept but not drawn (4-7), three cleanups (8-10): eleven
rows. Undo and `.mlp` need nothing. About 500-700 lines, most of them in the glTF importer and
exporter and in the USD material code.

Approved 2026-11-28, with untextured materials kept as face or vertex colors (question 5).

### 6.1 As implemented (2026-11-28)

`MeshIOMaterialSlot` gained `metallicTexture`, `emissiveTexture`, `baseColorFactor` (RGBA, the
alpha the opacity), `metallicFactor` (0 unless a file says otherwise), `emissiveFactor`,
`alphaMode` and `alphaCutoff`; `MeshIOMaterialTextureRef` gained `channel` (RGB, R, G, B, A);
`forEachTextureRef(slot, f)` walks all six references, and every former hand-written walk uses
it (reference normalization, texture registration on load, the load log, texture removal, the
layer widget's list, which now tags Metal and Emissive).

- **glTF import** (row 0): one slot per material whose base color texture could be read, each
  face's texture index its material's slot, a material with no base color texture giving its
  faces its base color factor and slot -1. Base color assets sit at their slot's index, channel
  images after, once each. Every input is read: roughness green and metallic blue of the
  metallic-roughness image, occlusion red, emissive, the factors (tinygltf supplies the
  specification's defaults), alpha mode and cutoff. A textured material's base color factor is
  kept in the slot and no longer baked into vertex colors.
- **glTF export** (row 3): one material per slot the faces use, written whole, each image once;
  the metallic-roughness image is the slot's own when roughness and metallic name it in green and
  blue (or there is no metallic map and the factor is 0), else packed from the two maps;
  a single-channel map in another channel than glTF's is repacked; factors are written always,
  so a slot with no metallic of its own no longer reads as fully metallic.
- **USD** (row 2): channels from the connected `UsdUVTexture` output, both ways; each texture's
  `scale` carries the factor that multiplies it (base color, roughness, metallic, emissive), the
  normal scale as scale and bias; opacity is the base texture's alpha (or a value), a threshold
  a mask. The converter makes one texture per connected output, so the base color and its
  alpha are matched by prim, not by texture id. USD has no occlusion strength: a strength other
  than 1 is not written (a save has no channel to the log, so this is said in the code only).
  `ior` and `displacement` are now noted as dropped; `metallic`, `emissiveColor` and an
  opacity read from the base texture no longer are.
- **Drawn** (row 1): the roughness texture's channel reaches the fill shaders in the roughness
  flag (mode + 4 × channel), so the uniform block's layout is unchanged; a glTF roughness is
  read from green, no longer from the mean of three channels. Nothing else new is drawn.
- **Row 0b dropped**: with untextured materials kept as colors (question 5), no slot is ever
  without a base color texture, so the GPU cache needs no change for it.
- **Cleanups**: the GPU cache's two batch-filling blocks are one (`takeGroupMaterial`), and the
  memory accounting no longer walks the material set's assets, which only travel in from an
  importer (the field's comment now says so).

Measured: `gltfFacesUseTheirOwnMaterial` (fixture `tests/data/gltf/shared_materials.gltf`);
`materialsRoundTripThroughGltfAndUsd` (every field through `.glb` and `.usda`, the packed image's
channels, a red roughness map repacked to green). Not measured: the draw of the roughness
channel, which only a render probe sees.

**Left to the rendering proposal**: drawing the base color factor, metallic, emissive and
alpha; sRGB decoding of base color and emissive (every image is uploaded as plain RGBA8,
`meshgpuresourcecache.cpp:566-568`); the global PBR values written to the uniform block and
always overwritten (`renderwidget_internal.h:403-448`). **A separate fix**: six texture filters
rebuild a layer's slots from base colors only, dropping its normal, occlusion and roughness maps
and its factors (`replaceTextureAssociations`, `textureassociationutils.cpp:359-375`).

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

1. ~~**Up axis.**~~ Settled 2026-11-28: MeshLab keeps a file's own frame, for every format.
   A rotation is the user's choice, the `rotateYUpToZUp` load option of USD and glTF, off by
   default and logged ([Data Model](../data_model.md), "Frames are kept").
2. **Unit fallback.** A stage that does not author `metersPerUnit` is in centimeters by the
   specification. Report centimeters (by definition, as glTF's meters), or Unknown (as for a
   format that does not say)? Stage 0 showed the importer can tell the two apart
   (`metersPerUnit_set`); Tydra's own fallback, 1.0, is used by neither choice. *Stage 1
   reports centimeters.*
3. ~~**Scene interface** (5.1).~~ Approved 2026-11-28: `holdsScenes`, `loadScene`, `saveScene`.
   Cameras are left to the annotations framework, and imported layers have no file of their
   own for now (5.3).
4. **Stage 3 timing**: right after stage 1, or after stage 2?
5. **Untextured materials** (stage 3, row 4). Kept as today -- face or vertex colors, so that the
   renderer's grouping by texture is unchanged, and their metallic, emissive and alpha are lost
   on a round trip -- or slots of their own carrying only factors, which keeps the material but
   makes every material a draw group?
