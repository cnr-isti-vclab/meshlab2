# LLM Integration

This document maps the option space for driving MeshLab from a large language
model and says which path we would take. **Nothing described here is
implemented, and none of it is scheduled.** It exists to make the design choices
explicit before any code is written, and to name which of MeshLab's existing
mechanisms are already load-bearing for this purpose.

See also: [Architecture](../architecture.md) (layers and ownership),
[Python Scripting](../../python_scripting.md) (the `ms` / `pymeshlab2` surface),
[Vocabulary](../vocabulary.md) (the naming grammar that makes filters machine-legible),
[Filter Organization](../filter_organization.md) and [Adding a Filter](../adding_a_filter.md)
(descriptor schema).

## The substrate that already exists

MeshLab is closer to being model-drivable than it looks. The relevant assets:

| Asset | Where | Why it matters |
|---|---|---|
| **423 filters / 39 plugins**, 1473 declared parameters | `plugins/*/filters.json` | Each descriptor carries `id`, `pythonName`, `shortDescription`, `longDescriptionMarkdown`, `tags`, `categories`, `inputDomain`/`outputDomain`, `inputRequirements`, and typed `parameters` with `default`/`min`/`max`/`help`. This is a tool schema already; converting it to JSON Schema is mechanical. |
| Controlled vocabulary and naming grammar | [Vocabulary](../vocabulary.md) | Tool-selection accuracy is dominated by whether names and descriptions are predictable. |
| Generic dispatch | `Document::runFilter(filterKey, params)`, `MeshSet::apply_filter()` | One entry point; no per-filter binding work. |
| Descriptors from Python | `MeshSet::list_filters()`, `MeshSet::filter_descriptor(name)` | The live build's catalogue, and each filter's full descriptor keyed as in `filters.json`, including injected parameters, with defaults and ranges resolved against the current mesh. This is what `search_filters` and `describe_filter` return. |
| Pre-flight validation | `Document::validateFilterInvocation()`; `FilterInfo::applicable` / `applicabilityError` | Lets a caller be told *why* a filter cannot run, in a form it can act on, before running it. |
| Structured results | `MeshFilterRunResult` — `success`, `errorMessage`, `infoMessages`, `newMeshIndices`, `outputValues` | The observation half of an act/observe loop. |
| Declared input requirements | `requireFaces`, `requireVertices`, `requireEdges`, `requireVertexQuality`, `requireFaceQuality`, `requireEdgeQuality`, `requireVertexColor`, `requireFaceColor`, `requireTextureCoordinates`, `requirePerVertexTexCoords`, `requirePerWedgeTexCoords`, `requireTextures` | Machine-checkable preconditions — a caller can repair state instead of guessing. |
| **20 measurement / information filters** | `Measurement/*` categories | `get_info`, `compute_geometric_measures`, `compute_topological_measures`, vertex/face quality stats and histograms, `compute_hausdorff_distance`, `compute_chamfer_distance`. This is the sensor set: it is what makes *self-checking* possible rather than open-loop guessing. |
| Offscreen rendering | `MeshSet::render_snapshot()`, `MlGui::save_snapshot()`, `HeadlessRenderContext` | Visual feedback for a multimodal model. |
| Scale-relative parameter types | `absperc` (97 parameters), `@bboxDiag` expressions | Directly addresses the hardest grounding problem (see below). |
| Reproducibility | `ScriptAction` (`filterKey`, `params`, `pythonCall`, `compactPythonCall`), `UndoActionRecord`, undo-graph Python export | Audit trail, single-step rollback, and a replayable transcript, all free. |
| Seed discipline | `randomSeed` declared by 52 filters across 13 plugins, resolved via `FilterParams::getRandomSeed()` | Randomized filters can be made deterministic on demand. |
| Importable headless package | `pymeshlab2` wheels, built from this repository's build graph and binding sources for CPython 3.10–3.14 on macOS arm64 and Windows x64 | A tool server can `import pymeshlab2` with no GUI. The wheel links the whole plugin set, so it exposes the same filters as the application. |

## What is missing

- **No IPC of any kind.** No `QLocalServer`, no socket, no RPC. Driving the
  *live* document from outside the process requires new plumbing plus thread
  marshalling onto the GUI thread, and has to interact correctly with the
  existing "block UI while filters run" behaviour.
- **No batch command line.** The executable accepts only `--generate-docs` and
  `--capture-window`; there is no `meshlab --script foo.py`.
- **The wheel is not on PyPI**, and there are no Linux wheels. An out-of-tree
  server is easy to install only once `pip install pymeshlab2` works.

## The catalogue comes from the build

A model that is told "these are MeshLab's filters" should be told so by the build
it is actually talking to. Any tool server enumerates filters from the live
`MeshSet` (`list_filters()`), never from this repository's descriptors, and
reports the backing version alongside. Today the wheel and the application carry
the same set, but an installed wheel can lag the tree it is compared against, and
a build without an optional dependency drops that plugin's filters.

## Feeding filter knowledge to the client

The client learns about filters only through what the server sends it, and the
whole catalogue is too large to send at once: the manifests total 1.1 MB, around
300k tokens, and even a one-line index of names and short descriptions is around
16k tokens, beyond what a small local model can spend before it starts working. So
the knowledge goes in layers, each pulled only when needed:

