# CinoLib Port

This document proposes a `filter_cinolib` plugin bringing CinoLib's surface
mapping, topology and polygon-processing algorithms into MeshLab. **Not
implemented.** The recommended starting point is four filters; five more follow
in two separately gated stages. These are proposed choices, not completed rulings.

See also: [Geogram Port](../history/geogram_port.md), [Adding a Filter](../adding_a_filter.md),
[Filter Organization](../filter_organization.md), [Vocabulary](../vocabulary.md),
[Data Model](../data_model.md), [Directional and Frame Fields](directional_port.md).

## Status and evidence

Surveyed on **2026-10-03**, against CinoLib `master` at commit
[`151dc5c46309affedadb9b45619cc137fb7326ed`][upstream]. The headers, implementations,
examples and build configuration were read from that revision. MeshLab comparisons
come from the current `plugins/*/filters.json` and adapter implementations, rather
than the historical counts in earlier proposals.

This is a source-level feasibility study. No CinoLib algorithm has been compiled,
benchmarked or exercised in MeshLab for this proposal. Build compatibility,
numerical behaviour and effort estimates remain to be measured in Phase 0.

## Why CinoLib, given the existing backends

CinoLib is especially interesting where its author's research implementations
extend what MeshLab can do. Another implementation of an existing algorithm is
also welcome under the algorithm-archive policy, but need not lead the port.

| Capability | Current MeshLab | What CinoLib would add |
|---|---|---|
| Homotopy basis and canonical polygonal schema | A Reeb-graph filter exists, but no homotopy-basis/schema filter | Explicit handle loops and a route from a closed genus-g surface to a cut planar domain |
| ARAP UV mapping | libigl harmonic/LSCM; geogram LSCM, spectral LSCM, ABF and atlas filters | A local/global ARAP surface parametrizer |
| Robust disk embeddings | Existing parametrizers have different numerical and distortion objectives | Advancing Front Mapping (AFM) and Stripe Embedding, both research implementations with refinement and exact/high-precision paths |
| Polygon dualization | Quad remeshers and polygon subdivision exist | Direct construction of a polygonal surface dual, including boundary handling |
| Coarse quad layout | Instant Meshes and QuadWild generate quad meshes | Analysis of an **existing** quad mesh into singularity-driven patches |
| Polyharmonic scalar interpolation | A harmonic scalar filter and libigl Hessian-energy smoothing exist | An explicit n-harmonic constrained solve, with order as a parameter |
| Tutte embedding | Some overlap with harmonic parametrization | A simple uniform-weight convex-boundary baseline and a useful adapter test |

The full CinoLib surface/volume abstraction is not the scope of this port.
MeshLab's persistent mesh is a `vcg::tri::TriMesh`; logical polygons use faux-edge
flags, and there is no persistent tetrahedral/hexahedral/polyhedral cell container.
Surface algorithms fit today. Preserving volume cells requires a separate data-model,
I/O, rendering and undo design.

## Dependency shape

The [upstream CMake configuration][cmake] creates an INTERFACE target named
`cinolib`, advertises C++11, adds its own include directory **and bundled Eigen**,
and enables optional components through `CINOLIB_USES_*` switches. MeshLab uses
C++17. Header-only integration avoids a new CinoLib runtime library, but does not
prove the transitive headers or optional components are dependency-free.

Recommended integration:

- Pin a submodule at `external/cinolib`; record local patches and provenance in
  `plugins/filter_cinolib/UPSTREAM.md`. There is no CinoLib port in this checkout's
  vcpkg ports tree. An overlay port remains an alternative if dependency policy
  later calls for it.
- Use a small project-owned INTERFACE target exposing `include/` and linking the
  existing `Eigen3::Eigen`. Do not put CinoLib's bundled Eigen on the include path.
  Prove compatibility with the installed Eigen in Phase 0.
- Keep OpenGL/GLFW/ImGui, VTK, Triangle, TetGen, graph cuts, Spectra and exact
  arithmetic modules disabled for the initial set. Do not invoke unpinned upstream
  `FetchContent` downloads during application configuration.
- Retain header-only inclusion of implementation `.cpp` files; do not define
  `CINO_STATIC_LIB` without supplying the corresponding compiled definitions.
  Include algorithm headers in private translation units to contain compile cost.
