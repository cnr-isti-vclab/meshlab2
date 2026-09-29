# Transform result — audit and migration

**Applied 2026-09-29.** Every filter that leaves a matrix on a layer now declares a
`transformResult` instead of a `Freeze` toggle of its own. The framework injects one
two-option parameter — *Assign matrix* / *Bake positions* — and does the baking itself. How
that works today is in [Adding a Filter](../adding_a_filter.md) and the label ruling in
[Vocabulary](../vocabulary.md) §3–4; this is the record of what was found and changed.

## What the audit found

| | |
|---|---|
| The toggle | 9 filters, all in `filter_meshing`, declare the same bool: id `Freeze`, label *Freeze Matrix*, help *Transformation is explicitly applied to vertices.*, default **on**, group `main`, last. |
| What it does | On, the filter bakes the **combined** matrix (its own × the layer's existing one) into the vertex positions and normals, and resets the layer matrix to the identity. Off, it composes its matrix onto the layer matrix and leaves the vertices alone (`applyTransform`, `meshingfilterplugin.cpp:402`). *Invert Matrix* has its own copy of the same branch (`:1314`). |
| Filters with no choice | The 5 alignment filters only ever assign a matrix. TrueForm's says so on purpose: "Alignment moves the layer, it never touches vertex coordinates" (`trueformfilterplugin.cpp:212`). |
| Where the bake lives | `applyTransformToMesh` is local to `filter_meshing` (`:345`), so no framework code can reach it. *Freeze Matrix* repeats the bake inline (`:1306`). There is no matrix-baking helper in core. |
| Consumers of `Freeze` | The interactive Transform tool commits every gesture with `Freeze = false` (`transformtool.cpp:310`); 11 call sites in `test_filters.cpp`; one example in `docs/python_scripting.md:236`, which also calls a filter id that no longer exists (`compute_matrix_from_translation`). |

Two defects on the same code paths:

- **`compose` is never read.** Both *Set Matrix* filters declare *Compose with current*
  (default off), but they call `applyTransformAndMark`, which always composes. "Set" never
  replaces a non-identity matrix. The Transform tool depends on the composing: it passes a
  delta and says so (`transformtool.cpp:309`).
- **The camera filters move meshes twice.** With *Apply to all visible cameras*, the four
  camera-transform filters bake the transform into every visible layer's vertices **and**
  compose it onto that layer's matrix (`camerafilterplugin.cpp:147`, called from `:320`,
  `:377`, `:418`, `:462`). They leave the normals alone and declare `outputModifies: []`.
  Found by reading, then reproduced by a test (see below).

## Mechanism

Modelled on [`selectionScope`](selection_scope_audit.md):

- **Declaration.** A filter that leaves a matrix on one or more layers declares
  `"transformResult": "bake_positions"` or `"assign_matrix"`. The value is its default, so
  every filter keeps today's behaviour.
- **One injected parameter.** The loader appends one `enum`, id `transformResult`, label
  *Result*, options `assign_matrix` *Assign matrix* / `bake_positions` *Bake positions*, as
  the last parameter of `main`. That is where `Freeze` sits today.
- **Filters only assign.** When *Bake positions* is chosen, the manager bakes every layer
  that existed before the run and whose matrix the run changed. It does this inside the
  filter's undo step, through a new core `Document::freezeMeshTransform()`, which
  *Freeze Matrix* then calls too. Diffing the matrices covers *Align Meshes Globally*, which
  moves many layers, without the manager knowing which ones a filter touches.
- **Tier-1 rule.** No manifest declares `Freeze` or `transformResult` by hand. Every
  declaring filter lists `TM`, `VG`, `VN` and `FN` in `outputModifies`, and the loader
  injects exactly one such parameter, last and not advanced.
- **Python.** Scripts change `"Freeze": True` to `"transformResult": "bake_positions"`.
  There is no alias, as for every other renamed parameter.

## Mapping table

Line numbers are `plugins/<plugin>/filters.json` for the declaration and the plugin's
`.cpp` for the code.

