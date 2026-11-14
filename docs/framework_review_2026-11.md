# Framework review: rendering, undo, Qt integration (2026-11-14)

A one-off assessment, written to be handed to an agent as a work list. It is **not** a
`docs/design/` document: nothing here is settled design. Delete it once the items are
done or rejected.

Every item says how it was established:

- **measured**: timed or sized on the Release wheel (`build-wheel`, `CMAKE_BUILD_TYPE=Release`)
  against `.reference/TestDataset`;
- **confirmed**: read through the whole code path;
- **likely**: read, but one link not verified. Check it before acting.

Line estimates are `+added / -removed`. The house rule applies to every item: least code wins,
reuse vcglib and existing mechanisms, and **ask before anything marked "large"**.

---

## 0. Measurements the findings rest on

Release wheel, `QT_QPA_PLATFORM=offscreen`, RSS from `ps`. Each step is one `apply_filter`.
That includes the undo capture, which is where most of the memory goes.

| Mesh | V / F | Step | Time | RSS delta |
|---|---|---|---|---|
| RattoA_2M | 1.08M / 2.16M | load | 189 ms | +380 MiB |
| | | select_all / select_none / invert (delta undo) | 45-63 ms | +3..6 MiB |
| | | `set_mesh_color` (one per-mesh RGBA) | 64 ms | **+171 MiB** |
| | | `colorize_vertices_by_perlin_noise` (color delta) | 66 ms | +9 MiB |
| | | `translate` (transform only), first after the color delta | 72 ms | **+180 MiB** |
| | | `translate` again | 25 ms | +1 MiB |
| | | `duplicate_current_layer` | 107 ms | +499 MiB |
| ESTE_PRINT | 4.97M / 9.94M | load | 836 ms | +1726 MiB |
| | | `measure_geometric_properties` (read-only) | 11.4 s | +645 MiB |
| | | select_all, then `measure_geometric_properties` again | | **+321 MiB** |
| | | select_none, then `translate` | 244 ms | **+533 MiB** |
| | | `set_mesh_color` twice | ~200 ms each | **+762 MiB each** |
| afrodite.mlp | 41 meshes, 30.6M F | load project | 2.8 s | +5.1 GiB |
| 2930Colmap | 2 meshes + 35 rasters (3840x2160) | load project | 361 ms | +600 MiB (no photos decoded yet) |

What the table shows:

- **Selection, color and scalar deltas work as designed** (a few MiB).
- **Every other step costs a full deep copy of the mesh** whenever the mesh's identity key has
  moved since the last capture. That includes steps that change nothing in the geometry: a
  per-mesh color, a transform after a selection, a read-only measurement after a selection.
  See U1-U3.

Undo/redo timing could not be measured from Python, because the bindings expose no `undo()`.
Section U4 is from code reading. A 30-line `undo`/`redo`/`jump` binding would make these
numbers measurable; it is worth adding only if the maintainer wants them in scripts.

---

## 1. Undo system

The tree model, node serials, geometry interning by `weak_ptr` and the memory accounting are
sound. The costs are in what is captured and what is restored.

### U1. A selection or color change forces the next full step to deep-copy the mesh. Measured, confirmed. High.

`captureUndoState()` interns geometry by `(meshId, geometryRevision, selectionRevision,
materialRevision)` (`src/core/document_undo_state.cpp:46`, `documentundomanager.h:70-75`).

- A selection delta bumps `selectionRevision`.
- A color or scalar delta bumps `geometryRevision`.

So the next full-snapshot step (a transform, a rename, a read-only filter: see U2) misses the
cache and deep-copies the whole mesh: +533 MiB on ESTE for a `translate` after `select_none`.
`pushStep` then stores that copy on the delta node (`documentundomanager.cpp:898`), so the delta
saving lasts only until the next non-delta action.

**Fix, smallest form (medium).** Take selection out of the geometry identity:

- `MeshSnapshot` gets a packed selection (`std::shared_ptr<const std::vector<uint32_t>>` per
  element kind, interned by `selectionRevision` exactly as geometry is by its key).
- The geometry key drops `selectionRevision`.
- `restoreUndoState` writes the bits back. `captureAttributeDelta`/`applyAttributeDelta` already
  pack and unpack selection bits, so reuse them.