- Preserve the upstream [MIT notice][license]. Optional dependencies have their
  own notices: in particular, Stripe Embedding includes the bundled `mpreal.h`,
  whose banner states GPL-3.0-or-later or a commercial alternative. Its dependency
  and distribution treatment must be recorded before that stage ships; the
  CinoLib MIT provenance alone is not a complete account of that configuration.

Follow geogram's one-dependency/one-plugin shape:

```text
plugins/filter_cinolib/
  CMakeLists.txt
  filters.json
  UPSTREAM.md
  cinolibfilterplugin.h/.cpp       # registration and dispatch
  cinolibmeshadapter.h/.cpp        # compact indices, polygons, write-back
  cinolibparametrization.h/.cpp    # Tutte, ARAP; later schema, AFM, stripes
  cinolibtopology.h/.cpp           # homotopy basis, dual, coarse layout
  cinolibscalar.h/.cpp             # constrained polyharmonic fields
```

Build gate: `MESHLAB2_PLUGIN_FILTER_CINOLIB`. Static plugin target:
`MeshLab2PluginFilterCinolib`. Plugin id: `meshlab2.filter.cinolib`; display name:
`CinoLib Filters`. Wire the target through `plugins/CMakeLists.txt` and registration
through `plugins/filterpluginregistry.cpp`, including the test and Python consumers
of the shared registry. Use provenance project `CinoLib`, repository
`https://github.com/mlivesu/cinolib`, license `MIT`, integration `external/cinolib`.
The gitlink is the installed revision; do not duplicate it in the descriptor.

For a given stage, register a complete, tested filter set. If the exact-mapping
stage is adopted, explicitly settle whether its dependency union becomes required
for the plugin or a tested build feature controls both its sources and descriptors.
Never register a filter whose compiled implementation is unavailable.

## Recommended filter set

All names below carry the suffix **(CinoLib)**. The identifiers in the tables are
both `id` and `pythonName`; all filters take `SingleMesh` input. New-layer tags
follow the existing source-name plus `outputTag` convention.

### Phase 1 — four filters using the triangle adapter

| Display name, before suffix | Identifier | Primary category | Output |
|---|---|---|---|
| Parametrize by Tutte Embedding | `parametrize_by_tutte_embedding_cinolib` | `Parametrization/UV Creation` | Current mesh: UVs |
| Parametrize by As-Rigid-As-Possible Mapping | `parametrize_by_as_rigid_as_possible_mapping_cinolib` | `Parametrization/UV Creation` | Current mesh: UVs |
| Compute Polyharmonic Scalar Field | `compute_polyharmonic_scalar_field_cinolib` | `Attribute/Scalar` | Current mesh: vertex scalar |
| Create Polylines from Homotopy Basis | `create_polylines_from_homotopy_basis_cinolib` | `Creation` | New edge layer; tag `homotopy basis` |

**Tutte.** [`Tutte(Trimesh&, polygon_type)`][tutte] replaces the temporary mesh's
positions with the embedding; it does **not** write UVs. Copy its XY coordinates
to the original mesh's UV channels without changing original geometry. Start with
a circle boundary; a square option can follow once boundary sampling is tested.
Require a connected, consistently oriented, nondegenerate manifold disk: one
boundary loop and Euler characteristic one, with no isolated vertices. Reject
other topology instead of choosing an arbitrary boundary. Check the boundary and
graph conditions needed for a nondegenerate Tutte embedding, including separating
simplices. Report failure if the resulting map collapses faces.

**ARAP.** [`ARAP_2D_mapping(Trimesh&, ARAP_2D_map_data&)`][arap] provides `uv_out`
and writes `vert_data(vid).uvw`. Parameter `iterations` defaults initially to the
upstream value 4, with a bounded positive range. Its initializer calls LSCM and
the global solve fixes the last vertex; it is not an API for arbitrary user-pinned
UVs. Start with the same disk restriction. Do not advertise foldover-free mapping:
check finite output and report flipped/degenerate UV faces. Validate the solver
before writing anything to the document.

