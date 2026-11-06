# TextureConsolidation integration

## Provenance and boundary

*Transfer Color from Visible Rasters to Texture (Texture Consolidation)* runs the
programs of [`hliuw/TextureConsolidation`](https://github.com/hliuw/TextureConsolidation),
the code of Liu, Wang, Kim and Kazhdan, *Texture Consolidation of Generated Multi-View
Renderings* (arXiv 2610.36098). MeshLab pins it as the recursive submodule
`external/texture-consolidation`, which brings three libraries of its own:

| Repository | Licence | Submodule URL now |
|---|---|---|
| TextureConsolidation | BSD-3-Clause (`LICENSE`) | `cignoni/TextureConsolidation` |
| Misha | BSD-3-Clause, in every header; the repository has no `LICENSE` file | `cignoni/Misha` |
| PhongRodriguesVF | BSD-3-Clause (`LICENSE`) | `cignoni/PhongRodriguesVF` |
| TextureSignalProcessing | MIT (`LICENSE`) | `cignoni/TextureSignalProcessing` |

**All four point at forks**, which carry fixes proposed upstream. MeshLab's own tree
carries no patch, so this is still the pinned-submodule case of
[the third-party rule](../../docs/design/architecture.md#choosing-how-to-bring-third-party-code-in);
the goal is forks with nothing beyond upstream, and then the original URLs.

Nothing is linked into MeshLab. `ExternalProject` builds three programs --
`BackProjection`, `TextureFlow` and `TextureDomainStitching` -- and the filter runs them
in turn through `HelperProcess` (`src/core/helperprocess.h`), exchanging a PLY, PNG images
and a `cameras.json` through a temporary directory. A crash or an `exit()` in the research
code therefore stops a helper, not the application. The adapter's own code is
`textureconsolidationfilterplugin.cpp` and the camera conversion `pinholecamera.h`.

## What the forks carry

| Fork | Commit | Change | Kind |
|---|---|---|---|
| TextureConsolidation | `f42e50f`, `3598fe7`, `d52ce2a`, `7ff6379`, `387b336` | submodule URLs and gitlinks | integration only, dropped at the end |
| | `4efb2e6` | `EIGEN_HAS_STD_RESULT_OF=0`, so the vendored Eigen 3.3.7 builds as C++20 under libc++; the stitcher built through a wrapper that makes Misha's `MK_ERROR_OUT` throw, so its failures exit non-zero; an error instead of an endless loop when every overlap is NaN | proposed |
| | `e5104a9` | advected confidence zeroed outside the atlas (an uninitialized read); `BackProjection` variance summed in order | proposed |
| | `e21d060` | merge of upstream `3a27d41`, the flow anchored to the unregistered blend | upstream |
| Misha | `bcc2065` | `MishaK::Polynomial::NumCoefficients<…>` qualified, which Apple clang 21 needs | proposed |
| | `b8199a8` | nearest-simplex ties broken by index | proposed |
| PhongRodriguesVF | `2b96675` | system assembly in simplex order; deterministic tangent frames | proposed |
| | `504d022` | element matrices assembled in blocks, bounding memory | proposed |
| TextureSignalProcessing | `b7d2d65` | the polygon-clipping guard | proposed |
| | `8c6f35a` | scheduling-independent hierarchy, operators, quadrature and components. *The message says "bit depth"; this is the content* | proposed |
| | `baa775b` | `TextureStitching.cpp` and `ImageIO.h`: the bit depth as a run-time value, the cell-confidence padding fix and the ordered scatter, together | proposed |

TextureSignalProcessing's commits mix three proposals and want splitting before their
pull request. The measurements behind each change are in
[texture_consolidation.md](../../docs/design/proposals/texture_consolidation.md).

## Building and shipping

- **C++20, and OpenMP required**, as upstream builds them. GCC needs version 13 or later,
  which is why the AppImage builds on Ubuntu 24.04; Apple clang needs an OpenMP runtime
  (`OpenMP_ROOT`, or Homebrew's `libomp`), and without one the plugin is skipped.
- **`libpng` and `libjpeg-turbo`** come from MeshLab's vcpkg tree, passed as a prefix path.
- **The stitcher dominates the build**: one translation unit, about a minute and 2.6 GB with
  clang at `-O3`, roughly three times as long with GCC. The plugin library does not depend
  on the helper build, so the rest of MeshLab compiles alongside it.
- **Bundling** (`meshlab2_bundle_texture_consolidation`): the programs go to
  `Contents/Helpers/texture-consolidation/` in the macOS bundle and to
  `texture-consolidation/bin/` beside the executable elsewhere. The DMG script repoints them
  at the bundled `libomp`.
- **Licence notices: incomplete.** Only TextureConsolidation's `LICENSE` is copied into the
  packages. BSD-3-Clause and MIT both require the notice to accompany binaries, so Misha's,
  PhongRodriguesVF's and TextureSignalProcessing's notices have to be shipped too.

## Updating

```sh
git -C external/texture-consolidation fetch origin
git -C external/texture-consolidation checkout <reviewed-commit>
git submodule update --init --recursive external/texture-consolidation
```

Then rebuild the three helpers and run `textureConsolidationCameraMatchesShotProjection`
and `textureConsolidationBuildsTextureFromRasters`; the latter runs the whole filter twice
and requires identical pixels. Review upstream's command-line options (the filter passes
them by name in `textureconsolidationfilterplugin.cpp`), output file names, the recursive
submodule commits and the licences before recording the new gitlink.

When the pull requests merge: point `.gitmodules` back at the original repositories
(`hliuw/TextureConsolidation`, `mkazhdan/Misha`, `mkazhdan/PhongRodriguesVF`,
`mkazhdan/TextureSignalProcessing`), drop the integration-only commits, and update this
file.
