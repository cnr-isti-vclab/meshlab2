# Gaussian Splatting

This document evaluates adding 3D Gaussian splat loading, rendering and editing
to MeshLab. **Nothing described here is implemented.** It records what the
technique demands, which of MeshLab's mechanisms already carry weight for it,
which do not, and the design decisions already taken.

See also: [Rendering](../rendering.md) (the QRhi pipeline this would extend),
[Data Model](../data_model.md) (layers, revisions, undo), [Architecture](../architecture.md),
[Adding a Filter](../adding_a_filter.md).

## Status

As of 2026-09-10: nothing implemented, nothing scheduled. Three decisions have
been taken in advance and the rest of this document is written around them —
see [Decisions taken](#decisions-taken).

2026-11-04: Open3D 0.20 shipped a splat renderer and splat-aware transforms under the
MIT licence. They are not usable through MeshLab's Open3D plugin, but they are worth
lifting, and they change the renderer plan and decision 1 — see
[Open3D 0.20](#open3d-020-2026-09).

## What a splat is, and why it is awkward

A 3D Gaussian splat scene is a point cloud where every point carries an
ellipsoidal footprint and a view-dependent colour:

| Per splat | Floats | Notes |
|---|---|---|
| Centre | 3 | An ordinary vertex position. |
| Opacity | 1 | Stored logit-encoded in the standard PLY. |
| Scale | 3 | Log-encoded; the ellipsoid's semi-axes. |
| Rotation | 4 | Quaternion. With scale it forms the 3×3 covariance. |
| Spherical harmonics | 3 to 48 | Degree 0 is a flat colour (3). Degree 3 is 16 coefficients × 3 channels (48), and is what makes a scene look right. |

At degree 3 that is roughly **240 bytes per splat**, so a one-million-splat
scene is ~240 MB of attributes on top of the geometry, and real captures run to
several million.

Two properties make this different from anything MeshLab renders today:

- **Rendering is order-dependent.** Splats are transparent and blended, so they
  must be drawn back-to-front in view order. The ordering changes whenever the
  camera moves, over millions of primitives, every frame.
- **The attributes are geometric.** Scale and rotation define an ellipsoid in
  world space and the SH coefficients are expressed in a world-space basis, so
  any transform of the cloud has to transform them too. They are not decoration
  that can ride along unchanged.

## The substrate that already exists

| Asset | Where | Why it matters |
|---|---|---|
| QRhi renderer over `QRhiWidget` | `src/render/` (~20k lines) | Cross-backend already: Metal on macOS, D3D/Vulkan/GL elsewhere. Shaders are GLSL cross-compiled to `.qsb`. |
| Frame-plan / pass architecture | `renderwidget_frame_plan.cpp`, `renderwidget_scene_passes.cpp` | New pass kinds slot into `collectRenderFramePassRequests()` and `buildRenderFramePlan()` rather than into one monolith. |
| Point topology and per-point pipelines | `QRhiGraphicsPipeline::Points`, `overlay_points.*` | Point clouds already render; splats are a different pipeline, not a different data path. |
| Render-mode plumbing | `renderwidget_modes.cpp`, `renderingsettings.h`, `rendersettingsjson.cpp` | An exclusive splat mode is an addition to an existing enum plus settings (remember the field-count constant). |
| vcglib in tree | `vcglib/` submodule | Attribute machinery, kd-trees, point-cloud algorithms, PLY reader with custom-property support. |
| Filter descriptor system | `plugins/*/filters.json` | The splat filters need no new UI work, only descriptors. |
| Undo geometry interning | `document_undo_state.cpp`, `deepCopyMesh()` | Snapshots share immutable `VCGMesh` copies keyed on revision — but see the gap below. |
| A student's loader | [adettori/vcglib `gaussian`](https://github.com/adettori/vcglib/tree/gaussian) | Most of the import/export work is already written. |

## The vcglib `gaussian` branch

40 commits from Sept–Oct 2024. Comparing against upstream, **all the splat code
is in `apps/sample/trimesh_gaussian/` — sample code, not library headers**:

| File | Lines | What |
|---|---|---|
| `gaussian_splat.h` | 280 | The splat type: colour + opacity, quaternion, scale, three `SphericalHarmonics`. |
| `import_ply_GS.h` / `export_ply_GS.h` | 155 / 159 | The standard 3DGS PLY, via `PlyInfo::AddPerVertexFloatAttribute`. |
| `import_splat.h` / `export_splat.h` | 80 / 72 | The `.splat` format. |
| `import_ksplat.h` | 335 | `.ksplat`, including level-2 compression. |
| `trimesh_gaussian_convert` | 320 | A mesh→splat converter using PCA. Useful in its own right, and the cheapest way to see splat data before a renderer exists. |

Two frictions:

- **`DegreeSH` is a compile-time template parameter.** The splat type differs
  per degree, so a loader must be instantiated for each of 0–3 and dispatched at
  runtime, or the representation reworked to carry the degree as data.
- **The branch's `vcg/` diffs are 2024-era divergence**, not splat work. The
  submodule is pinned much later. Lift the six headers; do not merge the branch.

## Prior art

| Viewer | Stack | Licence | Reusable? |
|---|---|---|---|
| [3DGS.cpp](https://github.com/shg8/3DGS.cpp) | C++ / Vulkan compute, macOS via MoltenVK | LGPL | Reference for the GPU sort. Standalone app with its own swapchain — not embeddable. |
| [vkgs](https://github.com/jaesung-cs/vkgs) | C++ / Vulkan | — | Same shape. |
| [antimatter15/splat](https://github.com/antimatter15/splat) | WebGL | MIT | The canonical minimal implementation. The covariance-projection maths and the sort strategy port directly. |
| [Open3D 0.20](https://github.com/isl-org/Open3D/tree/v0.20.0/cpp/open3d/visualization/rendering/gaussian_splat) | C++ / GLSL compute, on Vulkan and Metal | MIT | **Yes, shaders and maths** — see [Open3D 0.20](#open3d-020-2026-09). |

Nothing here drops into a QRhi renderer. The work is reimplementing roughly 600
lines of well-documented technique, not inventing anything. MeshLab is GPL-3.0,
so LGPL reference code is licence-compatible if any of it were ever lifted
verbatim, but it would not be.

## Open3D 0.20 (2026-09)

Open3D v0.20.0 (2026-09-16, the release `plugins/filter_open3d` vendors) added splat
reading (Gaussian PLY, `.splat`, SPZ) into tensor point clouds, splat-aware transforms,
and a compute renderer composited with its meshes. Everything below was read from the
v0.20.0 tree on 2026-11-04, and the shader results were measured with this project's
`qsb` (Qt 6.11.2).

**Not through the Open3D plugin.** The plugin vendors 116 files, the closure of the legacy
`geometry` and `pipelines/registration` code; all of the splat work is in the excluded
tensor API (`cpp/open3d/t/`) and `cpp/open3d/visualization/`. The renderer is bound to
Filament: Open3D creates the Vulkan device and shares it with Filament (Metal on macOS),
which does not fit a `QRhiWidget`. Widening the vendored subset to take the tensor core
(173 files) for this is not worth it. What follows is source to lift, with the MIT notice
kept.

| Open3D piece | Here | Reuse |
|---|---|---|
| Six GLSL compute shaders, about 1,400 lines, and `GaussianSplatDesign.md` (37 KB) in `cpp/open3d/visualization/rendering/gaussian_splat/` | Stage 2, the renderer | High, with the adaptation below |
| Ivanic–Ruedenberg rotation of real spherical harmonics, `BuildIrR1`/`BuildIrRl` at the top of `t/geometry/PointCloud.cpp` | The splat-aware rotation | About 85 lines of plain Eigen. General maths, so it goes into vcglib beside `vcg/math/spherical_harmonics.h` |
| `Rotate`/`Scale`/`Translate` semantics for splat clouds | Same | Rotate composes the quaternion (`q' = q_R q`) and rotates `f_rest`; a uniform scale multiplies the linear scales, and a negative one negates the odd SH bands; a general 4×4 `Transform` warns and leaves the splat attributes alone. That settles the shear and non-uniform scale question: refuse |
| Per-splat GPU layout | Buffer sizing | 36–84 B per splat, against the ~240 B assumed above: positions fp32, scales and colour+opacity fp16, quaternion snorm8, SH fp16, sigmoid applied once on the CPU |
| Depth-aware composite | Decision 1 | The composite reads the scene depth and skips splats behind meshes, so the exclusive mode is not needed even at first |
| SPZ reader (`t/io/file_format/FileSPZ.cpp`, over Niantic's library) | A format the vcglib branch lacks | The library is in vcpkg: port `spz` 2.1.0 in the pinned baseline, MIT, zlib its only dependency. Open3D builds 3.0.0; whether 2.1 reads the newest files is not checked |

**The renderer differs from the [Rendering](#rendering) plan** in one structural way, for
the better: it is tile-based and draws nothing. `gaussian_project.comp` projects and culls,
writing one 32-bit key per (splat, 16×16 tile) — tile index high, normalised linear depth
low, the split sized to the tile count; a four-pass radix sort (a histogram and a scatter
shader per 8-bit digit) orders them;
`gaussian_composite.comp` gives each tile a workgroup that blends front to back into an
RGBA16F storage image, stopping at T < 1/255. No instanced quads, and occlusion against
meshes is a depth-texture read in the same pass.

**Measured against QRhi**, compiling each shader unmodified with `qsb` at the targets
MeshLab already uses (GLSL 430, HLSL 5.0, MSL 1.2):

| Shader | Lines | Compiles | Why not |
|---|---|---|---|
| `gaussian_radix_sort_histograms.comp` | 66 | yes | |
| `gaussian_compute_dispatch_args.comp` | 142 | yes | but see indirect dispatch |
| `gaussian_depth_merge.comp` | 72 | yes | |
| `gaussian_project.comp` | 527 | no | subgroup operations (4 call sites) |
| `gaussian_composite.comp` | 429 | no | subgroup operations (2) |
| `gaussian_radix_sort_scatter.comp` | 163 | no | subgroup operations (3) |

- **Subgroups.** `qsb` emits SPIR-V 1.0 and subgroup operations need 1.3 ("subgroup op
  requires SPIR-V 1.3"); raising the HLSL and MSL targets does not change that. The calls
  batch atomic counters and compute prefix sums, and each has a shared-memory workgroup
  equivalent: slower, and portable to every QRhi backend including D3D11.
- **Indirect dispatch.** `QRhiCommandBuffer` has only `dispatch(x, y, z)`, so the counts
  that `gaussian_compute_dispatch_args.comp` writes on the GPU have to come back another
  way: a one-frame-late readback, or an upper-bound dispatch whose surplus workgroups exit.
- No push constants are used, which QRhi lacks anyway.

**Its own limits:** the shader evaluates SH to degree 2 only, rotating degree 3 without
rendering it; MSAA is off for splat views, the Gaussian low-pass and density compensation
serving instead; the release notes list hangs on some GPUs (Intel Arc A770m); and its own
to-do list says the subgroup radix scatter is untested at scale on Intel. Splats too large
on screen (more than 256 tiles by default) are culled, with a flag the CPU reads back.

**Unchanged:** the [prerequisite](#prerequisite-named-attributes-in-the-undo-pipeline)
still blocks everything, and decision 3 stands; Open3D's attributes (`f_dc`, `f_rest`,
`opacity`, `scale`, `rot`) map onto vcg per-vertex attributes one to one. The vcglib
`gaussian` branch is still the cheaper route for PLY and `.splat`.

## Decisions taken

1. **A separate render mode, initially exclusive.** When a splat layer is shown,
   ordinary mesh layers are not. This sidesteps the hardest visual problem —
   blending unsorted transparent splats against opaque geometry — and can be
   relaxed later. *To revisit:* Open3D's composite reads the scene depth and skips
   occluded splats, which solves that problem in the same pass, so the exclusivity
   may not be needed at all.
2. **Sorting on the GPU via `QRhiComputePipeline` from the start.** No CPU-sort
   stepping stone: a CPU sort that is adequate at 500k splats collapses at 5M,
   and the GPU path reshapes the buffer layout, so building the CPU one first
   means building the resource layer twice.
3. **Splat parameters live in vcg named per-vertex attributes**, and the undo
   pipeline is taught to carry them. Not `LayerData`.

### Why not LayerData

`LayerData` was designed for layer-scoped, plugin-owned blobs — an abstract
domain, a solver state — that are *about* a layer without being part of its
geometry. Splat parameters are the opposite: they are strictly per-vertex and
must follow vertices through deletion, compaction and reordering. Outlier
removal and selection-based deletion do exactly that, and vcg's attribute
machinery already handles it (`PointerToAttribute::Reorder`), while `LayerData`
by design does not — it is dropped wholesale when geometry changes.

## Prerequisite: named attributes in the undo pipeline

This is the one piece of core work that must land before anything else, and it
is worth doing on its own merits.

`deepCopyMesh()` at [`src/core/document_internal.cpp:526`](../../../src/core/document_internal.cpp)
copies a hand-listed set of fields — coordinates, normals, colour, quality,
flags, OCF texcoords, curvature directions, wedge texcoords. It carries **no vcg
named attributes**, so today an attribute-bearing mesh would lose its attributes
on the first undo checkpoint, silently.

The machinery to fix it generically is already in vcglib:

- `PointerToAttribute` carries `std::type_index _type` alongside the name, so an
  attribute's type is known at runtime.
- `SimpleTempDataBase::CopyValue(to, from, other)` is virtual, so values can be
  copied without knowing the concrete type — provided the destination attribute
  already exists with the same name and type. This is exactly how
  `tri::Append` handles attributes ("only those present in both meshes").

So the change is a small registry mapping `std::type_index` to a factory that
creates that attribute on the destination mesh, plus a loop in `deepCopyMesh()`.
MeshLab would register the handful of types it means to support (`float`,
`Point3f`, the splat struct) rather than trying to be universal. Roughly 40–100
lines plus tests, and it also has to be threaded through memory accounting
(`document_memory.cpp`) and the layer panel's size reporting.

## Rendering

The standard structure is three passes per frame, two of them compute:

1. **Preprocess and cull** (compute). Transform each splat to view space, project
   the 3×3 covariance to a 2D screen-space conic, discard splats outside the
   frustum or below a size or opacity threshold, and append the survivors to a
   compacted list with their depth key.
2. **Sort** (compute). A 4-pass least-significant-digit radix sort over 8-bit
   digits of a 32-bit quantized-depth key, payload being the splat index.
   Histogram, prefix scan, scatter per pass. The output is an index buffer.
3. **Draw**. One instanced screen-aligned quad per surviving splat, in sorted
   order; evaluate the Gaussian in the fragment shader; premultiplied alpha
   blending, depth test on, **depth write off**.

Points to settle while building it:

- **Compute availability.** No `QRhiComputePipeline` exists anywhere in the
  codebase today, so this is new ground here. Guard on
  `rhi->isFeatureSupported(QRhi::Compute)`: Metal, Vulkan, D3D11/12 and
  OpenGL 4.3 / ES 3.1 support it; older GL does not. Decide what a machine
  without compute is told — most likely that splat layers cannot be displayed.
- **Re-sort policy.** Even a GPU sort is worth skipping. Re-sort when the view
  direction has rotated past a threshold, not every frame.
- **Layer transforms.** A layer transform carrying a rotation must rotate the
  covariances *and* the SH basis. Either bake transforms into the splat data
  when they are applied, or carry the rotation into the shader. This is a
  rendering decision, not only a filter one, and it should be taken early
  because it interacts with the compute preprocess pass.
- **SH degree.** Degree 0 gives a picture and a small vertex buffer; degree 3 is
  what makes it look right and costs 48 floats per splat. Ship 0 first, but size
  the buffers for 3.

## Filters wanted

| Filter | Difficulty | The hard part |
|---|---|---|
| **Rotate (splat-aware)** | High | Composing the per-splat quaternion is trivial. Rotating the **spherical harmonics** is not: SH coefficients live in a world-space basis and rotating them means applying a Wigner-D rotation per band — block-diagonal, 1+3+5+7 for degree 3. vcglib already has `vcg/math/spherical_harmonics.h`, which the student's branch extended. A non-uniform scale or shear breaks the model outright and would need the covariance re-decomposed (polar or eigen) back into scale and rotation, or the operation refused. Open3D 0.20 has the SH rotation (Ivanic–Ruedenberg) and refuses the general case; both lift, see [Open3D 0.20](#open3d-020-2026-09). |
| **Outlier removal** | Low | Ordinary point-cloud work on the splat centres — vcglib has the kd-tree and the cleaning machinery. The only real requirement is that vertex deletion carries the attributes, which vcg's `Reorder` already does once the undo prerequisite is in. Worth also offering opacity- and size-based culling, which are cheaper and often more effective on splat data than geometric outlier tests. |
| **Select by splat characteristics** | Low–medium | Anisotropy (ratio of scale components), planarity, isolation (k-NN distance), opacity, footprint size. The minimal-code route is a filter that writes the chosen statistic into **per-vertex quality** and then reuses the existing quality-based selection, histogram and colorization filters, rather than building a bespoke selection UI. That also makes the statistic visible before it is used as a threshold. |

## Staging

0. **Named attributes in the undo pipeline.** Core work, no splat content,
   independently useful. Blocks everything else.
1. **Loading and inspection.** PLY and `.splat` readers over the student's
   headers; splats as per-vertex attributes; no renderer. Verified by round-trip
   and by the mesh↔splat converter, which makes the data visible as ellipsoids
   with zero renderer work.
2. **Compute-sorted renderer.** Exclusive splat mode, SH degree 0, the three-pass
   structure above.
3. **Production.** SH degree 3, the three filters, `.ksplat` and compressed
   formats, culling and LOD, and possibly relaxing the exclusive mode.

Rough effort, assuming the prerequisite is done: **3–5 days** for stage 1,
**1–2 weeks** for stage 2, **2–4 weeks** for stage 3.

Revised 2026-11-04, with Open3D 0.20 as a source:

- **Stage 1** also reads SPZ, through the vcpkg `spz` port.
- **Stage 2** becomes a port of Open3D's tile-based pipeline rather than the instanced-quad
  design above: three shaders as they are, three with their subgroup operations rewritten
  over shared memory, indirect dispatch replaced, and the QRhi resource and pass plumbing,
  which is the real work. Composited against the scene depth, not exclusive. About a week,
  with most of the design and debugging risk gone.
- **Stage 3** takes the Ivanic–Ruedenberg rotation into vcglib for the splat-aware Rotate,
  and adds the degree-3 SH evaluation that Open3D's shader leaves out.

## Open questions

- Is a splat layer a distinct layer kind, or a point cloud that happens to carry
  attributes? The latter is less code and lets existing point-cloud filters work
  unmodified; the former makes it harder to apply a filter that would silently
  invalidate the splats.
- Transform baking versus shader-side transform (see above).
- What the PLY writer guarantees on round-trip, and whether MeshLab should
  write the compressed formats at all or only read them.
- Whether SH degree should be a load-time choice (drop to degree 0 on load to
  save memory) or always preserved.

## Non-goals

- Training or optimizing splats. MeshLab views and edits; it does not fit.
- Any CUDA dependency. The renderer is QRhi and stays that way.
- Mixing splat and mesh layers in one view, initially.
- Reimplementing a third-party viewer inside MeshLab rather than using its
  technique.
