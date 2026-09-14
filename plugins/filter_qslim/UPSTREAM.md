# Original QSlim integration

This plugin compiles the original `MxEdgeQSlim` implementation directly from
the pinned `external/qslim` submodule. Update it with:

```sh
git -C external/qslim fetch
git -C external/qslim checkout <reviewed-commit>
```

Only the geometry-processing core listed in `CMakeLists.txt` is built. The
legacy command-line application, GUI, rendering, and file I/O are deliberately
excluded. MeshLab converts meshes in memory and returns a geometry-only layer.

The compiled files belong to MixKit, distributed under the GNU Library GPL
version 2 or later with the static-linking exception in
`external/qslim/mixkit/COPYING.txt`. Do not add files marked with additional
restrictions in `external/qslim/mixkit/COPYING.txt` without reviewing them.
