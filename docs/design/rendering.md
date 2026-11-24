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
- `trackball`: same trackball payload used by camera JSON (`center`, `rotation_xyzw`, `distance`, `radius`, `fov_y_degrees`, near-clip and gizmo fields, and `projection`: `"orthographic"`, omitted for the default `"perspective"`; see [Orthographic Camera](proposals/orthographic_camera.md))
- `render_settings`: sparse `GlobalRenderSettings` payload (background, histogram settings, UV options, current-mesh highlight settings, etc.; default-valued fields are omitted)
- `mesh_visibility`: per-view visibility bool array, emitted only when at least one mesh is hidden in the view
- `mesh_render_modes`: array of `{ mesh_id, settings }`, where `settings` is a sparse `PerMeshRenderSettings` payload
- `current_mesh_index`, `current_raster_index`, `current_layer_kind`
- `viewport_px`: informational viewport size metadata

Design notes:

- Mesh render modes are keyed by persistent `mesh_id`, not by row index.
- The JSON is intentionally request-level and compact; GPU resources and concrete draw plans remain internal.
- Export omits default-valued settings. During import, omitted settings/mode maps start from defaults, while omitted `view_mode` and `raster_opacity` keep the current view values; for that reason `raster_opacity` is always written.
- This contract is forward-versioned (`version`) and can be extended without changing filter call sites.

Filter integration:

- `MainWindow` wires `Document::setRenderStateSnapshotFunction(...)` to the active view. `Document::renderSnapshotFromStateJson(...)` applies JSON to that view, renders an offscreen image at the requested size, returns both the `QImage` and resulting `CameraShot`, then restores the previous render state. This keeps filter code document-centric while all GPU and per-view state stays inside `RenderWidget`.
- Layer menu filter `Render from Render-State JSON` (`id: render_from_render_state_json`) consumes typed `camera_state` (`MeshLab.CameraState`) and `render_state` (`MeshLab.RenderState`) payloads and runs an offscreen render.
- Both parameters support the same UI source modes: inline text, JSON file, or capture from current view -- the current 3D view, the one a filter's camera result goes back to.
- The MeshLab.CameraState document is written and read in one place, `ViewTrackball::cameraStateJson` and `stateFromCameraStateJson` (the view, the snapshot metadata, the camera filters and `select_by_rectangle` share them). Besides the trackball it records `viewport_px`, the size of the image the camera is for: the trackball fixes the height it sees and only this says how wide the image is. A snapshot records its own image's size; the view, its scene tile's.
- The `Document/Camera` framing filters return a camera that the GUI moves the current view to (`MeshFilterRunResult::cameraState`, animated through `RenderWidget::animateCameraTo`) and, as `cameraState` in `outputValues`, to a script. *Set Camera to Frame from Viewpoint* is Frame Scene from Here as a filter: it keeps the viewpoint of a camera-state parameter (the current view by default) and fits the visible layers, the current layer or its selection, for the camera's own `viewport_px` or for an image of the target's own shape, whose aspect it returns. *Set Camera to View Selection* stands in front of the selection, facing it; it and *Set Camera from Direction* frame a sphere around their target by its outline (`radius / sin(fov / 2)`), and give the current raster the camera for that raster's own image size, through `setRasterShot`.
- The filter can write a PNG snapshot and/or inject the snapshot as a raster layer, enabling reproducible scriptable rendering workflows.

## Scene3D Request and Plan Pipeline

Scene3D rendering is split into a lightweight request layer and a concrete GPU draw-plan layer.

`RenderFramePassRequests` is collected once per frame from visible meshes, visible/current rasters, and resolved `PerMeshRenderSettings`. It contains one `RenderMeshPassRequests` per visible mesh, raster backplate/projected/frustum requests when relevant, plus frame-wide aggregate flags for fill, wire, edges, bbox, points, selection, decorator normals, decorator boundary/seam/non-manifold resources, and raster drawing.

The same request object is used for two things:

1. `prepareDirtyBuffers(...)` asks the cache (through `RenderWidget::ensureMeshGpu`) only for the cache products needed by the requested passes, plus extra resources needed by current-mesh highlighting.
2. `buildRenderFramePlan(...)` converts the request into concrete draw items after resources/pipelines are available.

`RenderFrameRequest` adds frame-local state to the pass requests: view mode, pixel size, projection matrix, view matrix, light direction, and raster overlay state (opacity plus raster pan/zoom in `RasterImage` mode). `RenderFramePlan` is the concrete in-process result: fill items, buffer items, decorator items, selection items, and raster draw items with QRhi buffers/pipelines and material renderer pointers. Pass presence is derived from non-empty draw-item lists (`hasFillPass()`, `hasSceneDrawItems()`, etc.).

This is an internal architecture boundary, not a public serialization contract. The implemented programmatic path serializes camera/render-state intent and lets MeshLab build the GPU `RenderFramePlan` internally.

## Shared GPU Cache (`MeshGpuResourceCache`)

Cache key: `(QRhi*, meshId, variant, geometryRevision, materialRevision, selectionRevision where relevant)`. The quality variants hold the raw scalar, so no range setting is part of their key. Wire resources also track whether faux polygon edges should be respected.

Entries are released when their layer leaves the document (`removeMesh`, `clearAllLayers`, or an undo that drops the layer: `purgeMesh`) and, for one QRhi, when a view's QRhi is replaced (`releaseRhiResources`). A revision change releases the fill and point variants left at the older revision as soon as a frame prepares that mesh; a variant still at the current revision stays, since another view may draw it. The other products' stale buffers are replaced the next time a frame asks for them.

Cached outputs:

- **fill**: one or more batches — vertex/index buffers, optional base/normal/occlusion/roughness textures and per-batch PBR factors. Variants: `Constant`, `PerVertex`, `PerFace`, `PerVertexQuality`, `PerFaceQuality`, `Texture`. Texture lookup can use legacy texture paths, `MeshIOTextureAsset` entries, and material slots. PBR normal textures can be interpreted as tangent-space maps or object-space maps according to `fillPbr.normalMapSpace`.
- **wire**: barycentric-expanded triangle buffers, optionally honoring faux-edge bits for polygonal faces. Split into batches of at most 256 MB like the expanded fill: a QRhi buffer size is a `quint32`, and at 72 bytes per face one buffer wrapped past 60M faces and drew only the first faces of the mesh.
- **edges**: line buffer + fat-line buffer from explicit mesh edges. Each expanded vertex stores packed endpoint RGBA and packed edge RGBA; the shader selects constant, interpolated per-vertex, or flat per-edge color without rebuilding geometry.
- **points**: position/color/normal payload + normal-valid flag. Variants: `Constant`, `PerVertex`, `PerVertexQuality`.
- **bbox**: line buffer.
- **selection**: selected-face triangles, selected-vertex points, and selected edges both as plain segments and as fat-line quads (`LineRenderer::buildFatLineVertices`), keyed on `selectionRevision` so selection overlays rebuild without invalidating fill/wire/point resources.
- **decorators**: vertex normals, face normals, boundary edges (line + fat-line), texture seams (line + fat-line), non-manifold edges (line + fat-line), non-manifold vertices, and curvature principal-direction lines.

