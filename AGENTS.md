# MeshLab — working notes for coding agents

A ground-up rewrite of MeshLab: a single-document, multi-view Qt 6 application on QRhi,
vcglib, and a statically-linked plugin architecture, with several hundred filters in about
forty plugins and optional Python bindings through nanobind.

This file carries what is **not** written down elsewhere in the repo. Anything about how
the code works belongs in `docs/design/` — read from there, and add to there, rather than
growing this file.

## Three names, deliberately different

| | |
|---|---|
| Product / display name | **MeshLab** — bundle, window title, About box, bundle id `net.meshlab.MeshLab` |
| Repository and directory | **meshlab2** (`cnr-isti-vclab/meshlab2`) |
| Code identifiers | **`MeshLab2*`**, `MESHLAB2_*`, `meshlab2.filter.*` — they follow the repo, not the product |
| Python | distribution and module **`pymeshlab2`**, native `_meshlab` — not `pymeshlab`, which is the legacy PyMeshLab on PyPI |

The rename landed 2026-09. Anything written before it — older transcripts, `git log`
messages, docs in `docs/design/history/` — names targets like `QMeshLabCore` and options
like `QMESH_PLUGIN_*` that no longer exist. **Translate old names on sight rather than
running them**: CMake accepts an unknown `-D` silently, so a stale flag copied from
history changes the build without failing. `MeshLabProject*` (the classes that read
MeshLab's `.mlp` format) keeps its name on purpose.

## Working agreements

**Git is the maintainer's.** Never run a state-changing git command — commit, checkout,
branch, merge, `submodule update`, gitlink bump. Prepare the tree, verify it, say plainly
what changed, and hand over the exact command. Reading state (`status`, `log`, `diff`,
`show`, `grep`) is expected. **Never `git stash`**: the maintainer edits and commits in
this same worktree while a session runs, so a stash sweeps their in-flight work into your
entry and the pop then conflicts on files you never touched. To compare against a
baseline use `git show HEAD:<path>` or a separate `git worktree`. Outside the repo, the one
thing allowed is reading CI: `gh run list`, `gh run view --log-failed`, `gh api` reads.

**The handover.** List the changed files and paste the full commit message inline: the
maintainer reads it before committing. Files that were already modified when the session
started are the maintainer's work in progress — run `git status` at the start and again
before handing over, and leave them out unless the change was made to them on request.

**Say what was verified, and what was not.** Report what ran, with numbers (tests passed,
pixels compared, timings and on which build). Some things cannot be exercised headlessly —
a click in the 3D view, a native menu, a drag and drop — and the report says so rather than
implying they were. Then rebuild `build-release` from the final source: it is the binary the
maintainer launches.

**Least code wins.** Reuse before adding: vcglib (cloned at repo root `vcglib/`, not
`src/vcglib`) for anything geometric, and existing framework mechanisms over new
subsystems. Avoid duplicating code *or* functionality. Flag leftover files and dead
functions for removal when you notice them. If a request needs a lot of new code, say so
and propose a lower-code alternative **before** starting.

**You will maintain this codebase for years.** Fix the cause where it lives, not the
symptom where it shows. These are hacks here, and need saying out loud when one really
cannot be avoided: special-casing one filter id, file name or mesh in shared code; a
constant copied rather than named once and shared; an error caught and silently carried
past; a test-only hook in production code; a sleep or a retry in place of the ordering fix;
a workaround in a filter for a vcglib defect (fix vcglib, below). When the clean fix is much
larger than the hack, say so and let the maintainer choose.

**Mapping table before any move.** For any rename, reorganization or migration, produce
the table first and get it approved before touching code: current → target → *measured
evidence* (from CMakeLists, `.gitmodules`, directory contents — never inferred from
names) → verdict, with totals that add up. Record it in the relevant `docs/design/*.md`.
This is where errors are cheap: the plugin table caught that `filter_meshing` is not a
"meshing" family at all (its filters span nine categories) before anything moved.

**Multiple implementations of one algorithm are a feature.** MeshLab is an algorithm
archive: Screened Poisson *and* CGAL Poisson, APSS *and* RIMLS, seven surface
reconstructors. When evaluating a library or a new filter, judge it on its own terms —
licence, dependencies, build friction, output fidelity — and describe how its behaviour
will differ. Overlap with something we ship is context, never an objection. (The
least-code rule above is about framework code, not about algorithm choice.)

**Ask about vendored algorithms.** Paolo co-authored much of the research this codebase
vendors and has the papers to hand. `upstream/` trees carry almost no rationale, so when
an algorithm's conventions, normalizations or intended parameter semantics are unclear,
ask rather than reverse-engineer — then measure to confirm.

## vcglib is a shared upstream, not a vendored tree

**Improving vcglib is welcome, and is often the right fix.** A bug or a missing
primitive belongs upstream rather than worked around here — MeshLab is the library's
largest consumer, and a workaround in a filter leaves the defect in place for everyone
else.

**General geometry processing belongs in vcglib, not in MeshLab's own code.** When a
feature needs mesh, UV or geometric code that does not depend on MeshLab's document,
GUI, Qt or GPU, and there is no easy path to it (no suitable library, a candidate
repository with unclear licensing or a heavy dependency, or a port that would bury
general-purpose code inside a plugin), **write it into vcglib** and keep the filter a
thin adapter: descriptor, parameters, document and undo plumbing, `QImage` access,
`outputValues`. Precedents: the Reeb graph and handle/tunnel loops (`reeb_graph.h`,
`handle_tunnel_loops.h`), planar polygon tessellation with Steiner refinement
(`planar_polygon_tessellation.h`), and plane clipping and capping (`clip.h`). Vendored
algorithm code that must stay comparable with its upstream (the `upstream/` trees) is
the exception: it keeps its provenance where it is. Search vcglib first, as its own rules
require: what you are about to write is often already there.

**Read `vcglib/AGENTS.md` before touching anything under `vcglib/`.** Its rules govern
there and are not repeated here: the library idioms (`Allocator`, `MeshAssert`, throw
rather than assert, report through `CallBackPos`), no duplication inside the library, and
samples that are short documentation rather than tests. Claude Code (v2.1.277 or later)
loads it on its own once it reads a file under `vcglib/`; anything else, or anything that
edits there without reading first, has to open it by hand. A session that skipped it once
wrote a regression suite with a `CHECK` macro as a sample.

**Every vcglib change must be documented and motivated.** Say in the commit message what
it changes and why, and comment the code itself: the change lands for every other vcglib
user, most of whom will never see the MeshLab problem that prompted it. A change that
cannot be justified on vcglib's own terms probably belongs on the MeshLab side instead.

Landing one is a **four-part** task: commit inside `vcglib/`, push to `origin/devel`,
bump the gitlink, and land the MeshLab-side code that depends on it in the same bump. A
dirty or unpushed submodule builds fine locally and fails for everyone else, CI
included.

```bash
git -C vcglib status -sb && git -C vcglib rev-list --count origin/devel..HEAD
```

## Build

```bash
cmake --preset default          # vcpkg manifest mode -> build/
cmake --build build
```

`CMakePresets.json` also offers `local-no-vcpkg` (→ `build-local/`, fewer optional
plugins). Which build trees exist changes over time, and one of them is usually
configured by VS Code's CMake Tools with **no** `CMAKE_BUILD_TYPE` — no `-O` flags at
all, the same trap as Debug.

> **Before accepting any performance report, read `CMAKE_BUILD_TYPE` out of each tree's
> `CMakeCache.txt` and treat an empty value as unoptimized.** Unoptimized vcglib template
> code is 4–18× slower per stage. A "loading 3M faces got 4× slower" report once traced
> entirely to the running app being the unoptimized build.

A `.claude/worktrees/*` worktree has **empty submodules** and cannot be built in place,
since initializing them is a forbidden git action. Copy it out to a scratch directory
(`rsync -a --exclude .git`), copy the main checkout's `vcglib/` and `external/*` in after
checking with `git ls-tree HEAD vcglib external/` that the gitlinks agree, and symlink the
main tree's `build-release/vcpkg_installed` into the scratch build directory — a symlink,
not a path option, because `plugins/io_gltf` looks for tinygltf under
`${CMAKE_BINARY_DIR}/vcpkg_installed`. Sync later edits with plain `cp`: `cp -p` and
`rsync -a` keep an older mtime, and ninja then silently builds the old code. A hook refuses
edits to the main checkout from a worktree session, so hand back a patch.

## Data that is not ours to change

`.reference/` (git-ignored) holds reference material: the legacy `meshlab` and `PyMeshLab`
sources, `papers`, and datasets such as `Rocks` and `TestDataset`. **It is read-only**:
outputs, extracted archives and scratch files go elsewhere, and a script that takes an
input directory refuses an output directory inside it. `tests/local/` is git-ignored too:
local datasets (large textured atlases, for instance) that CI does not have, so a test that
needs them skips when they are absent.

## Tests

```bash
cd build-release && ctest --output-on-failure      # add QT_QPA_PLATFORM=offscreen if headless
```

Filter testing is three tiers, and **a new check belongs in the right one**:

1. **`tests/test_filter_descriptors.cpp`** — descriptor conformance. One row per filter,
   no filter is run, milliseconds. Every invariant decidable from the manifest alone:
   naming grammar, category validity, parameter ranges after bound-token resolution,
   dispatchability. New descriptor-shape rules go **here**, not in `test_filters.cpp`.
2. **`tests/test_filter_smoke.cpp`** — one row per SingleMesh filter, run with defaults
   against a ladder of fixtures (bare → attributed → parametrized → textured → selected →
   open → rasters), first rung that works is recorded. The contract is only *a filter
   either runs or refuses with a message* — never crashes, hangs, or reports success
   having produced nothing. Filters that no rung can drive sit in `expectedRefusals()`
   with a reason; that table fails the run in **both** directions, so it cannot rot.
3. **`tests/test_filters.cpp`** — behavioural assertions: real properties, real numbers.

Beyond filters: the widget tests (`test_layerwidget`, `test_toolstrip`, `test_viewgrid*`,
`test_parameterform` …) pin UI behaviour and run under `QT_QPA_PLATFORM=offscreen`; drive
them with signals and synthetic events rather than screen coordinates. The Python suite
(`python/tests/`, and `tests/test_script_mode.py` for the application's `--script` mode)
is run by CI against the built wheel (`test-command` in `pyproject.toml`). Locally no wheel
needs installing: put `python/pymeshlab2/__init__.py` in a `pymeshlab2/` directory beside
the `_meshlab` module a `MESHLAB2_PYTHON_WHEEL=ON` build (`build-wheel/`) produces, prepend
it to `sys.path`, and run `python3.13 -m pytest python/tests`.

The `rasters` rung is the committed photogrammetry fixture in
`tests/sample_mesh/gargoyle_small/` (928 KB). If its images are ever rescaled, the camera
intrinsics in the `.mlp` must follow — `ViewportPx` and `CenterPx` scale with the pixels,
`PixelSizeMm` inversely, `FocalMm` untouched — or every projection filter silently
misprojects while still reporting success.

**Nothing a shader decides is reachable from ctest**, because the render path lives in the
`MeshLab` executable and a Qt test cannot link `RenderWidget`. Those checks are render
probes: `tests/render_probes/*/generate_api.py`, run through the application's own
`--generate-docs` hook, needing a real GPU and a window server — so `QT_QPA_PLATFORM=offscreen`
breaks them, the opposite of the suite above. See
[rendering.md](docs/design/rendering.md#verifying-the-render-path).

## Where things are documented

`docs/design/README.md` indexes everything and separates **reference** (how MeshLab works
today) from **proposals** (not implemented) and **history** (dated records of finished
work — never a description of the present).

- **[vocabulary.md](docs/design/vocabulary.md)** — *normative*. The controlled vocabulary
  behind every user-visible name: the 11-root category ontology, the verb lexicon with its
  rejected synonyms, the `Verb Object [(Backend)]` grammar, and the spelling rulings
  (`scalar` user-facing / `quality` in code, `Remove` not `Delete`, `parametrization`,
  American `color`). If a term is not in it, it is not approved — extend the document
  rather than inventing a local synonym.
- **[adding_a_filter.md](docs/design/adding_a_filter.md)** — how a filter is declared and
  implemented: the JSON manifest, parameter schema, input/output domains, `randomSeed`,
  `markMeshGeometryChanged`, `visualizationHints`.
- **[filter_organization.md](docs/design/filter_organization.md)** — plugin↔family rules
  and the settled decisions behind the current layout.
- **[architecture.md](docs/design/architecture.md)** · **[data_model.md](docs/design/data_model.md)**
  · **[rendering.md](docs/design/rendering.md)** · **[memory_accounting.md](docs/design/memory_accounting.md)**
  · **[preferences.md](docs/design/preferences.md)**
