# Branches and Releases

How code moves from a change to a published build. The workflows are in
`.github/workflows/`; this document is the reasoning behind them.

> **Status.** The workflows and both branches exist, and Linux builds and passes its tests.
> The first tag, Windows code signing, a tested Linux package and the nightly/beta channels do not yet — see
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

## Releasing

A release is started by **pushing a tag**, not by running a workflow by hand. The tag
name is the version, and `release.yml` takes it from there.

1. On `devel`: bump `MESHLAB2_VERSION` in `CMakeLists.txt`; make sure `vcglib/` is
   clean, pushed to `origin/devel` and its gitlink bumped (see CLAUDE.md); make sure the
   tests pass.
2. Fast-forward `main` to that commit and push it. This deploys the release docs.

   ```bash
   git switch main && git merge --ff-only devel && git push origin main
   ```

3. Tag that commit with the version and push the tag. This starts `release.yml`.

   ```bash
   git tag 2026.11 main && git push origin 2026.11
   ```

4. `release.yml` checks the tag, builds the signed macOS DMG and the Windows ZIP, and
   creates a **draft** GitHub Release with both attached and generated notes. Watch it
   in the Actions tab.
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

## Open items

- Make `devel` the default branch on GitHub (Settings → Branches). Do this before the
  first release, for the cache reason above; branch protection comes after.
- Windows signing step (waiting on Certum activation and a manual `signtool` test).
- Linux: `linux-appimage.yml` builds, tests and packages an AppImage on `devel` pushes,
  and `release.yml` attaches it to a release. The build and the tests are green; the
  packaging step (`scripts/package-linux-appimage.sh`) has not run yet. It is built on
  `ubuntu-22.04`, so it needs a glibc and libstdc++ at least that new; the file has to be
  made executable after download (`chmod +x`). Flatpak later for discoverability.
- Nightly and beta channels as public GitHub pre-releases: designed, not built. Actions
  artifacts need a GitHub login, so testers cannot use them.
- `tests.yml` is manual on purpose: it restores the packaging job's vcpkg cache and the
  repository's cache budget is shared. Revisit if tests should gate pull requests.
- Move the signing secrets into a protected `release` environment with required
  reviewers.
