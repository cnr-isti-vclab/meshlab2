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

## 5. Plan

Six phases, one commit each, in this order. Phases 1-4 are net-negative or nearly so. Phases 5
and 6 change the undo memory model and the render data model. Phase 6 is "large": ask before
starting it.

Every phase ends the same way:

1. `cmake --build build-release`, then `QT_QPA_PLATFORM=offscreen ctest` there (about 6 min,
   dominated by `MeshLab2FilterTests` and `MeshLab2FilterSmokeTests`).
2. Run the render probes for any phase that touches `src/render` (they need the window server;
   see rendering.md).
3. Re-run the measurement in section 0 that the phase targets.
4. Leave `build-release` built from the final source: it is the maintainer's live binary.
5. Hand over the file list and the full commit message.

### Phase 1: undo restore that does not copy unchanged layers (U4 + U3 + Q2 + Q1)

**Done 2026-11-14.**

- **Measured** with a temporary `Document` harness on afrodite.mlp (41 layers, 30.6M faces):
  undo/redo of a one-layer transform went from 303/256 ms to 0/0 ms, and of a one-layer
  geometry edit from 251/252 ms to 7/4 ms (CPU only; the GPU cache is no longer cleared on top
  of that).
- **Two findings while doing it:**
  - **U3 was misdiagnosed above.** `set_mesh_color` moves no revision at all; its +762 MiB was
    U1. Its real defect was that its "after" state shared the "before" copy of the geometry,
    whose color is the old one, so redo restored the old color. Fixed by `setMeshColor` and
    `MeshSnapshot::meshColor`, and covered by `setMeshColorFilterUndoesAndRedoesWithoutACopy`.
  - **Keeping unchanged layers is only right for undo and redo.** The rollback of a cancelled or
    failed step must still copy everything, because a failed filter can edit a mesh without
    moving its revisions. Three `FilterTests` caught this; `restoreUndoState` takes
    `keepUnchangedMeshes`.
- **D1** was taken as (a): after a full undo every view shows the document's visibility.
- `Document::clearAllGpuResources` and `MeshGpuResourceCache::clearAll` lost their last caller
  and were removed.

**Why first.** It is the most visible cost in everyday use: every undo of a multi-layer scan
project re-copies every layer and re-uploads every GPU buffer. The diff is small and stays
inside `Document`, `RenderWidget`'s signal wiring and two plugin call sites.

**1a. `restoreUndoState` keeps live meshes whose content is unchanged** (`src/core/document_undo_state.cpp`).

- Move the live `std::unique_ptr<MeshEntry>`s out of `m_meshes` into an `unordered_map` keyed by
  `meshId`.
- For each `snap` in `state.meshes`:
  - If a live entry has the same `meshId` **and** equal `geometryRevision`,
    `materialRevision` and `selectionRevision`, reuse it. Overwrite only the cheap fields that
    the copy path sets today: `transform`, `name`, `sourcePath` (with the existing
    `m_storedMeshes` rule), `textureFileNames`, `textureFilePaths`, `textureAssets`,
    `materialSet`, `visible`, `ioMask`, `polygonFaceCount`, `pluginData`.
  - Otherwise build the entry as today, with `deepCopyMesh`.
- Push the entries back in snapshot order.
- `purgeMeshGpuResources` only the ids left in the map, i.e. layers that no longer exist.
- **Delete** the `clearAllGpuResources()` block (`:172-176`). The cache is keyed by revisions,
  and revisions are drawn from document-monotonic counters, so a restored older revision misses
  and rebuilds by itself.
- Keep the signal sequence as it is: `meshAdded(i)` for every index, then the current-index
  signals, then `undoRestoreCompleted`. Views that ignore signals while restoring
  (step 1c) then see one batch.

Risk: this trusts that every mutation bumps a revision. It is the same contract the GPU cache
already relies on: a filter that broke it already shows stale buffers on screen. The filter
smoke test's undo/redo pass (`tests/test_filter_smoke.cpp`) is the net for that.

**1b. Per-mesh color becomes undo metadata, like the transform (U3).**

- Add `Document::setMeshColor(int index, const vcg::Color4b &)`, modeled line for line on
  `setMeshTransform` (`document_layers.cpp:395-420`): own undo step "Set Mesh Color",
  write `mesh.C()`, log, `emit meshDataChanged`, **no revision bump**.
- Add `vcg::Color4b meshColor` to `MeshSnapshot`. Set it in `captureUndoState`, and write it into
  `mesh.C()` in `restoreUndoState` on both the reused and the copied path. This is needed
  because snapshots that share one interned geometry can carry different colors.
