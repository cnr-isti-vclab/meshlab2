# MeshFix upstream integration

The filter in this directory adapts the reusable core of
[MeshFix 2.1](https://github.com/MarcoAttene/MeshFix-V2.1). The unmodified
upstream repository lives in the `external/meshfix` Git submodule; its gitlink
is the authoritative exact revision.

MeshLab builds the upstream `TMesh`, `Algorithms`, and `Kernel` sources while
excluding the command-line program in `src/MeshFix`. The adapter calls the
same default pipeline as that program:

1. `removeSmallestComponents()`
2. `fillSmallBoundaries(0, true)`
3. `meshclean()`

The optional CLI component-joining mode is not exposed because its
implementation is part of the command-line source rather than the reusable
core. MeshFix uses process-global state and offers no cancellation callback,
so MeshLab serializes calls and reports only phase-level progress.

To update the dependency:

```sh
git -C external/meshfix fetch
git -C external/meshfix checkout <tag-or-commit>
git add external/meshfix
```

Keep the submodule unmodified. Integration-specific code belongs in this
plugin. When updating, compare the upstream `SOURCES` list in its root
`CMakeLists.txt` with `MESHFIX_CORE_SOURCES` here; MeshLab mirrors that list
except for `src/MeshFix/meshfix.cpp`.