`Document` owns the cache, so a layer's buffers go with the layer, and hands it to the views as `gpuCache()`; a view builds what its frame needs through `RenderWidget::ensureMeshGpu` (the products as one `MeshGpuResourceCache::Needs`) and reads the buffers back with the cache's `*PassView(rhi, meshId, ...)` calls.

`MeshGpuResourceCache::gpuMemoryStats()` sums the QRhi buffer sizes and known RGBA8
texture payloads for all cache variants and RHIs, including non-manifold decorator
buffers. This is a **logical mesh-cache size**, not total GPU or process memory: per-view
UV/raster/offscreen resources, backend alignment, staging/command storage, and driver
allocations are outside it. The Memory Info dialog therefore presents it separately from
tracked CPU allocations and OS physical footprint.

Fill uses an indexed path (shared vertices) or an expanded-triangle path for per-face colors or texture batching. For quality variants the buffer holds the raw scalar in the color's red slot, flagged in its alpha: -1 per-vertex, -2 per-face, or 0 (the plain fill color) for a value that is not finite, so missing data shows as a gap rather than as an average value. The shaders map it to a color map coordinate `t = clamp(q * scale + offset)` through the per-mesh `qualityMap` uniform (xy for per-vertex values, zw for per-face ones, `kUbufQualityMapOffset`), filled by `RenderWidget::qualityMapping`, and then sample the per-view LUT. So nothing in the quality settings rebuilds a buffer: the color map, inversion and isolines update the LUT texture, and fixed range, center-on-zero and percentile crop update the uniform. The range itself comes from `RenderWidget::qualityRange`, the fixed one when set and finite, otherwise `sampledQualityRange`, cached per mesh content and sampling settings. The histogram, Bake to Vertex Color, the fixed-range seeding and the UV view read it there too, so they cannot disagree. `tests/render_probes/scalar_color_map` checks the mapping, both halves of the uniform, and the gap.

Boundary extraction: topological edge incidence (`incidentCount == 1`). Non-manifold edge extraction: topological edge incidence above two. Non-manifold vertex extraction: vertices whose faces form more than one fan -- two cones touching at the tip -- and that lie on no non-manifold edge, which that decorator already shows; the definition of vcglib's `CountNonManifoldVertexFF`, which the info panel counts with. The cache holds a const mesh without FF adjacency, so it finds the fans from the same edge map, joining the corners at each end of every manifold edge. Seam extraction: per-topological-edge UV sample comparison (texture-index changes, missing/invalid UV). Colors: green boundaries, blue seams, and one red-purple for both non-manifold decorators.

## Per-Mesh Render Modes

`RenderWidget` holds a `PerMeshRenderSettings` per mesh id (`m_meshRenderModes`; `MeshRenderMode` is a local alias). View-level settings are in `m_renderSettings` (`GlobalRenderSettings`).

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
- quality histogram/range: `showQualityHistogram`, `qualityHistogramBins`, `qualityHistogramSource`, `qualityHistogramFixedRange`, `qualityHistogramMin`/`Max`, `qualityHistogramCenterOnZero`, `qualityHistogramPercentileCrop` (default `0.01`, cropping both tails of automatic ranges so a few outliers do not flatten the color map), `qualityHistogramColorMapId`, `qualityHistogramInvertColorMap`, `qualityIsolinesEnabled`, `qualityIsolineCount`
- overlay panel state: `settingsPanelVisible`, `currentPass`

`using RenderSettings = GlobalRenderSettings` and `using MeshRenderMode = PerMeshRenderSettings` (widget-local alias) are provided.

Default mode for a new mesh, read from the preferences as each mesh arrives:
- surfaces (`FN > 0`): fill on; wire on below `render.wireframeOnLoadMaxFaces` faces, and flat shading below `render.flatShadingMaxFaces`
- edge-only meshes: edges on, `render.edgeLayerWidth` wide
- point-only: points on

Point size, edge width and wireframe width start from `render.pointSize`, `render.edgeSize` and `render.wireSize`.

Default fill color source preference: texture → per-vertex → per-face → per-vertex-quality → per-face-quality → constant, clamped to mesh `ioMask` + texture availability. Texture availability is based on `Document::meshTextureAssociationCount(...)`.

Default explicit-edge color source preference: per-edge → per-vertex → constant. Availability is clamped to `IOM_EDGECOLOR` and `IOM_VERTCOLOR`; unavailable choices are disabled in the overlay panel. Edge-only PLY layers carrying edge colors therefore open with `PerEdge` selected automatically.

## Radiance scaling

Radiance scaling (Vergne, Pacanowski, Barla, Granier, *Radiance Scaling for Versatile Surface Enhancement*,
I3D 2010) is an **option of the plain fill material** (`fillPlain.radianceScaling`, with `rsEnhancement`
and `rsResolutionBias`), not a material of its own. A render state from when it was one (`fill_material: 2`, or a
`fill_rs` block) is refused by `parsePerMeshSettings`, with a message saying where it went.

**What it does.** The lighting scalar before the albedo -- `shade = ambient + diffuse`, in [0, 1] -- is replaced by
`shade * sigma(kappa, shade)`, and the colour is `albedo * shade + specular`. `sigma` is the paper's scaling function
(eq. 2) with the scaling-invariant point alpha = 0.1 and magnitude 1: it leaves `shade = alpha` alone, brightens
the brighter values on convexities and darkens them on concavities, and the other way round below alpha. The
colour source is out of it, so a texture, per-vertex colours or a scalar map keep their hues, and the
specular term is added unscaled. With the lighting off `shade` is one, which is the curvature shading the paper
notes. PBR does not have the option.

