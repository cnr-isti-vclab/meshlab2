# 3MF I/O

This plugin ports the original MeshLab's `io_3mf` functionality to this
rewrite's I/O API. The original implementation is retained for comparison in
`.reference/meshlab/src/meshlabplugins/io_3mf`.

Parsing and writing are provided by the upstream
[3MF Consortium lib3mf](https://github.com/3MFConsortium/lib3mf), obtained from
the pinned vcpkg manifest. No lib3mf source is copied or patched here.

MeshLab's current I/O interface returns a single mesh. Consequently, imported
3MF build items and nested components are recursively flattened into one layer,
with all component and build-item transforms baked into vertex coordinates.
Face colors, embedded texture images and per-corner texture coordinates are
imported. Export matches the original MeshLab plugin's scope and writes the
current layer as one geometry-only 3MF build item.
