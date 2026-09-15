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

`VCGMesh` is a `vcg::tri::TriMesh` specialization carrying the VCG attributes enabled by the imported data.

## Raster Data Type

`Document::RasterEntry` is the canonical raster-layer record. Each entry stores:

- identity/revision keys: `rasterId`, `imageRevision`, `cameraRevision`
- source metadata: `name`, `sourcePath`
- layer state: `visible`, `currentPlaneIndex`
- camera state: `CameraShot shot`
- image data: `std::vector<RasterPlane> planes`

`RasterPlane` records a semantic role (`RGBA`, `MaskUInt8`, `MaskFloat`, `DepthFloat`, or extra float/RGBA planes), display/source metadata, pixel size, and optional `QImage` payload. The current implementation loads ordinary raster images as RGBA planes and keeps the plane abstraction ready for mask/depth/extra image channels.

`CameraShot` (`src/core/camerashot.*`) wraps a VCG shot for raster and snapshot workflows. It stores viewport, pixel size, focal length, distortion, camera type, and extrinsics, and exposes projection/unprojection, depth, view-matrix, and projection-matrix helpers.

## Signals and Reactivity

- mesh lifecycle: `meshAdded`, `meshRemoved`, `meshDataChanged`, `meshSelectionChanged`
- raster lifecycle/data: `rasterAdded`, `rasterRemoved`, `rasterDataChanged`
- selection/visibility: `currentMeshChanged`, `currentRasterChanged`, `currentLayerChanged`, `meshVisibilityChanged`, `rasterVisibilityChanged`
- progress: `loadProgress*`, `filterProgress*`
- logging: `logCleared`, `logMessageAdded`
- undo/redo: `undoRedoStateChanged`

## I/O Model

`Document::loadMesh()`: resolves import plugin, runs plugin load, compacts imported mesh storage, updates bbox/normals, initializes transform and material set, resolves texture paths/assets, logs stats, appends entry, clears the `modified` flag, emits signals. `reloadMesh(index)` follows the same path while preserving mesh identity.

`Document::loadRasterImage()`: loads a `QImage`, creates a `RasterEntry` with an RGBA `RasterPlane`, appends the raster layer, makes it current, and emits raster/current-layer signals. Ordinary image loads do not synthesize a calibrated camera; callers such as snapshot capture can pass a `CameraShot` through `addRasterImage(...)`, and raster-mode mesh projection is enabled only when that shot is valid.

`Document::loadMeshLabProject()`: parses MeshLab project (`.mlp`) files directly, loads referenced meshes through the normal mesh plugin path, applies project labels/transforms, creates raster entries from project raster planes, stores project `CameraShot` data, records a `load_project` script action, and groups the operation under one undo step. Missing mesh files are logged and skipped; missing raster plane images are logged but still kept as plane metadata with a viewport-size fallback when possible.

`Document::saveMesh(...)`: resolves export plugin, passes `MeshIOSaveOptions` (mask, binary, embed textures, copy associated textures, Draco options).

`Document::saveMeshLabProject(...)`: writes `.mlp` project files directly from the current document. Save options can restrict export to visible meshes, re-save modified meshes, and copy external mesh/raster files into the project directory. Generated or source-less meshes are written under `meshes/`, raster snapshot images under `images/`, and the project XML preserves mesh transforms plus raster camera and plane metadata.

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

- `m_undoNodes` — flat arena of `UndoNode` objects; node id is the vector index
- node `0` — root state before the first recorded action
- `m_undoCurrentNode` — node id representing the current live state
- each `UndoNode` — incoming action label, parent id, child ids, display lane, preferred redo child, optional script-action record, and either a full `UndoState` snapshot or compact selection deltas

Committing an action appends a child to the current node, preserving alternate timelines instead of truncating siblings. `redo()` follows `preferredChild` when present, otherwise the first child. `jumpToUndoNode(nodeId, restoreCamera)` walks through the lowest common ancestor, suppressing intermediate GUI refresh signals and restoring camera only at the final target when requested. `undoTreeInfo()` exposes nodes, lanes, depths, current-node state, and current-path flags for `UndoGraphWidget`.

Undo-tree maintenance APIs keep the graph controllable after branching: `makeUndoRoot(nodeId)` promotes a chosen node to the new root and discards unreachable history, `purgeUndoBranch(nodeId)` deletes a descendant branch, and `linearizeUndoHistory()` keeps only the root-to-current path. Byte-aware pruning discards side branches first and then advances through old full checkpoints; delta nodes cannot become roots. These operations preserve the current live state and notify the UI through the normal undo/redo state signal.