On ESTE that is 1.2 MiB per selection state instead of 533 MiB.

**Fix, full form (large, ask first).** Do the same for color and scalar: they need their own
revision, `colorRevision`, instead of riding `geometryRevision`. Then every node is a full
snapshot made of shared layers (geometry, selection, color, scalar), and the `Delta` node kind
goes away. That removes:

- `beginDeltaStep`/`endDeltaStep`/`pushDeltaStep`;
- the delta branches in `undo()`/`redo()`/`endStep()`;
- `m_pendingDelta*`;
- the "delta nodes cannot become roots" special case in pruning (`documentundomanager.cpp:774-783`);
- the `outputModifies`→delta-kind table in `meshfilterpluginmanager.cpp:1056-1082`.

Estimated net **-150 lines**. It also removes the hazard data_model.md records ("the descriptor
is trusted completely"): a snapshot made of shared layers is correct whatever the descriptor
says.

### U2. Read-only filters still capture a full undo state. Measured, confirmed. High.

`MeshFilterPluginManager::runFilter` always opens an undo step
(`src/plugins/meshfilterpluginmanager.cpp:1097-1112`; the comment says this is on purpose,
because nested operations collapse into it). `beginStep` captures a full `UndoState`
(`documentundomanager.cpp:122`). After any selection or color change, that capture deep-copies
the mesh: +321 MiB on ESTE for `measure_geometric_properties`, and the copy is then thrown away.

**Fix (+15 / -0).** Make the "before" capture lazy:

- `beginStep` records only the label and script action.
- `Document` captures "before" the first time anything mutates inside the step. Hook it on the
  existing entry points: `markMesh*Changed`, `addMesh`, `removeMesh`, `setMeshTransform`, and
  the other mutators listed in data_model.md "APIs".
- A filter that never mutates never captures.

There is a cheaper partial fix: skip the capture when the descriptor's `outputModifies` is empty
**and** it creates no layer. But that again trusts the descriptor, so prefer the lazy capture.

### U3. `set_mesh_color` deep-copies the mesh and rebuilds every GPU buffer to change one RGBA. Measured, confirmed. Medium.

`set_mesh_color` declares no `outputModifies` and calls `markMeshGeometryChanged`
(`plugins/filter_colorproc/colorprocfilterplugin.cpp:250`). That costs +762 MiB on ESTE per call,
plus a full fill/wire/points rebuild.

The renderer never reads that color from a buffer: `renderwidget_fill.cpp:122-128` reads
`mesh.C()` into a uniform every frame.

**Fix (+3 / -1).**

- Carry `mesh.C()` as cheap snapshot metadata. `MeshSnapshot` already copies `transform`, so add
  one `vcg::Color4b`, and have restore put it back.
- Have the filter bump nothing but a repaint.

Audit the other per-mesh-only filters in `filter_colorproc` for the same pattern.

### U4. Every full undo/redo re-copies every mesh and drops the whole GPU cache. Confirmed by reading. High for multi-layer documents.

`Document::restoreUndoState` (`src/core/document_undo_state.cpp:127-175`):

- erases every live mesh and `purgeMeshGpuResources` each one;
- `deepCopyMesh`es **every** mesh out of the snapshot;
- then calls `clearAllGpuResources()`.

Undoing a translate of one layer of afrodite (41 layers, 30.6M faces) therefore copies 30.6M
faces. At the ~50 ms per M faces implied by `duplicate_current_layer` that is about 1.5 s. It
then rebuilds every fill/wire/point buffer on the next frame.

The rasters right below already do the right thing (`:185-208`): they keep a live entry whose
id and revisions match the snapshot.

**Fix (+20 / -15). Same pattern for meshes:**

- Keep a live `MeshEntry` whose `(meshId, geometryRevision, materialRevision,
  selectionRevision)` equals the snapshot's. Update only its cheap metadata (transform, name,
  visible, ...).
- Deep-copy only the meshes that differ.
- Delete the `clearAllGpuResources()` call and the per-mesh purge. The cache is keyed by
  revisions, and revisions are document-monotonic (data_model.md), so a changed mesh misses and
  rebuilds on its own. Purge only the `meshId`s absent from the restored state.

This also makes the `meshAdded` storm of Q4 unnecessary for unchanged layers.

