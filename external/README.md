# External dependencies

> Adding or removing anything here also means editing `kFocusedPackages` (or
> `kGeometryLibraries`) in `src/ui/aboutdialog.cpp`: the About dialog's **Libraries** tab
> is where these are shown to the user, and nothing generates it. See
> [Third-party geometry code](../docs/design/architecture.md#third-party-geometry-code).


How a package comes to be here, rather than in vcpkg or vendored under a plugin, is the
rule in [Choosing how to bring third-party code
in](../docs/design/architecture.md#choosing-how-to-bring-third-party-code-in): everything
in this directory is kept unmodified.

## JKQtPlotter

MeshLab uses only JKQtPlotter's `JKQTCommon` and `JKQTMathText` libraries to
render formulas in filter help. It is kept as a pinned Git submodule instead of
the `jkqtplotter` vcpkg package because that package:

- depends on vcpkg's Qt, while MeshLab intentionally uses an external Qt
  installation;
- builds the plotting libraries and additional dependencies that MeshLab does
  not need;
- does not provide a feature for installing only the MathText component.

The root CMake configuration disables the unused JKQtPlotter components and
builds only the formula renderer with the embedded Fira Math font.

Initialize the dependency after cloning MeshLab with:

```sh
git submodule update --init external/jkqtplotter
```

## MeshFix

MeshLab builds the reusable core of MeshFix directly from the pinned
`external/meshfix` submodule. The upstream tree stays unmodified; the adapter,
provenance, update procedure, and known API limitations are documented in
[`plugins/filter_meshfix/UPSTREAM.md`](../plugins/filter_meshfix/UPSTREAM.md).

Initialize it after cloning with:

```sh
git submodule update --init external/meshfix
```

## QSlim

MeshLab preserves Garland's original QSlim edge-collapse implementation in the
pinned `external/qslim` submodule. Only its reusable MixKit core is compiled;
integration, licensing, and update details are in
[`plugins/filter_qslim/UPSTREAM.md`](../plugins/filter_qslim/UPSTREAM.md).

Initialize it after cloning with:

```sh
git submodule update --init external/qslim
```

## QuadWild-BiMDF

MeshLab keeps the GPL-3.0-or-later QuadWild-BiMDF command-line pipeline in a pinned,
recursive submodule. Its two required executables are built in an isolated
CMake project and bundled with MeshLab; no upstream types or targets enter the
application. Integration details and the update procedure are documented in
[`plugins/filter_quadwild/UPSTREAM.md`](../plugins/filter_quadwild/UPSTREAM.md).

Initialize it after cloning with:

```sh
git submodule update --init --recursive external/quadwild-bimdf
```

## TextureConsolidation

MeshLab runs the three command-line programs of
[TextureConsolidation](https://github.com/hliuw/TextureConsolidation) (BSD-3-Clause) from
the pinned, recursive `external/texture-consolidation` submodule, which brings Misha,
PhongRodriguesVF and TextureSignalProcessing with it. They are built in an isolated CMake
project and bundled as helper programs; nothing of theirs is linked into the application.
No vcpkg port exists, and the code is research code that changes as its authors work on
it.

**The submodule and its three nested ones point at forks** (`cignoni/*`) carrying build,
memory and determinism fixes proposed upstream. MeshLab's tree carries no patch, and the
goal is forks with nothing beyond upstream. Which commits they carry, and the update
procedure, are in
[`plugins/filter_texture_consolidation/UPSTREAM.md`](../plugins/filter_texture_consolidation/UPSTREAM.md).

Initialize it after cloning with:

```sh
git submodule update --init --recursive external/texture-consolidation
```

## TextureSignalProcessing

MeshLab compiles the header-only `GradientDomain` library and the texel utilities of
[TextureSignalProcessing](https://github.com/mkazhdan/TextureSignalProcessing) (MIT) into
the application, for *Repair Texture Seams* and *Dilate Texture across Seams*, from the
pinned `external/TextureSignalProcessing` submodule. Only one file of the plugin includes
it. Misha, the library it is written on, comes from TextureConsolidation's checkout above,
since TSP's own copy under `third_party/` does not compile with Apple clang. No vcpkg port
exists.

**The submodule points at a fork** (`cignoni/TextureSignalProcessing`, branch `meshlab`)
that merges four fixes proposed upstream and not yet merged, without which the library
crashes on real atlases, links code under a no-sale licence into the application, or takes
3.5 times the memory. MeshLab's tree carries no patch. Which commits, and the way back to
upstream, are in
[`plugins/filter_texture_signal_processing/UPSTREAM.md`](../plugins/filter_texture_signal_processing/UPSTREAM.md).

Initialize it after cloning with:

```sh
git submodule update --init external/TextureSignalProcessing
```

## TrueForm

MeshLab uses the header-only [TrueForm](https://github.com/polydera/trueform)
geometry library from the pinned `external/trueform` submodule for a second,
independent OBJ/STL reader-writer and for a set of geometry filters.

**Licensing — read before redistributing.** TrueForm is dual-licensed: the
PolyForm Noncommercial License 1.0.0, or a commercial agreement with XLAB
(`external/trueform/LICENSE`, `COMMERCIAL.md`). Neither is GPL-compatible, so
this is not an ordinary dependency.

**MeshLab has explicit permission from the TrueForm owners (Polydera/XLAB) to
include the library.** That permission was obtained specifically for this
project; it is not conveyed by the public licence and does not extend to
third parties. Anyone redistributing a MeshLab binary built with the TrueForm
components needs their own agreement with XLAB — contact `info@polydera.com`.

The build option `MESHLAB2_PLUGIN_IO_TRUEFORM` is **ON** by default on the strength
of that permission. Integration details are in
[`plugins/io_trueform/UPSTREAM.md`](../plugins/io_trueform/UPSTREAM.md).

Initialize it after cloning with:

```sh
git submodule update --init external/trueform
```
