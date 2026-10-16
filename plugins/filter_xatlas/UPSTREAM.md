# xatlas provenance

- Upstream repository: `https://github.com/jpcy/xatlas`
- Vendored commit: `f700c7790aaa030e794b52ba7791a05c085faf0c`
- Vendored files:
  - `upstream/xatlas.h`
  - `upstream/xatlas.cpp`
  - `upstream/LICENSE`

## Local integration files

- `CMakeLists.txt`
- `xatlasfilterplugin.h`
- `xatlasfilterplugin.cpp`
- `filters.json`

## Local modifications to vendored upstream

- None. The vendored `xatlas` source files are copied verbatim from upstream commit `f700c7790aaa030e794b52ba7791a05c085faf0c`.

## Refresh workflow

1. Fetch or clone the upstream repository.
2. Copy `source/xatlas/xatlas.h`, `source/xatlas/xatlas.cpp`, and `LICENSE` into `plugins/filter_xatlas/upstream/`.
3. Rebuild `MeshLab2PluginFilterXAtlas` and `MeshLab`.
4. Re-test:
   - single-mesh atlas generation
   - UV view
   - progress/cancel behavior
   - failure path for multi-atlas output
