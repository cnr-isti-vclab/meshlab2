# Architecture

MeshLab follows a **single-document, multi-view** architecture:

- one `Document` is the authoritative model
- UI widgets observe it through Qt signals
- rendering ownership is split between shared mesh GPU data and per-view pipelines/state

See also: [Data Model](data_model.md) · [Rendering](rendering.md) · [Memory Accounting](memory_accounting.md) · [Filter Organization](filter_organization.md)

## Architectural Layers

1. Application shell (`src/app`, `MainWindow` in `src/ui`)
2. Data model (`src/core`: `Document`, `VCGMesh`, `DocumentUndoManager`, logs, progress/cancel state)
3. Plugin interfaces/managers (`src/plugins`) and built-in plugin registration (`plugins/`)
4. Shared mesh GPU cache (`MeshGpuResourceCache` in `src/render`)
5. Per-view rendering and interaction (`RenderWidget`, `ViewTrackball`, `ViewAxisGizmo`, `RenderOverlayPanel`, `InteractiveTool`)
6. Optional embedded Python layer (`src/python`, `_meshlab`, `PythonHost`)
7. Auxiliary views (`LayerWidget` tree/table layer dock, `MeshFilterPanel`, log dock, undo graph, Python console, status-bar stats/progress)

## Core Components

### `Document`

