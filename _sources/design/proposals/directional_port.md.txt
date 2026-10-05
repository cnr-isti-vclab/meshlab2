# Directional Port

This document proposes a `filter_directional` plugin for field synthesis, analysis,
visualization, seamless parametrization and field-driven polygon remeshing.
**Not implemented.** Ten filters are proposed in three stages, starting with a
face-based 4-RoSy field and the tools to inspect it. Choices below are
recommendations, not completed rulings.

See also: [Frame Fields](frame_fields.md) for the QuadWild/Instant Meshes consumer
workflow, [CinoLib Port](cinolib_port.md), [Geogram Port](../history/geogram_port.md),
[Adding a Filter](../adding_a_filter.md), [Vocabulary](../vocabulary.md),
[Data Model](../data_model.md), [Filter Organization](../filter_organization.md).

## Status and evidence

Surveyed **2026-10-03** against Directional `master` at
[`25738730958f0bfa68289ae628a5cd3c4b719d61`][upstream]. The current computational
headers, their implementations and relevant tutorials were read at that revision.
MeshLab storage and consumer references were checked in the current working tree.
This is a source-level plan: no Directional compilation, numerical validation or
performance measurement has been performed for this proposal.

The older [Frame Fields](frame_fields.md) proposal identified the opportunity but
several of its integration assumptions do not hold for the surveyed trees:

| Earlier assumption | Verified state and consequence |
|---|---|
| Per-face `PD1` is already available | `src/core/vcgmesh.h` has curvature directions on **vertices**, not faces. A generic vcglib exporter calling `face.PD1()` does not establish that `VCGFace` has that component. Field storage needs an explicit contract. |
| Combing solves face-to-vertex transfer | `combing` cyclically reorders directions on the existing tangent bundle. It does not interpolate a face field onto vertices. Transport, interpolation and singularity checks remain necessary for Instant Meshes. |
| No polygon/quad extraction | Current `setup_mesher.h`, `mesher.h`, `NFunctionMesher.h` and tutorial 505 implement polygon extraction from integrated functions. A third remeshing backend is a real candidate, subject to validation. |
| Three C++20 substitutions suffice | Current tutorials require C++20; `std::numbers` is used in multiple active headers, including tangent bundles and field conversion. Prefer a private C++20 implementation target over an assumed three-line patch. |
| Legacy named headers describe the current API | Conjugate/angle-bound implementations and `IterativeRoundingTraits.h` are under `Deprecated/`. Current PolyVector iteration and integration APIs must be surveyed independently. |

## Why add Directional

MeshLab's existing remeshers compute fields internally, but do not expose a general
workflow for creating a field, inspecting singularities, changing its constraints
and reusing it. Directional supplies that workflow and, in the current tree, its own
integration-to-meshing path.

| Capability | Existing route | Directional contribution |
|---|---|---|
| Designed surface directions | Vertex principal curvature directions; internal remesher fields | Constrained power fields and more general PolyVectors |
| Singularity analysis/design | Mostly internal to remeshers | Explicit indices, prescribed topology and residual checks |
| Field visualization | No general field workflow | Streamline segments; MeshLab can also build glyph polylines from raw vectors |
| Curl reduction | No general field-editing filter | Projection and PolyVector iteration before integration |
| Seamless parametrization | Existing conformal/harmonic/atlas methods solve different objectives | Branched functions with rotational and integer-transition constraints |
| Field-driven mesh extraction | Instant Meshes and QuadWild | Polygon extraction from integer isolines of a seamless N-function |

Keep three concepts separate: a **4-RoSy cross field** has four equivalent directions
spaced by 90 degrees; a **frame field** can contain two nonorthogonal axes and their
negatives; a general **N-directional field** need not have either symmetry. One
unit `PD1` vector can represent the first locally, but not the latter two. The first
release supports face-based degree four, with explicit symmetry metadata, rather
than promising every tangent bundle and degree supported by the library.

## Dependency and plugin shape

