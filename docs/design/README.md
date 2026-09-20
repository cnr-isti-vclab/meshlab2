# Design Documents

Three kinds of document live here, and the distinction is worth keeping:

- **Reference** (this directory) — how MeshLab works *today*. Read these to
  understand or change the code; correct them when the code changes.
- **[Proposals](proposals/)** — design settled before the code exists. Every one
  says "not implemented" in its opening paragraph. When one ships, its content
  moves into a reference document and the proposal is deleted, not left to rot.
- **[History](history/)** — dated records of work that is finished. They are
  evidence of how something came to be, not statements about the present.
  **Records are never edited after the fact**; if one turns out to be wrong,
  that is itself a fact about the record. Where a record's conclusions are still
  in force, the rule lives in a reference document and the record only shows the
  reasoning.

## Reference

| Document | What it covers |
| --- | --- |
| [Architecture](architecture.md) | Single-document, multi-view structure; ownership and signals. |
| [Data Model](data_model.md) | Layers, revisions, undo, what a snapshot holds. |
| [Rendering](rendering.md) | The QRhi pipeline, pass planning, render modes. |
| [Memory Accounting](memory_accounting.md) | Why there are several different numbers for "size". |
| [Preferences](preferences.md) | Application settings, sharing the filter-parameter schema. |
| [Adding a Filter](adding_a_filter.md) | The practical how-to: descriptor schema, plugin class, wiring. |
| [Vocabulary](vocabulary.md) | The controlled vocabulary: categories, verb lexicon, naming grammar, layer naming. |
| [Filter Organization](filter_organization.md) | Settled naming and organization principles, plus parameter conventions. |
| [TrueForm Plugin](trueform_plugin.md) | How the TrueForm-backed plugins relate to the rest of the tree. |

> **[Vocabulary](vocabulary.md) is read by the test suite at runtime.**
> `test_filter_descriptors.cpp` and `test_filters.cpp` open it by path and parse
> the `## 3. Verb lexicon` section. Renaming the file or that heading breaks the
> build. Adding or retiring a verb there changes what the tests enforce, which is
> the intent.

## Proposals

| Document | Status |
| --- | --- |
| [LLM Integration](proposals/llm_integration.md) | Option space for driving MeshLab from a model. Not implemented. |
| [Usage Statistics](proposals/usage_statistics.md) | Aggregate usage collection, designed for privacy. Not implemented. |
| [Gaussian Splatting](proposals/gaussian_splatting.md) | Splat loading, rendering and editing. Not implemented; some decisions taken. |
| [Edge Support](proposals/edge_support.md) | The two kinds of edge, what is missing in selection and rendering, and filters that would generate per-edge colour. Per-edge colour implemented; the rest not. |
| [Geogram](proposals/geogram_porting.md) | A `filter_geogram` plugin: booleans, ABF++/LSCM parametrization, atlas and CVT remeshing. Not implemented; plan complete and all rulings taken, ready for Phase 0. |

## History

| Document | Records |
| --- | --- |
| [Filter Classification](history/filter_classification.md) | The 2026-07-29 pass-1 migration of 272 filters. |
| [Filter Names](history/filter_names.md) | The pass-1 renaming rounds, verb by verb, with the rulings made along the way. |
| [Pass 2 Identifier Map](history/pass2_identifier_map.md) | The `id` / `pythonName` mapping applied 2026-09-03. |
| [Repository Rename](history/repository_rename.md) | The QMeshLab → meshlab2 rename, six surfaces in five phases, applied 2026-09-13. |
| [Layer Naming](history/layer_naming.md) | The 87 layer-creating filters, measured before and after, applied 2026-09-19. |
