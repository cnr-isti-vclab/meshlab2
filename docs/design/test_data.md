# Test Data

Where the test suite's inputs live, and what a test does when they are missing.

See also: [Adding a Filter](adding_a_filter.md) (the three filter-testing tiers),
`AGENTS.md` (the tier rules and the gargoyle intrinsics warning).

## Three places, by size

| Location | Tracked? | For |
|---|---|---|
| `tests/data/` | yes | Tiny hand-made inputs — a few KB. `simple.off`, `concave_polygon.*` |
| `tests/sample_mesh/` | yes | Small shared fixtures, up to about 1 MB. The spheres, and the 928 KB `gargoyle_small/` photogrammetry set that is the smoke tier's `rasters` rung |
| `tests/datasets/<name>/` | **no** | Anything larger, fetched on demand from a manifest |
| `tests/local/<name>/` | **no** | A maintainer's own data that is not publicly downloadable |
| `.reference/meshlab/sample/<name>/` | **no** | A clone of classic MeshLab. Its `sample/rangemaps/` tree is the standing corpus for the alignment filters — see below |

The first two are the ones a fresh clone gets. Everything a contributor needs to
run a green `ctest` is already in git.

## Fetching

`tests/datasets/datasets.json` is the manifest and the reviewable artefact; the
script is only its interpreter.

```bash
python3 tests/datasets/fetch.py --list     # what exists, what is present
python3 tests/datasets/fetch.py            # fetch everything missing
python3 tests/datasets/fetch.py --verify   # re-check what is here, download nothing
```

`cmake --build <dir> --target fetch_test_datasets` does the same thing from a
build tree.

Each entry carries `urls` (several, as mirrors), a `sha256`, `bytes`, a
`license` and a `ciEligible` flag. Three rules make it worth having:

- **The checksum is verified on every run of the script, not only after a
  download.** A file that changes under a stable URL has to fail loudly rather
  than quietly altering a behavioural test's numbers. `--verify` exits non-zero
  on a mismatch.
- **`license` is per dataset and is not assumed.** Research corpora differ, and
  some require a signed agreement. `UNRESOLVED` is an honest value and the
  script warns on every run until it is replaced. The first entry needed it: the
  licence was not discoverable from the download URL, the hosting repository
  (NOASSERTION) or the library's own source, and turned out to be public domain
  once someone who knew the provenance pointed at the right page. `licenseUrl`
  records where that was checked.
- **`citation` is separate from `license`.** Public domain is not the same as no
  obligation — a dataset can be freely usable and still ask to be cited, which
  matters as soon as results computed from it are published.
- **`ciEligible` is a separate, deliberate decision.** It is false until someone
  has read the licence and is willing to say unattended CI may download the
  data. `--ci-only` skips everything else rather than leaving the judgement to
  whoever next reads a workflow file.

## Using it from a test

```cpp
#include "testdatasets.h"

void MyTests::somethingNeedingRealData()
{
    MESHLAB2_REQUIRE_DATASET(path, "rangemaps/partial.mlp");
    // ... path is now an absolute path that exists
}
```

The macro looks under `tests/datasets/`, then `tests/local/`, then
`.reference/meshlab/sample/`, so a test does not care which supplied the data,
and **skips with an actionable message** when none does.

### Range maps are the standing corpus for alignment

Alignment filters are tested against the real range maps in classic MeshLab's
`sample/rangemaps/`, not against synthetic primitives. Scanner data has the
properties that matter and that a generated shape does not: uneven sampling,
boundary noise, partial overlap, and genuine surface detail for feature
descriptors to bite on.

Two files make it a *ground truth* rather than merely realistic input —
`two_roughly_aligned.mlp` and `two_finely_aligned.mlp` hold the same two scans,
with `faccia045.ply` identical in both and `faccia000.ply` carrying the
correction between them. An ICP filter given the rough pair has a known right
answer to reach. `partial.mlp` holds twelve scans for the multiway case.

Clone it with:

```bash
git clone --depth 1 https://github.com/cnr-isti-vclab/meshlab .reference/meshlab
```

**Absent data must skip, never fail.** This is the rule the mechanism exists to
enforce. Before it, two project-loading tests hard-coded paths under `tests/`
for data that only ever existed under `tests/local/`; they failed permanently on
every machine but one, and that permanent redness hid real regressions for
months because the suite was never expected to be green.

Keep the descriptor and smoke tiers fixture-free so they always run. Only the
behavioural tier in `test_filters.cpp` should opt into large data.

## A committed micro-fixture, when a dataset is large

Where a workflow needs a large corpus, commit a few frames of it as well, so the
descriptor and smoke tiers can still drive the filter at all. The gargoyle rule
applies to any such reduction and is easy to get wrong: **if the images are
rescaled, the camera intrinsics must follow** — `ViewportPx` and `CenterPx`
scale with the pixels, `PixelSizeMm` inversely, `FocalMm` untouched — or every
projection silently misprojects while still reporting success.

## CI

[`tests.yml`](../../.github/workflows/tests.yml) runs the suite on macOS arm64.
It is **`workflow_dispatch` only** — started from the Actions tab or with
`gh workflow run tests.yml`, never on a push. Building the whole project for
every commit would compete with the packaging jobs for the repository's shared
cache budget without telling anyone much.

Two inputs: `ctest_filter` passes a regex to `ctest -R`, and `fetch_datasets`
(default true) decides whether the CI-eligible datasets are fetched first.

How it treats the two caches is deliberate:

- **vcpkg: restore only, from the packaging job's key.** The dependencies are
  identical to the ones `macos-dmg.yml` already builds on every push to main, so
  this job reuses them and never saves an entry of its own. A second ~4 GB copy
  would spend twice from the 10 GB per-repository budget and let the two jobs
  evict each other. The cost is that a run with no warm packaging cache compiles
  the dependencies and throws them away.
- **Datasets: its own key**, `hashFiles('tests/datasets/datasets.json')`, so
  adding a dataset does not invalidate the dependencies and a dependency bump
  does not re-download data. Only `tests/datasets/.archives` is cached, because
  `fetch.py` re-extracts from a verified archive without touching the network —
  which also means a stale cache can never restore an old `datasets.json` or
  `fetch.py` over the checked-out ones.

The fetch step uses `--ci-only`, so whether a dataset may be downloaded
unattended is answered by its `ciEligible` flag in the manifest rather than in
the workflow file.

**Render probes are not run there.** They need a real GPU and a window server,
which a runner does not have, and `QT_QPA_PLATFORM=offscreen` — correct for this
suite — is exactly wrong for them. See
[rendering.md](rendering.md#verifying-the-render-path).