Recommended integration: a pinned submodule at `external/directional`, a small
project-owned wrapper target, and `plugins/filter_directional/UPSTREAM.md` recording
the pin, license evidence and any patches. No Directional port exists in this
checkout's vcpkg ports tree.

- Active computational headers use Eigen, including `unsupported/Eigen/Polynomials`.
  Use MeshLab's existing `Eigen3::Eigen`; avoid the bundled Eigen include path.
- Set C++20 **privately** on the translation units compiling Directional. MeshLab
  remains C++17. Do not expose Directional types or C++20 headers in the plugin's
  public interface. Verify the compiler/standard-library combination on macOS and
  Windows in Phase 0 rather than assuming mixed-standard compilation is sufficient.
- Do not add the tutorial CMake project: it sets C++20, adds Polyscope unconditionally,
  and handles optional GMP with global settings. Build only computational headers;
  no `directional_viewer.h`, Polyscope, tutorial I/O helpers or googletest submodule.
- The surveyed core files have MPL-2.0 banners but there is no top-level LICENSE
  file. Record per-file provenance and retain notices; asking upstream to add the
  top-level file is useful, not a reason to invent an absent license text.
- Extraction uses exact-number helpers. `exact_geometric_definitions.h` chooses
  `ENumber_GMP.h` when `USE_GMP_ENABLED` is defined, otherwise `ENumber_internal.h`
  and `BigInteger.h`. The former includes `gmpxx.h`: verify **both** GMP C++ and C
  linkage, packaging and notices. The internal fallback exists but is not evidence
  of equivalent performance or robustness. Recommend benchmarking both and using
  the existing GMP dependency for production extraction if the C++ target is usable.
- This is largely header-defined code, but some functions are **not inline**
  (`setup_mesher`, `mesher`, several PolyVector iteration helpers). Prove a
  multi-translation-unit link. Isolate definitions behind one implementation TU
  where necessary, or carry a documented inline patch; do not indiscriminately
  include algorithm headers in every family file.

```text
plugins/filter_directional/
  CMakeLists.txt
  filters.json
  UPSTREAM.md
  directionalfilterplugin.h/.cpp   # descriptors and dispatch
  directionalfielddata.h/.cpp      # immutable MeshLab-owned payload
  directionaladapter.h/.cpp        # source maps and plain-array interface
  directionalbackend.cpp           # private C++20 algorithm implementation
```

Split the backend further only after the header-definition audit. Build gate:
`MESHLAB2_PLUGIN_FILTER_DIRECTIONAL`; target `MeshLab2PluginFilterDirectional`;
plugin id `meshlab2.filter.directional`; display name `Directional Filters`.
Wire through `plugins/CMakeLists.txt` and `plugins/filterpluginregistry.cpp`, with
the same registry used by GUI, Python and tests. Provenance project `Directional`,
repository `https://github.com/avaxman/Directional`, license `MPL-2.0`, integration
`external/directional`. The gitlink identifies the installed revision.

## Field storage is the first implementation gate

Use the existing immutable [`LayerData`](../../../src/core/layerdata.h) mechanism,
under `meshlab2.filter.directional/field`, rather than a global plugin cache or
repurposed curvature component. `Document::setLayerData` and snapshots already
share immutable payloads; `describe()` gives the layer panel a field summary and
`approximateBytes()` accounts for its storage. An edited field installs a new
payload. A producer returns `ModifyCurrentMesh` even when its only output is data
attached to that layer.

The payload should own plain numeric data, not Directional objects:

- schema version, face domain, degree, symmetry kind (RoSy or sign-symmetric frame),
  normalization policy and generator/constraint metadata;
- a deterministic compact source-face map and a geometry/connectivity signature;
- raw extrinsic vectors, CCW ordered in each oriented face: initially four XYZ
  vectors per face, retaining lengths for general frames;
- optionally, validated matching/cycle diagnostics tied to that exact field version.
  Recompute solver matrices and factorization caches per invocation initially.