**Polyharmonic field.** [`harmonic_map(mesh, bc, n, laplacian_mode, solver)`][harmonic]
accepts a map of vertex indices to scalar Dirichlet values. Use the **selected
vertices' existing scalar values** as constraints; require vertex scalar data and
at least one selected constraint in every connected component. Parameters:
`order` (1–3, default 2) and `weightMode` (uniform/cotangent). It forms powers of
the discrete Laplacian, so help must describe that formulation rather than claim
equivalence to every mass-weighted biharmonic discretization. Restrict higher
orders initially because sparse matrix powers can increase memory substantially.
Write `VQ` only, keep prescribed values, and return a scalar visualization hint.
Selection supplies constraints; it does **not** confine the result to selected
vertices, so do not declare `selectionScope` or add `selectedOnly`.

**Homotopy basis.** [`homotopy_basis(mesh, HomotopyBasisData&)`][homotopy] returns
vertex-index loops and their total length. Require a connected, closed, orientable
manifold surface of positive genus. Use exactly one explicitly selected vertex as
the base point; reject zero or multiple roots with an actionable message. Set
`detach_loops=false` and `globally_shortest=false` for the initial filter. The
fixed-root route is documented as O(n log n), whereas searching all roots is
O(n² log n). Export one polyline layer containing the 2g loops, keeping vertices
separate between loops where needed to preserve their identity; report genus,
loop count and total length. This is a surface topology generator, not a curve
skeleton or a shortest path through the volume. The source mesh remains intact.

### Phase 2 — three filters after topology/polygon round trips

| Display name, before suffix | Identifier | Primary category | Output |
|---|---|---|---|
| Parametrize by Canonical Polygonal Schema | `parametrize_by_canonical_polygonal_schema_cinolib` | `Parametrization/UV Creation` | New cut/refined surface with UVs; tag `canonical schema` |
| Create Dual Mesh | `create_dual_mesh_cinolib` | `Meshing/Remeshing` | New polygonal surface; tag `dual` |
| Compute Coarse Quad Layout | `compute_coarse_quad_layout_cinolib` | `Attribute/Scalar` | Current mesh: face patch indices |

**Canonical schema.** [`canonical_polygonal_schema`][schema] requires a detached
homotopy basis and positive genus. Compute the basis and schema in one invocation;
do not depend on hidden state left by the polyline filter. The same root-selection
and closed-manifold preconditions apply. Start with edge-split refinement to
preserve the source surface; upstream's vertex/hybrid strategies can move it.
Expose a bounded refinement budget once it can be enforced in the implementation.
The schema routine cuts and can further refine `m_in`, then produces a planar
`m_out` bounded by a 4g-sided polygon. Export the final 3D cut mesh with UVs from
the corresponding planar mesh, verifying connectivity correspondence. This cannot
be implemented as a UV-only write-back onto the original index map. Use uniform
weights initially; the API's cotangent default is not a universal injectivity
guarantee. No atlas packing is implied.

**Dual surface.** The surface overload of [`dual_mesh`][dual] returns vertex
positions and arbitrary polygon faces, or a `Polygonmesh`. Parameter
`includeBoundaryCells` maps to `with_clipped_cells`; expose a crease-angle option
only after the adapter maps CinoLib's `CREASE` edge flag and tests its effect.
Initially support validated orientable manifold triangle/polygon surfaces.
Dualize logical polygons, not MeshLab's storage triangles. Export through the
polygon adapter with internal triangulation edges marked faux. This is not a
Voronoi remesher or a tetrahedral dual. Geometry-only output is acceptable and
must be stated explicitly.

**Coarse quad layout.** [`compute_coarse_quad_layout(Quadmesh&)`][coarse] traces
from singular vertices, marks interface edges, and writes a patch label per quad.
Require a pure logical quad mesh; reject quad-dominant input containing exceptional
triangles. Write each quad's label to all of its constituent MeshLab triangles
(`FQ`) and return a scalar hint, without baking colors. This analyzes quad topology;
it does not generate quads or compute a frame field. `Attribute/Scalar` is the
initial category because these are structural patches, not necessarily UV charts.
An optional separate boundary-polyline filter can follow when there is demand.

### Phase 3 — two distinctive mapping filters, subject to a headless port

| Display name, before suffix | Identifier | Primary category | Output |
|---|---|---|---|
| Parametrize by Advancing Front Mapping | `parametrize_by_advancing_front_mapping_cinolib` | `Parametrization/UV Creation` | New refined surface with UVs; tag `advancing front map` |
| Parametrize by Stripe Embedding | `parametrize_by_stripe_embedding_cinolib` | `Parametrization/UV Creation` | New refined surface with UVs; tag `stripe embedding` |