- Switch the two writers in `plugins/filter_colorproc/colorprocfilterplugin.cpp` to it:
  `set_mesh_color` (`:357`) and the scattered layer colors (`:379`), dropping their
  `markGeometry`.

No GPU buffer depends on it: `renderwidget_fill.cpp:122-128` reads `mesh.C()` into a uniform.
No file format stores a per-mesh color either (checked `plugins/io_*`), so, like the transform,
it does not mark a layer as changed against its file.

**1c. One batched view update after a restore (Q2)** (`src/render/renderwidget.cpp:626-739`).

- Fold the `meshAdded`/`meshRemoved`/`meshDataChanged` lambdas into one private
  `onLayersChanged()`. Keep the index-specific parts in the lambdas: the visibility
  insert/erase and the reframe decision.
- `return` early from all three when `m_doc->isRestoringUndoRedo()`.
- Connect `undoRestoreCompleted` to: `ensureVisibilitySize()`, the visibility rule chosen in
  decision D1 below, then `onLayersChanged()`.

**1d. Removal and raster signals use the existing coalescing (Q1).**

- `layerwidget.cpp:1479, 1505`: `&LayerWidget::rebuild` becomes `scheduleRebuild()`. Before
  changing it, click a layer row right after a deletion to confirm that no stale row can be
  clicked before the queued rebuild runs.
- `mainwindow.cpp:1450-1465`: `refreshFiltersMenu()` becomes `scheduleFilterUiRefresh()`. That
  also refreshes the filter panel, which the raster path currently skips.

**Tests** (`tests/test_document.cpp`, next to `undoRedoRestoresMeshList`):

- `undoKeepsUnchangedLayers`: two tiny meshes; record `&doc.mesh(0).mesh.vert[0]`; translate
  mesh 1; undo. Mesh 0's address and revisions are unchanged, and mesh 1's transform is back.
- `undoCopiesOnlyTheChangedLayer`: same setup, but run a geometry edit on mesh 1. After undo,
  mesh 1 has the old content and mesh 0 still has its old address.
- `setMeshColorIsMetadata`: `geometryRevision` is unchanged and
  `undoMemoryStats().uniqueGeometryCount` does not grow. Undo/redo round-trips the color
  (extend `perMeshColorSurvivesUndoAndRedo`).

**Measure.** Undo on `afrodite.mlp` after translating one layer: before and after. Needs
decision D2.

**Expected size:** about +60 / -45.

### Phase 2: render-side waste on large data (R2, R3, R4, R5)

All in `src/render`, independent of phase 1.

- **R2.** In `collectRenderFramePassRequests`' Scene3D branch (`renderwidget_frame_plan.cpp:75-85`)
  and in `ensureRaster`'s projected path (`renderwidget_raster.cpp:275-281`), require only
  `entry.shot.isValid()`. Do not call `ensureRasterPlaneImage`.
- **R3.** Move `convertToFormat(RGBA8888)` (`renderwidget_raster.cpp:336`) inside
  `if (needsTexture)`, testing `needsTexture` against `plane->image.size()`.
- **R4.** In the selection rebuild (`meshgpuresourcecache.cpp:1370-1430`), drop the
  `reserve(FN * 9)` and `reserve(VN * 3)`. Count first in a `parallelFor`, then fill with
  exact sizes. The indexed-overlay variant is a later option, not part of this phase.
- **R5.** At the end of `ensureMeshResources`, reset every fill and point variant of this mesh
  whose `geometryRevision`/`materialRevision` differs from `source`, and every variant that is
  neither the requested one nor valid. That is about 8 lines; `gpuMemoryStats()` then reports
  the drop.

**Measure:** RSS after opening `2930Colmap/project.mlp` in the 3D view (expect about -1 GiB).
Then switch the raster view between two photos and check per-frame CPU time in the status bar.
Then paint a selection on ESTE and check peak RSS.

**Size:** about +20 / -15.

**Done (2026-11-15).** +30 / -25 lines, comments included.
- **R2.** The 3D view needs only the camera to draw a frustum; the photo is decoded only for the raster view's backplate. On 2930Colmap (35 rasters, 4K), `--capture-window` peak RSS went from 2.55 GB to 1.38 GB and the run from 5.5-9 s to 2.8-3.4 s, with identical captures.
- **R3.** The RGBA8888 conversion moved inside the upload branch.
- **R4.** The selection buffers reserve vcglib's `UpdateSelection::FaceCount`/`VertexCount` instead of the whole mesh.
- **R5.** Fill and point variants at an older revision are released when the mesh is next prepared; current-revision variants stay, because split views may draw the same mesh with different color sources.
- R3, R4 and R5 were not measured: each needs interaction in the running app (switching photos, painting a selection on ESTE, switching color sources).

