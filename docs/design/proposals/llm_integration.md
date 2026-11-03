# LLM Integration

This document maps the option space for driving MeshLab from a large language
model. **Nothing described here is implemented.** It exists to make the design
choices explicit before any code is written, and to record which of MeshLab's
existing mechanisms are already load-bearing for this purpose.

See also: [Architecture](../architecture.md) (layers and ownership),
[Python Scripting](../../python_scripting.md) (the current `ms` / `pymeshlab` surface in the app),
[Vocabulary](../vocabulary.md) (the naming grammar that makes filters machine-legible),
[Filter Organization](../filter_organization.md) and [Adding a Filter](../adding_a_filter.md)
(descriptor schema).

## Status

As of 2026-11-03: no LLM integration exists, and none is scheduled. Two of the
prerequisites in [Staging](#staging) have since landed for their own reasons —
descriptor and naming discipline, and the convergence of the headless wheel
build with the application build. The first step that is actually about models
(a headless tool server) has not been started.

## The substrate that already exists

MeshLab is closer to being model-drivable than it looks. The relevant assets:

| Asset | Where | Why it matters |
|---|---|---|
| **403 filters / 38 plugins**, 1381 declared parameters | `plugins/*/filters.json` | Each descriptor carries `id`, `pythonName`, `shortDescription`, `longDescriptionMarkdown`, `tags`, `categories`, `inputDomain`/`outputDomain`, `inputRequirements`, and typed `parameters` with `default`/`min`/`max`/`help`. This is a tool schema already; converting it to JSON Schema is mechanical. |
| Controlled vocabulary and naming grammar | [Vocabulary](../vocabulary.md) | Tool-selection accuracy is dominated by whether names and descriptions are predictable. |
| Generic dispatch | `Document::runFilter(filterKey, params)`, `MeshSet::apply_filter()` | One entry point; no per-filter binding work. |
| Pre-flight validation | `Document::validateFilterInvocation()`; `FilterInfo::applicable` / `applicabilityError` | Lets a caller be told *why* a filter cannot run, in a form it can act on, before running it. |
| Structured results | `MeshFilterRunResult` — `success`, `errorMessage`, `infoMessages`, `newMeshIndices`, `outputValues` | The observation half of an act/observe loop. |
| Declared input requirements | `requireFaces`, `requireVertices`, `requireVertexQuality`, `requireTextureCoordinates`, `requirePerWedgeTexCoords`, `requireTextures`, `requireVertexColor`, `requireFaceQuality`, `requireFaceColor`, `requireEdges` | Machine-checkable preconditions — a caller can repair state instead of guessing. |
| **20 measurement / information filters** | `Measurement/*` categories | `get_info`, `compute_geometric_measures`, `compute_topological_measures`, vertex/face quality stats and histograms, `compute_hausdorff_distance`, `compute_chamfer_distance`. This is the sensor set: it is what makes *self-checking* possible rather than open-loop guessing. |
| Offscreen rendering | `MeshSet::render_snapshot()`, `MlGui::save_snapshot()`, `HeadlessRenderContext` | Visual feedback for a multimodal model. |
| Scale-relative parameter types | `absperc`, `@bboxDiag` expressions | Directly addresses the hardest grounding problem (see below). |
| Reproducibility | `ScriptAction` (`filterKey`, `params`, `pythonCall`, `compactPythonCall`), `UndoActionRecord`, undo-graph Python export | Audit trail, single-step rollback, and a replayable transcript, all free. |
| Seed discipline | `randomSeed` declared by 50 filters across 12 plugins, resolved via `FilterParams::getRandomSeed()` | Randomized filters can be made deterministic on demand. |

## What is missing

One substantial gap, and one that has closed.

### Closed: the headless build now follows the application build

This was previously the largest obstacle, and the fix landed in the sibling
wheel repository `cignoni/pymeshlab2` rather than here. The arrangement today:

- *Bindings* are shared by construction. `src/python/bindings/sources.cmake`
  defines `MESHLAB2_PYTHON_BINDING_SOURCES`, and both consumers include it:
  this repository's static `_meshlab2_bindings` (embedded console) and the
  wheel's `nanobind_add_module(_meshlab NB_STATIC …)`.
- *The build graph* is no longer restated. The wheel does
  `add_subdirectory(external/meshlab2 EXCLUDE_FROM_ALL)` and links the whole
  `MeshLab2Plugins` target, so a plugin added here appears there without edits.
  What it carries is expressed as explicit **opt-outs** rather than a hand-built
  list: `MESHLAB2_PLUGIN_OBJ_RAPIDOBJ`, `MESHLAB2_PLUGIN_E57` and
  `MESHLAB2_PLUGIN_GLTF` off unconditionally, and `FILTER_MESHFIX`,
  `FILTER_QSLIM`, `FILTER_QUADWILD` off when their vendored externals are
  absent.
- *Packaging* exists: `cibuildwheel` for CPython 3.10–3.14 on macOS arm64 and
  Windows x64, `delvewheel` / a macOS repair script, and bundling of the geogram
  runtime and the quadwild helper binaries.

Two consequences remain, and both matter for a model-facing catalogue:

1. **The wheel's filter set is a subset, and its size is not pinned.** The
   opt-outs above are deliberate, but `MESHLAB2_REQUIRE_VCPKG_DEPS OFF` also
   lets plugins drop out silently when an optional dependency is unavailable in
   the wheel environment. A build is therefore not self-describing: the only way
   to know what a given wheel exposes is to ask it. A locally built extension
   checked during this review reported **304 filters against 403 declared in
   `plugins/*/filters.json`** — part stale build, part disabled plugins, with
   nothing distinguishing the two.
2. **The submodule pin is a standing source of staleness.** The wheel repository
   pins MeshLab as a submodule, and that pin is routinely behind; at the time of
   writing it trailed `devel` by 135 commits. Whether the wheel should remain a
   separate repository at all, or become a workflow in this one, is an open
   question — see [Packaging location](#packaging-location) below.

A model that is told "these are MeshLab's filters" should be told so by the
build it is actually talking to. Any tool server must enumerate filters from the
live `MeshSet`, never from this repository's descriptors, and should report the
backing version alongside.

### Open: no IPC of any kind

No `QLocalServer`, no socket, no RPC. Driving the *live* document from outside
the process requires new plumbing plus thread marshalling onto the GUI thread,
and has to interact correctly with the existing "block UI while filters run"
behaviour. Nothing here has changed.

## Packaging location

An open question, recorded because it shapes step 2 rather than because it
blocks it.

The wheel lives in `cignoni/pymeshlab2`, which holds 22 files outside the
submodule: a `cibuildwheel` workflow, `CMakeLists.txt` (mostly the toggles
above), `pyproject.toml`, `vcpkg.json`, a ~35-line `__init__.py` of runtime
library shims, a macOS wheel-repair script, six examples and five test files.
Everything else it builds comes from the submodule.

The duplication is in CI rather than in code. This repository's workflows
already install Qt, bootstrap vcpkg pinned to the manifest baseline, apply the
overlay triplets in `ci/vcpkg-triplets`, and cache vcpkg artifacts across jobs,
on both macOS arm64 and Windows x64. `wheels.yml` re-implements that sequence,
and already reaches back through the submodule for the triplets
(`VCPKG_OVERLAY_TRIPLETS: …/external/meshlab2/ci/vcpkg-triplets`).

Folding the wheel in — a `pyproject.toml` in a subdirectory with
scikit-build-core pointed at the repository root (`cmake.source-dir`), plus a
tag-triggered workflow reusing the existing Qt and vcpkg steps — would delete
the submodule pin, the second vcpkg manifest, and the duplicated CI setup. The
arguments for keeping it separate are release cadence (a wheel release need not
be an application release), CI budget, and PyPI project identity; the first two
are solvable with a separate workflow trigger, so the decision rests mainly on
how independent the two release streams should be.

Note that the module is currently published as `pymeshlab2`
(`pyproject.toml: name = "pymeshlab2"`, imported as `pymeshlab2`), while
`AGENTS.md` declares the intended Python module name to be `pymeshlab`. Those
have not been reconciled.

## Four independent axes

Most confusion about "adding an LLM" comes from collapsing these. They can be
chosen separately.

### A. Where the model runs

- **Cloud API** — best reasoning; requires network, an API key, and a decision
  about what mesh-derived data may leave the machine.
- **Local model** (llama.cpp / Ollama) — offline and private, but selecting
  correctly among 400-odd filters is hard for small models.
- **No embedded model.** MeshLab speaks a protocol; the user brings their own
  client. Cheapest by a wide margin, and it makes the model a user choice rather
  than a shipped dependency.

### B. What the model manipulates

1. **Code generation.** The model writes Python; the existing console runs it.
   One integration point, very large reach, no new schema work. Weakness: no
   structured error recovery unless a loop is built around it.
2. **One tool per filter.** Precise and fully validated, but 403 tools with 1381
   parameters exceeds the practical tool budget of every current model.
3. **Search / describe / apply.** A small fixed tool set over dynamic discovery
   in the descriptors. Almost certainly the right shape; the vocabulary work is
   what makes the search leg viable.
4. **Driving the GUI directly.** Rejected — brittle, unobservable, untestable.

### C. How it is wired

- **Out-of-process, headless.** An MCP (or similar) server over an importable
  `_meshlab`. No GUI, no C++ changes beyond the extension-module packaging.
  Testable in CI, safe to iterate on, works with existing model clients.
- **Out-of-process, live document.** The same server attached to a running
  MeshLab over `QLocalServer`. Substantially more valuable — the user watches
  the mesh change, edits land in the undo graph, the viewport is the feedback —
  and the only option needing real new plumbing.
- **In-app chat dock.** Most product-like, most code. Amounts to
  re-implementing an agent harness (streaming, tool loop, provider config,
  cancellation, cost accounting) that existing clients already provide.
- **CLI batch mode** (`meshlab --script foo.py`). Not LLM-specific, but the
  substrate for headless automation and CI. Currently only `--generate-docs`
  exists.

### D. What it is for

The use case should drive the three axes above, not the reverse.

1. **Filter discovery** — "which filter removes these spikes?" Retrieval over
   descriptors. No mesh mutation, no agent loop. Lowest risk, immediate value.
2. **Script authoring** — natural language to a `pymeshlab` script.
3. **Pipeline execution** — "clean this scan, decimate to 50k, preserve
   boundaries." Needs the act/observe loop.
4. **Closed-loop quality** — "simplify until Hausdorff stays under 0.1%." The
   measurement filters make this genuinely reachable, and it is the case where
   MeshLab is better positioned than most comparable tools.
5. **Visual QA** — snapshot to a vision model: "normals are inverted on the
   inner shell."
6. **Parameter suggestion** inside the existing filter panel. The smallest
   possible integration, contained to one dialog.

## Likely interface shape

If an agentic integration is built, the tool surface should be small and fixed,
with the filter set reached through discovery rather than enumeration:

```
        model
          │
   ┌──────┴───────────────────────────────────────────┐
   │  search_filters(query)      → ranked descriptors │  descriptors
   │  describe_filter(name)      → full param schema  │  (filters.json)
   │  apply_filter(name, params) → FilterRunResult    │  Document::runFilter
   │  get_document_state()       → layers, bbox, caps │  Document
   │  measure(kind)              → outputValues       │  Measurement/* filters
   │  render_view(state, w, h)   → PNG                │  HeadlessRenderContext
   │  undo() / redo()                                 │  DocumentUndoManager
   └──────────────────────────────────────────────────┘
```

`get_document_state()` is not optional garnish: it is what supplies scale,
element counts, and available attributes, without which parameter choices are
unfounded.

## Hard parts

- **Scale grounding.** "Radius 0.05" is meaningless without the bounding box.
  Every result must carry scale context, and callers should be steered toward
  `absperc` parameters rather than absolute ones.
- **Descriptor quality is the actual bottleneck.** Model competence is bounded
  by the quality of `help` and `shortDescription`. The vocabulary, naming, and
  classification work already underway *is* the enabling work here.
- **Long-running calls.** Filters can run for minutes; agent clients assume fast
  tools. Needs progress, cancellation, and an async contract.
- **Safety gates.** File writes, `clear()`, destructive topology changes need
  confirmation or a dry-run mode. Undo covers mistakes inside the document; it
  does not cover the filesystem.
- **Privacy as an invariant, not an accident.** With the shape above, geometry
  never leaves the machine — only descriptions, measurements, and (if enabled)
  rendered images do. Worth stating as a deliberate constraint.
- **Evaluation.** A task benchmark ("reduce to N faces keeping Hausdorff under
  X") is needed to tell whether any of this works. The measurement filters plus
  the existing test harness make that unusually cheap to build.
- **Determinism.** Agent runs should pin `randomSeed` and record it, so a
  transcript replays identically.

## Staging

Ordered by risk and by what each step unblocks.

0. **Descriptor and naming quality.** *Done, and ongoing.* Pursued for its own
   reasons; it remains the prerequisite regardless of which path is taken, and
   its quality still bounds everything downstream.
1. **Converge the headless build with the application build.** *Done* — see
   [What is missing](#closed-the-headless-build-now-follows-the-application-build).
   The wheel consumes this repository's build graph and binding sources rather
   than restating them, and ships through `cibuildwheel`. What remains is
   bookkeeping, not structure: the submodule pin, and making a build state which
   filters it actually carries.
2. **Headless tool server** over that package, out of tree. *Next.* Find out
   empirically where a model goes wrong before designing around assumptions.
   Must enumerate filters from the live `MeshSet`, not from `plugins/*.json`.
3. **Sensor and vision tools** in the same server, closing the act/observe loop.
4. **`QLocalServer` bridge** so the same server can attach to a running GUI
   session.
5. **In-app chat dock** — only if 1–4 prove out and the goal shifts from a
   power-user capability to a shipped feature.

## Non-goals

- Replacing the filter panel or the Python console with a chat prompt.
- Letting a model drive the GUI by synthesising mouse and keyboard input.
- Shipping model weights, or hard-wiring a single provider.
- Uploading mesh geometry to a remote service as part of normal operation.