**Where the curvature comes from.** A **pre-pass** (`RenderWidget::renderRadianceScalingPrepass`,
`fill_radscale_buf.frag` for surfaces and `points_radscale_buf.frag` for points)
draws every filled layer and every layer's points into `m_rsGradTexture` (`RGBA32F`): `(gx, gy, logZ, 1)` from the
interpolated view-space normal and the log of the view depth, using the normals each layer is shaded with: the interpolated vertex normal
for a smooth layer, the face's own for a flat one (the pass is told which through the occlusion-strength slot of the
uniform block). For a flat layer the curvature is therefore zero inside a face and high across the edges, where the
convex ones come out as bright lines, as in the original MeshLab; from the smooth normals the curvature would spread
over every face and whiten it towards the silhouette. It draws all layers, not only those with the
option on, so that an occluder's silhouette is a depth step. The fill shaders (`fill_smooth.frag`,
`fill_flat.frag`) and the points shader (`overlay_points.frag`) read it back, all through one shared block,
`shaders/radiance_scaling.glsl` (pulled in with `#include`, through `GL_GOOGLE_include_directive`; `qt_add_shaders`
tracks it as a dependency of each), by absolute framebuffer position -- bound in the slot the
normal map would use, which plain does not -- takes the Hessian of the gradient over a 3x3 neighbourhood,
`tanh`s it with the gain, and fades it to nothing at depth discontinuities. The pre-pass runs only when some
layer asks, and a layer's draw drops the option for the frame if the buffer could not be made.