### U5. `jumpToNode` replays every intermediate state. Confirmed. Medium.

`jumpToNode` (`documentundomanager.cpp:329-430`) walks `undo()` up to the lowest common
ancestor, then `redo()` down. Each step is a full `restoreUndoState`, so with U4 unfixed, a jump
across k states makes k full document copies and k GPU-cache clears. The Action History click
and `updateUndoNodeCameras` (which visits many states) both use it.

**Fix (+30 / -45).** Find the target's nearest full-snapshot ancestor-or-self, restore it once,
then apply the `afterAttributes` of the delta nodes between it and the target. Set
`preferredChild` along the path as now. The debug-only `redoPreview` block (`:367-379`) goes
away. After U1's full form, every node is a snapshot and this collapses to one restore.

### U6. Four copies of the tree-compaction code. Confirmed. Low risk, pure deletion.

`makeRoot` (`:561-579`), `purgeBranch` (`:634-653`), `linearizeHistory` (`:686-703`) and
`pruneTreeToLimit` (`:1066-1087`) each:

- build `std::map<int,int> remap`;
- move nodes, remap `parentId`/`children`/`preferredChild`.

**Fix (+25 / -65).** One helper:
`void compact(const std::vector<int> &keepSorted, int newRootOldId, int currentFallbackOldId)`.
A `std::vector<int>` of size `m_undoNodes.size()` filled with -1 replaces the `std::map`.
`stepsToRemove` in `pruneTreeToLimit` (`:1048`, `:1090`) is computed and never used.

### U7. Memory pruning is quadratic. Confirmed. Low (off by default).

`pruneToMemoryBudgetInternal` calls `memoryStats()` after every root advance (`:790`).
`memoryStats()` walks every node and every image into `std::set`s. When the budget preference is
on this is O(nodes²) after every step. Compute the bytes once, subtract what each `makeRoot`
released, and re-measure only at the end.

### U8. Debug traces left in Release. Confirmed. Trivial.

`qDebug` runs in Release here: there is no `QT_NO_DEBUG_OUTPUT`. On every undo/redo/jump:

- `[JUMP]`, `[UNDO]`, `[REDO]`, `[STATE SAVED]` (`documentundomanager.cpp:374, 440, 483,
  924-928, 973-978`);
- `[STATE RESTORE]`, which **dumps the whole tree, one line per node**
  (`document_undo_state.cpp:90-102`).

The `QElapsedTimer` blocks that feed `writeLog(..., Debug)` in `restoreUndoState` are fine.
**Remove the 7 `qDebug` blocks (-35 lines).**

Also stale wording:

- The log says "(selection delta)" for color and scalar deltas too (`:451`, `:494`).
- The `selectionOnlyUndo` variable (`meshfilterpluginmanager.cpp:1082`) covers color and scalar.

---

## 2. Rendering system

The request → plan → execute split, the revision-keyed shared cache, the dynamic-offset UBO
allocators and the clip-space discipline are good architecture. Most findings are about doing
work every frame that should be done once, and about one QRhi API misuse.

### R1. Uniform uploads call `QRhiCommandBuffer::resourceUpdate` inside an open render pass. Confirmed in code; Qt-side effect likely. High (correctness).

Qt's documentation for `resourceUpdate`: *"Cannot be called inside a pass."* Yet:

- `uploadMainUbuf` (`renderwidget_render.cpp:219-221`) does it once per draw item;
- so do the decorator, fat-decorator and selection uploads (`renderwidget_scene_passes.cpp:114-117,
  144-147, 221-224`) and the solid-cut cap (`:341-344`);
- all of them run between `beginPass` (`renderwidget_render.cpp:716`) and `endPass` (`:844`).

It works today because Qt's backends happen to flush dynamic-buffer writes lazily. A Vulkan or
D3D debug build of Qt asserts on it, and it creates one `QRhiResourceUpdateBatch` per draw.

**Fix (+40 / -60).** The allocators already hand out disjoint offsets, so:

- During plan execution, write each slice into a per-frame CPU `std::vector<float>` per UBO.
- Issue one `updateDynamicBuffer` per UBO into the batch `u` that is already passed to
  `beginPass`.

That means splitting each `render*Items` into "fill uniforms" (before the pass) and "draw" (in
the pass). Equivalently, run the plan twice with a flag. Each `upload*` lambda loses its own
batch boilerplate.

