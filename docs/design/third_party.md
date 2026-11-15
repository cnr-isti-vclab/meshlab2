# Third-Party Code

How geometry libraries and vendored algorithm code enter the build, what each vendored
tree must record, and the About dialog lists that name them to the user.

See also: [Architecture](architecture.md) · [`external/README.md`](../../external/README.md)

## Third-party geometry code

The geometry libraries and the vendored algorithm packages are listed to the user in the
**Libraries** tab of the About dialog, as two lists: general-purpose libraries that many
filters draw on, and packages vendored for one algorithm. Each entry carries a link.

**Those two lists are `kGeometryLibraries` and `kFocusedPackages` in
[`src/ui/aboutdialog.cpp`](../../src/ui/aboutdialog.cpp), and nothing generates them.**
They are the only place either inventory is written down for the user, so they go stale
silently. Add or remove an entry whenever any of these changes:

| What changed | Where it shows |
| --- | --- |
| A geometry dependency added to or dropped from `vcpkg.json` | CGAL, libigl, Geogram, Embree today |
| A submodule added to or dropped from `.gitmodules` | VCGLib, TrueForm, MeshFix, QSlim, QuadWild-BiMDF, JKQtPlotter, TextureConsolidation, TextureSignalProcessing today |
| A tree vendored under `plugins/*/upstream/` or similar | PoissonRecon, Texture Defragmentation, xatlas, BPA, Instant Meshes, Isoparametrization, Open3D today |

Not every dependency belongs there: the lists are about *geometry processing*, so the
file-format, UI and numerics libraries are deliberately absent and the tab says so. A new
dependency that only reads a file format needs no entry.

`tests/test_third_party.cpp` enforces all of this. It reads the two arrays back out of the
source and checks them against `.gitmodules`, `vcpkg.json` and the vendored trees on disk,
in both directions: anything the build pulls in must be either listed in the dialog or
recorded in that test's `notGeometry()` table with a reason, and anything the dialog lists
must still come from somewhere. So the reminder above is a courtesy — the build is what
actually stops the lists going stale.

Each vendored package also records its own upstream and license next to its code, in
`plugins/<plugin>/UPSTREAM.md` — see [`external/README.md`](../../external/README.md) for
the submodules. Those are for a developer updating the package; the About lists are for a
user asking what MeshLab is built on, and the two are maintained separately.

### Choosing how to bring third-party code in

There are four ways third-party code reaches the build, and which one a package gets is
decided by one question first: **do we change the source?**

| Do we change it? | Mechanism | Because |
|---|---|---|
| **Yes** — patch it, adapt it to the document model, or take a subset of it | **Vendored**, under `plugins/<plugin>/upstream/` (PoissonRecon keeps its own spelling, `Src/`, so the tree matches upstream) | Neither vcpkg nor a submodule can carry a patch without a build-time patch step, which is machinery we would own for every package. A copy in the tree can be patched, diffed against the reference clone and refreshed by hand. |
| No, and a **vcpkg port** exists that suits us | **vcpkg** (`vcpkg.json`) | Someone else maintains the build. "Suits us" has to be checked: it must work with the external Qt, and not drag in heavy dependencies we do not use. |
| No, and no suitable port | **Pinned submodule** under `external/` | Upstream stays byte-for-byte what its authors published, at a recorded commit. |
| No, and only two or three files of it are wanted | **Vendored verbatim** | Copying `xatlas.cpp`/`xatlas.h` is cheaper than a submodule of a whole repository that we compile a corner of. |

**Upstream activity does not decide the mechanism, but it decides what vendoring costs.**
A dormant upstream cannot drift, so a vendored copy is the end of the story. An active
upstream will move away from the copy, so a vendored tree of one must be pinned to a
commit or tag, keep its patches few and recorded, and be revisited when a vcpkg port
appears. Measured 2026-10-16:

