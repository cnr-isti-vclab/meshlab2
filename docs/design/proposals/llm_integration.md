# LLM Integration

This document maps the option space for driving QMeshLab from a large language
model. **Nothing described here is implemented.** It exists to make the design
choices explicit before any code is written, and to record which of QMeshLab's
existing mechanisms are already load-bearing for this purpose.

See also: [Architecture](../architecture.md) (layers and ownership),
[Python Scripting](../../python_scripting.md) (the current `ms` / `pymeshlab` surface),
[Vocabulary](../vocabulary.md) (the naming grammar that makes filters machine-legible),
[Filter Organization](../filter_organization.md) and [Adding a Filter](../adding_a_filter.md)
(descriptor schema).

## Status

As of 2026-09-01: no LLM integration exists, and none is scheduled. The
prerequisite work — descriptor quality and naming discipline — is in progress
independently, for its own reasons.

## The substrate that already exists

QMeshLab is closer to being model-drivable than it looks. The relevant assets:

| Asset | Where | Why it matters |
|---|---|---|
| **327 filters / 33 plugins**, 1131 declared parameters | `plugins/*/filters.json` | Each descriptor carries `id`, `pythonName`, `shortDescription`, `longDescriptionMarkdown`, `tags`, `categories`, `inputDomain`/`outputDomain`, `inputRequirements`, and typed `parameters` with `default`/`min`/`max`/`help`. This is a tool schema already; converting it to JSON Schema is mechanical. |
| Controlled vocabulary and naming grammar | [Vocabulary](../vocabulary.md) | Tool-selection accuracy is dominated by whether names and descriptions are predictable. |
| Generic dispatch | `Document::runFilter(filterKey, params)`, `MeshSet::apply_filter()` | One entry point; no per-filter binding work. |
| Pre-flight validation | `Document::validateFilterInvocation()`; `FilterInfo::applicable` / `applicabilityError` | Lets a caller be told *why* a filter cannot run, in a form it can act on, before running it. |
| Structured results | `MeshFilterRunResult` — `success`, `errorMessage`, `infoMessages`, `newMeshIndices`, `outputValues` | The observation half of an act/observe loop. |
| Declared input requirements | `requireFaces`, `requireVertices`, `requireVertexQuality`, `requireTextureCoordinates`, `requirePerWedgeTexCoords`, `requireTextures`, `requireVertexColor`, `requireFaceQuality`, `requireFaceColor`, `requireEdges` | Machine-checkable preconditions — a caller can repair state instead of guessing. |
| **15 measurement / information filters** | `Measurement/*` categories | `get_info`, `compute_geometric_measures`, `compute_topological_measures`, vertex/face quality stats and histograms, `compute_hausdorff_distance`, `compute_chamfer_distance`. This is the sensor set: it is what makes *self-checking* possible rather than open-loop guessing. |
| Offscreen rendering | `MeshSet::render_snapshot()`, `MlGui::save_snapshot()`, `HeadlessRenderContext` | Visual feedback for a multimodal model. |
| Scale-relative parameter types | `absperc`, `@bboxDiag` expressions | Directly addresses the hardest grounding problem (see below). |
| Reproducibility | `ScriptAction` (`filterKey`, `params`, `pythonCall`, `compactPythonCall`), `UndoActionRecord`, undo-graph Python export | Audit trail, single-step rollback, and a replayable transcript, all free. |
| Seed discipline | `randomSeed` declared by 37 filters, resolved via `FilterParams::getRandomSeed()` | Randomized filters can be made deterministic on demand. |

## What is missing

Two gaps, of very different size.

