# External dependencies

> Adding or removing anything here also means editing `kFocusedPackages` (or
> `kGeometryLibraries`) in `src/ui/aboutdialog.cpp`: the About dialog's **Libraries** tab
> is where these are shown to the user, and nothing generates it. See
> [Third-party geometry code](../docs/design/architecture.md#third-party-geometry-code).


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
