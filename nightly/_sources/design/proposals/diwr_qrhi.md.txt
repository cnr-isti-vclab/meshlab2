# DiWR through QRhi compute — feasibility assessment

Assessed 2026-11-15. Proposal only; no implementation or migration is approved
by this document. DiWR snapshot: `2397689443016bfb8e1c73f26f146cdee83780c2`.

## Verdict and scope

Feasible, with substantial engineering work. The floating-point math is mostly
straightforward to express in compute shaders. The difficult parts are the GPU
memory representation, scattered gradient accumulation, repeated spatial queries,
and preserving numerical behavior across backends. QRhi provides dispatch and
resources, not equivalents of CUDA managed memory, Thrust, or cudaKDTree.

For one experienced C++/GPU engineer, estimate roughly one week for a targeted
feasibility experiment, 4–6 weeks total for an end-to-end prototype on one backend,
and 10–16 weeks total for a validated Metal/Vulkan/Direct3D MeshLab integration.
These are planning judgments, not measured implementation times. They assume
access to an NVIDIA reference machine, representative inputs, and the authors'
intended parameters. Near-CUDA performance on all devices is a separate,
unbounded optimization objective until profiling exists.

DiWR is interesting independently of existing reconstructors: it jointly adjusts
orientation, area weight and confidence for unoriented, unevenly sampled, noisy
point clouds with outliers. The paper's final extraction uses weighted Screened
Poisson, which MeshLab already contains. This is a potential reuse point, not a
replacement of DiWR's optimization with Poisson.

## Measured source inventory

Counts include comments, alternate paths and diagnostics; they are not estimates
of the required port size. Kernel counts exclude bundled cudaKDTree headers.

| Source | Lines | `__global__` definitions | Work |
|---|---:|---:|---|
| `src/TurboDWG.cu` | 4,442 | 28 | Iteration control, orientation updates, sampling, regularizers, reductions and I/O |
| `src/TurboLibiGWN.cu` | 3,394 | 16 | Octree winding evaluation, spatial derivatives and parameter derivatives |
| `src/KNN_GWN.cu` | 636 | 1 | Wrappers around templated cudaKDTree construction and queries |
| `src/TurboMC.cu` | 531 | 1 | Marching cubes, lookup tables and output allocation |
| `src/TurboNormalInit.cu` | 44 | 1 | Normal initialization |
| `src/RMSProp.cu` | 30 | 1 | Per-point optimizer update |
| **Total** | **9,077** | **48** | |

The six files contain 219 `cudaMallocManaged` call sites, 70 explicit
`cudaDeviceSynchronize` calls and 47 `atomicAdd` call sites. This is static source
counting, not runtime frequency. The inspected code has no cuBLAS/cuSOLVER or
neural-network runtime dependency. Its core device calculations use float/float3;
CPU preparation also uses Eigen doubles. The main device sources use ordinary
thread indexing, atomics and block barriers rather than warp-shuffle machinery.

## What must change

* **Memory layout: high effort.** `OctreeNodeLibiGPU` holds pointers to children,
  point indices, six derivative arrays and query queues. Flatten these into a
  small number of storage-buffer arenas addressed by offsets. Preserve the
  existing iterative parent/child traversal first. CUDA float3 host layouts must
  not be copied blindly into std430 vec3 arrays; use explicit packed scalars or
  padded records. Managed pointers are also read and written directly by CPU
  loops, so uploads, readbacks and CPU/GPU ownership must become explicit.
* **Backward accumulation: highest performance risk.** Multiple queries scatter
  floating-point contributions into the same area/confidence gradients. Normal
  updates also scatter into points. Native floating-point atomic addition is not
  a portable baseline across QRhi's shader targets. Integer compare-and-swap on
  float bit patterns is a candidate for initial correctness, but contention may
  require staged partial sums or gather/segmented reduction. Some per-element
  atomic operations can become ordinary writes where unique ownership is proven.
* **Thrust and spatial queries: medium/high effort.** Reductions, dot products,
  sorting and CUDA kd-tree construction have no QRhi library equivalent. Keep
  one-time preparation and infrequent sorting on CPU initially; use hierarchical
  compute reductions for the hot loop. Reuse vcglib's CPU kd-tree where practical,
  but recurring GPU queries need a shader implementation or measured CPU transfers.
* **Marching cubes: medium effort.** It is used inside the orientation loop to
  generate triangle centers/normals, not only for final output. CPU extraction is
  possible for an initial prototype but introduces repeated grid readbacks. For
  GPU extraction, replace its 64-bit atomic append counter with bounded 32-bit
  batches or count/scan/emit. Capacity checking must be explicit.
* **Scheduling: medium effort.** Replace blanket CPU synchronization with
  dependent compute passes and occasional scalar readbacks. QRhi inserts resource
  barriers at pass boundaries; a workgroup barrier is not a global dependency
  between dispatches. Keep intermediate arrays resident and remove diagnostic
  file output from the iteration path.
* **Numerics: substantial validation.** Different reduction order, atomics,
  fused operations and CUDA's `--use_fast_math` can alter convergence. Compare
  fields, gradients and optimizer steps before evaluating only the final mesh.

Memory needs attention beyond the point arrays. `AllocateQueryMem` allocates
`max(query_count, point_count)` integer slots for each node with at least 1,000
points; it also reserves 100 million integers in a temporary CPU vector. A cubic
260³ grid alone requires about 268 MiB for float3 positions and float field
values, before extraction buffers, octrees, optimizer state and CPU copies.
Deriving grid positions from indices and processing queries in bounded batches
are promising reductions. These are analytical sizes, not measured peak usage.

