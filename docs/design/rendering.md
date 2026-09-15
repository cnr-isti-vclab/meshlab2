# Rendering

See also: [Architecture](architecture.md) · [Data Model](data_model.md) · [Memory Accounting](memory_accounting.md)

## Overview

`RenderWidget` (`QRhiWidget`) runs in three modes:

- **`Scene3D`**: layered mesh rendering with trackball camera, depth picking, and current-mesh highlight.
- **`ParametrizationUV`**: orthographic UV-space rendering for the current mesh (requires faces + UV coords).
- **`RasterImage`**: active-raster image-domain rendering. Entering the mode requires a valid current raster. The active raster is drawn as a screen-space overlay reference; if it has a valid `CameraShot`, visible meshes are rendered by the normal Scene3D pass pipeline through that raster camera.

Ownership: `Document` owns canonical mesh/raster data; `MeshGpuResourceCache` (owned by `Document`) holds shared GPU mesh resources; `RenderWidget` owns per-view pipelines/SRBs/UBOs, offscreen targets, camera/UV state, raster GPU image resources, headlight/trackball/view-axis gizmo state, overlays, active/suspended interactive-tool state, depth-cued tool-line resources, and per-mesh render modes.

Undo/redo integration: undo-tree nodes include one `ViewState` snapshot (active view camera + render settings + per-mesh style map) captured/restored via `Document::setViewStateFunctions(...)`. History jumps can restore render style while intentionally leaving the live camera untouched until the final target node.

## Programmatic Render-State JSON

`RenderWidget` exposes a versioned render-state JSON contract intended for repeatable programmatic rendering (for example, filters that need deterministic offscreen snapshots):

- `RenderWidget::renderStateJson()` exports the current per-view rendering state.
- `RenderWidget::applyRenderStateJson(...)` applies such JSON back to a view.

Schema (`kind = "MeshLab.RenderState"`, `version = 1`) includes:

- `view_mode`: `Scene3D` | `ParametrizationUV` | `RasterImage`
- `raster_opacity`: float in `[0, 1]`
- `trackball`: same trackball payload used by camera JSON (`center`, `rotation_xyzw`, `distance`, `radius`, `fov_y_degrees`, near-clip and gizmo fields)
- `render_settings`: sparse `GlobalRenderSettings` payload (background, histogram settings, UV options, current-mesh highlight settings, etc.; default-valued fields are omitted)
- `mesh_visibility`: per-view visibility bool array, emitted only when at least one mesh is hidden in the view
- `mesh_render_modes`: array of `{ mesh_id, settings }`, where `settings` is a sparse `PerMeshRenderSettings` payload
- `current_mesh_index`, `current_raster_index`, `current_layer_kind`
- `viewport_px`: informational viewport size metadata

Design notes:

- Mesh render modes are keyed by persistent `mesh_id`, not by row index.
- The JSON is intentionally request-level and compact; GPU resources and concrete draw plans remain internal.
- Export omits default-valued settings. During import, omitted settings/mode maps start from defaults, while omitted `view_mode` and `raster_opacity` keep the current view values.
- This contract is forward-versioned (`version`) and can be extended without changing filter call sites.

Filter integration:

- `MainWindow` wires `Document::setRenderStateSnapshotFunction(...)` to the active view. `Document::renderSnapshotFromStateJson(...)` applies JSON to that view, renders an offscreen image at the requested size, returns both the `QImage` and resulting `CameraShot`, then restores the previous render state.
- Layer menu filter `Render from Render-State JSON` (`id: render_from_render_state_json`) consumes typed `camera_state` (`MeshLab.CameraState`) and `render_state` (`MeshLab.RenderState`) payloads and runs an offscreen render.
- Both parameters support the same UI source modes: inline text, JSON file, or capture from current view.
- The filter can write a PNG snapshot and/or inject the snapshot as a raster layer, enabling reproducible scriptable rendering workflows.

## Scene3D Request and Plan Pipeline

Scene3D rendering is split into a lightweight request layer and a concrete GPU draw-plan layer.