| # | Filter | Now | Evidence | Target | Verdict |
|---|---|---|---|---|---|
| 1 | Mirror or Swap Axes | `Freeze`, on | json `:839` · cpp `:913` | `bake_positions` | migrate |
| 2 | Rotate | `Freeze`, on | json `:972` · cpp `:960` | `bake_positions` | migrate |
| 3 | Rotate to Fitted Plane | `Freeze`, on | json `:1060` · cpp `:1032` | `bake_positions` | migrate |
| 4 | Normalize Reference Frame | `Freeze`, on | json `:1151` · cpp `:1239` | `bake_positions` | migrate |
| 5 | Scale | `Freeze`, on | json `:1256` · cpp `:1291` | `bake_positions` | migrate |
| 6 | Translate | `Freeze`, on | json `:1333` · cpp `:1263` | `bake_positions` | migrate |
| 7 | Invert Matrix | `Freeze`, on; its own bake branch | json `:1399` · cpp `:1314` | `bake_positions` | migrate; the branch goes |
| 8 | Set Matrix from Translation/Rotation/Scale | `Freeze`, on; `compose` ignored | json `:1535` · cpp `:1355` | `bake_positions` | migrate; `compose` now read |
| 9 | Set Matrix from Values or Layer | `Freeze`, on; `compose` ignored | json `:1748` · cpp `:1363` | `bake_positions` | migrate; `compose` now read |
| 10 | Align by ICP (vcglib) | assigns the moving layer | icp `:350` | `assign_matrix` | gains the choice |
| 11 | Align Meshes Globally | assigns every aligned layer | icp `:621` | `assign_matrix` | gains the choice |
| 12 | Align by Bounding Box (TrueForm) | composes onto the source | trueform `:213`, from `:248` | `assign_matrix` | gains the choice |
| 13 | Align by ICP (TrueForm) | composes onto the source | trueform `:213`, from `:316` | `assign_matrix` | gains the choice |
| 14 | Align to Corresponding Points (TrueForm) | composes onto the source | trueform `:213`, from `:371` | `assign_matrix` | gains the choice |
| 15 | Rotate Cameras | moves visible meshes twice | camera `:147`, from `:320` | — | fixed; no dropdown |
| 16 | Scale Cameras | moves visible meshes twice | camera `:147`, from `:377` | — | fixed; no dropdown |
| 17 | Translate Cameras | moves visible meshes twice | camera `:147`, from `:418` | — | fixed; no dropdown |
| 18 | Transform Camera Extrinsics | moves visible meshes twice | camera `:147`, from `:462` | — | fixed; no dropdown |
| 19 | Freeze Matrix | bakes by definition | cpp `:1302` | — | unchanged; calls the core helper |
| 20 | Set Matrix to Identity | nothing to bake | cpp `:1294` | — | unchanged |

**Totals:** 20 filters write a layer matrix. 9 migrate, 5 gain the choice, 4 had the
camera defect, and 2 stay as they are (9 + 5 + 4 + 2 = 20). Outside the filters, one tool,
11 test call sites and one documentation example pass `Freeze`.

## Decisions

Taken by the maintainer on the audit (2026-09-29), all four as recommended:

1. **The alignment filters gain the choice** (rows 10–14), defaulting to *Assign matrix*, which
   is what they did before.
2. **The camera filters are fixed, and get no dropdown** (rows 15–18). They move the cameras,
   and a dropdown would do nothing unless *Apply to all visible cameras* is on.
3. **`compose` is honoured** (rows 8–9). Off, which is the default, *Set Matrix* sets the
   matrix. The Transform tool passes `compose = true`, since what it hands over is a delta.
4. **Vocabulary**: *Bake positions* is admitted as an option label only (§3 note, §4 row), and
   *Freeze Matrix* keeps its name.

## What changed beyond the table

- **The bake moved to core.** `Document::transformMeshGeometry` and
  `Document::freezeMeshTransform` replace `filter_meshing`'s local `applyTransformToMesh` and
  the inline copy in *Freeze Matrix*. Vertex normals now follow the matrix's inverse
  transpose instead of its linear part. The result is the same for rotations, mirrors and
  uniform scales, and correct for a point cloud under a non-uniform scale, which the linear
  part was not.
- **The bake rule is "the matrix changed".** A run that leaves a layer's matrix unchanged,
  such as a zero rotation, bakes nothing on that layer. The retired toggle would have baked
  that layer's existing matrix as a side effect. The result on screen is the same either way.
- **The camera fix** writes the transform into the vertices once, conjugated by the layer's
  matrix so that a layer carrying one still moves in world space, and leaves that matrix
  alone. The four filters now declare `VG`, `VN` and `FN`.
  `FilterTests::cameraTransformsMoveVisibleLayersOnce` reproduced the defect before the
  fix: a translation of +1 moved a vertex by 2.
- **Tests.** `FilterDescriptorTests::transformResultIsDeclaredNotHandWritten` is the tier-1
  rule. The behavioural tests are `transformResultAssignsOrBakesTheFilterMatrix` (Rotate, and
  Freeze Matrix through the core helper), `alignmentBakesOnlyTheLayerItMoved` (vcglib ICP,
  where the moved layer is a parameter) and `setMatrixReplacesTheLayerMatrixUnlessComposing`.
  The 11 test call sites and the Python example now pass `transformResult`.