### R2. The 3D view decodes every raster photo to draw a frustum glyph. Confirmed. High for photogrammetry projects.

In `Scene3D`, `collectRenderFramePassRequests` calls `Document::ensureRasterPlaneImage` for
every visible raster and drops rasters whose image is null (`renderwidget_frame_plan.cpp:75-85`).
`ensureRaster` does the same (`renderwidget_raster.cpp:275-281`). Yet rendering.md says the 3D
view draws only "small line frustums" and "the raster image itself is not pasted into the 3D
scene".

On 2930Colmap the first 3D frame therefore decodes, and lens-undistorts
(`document_layers.cpp:850-853`), all 35 photos at 3840x2160. That is about 33 MB each in RGBA8,
**≈1.1 GiB**, to draw 35 line boxes. The project loads in 361 ms precisely because nothing is
decoded yet.

**Fix (-10).** In the Scene3D branch, test `entry.shot.isValid()` only. Decode only in
`RasterImage` mode and only for the current raster.

### R3. A raster backplate is converted to RGBA8888 on every frame. Confirmed. Medium.

`renderwidget_raster.cpp:336` runs `plane->image.convertToFormat(QImage::Format_RGBA8888)`
**before** the `needsTexture` test (`:340-344`). A PNG decodes to `RGB32`/`ARGB32`, so in raster
mode every frame converts and copies the full photo (33 MB at 4K) and then discards it.

**Fix (±0).** Test `needsTexture` against `plane->image.size()`, and convert only inside the
`if (needsTexture)` block.

### R4. Selection overlay rebuild reserves for the whole mesh. Confirmed. Medium.

`meshgpuresourcecache.cpp:1378`: `selectedFaceTriangles.reserve(FN * 9)` floats, plus
`VN * 3` (`:1410`), in a serial loop, on every `selectionRevision` change. On ESTE that is a
**358 MiB transient allocation** for each brush stroke or rubber-band, even when one face is
selected.

**Fix (+3 / -2).** Count the selected faces first, in the same parallel pass, or `push_back`
with no reserve. Better and less memory: upload a `uint32` index list of the selected faces and
draw it against the existing indexed fill vertex buffer: 12 B per selected face instead of 36,
and no position copy. Medium.

### R5. Unused fill and point variants are never released. Confirmed. Medium.

`MeshGpu` holds `std::array<FillVariantGpu, 6>` and `std::array<PointsVariantGpu, 3>`
(`meshgpuresourcecache.cpp:219-228`). Switching a layer's color source from per-vertex to
scalar to texture leaves the old variants' buffers alive until the mesh is purged. After a
geometry edit, the variants not on screen keep the **previous revision's** buffers forever.

Expanded fill uses 52 B per corner (`kFillVertexStrideFloats = 13`). On ESTE one expanded variant
is ~1.55 GB, an indexed one ~0.38 GB.

**Fix (+8).** In `ensureMeshResources`, after rebuilding, release every other variant of the
same mesh whose `geometryRevision`/`materialRevision` differs from the source's. An LRU is not
needed.

### R6. The quality range is baked into vertex buffers, with four copies of the invalidation logic. Confirmed. Medium.

Normalized quality is written into the buffers. So changing fixed min/max, center-on-zero or
percentile crop **rebuilds the whole fill or point buffer**: 10M faces per slider drag. The
"same range?" test exists four times:

- fill variant (`meshgpuresourcecache.cpp:284-317`);
- point variant (`:1074-1105`);
- UV cache (`renderwidget_uv.cpp:205-240`);
- histogram cache (`renderwidget.cpp:3165-3195`).

The range computation itself is already shared (`qualityrange.h`).

**Fix (+25 / -90).**

- Store raw quality in the buffer.
- Compute the automatic range once per `(meshId, geometryRevision, centerOnZero, crop)` in one
  small cache next to the histogram.
- Pass `(min, max)` as two floats in the per-mesh UBO slice; the shader normalizes.

The `quality*` fields leave `MeshSource`, `FillVariantGpu`, `PointsVariantGpu`, `UvMeshGpu` and
`Document::ensureMeshGpuResources`'s signature.

### R7. Document is a pass-through layer for the GPU cache. Confirmed. Low risk, pure deletion.