`CartesianField` stores a pointer to a tangent bundle, which in turn refers to a
`TriMesh`. Persisting a shallow copy would leave dangling references when the filter
returns. Rebuild `TriMesh` → `PCFaceTangentBundle` → `CartesianField` together for
each invocation and preserve their lifetimes in that order.

Two framework details need explicit tests and possibly small changes:

1. **Change notification and undo.** `setLayerData` installs a pointer but does not
   itself mark a layer modified or emit a change signal. Use the existing layer-data
   producer pattern, and verify the descriptor/result bookkeeping captures an undo
   step and refreshes the panel without falsely claiming curvature or position edits.
2. **Invalidation.** `markMeshGeometryChanged` drops payloads by default, and MeshLab
   uses geometry revision bookkeeping more broadly than vertex-position changes.
   Writing singularity scalars or UVs must not make the generating field disappear.
   Settle a geometry-sensitive invalidation path in Phase 0; alternatively, retain
   this immutable payload and validate its stored geometry/connectivity signature
   at every access. Never use an unconditional `survivesGeometryChange=true` with
   no such validation. Moved vertices, changed face winding, topology edits and
   index remapping must invalidate or explicitly transform/remap the field.

`LayerData` is **not serialized into project files**. The first release must say
that clearly and support reproducible generation through Python/filter replay.
Raw-field import/export is a follow-up, not an implicit persistence guarantee.
Ordinary UVs and generated geometry remain saveable through existing MeshLab I/O.

For consumers outside this plugin, define a small backend-independent field view
(plain arrays, source mapping, degree and symmetry) when needed. QuadWild and
Instant Meshes should not acquire a dependency on Directional's tangent-bundle types.
Named custom face attributes are an alternative, but arbitrary new types do not
automatically survive `deepCopyMesh`: that code currently explicitly copies float,
int and `Point3f` attributes. LayerData avoids that type-erasure trap.

## Proposed filters

All display names below have suffix **(Directional)**. Each identifier is both
`id` and `pythonName`. All ten filters take `SingleMesh` input.

**Category ruling needed:** propose `Attribute/Direction` for producers and field
editing. It is not in the closed ontology today; implementing it requires updating
`filtercategories` and `vocabulary.md` together. Tables mark it with **†**. If that
extension is declined, use existing `Attribute/Custom`; do not call a direction
field curvature merely because curvature can supply constraints.

### Phase 1 — generate a cross field and make it inspectable

| Display name, before suffix | Identifier | Primary category | Output |
|---|---|---|---|
| Compute Cross Field by Power Optimization | `compute_cross_field_by_power_optimization_directional` | `Attribute/Direction` † | Current layer: field payload |
| Create Polylines from Direction Field Glyphs | `create_polylines_from_direction_field_glyphs_directional` | `Creation` | New edge layer; tag `field glyphs` |
| Compute Direction Field Singularity Indices | `compute_direction_field_singularity_indices_directional` | `Attribute/Scalar` | Current mesh: vertex scalar |
| Create Polylines from Direction Field Streamlines | `create_polylines_from_direction_field_streamlines_directional` | `Creation` | New edge layer; tag `field streamlines` |

**Power field.** [`power_field`][power] → `power_to_raw` → `principal_matching`,
with degree fixed at four initially. Offer `constraintMode` = selected faces or
boundary alignment, `direction` (a local-space `point3f` direction for the selected
faces), and `alignmentWeight` with a separate hard/soft choice. Project directions
to each face tangent plane; reject zero projections and conflicting hard constraints
on a face instead of letting later entries be silently ignored. Boundary corners
can supply multiple incompatible directions, so aggregate or reject explicitly.
Require a nonempty, valid constraint set. Upstream's empty-constraint comment says
“lowest-eigenvalue”, but the implementation anchors the first tangent space to its
local X axis. Do not expose that as an unconstrained eigenfield.

The output is the complete field, not only selected faces. Selection is constraint
input, so no `selectionScope` or `selectedOnly`. Normalize only nonzero vectors;
detect vanishing power coefficients and report invalid directions instead of
dividing by zero. Store the source mesh unchanged and the field in LayerData.

