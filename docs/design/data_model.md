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
- mesh state: `visible`, `modified`, `VCGMesh mesh`

`Document` also owns: remembered current mesh/raster indices, explicit active layer kind, document log (application / VCG / error sources), I/O and filter plugin managers, shared GPU cache, `DocumentUndoManager`, operation cancel flag, and memory accounting APIs (`cpuMeshMemoryStats`, `cpuImageMemoryStats`, `undoMemoryStats`, `gpuMemoryStats`). Optional Python bindings borrow or own a `Document` through `MeshSetCore`; the embedded console borrows the live `MainWindow` document and does not take ownership.

## Mesh Data Type

`VCGMesh` is a `vcg::tri::TriMesh` specialization. `VCGVertex` has fixed coordinate, normal, color, quality, and flag components plus optional OCF texture-coordinate and curvature-direction data. `VCGFace` has fixed vertex references, normal, color, quality, and flags plus optional OCF wedge texture coordinates and algorithm-only adjacency/mark data. `VCGEdge` has fixed endpoint references, RGBA color, scalar, flags, and the edge half of vertex-edge adjacency; the vertex half is an optional OCF component, enabled only while a filter declares the `VE` preparation code. Because edge color storage is always allocated, `MeshEntry::ioMask & IOM_EDGECOLOR` is the semantic indication that meaningful per-edge colors are available.

A face's texture is the index of its first wedge, `WT(0).N()`, and a negative one -- vcglib's -1 -- means the face has **no texture**: its UVs mean nothing. Every face added to a mesh with OCF wedge texture coordinates starts that way, so the cap Trim Surface by Plane closes a cut with is untextured. Readers leave such a face out rather than reading texture 0 at its UVs: it is drawn untextured and is absent from the UV view and UV-space selection (`vcgFaceTextureGroup`, `vcgFaceCornerUV`); the UV measures count it as surface the mapping does not cover (`UvLayerUtils::buildUvLayer`); the atlas and packing filters carry it over unchanged. So anything that **writes** UVs must also set the index -- 0 for a single texture, as every parametrization filter does -- or the faces it maps read as untextured. Per-vertex texture coordinates carry no per-face texture, and their index is not reliably set, so it is never read as "no texture".

## Raster Data Type

`Document::RasterEntry` is the canonical raster-layer record. Each entry stores:

- identity/revision keys: `rasterId`, `imageRevision`, `cameraRevision`
- source metadata: `name`, `sourcePath`
- layer state: `visible`, `currentPlaneIndex`
- camera state: `CameraShot shot`
- image data: `std::vector<RasterPlane> planes`

`RasterPlane` records a semantic role (`RGBA`, `MaskUInt8`, `MaskFloat`, `DepthFloat`, or extra float/RGBA planes), display/source metadata, pixel size, and optional `QImage` payload. The current implementation loads ordinary raster images as RGBA planes and keeps the plane abstraction ready for mask/depth/extra image channels.

The planes of a raster share its camera, and everything that reads a raster's image -- the raster view, the color projection and texturing filters -- reads its current plane at the camera's `ViewportPx`, pixel for pixel. *Import Image to Current Raster* (`filter_layer`) replaces the current plane with an image file or adds one as a new current plane, keeping the camera. An image of another resolution with the same aspect ratio can replace a raster's only plane, and the camera then takes that resolution (`vcg::Camera::RescaleViewportPx`: `ViewportPx`, `CenterPx` and the distortion center scale with the pixels, `PixelSizeMm` inversely, `FocalMm` stays); any other plane must have the camera's resolution.

`CameraShot` (`src/core/camerashot.*`) wraps a VCG shot for raster and snapshot workflows. It stores viewport, pixel size, focal length, distortion, camera type, and extrinsics, and exposes projection/unprojection, depth, view-matrix, and projection-matrix helpers.

## Layer Provenance

A mesh layer carries three separate pieces of information about where it came from, and the Layers panel shows a fourth thing that is not stored at all.

