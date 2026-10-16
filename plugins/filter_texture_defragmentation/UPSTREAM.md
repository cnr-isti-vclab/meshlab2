# Texture Defragmentation provenance

- Project: [texture-defrag](https://github.com/maggio-a/texture-defrag) — *Texture
  Defragmentation for Photo-Reconstructed 3D Models*, Andrea Maggiordomo, Paolo Cignoni,
  Marco Tarini, Eurographics 2021.
- Licence: GPL-3.0, `upstream/TextureDefragmentation/LICENSE`.
- Vendored commit: **`5c5543af8b44b009533a26d2c6000511b71ea834`** (2021-05-02, "fixed
  vertex pointer access"), **inferred** — nothing recorded it when the code was imported.
  The evidence is below.
- Vendored files: the 33 `src/*` sources and `LICENSE`, in `upstream/TextureDefragmentation/`.
  Upstream's `src/gl_utils.{h,cpp}` are not vendored.
- Paths of the form `src/...` below are relative to `upstream/TextureDefragmentation/`.

The code was imported from the classic MeshLab copy of this filter, not from upstream
directly (`.reference/meshlab/src/meshlabplugins/filter_texture_defragmentation/
TextureDefragmentation`). That copy had been edited inside MeshLab, so what is vendored is
upstream *plus MeshLab's edits plus ours*, and only 9 of the 33 source files are
byte-identical to any upstream commit.

## How the commit was identified (2026-10-16)

Upstream's `master` is its only branch, has no tags, and holds three commits: the initial
import (2021-03-29), a README rename, and `5c5543a` (2021-05-02). The README rename changes
no source, and `5c5543a` changes **one** file, `src/intersection.h`:

```diff
-    Mesh::VertexPointer V0() const { return fp->cV0(e); }
-    Mesh::VertexPointer V1() const { return fp->cV1(e); }
+	Mesh::VertexPointer V0() const { return fp->V0(e); }
+	Mesh::VertexPointer V1() const { return fp->V1(e); }
```

Our `src/intersection.h` has the `fp->V0(e)` form, differing from `5c5543a` only in
indentation, so the vendored code is at least as new as that commit, and as it is the tip
of `master` it is that commit. This rests on that one file: the other differences cannot
separate the three commits, because none of them is the same in any upstream version.

It was found without downloading source: GitHub's tree listing gives the git blob hash of
every file at every commit, and `git hash-object --no-filters <file>` gives ours.

## QMeshLab Adaptations

The algorithmic source is kept intentionally close to upstream so it can be compared when a newer upstream version is integrated.

Adapted files:

- `src/mesh.cpp`: removed standalone mesh file IO behavior from the build path. QMeshLab feeds the algorithm from the in-memory `Document` mesh model.
- `src/texture_object.cpp`: changed `TextureObject` from an OpenGL texture owner into a QImage/size container. GPU upload is left to QMeshLab renderer backends.
- `src/texture_rendering.cpp`: replaced the original OpenGL atlas rasterizer with a QImage renderer behind the same `RenderTexture` API. This keeps the backend boundary narrow for a future QRhi renderer.
- `src/mesh_graph.cpp`: removed the stale OpenGL utility include.

### Merging islands chosen by hand

The `SmallIslandRemover` variant of `FilterType` is itself a QMeshLab addition (see
`seam_remover.h`, and the median-border threshold in `ComputeCost`). On top of it, the same
merger can take its candidates from the mesh's face selection instead of from a size
threshold, which is what the filter's *Islands to merge* option switches between. Three
changes, all marked `QMeshLab:` in the source:

- `src/mesh_graph.{h,cpp}`: `FaceGroup` caches whether any of its faces is selected, folded
  into the existing `UpdateCache` sweep, and exposes `AnySelectedFace()`. The plugin copies
  the selection onto the faces when it builds the defrag mesh, so the mark travels with the
  face -- the pipeline cleans and compacts before the chart graph is built, so index
  alignment with the source layer is not something to rely on.
- `src/seam_remover.{h,cpp}`: `AlgoParameters::mergeSelectedIslands` picks which rule gates
  a merge in `ComputeCost`. Both rules have the same shape: reject only when *neither* chart
  qualifies, so a qualifying island dissolves into an unqualifying neighbour.
- `src/seam_remover.cpp`: two things are needed to make "merge what I picked" mean it.
  `FaceGroup::ClearFaceSelection()` is called as a merge is committed, because the union of
  two charts inherits the mark and a single selected face would otherwise swallow the whole
  connected atlas. And eligibility is re-checked where the merge is executed, not only where
  its cost was computed -- costs are worked out up front, so a pair queued while the mark was
  live would otherwise still run after the mark had been consumed. A pair that no longer
  qualifies has its cost retired to infinity rather than being skipped, because the greedy
  loop terminates on an infinite queue top and a bare `continue` leaves it churning.

Measured on an eight-island layer with one island selected: one merge, seven islands left.
Before the first of those two changes the same input collapsed to a single island; before the
second it lost four.

Removed files:

- `src/gl_utils.cpp`
- `src/gl_utils.h`

Those files were only needed by the original OpenGL renderer and standalone importer path. QMeshLab does not build or use them.

## Other differences from the classic MeshLab copy

Measured 2026-10-16 against `.reference/meshlab` (HEAD `684885a`). Besides the adaptations
above, 12 further files differ from it. All but one are comments and formatting:

- `src/packing.{h,cpp}`: `Pack` takes a `randomSeed` (default `0`) and passes it to the
  packer's `packingParams.randomSeed`, so the filter can honour the `randomSeed` convention.
  This is the one functional change in the list.
- Doxygen comments added to `arap`, `mesh`, `mesh_attribute`, `packing`, `seams`, `shell`
  and `texture_optimization` headers and sources — most of the line count.
- `src/shell.h`: forward declarations of `Mesh` and `FaceGroup`.
- Brace and indentation style in `src/shell.cpp`, `src/seams.cpp` and `src/arap.cpp`.

MeshLab's own edits to its copy are a separate matter: 18 of its 35 files differ from
upstream `5c5543a`. Its history for this directory (C++17 compliance, removal of `using
namespace`, typo fixes, the Python rename) runs from 2021-05 to 2022-02 and accounts for
them, but that has not been checked file by file.

## Refresh

There is no `check_upstream_status.sh` for this tree, because the classic reference copy is
not upstream and differs from it. To update: clone upstream under `.reference/`, compare
`src/` file by file against the vendored files, reapply the adaptations above, rebuild and
retest, and record the new commit here as a recorded fact rather than an inference.