**Glyphs.** Build four short segments from each sampled face barycenter using the
raw vectors. This is MeshLab adapter code over Directional output, not a call to
the Polyscope viewer. Parameters: `glyphScale` relative to mean edge length and a
bounded `maxGlyphs`, with deterministic face sampling. Document uniform-length
versus magnitude-scaled display. It is the cheapest independent test that vectors
are tangent, correctly oriented and attached to the intended faces.

**Singularities.** [`principal_matching` and `effort_to_indices`][matching] require
a valid CCW-ordered raw field. Use the tangent bundle's local-cycle mapping to
recover source vertices; never treat all generator/boundary cycles as vertex IDs.
Write fractional index `singIndices / N` to `VQ`, zero on regular interior vertices,
and return a scalar visualization hint. Restrict this initial filter to closed
manifold input so boundary indices are not silently presented as interior ones.
For a closed connected surface, test that the sum of indices equals Euler
characteristic. Preserve the field payload and source selection.

**Streamlines.** [`streamlines_init` / `streamlines_next`][streamlines] expose
`StreamlineState::segStart` and `segEnd` as geometry. Use selected faces as explicit
seeds by default, or deterministic bounded seed-face sampling; expose `maxSteps`
and a positive step/time scale. The auto-sampling path uses `distRatio` in a
division, despite checking only nonnegativity: do not accept zero or unbounded
automatic sample counts. Stop at boundaries/dead trajectories and cap total
segments. The state can update existing segment endpoints, so assemble final
geometry from the final state rather than appending every snapshot blindly.
Return actual VCG edges; do not merge unrelated crossings by coordinate proximity.

### Phase 2 — richer frames and controlled topology

| Display name, before suffix | Identifier | Primary category | Output |
|---|---|---|---|
| Compute Frame Field by PolyVector Optimization | `compute_frame_field_by_polyvector_optimization_directional` | `Attribute/Direction` † | Current layer: field payload |
| Compute Cross Field from Prescribed Singularities | `compute_cross_field_from_prescribed_singularities_directional` | `Attribute/Direction` † | Current layer: field payload |
| Compute Frame Field by Curl Reduction | `compute_frame_field_by_curl_reduction_directional` | `Attribute/Direction` † | Current layer: replacement field payload |

**PolyVectors.** [`PolyVectorData`, `polyvector_field`, `polyvector_to_raw`][polyvector]
support degree-four sign-symmetric frames whose two axes need not be orthogonal.
Reuse Phase 1 constraints; add a second projected direction for each selected face,
and explicit `smoothnessWeight`, `symmetryWeight` and alignment controls. Validate
two nonparallel tangent axes. Retain vector lengths: a PolyVector frame is not a
normalized cross field. The negative upstream RoSy weight means exact symmetry;
map that to an explicit mode rather than an unexplained negative UI parameter.
Test CCW root ordering and near-coincident roots before matching/integration.

**Prescribed singularities.** [`index_prescription`][prescription] accepts integer
indices for **all basis cycles**, including topology generators and boundaries.
Begin with connected, closed genus-zero meshes. Read desired fractional indices
from selected vertices' existing scalar values, require each `N * value` to be an
integer within tolerance, and set unselected local cycles to zero. Require their
sum to equal Euler characteristic; e.g. eight +1/4 singularities on a sphere for
N=4. Expose `globalRotation`; check the returned infinity-norm residual and the
singularities recomputed from the result. Inconsistent input may otherwise produce
a least-squares field with unintended singularities. Higher genus and boundaries
need explicit cycle-period controls, not just relaxing the topology check.

**Curl reduction.** [`project_curl`][curl] and the current PolyVector iteration
hooks provide the numerical route. Start with a valid sign-symmetric face field,
measure curl, project with a symmetry reduction, and optionally iterate a bounded
number of times with the current `curl_projection`/`soft_rosy` hooks. Recompute
matching and report before/after curl and changed singularities. Low curl is not
a guarantee of a globally injective map. The direct projector currently leaves
hard constraints unimplemented (`TODO: hard constraints`); do not offer a “preserve
constraints” toggle unless the wrapper supplies and tests the missing constrained
solve. Preserve provenance but mark any inherited constraint guarantees invalid.

