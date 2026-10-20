# Layer Selection Model

This document proposes replacing two ideas -- a layer being *visible* and a layer being
*the current one* -- with one: a layer being **selected**, with one selected layer
distinguished as the **last selected**. It is a change to the data model first and to the
layer panel second, so it is meant to be decided whole and then done in small steps.
**Nothing here is implemented**; it is the mapping table the working agreements ask for
before anything moves, and the open decisions that table raises.

See also: [Data Model](../data_model.md) (what a layer entry holds, visibility sets, the
undo state), [Rendering](../rendering.md) (per-view visibility, the current-layer outline),
[Architecture](../architecture.md) (the layer panel), [Vocabulary](../vocabulary.md)
(names are approved there before they are used).

## Where it comes from

Two testing sessions raised it independently.

- **Tarini session, 10-11 September 2026** -- the *Layer selection model* section of the
  review, thirteen items sent by message after the call. The argument: adopt file-manager
  selection (click selects one and drops the rest, Ctrl toggles, Shift takes a range,
  Ctrl+Shift extends it, Ctrl+A selects all) because single selection is the common case;
  **merge selected and visible** into one set, the layers that are drawn; call the
  distinguished member *last selected*, which is also what a Shift range anchors on; retire
  the eye icons (his decision, separate from the semantics); give the current-layer outline
  an *Auto* mode that draws it only with several layers selected; mark the last-selected
  row in the panel with a frame and carry the 3D outline colours into the panel; outline the
  hovered layer in the 3D view; and offer a way back to the default colour pair. He calls
  the state "the active mesh is not visible" an interface error we have got used to, and
  the outline of an absent object built to show it a clever patch on a problem that should
  not exist.
- **Callieri session, 8 October 2026** -- 31:25 asks the same thing from the other side
  (selected rows are the visible ones, like a file manager, but he wants the visibility
  column kept, and if the two become independent he must be able to save and recall both);
  27:28 asks for the old visibility groups back, saved in the project; 38:32 cannot get rid
  of the current-layer outline, and notes that MeshLab always has a current mesh whereas
  Blender can have none; 25:39 finds the eye-icon modifiers (Shift, Alt, Ctrl) unclear;
  1:05:53 says "Apply to all" really means "to the visible ones" and should say so.

Already done on the way: named, saved, undoable *visibility sets* (the old groups); the
current-layer outline drawn only with several layers, and never in the grid arrangement.

## What is there today

Measured on 2026-10-16, not inferred.

- A layer's visibility is **per view**: `RenderWidget::m_meshVisibility`, a vector the
  view owns. `Document::MeshEntry::visible` is a proxy of the *current view's* vector, kept
  in step by `MainWindow` (`syncDocumentVisibilityFromCurrentView`, and the
  `meshVisibilityChanged` connection). Both are in the undo state (`MeshSnapshot::visible`).
  The project file does not store visibility.
- *Current* is `Document::currentMeshIndex` (and a current raster, and a current layer
  kind). It is independent of visibility, which is what allows a current layer that is not
  drawn.
- `Document::setMeshVisible` is not an undo step; the next step's snapshot carries it.
- Visibility sets (this month) are named lists of mesh ids that call `setMeshVisible`.

## Mapping table

Every reference to the layer visibility flag in `src` and `plugins`, found with
`grep -E "\.visible\b|->visible\b|meshVisible|setMeshVisible|onlyVisible|VisibleMeshes|visibleMesh|rasterVisible|setRasterVisible|meshVisibilityChanged|rasterVisibilityChanged"`:
**136 references in 32 files**. The verdict column is *rename* (same behaviour, a name
change), *same* (the merge changes nothing it relies on), *rethink* (the behaviour has to
be decided), or *retire*.

| Group | Where | Refs | Today | Under the merge | Verdict |
|---|---|---:|---|---|---|
| Per-view drawing | `renderwidget.cpp` (7), `.h` (3), `_selection` (4), `_frame_plan` (2), `_uv`, `_raster`, `_resources`, `_render` (1 each) | 20 | A view draws the layers its vector says; tiles, the scene bounds, rectangle pick and the UV and raster views gate on it | The vector is the view's selection. Drawing is unchanged | same |
| Document proxy and sync | `document_layers.cpp` (14), `document.h` (8), `document_internal.cpp` (1), `mainwindow.cpp` (3 of 4) | 26 | `MeshEntry::visible` mirrors the current view; `meshVisibilityChanged` moves it into the view | Renamed to the selection; the proxy and the signal stay as they are | rename |
| Undo state | `document_undo_state.cpp` | 9 | `visible` is restored with the layers | The selection is restored with them | rename |
| Layer panel eye and modifiers | `layerwidget.cpp` | 14 | Eye column; Ctrl / Alt / Shift click hide others, show others, invert | Replaced by file-manager selection in the rows; the eye column goes, or becomes a checkbox column (open decision 4) | retire |
| Apply to all | `meshfilterpanel.cpp`, `document_filters.cpp` (`runFilterOnVisibleMeshes`) | 3 | The panel's "apply to all visible layers" | "Apply to selected layers", which is what the label should have said | rename |
| Filters that act on the visible layers | `filter_camera` (16), `filter_colorproc` (11), `filter_plymc` (7), `filter_open3d` (5), `filter_layer` (4), `filter_icp` (3), `filter_cgal` (2), `filter_screened_poisson` (2), `filter_meshing` (1) | 51 | "Visible" decides which layers a filter reads or changes; several have a parameter `onlyVisibleLayers` / `OnlyVisibleMeshes` / `visibleOnly` | The same set under its new name; the parameters are renamed outright (no aliases) | rename |
| Raster-only filters | `filter_color_projection` (2), `filter_img_patch_param` (1); the raster uses inside `filter_camera` and `filter_layer` are in the row above | 3 | A raster's visibility chooses which cameras project | Rasters need a decision (open decision 6) | rethink |
| Saving | `document_project.cpp` (3), `mainwindow.cpp` save dialog (1) | 4 | "Only visible layers" leaves the others out of the project | "Only selected layers" | rename |
| Headless render | `headlessrendercontext.cpp` | 1 | A snapshot draws the visible layers | The selected ones | same |
| Python | `meshset_core.cpp`, `pymesh.cpp` | 5 | `visible` getters and setters on the set and on a mesh | Renamed outright: no alias for the old name | rename |
| Visibility sets | `document_layers.cpp` and `layerwidget.cpp` (counted above) | 0 more | Named lists of layers; show one and the others hide | **Named selections**: the same objects, with the same operations | same |
| Current-layer outline | `renderwidget_*` (not matched by the pattern: it keys on `currentMeshIndex`) | 0 more | Drawn around the current layer, even an invisible one; drawn only with several layers since October | Drawn around the last selected layer when more than one is selected; an unselected layer is never outlined, except by hover | rethink |
| Total | 32 files | **136** | | | |

