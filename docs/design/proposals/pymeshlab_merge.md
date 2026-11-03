# Folding the pymeshlab2 wheel into this repository

The Python wheel is built from a separate repository, `cignoni/pymeshlab2`, which
carries MeshLab as a git submodule. This document proposes moving that build here
and retiring the separate repository, and records the mapping table required by
`AGENTS.md` before anything moves.

**Status (2026-11-03):** both tables approved. The in-app facade rename is done,
and so are steps 2 and 3: `pip wheel .` builds `pymeshlab2` from this tree, the
wheel exposes the same 405 filters as the application, and `pytest` runs green
against it. Steps 4–6 (CI, tag, archive) have not started.

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

- The distribution and the import name are both **`pymeshlab2`**, and the
  in-app facade follows (see [The in-app facade](#the-in-app-facade)). `pymeshlab`
  on PyPI belongs to the legacy PyMeshLab line (`cnr-isti-vclab/PyMeshLab`, latest
  `2025.7.post1`, versioned `YYYY.M[.postN]`), which is still maintained.
  Publishing this API under that name as `2026.11` would sort as an ordinary
  upgrade, so an unpinned `pip install -U pymeshlab` would swap an incompatible
  API in silently, and the shared import name would stop the two from being
  installed side by side while scripts are ported. A calendar version cannot
  signal that break; a distinct name does. `pymeshlab2` has never been published
  (PyPI returns 404), so nobody is broken by adopting it, and it follows the
  `AGENTS.md` rule that code identifiers follow the repository name.
- `cignoni/pymeshlab2` is **archived, not deleted**, and not until a tag-built
  wheel has gone green from here.

## Mapping table

23 files live outside the submodule in `cignoni/pymeshlab2`. Target paths are
relative to this repository.

| # | Current (`cignoni/pymeshlab2`) | Target | Measured evidence | Verdict |
|---|---|---|---|---|
| 1 | `vcpkg.json` | — | Same `builtin-baseline` (`37bb045f…`) as this repository's manifest; 14 dependencies, all 14 also in ours, zero wheel-only. The 9 ours adds are **lag, not a leaner design**: the wheel manifest was last touched 2026-10-13, and `qhull`, `nanoflann`, `fmt`, `jsoncpp` arrived here 2026-10-16 for `filter_open3d`, `dirent` on 10-18, `openblas` on 10-21, `libpng`/`libjpeg-turbo` on 10-28 for Texture Consolidation. With `MESHLAB2_REQUIRE_VCPKG_DEPS OFF`, the plugins needing them drop out of the wheel silently. (`nanobind` the wheel gets from pip instead; see the vcpkg `python-console` feature under Known wrinkles.) | **Drop** — a stale copy; use the root manifest |
| 2 | `.gitmodules` | — | Single entry, `external/meshlab2`. Becomes meaningless once the build is in-tree. | **Drop** |
| 3 | `LICENSE` | — | GPL-3.0 text, byte-identical to ours. | **Drop** — duplicate |
| 4 | `README` (reStructuredText) | `python/README.md` | Describes the Phase A scaffold; most of it is obsolete. PyPI still needs a long description for `[project.readme]`. | **Rewrite**, much shorter |
| 5 | `pyproject.toml` | `pyproject.toml` (repo root) | `name = "pymeshlab2"` kept; `version = "0.0.1"` → derived from `MESHLAB2_VERSION` (`CMakeLists.txt:59`); `requires-python` `>=3.9` → `>=3.10`, the oldest interpreter the CI matrix builds for. Root placement avoids `cmake.source-dir` pointing at a parent, which would force the sdist to carry the whole tree anyway. | **Move + edit** |
| 6 | `pytest.ini` | `pyproject.toml` `[tool.pytest.ini_options]` | Three marker definitions only (`examples`, `filters`, `smoke`). | **Merge** into row 5 |
| 7 | `.gitignore` | `.gitignore` | Ours has **no** Python entries (`__pycache__`, `*.so`, `dist/`, `*.egg-info/`, `.pytest_cache/`, `wheelhouse/`, `_skbuild/`). Theirs also ignores `.reference/`, which ours already does. | **Merge** the Python block |
| 8 | `CMakeLists.txt` | split three ways | 96 lines. Lines 10–23: nine `MESHLAB2_*` toggles. Lines 28–50: nanobind discovery with a FetchContent fallback — **redundant here**, since `find_package(nanobind CONFIG REQUIRED)` already runs at `CMakeLists.txt:170` and `nanobind` is in our vcpkg manifest. Lines 52–64: `sources.cmake` + `nanobind_add_module(_meshlab …)` + link `MeshLab2Plugins`. Lines 66–95: install rules for the module, quadwild binaries and config, and the geogram DLL. | **Split** — see below |
| 9 | `.github/workflows/wheels.yml` | `.github/workflows/wheels.yml` | ~300 lines. Qt install, vcpkg bootstrap/baseline/cache/install, overlay triplets, failure-log upload — all already present in our three packaging workflows. Genuinely new: the `cibuildwheel@v4.2.0` step, the `CIBW_*` matrix (cp310–cp314, macOS arm64 + Windows x64), and the two `CMAKE_ARGS` composition steps. | **Move + collapse** onto existing steps |
| 10 | `scripts/repair_macos_wheel.py` | `scripts/repair_macos_wheel.py` | Rewrites Qt framework install names inside the wheel and re-signs; no counterpart in `scripts/` (which holds `package-macos-dmg.sh`, `package-linux-appimage.sh`). | **Move** unchanged |
| 11 | `python/pymeshlab2/__init__.py` | `python/pymeshlab2/__init__.py` | ~35 lines: `MESHLAB2_QUADWILD_ROOT` default, geogram runtime preload for macOS/Windows, then `from ._meshlab import FilterInfo, FilterRunResult, Mesh, MeshSet`. | **Move** |
| 12 | `examples/__init__.py` | — | Empty; existed so `test_examples.py` could import the examples as a package. | **Dropped** in step 3 (was *Move*): the test now loads examples by path, see below |
| 13–18 | `examples/{create_noisy_isosurface,list_filters,mesh_info,normalize_and_save,raster_basics,wavy_box}.py` | `python/examples/` | 17–43 lines each, 166 total. Imported and run by `test_examples.py`, so they are regression tests as well as documentation. | **Move**; two default-mesh paths edited |
| 19 | `tests/test_examples.py` | `python/tests/test_examples.py` | 89 lines; runs each example and checks it completes. | **Move + edit**, see below |
| 20 | `tests/test_mesh.py` | `python/tests/test_mesh.py` | 24 lines; exercises the `Mesh` bindings added in `pymesh.cpp`. | **Move + edit** paths |
| 21 | `tests/test_single_mesh_filters.py` | — | 64 lines. Reads `external/meshlab2/plugins/*/filters.json`, intersects with `list_filters()`, runs each `SingleMesh` filter with defaults in a subprocess, skipping on `applicable == false`. | **Dropped** in step 3 (was *Move + simplify paths*), replaced by a parity test; see below |
| 22 | `tests/test_smoke.py` | `python/tests/test_smoke.py` | 63 lines; import and basic MeshSet scaffold checks. | **Move + edit**; gains the parity test |
| 23 | `tests/wheel_smoke.py` | `python/tests/wheel_smoke.py` | 37 lines; the `CIBW_TEST_COMMAND`. Asserts three named filters exist, runs two, and checks the bundled geogram library and quadwild binaries are present and loadable. | **Move** |

**Totals:** 23 files → 5 dropped (rows 1–3, and 12 and 21 in step 3), 2 folded into
other files (rows 6–7), 1 split across three destinations (row 8), 1 rewritten
(row 4), 14 moved (rows 5, 9–11, 13–20, 22–23). Keeping the `pymeshlab2` name means
no file in this table is renamed.

### Step 3, in detail

The tests came over with three kinds of edit beyond their paths, all for the
same reason: in this tree, a test that skips is a test that hides a failure.

- **`pytest.importorskip("pymeshlab2")` became a plain import.** A broken or
  missing wheel used to turn the whole suite into skips and a green run.
- **Path guards went.** `test_mesh.py` and `test_smoke.py` found the sample mesh
  through a working-directory-relative `external/meshlab2/…` path and skipped when
  it was absent, which in-tree it always would have been. They now resolve it from
  the test file; so do the two examples. "Filter missing → skip" guards went too:
  a missing filter is now a build defect, caught by the parity test.
- **Examples are loaded by file path**, not imported as `examples.<name>`. Putting
  `python/` on `sys.path` to make the package importable would also make
  `import pymeshlab2` find the source package, which has no compiled module,
  ahead of the installed wheel.

`test_single_mesh_filters.py` was **replaced, not moved**. Its contract — every
filter either runs or refuses — is the one `tests/test_filter_smoke.cpp` already
owns, with a fixture ladder and an expected-refusals table. The Python copy ran
one fixture and counted refusals as failures, so in-tree it failed 87 filters for
saying, correctly, that a single triangle has no second layer or no rasters. What
it *could* catch that ctest cannot, a plugin missing from the wheel, is now
asserted directly: `test_wheel_exposes_every_declared_filter` requires the wheel's
filter ids to equal the ids in `plugins/*/filters.json`, both ways.

It still earned its keep on the way out. `tests/data/simple.off` is a single open
triangle, an input the C++ ladder (which starts from a closed sphere) never
gives, and on it, in the application as well as the wheel:
`subdivide_by_doo_sabin` segfaults; `remove_isolated_components_by_face_count`
refuses with "Missing component: PerFaceMark", an internal error rather than an
input requirement; and `create_voronoi_scaffolding`, `sample_volume` and
`remesh_to_quads_quadwild_bimdf` do not finish within 60 seconds. These are filter
bugs, and the fix belongs in the C++ smoke ladder (a degenerate rung), not in a
second sweep here.

### Row 8, in detail

| Part of the wheel's `CMakeLists.txt` | Target |
|---|---|
| The nine `MESHLAB2_*` toggles | Two kept, `PYTHON_CONSOLE` and `MATH_HELP` off, in `pyproject.toml` `[tool.scikit-build.cmake.define]` rather than the planned `CMakePresets.json` preset: scikit-build-core applies those on every `pip` build, while a preset would sit unused there. The other seven are **dropped**. `REQUIRE_VCPKG_DEPS` stays on, so a missing dependency now fails the build instead of thinning the wheel. The `RAPIDOBJ`/`E57`/`GLTF` plugins build fine here. The three submodule-presence checks are moot in-tree. |
| nanobind discovery + FetchContent fallback | Dropped. The existing `find_package(nanobind CONFIG REQUIRED)` now runs for console *or* wheel, and resolves to pip's nanobind for the wheel |
| `nanobind_add_module(_meshlab …)` + `MeshLab2Plugins` link | `src/python/CMakeLists.txt`, behind the new `MESHLAB2_PYTHON_WHEEL` option, which also turns on `CMAKE_POSITION_INDEPENDENT_CODE` |
| Install rules (module, quadwild bin + config + licence, geogram DLL) | Same place, same guard, in an install component `pymeshlab2`; `pyproject.toml` builds only `_meshlab` and installs only that component |

## The in-app facade

*Done 2026-11-03.* The application's console registers its own facade module
(`src/python/PythonHost.cpp:222–230`, a `types.ModuleType` placed in
`sys.modules`), and it was called `pymeshlab`; the user documentation taught
`import pymeshlab` against it. If the
wheel is `pymeshlab2` and the console is `pymeshlab`, a script written in one does not
run in the other — the portability the shared `sources.cmake` exists to give. So the
facade is renamed too, with no alias, per the no-compatibility-names rule for the
Python API.

Measured with `grep -o 'pymeshlab[A-Za-z0-9_]*'` keeping exact `pymeshlab` tokens only,
over tracked files outside `docs/design/history/` and `docs/design/proposals/`: **16
files, 50 occurrences**. Two of them name the *legacy* PyMeshLab and stay.

| File | Occurrences | What they are | Verdict |
|---|---|---|---|
| `src/python/PythonHost.cpp` | 12 | 11 in the facade registration (222–230); 1 at 214, "mirroring pymeshlab's approach", refers to legacy PyMeshLab | **Rename 11**, keep 1 |
| `src/ui/scripteditor.cpp` | 1 | Highlighted builtin name in the script editor (61) | **Rename** |
| `src/python/bindings/sources.cmake` | 1 | Comment naming the wheel consumer | **Rename** |
| `src/python/bindings/meshset_core.h` | 1 | Comment | **Rename** |
| `src/python/mlgui.h` | 1 | Comment | **Rename** |
| `src/core/headlessrendercontext.h` | 1 | Comment | **Rename** |
| `docs/python_scripting.md` | 19 | The user-facing guide; line 35, "If you already know `pymeshlab`", addresses legacy users | **Rename 18**, keep 1 (and say "legacy PyMeshLab" there) |
| `docs/index.rst` | 2 | API docs landing page | **Rename** |
| `docs/generate_api.py` | 1 | Emits `api/mlgui.rst` (untracked, regenerated) | **Rename** |
| `docs/examples/texmetro_report.py` | 3 | Docstring and `import pymeshlab` | **Rename** |
| `docs/texture_metrics.md` | 1 | Prose | **Rename** |
| `docs/design/architecture.md` | 2 | Lines 92, 287 | **Rename** |
| `docs/design/data_model.md` | 1 | Line 160 | **Rename** |
| `docs/design/rendering.md` | 1 | Line 563, `pymeshlab.MeshSet.render_snapshot` | **Rename** |
| `docs/design/raster_alignment.md` | 1 | Line 63 | **Rename** |
| `readme.md` | 2 | Lines 55, 122 | **Rename** |

**Totals:** 16 files, 50 occurrences → 48 renamed, 2 kept as references to legacy
PyMeshLab. `docs/design/history/` is left alone: it records the past. This set is
independent of the wheel move and can land first, as its own commit.

## Known wrinkles

- *Resolved in step 2:* `find_package(nanobind)` sat inside the
  `MESHLAB2_PYTHON_CONSOLE` guard; it now runs for either option.
- *Resolved in step 2:* the version comes from scikit-build-core's
  `scikit_build_core.metadata.regex` provider reading `MESHLAB2_VERSION` out of
  `CMakeLists.txt`; the local build produced `pymeshlab2-2026.11`.
- **vcpkg's `python3` hijacks `find_package(Python)`.** Not foreseen by the
  table. The `nanobind` port depends on `python3`, and that port installs a
  `vcpkg-cmake-wrapper.cmake` which, whenever `Development.Module` is requested,
  points FindPython at vcpkg's own 3.12 headers. The configure then fails against
  any other interpreter. The sibling never met it because its manifest lacked
  `nanobind`. Fix: `nanobind` moved into a `python-console` manifest feature,
  on by default (so the application, which embeds that very `python3`, is
  unchanged), and the wheel sets `VCPKG_MANIFEST_NO_DEFAULT_FEATURES` and takes
  nanobind from pip (`build-system.requires`, `nanobind>=2.13`; 3.1.0 built
  cleanly). Consequence for step 4: the wheel needs its own vcpkg install tree; it
  cannot reuse an application build's.
- **The local wheel is tagged `macosx_26_0`**, the build machine's OS, because
  scikit-build-core reads `MACOSX_DEPLOYMENT_TARGET` from the environment and not
  from `CMakeLists.txt`'s 15.0 floor. CI must export `MACOSX_DEPLOYMENT_TARGET=15.0`.
- **An unrepaired macOS wheel does not import**: `_meshlab` links
  `@rpath/libgeogram.1.9.3.dylib` and Qt frameworks that only
  `scripts/repair_macos_wheel.py` bundles. Row 10 was therefore moved in step 2,
  unchanged, to prove the wheel locally (`QT_ROOT_DIR`, `VCPKG_INSTALLED_DIR`,
  `VCPKG_TARGET_TRIPLET` and `delocate` needed).
- **`ci/vcpkg-triplets/` has no `x64-windows-release.cmake`** — both repositories
  name that triplet with the overlay set, and both fall through to vcpkg's
  builtin. Consistent today, but it means the overlay is load-bearing only on
  macOS and Linux.
- **The test story becomes two-headed**: `ctest` for C++, `pytest` for the wheel.
  Kept small on purpose: `pytest` covers only what is specific to the wheel (the
  binding surface, the examples, and that **the filters a wheel exposes match the
  descriptors in this tree**, the gap recorded in [LLM Integration](llm_integration.md));
  filter behaviour stays in the three ctest tiers.
- **cibuildwheel's isolated build environment** must see the vcpkg install tree
  the way the existing workflows lay it out. This is the one mechanically unknown
  piece and should be proven on a branch before the sibling repo is archived.

## Order of operations

1. Land the table (this document), approved. *Done*, together with the in-app
   facade rename, which touches nothing the wheel move does.
2. In-tree build: rows 5–8, 11 (and 10, see Known wrinkles). Prove `pip wheel .`
   produces an importable `pymeshlab2` locally. *Done*: the repaired cp313 wheel
   imports from a clean venv, lists 405 filters (identical to the application's
   set), and passes the sibling's `wheel_smoke.py`.
3. Tests and examples: rows 12–23. Prove `pytest` green locally. *Done*: 12
   tests pass with none skipped, against the repaired cp313 wheel; the moved
   `wheel_smoke.py` passes too. Run from the repository root with the wheel
   installed: `python -m pytest`.
4. CI: row 9, plus a `wheels` job in `release.yml`. Prove green on `devel`.
5. Tag a release; confirm the wheel builds from the tag.
6. Only then archive `cignoni/pymeshlab2`.

Steps 2–5 are each independently revertible; step 6 is the point of no easy return.
