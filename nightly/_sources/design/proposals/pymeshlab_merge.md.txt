# Folding the pymeshlab wheel into this repository

The Python wheel is built from a separate repository, `cignoni/pymeshlab2`, which
carries MeshLab as a git submodule. This document proposes moving that build here
and retiring the separate repository, and records the mapping table required by
`AGENTS.md` before anything moves.

**Nothing here has been done.** The table below is for approval first.

See also: [LLM Integration](llm_integration.md) (why wheel/app convergence came up),
[Branches and Releases](../release_process.md) (the release machinery this would join),
[Python Scripting](../../python_scripting.md) (the API the wheel exposes).

## Why

Three reasons, in order of weight.

1. **The submodule pin is a standing staleness bug.** The wheel repository pins
   MeshLab and that pin runs behind — 135 commits at the time of writing, so a
   locally built extension exposed 304 filters against 403 declared here.
   `AGENTS.md` already names this repository's submodule hygiene as something the
   sibling repo suffers from; merging removes one of the two layers.
2. **The duplication is in CI, and it is large.** `wheels.yml` re-implements the
   Qt install, the vcpkg bootstrap pinned to the manifest baseline, the overlay
   triplets and the vcpkg artifact caching that `tests.yml`, `macos-dmg.yml` and
   `windows-portable.yml` already perform, on the same two platforms the wheel
   targets. It already reaches back through the submodule for the triplets.
3. **The release machinery here already has the right shape.** `release.yml`
   orchestrates three reusable workflows on a `YYYY.MM` tag, each of which
   supports `workflow_dispatch`, `workflow_call` and a `devel` push. A wheel job
   fits that pattern without inventing anything, and inherits the tag/version
   check — which would also give the wheel a real version, replacing the
   `0.0.1` it has never moved off.

## Decisions taken

- The published module becomes **`pymeshlab`**, matching the name `AGENTS.md`
  declares. The current `pymeshlab2` name is retired at the same moment, so users
  take one breaking change rather than two.
- `cignoni/pymeshlab2` is **archived, not deleted**, and not until a tag-built
  wheel has gone green from here.

## Mapping table

23 files live outside the submodule in `cignoni/pymeshlab2`. Target paths are
relative to this repository.

| # | Current (`cignoni/pymeshlab2`) | Target | Measured evidence | Verdict |
|---|---|---|---|---|
| 1 | `vcpkg.json` | — | Same `builtin-baseline` (`37bb045f…`) as this repository's manifest; 14 dependencies, **all 14 also in ours**, zero wheel-only. Ours adds 9 more (`nanobind`, `openblas`, `qhull`, `fmt`, `jsoncpp`, `libpng`, `libjpeg-turbo`, `nanoflann`, `dirent`). | **Drop** — strict subset; use the root manifest |
| 2 | `.gitmodules` | — | Single entry, `external/meshlab2`. Becomes meaningless once the build is in-tree. | **Drop** |
| 3 | `LICENSE` | — | GPL-2.0-or-later, same as ours. | **Drop** — duplicate |
| 4 | `README` (reStructuredText) | `python/README.md` | Describes the Phase A scaffold; most of it is obsolete. PyPI still needs a long description for `[project.readme]`. | **Rewrite**, much shorter |
| 5 | `pyproject.toml` | `pyproject.toml` (repo root) | `name = "pymeshlab2"` → `pymeshlab`; `version = "0.0.1"` → derived from `MESHLAB2_VERSION` (`CMakeLists.txt:59`). Root placement avoids `cmake.source-dir` pointing at a parent, which would force the sdist to carry the whole tree anyway. | **Move + edit** |
| 6 | `pytest.ini` | `pyproject.toml` `[tool.pytest.ini_options]` | Three marker definitions only (`examples`, `filters`, `smoke`). | **Merge** into row 5 |
| 7 | `.gitignore` | `.gitignore` | Ours has **no** Python entries (`__pycache__`, `*.so`, `dist/`, `*.egg-info/`, `.pytest_cache/`, `wheelhouse/`, `_skbuild/`). Theirs also ignores `.reference/`, which ours already does. | **Merge** the Python block |
| 8 | `CMakeLists.txt` | split three ways | 96 lines. Lines 10–23: nine `MESHLAB2_*` toggles. Lines 28–50: nanobind discovery with a FetchContent fallback — **redundant here**, since `find_package(nanobind CONFIG REQUIRED)` already runs at `CMakeLists.txt:170` and `nanobind` is in our vcpkg manifest. Lines 52–64: `sources.cmake` + `nanobind_add_module(_meshlab …)` + link `MeshLab2Plugins`. Lines 66–95: install rules for the module, quadwild binaries and config, and the geogram DLL. | **Split** — see below |
| 9 | `.github/workflows/wheels.yml` | `.github/workflows/wheels.yml` | ~300 lines. Qt install, vcpkg bootstrap/baseline/cache/install, overlay triplets, failure-log upload — all already present in our three packaging workflows. Genuinely new: the `cibuildwheel@v4.2.0` step, the `CIBW_*` matrix (cp310–cp314, macOS arm64 + Windows x64), and the two `CMAKE_ARGS` composition steps. | **Move + collapse** onto existing steps |
| 10 | `scripts/repair_macos_wheel.py` | `scripts/repair_macos_wheel.py` | Rewrites Qt framework install names inside the wheel and re-signs; no counterpart in `scripts/` (which holds `package-macos-dmg.sh`, `package-linux-appimage.sh`). | **Move** unchanged |
| 11 | `python/pymeshlab2/__init__.py` | `python/pymeshlab/__init__.py` | ~35 lines: `MESHLAB2_QUADWILD_ROOT` default, geogram runtime preload for macOS/Windows, then `from ._meshlab import FilterInfo, FilterRunResult, Mesh, MeshSet`. | **Move + rename** |
| 12 | `examples/__init__.py` | `python/examples/__init__.py` | Empty; exists so `test_examples.py` can import the package. | **Move** |
| 13–18 | `examples/{create_noisy_isosurface,list_filters,mesh_info,normalize_and_save,raster_basics,wavy_box}.py` | `python/examples/` | 17–43 lines each, 166 total. Imported and run by `test_examples.py`, so they are regression tests as well as documentation. | **Move + rename import** |
| 19 | `tests/test_examples.py` | `python/tests/test_examples.py` | 89 lines; runs each example and checks it completes. | **Move + rename import** |
| 20 | `tests/test_mesh.py` | `python/tests/test_mesh.py` | 24 lines; exercises the `Mesh` bindings added in `pymesh.cpp`. | **Move + rename import** |
| 21 | `tests/test_single_mesh_filters.py` | `python/tests/test_single_mesh_filters.py` | 64 lines. Reads `external/meshlab2/plugins/*/filters.json`, intersects with `list_filters()`, runs each `SingleMesh` filter with defaults in a subprocess, skipping on `applicable == false`. Paths simplify to `plugins/*/filters.json` and `tests/data/simple.off` (present, 36 bytes). | **Move + simplify paths** |
| 22 | `tests/test_smoke.py` | `python/tests/test_smoke.py` | 63 lines; import and basic MeshSet scaffold checks. | **Move + rename import** |
| 23 | `tests/wheel_smoke.py` | `python/tests/wheel_smoke.py` | 37 lines; the `CIBW_TEST_COMMAND`. Asserts three named filters exist, runs two, and checks the bundled geogram library and quadwild binaries are present and loadable. | **Move + rename import** |