### Phase 3: uniforms uploaded before the pass (R1)

- Add one per-frame CPU staging buffer per dynamic UBO (main, decorator ×N, decorator-fat,
  selection, raster-projected, raster-backplate) next to the existing `DynamicUbufAllocator`s.
- Split each executor in `renderwidget_scene_passes.cpp`, `renderwidget_fill.cpp` and
  `renderwidget_selection.cpp` into two parts: a **prepare** walk over the plans that fills the
  staging slices and stores each item's offset in the draw item, and the existing **draw** walk,
  which only binds and draws.
- Before `cb->beginPass` (`renderwidget_render.cpp:716`), issue one `updateDynamicBuffer` per UBO
  into the batch `u` that is already passed to it.
- The offscreen passes (depth pick, masks, radiance-scaling pre-pass) do the same with their own
  `beginPass`.
- `uploadMainUbuf` then keeps `writeMainUbuf` and loses its per-call batch.
- The UV renderer (`renderwidget_uv.cpp:950-1383`) has the same pattern. Fix it in the same
  phase unless phase 6 is scheduled soon after.

**Reassessed before starting (2026-11-15).** Read against Qt 6.11's sources: every backend's
`resourceUpdate` asserts `recordingPass == NoPass`, so a **debug** Qt aborts on the first
in-pass upload. A release Qt draws correctly on all of them, because every in-pass batch here
holds only `updateDynamicBuffer` writes. Those are host writes that Metal and Vulkan queue and
apply when the buffer is next bound (`pendingUpdates`, `executeBufferHostWritesForSlot`), and
D3D and GL write them directly. No in-pass batch carries a texture or static-buffer upload, which
would be a real copy-inside-a-pass bug. The single-slot buffers rewritten at offset 0 inside the
pass (outline, mask debug view, solid-cut cap) are written once per frame or with identical data
per tile. The fix is larger than estimated: about 20 in-pass sites in six files (main pass,
depth pick, current-layer masks, radiance-scaling pre-pass, UV pass), each computing its
uniforms inside the draw loop, so each loop splits in two.

**Verify:** all render probes, plus `MESHLAB2_GRAPHICS_API=vulkan` and `opengl` on this Mac (see
the clipping-plane section of rendering.md), with the GPU timing preference on, to compare frame
times.

**Size:** about +60 / -70. This is the riskiest phase to get subtly wrong, so it gets its own
commit.

### Phase 4: collapse duplication (U6, R7, R8)

- **U6:** one `compactNodes(keepSorted, newRootOldId)` with a `std::vector<int>` remap. Used by
  `makeRoot`, `purgeBranch`, `linearizeHistory` and `pruneTreeToLimit`.
- **R7:** `Document::gpuCache()` plus `meshGpuSource(index)` and a `MeshGpuNeeds` struct. Delete
  the eight `*PassGpuView` forwarders and the `document.h` type re-exports, and stop including
  `meshgpuresourcecache.h` from `document.h`.
- **R8.1-6:**
  - one `worldCorners(const MeshEntry&)` helper used by the four bbox blocks;
  - `syncPerMeshRenderModesWithDocument` only from signals;
  - per-tile requests filtered from the frame request;
  - a `QHash` index in `Preferences::ensureLoaded`;
  - UV tick labels hidden on mode switch only;
  - the fallback-texture loop.

**Size:** about +80 / -330. Behavior unchanged; the existing suite covers it.

**Done (2026-11-15).** +309 / -735 lines across 25 files, comments included.
- **U6.** `subtreeNodes`, `compactNodes` (a `std::vector<int>` remap) and `rootAt` replace the four
  copies. Pruning by count now roots through `rootAt` as `makeRoot` does, so it clears the new
  root's label and action records too. Before, a script exported after a count prune could
  replay the action that produced the baseline.
- **R7.** `document_gpu.cpp` is deleted, with the eight forwarders, the 19-argument
  `ensureMeshGpuResources` and the eleven type aliases. `Document` keeps owning the cache and
  exposes it as `gpuCache()`; `document.h` no longer includes a `src/render` header. The nine
  `bool`s are `MeshGpuResourceCache::Needs`, and one `RenderWidget::ensureMeshGpu` builds the
  source. It always takes the quality range from the view's settings: the depth pick had passed
  only three of the five fields, so a scalar-colored point cloud was rebuilt for every pick and
  again on the next frame.
