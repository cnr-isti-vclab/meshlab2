# Branches and Releases

How code moves from a change to a published build. The workflows are in
`.github/workflows/`; this document is the reasoning behind them.

> **Status.** The workflows and both branches exist, and Linux builds and passes its tests.
> The first tag, Windows code signing, a desktop-tested Linux package and the nightly/beta channels do not yet — see
> [Open items](#open-items).

## Branches

| Branch | Holds | Receives changes by | Builds |
| --- | --- | --- | --- |
| `devel` | Integration; the default branch; every PR targets it | Pull request or direct push | macOS DMG (signed and notarized), Windows ZIP (unsigned), docs at `/nightly/` |
| `main` | Released states only | Fast-forward from `devel` at release time | Docs at the site root |
| `gh-pages` | The published documentation | `docs.yml` only | — |

`main` never gets a commit of its own, so nothing has to be merged back into `devel`.
The original MeshLab needed an automatic `main → devel` merge because its release
workflow committed the version from CI; here the version is an ordinary commit on
`devel`, made by the maintainer.

## Versions and tags

The version is `MESHLAB2_VERSION` in `CMakeLists.txt` (`YYYY.MM`). A release is a tag on
`main` with exactly that string — `2026.11`, without a `MeshLab-` prefix. A hotfix adds a
third field (`2026.11.1`); the Windows version resource pads to four numbers
(`2026,11,1,0`).

`release.yml` runs on such a tag and refuses to continue unless the tag equals
`MESHLAB2_VERSION` and the tagged commit is an ancestor of `origin/main`.

The title bar and the About box name the build as the original MeshLab did: a release by
its version (`MeshLab 2026.11`), any other build -- a nightly, a local one -- by the day it
was configured (`MeshLab 2026.10.29`, UTC, like the artifact names). The packaging
workflows pass `-DMESHLAB2_RELEASE_BUILD=ON` only when `release.yml` calls them with a
version; the date is otherwise refreshed at every configure, which a commit triggers.

## Releasing

A release is started by **pushing a tag**, not by running a workflow by hand. The tag
name is the version, and `release.yml` takes it from there.

1. On `devel`: bump `MESHLAB2_VERSION` in `CMakeLists.txt`; make sure `vcglib/` is
   clean, pushed to `origin/devel` and its gitlink bumped (see `AGENTS.md`); make sure the
   tests pass.
2. Fast-forward `main` to that commit and push it. This deploys the release docs.

   ```bash
   git switch main && git merge --ff-only devel && git push origin main
   ```

3. Tag that commit with the version and push the tag. This starts `release.yml`.

   ```bash
   git tag 2026.11 main && git push origin 2026.11
   ```

4. `release.yml` checks the tag, builds the signed macOS DMG, the Windows ZIP, the Linux
   AppImage and the `pymeshlab2` wheels, and creates a **draft** GitHub Release with all
   of them attached and generated notes. Watch it in the Actions tab.
5. Open the draft, download and try the artifacts, then publish it by hand.

The workflow stops before building if the tag does not equal `MESHLAB2_VERSION` or the
tagged commit is not on `main`, so a mistyped tag cannot produce a release.

### If a release run fails

Nothing is public until the draft is published. Delete the draft release on GitHub if
one was created, delete the tag, fix the cause, and tag again:

```bash
git tag -d 2026.11 && git push origin :refs/tags/2026.11
```

Artifacts built for a release are named `MeshLab-<version>-<platform>`; those built on a
push are named `MeshLab-<date>-<commit>-<platform>` and expire after 14 days. Actions
artifacts are visible only to logged-in GitHub users, which is why a release attaches
its files to the GitHub Release instead.

## Python wheels

`wheels.yml` builds `pymeshlab2` with cibuildwheel for CPython 3.10–3.14 on macOS
arm64 and Windows x64, and runs `python/tests` against every wheel. Its settings are
in `pyproject.toml` (`[tool.cibuildwheel]`), so a local `cibuildwheel` run builds the
same thing. The wheel's version is `MESHLAB2_VERSION`, read by scikit-build-core, so a
tagged release and its wheels always agree.

It runs on a release tag (called by `release.yml`), by hand, and on `devel` pushes that
touch the bindings or the packaging (`src/python`, `python/`, `pyproject.toml`,
`vcpkg.json`, the macOS repair script). Plugin changes do not trigger it: the
application workflows already build every plugin on every push.

The wheels are attached to the draft release; they do not reach PyPI on their own.
Publishing is a separate, deliberate step, not yet automated — see Open items.

## Hotfixes

Branch from the release tag, fix, merge into `main` and into `devel`, bump to
`YYYY.MM.N`, tag. The release workflow is the same.

## Signing

| Platform | Where it happens | Credentials |
| --- | --- | --- |
| macOS | Every `devel` push and every release (a nightly has to run on other machines) | Repository secrets `MACOS_CERTIFICATE_*`, `APPLE_API_*` |
| Windows | Releases only, once set up | Certum Open Source Code Signing; not yet automated |

The Windows workflow assembles the portable folder, then zips it; the signing step goes
between the two, because a ZIP cannot carry a signature.

## Build caches follow the default branch

GitHub lets a run restore caches saved by its own branch and by the **default** branch,
not by other branches. The vcpkg dependencies take hours to build, and the builds that
save them run on `devel`; a release run on a tag can reuse them only if `devel` is the
default branch. Otherwise every release starts from a cold cache.

### Helper programs

QuadWild and TextureConsolidation run as separate executables built from submodules,
and building them took about half of a cold Linux build (QuadWild 7 min, the
TextureConsolidation helpers 6 min, with all four cores) and 12 minutes of QuadWild
alone on Windows, although their sources change only when the submodule is bumped.
`.github/actions/helper-cache` keeps the five programs (25 MB) between runs, one cache
entry per set, keyed on the runner image, the submodule commit, the plugin's
`CMakeLists.txt` and `MACOSX_DEPLOYMENT_TARGET` (set only by the wheel jobs, and
checked by delocate). It sets `MESHLAB2_HELPERS_CACHE_DIR` in the environment, which
the root `CMakeLists.txt` reads: a plugin that finds its helpers there uses them and
skips the external build; otherwise it builds them and copies them in, and the save
step stores each set that is complete and was not restored.

Measured on 2026-11-03 before choosing this over ccache: the cache budget was 9.0 of
10 GB, and a compiler cache of a few GB per platform would evict the vcpkg caches,
whose cold rebuild costs two hours on Windows. Restoring the helpers' build directories
instead would not work: a fresh checkout makes the sources newer than the restored
objects, so the inner build recompiles everything.

## Open items

- Make `devel` the default branch on GitHub (Settings → Branches). Do this before the
  first release, for the cache reason above; branch protection comes after.
- Windows signing step (waiting on Certum activation and a manual `signtool` test).
- Linux: `linux-appimage.yml` builds, tests and packages an AppImage on `devel` pushes,
  and `release.yml` attaches it to a release. It packages and passes a headless smoke test
  (no unresolved libraries; the bundled interpreter and `_meshlab` start); it has not yet
  been tried on a real desktop. It needs a glibc at least as new as the one it was built
  against, so it is built in an `almalinux:9` container (glibc 2.34) and runs on Ubuntu
  22.04, Debian 12, RHEL 9 and their derivatives, and anything newer. That is as old as
  it can go: Qt's own Linux binaries, which it bundles, were built on RHEL 9 and need
  glibc 2.34 and `GLIBCXX_3.4.29` themselves. GCC 13, which TextureConsolidation's helpers
  are tested with, comes from `gcc-toolset-13`, whose libstdc++ links what is newer than
  the system's GCC 11 into the binaries, so nothing needs a libstdc++ newer than Qt's
  floor. On an older system an `AppRun` hook written by `scripts/package-linux-appimage.sh`
  says which glibc is needed (in a dialog too, through zenity or kdialog) instead of the
  loader's list of missing symbol versions. History: built on `ubuntu-22.04`, then on
  `ubuntu-24.04` from 2026-10 for its GCC 13, which shut out 22.04 (glibc 2.39) until a
  user report on 22.04 brought it to AlmaLinux 9 in 2026-11. The file has to be made
  executable after download (`chmod +x`). Flatpak later for discoverability, and for
  systems older than glibc 2.34.
- Nightly and beta channels as public GitHub pre-releases: designed, not built. Actions
  artifacts need a GitHub login, so testers cannot use them.
- `tests.yml` is manual on purpose: it restores the packaging job's vcpkg cache and the
  repository's cache budget is shared. Revisit if tests should gate pull requests.
- Move the signing secrets into a protected `release` environment with required
  reviewers.
- Publish the wheels to PyPI. `pymeshlab2` is not registered there yet. The
  recommended route is a trusted publisher (PyPI → the project → Publishing, naming
  this repository and `release.yml`) and a `pypa/gh-action-pypi-publish` job after
  `publish`, run from a protected environment so a person approves each upload.
- Linux wheels: `wheels.yml` builds macOS and Windows only. A manylinux build needs
  Qt and the vcpkg tree inside cibuildwheel's container.
