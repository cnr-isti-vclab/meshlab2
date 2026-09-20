# Geogram

This document plans a `filter_geogram` plugin bringing [geogram](https://github.com/BrunoLevy/geogram)'s
boolean, parametrization and remeshing algorithms into MeshLab. **Nothing described
here is implemented.**

See also: [Adding a Filter](../adding_a_filter.md), [Filter Organization](../filter_organization.md)
(the one-dependency-one-plugin rule this follows), [Vocabulary](../vocabulary.md)
(which fixes every name below), [Data Model](../data_model.md).

## Status

As of 2026-09-19: nothing implemented, and **ready to start**. The plan is complete and
every open ruling has been taken — see *Decisions taken* at the end. Phase 0 is
unblocked.

Every signature quoted here was re-read from tag **v1.9.3**, which is what the vcpkg
port pins, rather than from `main`. Two claims in the first draft did not survive that
reading and are corrected in place, marked **⚠** where they appear: spectral
parametrization needs a runtime library we do not ship, and geogram has no symmetric
difference. A third correction is structural — the parametrization entry points live in
`geogram/parameterization/`, not `geogram/mesh/`.

## Why geogram, given libigl, CGAL and TrueForm are already here

Shipping several backends for one algorithm is deliberate, so overlap is not an
objection — but the case is stronger than overlap. Geogram brings algorithms that
have **no route at all** in the current registry:

| Capability | Today | Geogram adds |
|---|---|---|
| ABF++ angle-based flattening | — | `mesh_compute_ABF_plus_plus` — the reference implementation, by its own author |
| ~~Spectral conformal parametrization~~ | — | `mesh_compute_LSCM(spectral=true)` (Mullen et al. 2008) — **not shipped**: needs ARPACK at run time, see decision 2 |
| Segment → flatten → pack in one call | — | `mesh_make_atlas`, a complete atlas pipeline with pluggable parametrizer and packer |
| Chart segmentation as a first-class step | — | `mesh_segment`, six segmenters (VSA, spectral, inertia) |
| CVT/Lloyd-Newton isotropic **and anisotropic** remeshing | vcglib and TrueForm do isotropic only | `remesh_smooth`, with a genuine anisotropic mode |
| Boolean with exact predicates | libigl (through CGAL, GPL) · TrueForm | a **third** implementation, BSD-licensed — **⚠ no symmetric difference** |
| OpenSCAD-subset CSG scripting | — | `CSGCompiler::compile_string` — primitives, extrusion, `hull`, n-ary ops driven from a script |
| Tetris chart packing | — | `pack_atlas_using_tetris_packer` (the original LSCM paper's packer) |

Two more reasons specific to this codebase:

- **Licensing is easier than the incumbents.** Geogram is BSD-3-Clause. The libigl
  boolean route goes through `igl_copyleft::cgal` and CGAL's GPL; a BSD backend for
  the same operations is a strictly freer path for downstream users.
- **Lineage.** LSCM, ABF++ and the CVT remesher are Lévy's own; `filter_isoparam`
  and the Voronoi atlas in `filter_texture` already descend from that line of work.

## Dependency shape

Geogram is a **built library**, not header-only — the first such geometry backend
here, and the integration constraints follow from that.

### The port forces dynamic linkage — a macOS-only problem

From `vcpkg/ports/geogram/portfile.cmake`:

```
if (VCPKG_TARGET_IS_OSX)
    message("geogram on Darwin only supports dynamic library linkage. Building dynamic.")
    set(VCPKG_LIBRARY_LINKAGE dynamic)
```

The port overrides whatever the triplet asked for. What that costs depends entirely
on the platform:

| | Linkage today | Consequence |
|---|---|---|
| **Windows** | `ci/vcpkg-triplets/x64-windows-release.cmake` already sets `VCPKG_LIBRARY_LINKAGE dynamic` | **None.** `windows-portable.yml` already copies `vcpkg_installed\…\bin\*.dll` into the portable directory |
| **Linux** | — | **None today.** CI is `docs.yml`, `macos-dmg.yml`, `windows-portable.yml`; there is no Linux job to break |
| **macOS** | `ci/vcpkg-triplets/arm64-osx.cmake` sets `VCPKG_LIBRARY_LINKAGE static`, and the installed tree is currently **0 dylibs against 43 static libs** | Geogram becomes the first and only dynamic dependency |

So the work is confined to the macOS bundle, and it is three things:

- **Bundling.** `macdeployqt` only chases Qt's own libraries, so `libgeogram.dylib`
  needs the hand-rolled treatment `scripts/package-macos-dmg.sh` already gives
  `libomp.dylib` at lines 75–87: copy into `Contents/Frameworks`,
  `install_name_tool -id @rpath/…`, then `-change` on the main binary. Run `otool -L`
  on the built dylib first — on macOS its BLAS/LAPACK resolves to the Accelerate
  system framework so the transitive chain should be empty, but the port unbundles
  and re-vendors amgcl, libMeshb and rply, so confirm rather than assume.
- **Signing.** Every Mach-O inside the bundle must be signed before the app is sealed
  or notarization rejects it. The packager already carries two loops for exactly this
  — the Python `lib-dynload` extensions and `Contents/Helpers` — and geogram is a
  third instance of the same pattern.
- **Deployment target stops being cosmetic.** This is the sharp one. A wrong minimum
  OS is only a linker warning for a static archive, and there are live examples in the
  tree: `libgmp.a`'s C objects carry `minos 15.0` while its arm64 assembly objects
  carry `minos 26.0`, which is where the `built for newer 'macOS' version (26.0)`
  warnings in every link come from. Harmless there — the assembly does not care what
  OS it runs on. For a **dylib**, `minos` is a single value for the whole image and
  dyld enforces it: built at 26.0, the app refuses to launch on macOS 15 with a dyld
  error — precisely the failure `arm64-osx.cmake`'s own comment exists to prevent.
  Geogram is a plain CMake build so the triplet should reach it properly; it is simply
  the first artifact where getting this wrong is fatal rather than noisy. One
  `otool -l` check in Phase 0 settles it.

What dynamic linkage does **not** affect: the Python bindings
(`_meshlab2_bindings` and `MeshLab2PythonHost` are both `STATIC` and linked into the
app, so there is no second consumer needing the dylib), and licensing — BSD-3 imposes
nothing either way.

### The static alternative

The upstream port asserts dynamic-only on Darwin without saying why, and
`VORPALINE_BUILD_DYNAMIC` suggests static may be untested rather than impossible. A
static overlay port would delete every item above.

The plumbing is cheap because it mirrors something that already exists:
`ci/vcpkg-triplets/` is passed through `VCPKG_OVERLAY_TRIPLETS` in both
`CMakePresets.json` and the Windows workflow, so `ci/vcpkg-ports/geogram/` plus
`VCPKG_OVERLAY_PORTS` follows the identical path. It must be tracked in-tree —
`vcpkg/` itself is gitignored, so the port cannot be patched in place.

Roughly a wash on effort: accepting the dylib is about twenty lines in the packager
plus a signing call, all following patterns already debugged; an overlay port means
owning a patched build of someone else's library indefinitely.

**Decided: bundle the dylib** (decision 1). The overlay port is not attempted. Phase 0
therefore does not experiment with linkage — but it still reads `minos` off the built
dylib, and for a bundled dynamic library that check is the sharp one described above,
not a formality.

### Other dependency facts

- `blas` + `lapack`. On macOS vcpkg's `blas` metapackage pulls **nothing** — it maps
  to the system Accelerate framework (`"platform": "!osx & !ios"` on the openblas
  dependency). On Linux and Windows it drags in OpenBLAS.
- The port builds with `GEOGRAM_LIB_ONLY=ON`, `GEOGRAM_WITH_VORPALINE=OFF`, and the
  `graphics` feature off by default — so no GLFW, no viewers. Request it as
  `"geogram"` with default features in `vcpkg.json`.
- Exported target comes from the port's own `Config.cmake.in`; confirm the name in
  Phase 0 and gate on it the way `filter_igl` gates on its candidate target list.

### ⚠ ARPACK is a runtime dependency — why the spectral paths are not shipped

Three of the capabilities above are spectral methods, and geogram reaches its
eigensolver through OpenNL's ARPACK extension. `parameterization/mesh_LSCM.h` says so
in the parameter documentation itself — *spectral mode requires support of the ARPACK
OpenNL extension*.

`NL/nl_arpack.c` calls itself a "weak-coupling adapter": it holds four function
pointers and an `NLdll DLL_handle`, and resolves them by loading a library **by name at
run time** —

```c
#  ifdef NL_OS_APPLE
#      define ARPACK_LIB_NAME "libarpack.dylib"
```

So ARPACK is neither a link-time dependency nor something the port can be asked for:
`ports/geogram/vcpkg.json` depends on `blas`, `lapack` and two host CMake helpers, and
nothing else. A geogram built by vcpkg will call `dlopen("libarpack.dylib")`, find
nothing, and the spectral paths will fail at the point of use.

| Affected | Entry point | Degrades to |
|---|---|---|
| Spectral conformal parametrization | `mesh_compute_LSCM(spectral=true)` | plain LSCM |
| Spectral chart parametrizer in the atlas | `PARAM_SPECTRAL_LSCM` | `PARAM_LSCM` or `PARAM_ABF` |
| Spectral segmenters | `SEGMENT_SPECTRAL_8/20/100` | the two VSA segmenters — *unconfirmed*: they reach ARPACK through manifold harmonics rather than directly, so this was never measured. Omitted with the rest |

Shipping ARPACK ourselves was considered and rejected. vcpkg does carry `arpack-ng`
3.9.1, but satisfying that `dlopen` on macOS needs all three of a Fortran toolchain
(`vcpkg-gfortran`), a **dynamic** build of it — a static archive gives `dlopen` nothing
to open, so `arm64-osx.cmake`'s `VCPKG_LIBRARY_LINKAGE static` would have to be
overridden for this one port — and then bundling and signing it alongside geogram's own
dylib. Geogram's vendored copy at `src/lib/third_party/numerics/ARPACK` is no help: it
is compiled *into* geogram, and a symbol inside `libgeogram.dylib` does not satisfy a
`dlopen` for `libarpack.dylib`.

**Shipped** (decision 2, re-ruled). The first draft's cost estimate was wrong twice
over, and both corrections are worth keeping because each was found by measurement
after the previous conclusion looked solid.

**First correction — Phase 0.** The claim that ARPACK was missing is false. Phase 0
measured the installed tree and found it already there:

- `libgeogram_num_3rdparty.dylib`, which the port installs beside `libgeogram.dylib`,
  **exports `dsaupd_`, `dseupd_`, `dnaupd_` and `dneupd_`** — the four routines
  `nl_arpack.c` looks up, with exactly the trailing-underscore spelling its
  `find_arpack_func` macro asks for. Geogram compiles its vendored
  `third_party/numerics/ARPACK` into that dylib.
- So nothing is missing at all. The only obstacle is the **filename**: geogram
  `dlopen`s the leaf name `libarpack.dylib`, and the file is called something else.
- Measured directly: a copy of that dylib renamed `libarpack.dylib`, placed in a
  directory on the calling executable's `LC_RPATH`, is found by
  `dlopen("libarpack.dylib", RTLD_NOW)` from an unrelated working directory, and all
  four symbols resolve.

**Second correction — implementing it.** The conclusion drawn above, that the fix is a
copy under the name `libarpack.dylib`, is *also* wrong: it is unnecessary. A name shim
was built, and then measured against a run with the shim deleted — which produced
byte-identical results, because `nl_arpack.c` passes `NL_LINK_USE_FALLBACK` and
`nlOpenDLL` already retries the failed `libarpack` open with
`libgeogram_num_3rdparty`. geogram solves its own problem. The shim was removed again.

So the real cost of the spectral paths is **shipping a second dylib nothing links
against**, and knowing why. `scripts/package-macos-dmg.sh` bundles and signs both, and
the top-level `CMakeLists.txt` carries the explanation where the shim used to be.

The lesson worth keeping: `dlopen` succeeding under a name is not evidence that the
caller uses that name. Both wrong conclusions were reached from *correct* measurements
of the wrong thing.

**They work, and both routes are confirmed.** `nlInitExtension("ARPACK")` returns true
in the build tree and inside a bundle relocated to `/tmp`. The spectral segmenters do
reach ARPACK through manifold harmonics and now run — the "unconfirmed" note above is
settled. Every spectral entry point is gated on `GeoAdapter::arpackAvailable()` and
refuses with an actionable message if the probe fails, because geogram's own behaviour
is to log one line and quietly run the non-spectral solver instead: a filter named for a
spectral method must not report success having run something else.

One measurement that did *not* support its intended use: spectral LSCM against plain
LSCM on a sphere cap differs by **0.06 %** (0.00881344 against 0.00881838), because the
cap is symmetric enough that it hardly matters which two vertices plain LSCM pins. A
comparison that close proves nothing and would pass or fail on noise, so the test
asserts the refusal gate instead — the filter succeeding *is* the evidence ARPACK
loaded.

### `GEO::initialize()` must be called with `GEOGRAM_INSTALL_NONE`

Geogram needs one process-wide initialization before any call. Its flags are hostile
to a running Qt application:

```cpp
enum {
    GEOGRAM_INSTALL_NONE     = 0,
    GEOGRAM_INSTALL_HANDLERS = 1,   // installs signal handlers
    GEOGRAM_INSTALL_LOCALE   = 2,   // sets the process locale to POSIX
    GEOGRAM_INSTALL_ERRNO    = 4,
    GEOGRAM_INSTALL_FPE      = 8,   // enables floating-point exceptions
};
```

Signal handlers would fight Qt's crash path; a locale change is global and would
alter number formatting in the UI and in every file writer; enabling FPE traps would
affect unrelated code in the same process — MeshLab already learned what a
process-wide FP setting costs when libigl's config quietly switched CGAL's arithmetic
backend. So: `GEO::initialize(GEO::GEOGRAM_INSTALL_NONE)`, once, behind a
`std::once_flag` in the plugin's constructor or first `runFilter`, and route
`GEO::Logger` into MeshLab's log rather than `stdout`.

## Plugin shape

`plugins/filter_geogram/`, following the established convention that a plugin is a
dependency/build unit named after its dependency:

```
plugins/filter_geogram/
  filters.json
  geogramfilterplugin.h/.cpp      # registration + runFilter dispatch
  geogrammeshadapter.h/.cpp       # VCGMesh <-> GEO::Mesh  (the real work)
  geogrambooleans.h/.cpp
  geogramparametrization.h/.cpp
  geogramremeshing.h/.cpp
  CMakeLists.txt                  # option(MESHLAB2_PLUGIN_FILTER_GEOGRAM ...)
```

The split mirrors `filter_igl` (`iglbooleans`, `iglparametrization`, …), which is the
closest structural analogue: one external library, filters spanning several
categories, one adapter shared by all of them.

`provenance` block for `filters.json`:

```json
"provenance": {
  "project": "geogram",
  "repository": "https://github.com/BrunoLevy/geogram",
  "license": "BSD-3-Clause",
  "integration": "vcpkg:geogram"
}
```

## The adapter is the bulk of the work

`geogrammeshadapter` is to this plugin what `libiglmeshadapter` (306 lines) is to
`filter_igl`, and it is where the schedule actually goes. `GEO::Mesh` is a
container-of-containers (`vertices`, `facets`, `facet_corners`, `edges`) with typed
attributes bound by name, which is a poorer match for `VCGMesh` than libigl's plain
`(V, F)` matrices.

What it must handle:

- **Compaction and deleted elements.** Reuse `filter_igl`'s proven shape: build
  `vertexToSourceIndex` / `faceToSourceIndex` so every filter treats deleted and
  unreferenced vertices identically, and attribute write-back goes through one
  mapping.
- **The layer matrix.** Booleans take two layers, so operands must be brought into a
  common frame — `meshToEigen`'s optional `transform` argument is the pattern.
- **Named attributes, in both directions.** Geogram communicates through attributes
  with fixed names, and every family below depends on one:

  | Attribute | Where | Type | Used by |
  |---|---|---|---|
  | `tex_coord` | facet corners | `vec2` | `mesh_make_atlas`, the packers |
  | `tex_coord` | vertices | `vec2` | `mesh_compute_LSCM`, `mesh_compute_ABF_plus_plus` |

  | `chart` | facets | `index_t` | written by `mesh_segment`, read back by `mesh_get_charts`, and read as a *seed* by `mesh_make_atlas` |

  The first two rows are the same name on two different element containers, which is
  the trap: the flatteners write per-vertex UVs, the atlas pipeline writes per-corner
  UVs, and nothing in the API signature tells them apart. MeshLab's own split is the
  same one — `VT` versus `WT` — so the mapping is exact, but it has to be chosen per
  filter rather than once in the adapter.

- **Dimension is part of the mesh — but the adapter must *not* set it.**
  `set_anisotropy(M, s)` normalizes the vertex normals, scales them by `s`, and stores
  them in coordinates 3, 4 and 5; `remesh_smooth(..., dim=6)` then works in that space.
  It is tempting to conclude the adapter should build a 6-dimensional mesh, and Phase 4
  measured that this is exactly backwards: `set_anisotropy` raises the dimension itself,
  through `compute_normals`, and it does so **only when `dimension() < 6`**. Handed a
  mesh that is already 6-dimensional it skips the normal computation and normalizes the
  zeros the adapter left there. The adapter therefore always builds dimension 3 and lets
  `set_anisotropy` do the lifting, which is geogram's own documented idiom.

- **Triangles only.** `mesh_compute_ABF_plus_plus`, `mesh_compute_LSCM`, `mesh_segment`
  and `mesh_make_atlas` all document "only triangulated meshes are supported".
  `VCGMesh` stores triangles, so this is free — but a layer carrying polygon bits
  (`FP`) round-trips as triangles and the bits are lost. Declare it, do not silently
  drop it.

- **Logging and progress.** `GEO::Logger` writes to `stdout` by default, which the log
  panel never sees. Install a `GEO::LoggerClient` that forwards to `Document::writeLog`
  at `Info`, and `GEO::Console`'s warnings at `Warning`. Geogram's `GEO::ProgressTask`
  gets the same treatment against `doc.progressCallback()` — [Adding a
  Filter](../adding_a_filter.md) forbids logging progress as lines.

The adapter's surface, following `libiglmeshadapter.h` (72 lines of header, 306 of
implementation) closely enough that the two can be read side by side:

```cpp
namespace meshlab::geogram {

struct GeoMesh {
    GEO::Mesh mesh;
    std::vector<int> vertexToSourceIndex;
    std::vector<int> faceToSourceIndex;
    int skippedFaces = 0;
};

bool meshToGeo(const VCGMesh &in, GeoMesh &out, QString &error,
               const QMatrix4x4 *transform = nullptr, int dimension = 3);
bool geoToMesh(const GEO::Mesh &in, VCGMesh &out, QString &error);

bool readVertexTexCoords(const GEO::Mesh &in, VCGMesh &out,
                         const GeoMesh &source, QString &error);   // -> VT (+WT sync)
bool readCornerTexCoords(const GEO::Mesh &in, VCGMesh &out,
                         const GeoMesh &source, QString &error);   // -> WT
bool readFacetCharts(const GEO::Mesh &in, VCGMesh &out,
                     const GeoMesh &source, QString &error);       // -> FQ or FA

void ensureInitialized();   // GEO::initialize(GEOGRAM_INSTALL_NONE) behind std::once_flag
bool arpackAvailable();     // probes the dlopen the spectral paths depend on

} // namespace meshlab::geogram
```

## The filters

Eleven filters in four families. Names follow the `Verb Object (Backend)` grammar and
were checked against the shipped registry for collisions; every one of them is new.
A twelfth — the OpenSCAD script compiler — is deferred, not planned; see the end.

### Booleans — `Meshing/Boolean`

`WholeDocument` → `NewMeshes`, two `mesh` parameters, `outputSource`/`outputSecondSource`
so the layer is named `bunny (difference cube)` per [vocabulary §7](../vocabulary.md).
Direct siblings of the four `(libigl)` and four `(TrueForm)` filters already shipping.

| Display name | `id` / `pythonName` | Entry point | `outputTag` |
|---|---|---|---|
| Mesh Union (geogram) | `mesh_union_geogram` | `mesh_boolean_operation(R, A, B, "A+B")` | `union` |
| Mesh Intersection (geogram) | `mesh_intersection_geogram` | `… "A*B"` | `intersection` |
| Mesh Difference (geogram) | `mesh_difference_geogram` | `… "A-B"` | `difference` |
| Mesh Symmetric Difference (geogram) | `mesh_symmetric_difference_geogram` | **⚠ composed** — see below | `xor` |
| Repair Self-Intersections (geogram) | `repair_self_intersections_geogram` | `mesh_remove_intersections(M, maxIterations)` | `resolved` |

**⚠ Geogram has no symmetric difference.** `mesh_boolean_operation` accepts exactly
four operation strings — `"A+B"`, `"A*B"`, `"A-B"`, `"B-A"` — and the header declares
inline `mesh_union`, `mesh_intersection` and `mesh_difference` over them. Both incumbent
families ship the fourth operation (`mesh_symmetric_difference_libigl`, `…_trueform`),
so omitting it would leave geogram as the one backend with a hole in the family.

**Decided: compose it** as `(A−B) ∪ (B−A)`, three boolean evaluations, and say so in
`longDescriptionMarkdown` (decision 3) — the filter is visibly more expensive than its
three siblings and than the libigl and TrueForm versions, which resolve XOR natively,
and a user comparing run times deserves to know why.

`MESH_BOOL_OPS_ATTRIBS` interpolates attributes across the operation, which is how the
`transferFaceColor` / `transferVertexScalar` parameters the libigl booleans expose get
answered without birth-face bookkeeping in the plugin. It implies
`MESH_BOOL_OPS_NO_SIMPLIFY`, so turning it on costs the coplanar-facet merge — the
descriptor must say so.

### Parametrization — `Parametrization/UV Creation`

`SingleMesh` → `ModifyCurrentMesh`, `inputPrepare: ["VTex"]`, `outputModifies: ["VT", "WT"]`,
exactly matching `parametrize_by_least_squares_conformal_maps_libigl`.

| Display name | `id` / `pythonName` | Entry point |
|---|---|---|
| Parametrize by Least Squares Conformal Maps (geogram) | `parametrize_by_least_squares_conformal_maps_geogram` | `mesh_compute_LSCM(M, "tex_coord", false)` |
| Parametrize by Angle-Based Flattening (geogram) | `parametrize_by_angle_based_flattening_geogram` | `mesh_compute_ABF_plus_plus(M, "tex_coord")` |

Spelled out rather than *ABF++* and *LSCM* because the incumbent spells out *Least
Squares Conformal Maps*, and §5 forbids abbreviations that are not in the vocabulary.

`mesh_compute_LSCM`'s `spectral` argument is always `false`: the spectral variant is
not shipped (decision 2). Both entry points also take an optional
`angle_attribute_name`, which neither filter exposes — there is no MeshLab-side
producer of a desired-angle field to feed it.

### Atlas and segmentation

| Display name | Categories | Entry point | I/O |
|---|---|---|---|
| Parametrize by Atlas (geogram) | `Parametrization/UV Creation` | `mesh_make_atlas(M, hardAngleThreshold, param, pack)` | `SingleMesh` → `ModifyCurrentMesh`, `WT` |
| Pack UV Charts (geogram) | `Parametrization/Atlas Packing` | `pack_atlas_using_tetris_packer` · `…_using_xatlas` · `pack_atlas_only_normalize_charts` | `SingleMesh` → `ModifyCurrentMesh`, `WT` |
| Compute Chart Segmentation (geogram) | `Parametrization/Segmentation`, `Attribute/Scalar` | `mesh_segment(M, segmenter, segmentCount)` | `SingleMesh` → `ModifyCurrentMesh`, `FQ` |

`mesh_make_atlas` is the whole pipeline — segment, flatten, re-split where distortion is
high, pack — and its two enums are the filter's two interesting parameters:

| Parameter | Type | Options |
|---|---|---|
| `hardAngleThreshold` | `double`, default 45.0 | dihedral angle above which an edge becomes a chart boundary, in degrees |
| `chartParametrizer` | `enum`, default `abf` | `projection` · `lscm` · `abf` — `PARAM_SPECTRAL_LSCM` is omitted (decision 2) |
| `chartPacker` | `enum`, default `tetris` | `none` · `tetris` · `xatlas` |

The packer default departs from geogram's own `PACK_XATLAS` deliberately: MeshLab
already ships *Parametrize by Atlas (xatlas)* from `filter_xatlas`, so geogram's
vendored copy is the duplicate route and the Tetris packer — the one from the original
LSCM paper, available nowhere else here — is the reason to reach for this filter.
Carrying both copies is fine (decision 7); that they do not collide at link time is a
Phase 0 measurement.

`mesh_segment` writes a per-facet `index_t` named `chart` and returns the chart count.
Its primary category is the new `Parametrization/Segmentation` (decision 4), which must
exist in [vocabulary.md](../vocabulary.md) §1 and `filtercategories.h` before this
descriptor is written; `Attribute/Scalar` is the cross-listing, since the chart index
lands in the face scalar slot.
Three of its six segmenters are exposed: `vsaL2` · `vsaL12` (anisotropic) ·
`inertiaAxis`. `SEGMENT_SPECTRAL_8/20/100` are omitted under decision 2 — they reach
ARPACK through manifold harmonics rather than directly, so this was never confirmed by
measurement; if Phase 3 finds they run without it, reinstating them is one enum entry
each. The chart index is per-face integer data, so it lands in the face scalar slot
(`FQ`) with a `visualizationHints` request for scalar shading — **not** baked into face
color, per the compute-vs-colorize rule.

### Remeshing — `Meshing/Remeshing`

| Display name | `id` / `pythonName` | Entry point |
|---|---|---|
| Remesh by Centroidal Voronoi Tessellation (geogram) | `remesh_by_centroidal_voronoi_tessellation_geogram` | `remesh_smooth(in, out, targetVertexCount, dim, …)` |

`SingleMesh` → `ModifyCurrentMesh`, matching *Remesh Isotropically (vcglib)* and
*(TrueForm)* rather than geogram's out-parameter shape. One filter rather than two,
because isotropic and anisotropic differ only by `set_anisotropy` plus `dim=3` versus
`dim=6` — geogram's own documented idiom:

| Parameter | Type | Notes |
|---|---|---|
| `targetVertexCount` | `int` | geogram may exceed it to resolve problematic configurations; say so in `help` |
| `anisotropic` | `bool`, default false | selects `dim=6` and the `set_anisotropy` call |
| `anisotropy` | `double`, default 0.04 | `enabledWhen: "anisotropic"`; geogram's own example value |
| `lloydIterations` | `int`, default 5 | group `advanced.solver` |
| `newtonIterations` | `int`, default 30 | group `advanced.solver` |
| `newtonHessianSamples` | `int`, default 7 | `Newton_m`; group `advanced.solver` |
| `adjustToSource` | `bool`, default true | `mesh_adjust_surface` post-pass |
| `adjustMaxEdgeDistance` | `double`, default 0.5 | `enabledWhen: "adjustToSource"` |

The anisotropic mode is the one genuinely new thing here — vcglib and TrueForm both
remesh isotropically only.

## Phases

Each phase ends with something shippable and a green test run; nothing carries forward
except the adapter.

### Phase 0 — measure the dependency · no plugin code

One edit to `vcpkg.json`, one configure, five answers written back into this document.
Nothing else starts until they exist. The linkage and ARPACK questions the first draft
put here are already decided, so what remains is measurement of the dylib we have
committed to bundling.

| # | Question | How |
|---|---|---|
| 1 | What `minos` does the dylib carry? | `otool -l` on the installed `libgeogram.dylib`, reading `LC_BUILD_VERSION`. Anything but 15.0 means the triplet is not reaching the build, and the app will not launch on macOS 15. **The sharp one** — for a bundled dylib dyld enforces this, where a static archive only warns |
| 2 | What else does it pull in? | `otool -L`. Expect Accelerate and nothing from the re-vendored amgcl / libMeshb / rply. Anything else is a second library to bundle and sign |
| 3 | What are the CMake package and target names? | Read the installed `share/geogram/*.cmake`; gate on a candidate list the way [filter_igl/CMakeLists.txt](../../../plugins/filter_igl/CMakeLists.txt) does |
| 4 | Do geogram's and `filter_xatlas`'s copies of xatlas collide at link time? | Link the app with both. Ruled harmless by decision 7, but a duplicate-symbol error is a fact rather than a preference and has to be seen not assumed |
| 5 | What does it cost? | vcpkg build wall-clock and installed size, for the record |

**Exit criteria:** all five answered in writing; `vcpkg.json` carries `geogram`; the
packager's bundle-and-sign step for `libgeogram.dylib` is written and a built `.app`
launches from a path other than the build tree.

#### Results — 2026-09-19

`vcpkg.json` gained `"geogram"`; `build-release` reconfigured against the in-tree vcpkg
with `arm64-osx` and the overlay triplet. Geogram built from source locally (183 MB of
`vcpkg/buildtrees/geogram`, no binary-cache hit) inside a 54-second configure.

| # | Question | Measured |
|---|---|---|
| 1 | `minos` | **15.0 on both dylibs** (sdk 27.0). The overlay triplet reaches the geogram build, so the dyld-refuses-to-launch failure is not in play. This was the sharp one and it is clean |
| 2 | Transitive dependencies | `libgeogram`: libSystem, libc++. `libgeogram_num_3rdparty`: Accelerate, libSystem. **Nothing to bundle beyond geogram itself** — no OpenBLAS, nothing from the re-vendored amgcl / libMeshb / rply. Both already carry `@rpath` install names, so `install_name_tool -id` is not needed |
| 3 | CMake names | `find_package(Geogram CONFIG)`, one imported target **`Geogram::geogram`** (`SHARED IMPORTED`, interface link libraries `pthread;dl`) |
| 4 | xatlas link collision | **None.** Answered in Phase 1: the full build links `libMeshLab2PluginFilterXatlas.a` and `libgeogram.dylib` (which exports 17 xatlas symbols of its own) into every binary with no duplicate-symbol diagnostic. Two-level namespace, as expected |
| 5 | Cost | 12 MB installed (two dylibs plus headers), 183 MB of build tree, well under a minute to build |

**The port ships two dylibs, not one**, and only one of them is needed:

- `libgeogram.dylib` (6.9 MB) — the library, and the only exported CMake target.
- `libgeogram_num_3rdparty.dylib` (0.8 MB) — **not linked by `libgeogram`** (the two share
  zero symbols: 331 undefined in the former, 775 exported by the latter, intersection
  empty) and not exported as a target. Bundling can ignore it — except for what
  question 1 of the decisions now has to reckon with, below.

### Packaging — done 2026-09-20

`scripts/package-macos-dmg.sh` now embeds and signs geogram, following the `libomp`
block it sits beside: copy the versioned dylib into `Contents/Frameworks`,
`install_name_tool -id @rpath/...`, recreate the unversioned symlink, and `-change` the
main binary's reference to `@executable_path/../Frameworks/...`. A third signing loop
covers the hand-bundled dylibs, because macdeployqt signs only what it placed itself
and notarization rejects an unsigned Mach-O inside the bundle.

**Both geogram dylibs ship**, and the unversioned `libgeogram_num_3rdparty.dylib`
symlink is not decoration: it is the name OpenNL's ARPACK fallback opens. Nothing links
that library, so a future tidy-up would drop it without any build failing — the script
says why it is there.

The same pass also strips **build-tree rpaths**, which turned out to be a bundle-wide
problem rather than the single stale entry that prompted it — **64 `LC_RPATH` entries
across the bundle** pointed into a build directory:

| Source | Path they named |
|---|---|
| vcpkg's toolchain, on every target we link | `<build>/vcpkg_installed/arm64-osx/debug/lib` |
| the bundled Python extension modules, built elsewhere | `.../QMeshLab/build-debug/vcpkg_installed/...` — a directory that stopped existing at the 2026-09 rename |

None can resolve on another machine, and they ship the packager's home directory inside
the binary. Worse, a stale rpath that *does* exist on a developer's machine is exactly
how a missing bundled library goes unnoticed until someone else runs the app. Only
build-tree paths are removed, matched on `/vcpkg_installed/`; Homebrew Cellar rpaths on
third-party dylibs macdeployqt copied are left alone, because those libraries resolve
their own dependencies through them.

Verified end to end: the main binary is down to a single rpath,
`@executable_path/../Frameworks`; no Mach-O anywhere in the bundle retains a build-tree
rpath; no Mach-O has an `@rpath` dependency it can no longer resolve; `MeshLab.app`
copied to `/tmp` launches and stays up with no dyld diagnostics; and a probe built
against the bundled libraries with only `@executable_path/../Frameworks` on its rpath
reports `nlInitExtension("ARPACK") = 1` from that relocated copy. That closes the last
of Phase 0's exit criteria.

One trap worth recording, because it cost a silent failure: the script runs under
`set -o pipefail`, so the first version of the strip loop — which piped `grep` straight
into a `while` — **aborted the whole packaging run** at the first file with no match,
which is most files. It printed the one rpath it had already removed and exited, and
the `.dmg` left on disk was the previous run's, so nothing looked wrong. The matches are
now collected into a variable with `|| true`, and the loop reports a count.

### Phase 1 — plugin skeleton, adapter, booleans

The adapter is the whole risk, and the booleans are the smallest thing that exercises
all of it: two input layers, the layer matrix, a full rebuild on the way back.

- `plugins/filter_geogram/` per the layout above; three wiring edits
  (`plugins/CMakeLists.txt`, `filterpluginregistry.cpp`, the plugin's own `CMakeLists.txt`).
- `geogrammeshadapter` — `meshToGeo` / `geoToMesh` with the index mappings, `ensureInitialized`,
  the `GEO::Logger` client.
- Five filters: four booleans and *Repair Self-Intersections (geogram)*.

**Exit criteria:** `test_filter_descriptors` green (it is the tier that enforces name
grammar, category validity and `outputTag`); `test_filter_smoke` green with no new
`expectedRefusals()` rows; a boolean of two spheres produces a closed manifold result.

#### Results — 2026-09-20

Done. `plugins/filter_geogram/` builds as `MeshLab2PluginFilterGeogram` against
`Geogram::geogram`, with the five filters registered and all three test tiers green.

- **Adapter** — `geogrammeshadapter.{h,cpp}`. `meshToGeo` / `geoToMesh` with the
  `vertexToSourceIndex` / `faceToSourceIndex` mappings, the optional layer matrix, and
  the `dimension` argument Phase 4 needs for anisotropic remeshing.
  `ensureInitialized()` calls `GEO::initialize(GEOGRAM_INSTALL_NONE)` behind a
  `std::once_flag`; a `LogCapture` RAII object points `GEO::Logger` at a thread-local
  `QStringList` for the duration of one call, which avoids a global `Document` pointer
  for a per-run sink.
- **`geoToMesh` fan-triangulates.** Geogram merges coplanar facets by default, so its
  boolean output genuinely contains polygons — assuming triangles would have silently
  dropped geometry.
- **Behavioural test** — `FilterTests::geogramBooleansAgreeWithVolume`, modelled on the
  TrueForm one: two unit boxes overlapping in half their extent, so all four results
  have volumes that can be stated exactly. All four land within 2 % **and** come back
  closed and manifold (zero boundary edges, zero non-manifold edges). That is what
  confirms the composed symmetric difference is right.

**Attribute transfer, added 2026-09-20** to match what the libigl booleans offer:
`transferFaceColor`, `transferFaceScalar`, `transferVertexColor`,
`transferVertexScalar`, all defaulting to false, in an `attributes` group. (`scalar`
rather than libigl's `quality`, per [vocabulary](../vocabulary.md) §4 — the libigl
spelling is pass-2 debt, not a pattern to copy.)

geogram cannot supply the correspondence. Reading
`mesh_surface_intersection.cpp` settles it: `copy_operand` copies **only positions and
connectivity** out of each operand, so `MESH_BOOL_OPS_ATTRIBS` interpolates attributes
that are already on the result and there is no hook between the copy and the
intersection to put ours there. Nor is there a birth-face array of the kind libigl
returns.

It is recovered geometrically instead, and for this particular problem that is exact
rather than approximate: **every facet of a boolean result lies on one of the two
operand surfaces**, so the closest operand face to a result face's centroid is the face
it came from. The only ambiguity is where the two surfaces coincide, and there neither
answer is more right. Per-vertex values are interpolated barycentrically across the
source face rather than copied from a nearest vertex, because the vertices created
along the intersection curve match no operand vertex at all and are exactly the ones a
nearest-vertex rule gets visibly wrong. Query points are pulled back through the
inverse layer matrix rather than copying and transforming whole meshes.

`FilterTests::geogramBooleanTransfersAttributes` paints two overlapping boxes in
distinct colours and asserts that every face of the union comes back as one of them,
with its scalar agreeing — a mismatch between the two would mean they resolved to
different operands.

#### The dynamic dependency bites the tests, not just the bundle

The first run of `test_filter_descriptors` died before reaching a single assertion:

```
dyld: Library not loaded: @rpath/libgeogram.1.9.3.dylib
  tried: '/opt/homebrew/lib/libgeogram.1.9.3.dylib' (no such file)
```

The claim above that dynamic linkage does not affect the tests because "CMake embeds
the rpath automatically" was wrong, and is corrected there. It does not: the vcpkg
library is linked by a build-relative path and no `-rpath` is emitted for it, so the
only `LC_RPATH` any binary carried was `/opt/homebrew/lib`, left over from libomp. The
app and all fourteen test binaries were affected equally.

The fix is four lines in the top-level `CMakeLists.txt`, appending the vcpkg lib
directory to `CMAKE_BUILD_RPATH` before the first target is created (the variable is
read at target creation, not at link). It is general rather than geogram-specific,
because the next shared vcpkg dependency would hit exactly the same wall.

### Phase 2 — parametrization

Two filters, per-vertex `tex_coord` read-back, `WT` synchronization. No new adapter
machinery beyond `readVertexTexCoords`.

**Exit criteria:** both filters produce a UV layout a textured view can show; a
behavioural assertion in `test_filters.cpp` that ABF++ on a disk produces lower angle
distortion than LSCM on the same input — the claim the whole family rests on.

#### Results — 2026-09-20

Done. `geogramparametrization.{h,cpp}`, two filters, all three tiers green.

- **Adapter gained `readVertexTexCoords`** — binds the `tex_coord` vertex attribute,
  checks it is two-component, and returns it indexed by GEO vertex row for the caller
  to map through `vertexToSourceIndex`. Phase 3 reuses the same shape for facet corners.
- **Write-back mirrors `iglparametrization`** exactly: per-vertex UVs always, per-wedge
  refreshed from them when the layer already carried some, `IOM_VERTTEXCOORD` set,
  `markMeshGeometryChanged` called. Both backends therefore leave a layer in the same
  state.
- **Behavioural test** — `FilterTests::geogramAbfBeatsLscmOnAngleDistortion`. A sphere
  cap at 70° half-angle, subdivision 4: curved everywhere so no isometric layout
  exists, open so both methods apply. Mean absolute per-corner angle difference between
  surface and layout: **ABF++ 0.00805 rad against LSCM's 0.00882**, about 9 % better.
  The test also rejects a non-finite or collapsed layout, which is the
  "a textured view can show it" criterion.

Two things worth recording, both discovered by measurement:

- **`facets.connect()` is required, and colocating is not allowed.** A `GEO::Mesh`
  built by `create_triangles` + `set_vertex` has no facet adjacency, and the flatteners
  navigate through it — without the call every triangle is its own chart. geogram's own
  pipelines reach for `mesh_repair(MESH_REPAIR_COLOCATE)`, which would renumber
  vertices and silently invalidate `vertexToSourceIndex`. `facets.connect()` derives
  adjacency from vertex indices alone and leaves numbering untouched, so it is the only
  correct choice here. The cost is that coincident-but-distinct vertices stay split;
  the filters' help says so rather than welding behind the user's back.
- **The boundary check is ours, not geogram's.** Neither flattener refuses a closed
  surface — they return a degenerate layout. Both filters therefore count border edges
  first and refuse with an actionable message, which is what lets the smoke tier drive
  them: both land on the `open` rung, and neither needs an `expectedRefusals()` row.

### Phase 3 — atlas, packing, segmentation

Facet-corner attributes, which is the second half of the adapter.

**Exit criteria:** *Parametrize by Atlas (geogram)* on a closed mesh yields charts
packed inside the unit square with no overlap; chart count from `mesh_get_charts` is
reported in `infoMessages`; the segmentation filter writes `FQ` and asks for scalar
shading rather than baking color.

#### Results — 2026-09-20

Done. `geogramatlas.{h,cpp}`, three filters, all three tiers green.

Groundwork first, in the order decision 4 requires: `Parametrization/Segmentation`
added to `src/plugins/filtercategories.cpp` and to
[vocabulary.md](../vocabulary.md) §1 with its rationale, **before** any descriptor
named it. Under decision 5, `pack_uv_charts` became `pack_uv_charts_vcglib` /
*Pack UV Charts (vcglib)* — descriptor, the plugin's dispatch constant and four test
references. `docs/design/history/` was deliberately left alone: it records what was
true when written.

- **Adapter** gained `readCornerTexCoords`, `readFacetCharts` and
  `writeWedgeTexCoordsToGeo`. The last is the first write-direction attribute path: a
  packer rearranges an atlas it is *given*, so the layer's existing per-wedge UVs have
  to be copied into the facet-corner attribute before `pack_atlas_using_*` runs.
- **Facet corners map to per-wedge directly**, so the atlas write-back is a straight
  copy rather than the per-vertex plus sync dance Phase 2 needed.
- **Behavioural tests** — `geogramAtlasPacksChartsIntoUnitSquare` asserts every UV lies
  in the unit square, that total UV area is below 1 (overlap would exceed it) but above
  0.01, and that the chart count reaches `infoMessages`.
  `geogramSegmentationWritesScalarNotColor` asserts `FQ` carries at least two distinct
  chart indices, that a `FaceQuality` visualization hint is returned, and that **every
  face colour is byte-identical to before the run** — the compute-vs-colorize rule,
  tested rather than asserted in prose.

#### `GEO::initialize()` is not enough on its own

Chart segmentation aborted the process on every fixture:

```
Assertion failed: variable_exists. File: .../basic/environment.cpp, Line: 217
```

`GEO::initialize()` constructs the CmdLine environment but declares **no variables in
it**, and geogram reads its own algorithm defaults back out of that environment:
`Delaunay::create()` asks for `algo:delaunay` and `geo_assert`s when it is missing.
Anything reaching `CentroidalVoronoiTesselation` goes through that path — chart
segmentation now, and the CVT remesher in Phase 4.

`ensureInitialized()` therefore also calls `CmdLine::import_arg_group("algo")`, which
only installs defaults and parses no command line. Worth stating plainly because the
failure mode is bad: not an exception, a `geo_assert` that takes the application down.
The `GEOGRAM_INSTALL_NONE` section above explains which flags must stay off; this is
the one thing that must be switched **on**.

### Phase 4 — remeshing

A 6-dimensional `GEO::Mesh`, which nothing before this phase needs.

**Exit criteria:** isotropic mode on a scan produces a mesh within a few percent of
`targetVertexCount` with better triangle quality than the input; anisotropic mode
visibly aligns elements to curvature — the one claim no incumbent can match.

#### Results — 2026-09-20

Done, and the phase needed **no new adapter machinery at all** — it removed some.

- **The `dimension` argument is gone.** It existed solely for this phase, and
  measurement showed using it would break `set_anisotropy` (see the correction in the
  adapter section above). `meshToGeo` now always builds dimension 3. Dead parameter,
  removed rather than left as a trap.
- **One filter**, `geogramremeshing.{h,cpp}`, 141 lines. Isotropic and anisotropic in
  one descriptor, as planned: `anisotropic` selects `set_anisotropy` plus `dim=6`,
  with `anisotropy`, `adjustMaxEdgeDistance` gated by `enabledWhen` and the three
  solver knobs under `advanced.solver`.
- **The layer is rebuilt in place** — `geoToMesh` clears the target and refills it, so
  no correspondence with the old tessellation survives and per-vertex and per-face
  attributes are dropped. The help says so and points at the Transfer family.
- **`CmdLine::import_arg_group("algo")` paid off immediately**: the CVT remesher reaches
  `Delaunay::create()` on the same path chart segmentation did, so without Phase 3's
  fix this filter would have aborted the process too.

Behavioural tests, on a single-stack cylinder whose side is tiled by slivers:

| Assertion | Measured |
|---|---|
| Hits the requested vertex count | asked 2000, got **exactly 2000** |
| Improves element shape | mean triangle quality **0.150 → 0.979** |
| Anisotropy actually does something | anisotropic **0.929** against isotropic **0.979** at strength 0.2 |

The third is the interesting one and reads backwards on purpose: anisotropic elements
are *deliberately* less equilateral, stretched along the cylinder's flat direction and
compressed across its curved one. Lower mean quality than the isotropic run on the same
input is what confirms the six-dimensional lift did something, rather than the parameter
being quietly ignored.

## Decisions taken

All eight rulings, taken 2026-09-19. Nothing in this plan is open.

| # | Question | Ruling | What follows from it |
|---|---|---|---|
| 1 | Static overlay port, or bundle the dylib? | **Bundle the dylib** | No overlay port, and Phase 0 does not experiment with linkage. `scripts/package-macos-dmg.sh` gains a copy / `install_name_tool -id` / `-change` block for `libgeogram.dylib` following the `libomp` pattern at lines 75–87, plus a third `codesign` loop. The `minos` check becomes load-bearing: for a bundled dylib dyld enforces it, and a wrong value means the app will not launch on macOS 15 |
| 2 | The spectral filters | **Ship them** (re-ruled 2026-09-20, superseding the earlier "drop them") | *Parametrize by Spectral Conformal Maps (geogram)*, the `spectralLscm` chart parametrizer and the three `spectral*` segmenters are all present. They cost nothing to enable: ARPACK was in the tree the whole time and geogram finds it by itself — see the ARPACK section, which is now a record of a wrong diagnosis followed by the right one. Every spectral path is gated on a runtime probe and refuses rather than falling back silently |
| 3 | Symmetric difference | **Compose it, and say so** | `(A−B) ∪ (B−A)`, three boolean evaluations, with the cost stated in `longDescriptionMarkdown`. The geogram family ships the same four operations as libigl and TrueForm |
| 4 | A category for chart segmentation | **`Parametrization/Segmentation` is a worthy addition** | [vocabulary.md](../vocabulary.md) §1 and `src/plugins/filtercategories.h`/`.cpp` gain the subcategory **before** Phase 3 declares a descriptor against it — the loader validates against the closed set, so the order is not optional. *Compute Chart Segmentation (geogram)* takes it as its primary category, and *Parametrize by Voronoi Atlas (vcglib)* is worth reviewing for a cross-listing in the same edit |
| 5 | Renaming the incumbent `Pack UV Charts` | **Renaming shipped filters is acceptable** | `pack_uv_charts` in `filter_texture_defragmentation` takes a backend suffix in Phase 3, so the family names itself completely per §6. Its `pythonName` changes with it and no alias is added, per the standing ruling that pymeshlab compatibility does not constrain this API |
| 6 | Parameter id casing | **New filters follow the rule; the pass comes later** | `filter_geogram` uses `lowerCamelCase` throughout, including `firstMesh` / `secondMesh` where `filter_igl` writes `first_mesh` / `second_mesh`. The 107 snake_case ids already in the registry stay as they are and are retired by a separate mechanical pass, not by this plugin |
| 7 | Two copies of xatlas in one binary | **Not a problem** | *Parametrize by Atlas (geogram)* may expose `PACK_XATLAS` freely. The default stays Tetris on its own merits — it is the packer available nowhere else here. The link test stays in Phase 0 as measurement, not as a question |
| 8 | The OpenSCAD script compiler | **Deferred** | Out of scope for this plugin. Recorded below rather than dropped |

## See also: frame fields

Geogram's quad-meshing story is a frame-field generator
(`GlobalParam2d::frame_field`, `FrameField::create_from_surface_mesh`) and a periodic
global parametrization (`GlobalParam2d::PGP`), with **no quad extraction** — the
`quad:` command-line arguments it declares are never read anywhere in the open library,
because their consumer was Vorpaline. So geogram cannot add a third quad remesher
beside Instant Meshes and QuadWild. What it could add is the *input* to one, which is
only worth porting if something can consume it: see
[Frame Fields](frame_fields.md).

## Deferred: the OpenSCAD compiler

Not planned, and not a phase — recorded so the capability is not rediscovered from
scratch later.

`CSGCompiler::compile_string` in `mesh/mesh_CSG.h` is a complete OpenSCAD-subset
language: `CSGBuilder` exposes `square`, `circle`, `cube`, `sphere`, `cylinder`,
`multmatrix`, `linear_extrude`, `rotate_extrude`, `projection`, `hull`, and n-ary
`union_instr` / `intersection` / `difference` over a `CSGScope` of meshes. As
*Create Mesh from OpenSCAD Script (geogram)* under `Creation/Primitives` it would be a
single filter with `inputDomain: None`, one `string` parameter, and `outputTag`
standing alone as the whole layer name per [vocabulary §7](../vocabulary.md).

It is cheap to build and unlike anything MeshLab ships. It is also a different kind of
feature from the other eleven — a modelling language rather than an operation on the
current document — which is the reason it is deferred rather than folded in. Revisit it
once the four families are landed and the adapter has proven itself.