`src/core/document_gpu.cpp` (186 lines) holds:

- eight `*PassGpuView` methods that each check the index and forward to the cache by `meshId`;
- `ensureMeshGpuResources`, which takes **19 positional arguments, nine of them `bool`**, and
  repacks them into `MeshSource`.

`document.h:176-186` re-exports the cache's types as `Document::FillGpuVariant` and friends.
`RenderWidget` then `static_cast`s between the two names (`renderwidget_selection.cpp:88-91`).

**Fix (+30 / -200).**

- Give `Document` one `MeshGpuResourceCache &gpuCache()` and a `MeshSource meshGpuSource(int
  index)` helper.
- Replace the bool list with a `struct MeshGpuNeeds`.
- Have `RenderWidget` call the cache directly with the `meshId`.

`document.h` then no longer needs to include a `src/render` header.

### R8. Per-frame work that only needs to happen on change. Confirmed. Low each, cheap to fix.

1. `syncPerMeshRenderModesWithDocument()` runs at the top of every frame
   (`renderwidget_render.cpp:298`). It also runs in every document-signal slot, and allocates an
   `unordered_map` of all mesh ids each time. Call it from the structural signals only.
2. `collectRenderFramePassRequests` runs once for the frame and again once per grid tile
   (`renderwidget_render.cpp:336, 641`). Each run walks all meshes and copies their
   `PerMeshRenderSettings` (`renderModeForMesh` returns by value), which is O(N²) per frame in
   grid mode. Filter the frame-wide request by `meshIndex` instead.
3. The "8 bbox corners through the layer matrix" block appears **four times**:
   - `computeWorldSceneBBox` (`renderwidget_resources.cpp:18-57`);
   - `meshFitsInCurrentFrame` (`:59-96`);
   - raster depth fit (`renderwidget_render.cpp:430-461`);
   - `rasterFrustumDepth` (`renderwidget_raster.cpp:164-210`).

   The last one also runs **per raster per frame** and re-uploads every frustum's vertex buffer
   each frame. One `worldCorners(const MeshEntry&)` helper: about **-70 lines**. Cache the
   frustum vertices by `cameraRevision` plus the scene bbox.
4. `Preferences::value` finds the descriptor by linear search (`preferences.cpp:60-67`) on every
   read of a preference left at its default. The frame path reads several each frame:
   `view.renderDuringProjectLoad`, `view.loupeMagnification`, `render.selectedEdgeWidth` per
   selection item, and `viewGridStyle()`. Index the descriptors in a `QHash` once in
   `ensureLoaded` (+3 / -4).
5. `render()` hides every UV tick `QLabel` on every 3D frame (`renderwidget_render.cpp:282-289`).
   Do it on the mode switch.
6. Four copies of the 1x1 fallback-texture upload (`renderwidget_selection.cpp:34-69`) collapse
   to a loop over `{texture, pendingFlag, color}` (-25).

### R9. The UV renderer has its own copy of the GPU cache. Confirmed. Large: ask first.

`ensureUvMeshResources` (`renderwidget_uv.cpp:187-676`, about 490 lines) rebuilds in the view
what `MeshGpuResourceCache` builds in the document:

- fill variants;
- points;
- wire;
- boundaries;
- selection;
- its own quality-range key, and it bakes the **colormap** into the buffer, where the 3D path
  samples a LUT.

It is per view, not shared. rendering.md already names "later UV convergence" as the goal.

**Smallest convergence.** Add a `UvLayout` variant to the shared cache, whose positions are
`(u, v, 0)` taken from wedge or vertex texcoords, with the same 52-byte layout. Then the UV view
draws it with the existing pipelines and an orthographic matrix. Estimated -350 lines. Do this
after R6, which removes the quality and colormap divergence.

### R10. Wireframe memory. Confirmed. Low, note only.

The wire buffer is fully expanded: 3 corners × 6 floats = 72 B per face, so 716 MB on ESTE.
That is acceptable because wire defaults on only below 10k faces. If it matters later, the
barycentric can come from `gl_VertexIndex % 3` over the expanded flat-fill stream. That needs no
new buffer, but it does need faux-edge bits per face.

---

## 3. Qt integration

The basics are right:

- the layer panel coalesces rebuilds through a queued call and defers them during bulk loads
  (`layerwidget.cpp:1536-1547`), and builds only the visible presentation (`:1665-1671`);
- the filter UI refresh is coalesced the same way (`mainwindow.cpp:2374-2384`);
- a selection change repaints without rebuilding (`renderwidget.cpp:743-745`).

The leaks in that discipline:

### Q1. Removals and raster changes bypass the coalescing. Confirmed. Medium.

- `LayerWidget` connects `meshRemoved` and `rasterRemoved` straight to `rebuild()`
  (`layerwidget.cpp:1479, 1505`), while every other signal goes through `scheduleRebuild()`.
  `restoreUndoState` emits `rasterRemoved` once per changed raster, so that is one synchronous
  full rebuild each.
- `MainWindow` answers `rasterAdded`, `rasterRemoved`, `currentRasterChanged` and
  `rasterDataChanged` with a **synchronous** `refreshFiltersMenu()` (`mainwindow.cpp:1450-1465`).
  That recomputes applicability for all 405 filters and rebuilds the whole menu. The bulk-load
  guard does not cover it, so a 35-raster project rebuilds the menu 35 times. These slots also
  skip `m_filterPanel->reloadFilters`, while the mesh slots get it through
  `scheduleFilterUiRefresh()`. That looks like a bug: raster-dependent filters keep a stale
  enabled state in the panel. Likely.

**Fix (-6).** Use `scheduleRebuild` and `scheduleFilterUiRefresh` everywhere.

### Q2. Undo restore fans out per mesh into heavy RenderWidget slots. Confirmed. Medium.

`restoreUndoState` emits `meshAdded(i)` for every mesh (`document_undo_state.cpp:168-169`).
RenderWidget's `meshAdded` slot (`renderwidget.cpp:626-666`) checks `isRestoringUndoRedo()` only
for reframing. On each emission it then runs:

- the O(N) mode sync;
- `refreshColorSourceAvailability`;
- `m_textureSrbs.clear()`;
- the UV-cache sync;
- the bbox overlay;
- the histogram overlay;
- `layoutOverlayButtons`.