These are high-value candidates, but not thin wrappers at the surveyed revision:

- [`AFM_data`][afm] owns two `DrawableTrimesh<>` instances; [`SE_data`][stripe]
  owns one. That class is defined only with `CINOLIB_USES_OPENGL_GLFW_IMGUI`.
  Both APIs are additionally guarded by `CINOLIB_USES_CGAL_GMP_MPFR`.
  Disabling the viewer switch alone does not produce a headless implementation.
- Isolate computation onto `Trimesh<>`, carrying a small documented patch or
  obtaining the separation upstream. Do not introduce GLFW/ImGui to service
  a computational filter in MeshLab's QRhi application.
- Both initializers have a non-manifold `exit(-1)` path. Stripe initialization
  also removes a triangle when the Euler characteristic is not one. Validate a
  disk before calling either, and convert process-terminating failure paths into
  returned errors in the integration. A C++ exception handler cannot catch `exit`.
- Stripe's MPFR precision setting changes the default precision. Scope/restore
  that setting and establish thread safety before enabling it in-process.
- Both can refine connectivity. Return the refined 3D mesh and corresponding UVs,
  with a documented attribute-transfer policy. Check convergence and resource
  limits; AFM's per-step timeout is not a total execution budget.
- Exact internal coordinates do not prove that MeshLab's stored UVs are injective
  after conversion. Validate signed areas and boundary/global overlap conditions
  in the actual stored representation. Never label a rounded, flipped result
  “guaranteed injective.”

Start with a circle target domain. AFM exposes circle/square/star domains, while
Stripe can accept explicit boundary conditions; nonconvex targets and custom
boundary input should follow a separate contract and regression set. For Stripe,
expose the numeric mode only after the supported exact/MPFR configuration is
reproducibly built. Keep failure atomic: an unfinished map produces no new layer.

## Candidates to add later, and candidates to defer

| Candidate | Verified upstream route | Recommendation |
|---|---|---|
| Heat geodesic field | `geodesics.h`, `compute_geodesics` | Later comparison backend. Both ordinary and amortized implementations normalize the output to [0,1]; do not publish it as mesh-unit distance without changing and validating the implementation. vcglib and libigl already supply heat-distance filters. |
| Heat diffusion | `heat_flow.h`, `heat_flow` | A useful scalar filter after the constrained-field plumbing; needs explicit source and time/unit semantics. |
| LSCM | `lscm.h`, `LSCM` | Low-cost archive addition after ARAP, but overlaps libigl/geogram. |
| Isocontour polyline / contour refinement | `isocontour.h`, `Isocontour`, `tessellate` | Existing TrueForm contour creation/cutting already covers the workflow. Segments are protected, not exposed by a public getter; add a small headless accessor if ported. `tessellate` inserts vertices, not a complete cut-along-contour operation. |
| Isotropic remeshing | `remesh_BotschKobbelt2004.h`, `remesh_Botsch_Kobbelt_2004` | Existing vcglib/TrueForm overlap. One call performs one iteration. Its header includes `drawable_trimesh.h` despite taking `Trimesh&`; fix/include-audit before claiming headless standalone compilation. |
| ARAP deformation | `ARAP.h`, `ARAP` | Defer until handle/anchor constraints have a usable filter contract; the upstream interactive example is not itself a one-shot MeshLab filter. |
| Laplacian eigenfunctions | `matrix_eigenfunctions.h` and example 46 | Later scalar backend; needs a pinned compatible Spectra build and ordering/normalization tests. |
| Heat descriptors | `HKS.h`, `HKS` | Do not infer semantics from the name: implementation solves landmark heat fields over sampled times and returns a matrix, rather than the usual diagonal spectral HKS. Needs a descriptor-storage and naming decision; it does not call Spectra. |
| Subdivision, simplification, sampling, smoothing, intersections | Corresponding headers/examples in the pinned source tree | Possible archive expansion after the distinctive set. Compare attributes, robustness and performance against the existing filters before prioritizing. |
| Tetrahedralization, hex/polyhedral processing, volume isosurfaces | `tetrahedralization.h`, `coarse_layout.h`, `isosurface.h`, volume examples | Separate volume proposal. Boundary extraction alone would lose the principal result. Optional Triangle/TetGen dependencies are not part of this surface port. |
| Viewer, controls and file-format wrappers | `gl/`, drawable classes, I/O examples | Reuse MeshLab's viewer and I/O; no viewer port. |