`RenderFramePassRequests` is collected once per frame from visible meshes, visible/current rasters, and resolved `PerMeshRenderSettings`. It contains one `RenderMeshPassRequests` per visible mesh, raster backplate/projected/frustum requests when relevant, plus frame-wide aggregate flags for fill, wire, edges, bbox, points, selection, decorator normals, decorator boundary/seam/non-manifold resources, and raster drawing.

The same request object is used for two things:

1. `prepareDirtyBuffers(...)` asks `Document::ensureMeshGpuResources(...)` only for the cache products needed by the requested passes, plus extra resources needed by current-mesh highlighting.
2. `buildRenderFramePlan(...)` converts the request into concrete draw items after resources/pipelines are available.

`RenderFrameRequest` adds frame-local state to the pass requests: view mode, pixel size, projection matrix, view matrix, light direction, and raster overlay state (opacity plus raster pan/zoom in `RasterImage` mode). `RenderFramePlan` is the concrete in-process result: fill items, buffer items, decorator items, selection items, and raster draw items with QRhi buffers/pipelines and material renderer pointers. Pass presence is derived from non-empty draw-item lists (`hasFillPass()`, `hasSceneDrawItems()`, etc.).

This is an internal architecture boundary, not a public serialization contract. The implemented programmatic path serializes camera/render-state intent and lets MeshLab build the GPU `RenderFramePlan` internally.

## Shared GPU Cache (`MeshGpuResourceCache`)

Cache key: `(QRhi*, meshId, variant, geometryRevision, materialRevision, selectionRevision where relevant)`. Quality variants also include fixed-range mode, min/max, center-on-zero, and percentile crop. Wire resources also track whether faux polygon edges should be respected.

Cached outputs:

- **fill**: one or more batches — vertex/index buffers, optional base/normal/occlusion/roughness textures and per-batch PBR factors. Variants: `Constant`, `PerVertex`, `PerFace`, `PerVertexQuality`, `PerFaceQuality`, `Texture`. Texture lookup can use legacy texture paths, `MeshIOTextureAsset` entries, and material slots. PBR normal textures can be interpreted as tangent-space maps or object-space maps according to `fillPbr.normalMapSpace`.
- **wire**: barycentric-expanded triangle buffer, optionally honoring faux-edge bits for polygonal faces.
- **edges**: line buffer + fat-line buffer from explicit mesh edges. Variants: `Constant`, `PerEdge`. Each colored segment repeats its RGBA at both endpoints and across its fat-line triangles. The geometry-only variant remains available for highlight/depth masks; color variants also track material revisions and edge-color availability.
- **points**: position/color/normal payload + normal-valid flag. Variants: `Constant`, `PerVertex`, `PerVertexQuality`.
- **bbox**: line buffer.
- **selection**: selected-face triangles, selected-vertex points, keyed on `selectionRevision` so selection overlays rebuild without invalidating fill/wire/point resources.
- **decorators**: vertex normals, face normals, boundary edges (line + fat-line), texture seams (line + fat-line), non-manifold edges (line + fat-line), non-manifold vertices, and curvature principal-direction lines.

`MeshGpuResourceCache::gpuMemoryStats()` sums the QRhi buffer sizes and known RGBA8
texture payloads for all cache variants and RHIs, including non-manifold decorator
buffers. This is a **logical mesh-cache size**, not total GPU or process memory: per-view
UV/raster/offscreen resources, backend alignment, staging/command storage, and driver
allocations are outside it. The Memory Info dialog therefore presents it separately from
tracked CPU allocations and OS physical footprint.

Fill uses an indexed path (shared vertices) or an expanded-triangle path for per-face colors or texture batching. For quality variants, normalized quality is stored in the buffer and resolved via LUT sampling in shaders. Changing colormap, inversion, or isoline settings updates only the per-view LUT texture; changing fixed range, center-on-zero, or percentile crop changes normalization and rebuilds the affected quality buffers.

Boundary extraction: topological edge incidence (`incidentCount == 1`). Non-manifold edge extraction: topological edge incidence above two. Seam extraction: per-topological-edge UV sample comparison (texture-index changes, missing/invalid UV).

## Per-Mesh Render Modes

`RenderWidget` holds a `PerMeshRenderSettings` per mesh id (`m_meshRenderModes`; `MeshRenderMode` is a local alias). View-level settings are in `m_renderSettings` (`GlobalRenderSettings`). See [Data Model](data_model.md) for field details.