**Totals:** 23 files → 3 dropped (rows 1–3), 2 folded into other files (rows 6–7),
1 split across three destinations (row 8), 1 rewritten (row 4), 16 moved with edits.

### Row 8, in detail

| Part of the wheel's `CMakeLists.txt` | Target |
|---|---|
| The nine `MESHLAB2_*` toggles | A `wheel` preset in `CMakePresets.json`, so the app build stays ignorant of them |
| nanobind discovery + FetchContent fallback | Dropped — already at `CMakeLists.txt:170` |
| `nanobind_add_module(_meshlab …)` + `MeshLab2Plugins` link | `src/python/CMakeLists.txt`, behind a new `MESHLAB2_PYTHON_WHEEL` option |
| Install rules (module, quadwild bin + config + licence, geogram DLL) | Same place, same guard |

## Known wrinkles

- **`find_package(nanobind)` sits inside the `MESHLAB2_PYTHON_CONSOLE` guard**
  (`CMakeLists.txt:165–170`), and the wheel build sets that option `OFF`. The
  condition has to become `if(MESHLAB2_PYTHON_CONSOLE OR MESHLAB2_PYTHON_WHEEL)`,
  or nanobind lookup moves out of the guard entirely.
- **Deriving the wheel version from `MESHLAB2_VERSION`** needs a scikit-build-core
  metadata provider; the exact mechanism is unverified here and should be
  confirmed against the installed version before the table is executed.
- **`ci/vcpkg-triplets/` has no `x64-windows-release.cmake`** — both repositories
  name that triplet with the overlay set, and both fall through to vcpkg's
  builtin. Consistent today, but it means the overlay is load-bearing only on
  macOS and Linux.
- **The test story becomes two-headed**: `ctest` for C++, `pytest` for the wheel.
  That is a mild complication but also a gain — `test_single_mesh_filters.py`
  exercises the wheel path `ctest` structurally cannot reach, and post-move it
  becomes the natural place to assert that **the filters a wheel exposes match
  the descriptors in this tree**, the gap recorded in
  [LLM Integration](llm_integration.md).
- **cibuildwheel's isolated build environment** must see the vcpkg install tree
  the way the existing workflows lay it out. This is the one mechanically unknown
  piece and should be proven on a branch before the sibling repo is archived.

## Order of operations

1. Land the table (this document), approved.
2. In-tree build: rows 5–8, 11. Prove `pip wheel .` produces an importable
   `pymeshlab` locally.
3. Tests and examples: rows 12–23. Prove `pytest` green locally.
4. CI: row 9, plus a `wheels` job in `release.yml`. Prove green on `devel`.
5. Tag a release; confirm the wheel builds from the tag.
6. Only then archive `cignoni/pymeshlab2`.

Steps 2–5 are each independently revertible; step 6 is the point of no easy return.