| | Field | What it is |
|---|---|---|
| Name | `MeshEntry::name` | The label: what the panel, the grid-view captions and a project's `label` attribute show, and what the user edits. On load it is the file's base name, or the project's `label`. Renaming changes only this. `addMesh()` and `duplicateMesh()` make it unique (" 2", " 3", ...); loading and renaming do not check. |
| File | `MeshEntry::sourcePath` | The full path of the layer's file, or empty: the file it was loaded from, or last saved to -- saving names the layer's file, as in any document application, and a label that was still the old file's name follows. Reload reads it; a project writes it (relative to the project folder) as the `MLMesh` `filename`. Undo does not take a save back: restoring a state puts back the file the document last read or wrote for that layer, not the one the state was captured with. |
| Changed | `MeshEntry::modified` | Set by `markMeshGeometryChanged`, `markMeshMaterialChanged` and `markMeshSelectionChanged` -- so a selection change counts -- and cleared only by `reloadMesh`. A transform change does not set it, and neither does saving clear it. Being replaced by the stored state below ([project_persistence.md](proposals/project_persistence.md), stage 2). |
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

`Document::loadMesh()`: resolves import plugin, runs plugin load, merges duplicate vertices when the file is STL (a triangle soup with no shared vertices; on by default, `setMergeStlDuplicateVertices()`, driven by the `document.mergeStlDuplicateVertices` preference), compacts imported mesh storage, updates bbox/normals, initializes transform and material set, resolves texture paths/assets, logs stats, appends entry, clears the `modified` flag, emits signals. `reloadMesh(index)` follows the same path while preserving mesh identity. The VCG PLY path recognizes edge endpoint indices plus `red`/`green`/`blue` and optional `alpha` properties (including `diffuse_*` aliases), sets `IOM_EDGECOLOR`, and defaults missing edge alpha to 255.

`Document::loadRasterImage()`: loads a `QImage`, creates a `RasterEntry` with an RGBA `RasterPlane`, appends the raster layer, makes it current, and emits raster/current-layer signals. Ordinary image loads do not synthesize a calibrated camera; callers such as snapshot capture can pass a `CameraShot` through `addRasterImage(...)`, and raster-mode mesh projection is enabled only when that shot is valid.

`Document::loadMeshLabProject()`: parses MeshLab project (`.mlp`) files directly, loads referenced meshes through the normal mesh plugin path -- passing each one's project label and transform to `loadMesh()`, so a layer is announced already named and placed rather than at the origin under its file name -- creates raster entries from project raster planes, stores project `CameraShot` data, records a `load_project` script action, and groups the operation under one undo step. Missing mesh files are logged and skipped; missing raster plane images are logged but still kept as plane metadata with a viewport-size fallback when possible.

`Document::saveMesh(...)`: resolves export plugin, passes `MeshIOSaveOptions` (mask, binary, embed textures, copy associated textures, Draco options). Textures follow one rule in every format that stores them (PLY, OBJ, glTF): one whose image a file holds -- by `m_storedImages`, not by the asset's own path, which undo may have brought back stale -- is copied or referred to there (PLY and OBJ by a path relative to the mesh unless "copy associated textures" is on; glTF always copies, and no longer re-encodes a JPEG); one no file holds is written next to the mesh from its image under its name, suffixed when that is taken, whatever the options. The saver reports where each texture now is (`MeshIOTextureContext::savedTexturePaths`); the layer then takes the saved file as its own and the textures their files, and the stored state is recorded. PLY export exposes edge indices and edge color in the save mask and writes per-edge RGBA in both ASCII and binary modes when requested.

`Document::saveMeshLabProject(...)`: writes a `.mlp` project from the current document. It changes the document only through `saveMesh`: a layer it writes takes the written file as its own. No `modified` flag is cleared.

