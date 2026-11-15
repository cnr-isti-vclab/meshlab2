# Data Model

See also: [Architecture](architecture.md) · [Rendering](rendering.md) · [Memory Accounting](memory_accounting.md)

## Core Idea

MeshLab is **single-document, multi-view**: one `Document` owns canonical meshes and document-level state; widgets consume that state via Qt signals; the shared GPU cache lives in `Document`, while per-view rendering state stays in each `RenderWidget`.

## `Document` Ownership

`Document::MeshEntry` is the canonical mesh record. Each entry stores:

- identity/revision keys: `meshId`, `geometryRevision`, `materialRevision`, `selectionRevision`
- render placement: `transform`
- source metadata: `name`, `sourcePath`, `ioMask`
- texture metadata: `textureFileNames`, `textureFilePaths`, `textureAssets`
- material metadata: `materialSet`
- mesh state: `visible`, `VCGMesh mesh`

`Document` also owns: remembered current mesh/raster indices, explicit active layer kind, document log (application / VCG / error sources), I/O and filter plugin managers, shared GPU cache, `DocumentUndoManager`, operation cancel flag, and memory accounting APIs (`cpuMeshMemoryStats`, `cpuImageMemoryStats`, `undoMemoryStats`, `gpuMemoryStats`). Optional Python bindings borrow or own a `Document` through `MeshSetCore`; the embedded console borrows the live `MainWindow` document and does not take ownership.

## Mesh Data Type

`VCGMesh` is a `vcg::tri::TriMesh` specialization. `VCGVertex` has fixed coordinate, normal, color, quality, and flag components plus optional OCF texture-coordinate and curvature-direction data. `VCGFace` has fixed vertex references, normal, color, quality, and flags plus optional OCF wedge texture coordinates, principal curvature directions, and algorithm-only adjacency/mark data. Principal curvature directions live on vertices or faces (`PD1`, `PD2`, `K1`, `K2`, both storable and kept by undo), with the maximal curvature first on either, as the libigl filter writes them and the curvature decorator draws them: *Compute Principal Curvature Directions per Face (TextureSignalProcessing)* writes faces, the other curvature filters vertices, and the two *Transfer Curvature Directions* filters convert between them through vcglib's cross-field interpolation, which can return the two directions in either order, so they put the pair back. The layer panel lists vertex directions as `CD` and face directions as `FCD`; no file format stores either. `VCGEdge` has fixed endpoint references, RGBA color, scalar, flags, and the edge half of vertex-edge adjacency; the vertex half is an optional OCF component, enabled only while a filter declares the `VE` preparation code. Because edge color storage is always allocated, `MeshEntry::ioMask & IOM_EDGECOLOR` is the semantic indication that meaningful per-edge colors are available.

A face's texture is the index of its first wedge, `WT(0).N()`, and a negative one -- vcglib's -1 -- means the face has **no texture**: its UVs mean nothing. Every face added to a mesh with OCF wedge texture coordinates starts that way, so the cap Trim Surface by Plane closes a cut with is untextured. Readers leave such a face out rather than reading texture 0 at its UVs: it is drawn untextured and is absent from the UV view and UV-space selection (`vcgFaceTextureGroup`, `vcgFaceCornerUV`); the UV measures count it as surface the mapping does not cover (`UvLayerUtils::buildUvLayer`); the atlas and packing filters carry it over unchanged. So anything that **writes** UVs must also set the index -- 0 for a single texture, as every parametrization filter does -- or the faces it maps read as untextured. Per-vertex texture coordinates carry no per-face texture, and their index is not reliably set, so it is never read as "no texture".

## Raster Data Type

`Document::RasterEntry` is the canonical raster-layer record. Each entry stores:

- identity/revision keys: `rasterId`, `imageRevision`, `cameraRevision`
- source metadata: `name`, `sourcePath`
- layer state: `visible`, `currentPlaneIndex`
- camera state: `CameraShot shot`, a pinhole camera, and `CameraLens lens`
- image data: `std::vector<RasterPlane> planes`