Full snapshot nodes store an `UndoState` containing `MeshSnapshot` and `RasterSnapshot` arrays plus current mesh/raster/layer ids, next id counters, and one `ViewState`. A `MeshSnapshot` copies all cheap metadata fields by value (`transform`, names/paths/assets/materials/visibility/modified/mask/revisions) and holds geometry behind a `shared_ptr<const VCGMesh>`. `captureUndoState()` interns geometry objects in `m_undoGeometryCache` (keyed by `(meshId, geometryRevision, selectionRevision)`, stored as `weak_ptr`): if the full mesh content identity is unchanged since the last capture, nodes share the same allocation. A cache miss triggers a deep copy. On undo/redo, `restoreUndoState()` deep-copies geometry out of the shared pointer so the live document is always freely mutable. Main geometry replacement paths use document-level monotonic revision sources so branch-local edits do not accidentally reuse an older cache key after undo/redo navigation. Branch restore also evicts newer cached revisions for restored mesh ids to avoid stale geometry reuse across branches. `RasterSnapshot` stores raster metadata, `CameraShot`, planes, current plane, visibility, and raster image/camera revision ids. `undoMemoryStats()` de-duplicates geometry, includes packed selection-vector capacity, counts only image backing stores retained by history rather than the live document, and reports resources unique to a pending operation. Current-path rows describe references and are not additive.

Selection-only nodes can use `UndoStorageKind::Delta` instead of full snapshots. `DocumentUndoManager::beginDeltaStep(...)` captures a `SelectionDelta` for the target mesh before and after the operation, packing vertex and face selection bits. Undo/redo applies those deltas directly to the live mesh and bumps `selectionRevision`, avoiding a full geometry copy for rectangle/incremental selection filters while preserving the same tree behavior and script-action metadata.

Each `UndoState` also stores a `ViewState` snapshot (`src/render/viewstate.h`) captured via `Document::setViewStateFunctions(...)`. Current wiring captures/restores the active `RenderWidget` camera/render-style state (`ViewTrackball::State`, `GlobalRenderSettings`, per-mesh `PerMeshRenderSettings`) with each node. Per-view visibility vectors, UV pan/zoom, and view mode are not part of `ViewState`; camera restore can be skipped when jumping to a node.

APIs: `beginUndoStep(label)`, `beginUndoStep(label, ScriptAction)`, `beginUndoStep(label, meshIndexForSelectionDelta)`, `beginUndoStep(label, ScriptAction, meshIndexForSelectionDelta)`, `endUndoStep(commit, restoreOnCancel)`, `undo()`, `redo()`, `jumpToUndoNode(nodeId, restoreCamera)`, `updateUndoNodeCamera(nodeId)`, `makeUndoRoot(nodeId)`, `purgeUndoBranch(nodeId)`, `linearizeUndoHistory()`, `undoTreeInfo()`, `undoNodeScriptAction(nodeId)`, `clearUndoHistory()`, `setUndoLimit(limit)`, `setUndoMemoryLimitBytes(bytes)`, `pruneUndoToMemoryBudget(bytes)`, `handleUndoMemoryPressure(critical)`. Integrated with mesh mutations (`add/remove/duplicate/reload/rename/visibility`, `setMeshTransform`, `markMeshGeometryChanged`, `markMeshMaterialChanged`, `markMeshSelectionChanged`) and raster mutations (`add/remove/rename/visibility`, `setRasterShot`, `setCurrentRasterPlaneIndex`, `markRasterImageChanged`).

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
- fill material: `fillMaterial` (`Plain` / `Pbr` / `RadianceScaling`) + sub-structs `fillPlain` (`PlainFillParams`), `fillPbr` (`PbrFillParams`, including PBR texture sources, normal-map space, normal scale, AO strength, roughness factor), `fillRs` (`RsFillParams`, including RS shading mode)
- colors and sizes: `fillColor`, `wireColor`/`wireSize`, `edgeColor`/`edgeSize`, `pointColor`/`pointSize`, `bboxWireColor`, decorator colors, `decoratorBoundaryWidth`
- `pointColorSource`: `Constant` / `PerVertex` / `PerVertexQuality`
- `edgeColorSource`: `Constant` / `PerEdge`, serialized as `edge_color_source`; omitted fields retain the constant default for older render states. Explicit edges store RGBA in `VCGEdge::C()` and advertise populated colors with `IOM_EDGECOLOR`. PLY import/export and document copies preserve this data.

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

Cache keyed by `(QRhi*, meshId, variant, revision, quality-range mode/min/max, center-on-zero flag, percentile crop, wire faux-edge mode where relevant)`. Fill/wire/point variants use geometry/material revisions as appropriate, while selection resources include `selectionRevision` so selection overlays can refresh independently. `MeshSource` carries legacy texture paths, `textureAssets`, material metadata, and quality-range options. Invalidatable per-RHI or globally. Selection and decorator buffers are first-class cache outputs, including normals, boundary/seam lines, non-manifold edge/vertex markers, and curvature direction lines when the mesh provides the required data.

## Memory Diagnostics

`MainWindow::showMemoryInfo()` reads `cpuMeshMemoryStats`, `undoMemoryStats` (de-duplicates shared geometry), and `gpuMemoryStats` from `Document`.

These values are implementation-level mesh/cache estimates (capacity-based for VCG containers, VCG OCF side-data vectors, undo snapshots, and cache-owned GPU allocations), not full process RSS/Activity-Monitor memory.

For rendering pipeline details, see [Rendering](rendering.md).
