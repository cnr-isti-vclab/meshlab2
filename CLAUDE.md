# MeshLab — working notes for Claude

A ground-up rewrite of MeshLab: a single-document, multi-view Qt 6 application on QRhi,
vcglib, and a statically-linked plugin architecture, with over 300 filters across more
than 30 plugins and optional Python bindings through nanobind.

This file carries what is **not** written down elsewhere in the repo. Anything about how
the code works belongs in `docs/design/` — read from there, and add to there, rather than
growing this file.

## Three names, deliberately different

| | |
|---|---|
| Product / display name | **MeshLab** — bundle, window title, About box, bundle id `net.meshlab.MeshLab` |
| Repository and directory | **meshlab2** (`cnr-isti-vclab/meshlab2`) |
| Code identifiers | **`MeshLab2*`**, `MESHLAB2_*`, `meshlab2.filter.*` — they follow the repo, not the product |
| Python | module `pymeshlab`, native `_meshlab` |

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
baseline use `git show HEAD:<path>` or a separate `git worktree`.

**Least code wins.** Reuse before adding: vcglib (cloned at repo root `vcglib/`, not
`src/vcglib`) for anything geometric, and existing framework mechanisms over new
subsystems. Avoid duplicating code *or* functionality. Flag leftover files and dead
functions for removal when you notice them. If a request needs a lot of new code, say so
and propose a lower-code alternative **before** starting.

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

**Every vcglib change must be documented and motivated.** Say in the commit message what
it changes and why, and comment the code itself: the change lands for every other vcglib
user, most of whom will never see the MeshLab problem that prompted it. A change that
cannot be justified on vcglib's own terms probably belongs on the MeshLab side instead.

Landing one is a **four-part** task: commit inside `vcglib/`, push to `origin/devel`,
bump the gitlink, and land the MeshLab-side code that depends on it in the same bump. A
dirty or unpushed submodule builds fine locally and fails for everyone else — CI and the
pymeshlab sibling repo included.

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
since initializing them is a forbidden git action. Copy it out to scratch and symlink
`vcpkg_installed` instead — the recipe is in the `building-inside-a-worktree` memory.

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

The `rasters` rung is the committed photogrammetry fixture in
`tests/sample_mesh/gargoyle_small/` (928 KB). If its images are ever rescaled, the camera
intrinsics in the `.mlp` must follow — `ViewportPx` and `CenterPx` scale with the pixels,
`PixelSizeMm` inversely, `FocalMm` untouched — or every projection filter silently
misprojects while still reporting success.

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