Default mode for new meshes:
- surfaces (`FN > 0`): fill on, wire on for `FN < 10000`
- edge-only meshes: edges on (`edgeSize = 4.0`); with vertex colors, points are also on (`pointSize = 8.0`, per-vertex color)
- point-only: points on

Default fill color source preference: texture → per-vertex → per-face → per-vertex-quality → per-face-quality → constant, clamped to mesh `ioMask` + texture availability. Texture availability is based on `Document::meshTextureAssociationCount(...)`.

## `Scene3D` Frame Sequence

1. Advance animation/camera state, sync per-mesh render modes, and update the camera frame when needed.
2. Collect `RenderFramePassRequests` once for visible meshes and raster-layer requests.
3. Prepare shared mesh GPU resources from those requests.
4. Upload per-frame textures/UBOs such as the quality LUT, gizmo buffers, light gizmo UBO, and background gradient.
5. Execute depth pick (if scheduled).
6. Build current-mesh highlight masks (if `highlightCurrentMesh`).
7. Build a concrete `RenderFramePlan` from a `RenderFrameRequest`.
8. Run Radiance Scaling gradient pre-pass (if any planned fill item uses `FillMaterial::RadianceScaling`).
9. Run main onscreen pass.

Main pass draw order: scene background · raster background in RasterImage mode only · fill · wire · edges · bbox · points · raster camera frustums · decorators · trackball gizmo · light gizmo · depth-cued tool guides · current-mesh outline/debug composite · selection overlay · 2D informational/tool overlays.

## `Scene3D` Pass Details

**Scene background**: full-screen gradient triangle, `sceneBackgroundBottomColor`/`TopColor`, drawn first.

**Fill**: Fill planning and execution are isolated in `renderwidget_fill.cpp`. `SceneFillFramePlan` contains fill draw items, each with a material renderer selected from `PlainFillRenderer`, `PbrFillRenderer`, or `RadianceScalingFillRenderer`. Shared behavior such as UBO upload, texture SRB resolution, selected PBR texture lookup, and Radiance Scaling gradient resource access goes through `FillRenderServices`.

Smooth/Flat shading use distinct shader pairs. Depth test+write on; `fillBackfaceCulling` controls culling. Quality variants LUT-sample from the per-view colormap texture, including optional isoline stripes. Quality normalization honors fixed range, center-on-zero, and percentile crop settings; automatic ranges default to a `0.01` crop at both tails to reduce outlier influence when filters map newly computed quality values to color. PBR binds base/normal/occlusion/roughness per batch. Tangent-space normal maps derive TBN from screen-space derivatives of view position and UV; object-space normal maps are transformed by the normal matrix and blended with the base normal using `normalScale`. Radiance Scaling pre-pass is implemented by `RadianceScalingFillRenderer::renderPrepass(...)`; it renders planned RS fill batches into `m_rsGradTexture` (`RGBA32F`), storing `(gx, gy, logZ, 1)`, and the main fill pass samples it for final RS shading.

**Wireframe**: barycentric triangles + fragment edge test; depth `LessOrEqual`, no depth write; alpha blending; `wireBackfaceCulling`; optional `wireRespectFaux` controls faux polygon edge handling in the cached wire data.

**Edges**: fat-edge triangles when available, line fallback; depth `LessOrEqual`; alpha blending; width from `edgeSize`. `edgeColorSource` chooses a constant color or the edge RGBA imported from PLY (RGB defaults to opaque alpha). New meshes prefer per-edge colors when present. With points enabled, edges do not write biased depth, keeping the subsequently drawn vertex markers visible while surfaces still occlude them. UV face outlines use constant color.

**Bounding box**: line topology; depth on, no depth write.

**Points**: `QRhiGraphicsPipeline::Points`; depth test+write; point lighting, size, and color source from settings; quality variant LUT-sampled.

**Decorators**: depth `LessOrEqual`, no depth write. Normals and curvature directions use the line pipeline. Boundary, seams, and non-manifold edges use the fat-decorator pipeline (`decoratorBoundaryWidth`) with line fallback. Non-manifold vertices use a point pipeline.