| Vendored | Upstream's last push | Local changes |
|---|---|---|
| BPA | 2022-09 | none; 3 source files and the license |
| xatlas | 2024-06 | none; 2 source files and the license, verbatim from one commit |
| Instant Meshes | 2022-01 | 6 files: removed TBB API, deterministic sorts, dropped serialization |
| Texture Defragmentation | 2021-10 | adapted: OpenGL and file IO removed, plus MeshLab additions marked `QMeshLab:` (25 marked lines in 9 files) |
| Isoparametrization | MeshLab's own history | patched: levmar replaced by newuoa (63 marked lines in 15 files) |
| **PoissonRecon** | **2026-04, active** | 2 files: a thread-count setter, an MSVC parser workaround |
| **Open3D** | **2026-09, active** | 1 file, plus being a 116-file subset of the library |

The four dormant or quiet upstreams (BPA, xatlas, Instant Meshes, Texture Defragmentation) cost nothing to vendor. The two active ones are vendored
*because* we patch them, and are the ones to watch: PoissonRecon is held at version 18.75
(commit `cd6dc7d`), and Open3D at its v0.20.0 tag. Open3D is also the case to revisit
first, since [microsoft/vcpkg#53211](https://github.com/microsoft/vcpkg/pull/53211) would
become its port; see `plugins/filter_open3d/UPSTREAM.md` for why it is not an option yet.

Every **submodule** is unmodified (MeshFix, QSlim, QuadWild-BiMDF, TrueForm,
JKQtPlotter) except the TextureConsolidation and TextureSignalProcessing forks, which
carry fixes proposed upstream ([UPSTREAM](../../plugins/filter_texture_consolidation/UPSTREAM.md)), and `external/README.md` gives each one's reason; JKQtPlotter's is the
clearest case of a vcpkg port that does not suit (it needs vcpkg's Qt). **vcglib is the
exception**: it is the project's own library, edited in lockstep with MeshLab, and is
governed by its own rules in `AGENTS.md`. One package is fetched at configure time, not stored: QuadWild's GMM dependency, pinned by
SHA-224 with mirrors, because the unmodified QuadWild submodule would otherwise download it
over plain HTTP.

#### What a vendored tree must record

`plugins/<plugin>/UPSTREAM.md`, beside the plugin and outside the vendored tree (so the tree
stays comparable with upstream), giving: the upstream URL; the **commit or tag** vendored;
the licence; which files were taken; **every local modification, with the reason**; and how
to refresh. A `check_upstream_status.sh` that diffs against a reference clone under the
git-ignored `.reference/` is worth having once a tree has patches, as PoissonRecon, Instant
Meshes and Open3D do. A new vendored tree also needs its About entry and passes
`tests/test_third_party.cpp`, which now checks that the `UPSTREAM.md` exists.

The document had four names until 2026-10-16. The mapping that made them one, measured
before the move (no collision at any target, no script or CMake reference to any old name,
three documentation mentions updated):

| Was | Now |
|---|---|
| `filter_screened_poisson/POISSONRECON_PROVENANCE.md` | `filter_screened_poisson/UPSTREAM.md` |
| `filter_xatlas/UPSTREAM_PROVENANCE.md` | `filter_xatlas/UPSTREAM.md` |
| `filter_texture_defragmentation/upstream/README_QMESHLAB.md` | `filter_texture_defragmentation/UPSTREAM.md` — out of the vendored tree |
| the eight already named `UPSTREAM.md` (BPA, Instant Meshes, Isoparametrization, MeshFix, Open3D, QSlim, QuadWild, io_trueform) | unchanged |

#### Known gaps (2026-10-16)

- **Texture Defragmentation's vendored commit is inferred, not recorded** —
  `5c5543a`, from the one file in upstream's history that changed after the initial import.
  Its note also records, for the first time, 12 files that differ from the classic MeshLab
  copy it was imported from, one of them functional (`Pack` takes a `randomSeed`).
- Isoparametrization's "upstream" is the classic MeshLab repository, not a separate
  project, so it is vendored in the sense of "extracted and patched" rather than "copied".
