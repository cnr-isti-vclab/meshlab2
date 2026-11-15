# Documentation Reorganization

The reference documents had grown to about 48,000 words, and five of them changed audience
from one section to the next: component contracts beside widget mechanics, current rules
beside finished migrations and unbuilt designs, and the same topic written two or three
times. This record holds the mapping table approved on 2026-11-15, measured on the
documents as they stood that day, and what was done with it.

**The rule applied:** one topic, one home; each reference document speaks to one audience.
Developer contracts stay in the reference documents, how the interface behaves goes to
[User Interface](../user_interface.md), finished migrations come here, unbuilt designs go
to [proposals](../proposals/), and a duplicate is deleted once any fact only it held has
been merged into the home.

## Decisions

- **UI behaviour** gets a reference document of its own, `user_interface.md`, rather than
  being deleted.
- **The pass-1 plugin fold** (`filter_remesh`, `filter_transfer`, ...) was never carried
  out; its plan is recorded as [history](plugin_reorganization.md), not kept as a proposal.
- **How *Trim Surface by Plane* snaps and closes its cut** moves to that filter's help, in
  the user's terms; the implementation half was already documented in vcglib's `clip.h`.
- **Plugin lists** live in the README only; Architecture points at the two registries.

## The mapping table

Verdicts: **T** trim in place, **S** split, **M** move, **D** delete (duplicate). The 79
sections kept unchanged are not listed. Word counts are of each section's body. "P2" is
[Plugin Reorganization](plugin_reorganization.md).

| Doc | Section | Words | Verdict | Target | Evidence / note |
|---|---|---|---|---|---|
| architecture.md | `Document` | 168 | T |  | responsibility + ownership; field lists are data_model's |
| architecture.md | `MeshGpuResourceCache` | 72 | T |  | one paragraph, details are rendering's |
| architecture.md | `RenderWidget` | 242 | T |  | responsibility; per-field inventory is rendering's |
| architecture.md | `ViewTrackball` | 42 | T |  | drop gesture list (UI) |
| architecture.md | `InteractiveTool` | 338 | M | user_interface.md | tool-by-tool behaviour; keep 1 para contract |
| architecture.md | `RenderOverlayPanel` | 194 | M | user_interface.md | panel layout; keep 1 sentence |
| architecture.md | `LayerWidget` | 447 | M | user_interface.md | 447-word paragraph of widget mechanics; keep 1 sentence |
| architecture.md | `MeshFilterPanel` | 97 | M | user_interface.md | panel behaviour; keep 1 sentence |
| architecture.md | `MainWindow` | 139 | T |  | orchestration role only |
| architecture.md | Render Planning Types | 138 | D | rendering.md | 3rd copy (data_model, rendering) |
| architecture.md | Render Settings Types | 102 | D | rendering.md | 3rd copy |
| architecture.md | Plugin System | 161 | T |  | plugin list -> one home (decision P) |
| architecture.md | Third-party geometry code | 317 | M | third_party.md | policy doc, different audience |
| architecture.md | Choosing how to bring third-party code in | 824 | M | third_party.md |  |
| data_model.md | I/O Model | 1898 | T |  | contracts kept; second pass later |
| data_model.md | Preference Model | 59 | D | preferences.md | duplicate |
| data_model.md | The Action History view | 1366 | M | user_interface.md | dot strips, hover captions; keep serial-keying rule |
| data_model.md | Render-State Snapshot Model | 124 | D | rendering.md | duplicate of Programmatic Render-State JSON |
| data_model.md | Render Settings Model | 3 | D | rendering.md | duplicate |
| data_model.md | `PerMeshRenderSettings` | 109 | D | rendering.md | duplicate |
| data_model.md | `GlobalRenderSettings` | 84 | D | rendering.md | duplicate |
| data_model.md | Render Request / Plan Model | 164 | D | rendering.md | duplicate |
| data_model.md | Shared vs Per-View State | 144 | D | architecture.md | 39 shared 8-word runs with architecture |
| data_model.md | GPU Cache Integration | 152 | D | rendering.md | duplicate |
| data_model.md | Memory Diagnostics | 42 | D | memory_accounting.md | duplicate |
| rendering.md | Radiance scaling | 976 | T |  | technique kept, drop history of the old material |
| rendering.md | Reference grid | 580 | T |  | keep math, drop preference walk-through |
| rendering.md | Resetting the views | 120 | M | user_interface.md | File > New behaviour |
| rendering.md | `Scene3D` Pass Details | 745 | T |  | drop pixel widths |
| rendering.md | Clipping Plane | 1782 | S |  | split: shader contract + GL rules stay; snapping/capping/trim go to the trim filters' help (decision C) |
| rendering.md | `Scene3D` Camera and Interaction | 647 | M | user_interface.md | one 647-word paragraph of gestures |
| rendering.md | Interactive Tools | 1503 | S |  | tool controls() contract stays; per-tool behaviour to user_interface.md |
| rendering.md | `ParametrizationUV` Camera and Interaction | 69 | M | user_interface.md |  |
| rendering.md | Quality Histogram Overlay | 599 | T |  | keep layout contract, drop the Retina diary |
| vocabulary.md | Why this exists | 277 | T |  |  |
| vocabulary.md | Migration: today's 32 values → the ontology | 466 | M | history/category_migration.md | 2026-07 migration record |
| vocabulary.md | 2. Representation | 110 | M | proposals/filter_representation.md | schema not implemented (0 loader hits) |
| vocabulary.md | Terminology: `representation`, not "domain" | 55 | M | proposals/filter_representation.md |  |
| vocabulary.md | The values | 75 | M | proposals/filter_representation.md |  |
| vocabulary.md | Direction matters | 59 | M | proposals/filter_representation.md |  |
| vocabulary.md | Declared, not derived | 106 | M | proposals/filter_representation.md |  |
| vocabulary.md | The need is already visible | 35 | M | proposals/filter_representation.md |  |
| vocabulary.md | Discoverability: prerequisite (done) | 254 | M | history/category_migration.md | done; before/after benchmark |
| filter_organization.md | 5. First pass = plugins + categories only | 77 | M | history/plugin_reorganization.md | pass-1 scoping |
| filter_organization.md | Goals | 73 | M | history/plugin_reorganization.md |  |
| filter_organization.md | Remaining Issues | 125 | M | history/plugin_reorganization.md |  |
| filter_organization.md | Filter Categories | 73 | D | vocabulary.md | restates vocabulary §1 |
| filter_organization.md | Categories | 51 | D | vocabulary.md | restates vocabulary §1 |
| filter_organization.md | Migration Strategy | 87 | M | history/plugin_reorganization.md |  |
| filter_organization.md | Moves since pass 1 | 84 | M | history/plugin_reorganization.md |  |
| filter_organization.md | Plugin Mapping Table (historical pass-1 proposal) | 219 | M | P2 | plugin fold never executed (no filter_remesh/transfer) |
| filter_organization.md | Stays separate — real external dependency | 265 | M | P2 |  |
| filter_organization.md | Stays separate — vendored third-party code | 106 | M | P2 |  |
| filter_organization.md | Folds into family plugins | 383 | M | P2 |  |
| filter_organization.md | `filter_meshing` breakdown (37 filters → 9 families) | 224 | M | P2 |  |
| filter_organization.md | Judgment call | 71 | M | P2 |  |
| filter_organization.md | Not filter plugins | 71 | M | P2 |  |
| filter_organization.md | Third axis: plugin display names | 486 | M | P2 |  |
| filter_organization.md | Proposed post-fold plugin set, not the current tree | 227 | M | P2 |  |
| filter_organization.md | Rulings | 413 | M | P2 |  |