**Selection overlay** (final pass): semi-transparent red fill triangles + red vertex points; depth `LessOrEqual`, no depth write; per-mesh `showSelection`/`showSelectionFaces`/`showSelectionVertices`. Scene3D selection resources come from the shared mesh GPU cache; UV mode has a dedicated UV-space selection overlay.

**Decorator info overlay**: optional 2D label controlled by `showDecoratorInfo`. It reports numeric counts for enabled decorator data on the current mesh (boundary/seam/non-manifold families) and hides itself when no relevant decorator information is available.

Simple buffer pass execution (wire, edges, bbox, points), decorator execution, and selection execution are isolated in `renderwidget_scene_passes.cpp`. Their draw order remains controlled by `renderwidget_render.cpp`.

**Rasters**: `renderwidget_raster.cpp` owns per-raster texture upload and draw execution. In `Scene3D`, rasters with valid camera shots draw as small line frustums with the apex at the raster camera origin and the base oriented by the camera view frustum; the current raster frustum is highlighted in warm yellow while other raster frustums use blue. Rasters without cameras are not drawn in the 3D view. The raster image itself is not pasted into the 3D scene. In `RasterImage`, only the active raster layer is requested as the full-viewport background reference, and mesh passes are included only when that raster has a valid camera.

## Current Mesh Highlight

Runs when `highlightCurrentMesh` is on and the current mesh is visible.

**Surface/edge path**: render current mesh depth mask → extract silhouette into `work` → render all other meshes into `mask` for occlusion → composite outline. Occluded outline portions use half alpha.

**Point-cloud path**: render occupancy mask → dilate (`base → work`) → erode (`work → mask`) → composite outline from final mask.

Debug views: `FullMask`, `VisibleMask`, `OccludedMask`, `DilatedMask`, `ErodedMask`. Normal path is `Outline`.

## Depth Picking

Double click schedules an offscreen depth-pick frame: depth encoded in RGB → one pixel read back → backend conventions normalized (Y flip, clip-depth range) → unprojected via inverse MVP → `trackballCenterPicked(worldPos)` emitted, animated recenter starts.

## `Scene3D` Camera and Interaction

`ViewTrackball`: left drag = arcball/hyperbola rotation; middle/right drag or `Ctrl+Left` = pan; wheel = dolly; `Shift+Wheel` = vertigo (FOV + compensating dolly); double click = depth-pick + animated recenter. `Ctrl+Shift+Left` rotates the view-space headlight and shows the light gizmo while dragging. Gizmos are depth-aware and scale-stable across dolly/FOV changes. The corner `ViewAxisGizmo` uses the `view.axisGizmoSize` preference and lets axis clicks snap the 3D camera orientation. `MainWindow` can optionally synchronize camera state across 3D views and exposes a `Center on Selection` camera command; UV views keep independent pan/zoom.

## Interactive Tools

Interactive tools are thin view-owned interaction front-ends over filter/document operations. A tool is active in at most one `RenderWidget`; it owns mouse/keyboard input while active, shows a cursor/status hint/badge/icon, can paint a 2D overlay, and can contribute depth-cued line segments to the scene pass. Durable edits commit through `Document::runFilter(...)` when possible so undo nodes and Python action history stay coherent. `Tab` suspends the tool and temporarily returns gestures to camera navigation; `Esc` exits the tool or cancels the active gesture.

Current built-in tools:

- `Select Layer`: click in Scene3D to schedule an asynchronous GPU surface pick; on hit, the picked mesh becomes the current mesh layer.
- `Rubber-band Select`: drag a rectangle in Scene3D or UV mode, then run `meshlab2.filter.select::select_by_rectangle`. `Shift` adds, `Ctrl` subtracts, and `F`/`V` switch between face and vertex selection. The tool passes either camera-state JSON or UV pan/zoom parameters depending on the active view mode.
- `Measuring Tool`: click surface points in Scene3D to build/edit measurement segments. It uses asynchronous surface picking for placement and drag previews, draws labels and depth-cued segments, `C` clears, Backspace removes the last segment, `P` prints to the log, `S` saves a TSV, and `X` exports the measurements as an edge-only mesh layer.
- `Transform Layer`: modal layer transform tool for the current mesh. `G`, `R`, and `S` start translate/rotate/scale gestures; `X`/`Y`/`Z` constrain to an axis, `Shift` plus an axis constrains to the perpendicular plane, typed numbers set exact values, and Enter/click commits. During preview it updates the layer matrix directly, then restores the original matrix and commits exactly one transform filter so undo/script history remain clean. The tool exposes the shared `:/img/axis.png` toolbar icon.

