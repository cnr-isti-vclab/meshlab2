# Usage Statistics

A plan for collecting aggregate usage data from MeshLab installs, designed so that
the data is useful enough to change decisions and narrow enough that it cannot
identify anyone. **Not implemented.** The codebase today has no network code at all
(`find_package(Qt6 ...)` at `CMakeLists.txt:155` does not include `Network`) and no
crash handling.

The richer, local-only half of the same idea — the per-user action journal this
aggregates from, and the predictions built on it — is
[Local Usage Model](local_usage_model.md).

See also: [Preferences](../preferences.md) · [Architecture](../architecture.md) ·
[Filter Organization](../filter_organization.md)

## 1. Why collect anything

MeshLab ships **423 filters across 39 filter plugins, exposing 1473 parameters**,
organized into 11 root categories and 45 subcategories, plus 6 I/O plugins covering 9
file formats, 4 interactive tools and 42 preferences. (Counts as of 2026-11-24. The
first draft of this document, eleven weeks earlier, said 292 filters and 12
preferences — the surface grows by roughly a filter a day, which is itself the
argument.) Nobody knows which parts of that surface are load-bearing. Every maintenance decision — what to deprecate,
what to test, what to optimize, what to document first — is currently made on
intuition and on the loudest issue reports.

Telemetry is only worth its cost if it changes decisions, so the schema is derived
from decisions rather than from "what is easy to log". Each field must name the
question it answers; a field that answers nothing is deleted at the next release
review. This register is the contract:

| Decision | What answers it |
|---|---|
| Which filters can be deprecated or merged? | invocation counts per filter, per version, over months |
| Which filters are broken in the field? | failure counts + stable error codes per filter |
| Which of the 1473 parameters are dead UI? | count of runs where a parameter differed from its default |
| Where does time go for real users? | runtime histograms bucketed by mesh size |
| Did release N regress performance? | same histograms, sliced by app version |
| Which file formats deserve engineering? | I/O counts and failure rates per format and direction |
| Which importer should own a contested format? | the same counts keyed by plugin — `obj` is claimed by three plugins today, `stl` by two |
| Which languages and regions to translate for? | country distribution against UI language, collected unlinked (§3.6) |
| What hardware must keep working? | coarse OS / arch / GPU-family / RAM buckets |
| What document sizes must stay interactive? | peak mesh count, peak face count, peak memory buckets |
| Are new features discovered at all? | entry-point counters (menu, panel, search, Python, tool) |
| Is a release stable enough to promote? | unclean-exit rate per version, and which filter was running |
| How long must old versions be supported? | version distribution over time |
| Which defaults are wrong? | which preferences are non-default |

Fourteen decisions, and every one of them is answerable from counters and histograms.
None of them needs a per-action timeline, a user identity, a file name, or a
timestamp. That is the whole basis of the privacy design: **the useful questions are
aggregate questions, so collect aggregates and nothing else.**

## 2. What to change relative to old MeshLab

MeshLab's mechanism was a per-event ping to a server-side script, with the interesting
data arriving as URL parameters and being tallied into counts. It worked, and the
counts were genuinely useful. Four things about that shape are worth fixing:

1. **One request per event.** Chatty, IP-visible at every action, and it reconstructs
   a session timeline server-side whether you want one or not. Replaced here by *one
   digest per session*, aggregated on the client.
2. **Counters fixed at design time.** A tally can only answer the question it was
   built for; a new question requires waiting a release. Replaced by a versioned
   schema over *raw session digests* retained briefly, so new aggregations can be
   recomputed over recent history.
3. **No consent boundary and no visible payload.** Replaced by explicit opt-in, a
   local copy of everything ever sent, and a viewer that shows the exact bytes.
