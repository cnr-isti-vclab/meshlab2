# Adding a Filter Plugin

A short, practical guide. For naming/menu taxonomy conventions see
[Filter Organization](filter_organization.md); the same parameter schema also
declares application settings, see [Preferences](preferences.md); for the data model see
[Data Model](data_model.md).

## What a filter is

A **filter** is a one-shot operation on the document (the whole document or the
current mesh). Filters are grouped into statically-linked **plugins**. A plugin
is a small class plus a JSON file that *declares* its filters; the C++ code only
implements the algorithm. The framework handles menus, the parameter UI, Python
bindings, undo, and mesh bookkeeping for you.

Interactive, mouse-driven operations are **not** filters — see the interactive
tools system. But an interactive tool typically *commits* its result by calling
one filter, so the two compose.

## Anatomy of a plugin

```
plugins/filter_foo/
  filters.json          # declares the filters (ids, categories, params, I/O domains)
  foofilterplugin.h     # class FooFilterPlugin : public MeshFilterPlugin
  foofilterplugin.cpp   # runFilter() — the actual algorithms
  CMakeLists.txt        # build target + embeds filters.json as a resource
```

### 1. `filters.json` (the declaration)

```json
{
  "pluginId": "meshlab2.filter.foo",
  "provenance": {
    "project": "Upstream Project",
    "repository": "https://github.com/example/upstream",
    "license": "SPDX-license-identifier",
    "integration": "external/upstream"
  },
  "filters": [
    {
      "id": "do_something",
      "categories": ["Repair/Topology"],
      "name": "Do Something",
      "pythonName": "do_something",
      "shortDescription": "One line shown in the browser.",
      "longDescriptionMarkdown": "What it does, for the in-app help and the Python docs.",
      "tags": ["topology"],
      "inputDomain": "SingleMesh",          // None | SingleMesh | WholeDocument
      "outputDomain": "ModifyCurrentMesh",  // Information | ModifyCurrentMesh | NewMeshes
      "inputRequirements": { "requireVertices": true, "requireFaces": true },
      "inputPrepare": ["FF", "FNorm"],      // topology/normals the framework builds for you
      "outputModifies": ["VG", "FN"],       // which mesh attributes you change (see below)
      "parameters": [
        {
          "id": "amount", "label": "Amount", "type": "double", "group": "main",
          "default": 1.0, "min": 0.0, "max": 10.0, "decimals": 3,
          "help": "A sentence or two. Shown in the tooltip and the generated docs."
        }
      ]
    }
  ]
}
```

The default `MeshFilterPlugin::filters()` loads this from the Qt resource
`:/filters/<pluginId>/filters.json`, so the `CMakeLists.txt` PREFIX must match
`pluginId`.

Parameter types are matched **exactly, in lower case**. An unrecognised string
falls through to `string` silently, so `absPerc` is a bug rather than a synonym:

```
bool  int  double  absperc  enum  color  point3f  string  mesh
fileopen  filesave  textureref  textureoutputref  camerastate  renderstate
```

(`file_open` and `file_save` are accepted as alternates.)

Every parameter also carries `label`, `help` and `group` — all 1168 declared
parameters do. Each distinct `group` becomes one section of the form, in order
of first appearance, and headings only appear once there is more than one group.
The name is cosmetic except for one rule: **a group whose name begins with
`advanced` is hidden behind the advanced toggle**, which is why `advanced.tree`
and `advanced.solver` work. Underscores and dots become spaces in the heading
(`advanced.solver` reads "Advanced solver" — sections are flat, not nested), and
an empty group is "Main".

`enum` parameters must carry `enumOptions`, an array of `{"id", "label"}` — each
option may also carry its own `help`. Numeric parameters take `min`, `max` and
`decimals` (default 3).

`point3f` parameters take an optional `point3fRole`, which decides what the
editor's preset combo offers beside the three spin boxes:

| role | presets |
|---|---|
| `point` (default) | Origin, Mesh BBox Center, Camera Eye Position, Trackball Center |
| `direction` | ±X, ±Y, ±Z, View Direction |
| `axis` | X, Y, Z, View Direction — for an axis, where the sign means nothing |

`point3fDefaultPreset` opens the combo already on a context-derived preset. It
takes one of the tokens `cameraEye`, `trackballCenter`, `bboxCenter`,
`viewDirection` or `origin`, and only those — the fixed-axis entries have none.
A token the role does not offer, or one whose context is unavailable, silently
leaves the descriptor's literal `default` in place. An unknown role falls back
to `point`.

`inputRequirements` declares preconditions the framework checks *before* your
code runs, so the filter is greyed out with a reason instead of failing inside
`runFilter`. All are optional booleans: `requireVertices`, `requireFaces`,
`requireEdges`, `requireVertexQuality`, `requireFaceQuality`, `requireEdgeQuality`, `requireVertexColor`,
`requireFaceColor`, `requireTextureCoordinates`, `requirePerVertexTexCoords`,
`requirePerWedgeTexCoords`, `requireTextures`. Declare them instead of
re-checking by hand — 291 of 337 filters do.

`tags` are free-form search keywords; see [Vocabulary](vocabulary.md) for the
standing question of whether they should be generated rather than written.

`categories` is required for in-tree descriptors and is validated against the
closed ontology in [Vocabulary](vocabulary.md). The first category is the
canonical home; later entries are cross-listings. The loader still accepts a
single legacy `menuPath` string when `categories` is absent, but new descriptors
should not use it.

A parameter that only matters when another is set should say so structurally rather
than only in its help text:

```json
{ "id": "viewpoint", "type": "point3f", "enabledWhen": "!usecamera" }
```

`enabledWhen` names a **bool** parameter in the same filter; the editor greys out while
that bool is false, and a leading `!` inverts it. An unknown or non-bool id leaves the
row enabled, so a typo cannot strand a control. Conditions on enum choices or numeric
values are not expressible — keep describing those in `help`.

`provenance` is optional and declared once for every filter supplied by a
third-party project. It is shown in the in-app help and generated documentation.
When `integration` names a Git submodule, its gitlink is the authoritative exact
upstream revision; do not duplicate a commit hash in the descriptor.

Publication references are declared once in the top-level `references` array
using CSL-JSON field names, then attached to filters with `referenceIds`:

```json
{
  "references": [{
    "id": "author2026method",
    "type": "article-journal",
    "title": "Method Title",
    "author": [{"family": "Author", "given": "Ada"}],
    "container-title": "Journal",
    "issued": {"date-parts": [[2026]]},
    "DOI": "10.1234/example",
    "URL": "https://example.org/paper"
  }],
  "filters": [{
    "id": "do_something",
    "referenceIds": ["author2026method"]
  }]
}
```

The framework generates the in-app `[bib]`, `[doi]`, and `[web]` actions, help
citations, online references, and the combined `references.bib`; do not repeat
the citation in `longDescriptionMarkdown`.

### Descriptions and formulas

`longDescriptionMarkdown` is the single source for both the in-app help and the
generated Python documentation. Use Markdown, with LaTeX enclosed in `$...$`
for inline formulas or `$$...$$` for a centered formula:

```json
"longDescriptionMarkdown": "For a triangle of area $A$, the normalized quality is $$q = 2A/L_{\\max}^2.$$"
```

Because this is JSON, every LaTeX backslash must be escaped as `\\`. Keep
formulas to standard LaTeX math commands supported by both JKQTMathText and
MathJax; links and ordinary formatting should remain Markdown rather than HTML.

Angular parameters must state their convention explicitly. Use **Half-Angle**
for the angle from an axis to a cone boundary, and **Angular Diameter** (or
**Full Aperture**) for the angle between opposite boundary directions. Avoid the
ambiguous label **Cone Angle**.