`Rubber-band Select` is the only tool that currently supports `ParametrizationUV`. The other built-ins are Scene3D-only, and Raster mode remains image-navigation oriented.

## Camera Models: `CameraShot` vs `ViewTrackball`

MeshLab uses two distinct camera representations that overlap but are not interchangeable:

| | `CameraShot` | `ViewTrackball` |
|---|---|---|
| **Type** | Pinhole camera (eye position, view direction, FOV, intrinsics) | Orbit camera (center, rotation, distance, FOV) |
| **Stored in** | `Document::RasterEntry::shot` (raster cameras), serialized from MLP | `RenderWidget::m_trackball` (view navigation) |
| **Provides** | `project()`, `unproject()`, `depth()`, `viewMatrix()`, `projectionMatrix()` | `cameraEyePosition()`, `cameraViewDirection()`, `viewMatrix()`, `projectionMatrix()` |
| **Key difference** | No orbit center — just eye + direction | Has an explicit 3D orbit center |

**Conversion:**

- **Trackball → Shot** (`cameraShotForViewport()`): eye position and view direction from the trackball are used to derive a VCG shot. The center is lost. `PixelSizeMm` is set to `(1,1)` — a fictitious calibration, but self-consistent because `FocalMm` is computed from the same unit system. The ratio `FocalMm / PixelSizeMm` determines the angular resolution, which is correctly derived from FOV and viewport size.

- **Shot → Trackball**: underdetermined. The shot stores only the eye and direction; you must supply either an orbit center point or a distance along the view direction. No single conversion exists — `PeerViewCamera` therefore carries the actual `QMatrix4x4` view/projection matrices alongside near/far distances rather than just a `CameraShot`.

## `ParametrizationUV` Frame Sequence

Current status: UV mode is still a separate renderer in `renderwidget_uv.cpp`. It shares the document, per-mesh render settings, color-map/range controls, and some mesh GPU products, but it does not yet consume the Scene3D `RenderFrameRequest`/`RenderFramePlan` path. The intended next architecture step is UV convergence: reuse the Scene3D material/fill lighting path and substitute UV-space geometry/projection so lighting can remain the original 3D lighting reprojected into texture space.

1. Sync per-mesh mode state and UV cache against current document.
2. Ensure UV resources (`m_uvMeshGpu`); fit UV view if requested.
3. Draw UV background (`uv_background.vert/.frag`).
4. If `uvShowFullTexture`: draw the active group's selected PBR channel over `[0,1]²` (`uvTextureNearestSampling` switches bilinear → nearest).
5. Draw only the active texture group's triangles and their UV decorations: fill · wire · edges · boundary edges · texture seams · points · selection overlay.
6. If `uvShowReferenceFrame`: draw unit square outline + colored U/V axes from origin.

When the mesh has multiple texture/material groups, a bottom-centered viewport strip shows one thumbnail per group. Clicking a thumbnail changes view-local, per-mesh state; it does not modify the document or undo history. Each UV buffer remains a single GPU allocation, with compact per-group byte ranges selecting what is drawn.

UV fill: color source comes from `fillPlain.colorSource`. Quality UV buffers use the same fixed-range, center-on-zero, and percentile-crop normalization controls as Scene3D quality rendering. In `Texture` mode, `uvTextureChannel` selects base color, normal, occlusion, or roughness for the active group. Non-texture paths force `fillMaterial = Plain`. Scene corner/dimension overlays are hidden in UV mode.

## `ParametrizationUV` Camera and Interaction

Orthographic projection; `m_uvPan` + `m_uvZoom`. Left/middle drag = pan; wheel = zoom around cursor; double click = fit to mesh UV bounds (or `[0,1]²` when `uvShowFullTexture`); `Reset Camera` = UV fit. When `Rubber-band Select` is active, the same view coordinates are converted to UV-space filter parameters so rectangle selection can operate directly in the parametrization view.