Each mesh layer is sorted into one of three cases by its provenance (see [Layer Provenance](#layer-provenance)):

| Layer | What the save does | Project `filename` |
|---|---|---|
| No file (`○`) | Always written, as PLY, to `meshes/<name>.ply` in the project folder. The name is simplified and every character outside `[A-Za-z0-9_.-]` becomes `_`; two names that simplify to the same text overwrite each other. | `meshes/<name>.ply` |
| From disk, changed (`●`) with "Re-save modified meshes" on (the default) | **Not written yet.** The option marks the layer for re-saving over its own file, but nothing performs the write, so the project refers to the unchanged file and reopens without the change. | the original file |
| From disk otherwise | Not written; referenced where it is. | the original file |

The "no file" case used to be skipped when the layer was unmodified, which dropped every filter-created layer from the saved project without a message; the regression test is `saveMeshLabProjectKeepsLayersWithoutAFile`.

Options (`MeshLabProjectSaveOptions`, offered by the Save Project dialog): `onlyVisibleMeshes` leaves hidden layers out of the project altogether; `saveModifiedMeshes` is the re-save above; `copyFiles` lets the save proceed when some referenced meshes or rasters live outside the project folder. Without it, such a project is refused with an error rather than saved with paths that would break when the folder moves. With it the files are copied into `meshes/` and `images/`, but the project still names the original locations, so the copies are not what a reopened project loads. Paths in the file are relative to the project folder.

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

Metadata: `filterInfos()`, `loadedFilterPluginSummaries()`. Descriptors can be loaded declaratively from filter JSON resources via `FilterDescriptorLoader`, including validated ordered `categories` (first entry primary, later entries cross-listings), structured `provenance`, CSL-style publication `references`, tags, requirements, input/parameter preparation codes (`FF`, `VF`, `VTex`/`VT`, `WTex`/`WT`, `BorderFF`, `BorderVF`, `CurvDir`, normals, bbox, marks), cleanup hooks, dynamic bounds/default tokens, texture input/output references, mesh parameters with their own requirements/preparation, point/vector parameters, camera/render-state parameters, incremental selection, `pythonName`, and output-modifies codes. `categories` is the authored field; a legacy single `menuPath` string is still accepted as a loader fallback for old/out-of-tree descriptors. Mesh parameters can point to non-current layers for operations such as isotropic remeshing against a separate reference surface. `MeshFilterDescriptor::effectivePythonName()` returns explicit `pythonName` when present and otherwise derives a snake-case name from the display name.

Execution: `runFilter(filterKey, parameters)` with callback-based progress and cancel (`requestOperationCancel`, `isOperationCancelRequested`). The framework normalizes/validates parameters, exposes `validateFilterInvocation(...)` for preflight checks, prepares requested volatile VCG data, validates typed `CameraState`/`RenderState` JSON payloads when descriptors request them, runs pre/post cleanup hooks, compacts modified meshes, updates geometry/material/selection/transform revisions according to descriptor output codes, refreshes the cached logical polygon count after `FV`/`FP` changes, and can return visualization hints for quality-based rendering.

Script history: modifying filter runs record a `ScriptAction` on the undo node, including filter key, invocation parameters, active layer indices, and both full and compact Python calls. The full call includes every recorded parameter; the compact call omits parameters that matched descriptor defaults at invocation time. The filter panel and action-history export share the same Python-call formatter.

Render-state filters: `filter_layer` includes `render_from_render_state_json`, which consumes `MeshLab.CameraState` and `MeshLab.RenderState` payloads, asks `Document::renderSnapshotFromStateJson(...)` for an offscreen render, then can save the result as PNG and/or add it as a raster layer with the resulting `CameraShot`.

Filters can consume document-level mesh, raster, camera, texture, and render-state data when their descriptors request those parameter/input domains. Mutations stay descriptor-driven: vertex color, wedge texture, material, geometry, selection, transform, and new-layer outputs are surfaced through the normal output-modifies codes and document revision paths rather than through filter-specific data-model branches.

Python integration: `_meshlab.MeshSet` wraps a `Document` and exposes mesh/raster/project helpers, visibility/current-layer methods, `list_filters`, `apply_filter`, and `render_snapshot`. `apply_filter` resolves a fully qualified key, descriptor id, or Python name, converts supported Python kwargs to `MeshFilterParameterValues` (`bool`, integer, float, string, and 3-number point/vector sequences), and runs the same `Document::runFilter(...)` path used by the GUI. In the embedded console, `PythonHost` exposes the public `pymeshlab` facade, injects the live document as `ms`, injects the live view helper as `mlgui`, and dynamically adds one method per filter to `MeshSet` using each descriptor's `effectivePythonName()`.

## Preference Model

Application preferences live in `resources/preferences.json`, using the same parameter-descriptor schema as filter parameters. `Preferences` is a process-wide `src/core` singleton backed by `QSettings`; it exposes declared descriptors, typed reads, immediate writes, reset-to-defaults, and a `changed` signal. `PreferencesDialog` renders those descriptors through the shared `ParameterFormBuilder`, so adding one of the current 14 preferences is a JSON/schema change plus a call-site read, not a custom-widget change.

## Undo/Redo Model

History is a tree, not a flat stack:

- `m_undoNodes` — flat arena of `UndoNode` objects; node id is the vector index, so compacting the arena — `makeUndoRoot`, `purgeUndoBranch`, `linearizeUndoHistory`, and the pruning that enforces the undo limit — renumbers the nodes it keeps
- node `0` — root state before the first recorded action
- `m_undoCurrentNode` — node id representing the current live state
- each `UndoNode` — incoming action label, parent id, child ids, preferred redo child, optional script-action record, and either a full `UndoState` snapshot or compact selection deltas
- `UndoNode::serial` — the node's identity: unique, never reused, taken when the node is made (`nextUndoNodeSerial()`), and kept through compaction, which moves nodes whole. `undoTreeInfo()` reports it beside the id; anything held per state across calls is keyed by it rather than by id

Committing an action appends a child to the current node, preserving alternate timelines instead of truncating siblings. `redo()` follows `preferredChild` when present, otherwise the first child. `jumpToUndoNode(nodeId, restoreCamera)` walks through the lowest common ancestor, suppressing intermediate GUI refresh signals and restoring camera only at the final target when requested. `undoTreeInfo()` exposes nodes, depths, current-node state, and current-path flags; the Action History lays them out itself (below).

### The Action History view

`UndoGraphWidget` draws the tree as rows, newest at top, laid out by `UndoGraphLayout` (`src/ui/undographlayout.h`), which is pure and tested in `tests/test_undographlayout.cpp`. Two kinds of repeat fold into one row, drawn with a strip of dots under the label, one per state:

- **Tries** — siblings made by the same filter: the sweep left by undoing a filter and running it again. Only tries that went nowhere fold. A try that was continued keeps a row for its children to hang from, and when exactly one was, the others fold into its row.
- **Runs** — a chain of the same action, as an interactive tool leaves it, each step carried on by its oldest child made by that action. A branch does not split the run: the row that leaves it hangs from the step it left, whose dot carries a notch, and takes a column of its own even when it is the run's only branch, so it cannot pass for what came after the last step. Hovering the branch enlarges that dot; hovering the dot lights up the branches and names them in the caption. A run's dots are joined, solid along the path to the current state and dashed, muted, where it does not come from them; tries stand apart.

When siblings of one action hang from a step made by that same action and the oldest of them was continued, that one carries on the run and only the others can fold as tries: undoing two selections and selecting again is a branch off the run, not a sweep. A sweep of leaves on top of the same filter still folds as tries.

"The same action" is the same filter key whatever its parameters, or the same label for anything that is not a filter. When exactly one parameter differs between the states, each dot carries its value and the strip starts with that parameter's name. Hovering a thumbnail or a dot shows the state's snapshot, captioned with the try or step and, up to three, the parameters that differ. Where it shows is the `view.historyPreview` preference: by default over the 3D view itself (`RenderWidget::showStatePreview`), filling it inside a cloudy frame and under a *Preview* label so it cannot pass for the live document, which stays untouched underneath; or in a popup beside the panel. Camera and render states are left out of the comparison: they say where a screen-space tool acted from, not what was chosen. Clicking the count, or the `+N` standing for older states that do not fit, expands a group into a row per state.

*Update camera* stores the current view, camera and look alike, into a state and retakes its row thumbnail and hover snapshot. On a folded row, right-clicked anywhere but on a dot, it does so for every state the row holds. Only the live document can be photographed, so `MainWindow::updateUndoNodeCameras` visits each other state with `jumpToUndoNode(id, false)`, puts the live view back over the look the state was recorded with, grabs the frame, and returns to where it started. A visit's own refreshes of the filter UI and of the panel are skipped, since the document ends where it started. Selection steps are deltas and cost almost nothing to visit; filter steps restore a geometry snapshot each.

The panel's images (the row thumbnail and the hover snapshot) and its expanded rows are kept by node serial, so they follow their states through a renumbering; MainWindow maps the panel's node ids to serials at each refresh.

Columns are assigned on the folded rows by the rule the history has always used: a row's oldest child continues its column, and each later one opens a new column, in creation order. The undo model stores no display lane.

Undo-tree maintenance APIs keep the graph controllable after branching: `makeUndoRoot(nodeId)` promotes a chosen node to the new root and discards unreachable history, `purgeUndoBranch(nodeId)` deletes a descendant branch, and `linearizeUndoHistory()` keeps only the root-to-current path. Byte-aware pruning discards side branches first and then advances through old full checkpoints; delta nodes cannot become roots. These operations preserve the current live state and notify the UI through the normal undo/redo state signal.

Full snapshot nodes store an `UndoState` containing `MeshSnapshot` and `RasterSnapshot` arrays plus current mesh/raster/layer ids, next id counters, and one `ViewState`. A `MeshSnapshot` copies all cheap metadata fields by value (`transform`, names/paths/assets/materials/visibility/modified/mask/revisions) and holds geometry behind a `shared_ptr<const VCGMesh>`. `captureUndoState()` interns geometry objects in `m_undoGeometryCache` (keyed by `(meshId, geometryRevision, selectionRevision)`, stored as `weak_ptr`): if the full mesh content identity is unchanged since the last capture, nodes share the same allocation. A cache miss triggers a deep copy. On undo/redo, `restoreUndoState()` deep-copies geometry out of the shared pointer so the live document is always freely mutable. Main geometry replacement paths use document-level monotonic revision sources so branch-local edits do not accidentally reuse an older cache key after undo/redo navigation. Branch restore also evicts newer cached revisions for restored mesh ids to avoid stale geometry reuse across branches. `RasterSnapshot` stores raster metadata, `CameraShot`, planes, current plane, visibility, and raster image/camera revision ids. `undoMemoryStats()` de-duplicates geometry, includes the capacity of the packed attribute deltas, counts only image backing stores retained by history rather than the live document, and reports resources unique to a pending operation. Current-path rows describe references and are not additive.

A change confined to one attribute class can use `UndoStorageKind::Delta` instead of a full snapshot. `DocumentUndoManager::beginDeltaStep(...)` captures a `MeshAttributeDelta` for the target mesh before and after the operation, holding only the classes named in its `kinds` mask: **selection** (packed bits for vertices, faces and polyline edges), **color** (one packed RGBA word per element) and **scalar** (one float per element). Capture is selective on purpose: taking all three for a selection-only change would cost far more than the selection bits. Undo/redo applies the deltas directly to the live mesh, avoiding a full geometry copy while preserving the same tree behavior and script-action metadata.

Applying a delta bumps the revision the affected data is keyed on. Selection has its own `selectionRevision`; color and scalar live inside the `VCGMesh`, so restoring them bumps `geometryRevision`, or the GPU buffers that read them would keep showing the old values. Measured on a 200k-vertex, 100k-face mesh, a color or scalar step stores about 2.4 MB where a full snapshot adds a second 15 MB mesh, and a selection step about 0.075 MB.

The filter runner picks the path from the descriptor: a SingleMesh filter whose `outputModifies` codes all fall within `VS FS ES`, `VC FC EC` or `VQ FQ EQ` takes the delta path, with the union as its `kinds`. **The descriptor is trusted completely.** A filter that declares `VC` but also moves a vertex will not have the movement undone, because geometry is not captured; declaring `outputModifies` accurately is what keeps undo correct. Transforms, renames, visibility and material changes need no delta: they do not bump `geometryRevision`, so their snapshots already share one geometry allocation.

Each `UndoState` also stores a `ViewState` snapshot (`src/render/viewstate.h`) captured via `Document::setViewStateFunctions(...)`. Current wiring captures/restores the active `RenderWidget` camera/render-style state (`ViewTrackball::State`, `GlobalRenderSettings`, per-mesh `PerMeshRenderSettings`) with each node. Per-view visibility vectors, UV pan/zoom, and view mode are not part of `ViewState`; camera restore can be skipped when jumping to a node.

APIs: `beginUndoStep(label)`, `beginUndoStep(label, ScriptAction)`, `beginUndoStep(label, meshIndexForDelta, kinds)`, `beginUndoStep(label, ScriptAction, meshIndexForDelta, kinds)`, `endUndoStep(commit, restoreOnCancel)`, `undo()`, `redo()`, `jumpToUndoNode(nodeId, restoreCamera)`, `updateUndoNodeCamera(nodeId)`, `makeUndoRoot(nodeId)`, `purgeUndoBranch(nodeId)`, `linearizeUndoHistory()`, `undoTreeInfo()`, `undoNodeScriptAction(nodeId)`, `clearUndoHistory()`, `setUndoLimit(limit)`, `setUndoMemoryLimitBytes(bytes)`, `pruneUndoToMemoryBudget(bytes)`, `handleUndoMemoryPressure(critical)`. Integrated with mesh mutations (`add/remove/duplicate/reload/rename/visibility`, `setMeshTransform`, `markMeshGeometryChanged`, `markMeshMaterialChanged`, `markMeshSelectionChanged`) and raster mutations (`add/remove/rename/visibility`, `setRasterShot`, `setCurrentRasterPlaneIndex`, `markRasterImageChanged`).

## Revision and Transform Model

- `setMeshTransform(index, transform, contextMessage)` updates `MeshEntry::transform`, emits `meshDataChanged`, records undo.
- `markMeshGeometryChanged(...)` advances `geometryRevision`, sets `modified = true`, and rebuilds GPU geometry resources lazily. Main geometry replacement/edit paths allocate revisions from a monotonic document counter so undo branches cannot collide on the same `(meshId, geometryRevision)` cache key.
- `markMeshMaterialChanged(...)` increments `materialRevision`, sets `modified = true`, and rebuilds GPU material resources lazily.
- `markMeshSelectionChanged(...)` increments `selectionRevision`, emits `meshSelectionChanged`, and sets `modified = true`. Selection flags still live inside `VCGMesh`, but the separate revision lets selection overlays rebuild without invalidating the much larger fill/wire/point buffers.
- `markRasterImageChanged(...)` advances `imageRevision`; per-view raster GPU image resources are rebuilt lazily.
- `setRasterShot(...)` advances `cameraRevision`; raster-projected rendering and raster camera glyphs pick up the new camera lazily.
- `setCurrentRasterPlaneIndex(...)` changes which plane is considered current for display/export without changing raster image data.

## Render-State Snapshot Model

`RenderWidget` exposes two JSON state families:

- `MeshLab.CameraState`: camera/trackball payload used by camera copy/paste and filter parameters.
- `MeshLab.RenderState`: sparse per-view rendering payload containing view mode, raster opacity, trackball, `GlobalRenderSettings`, per-view visibility, per-mesh render modes keyed by `mesh_id`, current mesh/raster indices, current layer kind, and viewport metadata. Default-valued fields are omitted by export; on import, omitted settings/mode maps parse from default settings, while omitted view mode and raster opacity keep the current view values.

`MainWindow` wires `Document::setRenderStateSnapshotFunction(...)` to the active `RenderWidget`. `Document::renderSnapshotFromStateJson(renderStateJson, pixelSize, outImage, outShot, error)` applies the requested render state to that view, renders an offscreen image, returns the resulting `QImage` and `CameraShot`, then restores the previous view state. This keeps filter code document-centric while leaving all GPU and per-view state inside `RenderWidget`.

## Render Settings Model

Defined in `renderingsettings.h`:

### `PerMeshRenderSettings`

One instance per mesh id in `RenderWidget::m_meshRenderModes`. Holds:

- pass toggles: `showFill`, `showWire`, `showEdges`, `showPoints`, `showBoundingBox`, `showSelection`, `showSelectionFaces`, `showSelectionVertices`
- decorator toggles: `decoratorVertexNormals`, `decoratorFaceNormals`, `decoratorBoundaryEdges`, `decoratorTextureSeams`, `decoratorNonManifoldEdges`, `decoratorNonManifoldVertices`, `decoratorCurvatureDir`
- lighting/culling flags: `fillLighting`, `fillBackfaceCulling`, `wireLighting`, `wireBackfaceCulling`, `wireRespectFaux`, `pointLighting`
- fill material: `fillMaterial` (`Plain` / `Pbr`) + sub-structs `fillPlain` (`PlainFillParams`, including the radiance-scaling option: `radianceScaling`, `rsEnhancement`, `rsResolutionBias`) and `fillPbr` (`PbrFillParams`, including PBR texture sources, normal-map space, normal scale, AO strength, roughness factor)
- colors and sizes: `fillColor`, `wireColor`/`wireSize`, `edgeColor`/`edgeSize`, `pointColor`/`pointSize`, `bboxWireColor`, decorator colors, `decoratorBoundaryWidth`
- `pointColorSource`: `Constant` / `PerVertex` / `PerVertexQuality`
- `edgeColorSource`: `Constant` / `PerVertex` / `PerEdge`; this applies to explicit mesh edges, not triangle wireframe or topology decorators

### `GlobalRenderSettings`

One instance per view in `m_renderSettings`. Holds:

- scene overlay: `highlightCurrentMesh`, `currentMeshOutlineColor`/`Width`, `currentMeshDilateRadius`/`ErodeRadius`, `currentMeshDebugView`, `showTrackballGizmo`, `showViewCameras`, `showBoundingBoxCorners`, `showBoundingBoxDimensions`, `showDecoratorInfo`
- scene background: `sceneBackgroundTopColor`, `sceneBackgroundBottomColor`
- UV viewer: `uvShowReferenceFrame`, `uvShowFullTexture`, `uvTextureChannel`, `uvTextureNearestSampling`; the active texture group is view-local per-mesh state selected from viewport thumbnails
- quality histogram/range: `showQualityHistogram`, `qualityHistogramBins`, `qualityHistogramSource`, `qualityHistogramFixedRange`, `qualityHistogramMin`/`Max`, `qualityHistogramCenterOnZero`, `qualityHistogramPercentileCrop` (default `0.01`, cropping both tails for automatic ranges), `qualityHistogramColorMapId`, `qualityHistogramInvertColorMap`, `qualityIsolinesEnabled`, `qualityIsolineCount`
- overlay panel state: `settingsPanelVisible`, `currentPass`

`using RenderSettings = GlobalRenderSettings` and `using MeshRenderMode = PerMeshRenderSettings` (widget-local alias) are provided.

## Render Request / Plan Model

Scene3D rendering now has an explicit lightweight request layer before GPU draw planning:

- `RenderMeshPassRequests` stores one visible mesh index, its resolved `PerMeshRenderSettings`, and booleans for fill, wire, edges, bbox, points, selection, decorator-normal resources, and decorator-boundary/seam/non-manifold resources.
- `RenderFramePassRequests` is the frame-wide aggregate of those per-mesh requests plus raster backplate/projected/frustum requests. It answers whether a pass family is needed and is also used to decide which shared mesh GPU resources to prepare.
- `RenderFrameRequest` combines pass requests with frame-local view data (`ViewMode`, pixel size, projection matrix, view matrix, light direction).
- `RenderFramePlan` is the concrete GPU draw plan generated from a `RenderFrameRequest`; it contains mesh and raster draw items with QRhi pipelines/buffers/material renderer pointers and is therefore not suitable as a persisted or JSON object.

This split keeps user/render settings as stable input data, pass requests as an implementation-neutral frame description, and GPU objects inside the concrete plan. Programmatic rendering now targets versioned camera/render-state JSON, then lets the application build the GPU `RenderFramePlan` internally.

## Shared vs Per-View State

| Shared (Document) | Per-view (RenderWidget) |
|---|---|
| mesh geometry, material, and selection data | per-mesh render modes/styles |
| mesh transforms | per-view visibility vector |
| mesh/raster list, current indices, active layer kind | view mode, camera/trackball, headlight direction, UV pan/zoom, raster pan/zoom/opacity |
| document visibility proxy (`MeshEntry::visible`) | overlay settings, histogram cache |
| logs, progress, plugin registries | pipelines, SRBs, UBOs, render targets |
| undo tree, full snapshots, selection deltas, script-action records | UV-local GPU cache (`m_uvMeshGpu`), per-mesh active UV texture group, raster GPU image cache, active/suspended tool state, transient measure/transform feedback |
| shared mesh GPU cache | |

`MainWindow` synchronizes document visibility from the active view so the layer panel reflects it; each view keeps its own local visibility vector independently.

Undo stores serialized `ViewState` snapshots in undo-node history, but live render-widget ownership remains per-view.

## GPU Cache Integration

Renderer-facing APIs on `Document`: `ensureMeshGpuResources(...)`, `fillPassGpuView(...)`, `wirePassGpuView(...)`, `edgePassGpuView(...)`, `edgeFatPassGpuView(...)`, `pointsPassGpuView(...)`, `bboxPassGpuView(...)`, `selectionPassGpuView(...)`, `decoratorPassGpuView(...)`.

Scene3D resource preparation is driven by `RenderFramePassRequests`, not by a second ad-hoc scan of per-mesh render modes. For each visible mesh, the request says which cache products are needed; current-mesh highlighting can additionally request fill/edge/point resources for the highlighted mesh.

Cache keyed by `(QRhi*, meshId, variant, revision, quality-range mode/min/max, center-on-zero flag, percentile crop, wire faux-edge mode where relevant)`. Fill/wire/point variants use geometry/material revisions as appropriate, while selection resources include `selectionRevision` so selection overlays can refresh independently. `MeshSource` carries legacy texture paths, `textureAssets`, material metadata, and quality-range options. Invalidatable per-RHI or globally. Explicit-edge buffers carry both endpoint vertex RGBA and edge RGBA so changing `edgeColorSource` is a settings/UBO change rather than a cache rebuild. Selection and decorator buffers are first-class cache outputs, including normals, boundary/seam lines, non-manifold edge/vertex markers, and curvature direction lines when the mesh provides the required data.

## Memory Diagnostics

`MainWindow::showMemoryInfo()` reads `cpuMeshMemoryStats`, `undoMemoryStats` (de-duplicates shared geometry), and `gpuMemoryStats` from `Document`.

These values are implementation-level mesh/cache estimates (capacity-based for VCG containers, VCG OCF side-data vectors, undo snapshots, and cache-owned GPU allocations), not full process RSS/Activity-Monitor memory.

For rendering pipeline details, see [Rendering](rendering.md).