- **R8.1** The per-frame `syncPerMeshRenderModesWithDocument` calls are gone; `onLayersChanged`
  already runs it on every layer signal and undo restore. **R8.3** One `worldBoxCorners` (on
  vcglib's `Box3::P`) replaces five copies of the corner block, a fifth having been found in
  `computeVisibleSceneBoundingBox`, which is now a flag of `computeWorldSceneBBox`; the frustum
  depth is computed once per frame instead of once per raster. **R8.4** Preference descriptors
  are found by `QHash`. **R8.5** The UV tick labels are hidden on leaving the UV view, not every
  frame. **R8.6** The four fallback-texture uploads are one loop.
- **Not done.** R8.2: a tile's request skips every other mesh on an index comparison before it
  copies anything, so a tile copies one mesh's settings, not all; filtering the frame request
  would mean re-aggregating its flags for no measurable gain. The frustum vertex buffers are
  still refilled each frame: a few dozen floats per raster into a dynamic buffer.
- Phase 3 (R1) was skipped by decision; see the reassessment above.

### Phase 5: undo capture cost (U2, U1 small form, U5, U7)

- **U2, lazy "before" capture.**
  - `DocumentUndoManager::beginStep` stores only the label and script action and sets
    `m_beforePending`.
  - Add `Document::noteMutation()` (private, called at the top of every mutator listed under
    "APIs" in data_model.md, and by `markMesh*Changed`). It captures "before" when
    `m_beforePending` is set.
  - `endStep` with no capture commits nothing, but keeps the script action as a trailing record
    exactly as an informational filter does now.
  - Prerequisite: an exhaustive list of the mutators, built by grepping `m_meshes`/`m_rasters`
    writes in `src/core/document_*.cpp` and recorded in the commit.
- **U1 small form.**
  - Take `selectionRevision` out of the geometry key.
  - Snapshot selection bits as `shared_ptr<const MeshAttributeDelta>` (selection kinds only),
    interned by `(meshId, selectionRevision)`. Reuse `captureAttributeDelta`/`applyAttributeDelta`.
  - Restore applies them after the geometry.
  - Delta nodes still exist; this just stops them forcing copies.
- **U5:** `jumpToNode` restores the target's nearest full-snapshot ancestor once, then applies
  forward deltas.
- **U7:** incremental byte accounting in `pruneToMemoryBudgetInternal`.

**Measure:** re-run the whole section 0 table. Targets: `translate` after a selection, and
`measure_geometric_properties` after a selection, add a few MiB, not hundreds.

**Size:** about +120 / -60.

**Done (2026-11-15).** Measured with a temporary benchmark in the filter tests (Release,
`Document::runFilter`, resident memory from the kernel, undo bytes from `undoMemoryStats`):

| Mesh | Step | Before | After |
|---|---|---|---|
| RattoA_2M | `set_mesh_color` after `select_none` | +165 MiB undo copy | none |
| | `measure_geometric_properties` after `select_all` | +264 MiB resident | +1 MiB |
| | `translate` after `select_none` | +165 MiB undo copy | none |
| | whole sequence, undo history | 825 MiB in 5 copies | 495 MiB in 3 |
| ESTE_PRINT | `measure_geometric_properties` after `select_all` | +1074 MiB resident | +6 MiB |
| | `translate` after `select_none` | +758 MiB undo copy, 85 ms | +2 MiB, 32 ms |

- **U2 not done as designed.** The lazy capture cannot be hooked where the plan put it. Filters
  write into `doc.mesh(i).mesh` in place and announce it afterwards with `markMesh*Changed`, and
  `prepareMeshesForFilter` writes too (OCF components, normals) before the filter runs: no
  mutator call comes before the first write. Its measured cost was a cache miss caused by the
  selection revision in the geometry key, which U1 removes; the table shows it gone. What is
  left, a read-only filter after a color or scalar delta, goes with `colorRevision` in phase 6.
- **U1 small form.** A `MeshSnapshot` keeps its selection beside the geometry, as a
  selection-only `MeshAttributeDelta` interned by `(meshId, selectionRevision)`; the geometry key
  is `(meshId, geometryRevision, materialRevision)`. A restore writes the state's selection into
  every copied layer and into a kept layer whose selection revision differs, which also makes
  undo across a selection change keep the layer instead of copying it. The packed selection now
  includes the per-face edge flags (`FaceEdgeS`): the `FES` filters take full snapshots, and
  their edge bits used to travel inside the copy. `applyAttributeValues` is the delta apply
  without revision moves or signals, shared by the delta path and the restore.
- The cache hygiene in `restoreUndoState` dropped entries newer than the restored revision,
  states ahead of it included, so a redo followed by a new step made a second copy of a mesh the
  history already held. It now drops only expired entries.
- **U5.** `jumpToNode` restores one state: the target's, or its nearest full snapshot followed
  by the deltas below it. A walk made of delta steps alone is still replayed, as each is cheaper
  than a restore; until phase 6 a jump across a color delta and a full step copies the layer
  where the walk applied the delta.
- **U7.** `memoryStats` is built from a per-node `HistoryBytes` accumulator; pruning adds the
  current path from the tip back and roots once at the oldest full snapshot that fits, then
  measures once.
- `select_outliers` leaves the `outlierScore` attribute but moved only the selection revision;
  its undo was correct only because the selection key forced a copy. It now marks the material
  changed, as the other attribute filters do. The smoke sweep caught it.
- New tests: the next state after a selection shares the copy and every restore puts back its
  own selection; a full step undoes a face-edge selection; a jump across full and delta steps
  on two branches; a byte budget keeps exactly the newest states that fit.

### Phase 6 (large, ask first): data-model simplifications

- **R6:** raw quality in vertex buffers, range as a uniform; one range cache. Removes the four
  invalidation blocks.
- **R9:** the UV view draws a `UvLayout` variant of the shared cache. Removes most of
  `ensureUvMeshResources`. Needs R6 first.
- **U1 full form:**
  - `colorRevision`/`scalarRevision`;
  - every node a snapshot made of shared layers;
  - the `Delta` node kind and the `outputModifies`→delta table removed.

  Needs phase 5 first.

**Estimated size:** about -600 together, but each touches many files. Write a mapping table of
what moves where before starting, as AGENTS.md requires.

#### Mapping table (2026-11-15, awaiting approval)

Three commits in this order. R9 needs R6, because the colormap is the main difference between
the two caches. U1 comes after R9, so that the UV cache it deletes needs no new keys. Line
numbers are from the tree at `fe754200`.

**R6: raw quality in the buffers, the range as a uniform**

| # | Current | Target | Measured evidence | Verdict |
|---|---|---|---|---|
| R6.1 | Fill variant: range test `meshgpuresourcecache.cpp:277-318`, range computed `:363-406`, normalized into the buffer `:565-609` and `:919-938` | raw `cQ()` in slot 6, LUT flag −1 unchanged; no range state in the variant | 5 `quality*` fields in `FillVariantGpu` (`:127-131`); the shaders read only `v_meshColor.r` (`fill_flat.frag:121`, `fill_smooth.frag:110`) | change |
| R6.2 | Points variant: range test and compute `:1074-1160` | same as R6.1 | `PointsVariantGpu` `:156-160`; `overlay_points.frag:56` | change |
| R6.3 | `MeshSource::quality*` (`meshgpuresourcecache.h:46-50`), filled in `renderwidget_selection.cpp` | removed | 24 + 3 + 3 references in the cache `.cpp`/`.h` and `renderwidget_selection.cpp` | delete |
| R6.4 | The automatic range is computed from the mesh at 5 sites: fill, points, UV (`renderwidget_uv.cpp:370-392`), histogram (`renderwidget.cpp:3098-3170`), and `automaticQualityRangeForCurrentMesh` (`:243-297`, used by fixed-range seeding `:2066` and by Bake `:3425`) | one `RenderWidget` range function, cached by (meshId, geometryRevision, element, centerOnZero, crop); the histogram and both callers read it | each site gathers the values and calls `sampledRenderQualityRange` (`qualityrange.h:22`) | merge 5 → 1 |
| R6.5 | 15 shaders share the `buf` block, which ends at `clipRim` (offset 368, `kUbufSize = 384`) | `vec4 qualityRange` (min, 1/(max−min)) at offset 384, `kUbufSize = 400`, declared only by the 3 shaders that sample the LUT | grep `vec4 clipRim`: 15 files; LUT lookups: 3. std140 lets the other 12 omit a trailing member. The `ubufAligned` stride stays 512 at 256-byte alignment | change |
| R6.6 | `qualityrange.h` | stays | used by `colorize_vertices_by_scalar` (`colorprocfilterplugin.cpp:295-570`), Bake, `test_colormap.cpp` | keep |

Behaviour:

- **Unchanged except when the range clamps inside a triangle.** There the clamp moves from the
  vertices to the fragments, which is the correct result: normalization is affine, so it
  commutes with interpolation.
- **Non-finite values (decision D6-3).** Today they paint the middle of the LUT
  (`normalizedRenderQuality` returns 0.5). A shader cannot test NaN reliably under Metal fast
  math, so the CPU has to replace them when it builds the buffer.

Size: about +40 / −130.

**R6 done (2026-11-15).** Source and shaders +197 / −381; the 6 rows as mapped.

- **Buffers and uniform.** The buffers carry the raw scalar, flagged −1 per-vertex, −2 per-face,
  or 0 when it is not finite (D6-3). The fill and point variants hold no range state. The shaders
  map through `qualityMap` (scale, offset per element), written by `uploadMainUbufForMesh`.
- **One range.** `RenderWidget::qualityRange` and `sampledQualityRange` replace
  `automaticQualityRangeForCurrentMesh` and the four inline computations; the UV view keys its
  baked colors on the ranges it reads there.
- **Shader interface.** `tests/test_shader_interfaces.cpp` requires linked stages to declare the
  block alike, so `fill_flat.vert`, `fill_smooth.vert` and `overlay_points.vert` declare
  `qualityMap` too. R6.5's "std140 lets the others omit it" holds only for stages not linked to
  one of the three.
- **Duplicate invalidation removed.** Both settings handlers cleared the histogram cache by hand
  for fields its own key already compares; those blocks are gone.
- **Tests.** The new `tests/render_probes/scalar_color_map` measures:
  - a per-vertex 0.25 and a per-face 0.75 in [0, 1] on the gray map: 64 and 191 for fill, 64 for
    points;
  - the automatic range drawing pixel-for-pixel what the same fixed range draws;
  - a clamp saturating to white;
  - non-finite scalars drawing exactly the fill color.

  All 10 render probes and the 23 ctest suites pass.
- **Seen, not changed.** `uv_fill_texture.vert` declares a shorter anonymous `buf` than
  `fill_smooth.frag`, which it is linked with. The shader-interface test only compares named
  instances, so it does not see this.

**R9: the UV view draws from the shared cache**

| # | Current | Target | Measured evidence | Verdict |
|---|---|---|---|---|
| R9.1 | `ensureUvMeshResources` (`renderwidget_uv.cpp:187-674`, 488 lines) builds 5 fill variants, 3 point variants, wire, boundaries, seams, and selected faces and vertices, split by texture group | `MeshGpuResourceCache` products in a UV layout: positions `(u, v, 0)` from corner UVs, normal (0,0,1), key `(meshId, uvGroup)`; `uvGroup = −1` is the 3D layout | the 3D fill already has an expanded per-corner path (`expandTriangles`, `meshgpuresourcecache.cpp:361-408`) and the same 13-float stride (`:59` = `renderwidget_internal.h:75`); 3D boundaries and seams use the same vertex-pair edge map (`:1686-1840`) as the UV code (`renderwidget_uv.cpp:478-547`) | move into the cache, delete |
| R9.2 | `UvMeshGpu` (`renderwidget.h:1101-1147`), `m_uvMeshGpu` (`:1161`), `syncUvCacheWithDocument` (`renderwidget_uv.cpp:41-66`) | the cache's `purgeMesh` | 20 references in `renderwidget_uv.cpp`, 1 in `renderwidget_resources.cpp` | delete |
| R9.3 | The draw picks a `DrawRange` per product (`renderwidget_uv.cpp:1229-1386`) | the cache's pass views for `(meshId, activeGroup)` | only the active group is ever drawn (`:1019`, `:1229`) | simplify |
| R9.4 | UV-only drawing: background, texture quad, axes, unit box, tick labels, group list, fit/pan/zoom | stays | `renderwidget_uv.cpp:67-186`, `676-1018` | keep |

Behaviour:

- **Colormap.** The UV code bakes the colormap into its buffer (`renderwidget_uv.cpp:342-347`).
  After the move it samples the LUT, so changing the map no longer rebuilds anything.
- **Variants.** Any change today rebuilds all 8 color variants (`:269-274`, `:636-651`). The cache
  builds only the variant being drawn.
- **Faux edges (decision D6-1).** The UV wire draws every edge, while the 3D wire respects faux
  edges (`wireRespectFaux`, `meshgpuresourcecache.cpp:1018`).
- **Seams.** The UV code compares UVs only; the 3D code also compares the texture index. The 3D
  rule is taken.
- **Switching texture group.** Today it only switches the draw range. After the move, the first
  view of a group builds that group's faces.
- **Memory.** There is one shared copy instead of one per UV view.

Size: about +110 / −470.

**R9 done (2026-11-16).** `src/render` +428 / −716, net −288. The 4 rows were done, but not in the form the table planned.

- **Implemented differently.** Building `(u, v, 0)` positions inside every cache builder would have touched each of them. Instead, `RenderWidget::ensureUvLayout` builds the active group as a mesh of its own, laid out in UV space:
  - one vertex per vertex and UV pair;
  - the colors, scalars and flags of its source;
  - the UVs as wedge texcoords.

  The cache builds every product from that mesh unchanged, under its own id: high bit set, the group above the mesh id. The key is `(meshId, group)`, as R9.1 planned.
- **One cache change.** `MeshSource::vertexIdentity` lets the boundary and seam pass key edges by the layer's vertex and draw an edge on each of its sides. In 3D it is null, and the behaviour is unchanged.
- **Gone:**
  - `ensureUvMeshResources` (428 lines);
  - `UvMeshGpu` and `m_uvMeshGpu`;
  - the per-group draw ranges;
  - the UV view's CPU color map baking.
- **Selection.** A selection change alone copies the flags into the layout. The old code rebuilt every UV buffer.
- **Behaviour.**
  - Seams follow the 3D rule, which counts a boundary edge with UVs as a seam too.
  - An edge between the shown group and another group is a boundary of the shown group.
  - The wire is the 3D barycentric overlay and honors faux edges (D6-1).
  - Edges draws that wire in the edge style, as before.
- **Found on the way.** The UV view never uploaded the color map texture, because it baked colors on the CPU. The LUT refresh is now `RenderWidget::updateQualityColorMapLut`, called by both views.
- **Tests.** The new `tests/render_probes/uv_view` renders a gargoyle parametrized by xatlas in the UV view and checks:
  - the fill covers the charts;
  - a per-vertex 0.25 is a quarter gray over exactly that area;
  - select all tints the charts, and select none, a selection change alone, clears the tint;
  - the wire draws;
  - the seams outline the charts.

  All 11 render probes and the 23 ctest suites pass.
- **Not measured.** Speed and memory against the old UV renderer: its binary is gone with R6's commit.

**U1 full form: every node a snapshot made of shared layers**

| # | Current | Target | Measured evidence | Verdict |
|---|---|---|---|---|
| U1.1 | `MeshEntry` revisions geometry, material and selection (`document.h:69-73`) | add `colorRevision` and `scalarRevision`, moved by new `markMeshColorChanged` and `markMeshScalarChanged` beside `markMeshSelectionChanged` (`:406-413`) | color and scalar ride `geometryRevision` (`applyAttributeDelta`, `document_layers.cpp:1211-1213`) | new |
| U1.2 | 63 filters that change only color or scalar call `markMeshGeometryChanged` | call the new markers instead, call site by call site | descriptor sweep (`outputModifies` ⊆ {VC, FC, EC, VQ, FQ, EQ}, SingleMesh): colorproc 28, expression 6, igl 5, geodesic 4, unsharp 3, sampling 3, trueform 3, embree 2, mls 2, color_projection 2, img_patch_param 2, camera 1, texture 1, geogram 1. Those 14 plugins hold 114 geometry-marker calls in all | migrate by hand. A filter left unmigrated stays correct and costs a copy, so nothing trusts the descriptor |
| U1.3 | `MeshSnapshot::selection` and `selectionCache` keyed (meshId, selectionRevision) (`document_undo_types.h:81-83`, `documentundomanager.h:75-80, 110`) | selection, color and scalar layers on `MeshSnapshot`, in one cache keyed (meshId, kind, revision) | the phase 5 shape | generalize |
| U1.4 | The delta node kind: `UndoStorageKind` (`document_undo_types.h:219-225`), `storageKind`, `before/afterAttributes` (`:243`, `:258-261`) | gone; every node is a snapshot | | delete |
| U1.5 | The delta code in `DocumentUndoManager`: `beginDeltaStep`/`endDeltaStep` (`.cpp:210-248`), `pushDeltaStep` (`:888-928`), the delta branches in `endStep` (`:164-181`), `undo` (`:453-462`) and `redo` (`:493-501`), the delta walk in `jumpToNode` (`:401-425`), `clear` (`:533-534`), the root and prune guards (`:605-606`, `:733`, `:994-996`), `m_pendingDelta*` (`.h:120-122`), and the declarations (`.h:23-28`, `91-95`) | gone; a jump is one restore | line ranges read | delete |
| U1.6 | The delta entry points: two `beginUndoStep` overloads (`document_undo.cpp:22-37`), `applyAttributeDelta` (`document_layers.cpp:1200-1221`), the "Change Selection" step (`:1023`), and the `outputModifies`→kind table with its branch (`meshfilterpluginmanager.cpp:1038-1072`, `1097-1101`) | gone. `captureAttributeDelta` and `applyAttributeValues` stay, to build and restore the layers | no callers outside undo (grep) | delete |
| U1.7 | Code that learns of a color or scalar change through `geometryRevision`: the GPU fill/point keys (`MeshSource`, `meshgpuresourcecache.h:40-44`), the histogram (`renderwidget.cpp:3098`), the saved-file check (`document_io.cpp:727`, `761`), the metadata copy (`document_internal.cpp:395`) | add the two revisions | `geometryRevision` appears in 20 files; the others need geometry only (occluder, decorator counts, selection scope, xatlas, texture) | change |
| U1.8 | Memory reporting: `deltaBytes`, `pendingDeltaBytes`, `attributeDelta` (`document_undo_types.h:283-302`), the memory dialog rows (`mainwindow.cpp:3014-3040`, `3123`), the Make Root guard (`undographwidget.cpp:605`) | layers counted with the geometry, as phase 5 does for selection; the guard removed | | delete (decision D6-2) |
| U1.9 | Tests that assert delta storage: `test_document.cpp` 813-819, 3233-3285, 3310, 3389; `test_filters.cpp` 14691-14725, 15568-15570 | assert "no geometry copy" instead: geometry bytes unchanged across the step | grep | rewrite |

Behaviour and risks:

- **Memory.** A delta node keeps the before and after values; a layer keeps one set, shared by
  the states that have it. That is about half.
- **Cache miss.** A selection or color step whose "before" geometry is no longer interned (the
  history was pruned below it) now copies, where a delta did not. Measure this on ESTE after a
  prune.
- **Hand-incremented revision.** `filter_texture` increments `materialRevision` by hand
  (`texturefilterplugin.cpp:1033`, `1308`). It is harmless only because a fresh geometry
  revision follows at once; fold it into `markMeshMaterialChanged` while there.

Size: about +120 / −330.

**Totals:** 19 rows.

- R6, 6 rows: 3 change, 1 delete, 1 merge, 1 keep.
- R9, 4 rows: 1 move, 1 delete, 1 simplify, 1 keep.
- U1, 9 rows: 1 new, 1 migrate, 1 generalize, 4 delete, 1 change, 1 rewrite.

Estimated about +270 / −930, a net −660 (the review's estimate was about −600).

**Decisions (2026-11-15):**

- **D6-1, UV wire and faux edges: no decision needed.** Hiding faux edges is already the
  per-mesh "respect faux" setting (`PerMeshRenderSettings::wireRespectFaux`, on by default). After
  R9 the UV wire follows it as the 3D wire does.
- **D6-2, memory dialog: one "Attribute layers" row.** It counts the selection, color and scalar
  layers, and the selection moves back out of the geometry bytes. The row reuses `deltaBytes`.
- **D6-3, non-finite quality: the mesh's plain fill color.** The vertex carries the plain color
  flag instead of the LUT flag, so it shows as a gap rather than as an average value. For
  per-vertex quality the gap reaches about halfway to the neighbors. Bake to Vertex Color and
  Colorize by Scalar keep painting the LUT's middle (`normalizedRenderQuality`); they are filters,
  outside this change.

### Decisions needed before phase 1

- **D1: per-view visibility after undo.** Today a full undo effectively resets each view's
  hidden layers to the document's visibility (section 3, Q2 side issue). The options:
  - (a) keep that, now explicitly;
  - (b) keep each view's own visibility for layers that still exist.

  (b) is one line more and is what data_model.md's "each view keeps its own local visibility
  vector" suggests.
- **D2: how to measure undo.** The options:
  - (a) add `undo()`, `redo()` and `undo_history()` to `pymeshlab2.MeshSet`: about 20 lines in
    `src/python/bindings`, a public API that scripts could use;
  - (b) a throwaway timing harness in the scratchpad, never committed.
