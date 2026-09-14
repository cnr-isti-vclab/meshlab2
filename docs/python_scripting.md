# Python Scripting

MeshLab can expose an embedded Python console and script editor when it is built
with `MESHLAB2_PYTHON_CONSOLE=ON`. The embedded interpreter works on the live
application document, so scripts can inspect the current scene, run filters,
load/save meshes and rasters, and capture view snapshots.

The shared API is the `pymeshlab.MeshSet` interface, and MeshLab reuses that
same model inside the desktop application. Generated API reference files live
under `docs/api/` when regenerated from the current filter descriptors; the
local generated snapshot can lag until the app is run with `--generate-docs`.
The desktop-only additions are the predefined live `ms` object and the `mlgui`
helper.

## Predefined Names

The script environment is the Python `__main__` namespace used by the embedded
console. Names created in the console and names created by scripts share the
same namespace for the current MeshLab session.

### `ms`

`ms` is the live document, exposed as a `pymeshlab.MeshSet`-compatible object. This is the
main entry point for document and filter operations.

```python
print("Meshes:", ms.mesh_number())
print("Current mesh:", ms.current_mesh())
print("Rasters:", ms.raster_number())
```

Important: `ms` borrows the live MeshLab `Document`. Calling modifying methods
on it changes the open document.

If you already know `pymeshlab`, you can think of `ms` as "the current
MeshLab document presented through the same MeshSet-style API".

### `mlgui`

`mlgui` is the live GUI/view helper. It is backed by the private `_meshlab.MlGui` type. It is
available in the desktop application when an active render view exists.

```python
print(mlgui.camera_state_json())
print(mlgui.render_state_json())
mlgui.save_snapshot("/tmp/meshlab_snapshot.png", 1200, 900)
```

Use `mlgui` for view state and rendering operations. It is not meant to be a
general document API.

### `pymeshlab`

`pymeshlab` is the public Python facade for standalone, GUI-less mesh processing.
In the embedded MeshLab console it is injected as a lightweight module backed by
the same private `_meshlab` extension used by the headless package.

Use it when you want a separate mesh set that does not operate on the live GUI
document.

```python
import pymeshlab

other = pymeshlab.MeshSet()
print("Standalone mesh set:", other.mesh_number())
```

Most scripts inside MeshLab should use the predefined `ms` object. Import
`pymeshlab` only when you want the same public API exposed by the GUI-less
package, or when you want to create an independent `MeshSet`.

## Console and Script Editor

The Python dock contains a script editor and an interactive console side by
side. Both execute in the same `__main__` namespace, so variables created in a
script remain available at the console prompt and vice versa.

Useful entry points:

- Run the editor contents with the `Run` button or `Ctrl+Enter` / `Cmd+Return`.
- Use the console for quick one-line calls and Up/Down history navigation.
- Use the filter panel's copy-to-console action when you need exact parameter ids.
- Use the undo graph's `Generate Python Script` command to export the current
  undo path into the script editor.

History export offers two styles: full scripts keep every recorded parameter for
exact replay, while compact scripts omit parameters that matched the descriptor
defaults at the time each action was recorded.

## Available Module Objects

The public `pymeshlab` facade exposes:

- `pymeshlab.Mesh`
- `pymeshlab.MeshSet`
- `pymeshlab.FilterInfo`
- `pymeshlab.FilterRunResult`
- `pymeshlab.MlGui`

## MeshSet Basics

The live `ms` object supports the following core methods:

- `mesh_number()`
- `current_mesh()`, `current_mesh_id()`, `set_current_mesh(index)`
- `mesh_id_exists(index)`
- `set_current_mesh_visibility(visible)`
- `set_mesh_visibility(index, visible)`
- `is_current_mesh_visible()`, `is_mesh_visible(index)`
- `load_new_mesh(path)`
- `save_current_mesh(path)`
- `raster_number()`
- `current_raster()`, `set_current_raster(index)`
- `load_new_raster(path)`
- `clear()`
- `load_project(path)`, `save_project(path)`
- `filter_list()`, `list_filters()`
- `apply_filter(filter, params={})`
- `render_snapshot(render_state_json, width, height)`

Example:

```python
print("Mesh count:", ms.mesh_number())

if ms.mesh_number() > 0:
    print("Current mesh index:", ms.current_mesh())
    print("Current mesh id:", ms.current_mesh_id())
```

## Listing Filters

Use `ms.list_filters()` for structured filter information:

```python
for info in ms.list_filters()[:20]:
    print(info.python_name, "-", info.name)
```