**Points.** The option is the same switch for a layer's points: there is no separate setting, and the Points settings panel carries the same three controls as the Fill one (`RenderOverlayPanel::buildRadianceScalingRows`, kept in step by `syncRadianceScalingRows`). The points take it whatever the layer's fill material is (the frame plan gives their draw items the plain material), so with a PBR fill the points panel is where it is controlled. A point with a normal
writes it (made two-sided, as the lighting of the points is) into the buffer as a round dot, and a point without one
writes a flat patch facing the viewer, which occludes but has no curvature; `overlay_points.frag` scales its lighting
scalar (the ambient `0.75` plus the diffuse part, or one when the points' lighting is off) by the curvature at the
point, and leaves a point without a normal alone. The points draw with the fill-texture bindings so that the buffer sits in
the normal-map slot, as for the fill; a point cloud with no fill still gets the pre-pass. The texture-coordinate
view does not draw the buffer and so draws its points without the option.

**The curvature is measured in pixels, on purpose.** It makes the enhancement crisper the smaller the object is on
screen, which is a feature of the technique; there is no world-space normalization. The strength therefore also
depends on the zoom and on the size of the render target, a snapshot of another size included.

`rsResolutionBias` (JSON `rs_resolution_bias`, *Resolution bias* next to the other radiance-scaling controls) lets the
user trade some of that away. The Hessian of a gradient buffer measured per pixel shrinks in proportion to the view's
height, so a snapshot twice as big reads half as curved. The bias multiplies the gain by
`(viewHeight / 1000) ^ bias` (`radianceScalingGain`, on the CPU, against the height of the tile being drawn): 0, the
default, leaves the pixel-space behaviour as it is, 1 makes the strength about the same at any resolution, and
between is a blend. The reference of 1000 pixels is where the bias changes nothing. With the bias at 1 the strength
at 240 and at 720 pixels is much closer than at 0, though not equal: the signal goes through `tanh` and the display
clamps.

**Bindings.** The pass binds the main uniform buffer through its own shader-resource bindings, with a dynamic
offset like every other binding of that buffer. The buffer is replaced when it has to grow (its capacity follows the
layer count), so the replacement drops the pass's pipeline and bindings, and `ensureRsGradResources`, which tears
everything down when the size is unset, rebuilds them; a binding left behind would hold a pointer to the old
buffer.

Tested by the render probe `tests/render_probes/radiance_scaling` (the option changes the lighting, scales
nothing at zero gain, keeps the colour proportions of vertex colours, works with flat shading and with point
clouds, survives the buffer being replaced under 81 layers, evens out with the resolution bias, and the refusal) and
by `RenderSettingsJsonTests`.

## Back lighting

`PerMeshRenderSettings::fillBackLighting` (`Single`, `Double`, `Fancy`; JSON `fill_back_lighting`; *Back lighting*
in the Fill settings, for both materials) is how the old MeshLab's Single / Double / Fancy back-face modes come over.
They were two fixed-function lights, the light and one opposite to it; here the fill shaders do the same with the
diffuse term, through `shaders/two_sided_lighting.glsl` (`sidedDiffuse`, shared by `fill_smooth.frag` and
`fill_flat.frag`):

- **Single** -- `max(N.L, 0)`: a surface turned away from the light gets the ambient and nothing else.
- **Double** -- `abs(N.L)`: the opposite light is the same as the first, so a surface is lit from whichever side it faces.
- **Fancy** -- as Double, with the diffuse part tinted bluish (`0.8, 0.8, 1.0`) where the light reaches the surface and
  pinkish (`1.0, 0.8, 0.8`) where only the opposite one does; the old defaults for its two diffuse colours. Together with
  the lighting of the back it makes the side of a surface readable from its colour.

"Back" is relative to the **light**, not the viewer, as it was in the old MeshLab, so the tints follow the light when it
is moved; the ambient stays white and the specular term is left as it was. It reaches the shader in
`lightingParams.w`, which is 0 with the fill lighting off and `1 + mode` otherwise. The tints are constants of the
shader (the old MeshLab had them as application settings). Backface *culling* is a separate switch.
Tested by the render probe `tests/render_probes/back_lighting`.

## Reference grid

`GlobalRenderSettings::referenceGrid` (`None`, `XY`, `XZ`; JSON `reference_grid`; *Reference grid* in the
View Settings panel) draws a plane through the world origin, ruled in powers of ten, with the two
world axes in it drawn red (X), green (Y) and blue (Z): a reference for where the origin is and how big
things are. It is a different thing from the layer grid (`LayerArrangement::Grid`), the tiling of layers.

**One quad, ruled by its fragment shader** (`shaders/grid.frag`; one draw in `render()`, after the
fills and the point/edge/box items and before the decorators). The quad is `halfSize` wide about the
point of the plane nearest the orbit centre -- so it follows a pan -- and the shader fades the grid out
over its outer part, so no edge is seen. It is depth-tested against the scene and writes no depth.
Lines are one pixel wide whatever the zoom (from the screen-space derivative of the cell coordinate)
and the axes about two.

**Three decades are drawn at once**, each ten times the last (`viewGridLevels`): from the world size of
a pixel at the orbit centre, the fine one is between 1 and 10 pixels apart, the middle one 10 to 100 and
the coarse one 100 to 1000. The middle is always drawn in full; the fine fades out as it gets too dense
to read and the coarse fades in as it becomes sparse enough to, so a decade never pops in or out when
zooming (the weights are one fraction and its complement, and line up across a decade). Each level also
fades by how many of its cells fall in a pixel, which is what lets a plane seen at a grazing angle go
quietly to nothing in the distance instead of to noise.

**Which plane** (`chooseViewGridPlane`): the setting names XY (a scene standing on Z) or XZ (on Y). An
*orthographic* view looking straight down a coordinate axis (within about one degree) -- front, side, top,
which the axis gizmo and the numeric views give -- always gets the plane *across* its line of sight
whatever the setting, so the grid is never seen edge-on and an orthographic view with the grid on always
has something to measure with. An orthographic view off the axes uses the chosen plane.

**Depth.** An orthographic view fits its depth range to the scene, and the plane through the origin may be
nowhere near it, so `sceneProjection` widens the volume to hold the plane (both ends, as for the peer-view
glyphs). A perspective view lets the far reaches of the grid be clipped by its far plane; they have faded
by then. The setting is part of the render-state JSON, so snapshots and scripted renders draw it too.

**Preferences.** `view.referenceGrid` is the grid a new view starts with (read when a view is created and by
File > New). `view.grid.*` set how it looks; they are read every frame (`viewGridStyle()`), so a change in the
dialog repaints the views.

Tests: `ViewGridTests` (plane choice and level arithmetic) and the render probe
`tests/render_probes/reference_grid` (the axis colours, the orthographic rule, and the grid far from the
scene).

## Clip-space convention

Every projection the application builds -- the trackball's, a raster's, the 2D UV view's -- is in
OpenGL convention: clip depth in [-1, 1], Y up. The backend may want otherwise (Metal and
Direct3D keep depth in [0, 1], Vulkan has Y down), and that is Qt's to describe, not ours:
`QRhi::clipSpaceCorrMatrix()` is the matrix that converts. `RenderWidget::forBackend()` applies
it, and it is applied where a matrix is written into a GPU buffer -- never to the matrices the CPU
keeps. The depth pick therefore unprojects in OpenGL convention too (`ndcZ = depth * 2 - 1`, Y up),
and nothing outside `forBackend` asks which backend is running. A new pass that uploads a
view-projection must go through it; `tests/render_probes/clip_space` fails if a pass forgets, as a
near plane pushed in front of an object cuts the object on Metal.

## `Scene3D` Frame Sequence

1. Advance animation/camera state, sync per-mesh render modes, and update the camera frame when needed.
2. Collect `RenderFramePassRequests` once for visible meshes and raster-layer requests.
3. Prepare shared mesh GPU resources from those requests.
4. Upload per-frame textures/UBOs such as the quality LUT, gizmo buffers, and background gradient.
5. Execute depth pick (if scheduled).
6. Build current-mesh highlight masks (if `highlightCurrentMesh`).
7. Build a concrete `RenderFramePlan` from a `RenderFrameRequest`.
8. Run the radiance-scaling gradient pre-pass (if any planned fill or point item has the radiance-scaling option on).
9. Run main onscreen pass.

Main pass draw order: scene background · fill · solid cut · wire · edges · bbox · points · reference grid · raster camera frustums · decorators · depth-cued tool guides · raster image (`RasterImage` mode only: an overlay with no depth test, faded by the raster opacity) · trackball gizmo · current-mesh outline/debug composite · selection overlay · tool loupes · 2D informational/tool overlays.

## `Scene3D` Pass Details

**Scene background**: full-screen gradient triangle, `sceneBackgroundBottomColor`/`TopColor`, drawn first.

**Fill**: Fill planning and execution are isolated in `renderwidget_fill.cpp`. `SceneFillFramePlan` contains fill draw items, each with a material renderer selected from `PlainFillRenderer` or `PbrFillRenderer`. Shared behavior such as UBO upload, texture SRB resolution, selected PBR texture lookup, and radiance-scaling gradient resource access goes through `FillRenderServices`.

Smooth/Flat shading use distinct shader pairs. Depth test+write on; `fillBackfaceCulling` controls culling. Quality variants LUT-sample from the per-view colormap texture, including optional isoline stripes. Quality normalization happens in the shader, through the `qualityMap` uniform. PBR binds base/normal/occlusion/roughness per batch. Tangent-space normal maps derive TBN from screen-space derivatives of view position and UV; object-space normal maps are transformed by the normal matrix and blended with the base normal using `normalScale`. Radiance scaling has [its own section](#radiance-scaling).

**Wireframe**: barycentric triangles + fragment edge test; depth `LessOrEqual`, no depth write; alpha blending; `wireBackfaceCulling`; optional `wireRespectFaux` controls faux polygon edge handling in the cached wire data.

**Edges**: fat-edge triangles when available, line fallback; depth `LessOrEqual`; alpha blending; width from `edgeSize`. `edgeColorSource` chooses `edgeColor` (`Constant`), interpolated endpoint vertex colors (`PerVertex`), or one flat `VCGEdge` color (`PerEdge`). This pass renders explicit `VCGMesh::edge` elements and is independent of triangle **Wireframe** and boundary/seam/non-manifold decorators.

**Bounding box**: line topology; depth on, no depth write.

**Points**: `QRhiGraphicsPipeline::Points`; depth test+write; point lighting, size, and color source from settings; quality variant LUT-sampled. The size is `pointSize` pixels, or, with `pointSizeObjectSpace`, a diameter of `pointObjectSize` in the mesh's own units: `pointParams.x` carries the pixel size as a positive number and the object size as a negative one (`encodedPointSize`). The vertex shader projects an offset of that length along the view's vertical, carried back into mesh space, so the dot is as tall on screen as the surface it stands for, under a perspective or an orthographic camera, and holds it between one pixel and `kMaxObjectPointPixels` (`overlay_points.vert`). `depth_pick.vert` repeats the computation, so a pick hits the dot that was drawn. The first time a layer switches to object space with no size set, the view fills in the spacing its points would have spread evenly over its bounding box (diagonal / sqrt(vertex count), to two significant digits).

**Decorators**: depth `LessOrEqual`, no depth write. Normals and curvature directions use the line pipeline. Boundary, seams, and non-manifold edges use the fat-decorator pipeline (`decoratorBoundaryWidth`) with line fallback. Non-manifold vertices use a point pipeline.

**Selection overlay** (final pass): semi-transparent red fill triangles + red vertex points; depth `LessOrEqual`, no depth write; per-mesh `showSelection`/`showSelectionFaces`/`showSelectionVertices`/`showSelectionEdges`, all in `RenderWidget::kSelectionColor`. Selected edges are drawn as quads `render.selectedEdgeWidth` pixels wide, through the fat-line decorator pipeline: `planSelectionPasses` puts them on the decorator list, so they draw in the decorators' slot of the order above. A line primitive is one device pixel wide on Metal whatever width is asked for, so the plain-line selection pipeline only stands in when the fat one cannot be drawn. Selection resources come from the shared mesh GPU cache, in UV mode those of the UV layout (see [`ParametrizationUV` Frame Sequence](#parametrizationuv-frame-sequence)).

**Decorator info overlay**: 2D label controlled by `showDecoratorInfo`, on by default. It reports numeric counts for enabled decorator data on the current mesh (boundary/seam/non-manifold families) while the boundary decorator is on, and hides itself otherwise or when no relevant decorator information is available. All four boundary-page decorators -- boundary edges, texture seams, non-manifold edges and vertices -- default on, so turning the pass on shows everything it can.

Simple buffer pass execution (wire, edges, bbox, points), decorator execution, and selection execution are isolated in `renderwidget_scene_passes.cpp`. Their draw order remains controlled by `renderwidget_render.cpp`.

**Rasters**: `renderwidget_raster.cpp` owns per-raster texture upload and draw execution. In `Scene3D`, rasters with valid camera shots draw as small line frustums with the apex at the raster camera origin and the base oriented by the camera view frustum (an orthographic shot draws as a box instead: no apex, four parallel edges between two equal rectangles, as does a peer view's camera when that view is orthographic); the current raster frustum is highlighted in warm yellow while other raster frustums use blue. Rasters without cameras are not drawn in the 3D view. The raster image itself is not pasted into the 3D scene. In `RasterImage`, only the active raster layer is requested as the full-viewport reference, drawn over the meshes, and mesh passes are included only when that raster has a valid camera.

## Current Mesh Highlight

Runs when `highlightCurrentMesh` is on, the document holds more than one layer, and the layers are overlaid rather than arranged in a grid: with a single layer the outline marks nothing, and in the grid every layer already has a tile of its own. Every outline pass asks `RenderWidget::wantsCurrentMeshOutline()`, so the rule lives in one place.

**Surface/edge path**: render current mesh depth mask → extract silhouette into `work` → render all other meshes into `mask` for occlusion → composite outline. Occluded outline portions use half alpha.

**Point-cloud path**: render occupancy mask → dilate (`base → work`) → erode (`work → mask`) → composite outline from final mask.

Debug views: `FullMask`, `VisibleMask`, `OccludedMask`, `DilatedMask`, `ErodedMask`. Normal path is `Outline`.

## Clipping Plane

A world-space plane that cuts away everything on its negative side, in every `Scene3D` pass
including depth picking. Off by default; `GlobalRenderSettings` holds it, so it is per view,
inherited by a split, and carried in a render state.

`ClipPlane::world()` ([clipplane.h](../../src/render/clipplane.h)) resolves
`clipPlaneAxis` (`X`/`Y`/`Z`/`View`/`Custom`), `clipPlaneRelativeTo` (`Origin`/`Center`/
`Min`/`Max`), `clipPlaneOffset` and `clipPlaneFlipped` against the visible scene's bounding
box into a `vec4` — xyz normal, w offset, a point surviving when `dot(vec4(p, 1), plane)`
is at least 0. All-zero means keep everything, which is what every pass writes when clipping
is off, so no shader branches and no pass has a second pipeline. The offset is a fraction of
the scene's diagonal, so one range fits every model and a saved state survives a rescale.
`View` resolves from the camera each frame; the others hold still while you orbit.

Vertex shaders see **local** positions, so the world plane is transformed per layer:
`ClipPlane::toLocal()` applies the transpose of the layer matrix, since a plane transforms
as a row vector. That makes the plane per-mesh data with the same cadence as `mvp`, which is
why it rides in the per-mesh uniform slice (`kUbufClipPlaneOffset`, and
`kDecoratorClipPlaneOffset` for the two decorator blocks) rather than in a buffer of its own.

Eleven vertex shaders write `gl_ClipDistance[0]`: the seven fill/wire/edge/point/bbox
shaders and `selection_mask.vert` on `m_srb`, `depth_pick.vert` on `m_depthPickSrb`, and
`overlay_decorator.vert` / `overlay_fat_decorator.vert` on the decorator and selection SRBs.
The three that already redeclare `gl_PerVertex` for `gl_PointSize` list the clip distance
there too, or `qsb` refuses the build. Gizmos, the background, raster passes and everything
in UV mode are deliberately not clipped.

> **This sets the OpenGL floor at 3.2 core.** `qt_add_shaders` passes `GLSL "150"` because
> Qt's default set also bakes GLSL 100 es and 120, and SPIRV-Cross emits a global `out
> float gl_ClipDistance[1]` for those profiles, which they reject. `qsb` exits 0 either way,
> so the breakage would only appear when the GL backend compiled the shader at run time.
> Metal, Vulkan and D3D are unaffected.

> **OpenGL links a pipeline's two stages into one program, and checks what they share.**
> The GLSL that Qt bakes for its OpenGL backend has no uniform blocks: the block a stage
> declares as `ub` becomes a plain struct uniform, so **every stage of a pipeline must declare
> its blocks with the same members** -- a fragment shader that stops at the members it reads
> declares a different struct under the same name, and the driver refuses the link ("Uniform
> type mismatch 'ub'"). And GLSL 150 matches what the vertex shader passes on to the fragment
> shader **by name**, not by location: a fragment input that no vertex output of that name and
> type writes fails the link too. Metal, Vulkan and D3D check neither, which is how the
> fragment blocks left behind when the clipping plane was added to their vertex shaders took
> points, wireframe, the bounding box and the decorators off the screen on Linux only, with
> fill untouched (its vertex block is anonymous, so the stages' uniforms have different
> names). `tests/test_shader_interfaces.cpp` reads the GLSL 150 the build baked for every
> vertex and fragment shader the renderer pairs (found by the order `src/render/*.cpp` loads
> them) and checks both rules, so the next one fails in ctest on any platform.
> `depth_pick.frag` takes the picked id from the block (`pointParams.y`) rather than from the
> vertex shader for this reason: it is paired with the fat edges' vertex shader too.
>
> To see the OpenGL path on another platform, set `MESHLAB2_GRAPHICS_API` (`opengl`, `vulkan`,
> `metal`, `d3d11`, `d3d12`): `RenderWidget` hands it to `QRhiWidget::setApi`, which is what
> picks the API -- `QT_WIDGETS_RHI_BACKEND` does not, under a `QRhiWidget`. Asked for OpenGL,
> it also requests a 3.3 core context, since a Mac's default 2.1 cannot run GLSL 150; a Mac
> then fails as a Linux driver does. `QT_LOGGING_RULES="qt.rhi.general=true"` says which
> context Qt created.

**Cut rim.** `fill_smooth.vert` and `fill_flat.vert` also pass the clip distance to the
fragment stage, where `fill_smooth.frag` and `fill_flat.frag` blend
`clipRim.rgb` into the sliver just above zero, `clipRim.a` pixels wide:
`smoothstep(width * fwidth(d), 0.0, d)`. `fwidth` is what keeps the band a constant number
of pixels whatever angle the surface meets the plane at. Width 0 disables it.

**Solid cut.** With `clipPlaneSolidCut` the cut is drawn as a face on the plane, found per
pixel rather than built as geometry, in two draws after all the fills
(`renderSceneSolidCut`):

1. *Count.* Every clipped layer whose fill is showing is drawn again into the stencil only —
   colour and depth off, both faces — decrementing on front faces and incrementing on back
   faces. The main pass clears stencil to `kSolidCutStencilBias` (128), so the count can go
   negative without wrapping into the range tested next.
2. *Cap.* `ClipPlane::capQuad` — the square the plane gizmo spans, which covers every point
   where the plane can meet the scene's box — drawn where the stored value exceeds the bias,
   depth-tested and depth-writing.

Seen from the discarded side, a ray through a point of the cut that lies inside a closed
solid leaves it once more than it enters, so the count is positive exactly on the cross-
section. A **winding** count rather than parity keeps nested and overlapping solids capped
(parity would cancel them), and on open surfaces it turns most failures into a missing cap
rather than a spurious sheet: a ray that leaves through a hole simply counts zero. That is
the remaining limit — on an open mesh, the cap is absent wherever a view ray leaves the
object through a hole. On by default, since the failure is a missing face and never a
wrong one.

The cap is planar and the light directional, so its diffuse shading is one value per frame;
the CPU computes it with the fill shaders' own model (ambient 0.18 plus Lambert) against the
cap's normal, which faces the discarded side. It is drawn through the raster-projected
shaders, which carry no clip distance: the cap lies exactly on the plane that would cut it.
Depth picking does not see it — a click on the cap picks the wall behind.

**Plane gizmo.** A bordered grid on the plane with a stem along the surviving side, built by
`ClipPlane::planeGizmo()` and drawn on the line-gizmo pipeline the peer-view cameras use
(`kClipPlaneGizmoRasterIndex`). It appears while the plane is moving and for 1200 ms after;
`clipPlaneShowPlane` keeps it up permanently.

How *Trim Surface by Plane* splits, snaps and closes the cut is in that filter's help. The clip-plane page's *Trim Current Layer* button and the mouse controls are in [User Interface](user_interface.md#clipping-plane-controls).

## Depth Picking

Double click schedules an offscreen depth-pick frame: depth encoded in RGB → one pixel read back → backend conventions normalized (Y flip, clip-depth range) → unprojected via inverse MVP → `trackballCenterPicked(worldPos)` emitted, animated recenter starts.

A tool's pick (`requestSurfacePick`) is the same pass. In the raster view it runs too, for a tool that supports that view (`supportsRasterView()`) and a raster with a camera: it projects through the matrices that frame drew with, the raster's camera behind the image's pan and zoom, so the point it returns is the surface under the cursor as the mesh is drawn over the photo. No other pick is scheduled there.

## Interactive Tools

Interactive tools are thin view-owned interaction front-ends over filter/document operations. A tool is active in at most one `RenderWidget`; it owns mouse/keyboard input while active, shows a cursor and an icon, can paint a 2D overlay, and can contribute depth-cued line segments to the scene pass. Durable edits commit through `Document::runFilter(...)` when possible so undo nodes and Python action history stay coherent. `Tab` suspends the tool and temporarily returns gestures to camera navigation; `Esc` exits the tool or cancels the active gesture.

The buttons and the keys come from one description: a tool lists its controls (`InteractiveTool::controls()`: id, toggle or action, label, help, keys, group) and answers `isControlChecked`, `isControlEnabled` and `triggerControl(id, modifiers)`. A tool measures the screen against `RenderWidget::sceneRect()` -- where the scene is drawn: the whole view, less the quality histogram's strip while it is shown -- and places world points with `projectToWidget`, which goes through the same camera (`projectionMatrix()` takes the scene's aspect, not the widget's); the selection's rectangle and lasso are normalized over that rect, and the measure labels, the endpoint hit test and the transform's pivot use it, so none of them is thrown off by the strip. `RenderWidget` runs a control from its button or from its key alike (`toolControlForKey`, then `triggerToolControl`); only what is left -- a gesture's own keys, such as typing a value or closing a lasso -- reaches `InteractiveTool::keyPress`. A key with `Ctrl`, `Alt` or `Cmd` held never reaches a control, so the menus keep their shortcuts, an auto-repeated press is swallowed, and a disabled control lets its key go on to the tool. The strip never takes the keyboard focus, keeps the presses on its background so that a click between two buttons does not reach the scene, and makes its view current when pressed. Its test (`tests/test_toolstrip.cpp`) drives it with a stand-in tool; `MESHLAB2_GRAB_DIR` saves what it looks like.

The tool strip and the built-in tools are described in [User Interface](user_interface.md#interactive-tools).

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

UV mode draws in `renderwidget_uv.cpp` from the shared mesh GPU cache. `RenderWidget::ensureUvLayout` lays the current layer's active texture group out in UV space: a `VCGMesh` with one vertex per vertex and UV pair at `(u, v, 0)`, facing +z, carrying the colors, scalars, selection, faux and selected-edge flags of the vertex and face it comes from, and the UVs as wedge texcoords. A change of selection alone copies the flags across instead of rebuilding it. `ensureMeshGpu(..., uvLayout = true)` hands it to the cache under its own id (high bit set, the group above the mesh id), with the layer's revisions, so every view drawing the same group shares the buffers; `syncUvCacheWithDocument` purges a removed layer's layout ids, which the cache does not know belong to it. `MeshSource::vertexIdentity` (`UvLayout::sourceVertex`) makes the boundary and seam pass key edges by the layer's vertex, so the two sides of a seam meet, and draw such an edge on each side; the rules are the 3D ones. The fill, wire (the 3D barycentric overlay, `wireRespectFaux` honored), points, boundaries, seams and selection all come from the cache's pass views, and scalars go through the same `qualityMap` uniform and LUT as in 3D. It does not yet consume the Scene3D `RenderFrameRequest`/`RenderFramePlan` path. The intended next architecture step is UV convergence: reuse the Scene3D material/fill lighting path and substitute UV-space geometry/projection so lighting can remain the original 3D lighting reprojected into texture space.

1. Sync per-mesh mode state and UV cache against current document.
2. Ensure the UV layout (`m_uvLayout`) and its cache products, and refresh the color map LUT, before the pass; fit UV view if requested.
3. Draw UV background (`uv_background.vert/.frag`).
4. If `uvShowFullTexture`: draw the active group's selected PBR channel over `[0,1]²` (`uvTextureNearestSampling` switches bilinear → nearest).
5. Draw only the active texture group's triangles and their UV decorations: fill · wire · edges · boundary edges · texture seams · points · selection overlay.
6. If `uvShowReferenceFrame`: draw unit square outline + colored U/V axes from origin.

When the mesh has multiple texture/material groups, a bottom-centered viewport strip shows one thumbnail per group. Clicking a thumbnail changes view-local, per-mesh state; it does not modify the document or undo history. Each group is a layout of its own, built the first time it is shown.

UV fill: color source comes from `fillPlain.colorSource`. Scalars are drawn as in Scene3D. `tests/render_probes/uv_view` checks the fill, a scalar, a selection change alone, the wire and the seams on a parametrized gargoyle. In `Texture` mode, `uvTextureChannel` selects base color, normal, occlusion, or roughness for the active group. Non-texture paths force `fillMaterial = Plain`. Scene corner/dimension overlays are hidden in UV mode.

## `RasterImage` Frame Sequence

Current status: raster mode reuses the Scene3D request/plan/pass executors for mesh rendering. The active raster image is drawn over the meshes as a reference, without a depth test, fitted to the widget while preserving its image aspect ratio. If the raster has a valid `CameraShot`, `CameraShot::viewMatrix()` and `CameraShot::projectionMatrix(...)` provide the frame matrices and visible meshes are rendered through that camera. The same raster pan/zoom transform is applied to both the background image and the raster-camera projection, so image navigation keeps the mesh aligned with the raster. If no camera is available, raster mode behaves as an image-only view.

Raster mode interaction mirrors UV mode: left or middle drag pans the raster, wheel zooms around the cursor, and double click zooms in around the clicked image position. Raster mode starts with `75%` opacity. `Ctrl+Wheel` adjusts raster opacity. Trackball motion, current-mesh outline, and camera synchronization remain disabled so the raster camera stays locked; depth picking runs only for a tool that supports the raster view (see [Depth Picking](#depth-picking)). `widgetToRasterImage` and `rasterImageToWidget` map between widget positions and the image, from (0, 0) at its top-left corner to (1, 1), as panned and zoomed.

A tool's loupes (`InteractiveTool::loupes`, placed by `toolLoupeRects`) are more frame plans, built like the view's own with the raster's projection panned to the loupe's point and zoomed `view.loupeMagnification` times the view, and with the layers' passes alone or the backplate alone. They are drawn after the whole view, each over its own backdrop: the backdrop pipeline writes depth with `Always` under a `[1, 1]` viewport depth range, so it resets the depth of the square as well as painting it. The backplate's uniforms take a dynamic-offset slot per draw for the same reason the main uniforms do: the view and a loupe draw the photo with different framing in one frame. Captures draw no loupes.

## Raster Projection Filters

`filter_color_projection` performs CPU-side projection from calibrated raster layers into mesh color/texture data:

- `compute_color_from_current_raster_projection`: projects the current raster onto current mesh vertex colors, optionally restricted by a software depth buffer and/or vertex selection.
- `compute_color_from_active_rasters_projection`: blends all visible rasters with valid `CameraShot`s into vertex colors using optional angle, distance, image-border, depth-discontinuity, and alpha weights.
- `compute_color_and_texture_from_active_rasters_projection`: projects visible rasters into a generated texture image over existing wedge UVs, with optional pull-push gap filling to reduce mipmapping seams.

These filters use `CameraShot::project(...)`/`depth(...)` and the mesh transform, not the live GPU scene pass. Their outputs are ordinary mesh data changes: vertex-color filters mark `VC`, while the texture bake marks wedge texture output and updates material/texture metadata through the normal filter/document path.

## Texture Normal-Map Workflow

PBR rendering can consume normal maps directly as either tangent-space or object-space textures through the render overlay's `Normal space` control. The Texture filter `convert_object_space_normal_map_to_tangent_space` converts an associated object-space normal map into a tangent-space image for the selected UV/material slot. It samples only faces using that slot, supports axis inversion and output-to-existing-or-new texture targets, and can bind the generated image as the selected material slot's PBR normal texture.

## Quality Histogram Overlay

A 2D panel inside `RenderWidget`, driven by the `qualityHistogram*` and `qualityIsoline*` fields of `GlobalRenderSettings`: the source (auto, forced vertex or forced face), bins, range, colormap and isolines. It shares the color mapping of quality-based rendering, so its colors and the rendered ones agree.

Unlike the pass-bar/settings overlay panel, which floats over the 3D content regardless (fine for something you open briefly to change a setting), the histogram claims a real strip on the right of the on-screen view for as long as it is showing, so it never sits on top of the mesh it describes. The right, not the left, so that opening it leaves the pass bar's buttons where they are. `RenderWidget::sceneContentRect()` narrows the viewport by that strip whenever `wantsQualityHistogramPanel()` holds -- `showQualityHistogram`, `Scene3D`, and a valid current mesh, mirroring `updateQualityHistogramOverlay()`'s own conditions for showing something at all -- and `viewTiles()` tiles within the result, in both layer arrangements, so the camera aspect ratio, mouse hit-testing, and every overlay that follows a tile's rect (the trackball gizmo, the current-mesh outline, grid tile captions) move together. The axis gizmo and the decorator info, which anchor to the right edge of the content area rather than of the widget, shift left by the same amount in `layoutOverlayButtons()`, and the tool strip and the status line centre on the content. In UV/Raster mode, where nothing consults `viewTiles()` for its own viewport, the histogram (if shown there at all) still just floats, at the widget's right edge.

The strip is decided once, in logical pixels: `qualityHistogramPanelWidth()` takes no size and reads the widget's own width (a fraction of it, clamped), because the panel is a native widget laid out in logical pixels. `sceneContentRect()` only scales it into the caller's units -- 1:1 for hit-testing, the device pixel ratio for the on-screen render target. Clamping separately in each unit would make the rendered strip and the panel disagree on a high-DPI screen, and the camera would frame a different area from the one the mouse is tested against.

An offscreen capture keeps the whole frame (`m_offscreenCaptureActive`, set for the duration of `renderOffscreenToImage()`): the panel is never part of a capture, and `cameraShotForViewport()` describes a capture as one full-frame camera, so a reserved strip would come out as a blank band and a snapshot raster would be misregistered against its own camera.

The backdrop is drawn across the whole target when there is a single tile, rather than in the tile's viewport, so the strip behind the translucent panel continues the gradient instead of showing the flat clear colour; the gradient only varies with height and the tile spans all of it, so nothing inside the tile changes. With more than one tile the strip is one more gap and clears to the grid's own frame colour like the others. `tests/render_probes/quality_histogram_layout` checks the capture side: a snapshot is pixel-identical with the histogram on or off, in both arrangements. The on-screen strip cannot be checked from a probe, since every render a probe can ask for is a capture, which keeps the full frame (see [Verifying the Render Path](#verifying-the-render-path)).

## Snapshot Capture

`MainWindow::saveSnapshotPng()`: set fixed color-buffer size → request update → wait for `frameRendered` → `grabFramebuffer` → restore previous size. Saved PNG embeds camera JSON in `MeshLab.CameraTrackballState` metadata.

*Fit the image to the scene* in the snapshot dialog is Frame Scene from Here for the image alone, with the aspect chosen too (`RenderWidget::fitSceneFromEye(..., fitAspect)`): the camera turns about the eye and zooms so that the visible layers touch all four sides, the aspect is locked to theirs, and the size already entered (or a preset) becomes the box the image fits in, its height rounded down so that rounding never cuts into the scene. The fit is worked out the first time the box is ticked, since it reads every visible vertex; a fit that cannot be made unticks and disables it with the reason. `SnapshotDialog::capture()` lends the fitted camera to the view through `applySynchronizedTrackballState`, which does not broadcast it to the views following this one, and returns the `CameraShot` and camera JSON of the capture with the image, so the PNG metadata and a snapshot raster describe the camera that took it rather than the view's.

Snapshot-to-raster paths reuse the same view capture mechanics but add the resulting image to `Document` through `addRasterImage(...)` with a `CameraShot` from `RenderWidget::cameraShotForViewport(...)` or from `renderSnapshotFromStateJson(...)`. This is how manual snapshot rasters and the `Render from Render-State JSON` layer filter create raster layers.

Programmatic snapshots use the same render-state JSON contract. Embedded `mlgui.render_snapshot(...)` and `mlgui.save_snapshot(...)` render through the live active `RenderWidget`; standalone `pymeshlab2.MeshSet.render_snapshot(...)` uses `HeadlessRenderContext`, which owns a hidden `RenderWidget`/QRhi lifecycle for offscreen and batch rendering.

For documentation images that must show the entire application, `MeshLab --capture-window
<mesh-or-project> <output.png>` opens the input in a 1440×900 main window, waits for
its first frames, saves `MainWindow::grab()` in a simple frame bearing the actual
`windowTitle()`, then exits. This captures the Qt window content (including docks and
the render view); the frame is drawn by MeshLab because Qt omits macOS window
decorations from `QWidget::grab()`. Run it with a real window server and GPU, as for the render probes below. The
window's dock layout comes from the current MeshLab settings; use a clean profile or
arrange it before capture when a repeatable presentation matters.

## Verifying the Render Path

The render path lives in the `MeshLab` executable, not a library, so a Qt test cannot link
`RenderWidget` and there is no ctest for anything a shader decides. The route is the
application's own `--generate-docs <dir>` hook, which execs `<dir>/generate_api.py` with the
`_meshlab` module importable; a probe there builds a `MeshSet`, calls `render_snapshot()`
with a render-state JSON, and asserts on the returned raw RGBA8888.

`tests/render_probes/` holds these -- `clip_plane` for the clipping plane and solid cut,
`clip_space` for the backend correction of clip space, `radiance_scaling` for the radiance-scaling option, `reference_grid` for the reference grid, `orthographic` for the orthographic camera through snapshots and projection filters, `boundary_decorators` for what the boundary decorator marks and in which colors,
`quality_histogram_layout` for captures staying full-frame while the histogram is open, `current_layer_outline` for the current-layer outline landing round the current layer (not mirrored top to bottom). Each is a
directory with a `generate_api.py`, because that is the filename the hook runs:

```
build-release/MeshLab.app/Contents/MacOS/MeshLab --generate-docs tests/render_probes/clip_plane
```

They need a real GPU and a window server. Under `QT_QPA_PLATFORM=offscreen` there is no QRhi
and every snapshot fails, which is the opposite of how the ctest suite is run. With
`MESHLAB2_GRAPHICS_API=opengl` they run on OpenGL (4.1 core on a Mac), the API Linux uses:
run them both ways after touching a shader or the way a pass samples another's output.
`HeadlessRenderContext` adds its view to the hidden window *before* showing it, since a
top-level window sets up its QRhi only for the QRhiWidgets it holds when first shown: shown
empty, every snapshot fell back to a QRhi of its own, made and destroyed each time, and on
OpenGL made as a legacy 2.1 context the GLSL 150 shaders cannot run in.

**Sampling a pass's own output.** A pass that reads, at its fragment, a texture another pass
of this frame rendered at the same size (the current-layer masks, read back by the outline)
reads at `gl_FragCoord.xy / size` and never flips: both targets were rendered by the same
backend, so their rows run the same way -- from the bottom on OpenGL, from the top elsewhere
-- and `isYUpInFramebuffer` must not enter into it. The outline composite flipped on Y-up
framebuffers, which mirrored the outline on OpenGL only. A CPU readback (the depth pick) is
different: there the bottom-up rows of OpenGL do have to be turned round, and that is the one
place the flag is read.

**The clipping plane does nothing on OpenGL.** Qt's OpenGL backend never enables
`GL_CLIP_DISTANCE0`, so the `gl_ClipDistance` the shaders write is ignored; Metal, Vulkan and
D3D apply it without being asked. `clip_plane` fails on OpenGL for this reason.

Two things to know before writing one, both of which look like bugs first: the default
background is a gradient that passes through a band any naive "is this pixel lit" test
scores as geometry, so render on a flat colour; and a pass that changes only hidden geometry
must leave the image **exactly** unchanged, so equality is the assertion, not a tolerance.

**What only the screen shows.** Every render a probe can ask for is a capture, so what a view
does only on screen -- the strip the quality histogram reserves -- is out of a probe's reach.
Check it by grabbing a live window with `QWidget::grab()`, which composites native child
widgets over the view, at a device pixel ratio of 2.

Moving the render sources into a linkable library would let these become real tests.

## Diagnosing Another Machine

Qt's warnings -- a shader the driver will not compile, a pipeline it will not link -- go to
stderr, which nobody reads for a program started from a desktop launcher. `SystemInfo`
(`src/ui/systeminfo.h`) forwards them, and MeshLab's own `qWarning()`s, to the log as warnings
(each distinct one once, the first few lines of it; a pipeline that fails is retried every
frame), and keeps the last twenty. The first frame any view draws writes one *Graphics:* line
to the log: the API, the device and its kind, and on OpenGL the driver's version string.
**Help > About > System** shows the build, the windowing system and the graphics-related
environment, the screens, the API, device and driver (OpenGL's version, renderer, vendor, GLSL
version and profile), the QRhi features and limits the renderer leans on, and the recent
warnings; *Copy System Information* puts it all on the clipboard for a bug report.

## Frame Timing

`RenderWidget` emits per-frame CPU time (`QElapsedTimer`) and GPU time when `QRhi::Timestamps` is supported (`lastCompletedGpuTime`). `MainWindow` displays a rolling 100-frame average in the status bar. The GPU time needs a graphics context created with timestamps: with the `advanced.gpuTiming` preference on, `main.cpp` sets `QSG_RHI_PROFILE` before the first window, and without it the status bar reads *GPU: not measured*.
