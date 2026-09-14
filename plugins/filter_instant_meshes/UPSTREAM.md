# Instant Meshes provenance

- Project: [Instant Meshes](https://github.com/wjakob/instant-meshes)
- License: BSD-3-Clause (see `upstream/LICENSE.txt`)
- Vendored commit: `7b3160864a2e1025af498c84cfed91cbfb613698`
- Local reference clone: `.reference/instant-meshes`

Only the mesh-processing sources used by the in-memory filter are vendored. The
original GUI, command-line program, file serializers, OpenGL code, resources,
NanoGUI, GLFW, legacy TBB, and build system are intentionally omitted. MeshLab
uses VCGLib for conversion to and from its mesh model and vcpkg's current oneTBB.

## Local changes inside `upstream`

- `src/bvh.cpp`: replace the removed `tbb::task` API with `tbb::task_group`.
- `src/field.cpp`: remove the legacy global TBB scheduler and omit unused state
  serialization methods.
- `src/field.h`: omit the corresponding serialization API.
- `src/hierarchy.cpp`: use `std::stable_sort` in deterministic mode and omit
  unused state serialization methods.
- `src/hierarchy.h`: omit the corresponding serialization API.
- `src/extract.cpp`: use `std::stable_sort` in deterministic mode.

The MeshLab adapter and plugin remain outside `upstream`. To inspect or update
the vendored code, clone or update the reference repository and run:

```bash
plugins/filter_instant_meshes/check_upstream_status.sh
```

When updating, compare against the new commit, copy only the listed computation
files, reapply the small compatibility changes above, rebuild, test, and update
the pinned commit in this document.
