# Branches and Releases

How code moves from a change to a published build. The workflows are in
`.github/workflows/`; this document is the reasoning behind them.

> **Status.** The workflows are written for this model. The `devel` branch, the first
> tag, Windows code signing and a Linux package do not exist yet — see
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

1. On `devel`: bump `MESHLAB2_VERSION`; make sure `vcglib/` is clean, pushed to
   `origin/devel` and its gitlink bumped (see CLAUDE.md); make sure the tests pass.
2. Fast-forward `main` to that commit and push it. This deploys the release docs.
3. Tag the commit with the version and push the tag.
4. `release.yml` builds the signed macOS DMG and the Windows ZIP, then creates a
   **draft** GitHub Release with both attached and generated notes.
5. Check the draft, download and try the artifacts, then publish it by hand.

Artifacts built for a release are named `MeshLab-<version>-<platform>`; those built on a
push are named `MeshLab-<date>-<commit>-<platform>` and expire after 14 days.

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

## Open items

- Create `devel` and make it the default branch; branch protection afterwards.
- Windows signing step (waiting on Certum activation and a manual `signtool` test).
- Linux: no workflow exists. Plan is a plain build job first, then an AppImage built on
  `ubuntu-22.04`; Flatpak later for discoverability.
- `tests.yml` is manual only; it should run on pull requests once its cost is known.
- Move the signing secrets into a protected `release` environment with required
  reviewers.
