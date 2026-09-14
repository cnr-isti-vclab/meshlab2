# QuadWild-BiMDF integration

## Provenance and boundary

MeshLab pins [`cgg-bern/quadwild-bimdf`](https://github.com/cgg-bern/quadwild-bimdf)
as the recursive submodule `external/quadwild-bimdf`. The upstream tree remains
unmodified. It is GPL-3.0-or-later and is itself a fork of the original QuadWild
implementation of *Reliable Feature-Line Driven Quad-Remeshing*.

The code is intentionally not linked into MeshLab. Its CMake project has broad
global settings and dependencies, and the command-line programs contain
assertions and process-level exits. `ExternalProject` builds only `quadwild` and
`quad_from_patches`; MeshLab runs them as helper processes and exchanges OBJ
files through a temporary directory. Upstream failures therefore cannot crash
the application, at the cost of temporary disk I/O.

Both are launched through `HelperProcess` (`src/core/helperprocess.h`), which
owns everything the process boundary costs us: it logs the command and its pid,
echoes the helper's own output as it arrives, maps the banners `quadwild` prints
between phases onto the progress bar, and keeps a registry of what is still
running so Cancel and a window close stop the helper -- and everything it
spawned -- rather than leaving it to outlive the application.

GMM 5.4.2 is the only dependency not recorded as a QuadWild submodule. Upstream
pins its archive by SHA-224 but names an HTTP mirror. MeshLab fetches that exact
archive over HTTPS and passes its include path to the isolated build.

## Updating

```sh
git -C external/quadwild-bimdf fetch origin
git -C external/quadwild-bimdf checkout <reviewed-commit>
git submodule update --init --recursive external/quadwild-bimdf
```

Then rebuild both helper targets and run a feature-aware and an organic smoke
test. Review upstream command-line arguments, output file names, configuration
paths, recursive submodule commits, and license before recording the new gitlink.
MeshLab carries no source patch to rebase.
