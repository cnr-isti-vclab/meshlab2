# Architecture

QMeshLab follows a **single-document, multi-view** architecture:

- one `Document` is the authoritative model
- UI widgets observe it through Qt signals
- rendering ownership is split between shared mesh GPU data and per-view pipelines/state

See also: [Data Model](data_model.md) · [Rendering](rendering.md) · [Memory Accounting](memory_accounting.md) · [Filter Organization](filter_organization.md)

## Architectural Layers

1. Application shell (`src/app`, `src/ui/MainWindow`)
2. Data model (`src/core/Document`, `VCGMesh`, `DocumentUndoManager`, logs, progress/cancel state)
3. Plugin interfaces/managers (`src/plugins`) and built-in plugin registration (`plugins/`)
4. Shared mesh GPU cache (`src/render/MeshGpuResourceCache`)
5. Per-view rendering and interaction (`RenderWidget`, `ViewTrackball`, `ViewAxisGizmo`, `RenderOverlayPanel`, `InteractiveTool`)
6. Optional embedded Python layer (`src/python`, `_qmeshlab`, `PythonHost`)
7. Auxiliary views (`LayerWidget` tree/table layer dock, `MeshFilterPanel`, log dock, undo graph, Python console, status-bar stats/progress)

## Core Components

### `Document`

Owns the ordered mesh list (`MeshEntry`), ordered raster list (`RasterEntry`), current mesh/raster indices, explicit active-layer kind, per-document log, the `DocumentUndoManager`, I/O and filter plugin managers, the shared GPU cache, render-state snapshot callback, and tracked mesh/image/undo/GPU memory APIs. Does not own live per-widget pipelines/camera state; those remain in `RenderWidget`. Undo nodes include serialized mesh/raster state or compact selection deltas, optional script-action metadata, and one `ViewState` snapshot captured/restored through callbacks (with camera restoration optionally skipped during history jumps). The undo manager can enforce an opt-in byte budget and defer OS-pressure purges until an active transaction closes.

Each `MeshEntry` stores: identity/revision keys (`meshId`, `geometryRevision`, `materialRevision`, `selectionRevision`), render placement (`transform`), source metadata (`name`, `sourcePath`, `ioMask`), texture metadata (`textureFileNames`, `textureFilePaths`, `textureAssets`), material set, `visible` and `modified` flags, and the `VCGMesh`.

Each `RasterEntry` stores: identity/revision keys (`rasterId`, `imageRevision`, `cameraRevision`), source metadata (`name`, `sourcePath`), `visible` flag, a `CameraShot`, a list of `RasterPlane` image planes, and `currentPlaneIndex`. A `RasterPlane` records semantic role (`RGBA`, masks, depth, or extra planes), display/source names, pixel size, and optional `QImage` payload.

### `CameraShot`

Core camera model (`src/core/camerashot.*`) used by raster layers and snapshot rendering. It wraps a VCG shot, carries viewport/pixel/focal/distortion/extrinsic fields, supports perspective/orthographic/isometric/cavalieri camera types, and provides projection, unprojection, depth, view-matrix, and projection-matrix helpers.

### `MeshGpuResourceCache`

Central cache for mesh GPU resources, keyed by `(QRhi*, meshId, variant, revision, quality-range mode/min/max/center/crop where applicable)`. Caches fill batches (vertex/index buffers + PBR textures), wire, edge, points, bbox, selection, and decorator buffers (normals, boundaries, seams, non-manifold markers, curvature directions). Allows GPU buffer reuse across render-mode changes and across views sharing the same `QRhi`.

### `LineRenderer`

Shared utility for generating triangle-expanded fat-line geometry from line segments. Used by the cache for edge/boundary/seam uploads and by `RenderWidget` fat-line pipelines.

### `RenderWidget`

`QRhiWidget` owning per-view state: pipelines/SRBs/UBOs, fallback textures, quality LUT texture, offscreen targets (depth pick, current-mesh mask), Radiance Scaling pre-pass resources, view mode (`Scene3D` / `ParametrizationUV` / `RasterImage`), `ViewTrackball`, headlight rotation/gizmo state, view-axis gizmo state, depth-cued tool-line resources, UV pan/zoom/cache, raster pan/zoom/opacity plus GPU image resources, per-mesh render modes, per-view visibility vector, active/suspended interactive-tool state, camera/render-state JSON capture/apply helpers, help/interaction overlays, tool overlays, decorator info overlay, and quality histogram cache. Implementation is split under `src/render/` across `renderwidget_{render,resources,selection,uv,modes,frame_plan,fill,scene_passes,raster}.cpp` plus the tool files under `src/render/tools/`.

Scene3D rendering is now organized around two internal data boundaries:

- `RenderFrameRequest` is the lightweight per-frame intent: view mode, viewport size, projection/view matrices, light direction, and `RenderFramePassRequests`.
- `RenderFramePlan` is the concrete GPU draw plan: fill items, simple buffer items, decorator items, and selection items holding pipelines, buffers, SRBs, and material renderer pointers.

