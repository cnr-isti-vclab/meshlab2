# User Interface

How the panels, gestures and interactive tools behave: what the user sees and does. The structures behind them are in [Architecture](architecture.md) and [Rendering](rendering.md); the history tree itself is in [Data Model](data_model.md#undoredo-model).

## `LayerWidget`

Layer dock with two synchronized presentations over the same `Document`: a detailed tree view and a sortable table view. Mesh rows expose visibility, the stored-state labels of [Layer Provenance](data_model.md#layer-provenance), counts, texture/material/attribute summaries, and current-layer selection. Raster rows expose visibility, active raster selection, current plane, plane export, image size, plane count, and camera summary. Every column of the table but the eye, the name and a raster's thumbnail can be hidden from a right-click on the header, and the choice is remembered (`QSettings`, `layers/meshTableColumns` and `layers/rasterTableColumns`). Two columns are off until asked for: `#`, the layer's position in the document counting from 0 -- the order of the tree view and the index a script uses -- and `ID`, its persistent id (`MeshEntry::meshId`, `RasterEntry::rasterId`), never reused. Sorting by `#` restores the tree order. A layer row of the tree can be dragged to another place in the list (`LayerTreeWidget`, for meshes and for rasters): the drop becomes `Document::moveMesh(from, to)` (`moveRaster` for a raster), one undo step, which changes the `#` and nothing else -- the id, the current layer and the layer's settings stay with it -- and emits `meshMoved` (`rasterMoved`), on which a view moves its per-layer visibility. The order is the layer list's: grid arrangements and a saved project follow it. The title bar holds two 22-px buttons placed over the tree's two narrow columns: the collapse-all toggle over the disclosure arrows and, to its right, one **eye** over the visibility check marks (`LayerWidget::disclosureArrowCenter()` and `visibilityCheckCenter()` say where the columns are, from the style's own geometry, and `MainWindow` spaces the buttons by them). The eye shows every layer, or hides every layer when they all are shown (`toggleAllVisible()`; open when all are shown, closed when none, faded between, drawn from `layerVisibilityChanged(shown, total)`; meshes and rasters both). Each layer row carries only the style's own small check mark for its visibility, with the Ctrl / Alt / Shift modifier clicks on it as before (hide others, show others, invert). Names are edited in place: a layer's name in the tree or in the table's name column (double-click or the edit key), and a texture's name by double-clicking its row in the tree, which puts a line editor where the name is (Escape cancels). The panel never writes the name itself: it makes that layer current and asks the main window to run *Rename Current Mesh Layer*, *Rename Current Raster* or *Rename Texture* (`LayerWidget::renameLayer`, `editTextureName`), so a rename is an action in the history and a line in a generated script. An empty or unchanged name runs nothing and puts the old one back. A mesh row's right-click menu puts what is done to that layer first, and its removal last: *Rename* and *Duplicate* (Ctrl+D, an application shortcut on the current layer, like Alt+1 .. Alt+9 below); *Save Mesh...* and *Reload*, which the main window runs as its File menu does (`saveMeshRequested`, `reloadMeshRequested`); *Set Random Layer Color* and the visibility-set submenus; the other *Document/Layer* filters that work on one mesh, then those that take the layers together, each group by name; and *Remove*. The filters act on the current mesh, so the row is made current first; raster filters are left out. Off a row the menu lists every layer filter, as before. A texture row's menu has *Rename*, *Set Texture from File...* -- *Set Texture from File* on that texture alone, whose `texture` parameter (0, all of them, by default) replaces one image and keeps the other textures -- *Save Texture...*, and *Remove*, which runs *Remove Textures* on that texture alone. Under the layers a row of **visibility set** chips (see [data_model.md](data_model.md)): a "+" keeps the layers visible now under a name, and each set is a chip that shows exactly its layers in the current view (Shift+click adds them to the visible ones; right-click renames, replaces the set with the visible layers, or deletes; hovering a chip previews the set by shading its layers' rows, which the row delegate paints rather than setting item backgrounds, so nothing is re-sorted or re-reported). A layer's right-click menu has *Add to Visibility Set* and *Remove from Visibility Set* for editing a set one layer at a time; the chip of the set that is on screen is highlighted, and Alt+1 .. Alt+9 show the first nine from anywhere.

Mesh rows show **Saved**, **Modified**, or **Not saved** in a trailing status area, separate
from the name. The tree delegate reserves that space before eliding a long name, and
uses the style's selected-text color when the row is current. The mesh table's **Status**
column shows the same words and keeps the previous source column's position and width
preferences. Texture and raster-plane metadata begin with the same explicit labels;
hover text retains the save path. Stored-state rules are in [Layer Provenance](data_model.md#layer-provenance).

## `MeshFilterPanel`

Filter browser/runner: search box, parameter form from `MeshFilterDescriptor`, optional markdown description, advanced-parameter toggle, reset-to-defaults button, per-filter parameter-value cache, a "?" *details* button that opens the filter's long description and puts each parameter's help under its row (hidden, the help is the row's tooltip: rich text so that it wraps, markdown spans rendered, with the range and default added; the button's state is remembered per filter in QSettings under `filterPanel/details/<filter key>`, kept only while on, default off), current-view providers for camera/render-state JSON parameters, and, when Python support is compiled in, compact/full Python call formatting shared with action-history export.

### Filter search

`FilterPresentation` (`src/ui/filterpresentation.{h,cpp}`) holds the matching, so the filter
panel and the Filter Plugins Info dialog agree on which filters a query finds; the panel also
orders them by `rank()`, while the dialog keeps its own grouping by plugin or category.
The preferences dialog and the parameter search still match by plain substring.

**Matching.** Text is split into lower-cased words at anything not a letter or a digit, so
`remove_textures`, `Texture/Assignment` and `Per-Wedge` all split; the query is split the
same way, and every term must match somewhere. A term matches a word it **starts**
(`vert` → *vertex*), or a word it reaches by **word form** -- one common ending (-ing, -ion,
-es, -ed, -e, -s) dropped while at least four letters remain, so `decimate` finds
*decimation* -- or, if it has **three letters or more**, the targets of a **search alias** it
starts (`del` starts *delete*, which reaches *remove*). Two-letter terms never expand. A
match **inside** a word is the last resort: it still finds the filter, ranked last.

**Aliases** run one way, from the word a user types to the word the names use, never back:
`position` reaches *coordinates*, but `coordinates` does not reach *position*, which would
flood a position search with texture coordinates. The verb aliases are the "Rejected
synonyms" column of [vocabulary.md](vocabulary.md) §3, and the noun aliases its "Search
aliases" section; the table is `FilterPresentation::searchAliases()`, and
`searchAliasesCoverTheRejectedSynonyms` fails when a rejected synonym has no alias. Some are
left out on purpose, as the test's listed exceptions: `copy`, `map`, `push`, `pull` and
`apply`, each common enough in another sense to flood a search, and `Bin` for *pack*, which
would match *binary*.

**Ranking.** Each term scores its best match, and the filter takes the level of its worst
term: (1) every term starts a word of the name; (2) some only through an alias, still in the
name; (3) in the tags, categories, Python name or plugin; (4) in the descriptions; (5) only
inside a word. Within a level, exact words beat prefixes, which beat word forms; then
matches earlier in the name, and in the typed order; then names with fewer extra words; then
registry order, so the list is stable. `searchRanksTheReferenceQueries` in
`test_filter_descriptors.cpp` pins the first result of 24 reference queries (`set tex`,
`del tex`, `vert pos expr`, `fill holes`, `load photo`, ...), and checks queries whose best
answers differ only in backend (`quadric`, `icp`, `ball pivot`) as a group, in any order.

## `RenderOverlayPanel`

Compact pass/settings panel: pass toggles, mode-specific world settings page (Scene vs UV), per-pass style controls (colors, color sources, widths, lighting/culling, quality histogram), PBR texture/normal-space controls, decorator info overlay toggle, and `PerMeshRenderSettings`/`GlobalRenderSettings` sync. Explicit-edge color choices are constrained by the current mesh's vertex/edge color mask. Every page opens under a one-line title row: the pass's name (see the pass-bar table in [vocabulary.md](vocabulary.md#the-views-pass-bar-2026-09-29)) and, for the eight per-layer passes, one shared *Apply to all* that copies the page's settings to every visible layer — the same `applyToAllMeshesRequested` a shift-click on the pass button sends. Every button that acts rather than sets — *Apply to all*, and those that hand the view's state to a filter or pin it (*Bake to Vertex Color*, *Trim Current Layer*, *Freeze to View*) — is made by `makeActionButton` and shares its small style. A page stays open until its arrow is pressed again, so it never closes under someone reading a help tooltip or working in another window. Pages are as tall as their own content: the stack declines height-for-width, because `QStackedLayout` answers it with the tallest page's minimum height.

## `Scene3D` Camera and Interaction

The `ViewTrackball` gestures:

- **Rotate**: left drag, arcball/hyperbola.
- **Pan**: middle or right drag, or `Ctrl+Left`.
- **Roll**: `Shift+Left` drag turns the view about its own axis by the angle the cursor sweeps about the centre of the viewport. Nothing turns within 32 pixels of the centre, and a layer tool that is live takes the drag first, as it does every left drag.
- **Dolly**: wheel. **Vertigo**: `Shift+Wheel` changes the field of view with a compensating dolly, and past the narrowest field of view goes on into orthographic and back.
- **Projection**: `5` or View > Orthographic Projection toggles the projection of the current 3D view; the `view.projection` preference sets where a new view starts.
- **Make a layer current**: a plain click (left button, no modifier, released within the drag distance) makes the layer under the cursor current (`PickPurpose::CurrentLayer`, the same GPU pick as the double click's). It is decided on release, so that a press that becomes a drag stays a rotation, and run only once the double-click interval has passed with no second press, so that a double click recenters without changing the current layer. A click on the background changes nothing, one that stops a camera animation picks nothing, and a press in a view that is not current only makes that view current, here and on a tile of the layer grid. A change of layer is said briefly over the view. A raster's camera is a few lines, too thin for the GPU pick, so before it the click is tested on the screen against the frustums the view draws (`RenderWidget::rasterFrustumAt`: the segments of `rasterFrustumSegments`, the ones uploaded for drawing, placed with `projectToWidget`); within 6 px of one, the nearest raster is made current and no mesh is picked. Not in the layer grid, which draws no cameras, nor while *Show Raster Cameras* (`show_raster_cameras`, beside *Show View Cameras* in the 3D view settings) is off, which hides every raster's frustum at once -- the layer panel's eye hides one.
- **Recenter**: double click depth-picks the point under the cursor and animates the orbit centre to it.
- **Headlight**: `Ctrl+Shift+Left` rotates the view-space headlight, the light following the cursor as the model does under the trackball. While dragging, `LightGizmo` shows the light in the bottom-left corner: a ball lit as the current layer's fill is, under its back-lighting mode (Single, Double or Fancy, with the fill shader's ambient, sided diffuse and tints), the light as a small ball 1.5 radii out with the ray it comes along from the big ball's surface (both drawn fainter where the big ball occludes them, and the ray and the small ball occluding each other, as 3D objects would, so a light behind the model does not read as its mirror in front), and its azimuth and elevation. It is a `QPainter` child widget like `ViewAxisGizmo`, held for a moment after release and then faded; the `view.lightGizmoSize` and `view.lightGizmoLinger` preferences set its size and how long it stays, read each time it is shown.
- **Clipping plane**: `Ctrl+Wheel` slides it and `Alt+Left` tips it (see [Clipping plane controls](#clipping-plane-controls)).
- **Axis snaps**: a click on an axis of the corner `ViewAxisGizmo` (sized by the `view.axisGizmoSize` preference) snaps the camera orientation to it.

Gizmos are depth-aware and scale-stable across dolly/FOV changes. `MainWindow` can synchronize the camera across 3D views, and offers *Center on Selection* and *Frame Scene from Here*.

**Frame Scene from Here** (`Ctrl+Shift+F`, `ViewTrackball::frameFromEye`) frames the vertices of the visible layers exactly without moving the eye: it turns the view axis about the eye until their extent on the image plane is centred -- by half the angle it is off centre, as aiming at the middle of the extent on the image plane overshoots by 1 + x² and diverges in a wide view -- keeping the old up direction, then sets the field of view to the tighter of height and width. The trackball gizmo keeps its size on screen, as through a zoom: re-basing it on the scene's half-diagonal, as a reset does, makes a sphere that a view framed this tightly cannot hold. It is animated (`startCameraAnimation(target, aboutEye)`, which the axis-gizmo snaps share): the rotation slerps, the field of view changes by equal ratios as a zoom does, and the orbit centre is recomputed from the fixed eye every frame rather than interpolated, which would let the eye wander off and come back. It does nothing, saying why in the status bar, in orthographic, when a point is behind the eye or nearer than the near plane would be, or when the field of view would leave the trackball's 10°–120° range.

## `ParametrizationUV` Camera and Interaction

Orthographic projection with a pan and zoom of its own (`m_uvPan`, `m_uvZoom`), which undo does not restore. Left/middle drag = pan; wheel = zoom around cursor; double click = centre on the clicked point and zoom in; `Reset Camera` = fit to the mesh's UV bounds (or `[0,1]²` when `uvShowFullTexture`). When `Rubber-band Select` is active, the same view coordinates are converted to UV-space filter parameters so rectangle selection can operate directly in the parametrization view.

## Clipping plane controls

**Interaction.** `Ctrl`+wheel slides the plane along its normal at 0.02 diagonals a notch;
`Alt`+drag tips it, switching the axis to `Custom`. Both enable clipping if it is off, and
the wheel starts it at `ClipPlane::offsetClearOfScene()` — the offset where the plane just
touches the scene — so the next notch cuts. The near-plane ratio, `view.nearClipRatio`, is a depth-precision
setting in the preferences, not a gesture. The controls also live behind the
`RenderPass::ClipPlane` button on the pass bar.

**Making the cut real.** The clip-plane page's *Trim Current Layer* button runs
`meshlab2.filter.meshing::trim_surface_by_plane` on the current layer -- or, when the layer
is a bare point cloud (vertices, no faces or edges),
`meshlab2.filter.meshing::trim_point_cloud_by_plane`, which drops the points behind the plane
and has no cut to close -- and then switches the clipping plane off, so what is on screen
becomes the geometry rather than a view of it.

The plane is handed over as a normal measured from the origin rather than copied across as
the settings: the view measures its offset in diagonals of the whole visible scene while the
filter measures a distance against one layer's bounding box, so the same numbers would mean
different planes as soon as there is more than one layer. And it is the plane in the layer's
own coordinates (`ClipPlane::toLocal`, what the shaders clip with), because the filters work
in those and ignore the layer matrix; the world plane would cut a different slab once the
layer has been moved. It matches what the viewport shows -- one layer, and the cut closed when
it is shown as solid (`closeCut` follows `clipPlaneSolidCut`); the Filters panel has the rest
of the filter's options.

## Interactive Tools

`createBuiltinInteractiveTools()` makes the built-in tools, and `MainWindow` owns them and their toolbar and menu actions. A tool is pinned to its owner view and commits each durable change as one filter run, so each is one undo step and one recorded script call (the measuring tool's *Export Layer* is the exception: it adds its layer directly, in an undo step of its own); the contract a tool implements is in [Rendering](rendering.md#interactive-tools).

The Edit Tools toolbar uses one filled icon family on a 24-unit grid, with the existing
mesh-face palette and solid triangular arrowheads. Selection shows flat gray faces with
the renderer's 50% red overlay on faces whose centroids lie inside the marquee. Its cursor
tip covers the marquee's lower-right corner and keeps a black border in both themes. Raster alignment
shows a photo, a cube and a floating arrow with clear gaps at both ends. Snapshot uses a
camera and remains separated from the four modal tools. The editable masters and their
runtime PNG exports share the following stems in `img/`:

| Action | Asset stem | Symbol |
|---|---|---|
| Rubber-band Select | `tool_select_rect` | Dashed marquee and pointer |
| Measuring Tool | `icon_measure` | Diagonal ruler |
| Transform Layer | `axis` | Directional axes and rotation arc |
| Raster Alignment Tool | `icon_raster_align` | Photo, cube and floating arrow |
| Snapshot PNG | `snapshot` | Camera |

The original artwork is preserved as archival copies under `_legacy.png` names;
the active resource paths above continue to identify the replacement artwork.
The originals were recovered byte-for-byte from Git, rather than from the already
replaced working-tree files. Inventory measured with `git ls-tree -l HEAD` and
resource usage checked against `CMakeLists.txt` and the tool action sources:

| Original asset | Preserved copy in `img/` | Original bytes | Verdict |
|---|---|---:|---|
| `tool_select_rect.png` | `tool_select_rect_legacy.png` | 3,537 | Preserve; active path retained |
| `icon_measure.png` | `icon_measure_legacy.png` | 3,860 | Preserve; active path retained |
| `axis.png` | `axis_legacy.png` | 1,096 | Preserve; active path retained |
| `icon_raster_align.png` | `icon_raster_align_legacy.png` | 9,907 | Preserve; active path retained |
| `snapshot.png` | `snapshot_legacy.png` | 3,553 | Preserve; active path retained |
| **Total** | **5 archival copies** | **21,953** | **5 active paths retained; 0 originals discarded** |

The five existing PNG resource paths stay unchanged. Their 96 × 96 exports provide
enough resolution for 32-pixel icons at 3× display scaling without a Qt SVG dependency.
`MainWindow::updateToolIcons` chooses the matching `_dark.png` variant when the toolbar's
button palette is dark. The dark artwork has brighter edges and arrows while preserving
the cursor's black border. A toolbar `PaletteChange` refreshes the shared toolbar/menu
actions; Qt supplies disabled/selected icon treatment and checked-button backgrounds.
Every dark PNG also has an editable `_dark.svg` master.
Regenerate the PNGs after editing the SVG masters with ImageMagick:

```sh
for icon in tool_select_rect icon_measure axis icon_raster_align snapshot; do
    for suffix in "" _dark; do
        magick -background none -density 384 "img/$icon$suffix.svg" -resize 96x96 -strip "img/$icon$suffix.png"
    done
done
```

**The tool strip** (`ToolStrip`, `toolstrip.h`) is how an engaged tool shows itself: a bar at the bottom centre of its owner view (above the texture chooser in the UV view) with the tool's name, a line of text, a button for each of the tool's options and actions, and two buttons every tool has, *Camera* (`Tab`) and *Exit* (`Esc`, which reads *Cancel* while a gesture is under way, since `Esc` then cancels it). Each button shows its key as a keycap and its purpose in the tooltip; pressing the key lights the button. The border is the accent colour while the tool has the mouse and grey while it does not (Tab, another view current), and in a view that does not take the tool (the layer grid, an unsupported mode) the buttons are greyed and the text says so. The text is the tool's `statusHint()` -- what the mouse does, in a few words -- or, while it works, its `statusDetail()` (the transform readout, the measurement count). On a narrow view the groups of buttons wrap onto more rows and the text is cut short, whole in its tooltip.

The built-in tools:

- `Rubber-band Select` (`RubberBandSelectTool`): drag a rectangle in Scene3D or UV mode, then run `meshlab2.filter.select::select_by_screen_rectangle`. `Shift` adds, `Ctrl` subtracts, *Faces* (`F`) and *Vertices* (`V`) choose what is selected, *Visible Only* (`B`) keeps only the faces visible from the viewpoint, and *Components* (`C`) and *UV Islands* (`T`) grow what the rectangle touched to whole connected components or whole UV islands (the filter's `expand_to`). *Lasso* (`P`) switches to a polygonal lasso: click the corners, close it with a double click, `Enter` or a click on the first corner (`Backspace` takes back the last corner, `Esc` drops the polygon, and `Tab` is held back while one is open); it commits `select_by_screen_polygon`, whose polygon is a JSON array of normalized `[x, y]` points tested even-odd, and the modifiers and the other keys apply to it as to the rectangle. The tool passes either camera-state JSON or UV pan/zoom parameters depending on the active view mode. In the 3D view the filter projects through the layer's matrix (`MeshEntry::transform`), and casts the visible-only rays from the eye -- or along the view direction, orthographically -- mapped into the layer's own coordinates, so a moved, turned or stretched layer is selected where it is drawn (`screenSelectionFollowsTheLayerMatrix`, `visibleOnlySelectionLooksThroughTheLayerMatrix`); the selection overlay is drawn through the same matrix.

  Six more controls edit the selection already made, each by running one ordinary selection filter (so each is one undo step and one recorded script call) on whichever of faces or vertices the tool is picking: *All* (`A`), *None* (`N`), *Invert* (`I`), *Dilate* (`+` or `=`), *Erode* (`-`), *Remove* (`X`). Remove removes the selected faces together with the vertices no face uses any more (`remove_selected_faces_and_vertices`) in face mode, and the selected vertices with their faces (`remove_selected_vertices`) in vertex mode. The key-to-filter mapping lives in `tools/selectionshortcuts.h` and passes the kind explicitly, because the filters' own defaults act on both kinds (`select_all`, `select_none`), on whichever kind already has something selected (`invert_selection`, which would do nothing to an empty selection) or, before `dilate_selection` and `erode_selection` took an `element`, from faces only. They are disabled while a drag or a polygon is in progress, and None, Dilate and Erode skip an empty selection rather than leave an empty undo step. They apply in the UV view too, where selection is the same selection.
- `Measuring Tool` (`MeasureTool`): click surface points in Scene3D to build/edit measurement segments. It uses asynchronous surface picking for placement and drag previews, draws labels and depth-cued segments, *Undo* (Backspace) takes back the point just placed or the last segment, *Clear* (`C`) removes them all, *Print* (`P`) writes them to the log, *Save* (`S`) saves a TSV, and *Export Layer* (`X`) adds them as an edge-only mesh layer; the strip's text says how many there are and which point comes next.

  Each length, in the label, the log and the saved table, carries the document's unit (`Document::unit()`) once it is set -- "12.5 mm", and `length_mm`, `ax_mm`... as the table's column names -- and plain numbers while it is unknown; the view redraws when the unit changes (`RenderWidget` follows `unitChanged()`). *Scale to Length* (L) brings a layer to its real size from the last measurement: it shows the form of the *Scale to Known Length* filter (`scale_to_known_length`), less the two points, which are the measurement's, and runs it on the layer the points were picked on (the tool keeps each point's layer id; a segment between two layers presets *Scale all layers*). The filter returns the factor and the center it used (`outputValues`), and the tool moves the points on the scaled layers with them, so the label reads the length just given.
- `Raster Alignment Tool` (`RasterAlignTool`): pairs points of the mesh with pixels of the current raster's photo, in the raster view, which engaging it switches to when the raster has a camera. A left drag goes from a feature where the mesh is drawn (a surface pick through the raster's camera) to where the photo shows it; a click pairs a point already in the right place; the middle button pans. Its cursor carries a small *3D→2D* to say so, and becomes a move cursor over an end of a pair: dragging the photo end (an orange square with a cross, flat on the image) moves its pixel, dragging the mesh end (a cyan disc lying on the surface, with its normal, drawn faint when the surface faces away) picks the pair's point again where the drag ends, on the surface, and `Esc` during the drag puts the end back. The photo end is taken first where the two coincide, as after a fit. The view refreshes a tool's cursor on every move it takes (`applyToolCursor`), so a cursor can follow what is under the pointer. Each pair is drawn as a line from where the camera projects its point to its pixel, which a fit shrinks to that pair's error. Two loupes in the top corner magnify what is being placed, four times the view by default (the `view.loupeSize` and `view.loupeMagnification` preferences set their size and magnification): a *3D* one with the layers alone and a *2D* one with the photo alone, each with a sight at the point it magnifies. Hovering shows the 3D one at the pointer, where a press would pick; during a drag both are shown, the end being placed under the pointer and the pair's other end beside it, so the two can be compared. *Fit* (`F`, Enter) commits *Align Current Raster to Mesh by Point Correspondences* once, *Mutual Information* (`M`) refines the camera by *Align Current Raster to Mesh by Mutual Information* with the pairs as point pairs (or without, when there are none), *Focal Length* (`L`) lets either change it, *Delete* (Backspace, Delete) removes the pair under the cursor, or the last one, *Clear* (`C`) removes them all, and *Save* (`S`) and *Load* (`O`) write and read them as the filter's JSON. Pairs are kept per raster (by `rasterId`) while the tool is engaged; a raster without a camera greys the controls and says so. How the filters behind it work is in [Raster Alignment](raster_alignment.md).
- `Transform Layer` (`TransformTool`): modal, Blender-style layer transform tool for the current mesh. *Move* (`G`), *Rotate* (`R`) and *Scale* (`S`) start translate/rotate/scale gestures, drawn with depth-cued constraint guides; *X*, *Y* and *Z* constrain to an axis, `Shift` plus an axis constrains to the perpendicular plane, typed numbers set exact values, and *Apply* (Enter) or a click commits. The readout is the strip's text. A gesture started from its button, with the pointer on the strip, starts from where the pointer first moves in the scene. During preview it updates the layer matrix directly, then restores the original matrix and commits exactly one transform filter so undo/script history remain clean. The tool exposes the `:/img/axis.png` toolbar icon.

`Rubber-band Select` is the only tool that works in `ParametrizationUV`, and `Raster Alignment Tool` the only one that works in the raster view (`supportsUvView()`, `supportsRasterView()`).

## The Action History view

`UndoGraphWidget` draws the tree as rows, newest at top, laid out by `UndoGraphLayout` (`src/ui/undographlayout.h`), which is pure and tested in `tests/test_undographlayout.cpp`. Two kinds of repeat fold into one row, drawn with a strip of dots under the label, one per state:

- **Tries** — siblings made by the same filter: the sweep left by undoing a filter and running it again. Only tries that went nowhere fold. A try that was continued keeps a row for its children to hang from, and when exactly one was, the others fold into its row.
- **Runs** — a chain of the same action, as an interactive tool leaves it, each step carried on by its oldest child made by that action. A branch does not split the run: the row that leaves it hangs from the step it left, whose dot carries a notch, and takes a column of its own even when it is the run's only branch, so it cannot pass for what came after the last step. Hovering the branch enlarges that dot; hovering the dot lights up the branches and names them in the caption. A run's dots are joined, solid along the path to the current state and dashed, muted, where it does not come from them; tries stand apart.

When siblings of one action hang from a step made by that same action and the oldest of them was continued, that one carries on the run and only the others can fold as tries: undoing two selections and selecting again is a branch off the run, not a sweep. A sweep of leaves on top of the same filter still folds as tries.

"The same action" is the same filter key whatever its parameters, or the same label for anything that is not a filter. When exactly one parameter differs between the states, each dot carries its value and the strip starts with that parameter's name. Hovering a thumbnail or a dot shows the state's snapshot, captioned with the try or step and, up to three, the parameters that differ. Where it shows is the `view.historyPreview` preference: by default over the 3D view itself (`RenderWidget::showStatePreview`), filling it inside a cloudy frame and under a *Preview* label so it cannot pass for the live document, which stays untouched underneath; or in a popup beside the panel. Camera and render states are left out of the comparison: they say where a screen-space tool acted from, not what was chosen. Clicking the count, or the `+N` standing for older states that do not fit, expands a group into a row per state.

*Update camera* stores the current view, camera and look alike, into a state and retakes its row thumbnail and hover snapshot. On a folded row, right-clicked anywhere but on a dot, it does so for every state the row holds. Only the live document can be photographed, so `MainWindow::updateUndoNodeCameras` visits each other state with `jumpToUndoNode(id, false)`, puts the live view back over the look the state was recorded with, grabs the frame, and returns to where it started. A visit's own refreshes of the filter UI and of the panel are skipped, since the document ends where it started. A state that differs from the live document in its selection, colors or scalars alone costs almost nothing to visit; one with other geometry restores a copy.

Columns are assigned on the folded rows by the rule the history has always used: a row's oldest child continues its column, and each later one opens a new column, in creation order. The undo model stores no display lane. The panel's images and expanded rows are kept by node serial ([Data Model](data_model.md#undoredo-model)).

## Resetting the views

*File > New* closes the layers, the undo history and the log, and then puts everything else the
user could have changed back as a fresh start has it: `RenderWidget::resetToDefaults()` on the
remaining view (render settings seeded from the preferences again, per-layer render modes dropped, default
camera and projection, headlight, 3D mode, UV and raster views refitted), the active tool, the
camera synchronisation, and the values the filter panel remembers
(`MeshFilterPanel::forgetRememberedParameters()`). The camera is framed by the next layer, by the
same path as Reset Camera. Split views are closed down to one, the current view, as a launch opens with. Preferences are kept. State a new
view carries should be added to `resetToDefaults()`, or New will not clear it.