1. **Orientation, sent once.** The descriptions of the fixed tools, plus the
   server's `instructions` (MCP sends them when the client connects): what MeshLab
   is, the `Verb Object [(Backend)]` naming grammar, the 11 categories, that
   parameters scale with the mesh (`absperc`, the bounding box), and to measure
   before and after changing anything. A few hundred tokens, written by hand.
2. **Discovery, per question.** `search_filters` returns a few ranked matches:
   name, `pythonName`, short description, categories, tags, and whether each can
   run on the current document and if not why. Search runs in the server.
   Lexical matching over names, tags and descriptions comes first, expanded with
   the rejected synonyms of [Vocabulary](../vocabulary.md), which map a user's
   `delete` or `decimate` onto `Remove` and `Simplify`; embeddings only if
   measurement shows lexical search missing.
3. **Detail, per filter.** `describe_filter` returns the full descriptor: the
   long description, every parameter with type, default, range and help, input
   requirements, and references.
4. **Browsable material.** The vocabulary, the category tree and one page per
   filter as MCP resources (`meshlab://filter/<id>`), for clients that let the
   user or model browse. Many clients support only tools, so nothing essential
   lives only here.
5. **Recipes.** How filters combine — clean a scan, simplify under a Hausdorff
   bound, bake a texture from photos — is the knowledge no descriptor holds.
   Written as MCP prompts or as short `pymeshlab2` example scripts, it is new
   content rather than an export, and the place where MeshLab's experience would
   go.

Layers 2 and 3 come from the running build (see the previous section), so they
are only as good as the descriptors; layers 1 and 5 are written for the purpose.

## Distribution and clients

An MCP server is a program the client starts and talks to, usually over
stdin/stdout. It does not need a new native executable:

- **Headless.** A Python module over `pymeshlab2`, using the official `mcp`
  Python SDK, shipped as an optional extra of the wheel (`pip install
  pymeshlab2[mcp]`) and started as `python -m pymeshlab2.mcp`. Everything native
  is already in the wheel.
- **Live document.** A mode of the existing executable (`MeshLab --mcp`), or a
  `QLocalServer` inside the running application with a small stdio shim
  forwarding MCP messages to it, so edits are visible and land in the undo graph.

The model is chosen by the user's client, not by MeshLab. A local runtime such as
Ollama serves the model and its tool calling but does not speak MCP itself: a client
sits between them, running the model, starting the server and relaying the calls.
Goose, mcphost and Open WebUI do this with Ollama; LM Studio does it with its own
local runtime. The client is configured with an entry such as

```json
{ "command": "python", "args": ["-m", "pymeshlab2.mcp"] }
```

and the same server works unchanged with cloud clients. Run locally, the model,
the server and the mesh all stay on the user's machine.

Choosing among 423 filters is the hard part for a small model. The small fixed tool
set and the predictable names are what make it workable; a mid-size model with
good tool calling is the realistic floor, to be confirmed by the benchmark below
rather than assumed.

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
2. **One tool per filter.** Precise and fully validated, but 423 tools with 1473
   parameters exceeds the practical tool budget of every current model.
3. **Search / describe / apply.** A small fixed tool set over dynamic discovery
   in the descriptors. Almost certainly the right shape; the vocabulary work is
   what makes the search leg viable.
4. **Driving the GUI directly.** Rejected — brittle, unobservable, untestable.

### C. How it is wired

- **Out-of-process, headless.** An MCP (or similar) server over `pymeshlab2`.
  No GUI and no C++ changes. Testable in CI, safe to iterate on, works with
  existing model clients.
- **Out-of-process, live document.** The same server attached to a running
  MeshLab over `QLocalServer`. Substantially more valuable — the user watches
  the mesh change, edits land in the undo graph, the viewport is the feedback —
  and the only option needing real new plumbing.
- **In-app chat dock.** Most product-like, most code. Amounts to
  re-implementing an agent harness (streaming, tool loop, provider config,
  cancellation, cost accounting) that existing clients already provide.
- **CLI batch mode** (`meshlab --script foo.py`). Not LLM-specific, but the
  substrate for headless automation and CI.

### D. What it is for

The use case should drive the three axes above, not the reverse.

1. **Filter discovery** — "which filter removes these spikes?" Retrieval over
   descriptors. No mesh mutation, no agent loop. Lowest risk, immediate value.
2. **Script authoring** — natural language to a `pymeshlab2` script.
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
   │  search_filters(query)      → ranked descriptors │  MeshSet::list_filters()
   │  describe_filter(name)      → full param schema  │  MeshSet::filter_descriptor()
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
  classification work *is* the enabling work here, and it is never finished.
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

Ordered by risk and by what each step unblocks. Descriptor and naming quality
is the standing prerequisite for all of them: whichever path is taken, its
quality bounds everything downstream.

1. **Headless tool server** over `pymeshlab2`, out of tree. Find out
   empirically where a model goes wrong before designing around assumptions.
   Enumerates filters from the live `MeshSet` and reports the wheel's version.
   Publishing the wheel to PyPI makes it installable by anyone.
2. **Sensor and vision tools** in the same server, closing the act/observe loop.
3. **`QLocalServer` bridge** so the same server can attach to a running GUI
   session.
4. **In-app chat dock** — only if 1–3 prove out and the goal shifts from a
   power-user capability to a shipped feature.

## Non-goals

- Replacing the filter panel or the Python console with a chat prompt.
- Letting a model drive the GUI by synthesising mouse and keyboard input.
- Shipping model weights, or hard-wiring a single provider.
- Uploading mesh geometry to a remote service as part of normal operation.