Prefer the half-angle: every cone and cap parameter MeshLab exposes is one, so
a new filter that adopts a full aperture makes two sibling filters disagree
about what the same number means. Name the parameter for the convention as well
as labelling it -- `half_angle`, `cone_half_angle` -- because a label protects
the GUI user while the Python caller sees only the id. When a vcglib entry point
takes the full aperture, as `vcg::tri::SphericalCap` does, convert at the call
site rather than changing the shared signature.

### 2. The plugin class

```cpp
class FooFilterPlugin final : public MeshFilterPlugin {
public:
    QString pluginId() const override { return QStringLiteral("meshlab2.filter.foo"); }
    QString name() const override { return QObject::tr("Foo Filters"); }
    MeshFilterRunResult runFilter(const QString &filterId,
                                  const FilterParams &params,
                                  Document &doc) const override;
};
void registerFooFilterPlugin(MeshFilterPluginManager &pm);
```

`runFilter` reads params (`params.getDouble("amount")`, `getEnum`, `getBool`,
`getPoint3f`, `getCameraState`, …), operates on the mesh, and returns a
`MeshFilterRunResult { success, documentModified, errorMessage, infoMessages, … }`.

### 3–5. Wiring (three edits)

- `plugins/filter_foo/CMakeLists.txt`: `option(MESHLAB2_PLUGIN_FILTER_FOO … ON)`, an
  `add_library(... STATIC)` linking `MeshLab2Core`, and `qt_add_resources(...
  PREFIX "/filters/meshlab2.filter.foo" FILES filters.json)`. Copy `filter_basic`.
- `plugins/CMakeLists.txt`: `add_subdirectory(filter_foo)` + link it into
  `MeshLab2Plugins` guarded by `MESHLAB2_PLUGIN_FILTER_FOO_ENABLED`.
- `plugins/filterpluginregistry.cpp`: `#include` the header and call
  `registerFooFilterPlugin(pm)` under the same `#if …_ENABLED`.

`filter_basic` is the smallest complete example to copy.

## What the framework does for you — don't do it yourself

- **Undo**: `runFilter` for a non-`Information` filter is automatically wrapped in
  one undo step, and the Python call is recorded. **Never touch the undo stack.**
  A `SingleMesh` filter whose `outputModifies` is non-empty and contains only
  selection codes (`VS`/`FS`) automatically uses the cheap bit-packed
  selection-delta undo instead of a full snapshot — which matters on large
  meshes, where snapshotting is seconds of deep copy.
- **Input preparation**: declare `inputPrepare` codes instead of computing
  adjacency, normals or flags by hand — the framework enables the OCF
  components, runs the vcglib update in dependency order, and disables them
  afterward. The full set: `FF`, `VF`, `BorderFF`, `BorderVF` (each border code
  implies its adjacency), `FNorm`, `VNorm` (implies `FNorm`), `BBox`, `FMark`,
  `VMark`, `VTex`, `WTex`, `CurvDir`.
- **Cleanup/compaction**: after a successful filter the framework compacts the
  affected meshes; don't leave deleted elements around expecting them to persist.
- **Notify changes**: after mutating a mesh, call the matching
  `doc.markMeshGeometryChanged` / `markMeshSelectionChanged` /
  `markMeshMaterialChanged` so the render/layer views refresh.

## Best practices / what's expected