The rows add up: 20 + 26 + 9 + 14 + 3 + 51 + 3 + 4 + 1 + 5 = 136. Only `mainwindow.cpp`
is split between two rows (3 + 1, by line).

## What the merge changes

1. **There is no hidden current layer.** The last selected layer is drawn. The outline-of-an-
   absent-object rendering loses its reason and goes, except as the hover feedback below.
2. **A filter's "current layer" is the last selected one.** With nothing selected there is
   no current layer, which every filter's applicability already has to handle (the framework
   does the check); this is the one place the merge can surface a filter that assumed one.
3. **Selecting is changing what is drawn.** Dragging a range in the panel changes the view.
   That is the point of the proposal, and it is why the file-manager semantics (a click
   replaces the selection) matter: with the old toggle-on-click semantics a casual click
   would hide a layer.
4. **Visibility sets become named selections**, and Alt+1..9 become "select this set".
5. **The selection stays per view**, as visibility is: each view has its own, and the panel
   shows the current view's, as it shows its visibility now.

## Open decisions

1. **The names.** One for the merged set -- Tarini notes *Active* may be free for it -- and
   one for its distinguished member (*last selected*). Settled once, in `vocabulary.md`,
   before any code uses them. *Recommendation:* **selected** and **last selected** in the
   interface; `selected` in code. *Active* stays unused so that nothing old reads as new.
2. **May the selection be empty?** Blender's can. Old MeshLab always had a current layer.
   *Recommendation:* yes, empty is allowed, and filters that need a layer refuse with their
   usual message.
3. **Selection semantics.** File manager, as above, on macOS with Cmd for Ctrl.
   *Recommendation:* adopt it without a preference.
4. **The eye column.** Retire it (Tarini), keep it (Callieri), or turn it into a checkbox
   column as in recent Windows Explorer, optional, which Tarini himself calls possibly
   overkill. *Recommendation:* retire the eye, offer the checkbox column as an optional
   column of the table view only, off by default, like `#` and `ID`.
5. **Rows kept.** The *last selected* frame colour, the hover colour and the outline colours
   as one pair of preferences with a restore-defaults button. *Recommendation:* as Tarini
   asks, and stored in the preferences, not per project.
6. **Rasters.** A raster's visibility chooses which cameras project and which show in raster
   mode. Folding it into the same selection is the natural reading, but a raster is not a
   mesh and the raster views have a current plane of their own. *Recommendation:* decide
   this last, after the meshes work; until then the raster eye stays.
7. **The project file.** Visibility is not stored today. A saved selection would make a
   project open as it was left. *Recommendation:* store the selection per view only once
   named selections (the visibility sets) have lived a release; they already cover the need.

## Staging

Slow, each step shippable on its own and each leaving the program consistent. None of the
first three changes the data model.

1. **Settle the vocabulary** (decisions 1-3) and approve this table. No code.
2. **The outline.** An explicit three-state option, *Auto* by default and on or off
   otherwise, with the colour pair and its restore-defaults button; the hover outline in
   the 3D view; the hover and last-selected frame colours in the panel. Independent of
   everything below.
3. **Selection in the panel, with the model unchanged.** File-manager selection of rows in
   the tree and the table, a frame on the current row, and the checkbox column as an
   option. The selection is the panel's own; "Apply to selected layers" acts on it, and the
   eye is untouched. Two concepts coexist for a moment, deliberately, so that this step can
   be judged on its interaction alone.
4. **The merge.** The panel's selection becomes the view's visibility: selecting shows,
   deselecting hides. The eye column is retired. "Apply to all visible" and its relatives
   are renamed (the table above), the Python names with them.
5. **The current layer.** `currentMeshIndex` becomes the last selected layer; an empty
   selection is allowed; the invisible-current-layer outline goes.
6. **Rasters**, if decision 6 says so.
7. **Retire the old words**: the `visible` flag, `onlyVisible*` parameters and
   `setMeshVisible` take their new names in one commit with the documentation, as the
   project does with a rename: outright.

## Not in scope

Camera views stored alongside layer sets (the old W1-W4), per-layer opacity, and layer
groups as a tree: each is its own question, and none is needed for this one.