Combing is an internal stage, not an extra filter in the initial count. It changes
representative ordering, not the geometric set of directions, and is meaningful
only together with matching and cuts. Its current traversal starts at tangent
space zero and assumes three neighbours: constrain this port to connected triangle
face bundles, and verify all faces are visited after any supplied cuts.

### Phase 3 — integrate, inspect the grid and extract a surface

| Display name, before suffix | Identifier | Primary category | Output |
|---|---|---|---|
| Parametrize by Seamless Direction Field Integration | `parametrize_by_seamless_direction_field_integration_directional` | `Parametrization/UV Creation` | Current mesh: wedge UVs and integration payload |
| Create Polylines from Integrated Field Isolines | `create_polylines_from_integrated_field_isolines_directional` | `Creation` | New edge layer; tag `field isolines` |
| Remesh by Direction Field Integration | `remesh_by_direction_field_integration_directional` | `Meshing/Remeshing` | New polygonal mesh; tag `field remesh` |

**Seamless integration.** [`setup_integration` → `integrate`][integration] works
on face fields and creates a temporary cut mesh. Initially accept N=4 with checked
sign symmetry and two independent directions. `IntegrationData(4)` uses two
independent functions; expose a positive `lengthRatio` (upstream default 0.02) and
an explicit integer-transition mode (`integralSeamless`). `roundSeams` determines
the rounding route; validate each exposed configuration rather than inheriting a
tutorial default unnoticed.

Check the boolean solver result, constraint residuals, finite values and flipped
UV faces. The **implementation** returns `NCornerFunctions` as `#F x (3*N)`, despite
a transposed dimension in the header comment. Map the two independent coordinates
of each corner to `WT`. Per-vertex UVs cannot represent seams, so do not overwrite
them with an arbitrary incident wedge value. Retain the cut mesh, full N-functions
and required transition/integer data in a second immutable payload keyed by field
version and geometry. Do not normalize each chart into [0,1]: that would destroy
the grid scale and transition relations. No atlas packing or foldover-free
guarantee is implied.

**Isolines.** [`branched_isolines`][isolines] returns vertices, edges and function
identity on the cut mesh. Clear output arrays before use; the routine appends.
Use it as the reference for a diagnostic polyline preview, with a strict segment
budget. Its convenience implementation passes a fixed `100` to `isolines`; inspect
and expose meaningful spacing/level semantics through the lower-level API rather
than pretending that constant is a user parameter. Keep seam/branch identity:
coordinate welding can incorrectly connect different branches. These are contours
of integrated functions, distinct from tracing the raw direction field.

**Remeshing.** [`setup_mesher` → `mesher`][mesher] returns `(VOutput, DOutput,
FOutput)`, where D contains each polygon's valence. Consume a valid integer
integration payload and return a new geometry-only surface; fail with guidance to
run integration if it is missing or stale. Preserve the existing layer and field.
Do not try to reconstruct MesherData from wedge UVs alone: its exact transition
matrices, compressed function and integer variables are part of the input.

Degree four should produce a quad-oriented grid, but the API promises **polygons**,
not all quads. Preserve exceptional polygons at singularities and boundaries,
report the valence distribution, and defer a name such as “Remesh to Quads” until
the actual output contract warrants it. Export via MeshLab's polygon support with
faux internal edges; do not silently triangulate away the logical faces. Check
mesher success, degeneracies, manifoldness and geometric deviation. Exact internal
arithmetic does not remove the need to validate the final floating-point output.

## Adapter, execution and consumer contracts

All initial filters require nonempty, finite, consistently oriented, nondegenerate
**triangle** surfaces. Begin with a single connected orientable manifold component;
permit boundaries only on paths that are tested for them. Reject logical polygons
with instructions to convert to triangles explicitly. Build compact index maps for
live elements; exclude unreferenced vertices from solvers without losing source
attributes, and reject constraints on excluded elements. Never weld seam vertices
or repair topology implicitly. Committed field maps must match the document's
post-cleanup indices, not just the temporary import order.