The normalized geodesic result is visible in [the implementation][geodesics],
not its return type. Likewise, the heat-descriptor behaviour is visible in
[`HKS.cpp`][hks]. These are the same kind of API-versus-behaviour distinction that
made the geogram dependency and spectral probes necessary.

## The adapter is the shared implementation work

**Triangle import.** Build compact CinoLib vertices/faces and explicit forward and
reverse source maps, skipping deleted elements. Exclude unreferenced vertices from
solves while retaining the source data untouched; reject selected constraints or
roots on excluded vertices. Reject invalid indices, repeated corners, nonfinite
positions and zero-area triangles rather than silently repairing them. Check
manifoldness, orientation, connectivity and boundary conditions before algorithms
that assert those properties. Do not weld coincident vertices: they may represent
intentional seams. Triangle-only filters initially reject logical n-gons with an
explanation to convert to triangles explicitly.

**Polygon import/export.** Follow the existing `PolygonSupport<VCGMesh, PMesh>`
route in `filter_meshing`: check faux-edge consistency, reconstruct each ordered
logical face, and maintain logical-face-to-storage-face mappings. Export arbitrary
polygons with a valid tessellation and symmetric faux flags. Check concave faces,
boundary cells and malformed faux groups. Never send each storage triangle as a
quad or reconstruct quads from an arbitrary adjacent triangle pair.

**Attribute write-back.** For unchanged topology, copy only the declared output
through source maps. Tutte/ARAP write vertex UVs and synchronize wedge UVs so stale
corners cannot override them; retain geometry, colors and scalar data. Scalar
filters modify `VQ` or `FQ` only and return visualization hints. CinoLib's `uvw`,
`label`, `MARKED` and `CREASE` fields are algorithm scratch state, not automatic
counterparts of MeshLab texture coordinates, custom attributes or selection.

**Changed topology.** Build a new result off-document and validate it before adding
the layer. Dual and refined mapping outputs initially promise geometry and the
explicit generated attributes only; do not silently retain invalid textures,
selections or custom-attribute arrays. Refinement provenance/interpolation can be
added later. Require exact final 3D/UV index correspondence for schema/AFM/Stripe.

**Transforms.** Initial algorithms operate in source-local coordinates; scalar
units and homotopy lengths are described accordingly. New 3D surface and polyline
layers inherit the source transform. Keep the matrix unchanged for in-place
attributes. Test nonidentity and nonuniform transforms to catch double application;
do not claim world-space distances from a local-space solve.

**Selection.** Root and boundary-condition selection is input data, not an execution
scope. None of the initial nine filters declares `selectionScope`; none promises
partial remeshing or partial UV writes. Preserve user selection on the source.

**Execution safety.** Keep temporary meshes and caches invocation-local initially.
Audit solver failure reporting: finite output alone is insufficient; verify residuals
and constraints, adding status propagation where upstream only asserts. Use the
Document progress/cancellation path around stages and bounded iterations. A sparse
factorization may remain noninterruptible; document that limitation. If resource
limits cannot be enforced inside a refinement loop, add a hook or keep that filter
deferred. Forward useful diagnostics without globally redirecting process stdout.

## Delivery phases and acceptance gates

| Phase | Work | Exit evidence |
|---|---|---|
| 0 — dependency and feasibility | Pin source; compile headless probes using MeshLab's Eigen; inspect transitive includes, error paths and optional dependencies | Tutte, ARAP, harmonic solve and fixed-root basis run on tiny fixtures; no viewer linkage or configure-time network fetch; second translation unit links without duplicate definitions |
| 1 — first four filters | Plugin, triangle/edge adapters, descriptors, UV/scalar/basis filters | GUI and Python execute the same registered filters; source attributes and selection survive; undo/redo works; invalid inputs return errors |
| 2 — topology and polygons | Schema with refinement budget, polygon adapter, dual and coarse layout | Cut mesh/UV correspondence, polygon round trips and quad labels verified; no faux-edge loss or partial result on failure |
| 3 — exact mapping | Headless AFM/Stripe adaptation, dependency/license record, numeric and cancellation controls | macOS and Windows build/package checks; convergence and stored-UV validation; no process exit or implicit source editing |

