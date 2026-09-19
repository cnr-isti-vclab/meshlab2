# Geogram

This document plans a `filter_geogram` plugin bringing [geogram](https://github.com/BrunoLevy/geogram)'s
boolean, parametrization and remeshing algorithms into MeshLab. **Nothing described
here is implemented.**

See also: [Adding a Filter](../adding_a_filter.md), [Filter Organization](../filter_organization.md)
(the one-dependency-one-plugin rule this follows), [Vocabulary](../vocabulary.md)
(which fixes every name below), [Data Model](../data_model.md).

## Status

As of 2026-09-18: nothing implemented, nothing scheduled. API signatures below were
read from geogram `main` and are exact; the vcpkg port is `geogram` 1.9.3.

## Why geogram, given libigl, CGAL and TrueForm are already here

Shipping several backends for one algorithm is deliberate, so overlap is not an
objection — but the case is stronger than overlap. Geogram brings algorithms that
have **no route at all** in the current registry:

| Capability | Today | Geogram adds |
|---|---|---|
| ABF++ angle-based flattening | — | `mesh_compute_ABF_plus_plus` — the reference implementation, by its own author |
| Spectral conformal parametrization | — | `mesh_compute_LSCM(spectral=true)` (Mullen et al. 2008) |
| Segment → flatten → pack in one call | — | `mesh_make_atlas`, a complete atlas pipeline with pluggable parametrizer and packer |
| Chart segmentation as a first-class step | — | `mesh_segment`, six segmenters (VSA, spectral, inertia) |
| CVT/Lloyd-Newton isotropic **and anisotropic** remeshing | vcglib and TrueForm do isotropic only | `remesh_smooth`, with a genuine anisotropic mode |
| n-ary boolean by expression | TrueForm's CSG filter | a second, independent implementation with exact predicates |
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

What dynamic linkage does **not** affect: the tests (they run from the build tree,
where CMake embeds the rpath automatically), the Python bindings
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
owning a patched build of someone else's library indefinitely. **Recommendation: try
the static build once in Phase 0 — an hour of work — and fall back to bundling if it
fights back.** Either way this is settled before Phase 1 starts.

### Other dependency facts

- `blas` + `lapack`. On macOS vcpkg's `blas` metapackage pulls **nothing** — it maps
  to the system Accelerate framework (`"platform": "!osx & !ios"` on the openblas
  dependency). On Linux and Windows it drags in OpenBLAS.
- The port builds with `GEOGRAM_LIB_ONLY=ON`, `GEOGRAM_WITH_VORPALINE=OFF`, and the
  `graphics` feature off by default — so no GLFW, no viewers. Request it as
  `"geogram"` with default features in `vcpkg.json`.
- Exported target comes from the port's own `Config.cmake.in`; confirm the name in
  Phase 0 and gate on it the way `filter_igl` gates on its candidate target list.

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