Work in layer-local coordinates. Field vectors are tangents, not normals; retain
the source matrix on new glyph, streamline, isoline and remeshed layers. Matrix-only
transforms leave the local representation usable; freezing a nonuniform transform
changes orthogonality and must invalidate/recompute a cross field unless an explicit
transport policy exists. Never reapply a source transform to already transformed
output. Tests must cover reflection and nonuniform scale as well as translation.

Only scalar visualization writes `VQ`, only integration writes `WT`; field-only
filters must not claim `PD1`, curvature or geometry outputs that they do not write.
New-layer descriptors declare their `outputTag`. Keep computation and colorization
separate, with scalar hints for diagnostics. Inputs that depend on LayerData need
plugin preflight validation or a framework requirement extension; today's
`inputRequirements` booleans do not express “has a valid direction field.”

Run algorithms on temporary data and commit only validated results. Several upstream
solvers and meshing invariants use assertions; propagate failures as filter errors
where the algorithm lacks status returns. Add cancellation at iterations/stages
and bound streamline/meshing memory. Sparse factorizations may remain
noninterruptible. A production mesher needs a cooperative budget hook or isolated
worker if its internal growth cannot be bounded; an elapsed-time check after it
returns is not a working timeout. Avoid global stdout redirection for diagnostics.

The existing remeshers remain useful consumers, but are **separate integration work**:

- **QuadWild:** its `.rosy` input expects a 4-RoSy representative per face. Serialize
  the new field view or use an appropriately equipped temporary mesh; the current
  `VCGFace` cannot simply be passed to `Save4ROSY`. Retain the warning from the
  consumer proposal: providing a field bypasses `BatchProcess`, including adaptive
  remeshing and sharp-feature detection. Test supplied `.sharp` data or a separated
  preprocessing path before presenting this as equivalent to the default workflow.
- **Instant Meshes:** use `CQ()`/`CQw()` guidance after an explicit face-to-vertex
  transport and N-fold interpolation. Combing is only an aid to representative
  consistency. Check singularities and degeneracies after transfer; a visually
  smooth average is insufficient. Begin with orthogonal 4-RoSy fields, not arbitrary
  nonorthogonal PolyVector frames.

A negative QuadWild comparison does not invalidate Directional's independent field,
parametrization and extraction features. Judge each workflow on its own results.

## Further candidates and exclusions

| Candidate | Route in the surveyed tree | Priority |
|---|---|---|
| Field import/export | `read_raw_field`, `write_raw_field`, matching/singularity I/O | Useful follow-up for reproducibility; require degree, face-order and mesh-correspondence validation. A raw file alone does not identify its mesh. |
| Higher-degree fields and meshes | Power/PolyVector APIs; tutorials 502/505 | After N=4. General N-functions are not ordinary two-coordinate UVs; design storage/UI semantics before exposing an arbitrary degree spinner. |
| Vertex-based fields | `IntrinsicVertexTangentBundle`, `ExtrinsicVertexTangentBundle` | Later synthesis/analysis; current integration and curl projection are face-based. |
| Curvature-driven constraints | `shape_operator`, tutorial 106; MeshLab vertex curvature | Useful producer option after curvature reliability and vertex-to-face transfer are tested. Do not confuse a principal-curvature frame with a designed field. |
| Ginzburg–Landau, conjugacy, extrinsic alignment | Current PolyVector iteration hooks; tutorials 303/305 | Advanced field families after the basic frame representation and nonlinear stopping criteria work. |
| Hodge decomposition, harmonic forms and DEC | `discrete_exterior_calculus.h`, tutorials 601–605 | Separate analysis roadmap: edge cochains/forms need distinct storage and interpretation. |
| Legacy angle-bound/subdivision implementations | `Deprecated/`, unsupported tutorials | Do not treat as current supported filters. These can pull libigl and legacy APIs; resurvey if a specific method is requested. |
| Interactive painting and singularity dragging | Viewer/tutorial interactions | Separate tool design. One-shot filters and scalar constraints establish the computational path first. |
| Polyscope viewer | `directional_viewer.h` | Not ported. MeshLab renders generated edge geometry and existing mesh/UV data. |