**Lens boundary.** The camera every consumer sees -- filters, depth buffers, the raster view --
is the pinhole `shot`, always. The `lens` (`vcg::LensDistortion<double>`, OpenCV's Brown-Conrady
model, `vcglib/vcg/math/lens_distortion.h`; the identity for a pinhole photo) relates the photo as
shot, which is what a plane file holds, to the image the pinhole camera describes, which is what
a plane holds in memory. It is applied where a plane is decoded, `Document::ensureRasterPlaneImage
(raster, plane)`, and by *Import Image to Current Raster*: `CameraShot::undistortImage` resamples
the photo at the same size and with the same intrinsics, leaving transparent what the photo does
not cover and what lies past a fold of the lens model (`IsInvertibleAt`). A plane whose file was
written from memory (`saveRasterPlaneImage`, a project save) is in the camera's frame already and
says so (`RasterPlane::sourceUndistorted`, `undistorted="1"` in the `.mlp`), so it is not
undistorted twice. `setRasterLens` is an undo step that advances both revisions and drops the
planes decoded from photos, which then decode through the new lens; an edit made in memory to
such a plane is dropped with it, as it was in the old lens's frame. The layer panel's
thumbnails read the files directly and show the photos as shot. See
[Camera Lens Model](proposals/camera_lens_model.md).

`RasterPlane` records a semantic role (`RGBA`, `MaskUInt8`, `MaskFloat`, `DepthFloat`, or extra float/RGBA planes), display/source metadata, pixel size, and optional `QImage` payload. The current implementation loads ordinary raster images as RGBA planes and keeps the plane abstraction ready for mask/depth/extra image channels.

The planes of a raster share its camera, and everything that reads a raster's image -- the raster view, the color projection and texturing filters -- reads its current plane at the camera's `ViewportPx`, pixel for pixel. *Import Image to Current Raster* (`filter_layer`) replaces the current plane with an image file or adds one as a new current plane, keeping the camera. An image of another resolution with the same aspect ratio can replace a raster's only plane, and the camera then takes that resolution (`vcg::Camera::RescaleViewportPx`: `ViewportPx` and `CenterPx` scale with the pixels, `PixelSizeMm` inversely, `FocalMm` stays; the lens, being normalized, does not change); any other plane must have the camera's resolution.

`CameraShot` (`src/core/camerashot.*`) wraps a VCG shot for raster and snapshot workflows. It stores viewport, pixel size, focal length, camera type, and extrinsics -- no lens distortion, which is the raster's `lens` -- and exposes projection/unprojection, depth, view-matrix, and projection-matrix helpers. `readVcgCameraXml` / `writeVcgCameraXml` are the one reader and writer of the `VCGCamera` element, for the `.mlp` and for *Import/Export Cameras* (`filter_layer`), with the lens in `LensModel="brown" LensParams="k1 k2 p1 p2 k3 k4 k5 k6"`, written only when it is not the identity; `LensDistortion`, old MeshLab's radial model, is written as `"0 0"` and a non-zero one read is dropped with a warning.

## Layer Provenance

A mesh layer carries three separate pieces of information about where it came from and whether it is saved.

