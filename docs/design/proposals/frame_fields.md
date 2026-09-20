# Frame Fields

This document plans making a **frame field** a first-class attribute in MeshLab, and
feeding it to the two quad remeshers that already ship. **Nothing described here is
implemented.**

See also: [Adding a Filter](../adding_a_filter.md), [Vocabulary](../vocabulary.md),
[Data Model](../data_model.md), [Geogram Port](../history/geogram_port.md) (one possible producer).

## Status

As of 2026-09-20: nothing implemented, nothing scheduled. Every file reference below
was read from the tree at that date and is exact.

The idea came out of the geogram port: geogram turns out to ship a frame-field
generator and a periodic global parametrization but **no quad extraction**, so it
cannot add a third quad remesher. What it can add is the *input* to one — which is only
interesting if something in MeshLab can consume it.

## The premise is already half-true

"Make frame field a first-class attribute" is mostly a matter of naming, not storage.
vcglib's `Save4ROSY` ([export_field.h:93](../../../vcglib/wrap/io_trimesh/export_field.h))
writes its field straight out of `face[i].PD1()`:

```
<face count>
4
<x> <y> <z>          one line per face
```

`PD1` is the per-face principal-curvature-direction OCF component MeshLab already
carries (`PD1`/`PD2`/`K1`/`K2`, guarded by `IsCurvatureDirEnabled`), already written by
*Compute Principal Curvature Directions* and already understood by the layer panel. A
4-RoSy frame field on faces **is** `PD1`, with the other three directions implied by
the 4-fold symmetry and the face normal.

So what is missing is not a place to put the data. It is:

- a **decorator** to draw it, so a field can be judged before it is used;
- filters that treat a field as their **declared output** rather than a by-product of
  curvature;
- a ruling on where such a filter sits in the category ontology (see the open
  questions — `Attribute/Curvature` is where the *storage* lives but a frame field is
  not curvature).

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

1. `ExporterFIELD<VCGMesh>::Save4ROSY(mesh, path)` beside the temp OBJ — one existing
   vcglib call, no new format code;
2. append that path to the existing argument list
   (`{inputPath, "2", prepConfig}` gains a fourth entry);
3. gate the parameter on `IsCurvatureDirEnabled`, so it is greyed out with a reason
   when the layer carries no field.

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

**Effort: about half a day**, most of it descriptor text and a test.

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

**The real work is neither.** Instant Meshes' field is **per-vertex**; ours is
**per-face**. 4-RoSy directions cannot be averaged naively — each is defined only up to
a 90° rotation about the normal, so a plain mean of incident face directions cancels.
Every contribution must be rotated into a common representative frame first. vcglib's
`CrossField` has the primitives (QuadWild leans on `OrientDirectionFaceCoherently` for
the same reason), but it is fiddly and quietly wrong when done badly: a bad transfer
yields a plausible-looking field whose **singularity structure** is different, and
singularities are what determine the quad layout.

**Effort: two to three days**, most of it in the transfer and in verifying the
singularities survived it — an estimate the Directional section below cuts
substantially, because `combing.h` and `principal_matching.h` are that problem solved.

## Where a field would come from

| Producer | Status | Notes |
|---|---|---|
| Principal curvature directions | **ships today** | already in `PD1`; the obvious first source, and the least interesting one — see below |
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
the plumbing is built, because it decides whether per-face `PD1` is even the right
carrier. A painted field, for instance, might want per-vertex storage and a weight.

## Directional