## MeshLab integration and reuse

The current tree has no implemented `QRhiComputePipeline` or `beginComputePass`
call. Rendering infrastructure exists, but a compute filter execution path does
not. The Gaussian-splatting design discusses compute as proposed work.

Use a small compute context with an explicit QRhi lifecycle, independent of a
visible RenderWidget, so the filter can eventually serve GUI and Python use.
Resource ownership must respect QRhi thread affinity. Gate execution on Compute,
storage-buffer readback and device limits; report a meaningful refusal on
unsupported devices. OpenGL below 4.3 cannot provide this path, including macOS's
OpenGL backend; Metal is the intended macOS backend. Headless setup needs its own
validation rather than assuming an existing window or offscreen widget suffices.

MeshLab requires Qt >= 6.11. Its current shader batch targets GLSL 150. Compute
shaders need a separate shader batch with suitable targets, such as GLSL 430,
HLSL 50 and MSL, retaining SPIR-V for Vulkan. Shader resources must be linked into
the consumers of the filter, including Python, not solely the MeshLab executable.

PCL is used for initial normal estimation; vcglib has point-cloud normals and
kd-tree implementations that may remove that dependency after numerical checks.
Eigen, libigl and CGAL already have integration precedents. However, DiWR calls a
custom `igl::octreeDWG` present in its bundled libigl: substituting stock libigl
without accounting for that change will not work. Preserve algorithm provenance
and avoid introducing a second unrestricted copy of libigl's headers.

MeshLab's Screened Poisson adapter already normalizes the normal and scales it
by vertex quality when confidence is enabled
(`plugins/filter_screened_poisson/poissonrecon_adapter.cpp`). The paper describes
effective weights a*c, but DiWR's exported PLY min–max rescales a*c and adds 0.001.
Confirm the intended reconstruction convention before connecting those paths.
CPU geometry additions belong in vcglib where general-purpose; QRhi resources
and dispatch belong on the MeshLab side. A provenance-preserving upstream copy
is another option for code that must remain comparable to DiWR.

The top-level license is MIT. Bundled components carry separate notices, including
Apache-2.0 in cudaKDTree and MPL-2.0 in inspected libigl files; preserve the relevant
notices and inventory the actual retained dependency subset before shipping.

## Establish a reference before porting

The executable defaults to `com_As = 1`, which dispatches
`TurboScreenedDWGLaunch`, not the joint area/confidence optimization. Values 4/5
dispatch `TurboScreenedDWGOptLaunch`, and value 5 propagates the optimized weights
through a later output path. Do not treat an unconfigured run as a DiWR reference.
The README does not provide a reproduction recipe or shipped benchmark models.

Obtain an intended command line, small representative data and reference outputs
from the authors/maintainer. Confirm the normalization, weight export and final
Poisson settings rather than inferring them from branches and constants. Freeze
that configuration while comparing the port. The paper reviewed here is arXiv
v1; correspondence with the current released code must also be established.

## Recommended first experiment

Budget approximately one week for a narrow decision point:

1. Establish CUDA outputs for a fixed small case: winding values, spatial
   gradients, area/confidence gradients and one RMSProp step.
2. Flatten the octree and port the principal forward and backward paths. Use
   integer-CAS float accumulation initially; do not port every alternative mode.
3. Run on Metal and a second backend. Check finite-difference gradients on small
   cases, numerical agreement with CUDA, contention and peak allocations.
4. Decide whether simple CAS is adequate or accumulation needs redesign. Then
   commit to the full orientation loop, extraction, integration and validation.

The lower-code initial route keeps CPU preparation, selected spatial operations
and final Screened Poisson, with the costly field/gradient work on QRhi. It still
implements DiWR's orientation loop. Moving all marching cubes and kd-tree work
to the GPU immediately would enlarge the project before the central risk is known.

Validation should separate descriptor conformance, graceful refusal without
compute, and numerical behavior. Shader behavior requires a real GPU harness;
existing CPU ctests alone cannot validate it. Compare final reconstruction
distance, normal consistency, connected components and boundary/nonmanifold
edges on clean, nonuniform, noisy and outlier-heavy inputs. Use optimized builds
for performance comparisons and record hardware/backend/parameters.

## Checks performed for this assessment

Read the pinned public source, paper and local MeshLab implementation; did not
build or run CUDA and did not benchmark the algorithm. Source was downloaded to
scratch without changing Git state. A tiny integer-CAS float accumulation shader
was successfully baked with local qsb 6.12.0 into SPIR-V 100, GLSL 430, HLSL 50 and
MSL 12. A plain float `atomicAdd` shader failed without
`GL_EXT_shader_atomic_float`, as expected. This checks shader translation only,
not native driver compilation, numerical correctness or runtime performance.

Sources:

* [Pinned DiWR source](https://github.com/LaoChui999/DiWR/tree/2397689443016bfb8e1c73f26f146cdee83780c2)
* [Paper, arXiv v1](https://arxiv.org/html/2602.13801v1)
* [QRhi features, limits, synchronization and threading](https://doc.qt.io/qt-6/qrhi.html)
* [Qt shader baker](https://doc.qt.io/qt-6/qtshadertools-qsb.html)