## Phases and acceptance gates

| Phase | Deliverable | Required evidence |
|---|---|---|
| 0 — feasibility and storage | Pinned headless C++20 probe, Eigen/ODR audit, immutable field payload and invalidation contract | Power→raw→matching runs on fixtures; two-TU link; no viewer dependency; undo/duplicate/reindex behaviour; cheap integration/mesher probes expose later blockers |
| 1 — four inspectable filters | Power field, glyphs, singularities, streamlines | GUI and Python agree; constraints respected; tangent/CCW vectors; correct closed-surface index sum; bounded valid edge geometry |
| 2 — three field-design filters | PolyVector frame, prescribed singularities, curl reduction | Frame symmetry/length preservation; prescription residual and actual singularities; measured curl reduction without false hard-constraint guarantees |
| 3 — three downstream filters | Seamless integration, isolines, polygon extraction | Correct corner layout and seams; integer transition checks; valid polygon/faux-edge round trips; extraction budgets and GMP configuration tested |

Planning estimates, not commitments: Phase 0 2–3 days; Phase 1 4–7 days; Phase 2
4–7 days. Estimate Phase 3 after the integration and mesher probes; exact extraction,
growth control and failure propagation can dominate the work. QuadWild/Instant
Meshes consumers have their own acceptance gates and are not included in these
ten filters.

Use actual invariant tests rather than screenshots as the primary evidence:

- A planar patch with imposed direction: tangency, N-fold symmetry, hard-constraint
  agreement, deterministic replay and known straight streamlines.
- Sphere and torus: closed-surface singularity sum, local-cycle/source mapping,
  prescribed integer numerators and incompatible prescriptions rejected.
- Irregular disk: bounded streamlines, no invalid boundary accesses, chart seams,
  integer transition residuals and flipped/degenerate UV detection.
- A constant planar integrated frame: predictable grid orientation/spacing and
  polygon valences; a curved singular field to test exceptional cells.
- Deleted elements, seams, invalid winding, degeneracy and disconnected input:
  safe refusal or documented remapping, never silently dropped constraints.
- Undo/redo, duplicate layer, scalar writes, UV writes, geometry edits and transforms:
  payload stays usable only when its mapping and geometry remain valid.
- Plugin-disabled and enabled builds, headless Python calls, macOS/Windows packages,
  and multi-TU linking; exact backend comparison when extraction is enabled.

No implementation test is claimed by this proposal. These are the checks the port
must pass before its filters are considered shipped.

## Decisions to settle before implementation

1. Approve the ten-filter staged scope, with face-based degree four first.
2. Choose `Attribute/Direction` or existing `Attribute/Custom` for field producers.
3. Adopt immutable LayerData, including its session-only persistence limitation,
   and settle the geometry-sensitive invalidation/notification path in Phase 0.
4. Prefer private C++20 translation units and a pinned submodule over a global
   standard bump or a speculative compatibility patch.
5. Confirm the constraint UI: projected directions on selected faces, and selected
   vertex scalars for prescribed fractional singularity indices.
6. Choose the tested exact-number backend and extraction budget mechanism after
   measurement. Keep polygon output explicit until pure-quad guarantees are proven.

[upstream]: https://github.com/avaxman/Directional/tree/25738730958f0bfa68289ae628a5cd3c4b719d61
[power]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/power_field.h
[matching]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/principal_matching.h
[streamlines]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/streamlines.h
[polyvector]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/polyvector_field.h
[prescription]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/index_prescription.h
[curl]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/project_curl.h
[integration]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/integrate.h
[isolines]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/branched_isolines.h
[mesher]: https://github.com/avaxman/Directional/blob/25738730958f0bfa68289ae628a5cd3c4b719d61/include/directional/mesher.h