That is O(N²) per undo. `undoRestoreCompleted` exists exactly to batch this ("This is the batch
they were promised").

The `meshAdded`, `meshRemoved` and `meshDataChanged` lambdas are also three near-copies of the
same eight calls (`:626-739`).

**Fix (+10 / -35).**

- Fold the three lambdas into one `onLayersChanged()`.
- Return early while restoring.
- Call it once from `undoRestoreCompleted`.

**Likely side issue to verify.** Because restore emits `meshAdded` without the matching
`meshRemoved`, `m_meshVisibility` grows by N with `true` entries and is then truncated
(`ensureVisibilitySize`). So after any full undo, the view's own hidden layers become visible
again, except those hidden in the document proxy. Check that against the intended per-view
visibility semantics in data_model.md.

### Q3. History thumbnails. Confirmed. Low.

Every new current state calls `QRhiWidget::grabFramebuffer()` (`mainwindow.cpp:3869`): a full
extra render plus a synchronous GPU readback. It keeps:

- a 96×48 thumbnail;
- a **half-framebuffer** hover snapshot: about 14 MB per state on a 5K display.

Old states are pruned with the tree (`:3988-4005`), so this is bounded but large.

**Fix (±0).** Cap the snapshot to about 640 px wide.

### Q4. `Preferences::changed` and settings. Checked, fine.

Preferences are cached in memory after the first load, and `QSettings` is touched only on writes
(`preferences.cpp`). Q1's slots are the only signal fan-out problems found.

---

## 4. Leftovers and duplication. Approval needed before removal.

**Done 2026-11-14:** every row whose verdict is "remove" and U8 have been removed (the
`undoStackLabels` use became `canUndo()`, which is what it meant: a non-root current node).
Still open: `undoCursorPosition`, `menuPath`, the aliases, and the collapse items.

Evidence is a whole-repo grep over `src plugins tests python` (`*.cpp *.h *.py *.mm`).

| Item | Where | Evidence | Verdict | Lines |
|---|---|---|---|---|
| `MainWindow::captureUndoHistoryThumbnail()` | `mainwindow.cpp:3819-3845`, `.h:143` | 0 callers; superseded by `captureUndoNodeImages`, which repeats its crop code | remove | -29 |
| `Document::loadedPluginSummaries`, `loadedFilterPluginSummaries`, and the two managers' `loadedPluginSummaries` | `document_filters.cpp:5-15`, `meshiopluginmanager.cpp:135-142`, `meshfilterpluginmanager.cpp:980-988`, headers | 0 callers outside their own definitions | remove | -30 |
| `undoHistoryLabels` / `undoStackLabels` (Document + manager) | `documentundomanager.cpp:247-265`, `document_undo.cpp:128-136` | `undoHistoryLabels` 0 callers; `undoStackLabels` is an alias with one use, `mainwindow.cpp:2086` `!undoStackLabels().isEmpty()`, which means "there is history" | replace that use with `canUndo() \|\| canRedo()`, remove both | -30 |
| `undoCursorPosition` (Document + manager) | `documentundomanager.cpp:267-278` | used only by `tests/test_filters.cpp` (5 uses) | keep, or move into the test as a helper over `undoTreeInfo()` | -15 |
| `DocumentUndoManager::suppressSignals()` | `documentundomanager.h:66` | 0 callers | remove | -1 |
| `EnsureStats::uploadedResources` | `meshgpuresourcecache.h:161`, `.cpp:1962` | written, never read | remove | -2 |
| `stepsToRemove` | `documentundomanager.cpp:1048, 1090` | computed, `(void)`-cast | remove (goes with U6) | -2 |
| 7 `qDebug` trace blocks | U8 | debug leftovers | remove | -35 |
| legacy `menuPath` loader fallback | `filterdescriptorloader.cpp:211-225`, `.h:23` | 0 JSON files in `plugins/` or `resources/` use it; kept "for out-of-tree descriptors" | ask: are there any? | -12 |
| `using RenderSettings = GlobalRenderSettings`, `MeshRenderMode` alias | `renderingsettings.h:369`; 28 and 22 uses | compatibility aliases, documented as such | rename to the canonical names; this is a rename, so the mapping-table rule applies | ±0 |
| `Document::*GpuVariant` / `*PassGpuView` re-exports | `document.h:176-186` | pure aliases of cache types | remove with R7 | (in R7) |
| bbox-corner block ×4, compaction ×4, quality-range check ×4, fallback texture ×4, RenderWidget layer-change lambda ×3 | R8.3, U6, R6, R8.6, Q2 | read side by side | collapse | (in items) |

Checked and **not** leftovers: all 44 files in `shaders/` are loaded or included by
`src/render`, including `selection_mask_debug.frag`, which backs the outline debug views.

**Not covered.** The planned exhaustive zero-caller sweep of every header in `src/ui` and
`src/render` was cut short by a usage limit. The table above covers only the symbols checked
directly. A follow-up sweep should go header by header through `renderwidget.h` (1169 lines),
`mainwindow.h`, `layerwidget.h` and `renderoverlaypanel.h`.

---

## 5. Suggested order

All items except U1-full and R9 are small and net-negative. Do them in this order, each with its
own tests:

1. **Pure deletions:** section 4 table, U8, U6, R7, R8.3, R8.6. No behavior change; the existing
   suite covers it.
2. **Correctness:** R1. Verify with `MESHLAB2_GRAPHICS_API=vulkan` on a debug Qt if available,
   plus the render probes.
3. **Big wins, small diffs:**
   - R2 and R3: check by opening `2930Colmap/project.mlp` in 3D and watching RSS.
   - U4, Q1, Q2: undo on `afrodite.mlp`.
   - U3, R4, R5.
4. **Medium:**
   - U2 (lazy capture);
   - U1 small form (selection out of the key);
   - U5;
   - R6.
   - Re-run the section 0 table: the `translate`, `measure` and `set_mesh_color` rows should
     drop to a few MiB.
5. **Large, ask first:** U1 full form (removes delta nodes), R9 (UV onto the shared cache).

Tests to add (behavioural tier, per AGENTS.md "Tests"):

- `tests/test_document.cpp`: an undo step after a selection change does not deep-copy geometry
  (compare `undoMemoryStats().uniqueGeometryCount`);
- a full undo keeps unchanged meshes' `geometryRevision` and leaves their GPU cache entries in
  place;
- a read-only filter adds no geometry bytes.