- **Declare `outputModifies` accurately** — the framework keys undo storage,
  compaction, and cache invalidation off it. The codes, as the UI spells them
  out (`FilterPresentation::modifiedDataLabels`):

  | | vertex | face | edge |
  | --- | --- | --- | --- |
  | geometry / connectivity | `VG` | `FV` | — |
  | normals | `VN` | `FN` | — |
  | color | `VC` | `FC` | `EC` |
  | scalar (quality) | `VQ` | `FQ` | `EQ` |
  | texcoords | `VT` | `WT` (per wedge) | — |
  | named attributes | `VA` | `FA` | — |
  | selection | `VS` | `FS` | `ES` |

  The edge codes refer to the real `VCGEdge` elements of a polyline layer, not to
  the sides of triangles — see [Edge Support](proposals/edge_support.md). For the
  sides of triangles, which are a fourth element class rather than a column of this
  grid, selection is `FES`: the three face-edge bits each face carries, which is what
  the crease and non-manifold-edge filters mark.

  plus `FP` (face polygon bits), `TX` (texture images) and `TM` (per-mesh
  transform).

  Declaring a selection code is what makes the framework append the
  `Selection now contains …` line to a filter's log output, so a filter that marks
  face edges and declares only `FS` will report the wrong counts.
- **Reuse vcglib** — the `vcglib/` submodule at the repo root — for all 3D
  computation.
- **Parallelize heavy per-element loops** with `std::thread` when independent.
- **Return useful `infoMessages`** — they appear in the log; keep `errorMessage`
  actionable and return `success = false` on bad input rather than asserting.
- **Suggest a view change** (optional) via `MeshFilterRunResult::visualizationHints`
  (e.g. switch to textured/quality shading) — see the visualization-hint path in
  `MainWindow::applyFilterVisualizationHints`.
- **Randomized filters declare `randomSeed`** — `int`, default `0`, minimum `0`,
  resolved through `FilterParams::getRandomSeed()`, with the drawn value reported
  in the result messages so a run can be pinned afterwards. 39 parameters follow
  this; see [Filter Organization](filter_organization.md#randomseed).
- **Per-layer plugin data** (rare) — `doc.setLayerData(meshIndex, "<pluginId>/<name>", ptr)`
  attaches an immutable, plugin-owned object to a layer: a solver state, a
  parametrization domain, anything too structured for mesh attributes. Undo
  carries it, and it is dropped when the layer's geometry changes unless it
  says it survives. Subclass `LayerData` (`src/core/layerdata.h`); read it back
  with `doc.layerData(meshIndex, key)`.
- **Follow the naming/menu taxonomy** in [Filter Organization](filter_organization.md);
  names should describe the observable result, not the algorithm.

## Logging

Filters normally say nothing at all: return `infoMessages` and let the framework log
them. Use `Document::writeLog` only for what does not fit that — progress narration of a
long multi-stage run, or diagnostics.

```cpp
doc.writeLog(msg);                                                    // Info (default)
doc.writeLog(msg, Document::LogSource::Application, Document::LogLevel::Debug);
```

`LogSource` says *who spoke* (`Application`, `VCG`); `LogLevel` says *whether the user
wants to hear it*, and is what the `log.verbosity` preference filters on:

| Level | Use for | Examples |
| --- | --- | --- |
| `Error` | the operation failed and the user must know | "Cannot bake quality colors: no current mesh selected." |
| `Warning` | it went ahead, but degraded or partially | "Project mesh '%1' has no filename and was skipped" |
| `Info` | the normal narration of what happened — the default | "UV islands after defragmentation: %1" |
| `Debug` | timings, counters, mask dumps, cache and GPU bookkeeping | "Load timing '%1': import %2 ms, post %3 ms" |

Rules of thumb:

- **Anything with a millisecond count in it is `Debug`.** So is anything a user could not
  act on: buffer sizes, revision numbers, callback statistics, `mask: 0x…`.
- **One line per outcome, not per element.** A per-face or per-vertex message is a bug;
  aggregate and report once.
- **Do not log progress.** Call `doc.progressCallback()` (or the `vcg::CallBackPos` you
  were given) instead. The framework renders it as a single self-overwriting line that is
  removed when the run ends, so it never accumulates in the log.
- **Do not log the failure you are already returning.** Set `errorMessage` and
  `success = false`; the caller logs it at `Error` once.
- **`qWarning`/`qDebug` do not reach the log panel** — they go to the terminal only. Use
  them for developer-only tracing, never for anything a user is meant to read.
