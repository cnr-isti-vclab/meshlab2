# Frame Fields

This document proposes feeding an explicit **frame field** to the two quad
remeshers that already ship. **Not implemented.** The computational library port
is planned separately in [Directional Port](directional_port.md).

See also: [Adding a Filter](../adding_a_filter.md), [Vocabulary](../vocabulary.md),
[Data Model](../data_model.md), [Geogram Port](../history/geogram_port.md) (one possible producer).

## Status

Originally surveyed 2026-09-20; revised **2026-10-03** after reading the current
Directional source and MeshLab types. The [Directional Port](directional_port.md)
now owns the filter roadmap, source pin, dependency and storage design. This
proposal owns the two existing-remesher consumer workflows.

The earlier survey incorrectly assumed that `VCGFace` already stored `PD1`, that
combing implemented face-to-vertex transfer, and that Directional had no mesh
extractor. Those assumptions are corrected below and in the port proposal.

## A field needs its own representation

`src/core/vcgmesh.h` stores principal-curvature directions on **vertices**.
`VCGFace` does not have a curvature-direction component. The generic vcglib
`Save4ROSY` exporter calling `face[i].PD1()` is not evidence that MeshLab's current
face type can instantiate it.

The recommended storage is immutable plugin-owned `LayerData`, containing a
face-based raw field with degree, symmetry and source-mapping metadata. See
[Directional Port — field storage](directional_port.md#field-storage-is-the-first-implementation-gate)
for undo, invalidation, memory accounting and the project-serialization limitation.
A small backend-independent field view should connect producers and consumers.

Start with orthogonal 4-RoSy fields for these two consumers. Arbitrary nonorthogonal
PolyVector frames cannot be reduced to one representative direction without losing
information. Directional's richer frame and integration workflows remain useful
independently of external-remesher support.

## Consumer 1 — QuadWild

**Upstream already accepts a field file, and the format is ours.**

[`quadwild.cpp:122`](../../../external/quadwild-bimdf/quadwild/quadwild.cpp) parses any
argument containing `.rosy` and sets `parameters.hasField`. `functions.cpp:35` then
branches:

```cpp
if (!parameters.hasField) {
    MeshPrepocess<FieldTriMesh>::BatchProcess(trimesh, BPar, FieldParam);
} else {
    bool success = trimesh.LoadField(fieldFilename.c_str());
    ...
}
```

`LoadField` ([triangle_mesh_type.h:328](../../../external/quadwild-bimdf/components/field_computation/triangle_mesh_type.h))
dispatches `.rosy` to **vcglib's** `ImporterFIELD::Load4ROSY`
([import_field.h:222](../../../vcglib/wrap/io_trimesh/import_field.h)) — the same
vcglib this repo builds against — then reorients the field coherently and recomputes
singularities.

Because QuadWild runs as a helper process over files
([quadwildfilterplugin.cpp:188](../../../plugins/filter_quadwild/quadwildfilterplugin.cpp)),
the plugin-side change is three things:

1. serialize the face-based 4-RoSy field beside the temp OBJ, preserving face
   order; use a dedicated writer or a temporary mesh with the required face
   component, since `ExporterFIELD<VCGMesh>::Save4ROSY` is not directly usable;
2. append that path to the existing argument list
   (`{inputPath, "2", prepConfig}` gains a fourth entry);
3. gate the parameter on a valid, matching face-field payload; enabled vertex
   curvature storage is not evidence that such a field exists.

`SaveAllData` ([mesh_manager.h:775](../../../external/quadwild-bimdf/components/field_computation/mesh_manager.h))
writes `_rem.obj`, `_rem.rosy` and `_rem.sharp` on both branches, so the downstream
`quad_from_patches` plumbing is untouched.

### ⚠ The catch: a supplied field disables much more than field computation

`BatchProcess` ([mesh_manager.h:722](../../../external/quadwild-bimdf/components/field_computation/mesh_manager.h))
is not a field routine that happens to be skipped. In order, it does:

| Step | Lost when a field is supplied |
|---|---|
| `UpdateDataStructures` | bookkeeping |
| `InitSharpFeatures` | **sharp-feature detection** |
| `AutoRemesher::RemeshAdapt` | **the adaptive remesh** |
| `InitFeatureCoordsTable` | the feature table downstream reads |
| `SolveGeometricArtifacts` | artifact repair after refinement |
| `RefineIfNeeded` | refinement for field consistency |
| `MeshFieldSmoother::SmoothField` | the field itself — the part we meant to replace |

So the user gets their field at the cost of the remesh and the feature lines, on a tool
whose paper is *Reliable **Feature-Line Driven** Quad-Remeshing*. In practice a `.sharp`
file would have to be supplied alongside (the CLI accepts one on the same footing), and
the input pre-remeshed by hand. This is an upstream design choice; nothing on the
MeshLab side can hide it, and the descriptor must say so plainly rather than presenting
the option as free.

**Effort remains to be measured** after the field view and export adapter exist.
Include face-order correspondence, constraints, supplied feature data and the
preprocessing difference in the test, not just whether the helper accepts a file.

## Consumer 2 — Instant Meshes

Instant Meshes is vendored and linked, so there is no file boundary to work through,
and [the adapter](../../../plugins/filter_instant_meshes/instantmeshes_adapter.cpp)
already touches the machinery involved. Two injection points, both with accessors
already public in [hierarchy.h:50-83](../../../plugins/filter_instant_meshes/upstream/src/hierarchy.h):

- **As a guidance constraint** — `hierarchy.CQ()` / `CQw()` followed by
  `propagateConstraints(4, 4)`. This is *exactly* the mechanism the existing
  `alignBoundaries` option uses, applied to every vertex rather than to boundary
  vertices. The field steers the optimizer instead of replacing it, so the multigrid
  solve still runs and a mediocre field degrades the result rather than wrecking it.
- **As the initial solution** — `hierarchy.Q(0)` is a non-const accessor; write it
  after `resetSolution()` and shorten or skip `optimizeOrientations`.

**The real work is neither.** Instant Meshes' field is **per-vertex**; the proposed stored field is
**per-face**. 4-RoSy directions cannot be averaged naively — each is defined only up to
a 90° rotation about the normal, so a plain mean of incident face directions cancels.
Every contribution must be rotated into a common representative frame first. vcglib's
`CrossField` has the primitives (QuadWild leans on `OrientDirectionFaceCoherently` for
the same reason), but it is fiddly and quietly wrong when done badly: a bad transfer
yields a plausible-looking field whose **singularity structure** is different, and
singularities are what determine the quad layout.

Directional's `principal_matching` and `combing` help with representative
consistency, but **do not implement this domain transfer**. The current combing
routine reorders vectors on an existing tangent bundle. Transport to vertex tangent
planes, weighting, interpolation, degeneracy handling and verification remain
adapter work. Estimate that work after a measured transfer prototype.

## Where a field would come from

| Producer | Status | Notes |
|---|---|---|
| Principal curvature directions | **ships today, per vertex** | A possible guidance source; needs a validated field conversion, not a face `PD1` passthrough |
| geogram `GlobalParam2d::frame_field` | not ported | per-facet `vec3` attribute, sharp edges as constraints; see [Geogram Port](../history/geogram_port.md) |
| geogram `FrameField::create_from_surface_mesh` | not ported | separate class, spatial search, handles volumetric fields too |
| Directional (`power_field`, `polyvector_field`, `index_prescription`, …) | not ported | a whole family of designed fields nothing here can produce — see below |
| Transferred from another layer | not implemented | a `Transfer/Between Layers` filter; needs the same rotation-into-a-common-frame care as above |
| Painted / user-authored | not implemented | an interactive tool, not a filter |

**The honest caveat, and the reason this is a proposal rather than a task.** Both
remeshers already compute curvature-driven fields internally — QuadWild's
`FieldSmoother` runs with `alpha_curv = 0.3`. So feeding them *our* curvature
directions is not obviously a win; it may well be a downgrade, since their internal
fields are smoothed and singularity-aware while ours are raw. The value is in fields
they **cannot** compute: geogram's, a painted one, or one transferred from a related
mesh so that two models quadrangulate compatibly.

That means the question "what is the intended source?" has to be answered **before**
the consumer plumbing is built, because it determines the constraints and transfer
policy. A painted field, for instance, might want per-vertex input and a weight.

## Directional port and independent value

The [Directional Port](directional_port.md) proposes ten filters in three stages:
field synthesis and inspection; PolyVector frames, prescribed singularities and
curl reduction; seamless integration, grid isolines and polygon extraction.

It also replaces this document's older dependency survey. The pinned current
source uses C++20 across multiple active headers, keeps legacy methods under
`Deprecated/`, and includes `setup_mesher`/`mesher` for polygon extraction. Combing
is useful but is not a face-to-vertex interpolator. Neither a one-vector curvature
slot nor a shared solver object is the proposed persistent field representation.

## Suggested consumer order

1. Establish the field payload and glyph/streamline visualization from the port
   proposal. Check tangency, symmetry, constraints and singularities before handing
   a field to a remesher.
2. Prototype QuadWild `.rosy` input on prepared meshes, preserving face order and
   providing feature data where needed. Compare against its normal preprocessing
   path, documenting the operations bypassed by a supplied field.
3. Prototype Instant Meshes guidance with explicit face-to-vertex transport and
   N-fold interpolation. Compare against its own field using the same resolution
   and boundary settings.
4. Keep each consumer only if its results justify the extra controls. A negative
   QuadWild experiment does **not** block Directional's independent field-design,
   parametrization or mesh-extraction features.

## Open consumer questions

1. Which designed fields improve each remesher's output, and on which fixtures?
2. Can QuadWild's input-field route preserve the desired preprocessing and feature
   constraints through supplied files, or does it need an upstream separation?
3. What transfer/weighting policy preserves useful singularity structure for
   Instant Meshes, and what should happen at an ambiguous or vanishing average?
4. Should consumers use only guidance constraints, or also expose initial-field
   replacement once its consequences are measured?

Storage, category, dependency and C++ standard choices are tracked in
[Directional Port — decisions](directional_port.md#decisions-to-settle-before-implementation),
so there is one current implementation plan for the library.