`RenderFramePassRequests` is collected once from visible meshes, rasters, and per-mesh render settings, then reused by both GPU resource preparation and concrete plan construction. In Scene3D, raster camera layers plan small frustum glyphs rather than textured image planes. In `RasterImage` mode the request layer pins the current raster as the background reference and, when that raster has a valid camera, still feeds the normal mesh passes through the same Scene3D frame-plan path. This keeps the per-frame "what passes are needed?" decision in one place and prepares the codebase for later UV convergence. Public programmatic rendering targets versioned camera/render-state JSON; the concrete `RenderFramePlan` remains an internal process-local GPU object.

### `ViewState`

Lightweight snapshot type (`src/render/viewstate.h`) for one `RenderWidget`: `ViewTrackball::State`, `GlobalRenderSettings`, and per-mesh `PerMeshRenderSettings` map. `MainWindow` wires `Document::setViewStateFunctions(...)` so each undo node captures/restores the current view's camera/render-style state together with mesh data.

### `ViewTrackball`

Scene navigation: arcball/hyperbola rotation, pan, dolly, `Shift+Wheel` vertigo (FOV + compensating dolly), reset/reframe, animated recenter. The companion `ViewAxisGizmo` draws the corner orientation widget, uses the `view.axisGizmoSize` preference, and lets axis clicks snap the current 3D view.

### `InteractiveTool`

Base class for interactive editing tools. A tool owns mouse/keyboard handling for one `RenderWidget` at a time, shows live feedback/cursors, and commits durable document changes by calling `Document::runFilter(...)` once. This keeps tool edits in the same undo tree and Python script-action history as menu-invoked filters.

Built-in tools are registered by `createBuiltinInteractiveTools()`:

- `SelectLayerTool`: uses asynchronous surface picking to make the clicked mesh layer current.
- `RubberBandSelectTool`: runs the `select_by_rectangle` filter in Scene3D or UV space, with replace/add/subtract modes and face/vertex selection.
- `MeasureTool`: uses asynchronous surface picks to place/edit endpoints, draws 2D labels plus depth-cued segments, can print/save measurements, and can export them as an edge-only document layer.
- `TransformTool`: provides Blender-style `G`/`R`/`S` layer transforms with axis/plane constraints, numeric entry, a toolbar icon, depth-cued constraint guides, and one filter-backed commit per gesture.

`MainWindow` owns the tool instances and toolbar/menu actions. A tool is pinned to its owner view; `Tab` temporarily suspends the tool so camera navigation can use the same gestures, and `Esc` exits the tool.

### `RenderOverlayPanel`

Compact pass/settings panel: pass toggles, mode-specific world settings page (Scene vs UV), per-pass style controls (colors, widths, lighting/culling, quality histogram), PBR texture/normal-space controls, decorator info overlay toggle, apply-current-settings-to-visible-meshes actions, and `PerMeshRenderSettings`/`GlobalRenderSettings` sync.

### `LayerWidget`

Layer dock with two synchronized presentations over the same `Document`: a detailed tree view and a sortable table view. Mesh rows expose visibility, loaded/generated/modified markers, counts, texture/material/attribute summaries, and current-layer selection. Raster rows expose visibility, active raster selection, current plane, plane export, image size, plane count, and camera summary.

### `MeshFilterPanel`

Filter browser/runner: search box, parameter form from `MeshFilterDescriptor`, optional markdown description, advanced-parameter toggle, reset-to-defaults button, per-filter parameter-value cache, current-view providers for camera/render-state JSON parameters, and, when Python support is compiled in, compact/full Python call formatting shared with action-history export.

### `MainWindow`

Orchestrates the central splitter (one or more `RenderWidget`s), right-column docks (`LayerWidget` + `MeshFilterPanel`), bottom log/Python docks, status bar (progress bars, frame-time stats), undo graph panel, tool toolbar/menu, and menus (file, edit, filters, view, help). Manages mesh/project/raster file open/save/drop flows, split/close, active-view highlight border, optional camera synchronization across 3D views, camera-state copy/paste, center-on-selection, document visibility proxy synchronization from the current view, view PNG snapshots, snapshot-to-raster creation, render-state snapshot callbacks for filters, undo-node thumbnails/snapshots, action-history Python script generation into the script editor, and embedded Python console visibility when enabled. It also presents the separated OS/CPU/GPU memory report and connects the platform `MemoryPressureMonitor` to opt-in undo purging.

### `PythonHost` and `_qmeshlab`

When `MESHLAB2_PYTHON_CONSOLE` is enabled, `src/app/main.cpp` registers the statically linked nanobind module `_qmeshlab` with `PyImport_AppendInittab` before `QApplication` starts. `PythonHost` owns the embedded CPython interpreter, redirects `stdout`/`stderr` to Qt signals, creates the combined script editor/interactive console, injects the live document as `ms`, injects the live view helper as `mlgui`, and installs the public `pymeshlab2` facade backed by the private extension. `_qmeshlab.MeshSet`/`MeshSetCore` wraps either a borrowed live `Document` (embedded console) or an owned standalone `Document`, exposes mesh/raster/project load/save helpers, lists filters, applies filters by key/id/Python name, and renders snapshots through the live view or `HeadlessRenderContext`.