1. **The headless distribution is a separate, much smaller build.** Inside the
   app, `_meshlab` is a *static* library (`src/python/CMakeLists.txt`)
   registered via `PyImport_AppendInittab`, and `pymeshlab` is a shim module
   synthesized at runtime in `PythonHost.cpp`. A real wheel does exist, in the
   sibling repository `cignoni/pymeshlab` (scikit-build-core + nanobind), which
   consumes QMeshLab as a submodule. Three kinds of drift separate them, and
   only the third is substantial:

   - *Bindings*: none by construction. That repository compiles
     `src/python/bindings/{module,meshset_core}.cpp` directly out of the
     QMeshLab submodule — the same sources the app compiles.
   - *Facade*: two hand-maintained name lists. `PythonHost.cpp` exposes seven
     names; the wheel's `python/pymeshlab/__init__.py` exposes three. Small,
     already drifted, and best fixed by giving it a single source of truth.
   - *Build graph*: the wheel's `CMakeLists.txt` hand-enumerates QMeshLab
     sources and enables **2 plugins out of the 40** wired in
     `plugins/CMakeLists.txt`. The headless build is therefore not a lagging
     copy of the app's filter set but a different, far smaller one — the
     opposite of what a model-facing catalogue needs.

   The fix is structural: either QMeshLab exports consumable CMake targets, or
   the wheel target moves into this repository and the sibling repo shrinks to
   packaging metadata. Two obstacles are real either way — `Document` does not
   separate cleanly from rendering (`meshgpuresourcecache`, `linerenderer`,
   `colormap` must be compiled for it to link), and enabling all 40 plugins
   pulls embree, CGAL, libigl, quadwild and screened-poisson into a wheel build.
   Deciding **which plugin set the headless build carries** precedes any LLM
   work: a curated subset is buildable but makes the model's filter catalogue
   diverge from the application's.
2. **No IPC of any kind.** No `QLocalServer`, no socket, no RPC. Driving the
   *live* document from outside the process requires new plumbing plus thread
   marshalling onto the GUI thread, and has to interact correctly with the
   existing "block UI while filters run" behaviour.

## Four independent axes

Most confusion about "adding an LLM" comes from collapsing these. They can be
chosen separately.

### A. Where the model runs

- **Cloud API** — best reasoning; requires network, an API key, and a decision
  about what mesh-derived data may leave the machine.
- **Local model** (llama.cpp / Ollama) — offline and private, but selecting
  correctly among 327 filters is hard for small models.
- **No embedded model.** QMeshLab speaks a protocol; the user brings their own
  client. Cheapest by a wide margin, and it makes the model a user choice rather
  than a shipped dependency.

### B. What the model manipulates

1. **Code generation.** The model writes Python; the existing console runs it.
   One integration point, very large reach, no new schema work. Weakness: no
   structured error recovery unless a loop is built around it.
2. **One tool per filter.** Precise and fully validated, but 327 tools with 1131
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
  QMeshLab over `QLocalServer`. Substantially more valuable — the user watches
  the mesh change, edits land in the undo graph, the viewport is the feedback —
  and the only option needing real new plumbing.
- **In-app chat dock.** Most product-like, most code. Amounts to
  re-implementing an agent harness (streaming, tool loop, provider config,
  cancellation, cost accounting) that existing clients already provide.
- **CLI batch mode** (`qmeshlab --script foo.py`). Not LLM-specific, but the
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
   QMeshLab is better positioned than most comparable tools.
5. **Visual QA** — snapshot to a vision model: "normals are inverted on the
   inner shell."
6. **Parameter suggestion** inside the existing filter panel. The smallest
   possible integration, contained to one dialog.

## Likely interface shape

If an agentic integration is built, the tool surface should be small and fixed,
with the 327 filters reached through discovery rather than enumeration:

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

0. **Descriptor and naming quality.** Already in progress for other reasons; it
   is the prerequisite regardless of which path is taken.
1. **Converge the headless build with the application build.** No LLM content
   at all, but it unblocks every out-of-process option, plus headless batch use
   and CI. Includes choosing the plugin set the wheel carries — see the second
   gap above.
2. **Headless tool server** over that package, out of tree. Find out empirically
   where a model goes wrong before designing around assumptions.
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
