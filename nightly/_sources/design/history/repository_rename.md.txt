# Repository Rename: QMeshLab → meshlab2 — record

> **Completed 2026-09-13.** All six surfaces were renamed and every phase is
> committed. This is a dated record of the plan and its execution, not a current
> description of the tree — the names in the "Current" column no longer exist.
> See [Outcome](#outcome).

> **Exclude this file from any rename sweep.** It quotes the old identifiers on
> purpose — the "Current" column below is the only surviving record of what the
> names used to be. The Phase 1 and Phase 2 sweeps rewrote it once and the
> mapping had to be restored from `git show 8fe38b7`.

See also: [Architecture](../architecture.md), [Adding a Filter](../adding_a_filter.md)
(the plugin-id convention), [Preferences](../preferences.md) (where user settings live).

## Outcome

**Status: APPLIED** (2026-09-13). Phase 0 renamed the repository to
`cnr-isti-vclab/meshlab2`; Phases 1-4 are committed (`f2542a0`, `75ca2cc`,
`2712a6c`, `1bdf16c`) and Phase 5 followed. The measurements throughout this
document were taken on that date against the then-current tree and are not
re-checked.

Verified 2026-09-16: no tracked file outside this record still contains
`QMESHLAB_`, `QMESH_` or `qmeshlab`. The `QMeshLab` spelling survives in exactly
three places, all deliberate — [history/filter_names.md](filter_names.md), which
is a dated record like this one; the vendored `upstream/` trees, which stay
pristine; and the `QMeshLab:` in-source patch markers documented by
`plugins/filter_isoparam/UPSTREAM.md`, whose value is that they match the
literal string in the vendored sources.

## "The rename" is six separate things

Only the first is the GitHub repository name, and they can be done — or declined
— independently.

| # | Surface | Measured extent | Reversible? |
|---|---|---|---|
| 1 | GitHub repository name | 1 setting; 7 in-tree URLs | Yes — GitHub redirects the old path indefinitely |
| 2 | Build identifiers (CMake targets, options, macros) | `QMeshLab*` 918, `QMESH_*` 251, `QMESHLAB_*` 157 | Yes, it is all internal |
| 3 | Plugin ids and Qt resource prefixes | 34 ids, 222 `qmeshlab.` occurrences | Mostly — one user setting keys off them |
| 4 | User-facing app identity (name, QSettings, bundle id) | 3 lines, but they own user data | **No** without a migration shim |
| 5 | Local working-copy directory | 1 `mv` | Yes, but it moves the agent state — see below |
| 6 | Release artifact names | 2 workflow lines | Yes |

Total: **1,297 occurrences across 213 tracked files.**

## What must not be renamed

This is the trap that makes a blanket `sed -i 's/[Qq]meshlab/meshlab2/gI'`
destructive. The tree contains roughly **180 references to the *original*
MeshLab that must survive verbatim**:

| Kind | Count | Why it stays |
|---|---|---|
| `MeshLabProject*` classes | ~50 | The `.mlp` reader/writer. It is MeshLab's format; renaming it makes the code lie. |
| Bare `MeshLab` in prose | ~130 | "Unlike MeshLab, the source keeps its own domain", "MeshLab project files". |
| `pymeshlab` (the old library) in prose | — | Discussion of the *previous* Python API stays about the previous API. |
| `vcglib` | — | Separate project, separate repo. |

So every rename below is **case-sensitive and family-by-family**, never a global
substitution. The decision to call the product *MeshLab* sharpens this rather than
relaxing it: user-facing strings do become "MeshLab", while `MeshLabProject*`
symbols must not move, so the sweep has to tell a display string from a symbol.
Review the diff for every `MeshLab` that gained or lost a `Q` or a `2`.

## Decisions taken (2026-09-13)

| Surface | Decision | Rationale |
|---|---|---|
| Product / display name | **MeshLab** | This codebase replaces the old one. It is the product's name, not a new product. |
| Repository, local directory | **meshlab2** | A genuinely different codebase, so a different repo. |
| Python facade | **pymeshlab** (from `pymeshlab2`) | Encourages transition to the new library. |
| CMake options and macros | **`MESHLAB2_*`** | Over the shorter `ML2_*`. |
| Code identifiers (targets, classes) | **`MeshLab2*`** | Matches `MESHLAB2_*`, and stays clear of the `MeshLabProject*` family. |

*Display name* and *code identifier* are deliberately different here. "MeshLab" is
the product; `MeshLab2*` is what the symbols are called, following the repository
rather than the product. That divergence is the point: it keeps `MeshLab2Core`
legible next to `MeshLabProjectMeshEntry`, which `MeshLabCore` would not be.

### The full mapping

Every family, with its measured extent. This is the table the sweep works from.

| Family | Current | Target | Extent |
|---|---|---|---|
| Application target and bundle | `QMeshLab` | `MeshLab2` (target) / `MeshLab` (display) | 27 target refs |
| Core library | `QMeshLabCore` | `MeshLab2Core` | 64 |
| Plugin aggregate | `QMeshLabPlugins` | `MeshLab2Plugins` | 93 |
| Plugin targets | `QMeshLabPluginFilter*`, `QMeshLabPluginIO*` | `MeshLab2PluginFilter*`, `MeshLab2PluginIO*` | 358 |
| Test targets | `QMeshLabTests`, `QMeshLabFilterTests`, … | `MeshLab2Tests`, … | in the 918 |
| CMake options | `QMESH_PLUGIN_*`, `QMESH_IGL_*`, `QMESH_MACOS_*` | `MESHLAB2_PLUGIN_*`, … | 251 |
| Macros and env vars | `QMESHLAB_PYTHON_CONSOLE`, `QMESHLAB_BUILD_ID`, … | `MESHLAB2_*` | 157 |
| Plugin ids and resource prefixes | `qmeshlab.filter.*`, `qmeshlab.io.*`, `qmeshlab.test.*` | `meshlab2.*` | 34 ids, 222 refs |
| Native Python module | `_qmeshlab` | `_meshlab` | 56 |
| Python facade module | `pymeshlab2` | `pymeshlab` | 51 |
| Qt application name | `QMeshLab` | `MeshLab` + settings migration | 3 lines |
| Window title, About, log strings | `"QMeshLab"` | `"MeshLab"` | ~10 |
| Snapshot PNG text key | `QMeshLab.CameraTrackballState` | `MeshLab.CameraTrackballState`, old key still read | 3 |
| Release artifacts | `QMeshLab-<date>-<sha>-…` | `MeshLab-<date>-<sha>-…` | 2 workflow lines |
| Repository and local directory | `QMeshLab` | `meshlab2` | 1 setting, 7 URLs, 1 `mv` |
| **Not renamed** | `MeshLabProject*`, bare `MeshLab` in prose, `vcglib` | unchanged | ~180 |

### Python: the consequential part is not in this repo

`pymeshlab2` here is only an in-process module name, created at
`PythonHost.cpp:165` over the native `_qmeshlab` module (`NB_MODULE(_qmeshlab)`,
56 occurrences). There is no packaging metadata in this tree — no `pyproject.toml`,
no `setup.py`. Renaming to `pymeshlab` costs three source files and seven docs.

What is consequential belongs to the sibling `pymeshlab2` repository: taking the
published `pymeshlab` name on PyPI. cnr-isti-vclab owns that name, so it is
available — but the new API is deliberately incompatible (`pythonName`s were
renamed outright, with no aliases for the old ones), so `pip install pymeshlab`
would move existing users onto a different API. A major version bump is the
normal way to signal that, and unpinned scripts will still break on upgrade.
That decision is worth taking in the other repo, explicitly.

### The bundle identifier: no longer a bug, now a choice

`CMakeLists.txt:23` sets `QMESHLAB_BUNDLE_IDENTIFIER "net.meshlab.MeshLab"` — the
same bundle id as the released MeshLab. Before the display name was settled that
read as a collision. With the product named **MeshLab and replacing the old one**,
it becomes defensible: macOS treats the new build as the *same application*, so
it upgrades in place and inherits the existing Launch Services registration, TCC
grants and preferences domain.

The question is whether that is wanted **during the transition**, while a user may
still have both installed. Sharing the id then means the two fight over file
associations and share a preferences domain with incompatible contents.

- Keep `net.meshlab.MeshLab` → a clean in-place succession, but the two cannot
  coexist properly on one machine.
- Use `net.meshlab.meshlab2` → they coexist cleanly; the succession is a separate
  install, and TCC/Gatekeeper prompts reappear once.

Coexistence during a transition period argues for the second, with a move to the
original id once the old MeshLab is retired.

### User settings need a migration shim, not a rename

`src/app/main.cpp:82-84` sets `organizationName` `QMeshLab`, `organizationDomain`
`meshlab.net`, `applicationName` `QMeshLab`. QSettings therefore stores
preferences at `~/Library/Preferences/net.meshlab.QMeshLab.plist` (and the
equivalent registry path on Windows). Renaming `applicationName` **silently
orphans every existing user's settings** — no error, just defaults.

The fix is a one-time migration at startup: if the new settings object is empty
and the old one exists, copy every key across and leave a marker. Roughly 20
lines in `Preferences`, and it should stay in place for at least one release
cycle.

One setting is keyed by plugin id rather than by name:
`meshiopluginmanager.cpp:28` reads the preferred import plugin per extension from
QSettings, storing a plugin id string. Renaming plugin ids orphans that
preference too — it degrades to the default rather than failing, so it is low
severity, but the migration shim should rewrite these keys while it is there.

### One persisted format key

`mainwindow.cpp:2586` and `:2622` embed the camera state in saved snapshot PNGs
under the text key `QMeshLab.CameraTrackballState`, and `renderwidget.cpp:936`
reads it back. Snapshots already saved in the wild carry that key. Rename it and
those images silently stop restoring their camera.

Cheap fix, and it belongs with Phase 3: write the new key, accept either on read.

`layerfilterplugin.cpp:451` is the same shape: the exported raster-camera XML uses
`QMeshLabRasterCameras` as its root element. Nothing in this tree reads it back, so
it is an export format for other tools to consume. A third is the memory report's
schema string `org.qmeshlab.memory-report.v1` (`mainwindow.cpp:2936`), which is
versioned and read by whatever consumes the exported JSON.

Phase 3 found two more of the same shape: `QMeshLab.CameraState` and
`QMeshLab.RenderState`, the `kind` tag on camera and render state JSON, which is
**validated on read** (`renderwidget.cpp`, `meshfilterpluginmanager.cpp`) and
travels in saved snapshots, copied state and any script pinning a `cameraState`
parameter.

All five were renamed in Phase 3 with the writers emitting the new spelling and
the readers accepting both. `stateJsonAcceptsBothNameSpellings` in
`tests/test_filters.cpp` holds that compatibility in place, since a promise of
this kind is otherwise deleted by the next cleanup.

## Sequencing

Five phases. Each is a separate commit, each leaves the tree building and the
tests green, and any one can be stopped at without leaving the project in a
half-renamed state.

### Phase 0 — GitHub repository rename

Rename `cnr-isti-vclab/QMeshLab` → `cnr-isti-vclab/meshlab2` in repository
settings. GitHub redirects the old URL — web, git, and API — indefinitely, so
existing clones keep working and no one has to do anything. Then:

- Update the 7 in-tree URLs (`readme.md`, About-dialog links, `docs/index.rst`).
- Fix the stale one while passing: `docs/index.rst:38` still points at
  `github.com/anomalyco/QMeshLab`, which is wrong today regardless of the rename.
- Update `git remote set-url` locally, though the redirect makes it optional.
- The GitHub Pages URL moves to `cnr-isti-vclab.github.io/meshlab2`; the old one
  redirects.

This phase is free and reversible, and it is worth doing first so the name is
settled before any code churns.

> **A regex with `\b` before the prefix is not enough.** Phase 1 used `\bQMESH_`
> and missed every `-DQMESH_PLUGIN_...` in CI and docs, because `-D` leaves no word
> boundary. CMake accepts an unknown `-D` silently, so `.github/workflows/docs.yml`
> would have built with defaults instead of the flags it names — a silent behaviour
> change, not a failure. Found and fixed during Phase 4. Verify option renames by
> grepping the `-D` call sites too, not just `option()` declarations.

### Phase 1 — build identifiers

`QMeshLab*` targets, `QMESH_*` options, `QMESHLAB_*` macros and env vars. Purely
internal: nothing here is visible to a user or persisted anywhere.

Mechanical, but do it as **three commits**, one per family, each verified by a
clean configure-and-build. The option rename in particular touches 55
`CMakeLists.txt` files and a stale `QMESH_PLUGIN_*` name fails silently by simply
not enabling a plugin — so after this phase, diff the built plugin list against
the list from before.

`QMESHLAB_PYTHON_CONSOLE` is documented in `readme.md` and used in CI; rename it
in the same commit as its documentation.

### Phase 2 — plugin ids and resource prefixes

34 ids of the form `qmeshlab.filter.foo` → `meshlab2.filter.foo`. Each appears in
its `filters.json`, in the plugin's `pluginId()`, and as the `qt_add_resources`
PREFIX — and [Adding a Filter](../adding_a_filter.md) states the invariant that
the prefix must match the id, so all three move together or the descriptors fail
to load.

Guard: `MeshLab2FilterDescriptorTests` and `MeshLab2FilterCreationTests` both
load every descriptor, so a mismatch fails fast rather than at runtime.

The Python API is unaffected — scripts call `apply_filter("do_something", …)`
using `pythonName`, and `filterKey` (`pluginId::filterId`) is in-memory only,
never serialized into a script or a project file.

### Phase 3 — user-facing identity

App name, QSettings migration shim, bundle identifier, window title, About box.
This is the only phase that can lose user data, and it should be one commit with
the shim included, never split.

### Phase 4 — local working copy and agent state

Rename `~/Documents/devel/github/QMeshLab` to `…/meshlab2`, for local clarity
against the new repository name. See the next section: the directory name is what
keys the agent state, so the two moves happen together.

### Phase 5 — comments, documentation and CI internals

Three exclusions, each for a reason worth keeping:

- **Vendored `upstream/` trees.** Their ~31 `QMeshLab:` adaptation markers stay, so
  the trees remain byte-comparable with upstream and our diffs against it stay
  minimal. The `UPSTREAM.md` files note that the marker predates the rename.
- **`docs/design/history/`.** Records are never edited after the fact — see
  [the design README](../README.md). `filter_names.md` keeps the two sentences that
  contrast "filters MeshLab has and QMeshLab does not", which is what was true when
  it was written.
- **This document**, for the reason in the banner at the top.

One sentence needed rewriting rather than substituting, the same trap as the About
dialog: `plugins/io_3mf/README.md` said the plugin "ports the original MeshLab
`io_3mf` functionality to QMeshLab's I/O API".

### Phase 5 (original note) — artifacts and documentation

`APP_NAME: QMeshLab` in `macos-dmg.yml:43` and the hard-coded stem in
`windows-portable.yml:246` produce `QMeshLab-<date>-<sha>-<platform>`. Renaming
them changes the artifact names; older artifacts keep their old names, which is
correct — they were built by the old project.

Then the doc sweep: 27 Markdown files, plus `docs/design/README.md` and the
memory of which docs are reference versus history.

## Impact on the Claude Code project

This is the part with a non-obvious failure mode, and it belongs to **Phase 4
only**. If the local directory keeps its name, none of this applies.

Claude Code keys its per-project state on the **absolute path of the working
directory**, with separators flattened to dashes. Today that is:

```
~/.claude/projects/-Users-cignoni-Documents-devel-github-QMeshLab/
```

Renaming `~/Documents/devel/github/QMeshLab` to `…/meshlab2` makes Claude Code
look for `-Users-cignoni-Documents-devel-github-meshlab2`, find nothing, and
create it empty. What is left behind:

| State | Extent | Consequence if orphaned |
|---|---|---|
| Session transcripts | 9 files, **146 MB** | `--resume` and `--continue` find no history |
| Memory files | 24 files, 120 KB | Every learned convention is gone; the agent starts from nothing |
| Per-project settings | `.claude/settings.local.json` (in-repo, moves with the tree) | — |

**Mitigation, before launching Claude from the new path:**

```bash
mv ~/.claude/projects/-Users-cignoni-Documents-devel-github-QMeshLab \
   ~/.claude/projects/-Users-cignoni-Documents-devel-github-meshlab2
```

Do this in the same sitting as the directory rename. The transcripts contain the
old absolute path in their recorded content, but that is history rather than a
lookup key, so resume is expected to work — this has not been verified, so do the
move first and confirm `--resume` before deleting anything.

Two further passes, both cheap:

- **Memory files.** 17 of the 24 mention `QMeshLab`, including six build-target
  names (`MeshLab2Core`, `MeshLab2Embree`, `MeshLab2FilterSmokeTests`, …). Stale
  target names in memory are worse than no memory: the agent will confidently
  run a build command that no longer exists. Sweep them in the same phase.
- **`.claude/settings.local.json`.** **65 entries** mention `QMeshLab`, most of
  them permission rules for build commands such as
  `cmake --build … --target MeshLab2 -j8`. After Phase 1 these stop matching and
  every build prompts for permission again. They are prefix rules, so the fix is
  the same rename applied to the file.

`.opencode/rules.md` also names the project and three design docs; it belongs to
a different agent but is in the same tree and should be swept with the docs.

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| A blanket case-insensitive replace destroys the ~180 genuine MeshLab references | High | Case-sensitive, family-by-family; `MeshLabProject*` explicitly excluded; review the diff for `MeshLab` without `2` |
| User preferences orphaned by the app-name change | High | Migration shim in Phase 3, kept for a release |
| A renamed `MESHLAB2_PLUGIN_*` option silently disables a plugin | Medium | Diff the built plugin list before and after Phase 1 |
| Bundle-id change resets TCC and Gatekeeper | Low, one-time | Expected and desirable; note it in the release notes |
| Agent memory and history orphaned | Medium | `mv` the project directory in the same sitting (Phase 4) |
| In-flight branches conflict massively | Medium | Do the rename when no long-lived branch is open; each phase is one commit, so rebases resolve per-family |

## Open questions

One remains, and it is smaller than the others: whether the macOS bundle
identifier stays `net.meshlab.MeshLab` (in-place succession, but the old and new
applications cannot coexist cleanly) or becomes `net.meshlab.meshlab2`
(coexistence during the transition, at the cost of one round of Gatekeeper and
TCC prompts). See [the discussion above](#the-bundle-identifier-no-longer-a-bug-now-a-choice).

It only has to be answered before Phase 3, and it can be revisited when the old
MeshLab is retired.
