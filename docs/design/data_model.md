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
- `polygonFaceCount`: the logical face count of a faux-edge polygon mesh, -1 for a triangle mesh
- `pluginData`: immutable, shared intermediates a plugin keeps on the layer (`setLayerData`; see [Adding a Filter](adding_a_filter.md))
- mesh state: `visible`, `VCGMesh mesh`

`Document` also owns: remembered current mesh/raster indices, explicit active layer kind, document log (application / VCG / error sources), I/O and filter plugin managers, shared GPU cache, `DocumentUndoManager`, operation cancel flag, and memory accounting APIs (`cpuMeshMemoryStats`, `cpuImageMemoryStats`, `undoMemoryStats`; GPU figures come from `gpuCache().gpuMemoryStats()`). Optional Python bindings borrow or own a `Document` through `MeshSetCore`; the embedded console borrows the live `MainWindow` document and does not take ownership.

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
| Name | `MeshEntry::name` | The label: what the panel, the grid-view captions and a project's `label` attribute show, and what the user edits. On load it is the file's name, extension included, or the project's `label`. Renaming changes only this. `addMesh()` makes it unique (" 2", " 3", ...); `duplicateMesh()` names the copy "<name> copy", and loading and renaming do not check. |
| File | `MeshEntry::sourcePath` | The full path of the layer's file, or empty: the file it was loaded from, or last saved to -- saving names the layer's file, as in any document application, and a label that was still the old file's name follows. Reload reads it; a project writes it (relative to the project folder) as the `MLMesh` `filename`. Undo does not take a save back: restoring a state puts back the file the document last read or wrote for that layer, not the one the state was captured with. |
| Stored | `Document::meshStoredState`, `meshTextureStoredState` | Whether a file holds what the document holds: `NotStored` (no file), `Changed` (a file did when last read or written, and the layer's geometry, material or selection revision has moved since), `Stored`. Recorded at load, reload and save, in `m_storedMeshes` (by `meshId`: file and the three revisions) and `m_storedImages` (by `QImage::cacheKey`: the file holding exactly that texture image, or the mesh file for one packed in a glb or 3mf). Neither is part of the undo state, so undoing past a save shows the layer changed and undoing back shows it stored again. That needs revisions that are never reused, so material and selection revisions draw from the document's geometry counter. A selection undone through the delta path gets a new revision, so a layer undone back to a saved selection shows changed: conservative, never the reverse. |

The Layers panel marks each layer, and each texture and raster plane under it, by its stored state (the row above), no mark when a file holds it, `●` when one did and it has changed since, `○` when no file holds it. A raster plane counts as held while its image is the one last read from or written to its file (`RasterPlane::sourceImageKey`, compared with `QImage::cacheKey`, which an edit changes). The tooltip says which file, or that it is inside its mesh file (a texture packed in a glb). Layers added through `Document::addMesh()` (the filters that create layers, the measure tool) and through `duplicateMesh()` have no file and are `○` until saved. A selection change rebuilds the panel only when it turns a layer's mark, so painting a selection stays cheap. These marks replaced the `[D]`, `[G]` and `*` tags, which said whether a layer had been loaded from a file, not whether the file held it.

A texture can be saved on its own (`Document::saveMeshTexture`, *Save Texture...* on a texture row, Python `save_current_mesh_texture`), and a raster plane likewise (`saveRasterPlaneImage`, *Save Plane Image...*), in any format the installed image plugins write, picked by the suffix. The texture or plane then lives in that file; when it is not the file the layer's mesh refers to, the layer is changed (its material revision moves) until the mesh is saved too.

The mark is part of the displayed text, not a field, so the panel cannot sort or filter on it. A separate provenance column (from disk / from disk and changed / generated) is the intended replacement.

## Signals and Reactivity

- mesh lifecycle: `meshAdded`, `meshRemoved`, `meshDataChanged`, `meshSelectionChanged`
- raster lifecycle/data: `rasterAdded`, `rasterRemoved`, `rasterDataChanged`
- selection/visibility: `currentMeshChanged`, `currentRasterChanged`, `currentLayerChanged`, `meshVisibilityChanged`, `rasterVisibilityChanged`
- progress: `loadProgress*`, `filterProgress*`
- logging: `logCleared`, `logMessageAdded`
- undo/redo: `undoRedoStateChanged`

## I/O Model

`Document::loadMesh()`: resolves import plugin, runs plugin load, merges duplicate vertices when the file is STL (a triangle soup with no shared vertices; on by default, `setMergeStlDuplicateVertices()`, driven by the `document.mergeStlDuplicateVertices` preference), compacts imported mesh storage, updates the bbox, computes the normals the file lacks (`computeMissingNormals`: imported vertex normals are kept as given, smooth ones generated only when absent, and face normals always computed unless the file brings them together with vertex normals, so every mesh enters the document with valid face normals), initializes transform and material set, resolves texture paths/assets, logs stats, records the stored state and the document folder, appends entry, emits signals. `reloadMesh(index)` follows the same path while preserving mesh identity and the layer's name; it adopts no unit. The VCG PLY path recognizes edge endpoint indices plus `red`/`green`/`blue` and optional `alpha` properties (including `diffuse_*` aliases), sets `IOM_EDGECOLOR`, and defaults missing edge alpha to 255.

`Document::loadRasterImage()`: loads a `QImage`, creates a `RasterEntry` with an RGBA `RasterPlane`, appends the raster layer, makes it current, and emits raster/current-layer signals. Ordinary image loads do not synthesize a calibrated camera; callers such as snapshot capture can pass a `CameraShot` through `addRasterImage(...)`, and raster-mode mesh projection is enabled only when that shot is valid.

`Document::loadMeshLabProject()`: parses MeshLab project (`.mlp`) files directly -- or a COLMAP sparse model, opened by its `cameras.txt` or `cameras.bin` (`document_colmap.cpp`, text or binary), whose images become rasters with their pinhole cameras and lenses and whose points a colored point cloud layer, with the reprojection of COLMAP's observations logged; `Document::isProjectFile` says which files it takes -- loads referenced meshes through the normal mesh plugin path -- passing each one's project label and transform to `loadMesh()`, so a layer is announced already named and placed rather than at the origin under its file name -- creates raster entries from project raster planes, stores project `CameraShot` data, records a `load_project` script action, and groups the operation under one undo step. Missing mesh files are logged and skipped; missing raster plane images are logged but still kept as plane metadata with a viewport-size fallback when possible.

`Document::saveMesh(...)`: resolves export plugin, passes `MeshIOSaveOptions` (mask, binary, embed textures, copy associated textures, Draco options). Textures follow one rule in every format that stores them (PLY, OBJ, glTF): one whose image a file holds -- by `m_storedImages`, not by the asset's own path, which undo may have brought back stale -- is copied or referred to there (PLY and OBJ by a path relative to the mesh unless "copy associated textures" is on; glTF always copies, and no longer re-encodes a JPEG); one no file holds is written next to the mesh from its image under its name, suffixed when that is taken, whatever the options. The saver reports where each texture now is (`MeshIOTextureContext::savedTexturePaths`); the layer then takes the saved file as its own and the textures their files, and the stored state is recorded. PLY export exposes edge indices and edge color in the save mask and writes per-edge RGBA in both ASCII and binary modes when requested.

`Document::saveMeshLabProject(...)`: writes a `.mlp` project from the current document. A layer it writes goes through `saveMesh`, so it takes the written file as its own and is recorded as stored; the save also repoints the raster planes it writes or copies to their files, and makes the project file and its folder the document's.

Each mesh layer is sorted by its stored state (see [Layer Provenance](#layer-provenance)). Every write goes through `saveMesh`, so it writes the layer's textures that no file holds beside it, takes the written file as the layer's own and records it as stored: the next project save writes only what changed since.

| Layer | What the save does | Project `filename` |
|---|---|---|
| No file (`○`) | Always written, as PLY, to `meshes/<name>.ply` in the project folder. The name keeps to `[A-Za-z0-9_.-]` and gets `_1`, `_2` ... when the name is taken on disk or by another layer in the same save, so two layers called "scan" become `scan.ply` and `scan_1.ply`. | `meshes/<name>.ply` |
| File in the project folder, changed (`●`), with "Re-save changed meshes" on (the default) | Written back over its own file. | its file |
| File outside the project folder | Refused unless "Copy files to project folder" is on; then saved into `meshes/` under a unique name in its own format (PLY when that cannot be written), with every texture copied beside it. A layer is saved rather than its file copied, because its texture references are relative to it. | the copy |
| File in the project folder otherwise | Not written; referenced where it is. | its file |

`saveMeshLabProjectKeepsLayersWithoutAFile` checks that a layer with no file is written even when unmodified, as a filter-created layer is.

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

Execution: `runFilter(filterKey, parameters)` with callback-based progress and cancel (`requestOperationCancel`, `isOperationCancelRequested`). The framework normalizes/validates parameters, exposes `validateFilterInvocation(...)` for preflight checks, prepares requested volatile VCG data, validates typed `CameraState`/`RenderState` JSON payloads when descriptors request them, runs pre/post cleanup hooks, compacts modified meshes, uses the descriptor's output codes to choose delta or full undo and to report selection changes (revisions move when the filter calls the `markMesh*Changed` functions, or when framework cleanup edits a mesh), refreshes the cached logical polygon count after `FV`/`FP` changes, and can return visualization hints for quality-based rendering.

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

Committing an action appends a child to the current node, preserving alternate timelines instead of truncating siblings. `redo()` follows `preferredChild` when present, otherwise the first child. `jumpToUndoNode(nodeId, restoreCamera)` restores one state, not every state on the way: the target's, or for a delta node its nearest full snapshot followed by the deltas below it. A walk made of delta steps alone is replayed instead, as each of those is cheaper than a restore. Either way the preferred children along the walk point back the way it came, as single undos and redos would leave them, and the camera is restored only when requested. `undoTreeInfo()` exposes nodes, depths, current-node state, and current-path flags; the Action History panel lays them out itself, as described in [User Interface](user_interface.md#the-action-history-view). The panel keeps its images (the row thumbnail and the hover snapshot) and its expanded rows by node `serial`, so they follow their states through a renumbering; `MainWindow` maps the panel's node ids to serials at each refresh.

Undo-tree maintenance APIs keep the graph controllable after branching: `makeUndoRoot(nodeId)` promotes a chosen node to the new root and discards unreachable history, `purgeUndoBranch(nodeId)` deletes a descendant branch, and `linearizeUndoHistory()` keeps only the root-to-current path. Byte-aware pruning discards side branches first, then counts the current path from the tip back and roots once at the oldest full snapshot whose states fit the target; delta nodes cannot become roots. When an operation drops the current state, the document first moves to the node it keeps -- the new root, or the node whose branch is purged -- so the live document is always the state the tree calls current. Count pruning likewise roots only at a full snapshot, keeping the few steps back to one. The operations notify the UI through the normal undo/redo state signal.

Full snapshot nodes store an `UndoState` containing `MeshSnapshot` and `RasterSnapshot` arrays plus current mesh/raster/layer ids, next id counters, and one `ViewState`. A `MeshSnapshot` copies all cheap metadata fields by value (`transform`, names/paths/assets/materials/visibility/mask/revisions) and holds geometry behind a `shared_ptr<const VCGMesh>` and the selection, as a selection-only `MeshAttributeDelta`, behind its own `shared_ptr`. `captureUndoState()` interns both, stored as `weak_ptr`: geometry in `m_undoGeometryCache` keyed by `(meshId, geometryRevision, materialRevision)`, selection in `m_undoSelectionCache` keyed by `(meshId, selectionRevision)`. States whose revisions are unchanged since the last capture share the allocations, and a selection change costs the next state its packed bits, not a copy of the mesh. A cache miss triggers a deep copy. The selection bits inside a shared geometry copy are whichever state made it, so they are never read. On undo/redo, `restoreUndoState()` keeps a live layer whose geometry and material revisions match the state's and deep-copies the others out of the shared pointer, so the live document is always freely mutable. It then writes the state's selection into every copied layer, and into a kept one whose `selectionRevision` differs. Rolling back a cancelled or failed step passes `keepUnchangedMeshes = false` and copies every layer, because such a step may have edited a mesh without moving its revisions. Main geometry replacement paths use document-level monotonic revision sources so branch-local edits do not accidentally reuse an older cache key after undo/redo navigation. A restore drops the cache entries no state holds any more; it keeps those of states ahead of it, so that a redo followed by a new step finds the copy instead of making another. `RasterSnapshot` stores raster metadata, `CameraShot`, lens, planes, current plane, visibility, and raster image/camera revision ids. `undoMemoryStats()` de-duplicates geometry, includes the capacity of the packed attribute deltas, counts only image backing stores retained by history rather than the live document, and reports resources unique to a pending operation. Current-path rows describe references and are not additive.

A change confined to one attribute class can use `UndoStorageKind::Delta` instead of a full snapshot. `DocumentUndoManager::beginDeltaStep(...)` captures a `MeshAttributeDelta` for the target mesh before and after the operation, holding only the classes named in its `kinds` mask: **selection** (packed bits for vertices, faces, polyline edges and the per-face edge flags), **color** (one packed RGBA word per element) and **scalar** (one float per element). Capture is selective on purpose: taking all three for a selection-only change would cost far more than the selection bits. Undo/redo applies the deltas directly to the live mesh, avoiding a full geometry copy while preserving the same tree behavior and script-action metadata.

**Custom attributes** (vcglib's named per-element attributes, such as `KH`, `outlierScore` or `BentNormal`) are in neither class, so a delta does not see them. A filter that leaves one on a layer declares `VA` or `FA`, which routes it to a full snapshot. `deepCopyMesh` carries named per-vertex, per-face and per-edge attributes of type `float`, `Point3f` and `int`; unnamed attributes, per-mesh ones and any other type are left behind, so a filter removes the scratch attributes it or vcglib created (pointers, booleans, a geodesic visit's `geod`) before it returns. The smoke sweep (`tests/test_filter_smoke.cpp`) undoes and redoes every filter that changes the set of attribute names, and fails if either step does not bring that set back.

Applying a delta bumps the revision the affected data is keyed on. Selection has its own `selectionRevision`; color and scalar live inside the `VCGMesh`, so restoring them bumps `geometryRevision`, or the GPU buffers that read them would keep showing the old values. Measured on a 200k-vertex, 100k-face mesh, a color or scalar step stores about 2.4 MB where a full snapshot adds a second 15 MB mesh, and a selection step about 0.075 MB.

The filter runner picks the path from the descriptor: a SingleMesh filter whose `outputModifies` codes all fall within `VS FS ES`, `VC FC EC` or `VQ FQ EQ` takes the delta path, with the union as its `kinds`. **The descriptor is trusted completely.** A filter that declares `VC` but also moves a vertex will not have the movement undone, because geometry is not captured; declaring `outputModifies` accurately is what keeps undo correct. Transforms, renames, visibility and material changes need no delta: they do not bump `geometryRevision`, so their snapshots already share one geometry allocation.

Each `UndoState` also stores a `ViewState` snapshot (`src/render/viewstate.h`) captured via `Document::setViewStateFunctions(...)`. Current wiring captures/restores the active `RenderWidget` camera/render-style state (`ViewTrackball::State`, `GlobalRenderSettings`, per-mesh `PerMeshRenderSettings`) with each node. Per-view visibility vectors, UV pan/zoom, and view mode are not part of `ViewState`; camera restore can be skipped when jumping to a node.

APIs: `beginUndoStep(label)`, `beginUndoStep(label, ScriptAction)`, `beginUndoStep(label, meshIndexForDelta, kinds)`, `beginUndoStep(label, ScriptAction, meshIndexForDelta, kinds)`, `endUndoStep(commit, restoreOnCancel)`, `undo()`, `redo()`, `jumpToUndoNode(nodeId, restoreCamera)`, `updateUndoNodeCamera(nodeId)`, `makeUndoRoot(nodeId)`, `purgeUndoBranch(nodeId)`, `linearizeUndoHistory()`, `undoTreeInfo()`, `undoNodeAction(nodeId)` (the one action that produced the state), `undoNodeScriptActions(nodeId)` (also the informational calls recorded around it), `clearUndoHistory()`, `setUndoLimit(limit)`, `setUndoMemoryLimitBytes(bytes)`, `pruneUndoToMemoryBudget(bytes)`, `handleUndoMemoryPressure(critical)`. Integrated with mesh mutations (`add/remove/duplicate/reload/rename`, `setMeshTransform`, `markMeshGeometryChanged`, `markMeshMaterialChanged`, `markMeshSelectionChanged`) and raster mutations (`add/remove/rename`, `setRasterShot`, `setRasterLens`, `setCurrentRasterPlaneIndex`, `markRasterImageChanged`).

## Revision and Transform Model

- `setMeshTransform(index, transform, contextMessage)` updates `MeshEntry::transform`, emits `meshDataChanged`, records undo.
- `markMeshGeometryChanged(...)` advances `geometryRevision` and rebuilds GPU geometry resources lazily. Main geometry replacement/edit paths allocate revisions from a monotonic document counter so undo branches cannot collide on the same `(meshId, geometryRevision)` cache key; raster image and camera revisions draw from the same counter, since a restore keeps a live raster whose revisions match.
- `markMeshMaterialChanged(...)` advances `materialRevision` and rebuilds GPU material resources lazily.
- `markMeshSelectionChanged(...)` advances `selectionRevision` and emits `meshSelectionChanged`. Selection flags still live inside `VCGMesh`, but the separate revision lets selection overlays rebuild without invalidating the much larger fill/wire/point buffers.
- `markRasterImageChanged(...)` advances `imageRevision`; per-view raster GPU image resources are rebuilt lazily.
- `setRasterShot(...)` advances `cameraRevision`; raster-projected rendering and raster camera glyphs pick up the new camera lazily.
- `setRasterLens(...)` advances both `cameraRevision` and `imageRevision`, and drops the planes decoded from photos so they decode again through the new lens.
- `setCurrentRasterPlaneIndex(...)` changes which plane is considered current for display/export without changing raster image data.