4. **No reciprocity.** The data went one way. Replaced by published monthly
   aggregates — see [§8](#8-giving-the-data-back).

## 3. Privacy architecture

Seven invariants. They are design-time constraints, not policies to remember while
writing code — the point of each is that violating it requires deliberately defeating
a mechanism rather than merely forgetting a rule.

### 3.1 PII is unrepresentable, not filtered

The wire format has **no free-text field anywhere**. Every value is one of:

- an identifier from a compile-time-known vocabulary (a filter key from the plugin
  registry, a parameter id, a preference id, an error code);
- an enum (`os`, `arch`, `channel`, `entry_point`);
- a bucket index into a fixed ladder (size, duration, memory, count);
- a small non-negative integer count.

There is no `QString` in the payload that came from user data, so a path, a mesh
name, a URL or a log line has nowhere to go. The serializer refuses unknown keys
(allowlist, not denylist), and the ingest endpoint validates against the same
schema and drops the whole submission on any unknown key. This matters because the
richest sources of "what the user did" in this codebase are also the most dangerous:
`ScriptAction` in `src/core/document_undo_types.h` carries `filePaths` and a
`pythonCall` string with paths interpolated into it. It is a tempting source and must
never be one; only `filterKey` and the *ids* of non-default parameters may be read
from it.

### 3.2 A session digest, not an event stream

The client accumulates counters in memory for the duration of a session and emits one
digest. Sending `{smooth: 12, decimate: 3}` instead of fifteen timestamped events
destroys the ordering and timing information that would let anyone reconstruct a
work session — and cuts request volume and storage by roughly two orders of
magnitude. Privacy and cost point the same way here, which is usually a sign the
design is right.

### 3.3 No timestamps

The payload contains no wall-clock time of any kind — no session start, no per-action
time, not even a timezone. Durations are sent as bucket histograms; the only date
attached to a digest is the **server's ingest day**. This removes "works at 03:00",
"active on Italian holidays", and clock-skew-as-a-fingerprint in one stroke, and it
removes any need to trust the client's clock.

### 3.4 Identity: a session id and a rotating install id

Two identifiers, both random, neither derived from anything about the machine:

- `session_id` — fresh UUIDv4 per run. Used only to deduplicate retries.
- `install_id` — random, stored locally, **rotated on the first launch of each
  calendar month**, with only the current value kept.

The rotating id is the compromise that makes the data honest: without it, one power
user running 200 sessions is indistinguishable from 200 users, and every per-user
statistic is wrong. With monthly rotation, "sessions per install this month" and
"filters per install this month" are answerable, while cross-month tracking is not —
the previous value is gone from disk and was never sent alongside the new one.

A "Forget me" button clears the local id and queues a deletion request for the
current id; combined with the 90-day raw retention in §3.8, most erasure happens by
expiry rather than by request.

### 3.5 A fingerprinting budget for the envelope

The envelope is where re-identification risk actually lives, because a combination of
coarse facts can be near-unique even when every fact in it is harmless on its own.
This is the oldest result in the field — ZIP code, birth date and sex identify most of
the US population; the EFF's browser-fingerprinting work found the average browser
carried around 18 bits and was unique among half a million. Each field is innocuous;
the tuple is an identifier.

The budget is not an efficiency measure. The envelope is a few hundred bytes against a
payload dominated by the filter map, and sending raw GPU and driver strings would cost
on the order of 60 MB a year even at the largest scale in §7.2 — nothing. It buys
three specific things:

1. **It protects the id rotation.** Monthly rotation (§3.4) only works if September's
   rows cannot be matched to October's. A rotated id sitting next to a rare hardware
   tuple is only pseudo-rotated: join on the tuple and the year-long profile
   reassembles. Without the budget, the rotation is theatre.
2. **It closes the linkage path this project actually has** — the public issue
   tracker. "Crashes on my M2 Ultra, 128 GB, Italian UI, 2026.3 from the dmg" is five
   envelope fields in one sentence, written voluntarily, in public, next to a name.
3. **It decides which legal regime applies.** Data that cannot single anyone out is
   not personal data and most of the GDPR does not reach it; data that can is
   pseudonymous, with the whole apparatus attached — records of processing, DPIA,
   erasure requests, breach notification. For a service hosted by a public research
   institution that distinction is worth more than any individual field.

So the envelope is budgeted in bits, and the budget is part of the schema review:

| Field | Granularity | ≈ bits |
|---|---|---|
| os | macos / windows / linux | 2 |
| os_version | major only | 3 |
| arch | arm64 / x86_64 | 1 |
| app_version | release tag, no build id in the envelope | 4 |
| gpu_family | allowlisted family, else `other` | 5 |
| ram_bucket | 8 / 16 / 32 / 64 / more | 3 |
| cpu_cores_bucket | 4 / 8 / 16 / more | 2 |
| language | language subtag only, no region | 5 |
| channel | dmg / installer / package / source | 2 |
| **total** | | **~27** |

27 bits is ~134 M distinct signatures against a population several orders of
magnitude smaller: on a uniform distribution every single install would be unique.
Real signatures are not uniform — most pile into a handful of modal tuples (current
macOS, arm64, English, latest release), while the singletons live in the tail: older
distributions, unusual memory sizes, rare languages. The bit count is therefore a
**prior, not a mechanism**.

Two controls do the actual work. First, the client sends `other` for anything not on
the shipped allowlist — notably GPU strings, which are otherwise nearly unique and
must never be sent raw. Second, **ingest enforces k-anonymity**, which measures real
occupancy instead of assuming it: a rolling per-month count per envelope signature,
and any signature below k = 50 installs is generalized before it is written to
storage.

The two are not redundant, and their interaction is the real reason to keep the budget
tight: **low entropy is what makes k-anonymity affordable.** At 40-odd bits the
coarsening would fire on a large fraction of records and the platform breakdown would
survive only for the modal configurations — losing precisely the tail where the bugs
are. Spending few bits up front is what lets the threshold almost never trigger.

Note what is *not* in the table: no timezone, no screen resolution, no locale region,
no build id, no CPU model, no GPU driver version — and no country, which *is*
collected, but not here. See §3.6.

### 3.6 Country, without spending bits

Country is worth keeping: it is the only thing that answers which languages and
regions to translate for, and UI language alone does not answer it — an Italian lab
running an English UI is a translation opportunity, an English UI in Ohio is not. But
as an envelope field it is expensive, roughly 6 effective bits once the skew of the
install base is accounted for, and it would push a large share of tuples into
k-anonymity coarsening.

The way out is that **the translation question needs a marginal distribution, never a
join.** Nobody needs to know which filters Japanese users run; they need to know how
many installs are in Japan and what language those installs are set to. So country
never enters the digest:

- the proxy already sees the client IP transiently, for rate limiting (§3.7);
- ingest maps it to a country code in memory, reads `language` and `app_version` from
  the payload, and **increments a counter** in a separate geo table;
- that table holds tallies only — no install id, no session id, no submission id, no
  behavioural data, and no per-row timestamp, because increments are accumulated in
  memory and flushed on a fixed schedule.

There is consequently nothing to join on: the geo table is a set of counts, not a set
of records. Country costs zero bits of the §3.5 budget, the digest never carries it,
and a full database breach yields a row saying "1 842 installs in Japan in August, 61
of them with an English UI" and nothing further. Published country figures get the
same k = 20 suppression as everything else in §8, with small countries rolled into a
region bucket rather than named.

The residual limitation is real and deliberate: country **cannot** be crossed with
filter usage, crash rate or hardware. If that ever becomes a question worth answering
it costs about 3 bits with an 8-region bucket — a decision to take explicitly rather
than drift into (§10).

### 3.7 The IP address is never stored

TLS terminates on a reverse proxy with access logging disabled; the application
receives no `X-Forwarded-For` and writes no client address. The IP exists only in the
proxy's memory for the life of the request, where it is used for rate limiting and
then discarded. The one thing derived from it — a country code — is immediately
reduced to a counter increment and never attached to a digest, as described in §3.6.

### 3.8 Self-hosted, short raw retention, no third party

No SaaS analytics, no error-reporting service, no CDN in front — a third-party
processor would add a data-processing agreement, an international-transfer analysis,
and a party that sees IPs, in exchange for convenience the workload does not need
(see [§7](#7-server-side-sizing)). Raw digests are kept **90 days** and then deleted;
daily rollups, which contain no envelope signature finer than the k-anonymized one,
are kept indefinitely. Server code and schema live in this repository so the claims
here are checkable.

### 3.9 Two channels, and what may ride on each

Collection is split in two, because the questions split in two and so do their
consent requirements.

**Tier 0 — the update check.** Not a small telemetry ping; an update check that
declines to carry anything an update check does not already need. "Is there a newer
build for this platform?" cannot be answered without the version, the OS and the
architecture, which makes those three fields *strictly necessary for a service the
user asked for* — the one footing on which automatic collection stands without
consent, rather than resting on the audience-measurement reading of the ePrivacy
exemption. The server counts what it unavoidably sees:

- payload `app_version`, `os`, `arch`, and nothing else. **No identifier of any kind.**
- the client checks at most once a day, so checks/day ≈ daily active installs — the
  same counting trick as Firefox's ADI and Debian's popcon;
- the server increments counters, as with the geo tallies of §3.6; no rows, no IP.

That yields active installs per day by version and platform, and the **upgrade curve**:
how fast a release propagates and how long old ones linger, which is the whole of
"how long must we support 2026.3". It cannot yield unique installs per month,
retention, or sessions per install — those need an identifier and stay in tier 1.

**Tier 1 — the session digest.** Everything else in this document, opt-in.

**Tier 2 — the local journal.** Richer than either, never transmitted, no consent
needed because nothing leaves: [Local Usage Model](local_usage_model.md).

#### Why the split is worth more than its parts

The census covers very nearly the whole population by version × OS × arch. The digests
cover the same strata at a far lower rate. Dividing them gives the **opt-in rate per
stratum**, and that makes the opt-in data correctable: reweight it so its platform and
version mix matches the census, and the structural skew is gone. Post-stratification
turns the standard objection to opt-in — unknown bias — into measured bias.

It does not fix everything. Skew *within* a stratum, power users being likelier to opt
in than casual ones on the same platform, stays uncorrectable. But a stratum opting in
at 40 % against another at 8 % tells you where to distrust the numbers, which opt-in
alone can never do.

#### The invariant that keeps tier 0 honest

**Tier 0's payload is frozen at what the update check functionally requires. Anything
else — anything at all — goes in tier 1.** The slope is real: once an automatic channel
exists every later question will want to ride on it, each addition individually
reasonable, and the functional-necessity argument survives exactly as long as the
payload stays functionally necessary. A field added here is not a schema change, it is
a change of legal basis.

One dependency, stated plainly: this is free *if* MeshLab ships update checking anyway,
which a desktop application distributed outside app stores generally should. Without
that feature a bare census ping has no functional cover and falls back to the weaker
audience-measurement argument.

### 3.10 Consent

This section governs **tier 1**; tier 0 is covered above, and tier 2 needs no consent.

**Opt-in, off by default.** The alternative — opt-out — yields far better coverage,
and for a project maintained inside a European public research institution it is not
worth defending. The form that works is **prompted choice**: a first-run dialog with
no pre-selected answer, stating the fourteen decisions, linking this document, and
offering a **"Show me exactly what would be sent"** button that renders the real
payload from the current session. Legally that is consent — freely given, specific,
informed — so it satisfies GDPR and ePrivacy Art. 5(3) without leaning on an
exemption, which matters because a public research body cannot fall back on legitimate
interest. Practically it recovers most of the coverage a buried checkbox loses, and it
changes the shape of the selection filter: "answered a dialog" is far less biased than
"went looking in preferences". The same viewer is the best debugging tool the feature
has, and it is also what removes the discovery risk — nobody finds a surprise
connection in a packet capture if they were shown the payload on first launch.

Additional rules: consent is versioned, and a schema change that adds a *category* of
data re-asks; `MESHLAB2_NO_TELEMETRY=1` and the conventional `DO_NOT_TRACK=1` force
off; headless, Python-driven, `--generate-docs` and test runs default to off, because
there is no one present to consent — a CI farm must never be able to skew the data.

The honest cost: an opt-in cohort skews toward engaged users. Absolute counts from
this tier are therefore never a KPI — the census in §3.9 is where install numbers come
from, and the digests are reweighted against it before any cross-platform or
cross-version comparison is published.

## 4. The schema

**Tier 0** is one line, and is shown in full:

```
GET /v1/update?v=2026.11&os=macos&arch=arm64
```

No body, no identifier, no cookie; the response is the update manifest. The server
increments `(app_version, os, arch, day)` and keeps nothing else.

**Tier 1** is one submission, one JSON object, gzipped — built by projecting the local
journal of [Local Usage Model](local_usage_model.md) through the aggregate, never read
from the journal directly (§3.1, and that document's §5). Illustrative rather than
final:

```json
{
  "schema": 1,
  "consent": 1,
  "submission_id": "b1c1…",
  "sample_rate": 1.0,
  "envelope": {
    "app_version": "2026.3", "os": "macos", "os_version": "26",
    "arch": "arm64", "gpu_family": "apple-m", "ram_bucket": 3,
    "cpu_cores_bucket": 2, "language": "it", "channel": "dmg"
  },
  "session": {
    "session_id": "7f3a…", "install_id": "d9e0…",
    "sessions_this_month_bucket": 2,
    "duration_bucket": 4,
    "exit": "clean",
    "prev_exit": "unclean", "prev_exit_filter": "filter_meshing::poisson_disk_sampling"
  },
  "filters": {
    "filter_meshing::taubin_smooth": {
      "runs": 4, "failed": 0, "cancelled": 1,
      "ms_hist":   [0,0,2,2,0,0,0,0],
      "size_hist": [0,0,1,3,0,0,0,0],
      "entry": {"menu": 1, "panel": 3},
      "nondefault": ["stepSmoothNum", "lambda"]
    },
    "filter_clean::remove_duplicate_vertices": {"runs": 2, "failed": 0}
  },
  "errors": {"filter_quadwild::quadrangulate": {"E_SOLVER_FAILED": 2}},
  "io": {
    "ply:in:io_vcg":          {"n": 3, "failed": 0, "size_hist": [0,0,0,2,1,0,0,0]},
    "obj:in:io_obj_rapidobj": {"n": 2, "failed": 0},
    "gltf:out:io_gltf":       {"n": 1, "failed": 1}
  },
  "ui": {"tool.measure": 6, "undo_graph.jump": 2, "render_mode.radiance_scaling": 1},
  "document": {"max_meshes_bucket": 2, "max_faces_bucket": 5, "peak_mem_bucket": 3},
  "preferences_nondefault": ["view.fieldOfView", "render.wireframeOnLoadMaxFaces"]
}
```

Notes on specific choices:

- **Histograms, not values.** Fixed exponential ladders (faces: 1e3, 1e4, 1e5, 1e6,
  1e7, …; ms: 10, 100, 1e3, 1e4, …). A histogram of eight small integers cannot carry
  a fingerprint the way a list of exact runtimes can, and it aggregates trivially.
- **`nondefault` lists parameter ids, never values.** A value could be a path or a
  coordinate; the id answers the question ("is this control dead?") on its own.
- **Errors are codes, not messages.** This requires a codebase change; see §6.
- **`prev_exit_filter`** is the cheapest crash signal that exists and is entirely
  public information: the filter key that was running when the previous session
  failed to shut down cleanly.
- **I/O keys carry the plugin id**, because three plugins claim `obj` and two claim
  `stl`, and `Document::preferredImportPluginForExtension`
  (`src/core/document_io.cpp:97`) makes the winner a user-visible choice. Which
  importer people actually end up on, and whether it fails more than its rivals, is a
  question nothing else answers.
- **No country field.** It is counted at ingest instead, unlinked; see §3.6.
- **`sample_rate`** is present from day one so that client-side sampling can be
  switched on later without invalidating comparisons.

## 5. Client architecture

The instrumentation cost is unusually low here because the existing architecture
already funnels everything interesting through single call sites:

| Signal | Funnel | Covers |
|---|---|---|
| filter runs, timing, success | `Document::runFilter` — `src/core/document_filters.cpp:48` | menu, filter panel, Python API, interactive tools — all of them, and it already measures elapsed ms |
| I/O counts, formats, failures | `Document::loadMesh` / `Document::saveMesh` — `src/core/document_io.cpp:131`, `:600` | every import and export |
| session lifecycle, unclean exit | `MainWindow` ctor/dtor, `src/app/main.cpp` | one flag file written at start, removed on clean quit |
| document shape | `Document` layer add/remove | peak counts and memory |
| feature discovery | `RenderWidget` mode changes, `InteractiveTool` activation | 4 tools, render modes |
| wrong defaults | `Preferences::changed` — `src/core/preferences.h` | 42 preferences |

```
Document::runFilter ──┐
Document::loadMesh  ──┼──> Telemetry::instance()   (src/core/telemetry.{h,cpp})
MainWindow lifecycle──┤        │  in-memory counters, no I/O during the session
RenderWidget / tools──┘        │
                               ├──> digest.json in QStandardPaths::AppDataLocation
                               │       one file per session, queue capped at 20 / 7 days
                               └──> sent.jsonl  (the local transparency log)
                                        │
   next app start ──> TelemetrySubmitter ──> POST https://stats.meshlab.org/v1/ingest
```

Design points worth committing to:

- **`Telemetry` is a `src/core` singleton shaped like `Preferences`** — same
  instance-plus-id-string idiom, no UI dependency, callable from any layer.
- **Submit the *previous* session at the *next* start.** One decision solves three
  problems: the network call never delays quit, a session that crashed still gets
  reported (that is exactly where `prev_exit` comes from), and offline machines simply
  submit later. Multiple queued digests go in one request.
- **Bounded queue.** At most 20 digests or 7 days, oldest dropped, so a machine that
  is offline for a year neither grows a spool nor sends a year of history at once.
- **One dependency added:** `Qt6::Network`. It is the only new external requirement.
- **Kill switch both ways.** A preference, two environment variables, and a
  server response field that can tell clients to back off (bad deploy, schema
  mistake) without shipping a build.
- **Tests assert the privacy invariants.** A test that serializes a digest built from
  a document with real file paths and asserts the bytes contain no `/`, no `\`, and
  nothing matching a path or extension pattern; plus a schema round-trip test and a
  test that the queue cap holds. These are cheap and they are what makes §3.1 a
  mechanism rather than a promise.

## 6. Codebase changes this requires

| Change | Where | Why |
|---|---|---|
| Stable error codes on filter results | `MeshFilterRunResult` — `src/plugins/meshfilterplugin.h:394` | `errorMessage` is a translated free-text `QString`; it can never be sent. Add an `errorCode` enum/interned id alongside it. Useful independently: scripts and tests can branch on a code instead of matching English prose |
| Unclean-exit detection | `src/app/main.cpp` | none exists today; a sentinel file at startup is ~15 lines and unlocks the single best release-gating metric |
| GPU family allowlist | `src/render` | map the driver string to a coarse family; raw strings must never leave |
| Bucket ladders | `src/core/telemetry.h` | shared by client and server rollups; changing one is a schema version bump |
| Consent + viewer UI | `src/ui` | first-run dialog, a preferences page, and a payload viewer |
| Telemetry preferences | `resources/preferences.json` | `stats.enabled`, `stats.consentVersion` — the existing schema renders them with no UI code, per [Preferences](../preferences.md) |
| Update checking | new, `src/core` + `src/ui` | tier 0 (§3.9) is a side effect of this feature, not a feature of its own. Wanted anyway for a desktop app distributed outside app stores; without it the automatic channel loses its justification |

## 7. Server side sizing

### 7.1 Shape

Deliberately boring, because the workload is small and the operational budget of a
research lab is the real constraint:

```
nginx (TLS, no access log, rate limit, 64 KB body cap)
  └─ ingest service (~300 lines, Go or Python): schema validate → k-anon coarsen
       ├─ append to /data/raw/YYYY-MM-DD.ndjson.gz         (deleted after 90 days)
       │    └─ nightly DuckDB job → /data/rollup/*.parquet (kept indefinitely)
       │         └─ static HTML dashboard, regenerated nightly
       └─ in-memory geo tallies, flushed hourly → /data/geo/*.parquet
            (country × language × app_version counts only, no join key — §3.6)
```

No database server, no dashboard service, no queue, no container orchestration. The
rollup tables are small enough to be embarrassing: filter × version × day is
334 × ~8 × 365 ≈ **975 k rows per year**, which DuckDB queries in milliseconds and
which fits in a file you can email. The geo table is smaller still — country ×
language × app_version × month is a few tens of thousands of sparse rows a year.

### 7.2 Traffic model

One submission per session; **average 2.5 KB raw, ~1 KB gzipped on the wire**
(median session touches ~6 distinct filters; p95 ≈ 5 KB raw). Payload hard-capped at
64 KB.

    submissions/month = monthly_active_installs × sessions_per_install × opt_in_rate

| Scenario | Active installs/mo | Sessions | Opt-in | Submissions/mo | Avg req/s | Peak req/s | Raw/mo | Raw for 90 days |
|---|---|---|---|---|---|---|---|---|
| Dogfood | 1 000 | 15 | 40 % | 6 000 | 0.002 | 0.02 | 15 MB | 45 MB |
| Established | 50 000 | 10 | 25 % | 125 000 | 0.05 | 0.5 | 310 MB | 0.9 GB |
| MeshLab-scale | 500 000 | 8 | 25 % | 1 000 000 | 0.4 | 4 | 2.5 GB | 7.5 GB |
| Blowout | 2 000 000 | 8 | 40 % | 6 400 000 | 2.5 | 25 | 16 GB | 48 GB |

Peak assumes a 10× diurnal factor over the average. Gzipped storage is ~40 % of raw.

**Tier 0** is the opposite shape: many more requests, essentially no storage. One
check per active install per day, ~200 B of query string in and an update manifest
out, counted into `(version, os, arch, day)` rows:

| Scenario | Checks/day | Avg req/s | Peak req/s | Inbound/day | Counter rows/year |
|---|---|---|---|---|---|
| Dogfood | 500 | 0.006 | 0.06 | 0.5 MB | ~25 k |
| Established | 17 000 | 0.19 | 1.9 | 17 MB | ~25 k |
| MeshLab-scale | 133 000 | 1.5 | 15 | 133 MB | ~25 k |
| Blowout | 533 000 | 6.2 | 62 | 530 MB | ~25 k |

Checks/day is taken as monthly active installs × sessions ÷ 30, which counts an
install used twice in a day once, as the daily rate limit does. The row count does not
move with scale because it is the product of versions, platforms and days, not of
users — a few tens of thousands of rows a year at any size.

So tier 0 multiplies the request count by roughly four and adds nothing measurable to
storage. TLS handshakes still dominate its cost: 62 req/s at the blowout peak is around
12 % of one core. **The machine in §7.3 does not change.**

### 7.3 What that costs

- **CPU.** Validate + coarsen + append ≈ 100 µs per submission, so even the blowout
  row is 0.25 % of one core. TLS handshakes dominate at ~1–2 ms of CPU each: 25 req/s
  ≈ 5 % of a core. nginx serves thousands of these per second.
- **Memory.** The ingest service is stateless apart from the rolling k-anonymity
  counters — one month of envelope signatures, tens of MB at worst.
- **Disk.** 90-day raw window plus rollups: **< 1 GB** through "Established",
  **~8 GB** at MeshLab-scale, ~50 GB at blowout. Rollups add a few hundred MB/year
  forever.
- **Bandwidth.** MeshLab-scale is ~33 MB/day inbound.
- **Machine.** **2 vCPU / 4 GB / 80 GB SSD** covers everything through
  MeshLab-scale with room to spare; blowout wants 200 GB or a shorter raw window with
  cold archive to object storage. That is a €5–10/month VPS, or a small VM on
  institutional infrastructure.
- **People.** This is the real cost: roughly 2–3 weeks of part-time work to build
  phases 0–2, then a few hours a month — plus the recurring hour per release to review
  the decisions register and delete dead fields.
- **When to reach for ClickHouse:** only if interactive slicing over the *raw* history
  becomes a daily activity, i.e. above ~1 billion fact rows retained. Nightly Parquet
  rollups do not get there.

### 7.4 Abuse resistance

An anonymous unauthenticated endpoint can be poisoned; identity is the only real
defense and it has been given up on purpose, so the goal is bounding the damage:
size cap, schema validation, plausibility caps (a session cannot run 10⁶ filters),
per-IP rate limiting at the proxy (transient use of an IP, never stored),
`submission_id` deduplication, and reporting that leans on medians, trimmed means and
version-to-version *distribution shifts* rather than absolute totals. A shared HMAC
key compiled into official builds raises the effort slightly and should be understood
as obfuscation, not authentication.

## 8. Giving the data back

The community supplies the data, so the community gets the data. Monthly, publish:

- a static dashboard with filter usage, failure rates, version adoption, platform mix
  and a country/region map;
- machine-readable aggregate CSVs, with any cell below k = 20 installs suppressed;
- the schema, the ingest code, and the decisions register — in this repository;
- the filter and transition counts as a small table shipped inside the application,
  where they become the cold-start prior for every install's local predictor
  ([Local Usage Model](local_usage_model.md) §4) — contributed data coming back as a
  feature that works for everyone, including those who did not contribute.

This is also what makes the feature defensible: a user who wonders what is collected
can read the schema, press the button that shows their own payload, and then look at
exactly what everyone's payloads became.

## 9. Phasing

| Phase | Content | Why in this order |
|---|---|---|
| 0 | The local journal and the aggregate projected from it, payload viewer. **No network code at all.** Shared with [Local Usage Model](local_usage_model.md) phase 0 | Schema mistakes are cheap now and expensive after collection starts. Dogfood a full release cycle, then delete the fields nobody looked at |
| 1 | Tier 0: update checking, census counters, the manifest endpoint | Independent of the consent question, so it need not wait for it, and it produces the denominators that phase 3 reweights against |
| 2 | Tier 1: consent flow, `Qt6::Network` submitter, ingest service, raw storage | Smallest thing that produces behavioural data |
| 3 | Nightly rollup, post-stratification against the census, public dashboard, published CSVs | Closes the loop; §8 is what earns the opt-ins |
| 4 | Error codes, unclean-exit signal, runtime/size histograms | Needs the `MeshFilterRunResult` change and is worth its own review |
| 5 | Sampling, k-anonymity tightening, cold archive | Only if scale demands it; the model in §7.2 says it may never |

## 10. Open decisions

1. **Tier 1's consent model** — prompted choice (recommended, assumed throughout) vs.
   opt-out with a prominent first-run notice. The tiering of §3.9 lowers the stakes:
   the census already carries the install and version numbers, so what opt-out would
   buy is behavioural coverage, against an open-source backlash risk that has little
   to do with the actual privacy properties.
2. **Whether tier 0 ships at all**, which is really the question of whether MeshLab
   ships update checking. If it does not, the automatic channel should be dropped
   rather than justified some other way.
3. **Identity model** — session-only (maximum privacy, cannot separate heavy users
   from many users) vs. monthly rotating install id (recommended) vs. a stable
   install id (rejected here).
4. **Hosting** — institutional VM vs. commercial VPS. Both are adequate; the
   institutional route affects who the data controller is.
5. **Whether country may ever be crossed with behaviour.** Assumed no: the marginal
   distribution in §3.6 is free, but a join needs country inside the envelope, which
   costs ~3 bits even with an 8-region bucket and re-opens the k-anonymity question.
6. **GPU driver version.** Currently excluded as 4–6 bits. It is also the difference
   between "Radeon users have problems" and "this driver branch has problems", which
   for a renderer-heavy application may be worth buying.

## Related

[Local Usage Model](local_usage_model.md) is the other half of the design: the same
actions, kept in full on the user's own machine and used to help them, with this
document's digest as a lossy one-way projection of it.

`Telemetry` is deliberately shaped like [`Preferences`](../preferences.md) — a `src/core`
singleton over a declared schema — so that adding a counter is a schema entry plus one
call, and so that the same JSON-declared-parameters machinery can render the consent
and settings UI without new widget code.
