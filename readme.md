# MeshLab

A ground-up rewrite of [MeshLab](https://github.com/cnr-isti-vclab/meshlab), the open
source system for processing and editing large 3D triangular meshes and point clouds.
MeshLab is a single-document, multi-view Qt 6 application built on QRhi (Metal, Vulkan,
Direct3D 12), vcglib, and a plugin architecture for I/O and filters, with optional
embedded Python bindings through nanobind.

**Status:** in active development, before the first tagged release. The user interface and
the Python API are still moving. Signed macOS and portable Windows builds are produced by
CI — see [GitHub Actions: macOS DMG](#github-actions-macos-dmg) below.

## Documentation Index
- [Python Scripting](docs/python_scripting.md)
- [Design Documents](docs/design/) — indexed and grouped in
  [docs/design/README.md](docs/design/README.md), which separates reference
  documents (how MeshLab works today) from proposals (not implemented) and
  history (dated records of finished work):
  - Reference: [Architecture](docs/design/architecture.md) ·
    [Data Model](docs/design/data_model.md) ·
    [Rendering](docs/design/rendering.md) ·
    [Memory Accounting](docs/design/memory_accounting.md) ·
    [Preferences](docs/design/preferences.md) ·
    [Adding a Filter](docs/design/adding_a_filter.md) ·
    [Vocabulary](docs/design/vocabulary.md) ·
    [Filter Organization](docs/design/filter_organization.md) ·
    [TrueForm Plugin](docs/design/trueform_plugin.md)
  - Proposals: [LLM Integration](docs/design/proposals/llm_integration.md) ·
    [Usage Statistics](docs/design/proposals/usage_statistics.md) ·
    [Gaussian Splatting](docs/design/proposals/gaussian_splatting.md) ·
    [Repository Rename](docs/design/proposals/repository_rename.md)
  - History: [Filter Classification](docs/design/history/filter_classification.md) ·
    [Filter Names](docs/design/history/filter_names.md) ·
    [Pass 2 Identifier Map](docs/design/history/pass2_identifier_map.md)

## Current Features
- Single `Document` shared by one or more `RenderWidget` views
- Split-view UI (horizontal/vertical), active-view indicator, dual tree/table layer dock, log/filter docks, undo graph, and optional Python console dock
- Per-view mode switching between `3D Scene`, `Parametrization (UV)` (when the current mesh has UVs), and `Raster` (when the active layer is a raster)
- Interactive tool framework with layer picking, rubber-band selection, surface measuring, and layer transforms; tools are pinned to their owner view, can be suspended with `Tab` for camera navigation, and commit durable edits through filters for undo/script history
- Scene overlays for selection, normals, boundaries, texture seams, non-manifold markers, curvature directions, current-mesh outline, trackball/light/axis gizmos, tool guides, quality histogram, and optional decorator info counts
- PBR fill with albedo/normal/occlusion/roughness maps, tangent-space or object-space normal-map interpretation, plus Radiance Scaling
- Scene3D rendering organized as lightweight `RenderFrameRequest` pass requests -> GPU resource preparation -> concrete `RenderFramePlan` draw items -> pass executors
- Fill rendering modes (`Plain`, `Pbr`, `RadianceScaling`) isolated behind material renderers with shared fill services and an RS pre-pass hook
- Fat-edge rendering for explicit edge meshes with constant, interpolated per-vertex, or flat per-edge color; decorator boundaries/seams/non-manifold edges have independently configurable styling
- UV mode support for boundary-edge, texture-seam, and selection overlays on the current mesh; rubber-band selection can operate in UV space, while UV rendering remains a separate renderer with convergence toward the Scene3D material path planned next
- Raster mode displays the current raster as the view reference with aspect-preserving fit, pan/zoom navigation, and opacity control; rasters with camera shots reuse the Scene3D mesh pass pipeline through the raster camera
- Versioned camera/render-state JSON supports copy/paste, capture/apply, active-view snapshots, and headless offscreen snapshot workflows; concrete GPU `RenderFramePlan` objects remain internal and are not serialized
- Plugin-based mesh import/export with per-extension preferred import plugin, including PLY per-edge RGBA round trips, plus direct MeshLab project (`.mlp`) loading and saving for mesh/raster layer sets
- Plugin-based filter framework with searchable category tree, generated parameter dialogs, `pythonName` metadata, structured provenance/references, markdown descriptions, default reset, and compact/full Python call generation
- Filter parameters include mesh, texture, point/vector, camera-state, and render-state values; parameter panels can reset to descriptor defaults and source state JSON from the active view
- Raster projection filters can transfer current/all visible raster colors to vertex colors or bake visible rasters into a mesh texture atlas using existing wedge UVs
- Remeshing filters include layer-aware mesh parameters such as an alternate reference surface for isotropic remeshing reprojection/distance checks
- Embedded Python bindings when `MESHLAB2_PYTHON_CONSOLE=ON`; the in-app Python dock exposes the live document as `ms`, the live view helper as `mlgui`, and the public standalone facade as `pymeshlab`
- Tree-shaped undo/redo integrated with mesh operations, filter runs, selection-delta storage, camera/render-style snapshots, script-action history, branch pruning/linearization, and opt-in byte-budget/system-pressure purging
- Structured logging for app/VCG/error messages, load/filter progress, GPU buffer rebuild timing, and automatic undo-prune events; `Help > Memory Info` separates OS footprint, tracked CPU ownership, and logical GPU-cache sizes and can copy exact JSON for external profiling
- PNG snapshot export from the active view (custom resolution + embedded camera/trackball JSON metadata), plus snapshot-to-raster workflows

Built-in I/O plugin families (dependency-gated at build time):
- `io_vcg` (`ply`, `obj`, `stl`, `off`, `vmi`)
- `io_obj_rapidobj` (`obj`)
- `io_3mf` (`3mf`)
- `io_gltf` (`gltf`, `glb`)
- `io_e57` (`e57`, optional)
- `io_trueform` (`obj`, `stl`, optional)

MeshLab project files (`.mlp`) are loaded and saved directly by `Document`, combining mesh plugin I/O, mesh transforms, raster planes, and raster camera shots.

Built-in filter plugins (dependency-gated at build time). A plugin is a
dependency/build unit, not a user-facing grouping — the Filters menu is organized by
filter *category* instead, see [Vocabulary](docs/design/vocabulary.md):
- `filter_basic`
- `filter_camera`
- `filter_cgal`
- `filter_clean`
- `filter_color_projection`
- `filter_colorproc`
- `filter_create`
- `filter_embree`
- `filter_expression`
- `filter_geodesic`
- `filter_icp`
- `filter_igl`
- `filter_img_patch_param`
- `filter_instant_meshes`
- `filter_layer`
- `filter_measure`
- `filter_meshfix`
- `filter_meshing`
- `filter_mls`
- `filter_plymc`
- `filter_quadwild`
- `filter_qslim`
- `filter_sampling`
- `filter_screened_poisson`
- `filter_select`
- `filter_texture`
- `filter_texture_defragmentation`
- `filter_trioptimize`
- `filter_trueform`
- `filter_unsharp`
- `filter_vertex_displacement`
- `filter_voronoi`
- `filter_xatlas`

## Build

### Recommended: vcpkg manifest mode

Prerequisites:
- Qt 6.11+ with Qt SVG
- CMake 3.25+
- Build tool: `ninja` or `make`
- vcpkg clone
- Python development libraries when `MESHLAB2_PYTHON_CONSOLE=ON` (default)

This repository contains `vcpkg.json`; non-Qt dependencies are installed via vcpkg manifest mode.
Qt6 and Python are intentionally kept outside vcpkg. `vcglib`, selected
algorithm archives such as MeshFix and QSlim, and the math-only JKQTMathText
dependency are git submodules. `nanobind` is provided through vcpkg and is used
for the private `_meshlab` extension behind the embedded `pymeshlab` facade.

### Dependency Installation

The dependencies listed in `vcpkg.json` (for example `rapidobj`, `draco`, `libigl`, etc.) are installed automatically using vcpkg's manifest mode. After bootstrapping vcpkg, simply run the following command to install all required dependencies:

```powershell
vcpkg install --triplet x64-windows
```

This will ensure all non-Qt dependencies are set up correctly. Qt6 remains outside of vcpkg and must be installed separately.

### Setup

```bash
git submodule update --init --recursive
git clone https://github.com/microsoft/vcpkg ./vcpkg
./vcpkg/bootstrap-vcpkg.sh
```

If your vcpkg is elsewhere, set:

```bash
export VCPKG_ROOT="/absolute/path/to/vcpkg"
```

### Build from terminal

Release:

```bash
cmake --preset vcpkg-release
cmake --build --preset vcpkg-release -j8
./build-release/MeshLab
```

Debug:

```bash
cmake --preset vcpkg-debug
cmake --build --preset vcpkg-debug -j8
./build-debug/MeshLab
```

Notes:
- `vcpkg-debug` and `vcpkg-release` are defined in `CMakeUserPresets.json`.
- They use `Unix Makefiles` and set `VCPKG_ROOT=${sourceDir}/vcpkg` explicitly, which helps in GUI environments (for example VS Code) where shell environment variables are not always inherited.

### Build from VS Code (CMake Tools)

1. `CMake: Select Configure Preset` -> choose `vcpkg-debug` or `vcpkg-release`.
2. `CMake: Configure`
3. `CMake: Build`
4. Launch with the CMake Tools run/debug actions.

If presets were changed and VS Code still uses stale values:
1. `CMake: Delete Cache and Reconfigure`
2. `Developer: Reload Window`

### Optional: local minimal build without vcpkg

```bash
git submodule update --init --recursive
cmake --preset local-no-vcpkg
cmake --build --preset local-no-vcpkg
./build-local/MeshLab
```

The local minimal preset disables dependency-heavy plugins (`io_gltf`, `io_e57`, `io_obj_rapidobj`). Because the embedded Python console is enabled by default, this path still needs local Python development files and nanobind. If you want a lean viewer-only build without those, configure with `-DMESHLAB2_PYTHON_CONSOLE=OFF`.
LaTeX rendering in filter help can likewise be disabled with
`-DMESHLAB2_MATH_HELP=OFF`.

## GitHub Actions: macOS DMG

The repository includes a manual GitHub Actions workflow at
`.github/workflows/macos-dmg.yml` that builds, signs, and notarizes an Apple
Silicon macOS `.dmg`.

What it does:
- checks out the repo with submodules
- installs the build tools, `libomp`, and the GNU autotools needed by vcpkg
- installs Qt 6.11 with `install-qt-action`
- reuses the action cache for Qt downloads/install files when available
- bootstraps local `vcpkg`
- configures a `Release` build from the tracked `vcpkg-manifest` preset
- generates a proper macOS `.icns` from the MeshLab app icon and embeds it in the app bundle
- bundles `libomp.dylib` into the app when OpenMP-linked plugins are present
- applies the same icon to the mounted DMG volume when the runner provides `SetFile`
- signs the app, frameworks, and plugins with a Developer ID Application certificate
- enables hardened runtime and secure timestamps
- submits the DMG to Apple's notary service, staples the ticket, and verifies it
- uploads the signed and notarized DMG as
  `MeshLab-YYYY-MM-DD-<short-sha>-macos-arm64.dmg`

Required repository secrets:
- `MACOS_CERTIFICATE_P12`: the base64-encoded `.p12` containing the Developer ID
  Application certificate and its private key
- `MACOS_CERTIFICATE_PASSWORD`: the password used when exporting that `.p12`
- `APPLE_API_KEY_ID`: the App Store Connect API key ID
- `APPLE_API_ISSUER_ID`: the App Store Connect API issuer ID
- `APPLE_API_PRIVATE_KEY`: the complete `.p8` private key, including its
  `BEGIN PRIVATE KEY` and `END PRIVATE KEY` lines

Create a **Developer ID Application** certificate in the Apple Developer portal,
install it in Keychain Access, and export the certificate together with its private
key as a password-protected `.p12`. On macOS, its secret value can be prepared with
`base64 -i DeveloperID.p12 | pbcopy`. Create the notarization API key under App
Store Connect's **Users and Access > Integrations** section. The `.p8` file can be
downloaded only once, so keep the original in a secure location.

How to use it:
1. Open the `Actions` tab on GitHub
2. Select `macOS DMG`
3. Click `Run workflow`
4. Download the `MeshLab-YYYY-MM-DD-<short-sha>-macos-arm64` artifact from the completed run

The workflow runs on `macos-15`, producing an `arm64` application. The resulting DMG
therefore requires an Apple Silicon Mac: Rosetta does not help here, so Intel Macs are
not covered by a prebuilt package and have to build from source. The packaging
script still produces an ad-hoc signed DMG when used locally without
`--sign-identity`; notarization is performed only by the GitHub Actions workflow.

## GitHub Actions: Windows Portable ZIP

The repository also includes a manual GitHub Actions workflow at
`.github/workflows/windows-portable.yml` that builds a portable Windows `.zip`.

What it does:
- checks out the repo with submodules
- sets up MSVC on `windows-2022`
- installs Qt 6.11 from the public `download.qt.io` package archives and caches the local Qt SDK directory
- bootstraps local `vcpkg`
- installs vcpkg dependencies in a separate manifest step before CMake configure
- uses a custom release-only Windows vcpkg triplet so CI does not build debug dependency variants too
- configures and builds a release build with the `vcpkg-manifest` preset
- runs `windeployqt` on `MeshLab.exe`
- copies additional runtime `.dll` files from the release-only manifest `vcpkg_installed/<triplet>/bin`
- archives the deploy directory as
  `MeshLab-YYYY-MM-DD-<short-sha>-windows-x86_64.zip`
- uploads the generated `.zip` as a workflow artifact

How to use it:
1. Open the `Actions` tab on GitHub
2. Select `Windows Portable`
3. Click `Run workflow`
4. Download the `MeshLab-YYYY-MM-DD-<short-sha>-windows-x86_64` artifact from the completed run

Current status:
- packaging is portable `.zip`, not an installer
- the workflow targets `x64` on `windows-2022`
- code signing can be added later if you want a more polished distribution path

## Setting up the development environment on Windows

To build on Windows you need:

1. [Visual Studio 2022](https://aka.ms/vs/17/release/) with the Desktop C++ workload.
2. [CMake 3.25+](https://cmake.org/download/).
3. [Git](https://git-scm.com/).
4. [Qt 6.11](https://www.qt.io/download/).
5. A local [vcpkg](https://github.com/microsoft/vcpkg) checkout.

Setup steps:

```powershell
git submodule update --init --recursive
git clone https://github.com/microsoft/vcpkg .\vcpkg
.\vcpkg\bootstrap-vcpkg.bat
$env:VCPKG_ROOT = (Resolve-Path .\vcpkg)
cmake --preset default
cmake --build --preset default
```

Notes:
- The `default` preset uses vcpkg manifest mode through `VCPKG_ROOT`.
- Packages from `vcpkg.json` such as `draco`, `rapidobj`, `tinygltf`, `libe57format`, `xerces-c`, `muparser`, `embree`, `nanobind`, `cgal`, and `libigl` are resolved automatically during configure. Do not install them manually.
- Qt stays outside vcpkg and must be installed separately.