The authoritative model: the ordered mesh (`MeshEntry`) and raster (`RasterEntry`) lists, the current layer, the log, the undo history (`DocumentUndoManager`), the I/O and filter plugin managers, the shared GPU cache, and memory accounting. It holds no per-view state. Pipelines, cameras and render modes stay in each `RenderWidget` and reach the document only through two callbacks: the `ViewState` an undo node captures and restores, and the render-state snapshot a filter asks for. What a layer record holds is in [Data Model](data_model.md#document-ownership), the history tree in [Data Model](data_model.md#undoredo-model), and the undo budget in [Memory Accounting](memory_accounting.md).

### `CameraShot`

Core camera model (`src/core/camerashot.*`) used by raster layers and snapshot rendering. It wraps a VCG shot, carries viewport/pixel/focal/extrinsic fields (the lens is the raster's, not the shot's), supports perspective and orthographic cameras, and provides projection, unprojection, depth, view-matrix, and projection-matrix helpers.

### `MeshGpuResourceCache`

The mesh GPU resources, owned by `Document` and keyed by `QRhi`, mesh, product and revision, so a render-mode change, or another view on the same `QRhi`, reuses buffers instead of rebuilding them. Keys, products and lifetime are in [Rendering](rendering.md#shared-gpu-cache-meshgpuresourcecache).

### `LineRenderer`

Shared utility for generating triangle-expanded fat-line geometry from line segments. Used by the cache for edge/boundary/seam uploads and by `RenderWidget` fat-line pipelines.

### `RenderWidget`

The `QRhiWidget` of one view, in one of three modes: `Scene3D`, `ParametrizationUV` or `RasterImage`. It owns everything that is per view: pipelines and offscreen targets, the camera, render modes, visibility, overlays and the active tool (see [State Ownership](#state-ownership)). It draws the document's meshes from the shared cache. The implementation is split under `src/render/` across `renderwidget_{render,resources,selection,uv,modes,frame_plan,fill,scene_passes,raster}.cpp`, with the tools in `src/render/tools/`.

A `Scene3D` frame, and the meshes a `RasterImage` view draws through its raster's camera, is planned in two steps. A lightweight `RenderFrameRequest` says which passes the visible layers need; the concrete `RenderFramePlan` holds the pipelines and buffers that draw them ([Rendering](rendering.md#scene3d-request-and-plan-pipeline)). The plan is internal: programmatic rendering goes through the versioned render-state JSON.

### `ViewState`

Lightweight snapshot type (`src/render/viewstate.h`) for one `RenderWidget`: `ViewTrackball::State`, `GlobalRenderSettings`, and per-mesh `PerMeshRenderSettings` map. `MainWindow` wires `Document::setViewStateFunctions(...)` so each undo node captures/restores the current view's camera/render-style state together with mesh data.

### `ViewTrackball`

The camera of a 3D view: an orbit centre, rotation, distance, field of view and projection, with animated moves. Its state is the `MeshLab.CameraState` JSON (`cameraStateJson`, `stateFromCameraStateJson`). The companion `ViewAxisGizmo` draws the corner orientation widget. The gestures that drive both are in [User Interface](user_interface.md#scene3d-camera-and-interaction).

### `InteractiveTool`

Base class for interactive editing tools. A tool owns mouse/keyboard handling for one `RenderWidget` at a time, shows live feedback/cursors, and commits durable document changes by calling `Document::runFilter(...)` once (all but the measuring tool's *Export Layer*, which adds its layer directly in an undo step of its own). This keeps tool edits in the same undo tree and Python script-action history as menu-invoked filters.

The built-in tools and how each behaves are in [User Interface](user_interface.md#interactive-tools).

### `RenderOverlayPanel`

The per-view pass bar and settings pages, writing `PerMeshRenderSettings` and `GlobalRenderSettings`; see [User Interface](user_interface.md#renderoverlaypanel).

### `LayerWidget`

The layer dock: a tree and a table over the same `Document`; see [User Interface](user_interface.md#layerwidget).

### `MeshFilterPanel`

The filter browser and runner: search, a parameter form built from the descriptor, and the Python call of the filter; see [User Interface](user_interface.md#meshfilterpanel).

### `MainWindow`

Builds the window and wires it to the document. It holds the central splitter of `RenderWidget`s, the layer, filter, log, Python and history docks, the status bar, the menus and the tool actions. It runs the file flows (open, save and drop of meshes, projects and rasters), splitting and closing views (the active one marked by a highlight border), camera synchronization and copy/paste, snapshots, and history-to-script export. It installs the document's two view callbacks (`ViewState` and render-state snapshots), keeps the undo-node thumbnails, and keeps the document's visibility in step with the current view. It also connects the platform `MemoryPressureMonitor` to opt-in undo purging. What each part does for the user is in [User Interface](user_interface.md).

### `PythonHost` and `_meshlab`

When `MESHLAB2_PYTHON_CONSOLE` is enabled, `src/app/main.cpp` registers the statically linked nanobind module `_meshlab` with `PyImport_AppendInittab` before `QApplication` starts. `PythonHost` owns the embedded CPython interpreter, redirects `stdout`/`stderr` to Qt signals, creates the combined script editor/interactive console, injects the live document as `ms`, injects the live view helper as `mlgui`, and installs the public `pymeshlab2` facade backed by the private extension. `_meshlab.MeshSet`/`MeshSetCore` wraps either a borrowed live `Document` (embedded console) or an owned standalone `Document`, exposes mesh/raster/project load/save helpers, lists filters, applies filters by key/id/Python name, and renders snapshots through the live view or `HeadlessRenderContext`.

## Plugin System

**I/O plugins** (`MeshIOPlugin`): `canLoad`/`load`, `canSave`/`save`, dialog filter strings, mask capability, and for the formats that state one the unit of their coordinates (see the Unit paragraph of [data_model.md](data_model.md)). MeshLab project (`.mlp`) loading and saving are handled directly by `Document` so project files can combine plugin-loaded/saved mesh files with transforms, generated mesh paths, raster planes, and raster camera shots.

**Filter plugins** (`MeshFilterPlugin`): plugin id and name; `filters(const Document&)`, which by default loads the descriptors from the plugin's `filters.json` and resolves their bound tokens against the document ([Adding a Filter](adding_a_filter.md)); and `runFilter(id, params, Document&)`.

**Managers** (`MeshIOPluginManager`, `MeshFilterPluginManager`): keep plugins in registration order; I/O manager stores per-extension preferred plugin in `QSettings`. Registration via `plugins/meshpluginregistry.*` and `plugins/filterpluginregistry.*`.

The built-in plugins are those registered in `plugins/meshpluginregistry.cpp` and `plugins/filterpluginregistry.cpp`, each when its dependencies are found; the [README](../../readme.md) lists them.

## State Ownership

| Shared (Document) | Per-view (RenderWidget) |
|---|---|
| mesh geometry, material, and selection data | per-mesh render modes/styles |
| mesh transforms | per-view visibility vector |
| mesh/raster lists, current indices, active layer kind | view mode, camera/trackball, headlight direction, UV pan/zoom, raster pan/zoom/opacity |
| document visibility proxy (`MeshEntry::visible`) | overlay settings, histogram cache, active/suspended tool state and transient tool feedback |
| logs, progress, plugin registries | pipelines, SRBs, UBOs, render targets |
| undo tree, labels, snapshots with shared geometry and attribute layers, script-action records | UV-local GPU cache, per-mesh active UV texture group, raster GPU image cache |
| shared mesh GPU cache | |

`MainWindow` synchronizes the document's visibility from the active view so the layer panel reflects it; each view keeps its own visibility vector.

Note: undo history stores `ViewState` snapshots, but this is serialized view data attached to undo nodes, not live ownership of render widgets.

The optional Python host is process-global rather than owned by `Document` or `RenderWidget`; in embedded mode it borrows the live document and the Python console widget is just a UI front-end for that host.

## Data Flow

```text
User Action
   │
   ▼
MainWindow (menus, docks, split-view orchestration)
   │
   ├──▶ Document (mesh/project/raster load/save, filter, undo/redo, logs, progress/cancel)
   │         │
   │         └──▶ MeshGpuResourceCache (shared per-mesh GPU data)
   │
   ├──▶ RenderWidget(s) (per-view passes, camera, modes, render-state JSON)
   └──▶ LayerWidget / MeshFilterPanel / Log Dock
```

## Typical Runtime Sequence

1. User opens or drops files → `Document` resolves mesh plugins, MeshLab project loading, or raster image loading, appends layers, and emits signals.
2. Views sync mesh/mode/visibility and collect frame pass requests.
3. Scene3D prepares shared GPU resources from those requests, builds a concrete `RenderFramePlan`, and executes layered passes. UV mode still uses a separate UV renderer and cache.
4. Undo/redo or undo-graph jumps restore mesh snapshots and the active view snapshot (`ViewState`).
5. Interactive tools either update transient view feedback or commit one durable filter-backed change.
6. Filter runs through the filter manager with progress/cancel, typed parameters, render-state capture when requested, undo integration, and compact/full Python action records.
7. Optional Python console/script-editor calls route through `pymeshlab2.MeshSet`/`_meshlab` back into the same `Document` and filter manager, or into an owned standalone document for headless-style scripts.
8. Status bar shows load/filter progress and rolling CPU/GPU frame timings.
