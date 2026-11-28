# Load Options

This document proposes letting a mesh importer declare options -- what to read and how.
**Opening a file never asks**: the layer is loaded with the default options, and *Reload with
Options...* in the layer's context menu reads its file again with different ones. **Nothing
described here is implemented.**

See also: [Data Model](../data_model.md) (Layer Provenance, the I/O model),
[Preferences](../preferences.md) (the precedent for reusing the filter parameter schema),
[Adding a Filter](../adding_a_filter.md) (the schema itself),
[LAS Support](las_support.md) and [USD Support](usd_support.md) (formats that need options).

## Status

| Stage | What | State |
|---|---|---|
| 1 | Declared options, loaded with defaults, kept on the layer, *Reload with Options...*, recorded in the action history and in Python; STL's merge moved onto them | not started; needs approval of the interface and of the mapping table in §5 |
| 2 | Options kept in the project | not started |
| 3 | Options that change how many layers a file gives (one per scan, per node) | not started; depends on the scene interface of [USD Support §5](usd_support.md#5-stage-2-scenes) |

## 1. Why

Several formats have choices that only the user can make, and today each is either hard-coded
in the importer or a global preference:

| Format | The choice | Where it is today |
|---|---|---|
| STL | merge the duplicated corners into shared vertices | the global preference `document.mergeStlDuplicateVertices` |
| E57 | which scans; intensity as scalar; open the photos as rasters; panoramas resampled into pinhole views | all of it always done (`plugins/io_e57`) |
| LAS/LAZ | keep every point or a subset; which per-point fields to keep; the coordinate offset | not supported ([LAS Support](las_support.md)) |
| glTF, USD, 3MF, E57 | one layer, or one per node, item or scan | always one layer (stage 3) |
| USD | apply the up axis to the layer matrix or to the coordinates | not supported ([USD Support §9](usd_support.md#9-open-questions)) |
| OBJ | which of the three importers that accept `.obj` reads it | the plugin order |

Legacy MeshLab had this mechanism (`IOPlugin::initPreOpenParameter`, in
`.reference/meshlab/src/common/plugins/interfaces/io_plugin.h`). STL's
`unify_vertices`, glTF's `load_in_a_single_layer`, 3MF's `usecolors` and
`forcetransform`, PTX's culling options and the TXT column format were options there.
Defaults came from the global settings, and a modal *Pre-Open Options* dialog opened on
**every** load unless turned off per format. Two weaknesses are worth avoiding: the dialog stood
between the user and every file, asking before the user had seen what the file holds, and a
choice was final, since nothing remembered how a layer had been read.

MeshLab now has what that version lacked. A layer knows its file (`MeshEntry::sourcePath`)
and whether it still holds what the file gave it (`Document::meshStoredState`). The filter
parameter schema already serves two clients, filters and preferences, with one form builder.

## 2. The interface

Options are declared with the **filter parameter schema** (`MeshFilterParameterDescriptor`),
which makes load options its third client after filters and preferences. The form,
the help, the reset buttons, the `enabledWhen` gating, validation and the Python names come
for free.

Two additions to `MeshIOPlugin`, both optional:

```cpp
// The options a load of this file accepts. Called before load() with the file, so a plugin
// may read the header cheaply and offer what the file has (an E57's scans, a LAS file's
// extra fields). Empty, the default, means the format has no options.
virtual std::vector<MeshFilterParameterDescriptor> loadParameters(const QString &filename) const;

// MeshIOLoadContext (src/plugins/meshioplugin.h, which already carries the document origin
// and the file's unit) gains the options: every declared parameter, defaults filled in by
// the Document. No new overload.
struct MeshIOLoadContext {
    std::optional<vcg::Point3d> origin;
    LengthUnit unit;
    QVariantMap options;   // new
};
```

A plugin with no options changes nothing. Declarations may live in the plugin's JSON, read by
`FilterDescriptorLoader::loadParameters()` as `preferences.json` is, or be built in code when they
depend on the file.

**Defaults** are resolved in three steps, as for preferences:
1. the declared default;
2. overridden by the value the user last chose for that plugin and parameter, stored under the
   plugin's `pluginId()`, which the interface already reserves for settings;
3. overridden by what the caller passes.

## 3. Load first, choose after

**Opening never asks.** The file dialog, drag and drop, recent files, a project's layers and
scripts all load with the resolved defaults. When the format has options, one log line says
which values were used and that *Reload with Options...* changes them. The user decides after
seeing the layer, which is when the choice can be made well: whether the subsampled cloud is
dense enough, or whether the photos are wanted.

**Reload with Options...** in a layer's context menu opens a form of the options of the layer's
file, on the values the layer was loaded with. It uses `ParameterFormBuilder`, as the filter
panel and the preferences dialog do. *Reload* reads the file again with the new values,
keeping the layer's identity, name, matrix and visibility, as one undo step ("Reload with
Options"). *Remember for this format*, a checkbox in the form, writes the values as the
defaults for later files (step 2 of §2). This is how a user who always wants, say, every point
of a LAS file stops having to reload.

| Layer state | *Reload with Options...* |
|---|---|
| **Saved**: the layer holds what its file gave it | reloads with the new options |
| **Modified**: changed since the file was read | offered with a warning that the changes since will be discarded; the reload is undoable, so nothing is lost |
| **Not saved**: no file | not offered |
| its file has no options | not offered |
| the file is gone or changed on disk | fails as *Reload* does; the layer is untouched |

The price is reading the file twice when the defaults were wrong. So a format whose defaults
could be costly has to make them cheap rather than ask. For example, a LAS file's default is to
load at most a point budget, 30 million points to start, by an even stride
([LAS Support §4](las_support.md#4-import)). Scripts pay nothing: they pass the options to
the load directly (§4).

## 4. Provenance: recorded and kept

**Recorded.** The `load_mesh` `ScriptAction` already has a `params` field, documented as "empty
for load/save"; it carries the resolved options. A reload with options is recorded as a load
of that file with those options, so the generated script reads the file once, with the values
the user ended with. Python gains keyword arguments:

```python
ms.load_new_mesh("tile.laz", max_points=20_000_000, keep_fields=["classification"])
```

Unknown names and bad values are refused as a filter's are.

**Kept.** `MeshEntry` gets `loadOptions`, beside `sourcePath`. Plain *Reload* reads the file
with them, so it repeats the load the user ended with rather than a default one. A save that
makes another file the layer's own (`saveMesh`) clears them: the layer now comes from that
file, read with its own format's defaults.

**In the project** (stage 2). An `.mlp` writes a layer's options as a child of its `MLMesh`,
after `MLMatrix44`, and loading the project passes them back. Legacy MeshLab looks up the
children of `MLMesh` by name (`firstChildElement("MLMatrix44")`, `"RenderingOption"` in
`.reference/meshlab/src/meshlabplugins/io_base/load_project.cpp`), so it skips this one.

## 5. Moving STL's merge onto it

The one existing load choice moves first. Per the mapping-table rule:

| Current | Target | Evidence | Verdict |
|---|---|---|---|
| preference `document.mergeStlDuplicateVertices` (`resources/preferences.json`) | the default of the STL load option `mergeDuplicateVertices` | read only in `src/ui/mainwindow.cpp` (lines 419 and 935), to call the setter below | move; the stored preference value becomes the remembered default (step 2), migrated once |
| `Document::mergeStlDuplicateVertices()` / `setMergeStlDuplicateVertices()` (`src/core/document.h`) | removed | callers: those two lines, and `tests/test_document.cpp` (4221, 4227, 4233), which would pass the option instead | remove |
| the merge in `Document::loadMesh` (`document_io.cpp:113`) | stays in `Document`, driven by the option | it runs for every STL whichever of `io_vcg` and `io_trueform` read it | keep, read the option instead |
| the mentions in `plugins/io_trueform/trueformioplugin.cpp:141` and `plugins/io_trueform/UPSTREAM.md:176` | name the option | comments only | reword |

There is one move, one removal, one kept piece and one rewording, four rows in all. The STL option is
declared once, by `Document`, for whichever STL importer runs, rather than by each plugin.
That one exception is worth it: the merge is the Document's, not the importer's.

## 6. Open questions

1. ~~**An overload or a request struct.**~~ Settled 2026-11-28 by the document origin
   ([LAS Support §2](las_support.md#2-the-real-problem-precision)): `MeshIOLoadContext`
   replaced the last overload's `LengthUnit *outUnit`, and `loadRasters` takes it too. The
   options go into it. Folding the three `load` overloads into one remains a separate cleanup.
2. **Save options.** `MeshIOSaveOptions` is a fixed struct holding glTF's Draco settings for
   every format. Declared save parameters would be the same mechanism; they are not proposed
   here.
3. **Several layers from one file** (stage 3). Reloading needs the layers of one load
   grouped by file and load, and options that change the layer count need the scene interface.
   Until then an option may change what a layer holds, never how many layers there are.
4. **OBJ importer choice.** Is the importer itself an option (an enum of the plugins that
   accept the file, offered by `Document`), or a preference? It would be the only option
   not declared by a plugin.