| | Field | What it is |
|---|---|---|
| Name | `MeshEntry::name` | The label: what the panel, the grid-view captions and a project's `label` attribute show, and what the user edits. On load it is the file's base name, or the project's `label`. Renaming changes only this. `addMesh()` and `duplicateMesh()` make it unique (" 2", " 3", ...); loading and renaming do not check. |
| File | `MeshEntry::sourcePath` | The full path of the layer's file, or empty: the file it was loaded from, or last saved to -- saving names the layer's file, as in any document application, and a label that was still the old file's name follows. Reload reads it; a project writes it (relative to the project folder) as the `MLMesh` `filename`. Undo does not take a save back: restoring a state puts back the file the document last read or wrote for that layer, not the one the state was captured with. |
| Stored | `Document::meshStoredState`, `meshTextureStoredState` | Whether a file holds what the document holds: `NotStored` (no file), `Changed` (a file did when last read or written, and the layer's geometry, material or selection revision has moved since), `Stored`. Recorded at load, reload and save, in `m_storedMeshes` (by `meshId`: file and the three revisions) and `m_storedImages` (by `QImage::cacheKey`: the file holding exactly that texture image, or the mesh file for one packed in a glb or 3mf). Neither is part of the undo state, so undoing past a save shows the layer changed and undoing back shows it stored again. That needs revisions that are never reused, so material and selection revisions draw from the document's geometry counter. A selection undone through the delta path gets a new revision, so a layer undone back to a saved selection shows changed: conservative, never the reverse. |

The Layers panel marks each layer, and each texture and raster plane under it, by its stored state (the row above), the same way for all three: no mark when a file holds it, `●` when one did and it has changed since, `○` when no file holds it. The tooltip says which file, or that it is inside its mesh file (a texture packed in a glb). Layers added through `Document::addMesh()` (the filters that create layers, the measure tool) and through `duplicateMesh()` have no file and are `○` until saved. A selection change rebuilds the panel only when it turns a layer's mark, so painting a selection stays cheap. These marks replaced the `[D]`, `[G]` and `*` tags, which said whether a layer had been loaded from a file, not whether the file held it.

A texture can be saved on its own (`Document::saveMeshTexture`, *Save Texture...* on a texture row, Python `save_current_mesh_texture`), and a raster plane likewise (`saveRasterPlaneImage`, *Save Plane Image...*), in any format the installed image plugins write, picked by the suffix. The texture or plane then lives in that file; when it is not the file the layer's mesh refers to, the layer is changed (its material revision moves) until the mesh is saved too.

The tag is part of the displayed text, not a field, so the panel cannot sort or filter on it. A separate provenance column (from disk / from disk and changed / generated) is the intended replacement.

## Signals and Reactivity

- mesh lifecycle: `meshAdded`, `meshRemoved`, `meshDataChanged`, `meshSelectionChanged`
- raster lifecycle/data: `rasterAdded`, `rasterRemoved`, `rasterDataChanged`
- selection/visibility: `currentMeshChanged`, `currentRasterChanged`, `currentLayerChanged`, `meshVisibilityChanged`, `rasterVisibilityChanged`
- progress: `loadProgress*`, `filterProgress*`
- logging: `logCleared`, `logMessageAdded`
- undo/redo: `undoRedoStateChanged`

## I/O Model

`Document::loadMesh()`: resolves import plugin, runs plugin load, merges duplicate vertices when the file is STL (a triangle soup with no shared vertices; on by default, `setMergeStlDuplicateVertices()`, driven by the `document.mergeStlDuplicateVertices` preference), compacts imported mesh storage, updates the bbox, computes the normals the file lacks (`computeMissingNormals`: imported vertex normals are kept as given, smooth ones generated only when absent, and face normals always computed unless the file brings them, so every mesh enters the document with valid face normals), initializes transform and material set, resolves texture paths/assets, logs stats, records the stored state and the document folder, appends entry, emits signals. `reloadMesh(index)` follows the same path while preserving mesh identity. The VCG PLY path recognizes edge endpoint indices plus `red`/`green`/`blue` and optional `alpha` properties (including `diffuse_*` aliases), sets `IOM_EDGECOLOR`, and defaults missing edge alpha to 255.

`Document::loadRasterImage()`: loads a `QImage`, creates a `RasterEntry` with an RGBA `RasterPlane`, appends the raster layer, makes it current, and emits raster/current-layer signals. Ordinary image loads do not synthesize a calibrated camera; callers such as snapshot capture can pass a `CameraShot` through `addRasterImage(...)`, and raster-mode mesh projection is enabled only when that shot is valid.

`Document::loadMeshLabProject()`: parses MeshLab project (`.mlp`) files directly -- or a COLMAP sparse model, opened by its `cameras.txt` or `cameras.bin` (`document_colmap.cpp`, text or binary), whose images become rasters with their pinhole cameras and lenses and whose points a colored point cloud layer, with the reprojection of COLMAP's observations logged; `Document::isProjectFile` says which files it takes -- loads referenced meshes through the normal mesh plugin path -- passing each one's project label and transform to `loadMesh()`, so a layer is announced already named and placed rather than at the origin under its file name -- creates raster entries from project raster planes, stores project `CameraShot` data, records a `load_project` script action, and groups the operation under one undo step. Missing mesh files are logged and skipped; missing raster plane images are logged but still kept as plane metadata with a viewport-size fallback when possible.

`Document::saveMesh(...)`: resolves export plugin, passes `MeshIOSaveOptions` (mask, binary, embed textures, copy associated textures, Draco options). Textures follow one rule in every format that stores them (PLY, OBJ, glTF): one whose image a file holds -- by `m_storedImages`, not by the asset's own path, which undo may have brought back stale -- is copied or referred to there (PLY and OBJ by a path relative to the mesh unless "copy associated textures" is on; glTF always copies, and no longer re-encodes a JPEG); one no file holds is written next to the mesh from its image under its name, suffixed when that is taken, whatever the options. The saver reports where each texture now is (`MeshIOTextureContext::savedTexturePaths`); the layer then takes the saved file as its own and the textures their files, and the stored state is recorded. PLY export exposes edge indices and edge color in the save mask and writes per-edge RGBA in both ASCII and binary modes when requested.

`Document::saveMeshLabProject(...)`: writes a `.mlp` project from the current document. It changes the document only through `saveMesh`: a layer it writes takes the written file as its own and is recorded as stored.

Each mesh layer is sorted by its stored state (see [Layer Provenance](#layer-provenance)). Every write goes through `saveMesh`, so it writes the layer's textures that no file holds beside it, takes the written file as the layer's own and records it as stored: the next project save writes only what changed since.

| Layer | What the save does | Project `filename` |
|---|---|---|
| No file (`○`) | Always written, as PLY, to `meshes/<name>.ply` in the project folder. The name keeps to `[A-Za-z0-9_.-]` and gets `_1`, `_2` ... when the name is taken on disk or by another layer in the same save, so two layers called "scan" become `scan.ply` and `scan_1.ply`. | `meshes/<name>.ply` |
| File in the project folder, changed (`●`), with "Re-save changed meshes" on (the default) | Written back over its own file. | its file |
| File outside the project folder | Refused unless "Copy files to project folder" is on; then saved into `meshes/` under a unique name in its own format (PLY when that cannot be written), with every texture copied beside it. A layer is saved rather than its file copied, because its texture references are relative to it. | the copy |
| File in the project folder otherwise | Not written; referenced where it is. | its file |

The "no file" case used to be skipped when the layer was unmodified, which dropped every filter-created layer from the saved project without a message; the regression test is `saveMeshLabProjectKeepsLayersWithoutAFile`.

Options (`MeshLabProjectSaveOptions`, offered by the Save Project dialog): `onlyVisibleMeshes` leaves hidden layers out of the project altogether; `saveModifiedMeshes` is the re-save above; `copyFiles` lets the save proceed when some referenced meshes or rasters live outside the project folder. Without it, such a project is refused with an error rather than saved with paths that would break when the folder moves. With it meshes are saved into `meshes/` as above and raster images copied into `images/`, each under a unique name, and the project -- and the document -- name the copies. A snapshot plane with no file is written to `images/` (`saveRasterPlaneImage`). Paths in the file are relative to the project folder.

**Folder** (`Document::directory()`): where new saves start, what the Layers panel shows paths against (`displayPath`: relative when inside the folder or one level up, absolute otherwise), and where a `filesave` parameter given as a bare name goes. It is the project's folder once a project is loaded or saved; before that, the folder of the first mesh loaded or saved; or what the user sets with the status-bar button, before the unit. `clearAllLayers()` clears it. It never resolves a mesh's own textures, which stay relative to that mesh's file as every format stores them. Not part of the undo state.

**Unit** (`Document::unit()`, `LengthUnit` in `lengthunit.h`): what the coordinates are measured in --
micrometers, millimeters, centimeters, meters, kilometers, inches or feet -- as far as it is known. Most
mesh formats do not say, so it is `Unknown` until the user sets it, or a project that holds one or
a file that states its unit does.
It is a label and changes no coordinate; it names the absolute half of an abs/perc parameter and the lengths the measure tool shows and saves.
The user sets it with the *Set Unit* filter (`set_unit`, category `Document`, in the layer plugin), which
the status bar's unit menu runs, so a change is in the action history and a generated script like any
other (`ms.set_unit(unit="mm")`; the parameter spells each unit as `LengthUnits::scriptId()` does, the
symbol in ASCII, `um` for µm, and `not_set`). Its default is the `@documentUnit` token, the unit the
document has, so the filter panel opens on it. *Scale to Known Length* sets it too, when the document has
none, to the unit of the length it is given; when the document has one, it converts the length into it
(`LengthUnits::metersPer`) and leaves the unit alone. It is
in the undo state (`UndoState::unit`) and setting it is an undo step ("Set Unit"; the same unit again is
no step), `File > New` (`clearAllLayers()`) forgets it, and `unitChanged()` tells the interface. In the
project it is `<LengthUnit name="mm"/>` after the visibility sets, written only when set and read by its
symbol, so an old version skips it and a symbol this version lacks reads as not set; a project without
one leaves the unit of the document it is loaded into as it is; it is applied before the project's layers
are loaded, so that their files have to agree with it. Old MeshLab reads only `MeshGroup` and `RasterGroup`
and skips the other elements, so a project with the unit opens there unchanged.

*Formats that know their unit* report it through the extended `MeshIOPlugin::load` (`outUnit`, left
`Unknown` by the formats that say none: PLY, OBJ, STL, OFF): glTF/GLB and E57 are in meters by their
definitions, and 3MF carries a unit in the file -- millimeters when it names none, which is what the
specification says. `Document::loadMesh` hands it to `adoptFileUnit`: it becomes the document's unit only
when the loaded layer is the only one and no unit is set, so a document with layers of unknown unit is not
told a guess; a unit the user chose is kept, a disagreeing file is reported in the log as a warning, and
nothing is ever converted. The adoption is part of the load's undo step. On the way out `saveMesh` puts the
document's unit in `MeshIOSaveOptions::unit`, which the 3MF writer records (it can name every unit but
kilometers, which it leaves out, with a note in the log); glTF and E57 have nowhere to say it.

**Visibility sets** (`Document::visibilitySets()`): a name and the persistent `meshId`s of the layers that show together, kept in the document, in the `.mlp` and in the undo state (`UndoState::visibilitySets`, captured and restored with the layers, so their ids stay valid): adding, redefining, renaming and deleting one are undo steps, each with the usual guard so that one made inside an open step -- a project load -- belongs to it. Showing one is not a step, as the layer panel's eye is not. A filter can leave one (`MeshFilterRunResult::visibilitySets`, by layer index): the framework stores it with `addVisibilitySet` inside the filter's own undo step, from the indices as they stand when the filter returns, under `uniqueVisibilitySetName` (a taken name gets " 2", " 3" ..., as a layer's does), so each run leaves a set of its own. A filter can take a set as a parameter (`visibilityset`, whose value is the set's name; `indexOfVisibilitySet`, `visibilitySetMeshIndices`). `moveVisibilitySet` reorders them (an undo step; the Alt+number keys follow the order). `addToVisibilitySet` and `removeFromVisibilitySet` edit a set a layer at a time (each an undo step unless it changes nothing). `showVisibilitySet(i, addToVisible)` goes through `setMeshVisible`, so it acts on the current view, as the layer panel's eye does; `visibilitySetIsShown(i)` is whether the visible layers are exactly the set's; a layer that is gone is not in the set any more, and `clearAllLayers()` (File > New) drops the sets. In the file they follow `RasterGroup` as `<VisibilitySets><VisibilitySet name="..." meshes="0 2"/></VisibilitySets>`, each number the position of an `MLMesh` among those written, so that a layer left out of the file (`onlyVisibleMeshes`, an empty path) does not shift the others; a set left with no layer is not written, and a loaded one maps the positions to the ids the layers got. Old versions skip the element. Rasters have no sets.

What the file records for a mesh layer is its `label` (the name), `filename` and transform (six decimals); visibility is not stored. A raster layer records its `label`, camera (`VCGCamera`) and planes, each with its own file; a plane without a file (a snapshot) is written to `images/<name>.png`, or `images/<name>_<plane>.png` after the first. A loaded raster makes its last plane current, as MeshLab always has, so that a plane added to a raster is current again after a reload; which plane was current is not stored. Relative paths are resolved as text (`QDir::cleanPath`), the inverse of how they are written, so that a project in a symlinked folder (`/tmp`, `/var` on macOS) still finds the files outside it.

Also exposes: `openDialogFilter()`, `saveDialogFilter()`, `saveMaskCapability()`, `importSupportedExtensions()`, `exportSupportedExtensions()`, `importPluginInfos()`, `exportPluginInfos()`.

## Filter Model

Metadata: `filterInfos()`. Descriptors can be loaded declaratively from filter JSON resources via `FilterDescriptorLoader`, including validated ordered `categories` (first entry primary, later entries cross-listings), structured `provenance`, CSL-style publication `references`, tags, requirements, input/parameter preparation codes (`FF`, `VF`, `VTex`/`VT`, `WTex`/`WT`, `BorderFF`, `BorderVF`, `CurvDir`, normals, bbox, marks), cleanup hooks, dynamic bounds/default tokens, texture input/output references, mesh parameters with their own requirements/preparation, point/vector parameters, camera/render-state parameters, incremental selection, `pythonName`, and output-modifies codes. `categories` is the authored field; a legacy single `menuPath` string is still accepted as a loader fallback for old/out-of-tree descriptors. Mesh parameters can point to non-current layers for operations such as isotropic remeshing against a separate reference surface. `MeshFilterDescriptor::effectivePythonName()` returns explicit `pythonName` when present and otherwise derives a snake-case name from the display name.

Execution: `runFilter(filterKey, parameters)` with callback-based progress and cancel (`requestOperationCancel`, `isOperationCancelRequested`). The framework normalizes/validates parameters, exposes `validateFilterInvocation(...)` for preflight checks, prepares requested volatile VCG data, validates typed `CameraState`/`RenderState` JSON payloads when descriptors request them, runs pre/post cleanup hooks, compacts modified meshes, updates geometry/material/selection/transform revisions according to descriptor output codes, refreshes the cached logical polygon count after `FV`/`FP` changes, and can return visualization hints for quality-based rendering.

Script history: modifying filter runs record a `ScriptAction` on the undo node, including filter key, invocation parameters, active layer indices, and both full and compact Python calls. The full call includes every recorded parameter; the compact call omits parameters that matched descriptor defaults at invocation time. The filter panel and action-history export share the same Python-call formatter.

Render-state filters: `filter_layer` includes `render_from_render_state_json`, which consumes `MeshLab.CameraState` and `MeshLab.RenderState` payloads, asks `Document::renderSnapshotFromStateJson(...)` for an offscreen render, then can save the result as PNG and/or add it as a raster layer with the resulting `CameraShot`.

Filters can consume document-level mesh, raster, camera, texture, and render-state data when their descriptors request those parameter/input domains. Mutations stay descriptor-driven: vertex color, wedge texture, material, geometry, selection, transform, and new-layer outputs are surfaced through the normal output-modifies codes and document revision paths rather than through filter-specific data-model branches.

Python integration: `_meshlab.MeshSet` wraps a `Document` and exposes mesh/raster/project helpers, visibility/current-layer methods, `list_filters`, `apply_filter`, and `render_snapshot`. `apply_filter` resolves a fully qualified key, descriptor id, or Python name, converts supported Python kwargs to `MeshFilterParameterValues` (`bool`, integer, float, string, and 3-number point/vector sequences), and runs the same `Document::runFilter(...)` path used by the GUI. In the embedded console, `PythonHost` exposes the public `pymeshlab2` facade, injects the live document as `ms`, injects the live view helper as `mlgui`, and dynamically adds one method per filter to `MeshSet` using each descriptor's `effectivePythonName()`.

## Undo/Redo Model

History is a tree, not a flat stack:

- `m_undoNodes` — flat arena of `UndoNode` objects; node id is the vector index, so compacting the arena — `makeUndoRoot`, `purgeUndoBranch`, `linearizeUndoHistory`, and the pruning that enforces the undo limit — renumbers the nodes it keeps
- node `0` — root state before the first recorded action
- `m_undoCurrentNode` — node id representing the current live state
- each `UndoNode` — incoming action label, parent id, child ids, preferred redo child, optional script-action record, and either a full `UndoState` snapshot or compact selection deltas
- `UndoNode::serial` — the node's identity: unique, never reused, taken when the node is made (`nextUndoNodeSerial()`), and kept through compaction, which moves nodes whole. `undoTreeInfo()` reports it beside the id; anything held per state across calls is keyed by it rather than by id

Committing an action appends a child to the current node, preserving alternate timelines instead of truncating siblings. `redo()` follows `preferredChild` when present, otherwise the first child. `jumpToUndoNode(nodeId, restoreCamera)` walks through the lowest common ancestor, suppressing intermediate GUI refresh signals and restoring camera only at the final target when requested. `undoTreeInfo()` exposes nodes, depths, current-node state, and current-path flags; the Action History lays them out itself (below).

The Action History panel that draws this tree is described in [User Interface](user_interface.md#the-action-history-view); it keeps its images by node `serial`, so they follow their states through a renumbering.

## Revision and Transform Model

- `setMeshTransform(index, transform, contextMessage)` updates `MeshEntry::transform`, emits `meshDataChanged`, records undo.
- `markMeshGeometryChanged(...)` advances `geometryRevision` and rebuilds GPU geometry resources lazily. Main geometry replacement/edit paths allocate revisions from a monotonic document counter so undo branches cannot collide on the same `(meshId, geometryRevision)` cache key.
- `markMeshMaterialChanged(...)` advances `materialRevision` and rebuilds GPU material resources lazily.
- `markMeshSelectionChanged(...)` advances `selectionRevision` and emits `meshSelectionChanged`. Selection flags still live inside `VCGMesh`, but the separate revision lets selection overlays rebuild without invalidating the much larger fill/wire/point buffers.
- `markRasterImageChanged(...)` advances `imageRevision`; per-view raster GPU image resources are rebuilt lazily.
- `setRasterShot(...)` advances `cameraRevision`; raster-projected rendering and raster camera glyphs pick up the new camera lazily.
- `setRasterLens(...)` advances both `cameraRevision` and `imageRevision`, and drops the planes decoded from photos so they decode again through the new lens.
- `setCurrentRasterPlaneIndex(...)` changes which plane is considered current for display/export without changing raster image data.