Phase 0 should also probe Phase 2/3 headers cheaply, so an advanced-method blocker
is found before their implementation starts. Reuse existing optional-plugin disabled
build checks and the actual registry/descriptor tests rather than maintaining a
parallel registration path. Estimated effort is only planning guidance: Phase 0
1–2 days, Phase 1 4–7 days, Phase 2 4–8 days; estimate Phase 3 after the headless
and numeric probes. These ranges include adapter and behavioural tests, not a
commitment to upstream response times or platform fixes.

### Behavioural checks that justify shipping

| Area | Fixtures and assertions |
|---|---|
| Common import | Deleted/unreferenced vertices, coincident seam vertices, invalid/non-manifold/degenerate faces; no silent welding or dropped constraints |
| UV filters | Asymmetric triangulated disk; finite UVs, expected boundary, unchanged 3D coordinates and attributes, vertex/wedge agreement; annulus/closed/disconnected surfaces refused |
| Polyharmonic | Prescribed values reproduced, residual checked, nonconstant fixture, every component constrained; empty selection/NaN constraints refused; distinguish orders 1 and 2 |
| Homotopy | Torus and genus-two surface yield 2 and 4 closed rooted loops along input edges; sphere/open surface refused; source unchanged |
| Canonical schema | Genus-one and genus-two surfaces become disks; final 3D and planar connectivity agree, seam copies receive independent UVs, edge-split geometry stays on the source surface; budget failure is atomic |
| Polygon dual | Closed tetrahedron plus an open patch with boundary-cell modes, concave polygon input; valid polygon boundaries and round-trip faux flags |
| Quad layout | Regular quad patch, extraordinary vertex and periodic mesh; labels cover every logical quad and agree on all constituent triangles; mixed triangle/quad input refused |
| AFM/Stripe | Difficult disk with narrow features, refinement and numeric-mode cases; detect nonconvergence and flips after output conversion, reject invalid topology before upstream initialization |
| Integration | Nonidentity transform, cancellation, undo/redo, scalar rendering, Python replay, plugin disabled; no new layer or source mutation after failure |

Do not use equality with another backend as the only correctness oracle. Use
topological invariants, boundary constraints, solver residuals and geometric checks;
backend comparisons are supplementary measurements of quality and cost.

## Decisions to settle before implementation

1. **Initial scope:** recommend the four Phase 1 filters, followed by the five
   gated candidates. If exact disk mapping is the primary motivation, advance its
   Phase 0 feasibility probes, not an unverified promise to ship it first.
2. **Dependency ownership:** recommend the pinned submodule plus a headless wrapper
   target using the existing Eigen; maintain a documented patch set only where needed.
3. **Constraints:** recommend one selected root for topology filters and existing
   scalar values on selected vertices for polyharmonic constraints. A richer named
   constraint-set UI can be designed separately.
4. **Output policy:** recommend new geometry-only layers plus generated UVs for
   topology-changing maps, and a new geometry-only dual. Attribute interpolation
   would expand the first implementation considerably.
5. **Advanced mapping:** decide whether to carry the headless/status-return patches
   locally or wait for upstream, and settle the exact-dependency build feature.
6. **Volumes:** defer to a separate proposal with a persistent cell representation.

These questions do not block reviewing the filter roadmap. Implementation should
start by resolving them and recording the Phase 0 measurements here, following the
geogram proposal's practice of replacing assumptions with evidence.

[upstream]: https://github.com/mlivesu/cinolib/tree/151dc5c46309affedadb9b45619cc137fb7326ed
[cmake]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/cinolib-config.cmake
[license]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/LICENSE
[tutte]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/tutte.cpp
[arap]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/ARAP_2D_map.cpp
[harmonic]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/harmonic_map.cpp
[homotopy]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/homotopy_basis.h
[schema]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/canonical_polygonal_schema.cpp
[dual]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/dual_mesh.h
[coarse]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/coarse_layout.cpp
[afm]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/AFM/AFM.h
[stripe]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/stripe_embedding/stripe_embedding.h
[geodesics]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/geodesics.cpp
[hks]: https://github.com/mlivesu/cinolib/blob/151dc5c46309affedadb9b45619cc137fb7326ed/include/cinolib/HKS.cpp