Each `FilterInfo` has:

- `key`
- `id`
- `plugin_id`
- `plugin_name`
- `name`
- `python_name`
- `applicable`
- `applicability_error`

Example:

```python
for info in ms.list_filters():
    if "texture" in info.name.lower():
        print(info.python_name, "|", info.name)
```

## Running Filters

There are two equivalent styles.

Call `apply_filter()` with the filter Python name:

```python
result = ms.apply_filter("simplification_quadric_edge_collapse", {
    "TargetFaceNum": 10000,
    "PreserveBoundary": True,
})
print(result.success, result.error_message)
```

Or call the dynamically generated convenience method:

```python
result = ms.simplification_quadric_edge_collapse(
    TargetFaceNum=10000,
    PreserveBoundary=True,
)
print(result.success)
```

The generated convenience methods are attached to the shared `MeshSet` type when the
console starts. Their names come from each filter descriptor's `pythonName`.

## Filter Parameters

Filter parameters are passed as Python keyword arguments or as a dictionary.
Currently supported parameter value types are:

- `bool`
- `int`
- `float`
- `str`
- `None`
- 3-item `tuple` or `list` for point/vector values, for example `(1.0, 0.0, 0.0)`

Example with a vector parameter:

```python
result = ms.apply_filter("compute_matrix_from_translation", {
    "traslMethod": "xyz",
    "axis": (1.0, 0.0, 0.0),
    "Freeze": False,
})
print(result.success)
```

Parameter names must match the filter descriptor ids. The easiest way to get a
valid call is to use the filter panel's copy-to-console action. For longer
reproducible scripts, export the current undo path from the action-history graph
and choose either the full or compact format.

## Filter Results

Filter calls return a `pymeshlab.FilterRunResult`-compatible object.

```python
result = ms.apply_filter("mesh_info", {"precision": 3})

print("success:", result.success)
print("modified:", result.document_modified)
print("error:", result.error_message)
print("new meshes:", result.new_mesh_indices)
print("outputs:", result.output_values)

for message in result.info_messages:
    print(message)
```

Fields:

- `success`
- `document_modified`
- `error_message`
- `info_messages`
- `new_mesh_indices`
- `output_values`

## Snapshot Examples

Save the current view to a PNG:

```python
mlgui.save_snapshot("/tmp/meshlab_view.png", 1600, 1200)
```

Capture and reapply render state:

```python
state = mlgui.render_state_json()
mlgui.save_snapshot("/tmp/meshlab_view.png", 1600, 1200, state)
```

Get raw RGBA bytes:

```python
state = mlgui.render_state_json()
pixels = mlgui.render_snapshot(state, 800, 600)
print("Bytes:", len(pixels))
```

Render from a standalone mesh set without borrowing the live GUI document:

```python
import pymeshlab
from pathlib import Path

other = pymeshlab.MeshSet()
other.load_new_mesh("/tmp/input.ply")
state_json = Path("/tmp/render_state.json").read_text()
pixels = other.render_snapshot(state_json, 800, 600)
print("Bytes:", len(pixels))
```

`mlgui` uses the live active render view. `MeshSet.render_snapshot(...)` uses a
hidden `HeadlessRenderContext`, so it is the right path for standalone
processing and batch-style scripts.

## Minimal Useful Scripts

Print a short document report:

```python
print("Meshes:", ms.mesh_number())
print("Rasters:", ms.raster_number())

for info in ms.list_filters()[:10]:
    print(info.python_name, "-", info.name)
```

Load a mesh and save it in another format:

```python
ms.load_new_mesh("/tmp/input.obj")
ms.save_current_mesh("/tmp/output.ply")
```

Run a filter and inspect the result:

```python
result = ms.apply_filter("mesh_info", {"precision": 2})

if not result.success:
    print("Filter failed:", result.error_message)
else:
    for message in result.info_messages:
        print(message)
```

## Caveats

- `_meshlab` is the private compiled extension module. User scripts should prefer the predefined `ms` object or `import pymeshlab`.
- `ms` operates on the live document; scripts can modify the scene.
- `mlgui` depends on an active desktop render view.
- Standalone `pymeshlab.MeshSet()` objects own their own document and do not
  automatically share the live application document.
- History-generated scripts replay recorded action metadata. Compact scripts are
  intentionally smaller and omit descriptor-default parameters; full scripts are
  better when exact parameter visibility matters.
- The API is still evolving. Prefer generated filter calls from the filter panel
  when you need exact parameter names.
