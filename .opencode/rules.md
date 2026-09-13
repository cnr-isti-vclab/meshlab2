# QMeshLab Project Rules

## Architecture

This is a **Qt6 rewriting of MeshLab** using `QRhi` for rendering. One `Document` owns
the canonical mesh list, raster list, undo tree, and shared `MeshGpuResourceCache`.
Multiple `RenderWidget` views observe it via Qt signals. Per-view state
(mesh render modes, camera, visibility vector) lives in each `RenderWidget`.

The optional embedded Python console exposes `_qmeshlab.MeshSet` (nanobind) that
wraps a `Document` and calls the same filter manager used by the GUI.

See `docs/design/architecture.md`, `docs/design/data_model.md`, `docs/design/rendering.md`.

## Build

- vcglib is a git submodule at `vcglib/`. **Always use vcglib for 3D computation**,
  except in `plugins/io_trueform` and `plugins/filter_trueform`, where TrueForm
  (`external/trueform`) is the computation layer — see
  [TrueForm Plugin](../docs/design/trueform_plugin.md).
- Include vcglib headers from `vcglib/`.
- Build with cmake presets: `cmake --preset vcpkg-debug`, then
  `cmake --build build-debug --target MeshLab2 -j8`.
- Plugin registration: 4-line `add_subdirectory` block in `plugins/CMakeLists.txt`, plus
  `#include` and register call in `plugins/filterpluginregistry.cpp`.

## Plugin Conventions

- **Static library** (`add_library(... STATIC ...)`), linked into `MeshLab2Plugins`.
- Include `${CMAKE_SOURCE_DIR}` and `${CMAKE_CURRENT_SOURCE_DIR}` in target.
- Link `MeshLab2Core` and `Qt6::Core` (add `Qt6::Gui` if QImage needed).
- **One `.cpp` per plugin** — consolidate algorithm logic; use separate files only when
  the algorithm requires its own data structures or exceeds ~800 lines.
- Filters declare metadata in `filters.json` loaded via `qt_add_resources`.
- Filter parameter access: `params.getBool("id")`, `params.getInt("id")`,
  `params.getDouble("id")`, `params.getString("id")`, `params.getEnum("id")`,
  `params.getColor("id")`, `params.getPoint3f("id")`.

### Avoiding code bloat

- **Never duplicate code or functionality.** Extract shared utilities to `src/core/`.
- **Remove unused files and functions** when refactoring. Ask before leaving leftovers.
- **Prefer fewer lines.** If an approach requires a lot of new code, warn and suggest
  a shorter alternative before implementing.
- **Reuse existing filters as pre-processing steps** instead of embedding their logic
  in new code (e.g., use `transfer_color_texture_to_vertex` before Poisson instead
  of re-implementing texture sampling).

## GPU / OpenGL

- **No GPU / OpenGL / QRhi access in filters.** All filter code is CPU-only.
- Use `CameraShot::project()` / `depth()` / `unproject()` and VCG algorithms.
- For visibility checks, use the software depth buffer (`src/core/softdepthbuffer.h`).
- For texture rendering, use QImage pixel operations.

## Common Pitfalls

1. **QMap / QHash range-for yields VALUES, not pairs.** Use `iterator` with
   `.key()`/`.value()` to iterate both, or use `QMap` which gives value-only range-for.
2. **QMatrix4x4 is column-major.** `operator()(row,col)` accesses logically;
   internally `m[col][row]`. `map(QVector3D)` applies the transform correctly.
   No transpose is needed when copying from/to VCG matrices element-by-element.
3. **VCG `Shot` methods:** use `Extrinsics.Tra()` (not `GetTra()`), `Extrinsics.Rot()`,
   `cameraType` (member field, not `GetCameraType()`). `shot.GetViewPoint()` is fine.
4. **`CameraShot::project()` returns pixel coords with Y=0 at bottom** (VCG convention).
   `QImage::pixel()` uses Y=0 at top. Always flip Y when reading/writing QImages.
5. **Filter `inputRequirements`** in `filters.json` checks mesh attributes BEFORE
   the filter runs. Filters that CREATE attributes (texcoords, quality, color)
   must NOT list them in `inputRequirements`. Use `outputModifies` and `inputPrepare`.
6. **`FilterParams` contains only DECLARED parameters.** The framework normalizes against
   the descriptor. Undeclared params are absent. Reading one with
   `params.getString("a")` returns `""` — which muparser rejects as "Expression is empty."
   Declare every parameter the C++ code reads, even if trivial (e.g., `"a"` defaults to `"1"`).
7. **MeshEntry has no `shot` member** in QMeshLab — only `RasterEntry` carries a camera.
8. **`QJsonDocument` parsing in Qt6:** `fromJson()` takes QByteArray by value and owns
   the data. Don't hold QJsonValue references across function boundaries.
9. **Python bindings:** `resolveFilterKey` must match against `effectivePythonName()`,
   not `computePythonName(displayName)`, so it aligns with the copy-to-console button.
10. **Meshes are always VCG-compact** in QMeshLab — no `IsD()` checks needed in loops.

## Porting a MeshLab Plugin

1. **Study the original** under `.reference/meshlab/src/meshlabplugins/<name>/`.
2. **Map filter classes:** original `RichParameter` subtypes → QMeshLab
   `MeshFilterParameterDescriptor::type` (`bool`, `int`, `double`, `string`, `enum`,
   `color`, `point3f`, `camerastate`, `renderstate`, etc.).
3. **Map mesh requirements:** original `MM_*` flags → `inputRequirements` boolean
   fields + `inputPrepare` string list (e.g., `["FF","VF","WT"]`) + `outputModifies`
   two-letter codes (`VG`, `VC`, `WT`, `TX`, `VQ`, `FQ`, etc.).
4. **Convert VCG types:** `CMeshO` → `VCGMesh`, `Shotm` → `CameraShot`,
   `MeshDocument &md` → `Document &doc`, `md.mm()` → `doc.mesh(currentMeshIndex())`.
5. **Replace GPU code with CPU equivalents.** QMeshLab filters have no GPU context.
6. **Use `inputDomain: "SingleMesh"`** unless the filter genuinely needs all visible layers
   (`"WholeDocument"`).
7. **Return `MeshFilterRunResult`** with `success`, `documentModified`, `infoMessages`,
   `outputValues` (key-value map for structured results — exposed to Python as a dict).

## Python Bindings

- `_qmeshlab.MeshSet.apply_filter(name, **kwargs)` → `FilterRunRecord`
- Dictionary keys map to filter parameter `id` fields (not labels).
- `FilterRunRecord.output_values` is a Python dict (converted from `QVariantMap`).
- Supports `bool`, `int`, `float`, `str`, `QVector3D` → `tuple`, and `QList<double>` → `list`.