[avaxman/Directional](https://github.com/avaxman/Directional) — *A library for
Directional Field Synthesis, Design, and Processing*, by Amir Vaxman. It changes the
shape of this proposal enough to be worth its own section: it is simultaneously the
missing **producer**, the missing **visualization geometry**, and the solution to the
**hardest part of the Instant Meshes integration**.

Surveyed at `master`, 2026-09-20.

### Integration friction is low

| | |
|---|---|
| **Licence** | **MPL-2.0**, in per-file banners — the same licence as libigl. ⚠ There is **no top-level `LICENSE` file**, so GitHub's detector reports no licence at all. The banners are unambiguous, but the provenance block will need a note, and somebody should ask upstream to add the file |
| **Dependencies** | **Eigen, and nothing else.** No libigl — `#include <igl/inline.h>` is present but commented out. `integrate.h`, the heaviest entry point, pulls only Eigen and its own headers |
| **Build** | Effectively header-only: 72 headers under `include/directional`, and the seven `.cpp` files there are `#include`d from their own headers, libigl-style. Nothing to compile separately, no library to link |
| **Submodules** | `googletest` and `polyscope` — tests and their viewer. Neither is needed by the computational headers |
| **Packaging** | No vcpkg port in the pinned baseline. Precedent exists both ways: a submodule like `external/quadwild-bimdf`, or an overlay port via `ci/vcpkg-ports/` |
| **C++ standard** | ⚠ Three headers (`principal_matching.h`, `effort_to_indices.h`, `index_prescription.h`) use `std::numbers`, which is **C++20**; this project sets `CMAKE_CXX_STANDARD 17`. Everything else surveyed is C++17-clean, and the usage is `std::numbers::pi`. Either bump the standard or carry a three-line patch |

### Their viewer is the one part we cannot use — and it is isolated

`directional_viewer.h` includes `polyscope/polyscope.h` and friends. MeshLab draws
through QRhi, so that header is simply one we never include. This matters less than it
sounds, because **the interesting visualization code is computation, not rendering**:

- `streamlines.h` (+ `.cpp`) — `streamlines_init` / `streamlines_next` trace the field
  and hand back `Eigen::MatrixXd` start and end points. Line segments, not draw calls.
- `isolines.h`, `branched_isolines.h` — isolines of a (branched) scalar field, likewise
  as geometry.

Those outputs map directly onto **MeshLab polyline layers** (see
[Edge Support](edge_support.md)), which means field visualization arrives as a
`Creation`-family filter producing a layer the existing renderer already draws — no
decorator, no new rendering code, no polyscope. That is a materially cheaper route to
step 1 of the order below than writing a glyph decorator from scratch.

### What it brings that nothing here has

| Capability | Headers | Why it matters |
|---|---|---|
| **Singularity computation** | `principal_matching.h`, `effort_to_indices.h` | The thing you cannot verify a field without. Every estimate in this document that says "and check the singularities survived" is this code |
| **Combing** | `combing.h` | Rotating a field into a consistent branch across a mesh — *exactly* the per-face→per-vertex problem that dominates the Instant Meshes estimate |
| **Field representations and conversions** | `power_field.h`, `polyvector_field.h`, `polyvector_to_raw.h`, `raw_to_polyvector.h` | Power fields and PolyVectors are strictly more expressive than a single `PD1` direction; conversions make `PD1` an interchange format rather than the only one |
| **Design by singularity placement** | `index_prescription.h` | Author a field by saying where the singularities go — the clearest example of a field neither remesher can compute for itself |
| **Curl reduction** | `curl_matching.h`, `project_curl.h`, `polycurl_reduction.h` | A field must be near-curl-free to integrate; without this a "valid-looking" field still produces a bad parametrization |
| **Seamless integration** | `integrate.h`, `cut_mesh_with_singularities.h`, `setup_integration.h` | A global seamless parametrization — and **with no CoMISo**: Directional carries its own iterative rounding (`IterativeRoundingTraits.h`). libigl's equivalent route is `igl/copyleft/comiso`, which we do not install and which is not MPL |
| **Constrained field design** | `conjugate_frame_fields.h`, `angle_bound_frame_fields.h` | Conjugate and angle-bounded fields, for planar-quad and bounded-distortion work |

### What it does not bring

**Still no quad extraction.** Directional gets further than geogram — it has the
integration step geogram's open library lacks — but turning a seamless parametrization
into quad facets is not in it either. Instant Meshes and QuadWild remain the only two
extractors we have, so Directional is a producer and an analyser, not a third backend.

### Assessment

It is a good fit, and unusually cheap for what it offers: MPL-2.0, Eigen-only,
header-only, and orthogonal to everything we ship. It strengthens this proposal in
three separate places at once, and it retires the main technical risk in the Instant
Meshes path.

It is still a new dependency in service of a feature set whose value is **unmeasured**
(open question 4). The order below therefore keeps the cheap experiment first.

## Suggested order

1. **See the field.** Nothing below is verifiable without it. Two routes: a glyph
   decorator written from scratch, or Directional's `streamlines.h` feeding a polyline
   layer, which needs no rendering code at all. Prefer the second if Directional is
   coming anyway.
2. **QuadWild `.rosy` passthrough.** Half a day, and it answers empirically whether an
   externally supplied field helps or hurts, with curvature directions as the control.
   **A negative result here should stop the rest.**
3. **Directional as a producer and analyser** — `principal_matching` for singularities,
   `power_field` / `index_prescription` for fields neither remesher can compute. This
   is where the answer to "what is the intended source?" actually gets settled.
4. **Instant Meshes guidance constraints**, using `combing.h` for the per-face→
   per-vertex transfer rather than writing it ourselves.

Steps 3 and 4 are the ones that justify adopting Directional; steps 1 and 2 do not
require it, and step 2 is deliberately the cheapest way to learn whether any of this
pays.

## Open questions

1. **What is the intended field source?** Everything above hangs on this, and the
   answer decides the storage. Curvature directions are the free option and probably
   the least valuable.
2. **Category for a frame-field filter.** The storage is `PD1`, which puts it in
   `Attribute/Curvature`, but a frame field is not curvature — it is a direction field
   that happens to reuse the curvature slot, exactly the kind of storage-slot-as-concept
   confusion [vocabulary](../vocabulary.md) §4 rejects for `quality`/`scalar`. A new
   `Attribute/Direction` subcategory may be the honest answer, which is an extension to
   a closed ontology and needs ratifying.
3. **Per-face or per-vertex?** `PD1` is per-face and matches QuadWild and geogram.
   Instant Meshes is per-vertex. A painted field probably wants per-vertex plus a
   confidence weight. Supporting both doubles the transfer code.
4. **Does a supplied field actually improve either remesher?** Unmeasured. Step 2 above
   exists to answer it, and a negative answer should stop the rest.
5. **Directional: submodule or overlay port?** No vcpkg port exists. A submodule
   matches `external/quadwild-bimdf`; an overlay port matches `ci/vcpkg-ports/`. The
   library is header-only, which argues for the submodule and a plain
   `target_include_directories`.
6. **Directional's missing `LICENSE` file.** The per-file MPL-2.0 banners are clear and
   sufficient, but `provenance.license` in a descriptor should not be asserted from a
   comment alone. Worth an upstream issue before we depend on it.
7. **Bump to C++20, or patch three headers?** `std::numbers` is the only C++20 usage
   found. Bumping `CMAKE_CXX_STANDARD` is a whole-project decision with its own
   consequences; a patch is three lines but is a patch we then own.
8. **Is the QuadWild trade-off acceptable?** Passing a field costs the adaptive remesh
   and sharp-feature detection. If not, the alternative is patching upstream to
   separate field computation from preprocessing — a fork of a GPL-3.0 submodule we
   currently keep unmodified on purpose.