## Render Planning Types

Defined privately in `RenderWidget`:

- **`RenderMeshPassRequests`** — one visible mesh plus the per-mesh pass requirements derived from `PerMeshRenderSettings`: fill, wire, edges, bbox, points, selection, decorator normals, and decorator boundary/seam/non-manifold data.
- **`RenderFramePassRequests`** — the frame-wide aggregate of visible `RenderMeshPassRequests` plus raster backplate/projected/frustum requests; answers whether any family of passes is needed and is used by resource preparation.
- **`RenderFrameRequest`** — the lightweight frame request object containing camera/viewport/light state plus pass requests.
- **`RenderFramePlan`** — the concrete draw list consumed by pass executors, including fill/simple/decorator/selection draw items and raster draw items. Presence of a pass is derived from planned draw items (`hasFillPass()`, `hasSceneDrawItems()`, etc.), not from separate request booleans.

The high-level Scene3D flow is:

```text
visible meshes + PerMeshRenderSettings
   │
   ▼
RenderFramePassRequests
   │
   ├──▶ prepareDirtyBuffers(...) / MeshGpuResourceCache
   │
   ▼
RenderFrameRequest
   │
   ▼
RenderFramePlan
   │
   ▼
fill/simple/decorator/selection pass executors
```

## Render Settings Types

Defined in `renderingsettings.h`:

- **`PerMeshRenderSettings`** — one instance per mesh id in `RenderWidget::m_meshRenderModes`. Holds pass toggles, decorator toggles (normals, boundary/seams, non-manifold markers, curvature directions), lighting/culling flags, wire faux-edge handling, fill material and sub-structs (`PlainFillParams`, `PbrFillParams`, `RsFillParams`), PBR normal-map space, colors, sizes, and point color source.
- **`GlobalRenderSettings`** — one instance per view in `m_renderSettings`. Holds scene highlight parameters, background colors, UV viewer options, quality histogram/range/isolines options, and overlay panel state. `using RenderSettings = GlobalRenderSettings` is provided as an alias.
- **`MeshRenderMode`** — widget-local alias for `PerMeshRenderSettings`.

## Plugin System

**I/O plugins** (`MeshIOPlugin`): `canLoad`/`load`, `canSave`/`save`, dialog filter strings, mask capability. MeshLab project (`.mlp`) loading and saving are handled directly by `Document` so project files can combine plugin-loaded/saved mesh files with transforms, generated mesh paths, raster planes, and raster camera shots.

**Filter plugins** (`MeshFilterPlugin`): plugin id/name, `filters(const Document&)` returning descriptors (domain/codomain, validated ordered categories, requirements, tags, structured provenance/references, parameters, `pythonName`), `runFilter(id, params, Document&)`.

**Managers** (`MeshIOPluginManager`, `MeshFilterPluginManager`): keep plugins in registration order; I/O manager stores per-extension preferred plugin in `QSettings`. Registration via `plugins/meshpluginregistry.*` and `plugins/filterpluginregistry.*`.

Built-in I/O (when enabled at build time): `io_vcg`, `io_obj_rapidobj`, `io_3mf`, `io_gltf`, `io_e57`, `io_trueform`.
Built-in filter plugins (when enabled at build time): `filter_basic`, `filter_camera`, `filter_cgal`, `filter_clean`, `filter_color_projection`, `filter_colorproc`, `filter_create`, `filter_embree`, `filter_expression`, `filter_geodesic`, `filter_icp`, `filter_igl`, `filter_img_patch_param`, `filter_instant_meshes`, `filter_layer`, `filter_measure`, `filter_meshfix`, `filter_meshing`, `filter_mls`, `filter_plymc`, `filter_quadwild`, `filter_qslim`, `filter_sampling`, `filter_screened_poisson`, `filter_select`, `filter_texture`, `filter_texture_defragmentation`, `filter_trioptimize`, `filter_trueform`, `filter_unsharp`, `filter_vertex_displacement`, `filter_voronoi`, `filter_xatlas`.

## State Ownership

| Shared (Document) | Per-view (RenderWidget) |
|---|---|
| mesh geometry, material, and selection data | per-mesh render modes/styles |
| mesh transforms | per-view visibility vector |
| mesh/raster lists, current indices, active layer kind | view mode, camera/trackball, headlight direction, UV pan/zoom, raster pan/zoom/opacity |
| document visibility proxy | overlay settings, histogram cache, active/suspended tool state and transient tool feedback |
| logs, progress, plugin registries | pipelines, SRBs, UBOs, render targets |
| undo tree, labels, lanes, full snapshots, selection deltas, script-action records | UV-local GPU cache, raster GPU image cache |
| shared mesh GPU cache | |

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
7. Optional Python console/script-editor calls route through `pymeshlab2.MeshSet`/`_qmeshlab` back into the same `Document` and filter manager, or into an owned standalone document for headless-style scripts.
8. Status bar shows load/filter progress and rolling CPU/GPU frame timings.