| Doc | Sections | Keep | Trim | Split | Move | Delete | Words now | Words kept in place (K+T+S) | Words moving | Words deleted |
|---|---|---|---|---|---|---|---|---|---|---|
| architecture.md | 24 | 10 | 6 | 0 | 6 | 2 | 3926 | 1469 | 2217 | 240 |
| data_model.md | 21 | 10 | 1 | 0 | 1 | 9 | 6513 | 4266 | 1366 | 881 |
| rendering.md | 29 | 20 | 4 | 2 | 3 | 0 | 11201 | 10365 | 836 | 0 |
| vocabulary.md | 26 | 17 | 1 | 0 | 8 | 0 | 9016 | 7856 | 1160 | 0 |
| filter_organization.md | 39 | 22 | 0 | 0 | 15 | 2 | 5801 | 2766 | 2911 | 124 |
| **total** | 139 | 79 | 12 | 2 | 33 | 13 | 36457 | 26722 | 8490 | 1245 |

## What was done

The moves, splits and deletions, in one pass on 2026-11-15:

- New reference documents: [Third-Party Code](../third_party.md) and
  [User Interface](../user_interface.md), each made of the moved sections verbatim.
- New records: [Category Migration](category_migration.md) and
  [Plugin Reorganization](plugin_reorganization.md); new proposal:
  [Filter Representation](../proposals/filter_representation.md). Vocabulary keeps §2 as a
  pointer, since the tests parse `## 3. Verb lexicon` by its number.
- The render-settings field lists, the detailed copy, moved from Data Model into
  Rendering's *Per-Mesh Render Modes*, which had pointed at them; the two facts that only
  a deleted copy held (the `*PassGpuView` calls, the visibility synchronization) were merged
  into Rendering and Architecture.
- Every link into a moved section was repointed and checked against the headings.

The trims (**T**) and the merging of the passages that now overlap inside
`user_interface.md` are a second pass.