Undo/redo restores trackball/render-style `ViewState`; UV pan/zoom and per-view visibility remain local runtime state.

## `RasterImage` Frame Sequence

Current status: raster mode reuses the Scene3D request/plan/pass executors for mesh rendering. The active raster image is drawn as a background reference fitted to the widget while preserving its image aspect ratio. If the raster has a valid `CameraShot`, `CameraShot::viewMatrix()` and `CameraShot::projectionMatrix(...)` provide the frame matrices and visible meshes are rendered through that camera. The same raster pan/zoom transform is applied to both the background image and the raster-camera projection, so image navigation keeps the mesh aligned with the raster. If no camera is available, raster mode behaves as an image-only view.

Raster mode interaction mirrors UV mode: left or middle drag pans the raster, wheel zooms around the cursor, and double click zooms in around the clicked image position. Raster mode starts with `75%` opacity. `Ctrl+Wheel` adjusts raster opacity. Trackball motion, depth picking, current-mesh outline, and camera synchronization remain disabled so the raster camera stays locked.

## Raster Projection Filters

`filter_color_projection` performs CPU-side projection from calibrated raster layers into mesh color/texture data:

- `compute_color_from_current_raster_projection`: projects the current raster onto current mesh vertex colors, optionally restricted by a software depth buffer and/or vertex selection.
- `compute_color_from_active_rasters_projection`: blends all visible rasters with valid `CameraShot`s into vertex colors using optional angle, distance, image-border, depth-discontinuity, and alpha weights.
- `compute_color_and_texture_from_active_rasters_projection`: projects visible rasters into a generated texture image over existing wedge UVs, with optional pull-push gap filling to reduce mipmapping seams.

These filters use `CameraShot::project(...)`/`depth(...)` and the mesh transform, not the live GPU scene pass. Their outputs are ordinary mesh data changes: vertex-color filters mark `VC`, while the texture bake marks wedge texture output and updates material/texture metadata through the normal filter/document path.

## Texture Normal-Map Workflow

PBR rendering can consume normal maps directly as either tangent-space or object-space textures through the render overlay's `Normal space` control. The Texture filter `convert_object_space_normal_map_to_tangent_space` converts an associated object-space normal map into a tangent-space image for the selected UV/material slot. It samples only faces using that slot, supports axis inversion and output-to-existing-or-new texture targets, and can bind the generated image as the selected material slot's PBR normal texture.

## Quality Histogram Overlay

2D overlay label inside `RenderWidget`. Controlled by `showQualityHistogram` and `qualityHistogramSource` (auto / forced vertex / forced face). Configurable bin count, optional fixed range (`qualityHistogramFixedRange`/`Min`/`Max`), center-on-zero mode, percentile crop (default `0.01` for automatic ranges), selectable colormap (`qualityHistogramColorMapId`), colormap inversion, and optional isolines (`qualityIsolinesEnabled`, `qualityIsolineCount`). Color mapping is shared with quality-based rendering so histogram colors and rendered quality colors stay aligned.

## Snapshot Capture

`MainWindow::saveSnapshotPng()`: set fixed color-buffer size → request update → wait for `frameRendered` → `grabFramebuffer` → restore previous size. Saved PNG embeds camera JSON in `MeshLab.CameraTrackballState` metadata.

Snapshot-to-raster paths reuse the same view capture mechanics but add the resulting image to `Document` through `addRasterImage(...)` with a `CameraShot` from `RenderWidget::cameraShotForViewport(...)` or from `renderSnapshotFromStateJson(...)`. This is how manual snapshot rasters and the `Render from Render-State JSON` layer filter create raster layers.

Programmatic snapshots use the same render-state JSON contract. Embedded `mlgui.render_snapshot(...)` and `mlgui.save_snapshot(...)` render through the live active `RenderWidget`; standalone `pymeshlab.MeshSet.render_snapshot(...)` uses `HeadlessRenderContext`, which owns a hidden `RenderWidget`/QRhi lifecycle for offscreen and batch rendering.

## Frame Timing

`RenderWidget` emits per-frame CPU time (`QElapsedTimer`) and GPU time when `QRhi::Timestamps` is supported (`lastCompletedGpuTime`). `MainWindow` displays a rolling 100-frame average in the status bar.
