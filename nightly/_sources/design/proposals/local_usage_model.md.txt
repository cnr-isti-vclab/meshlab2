# Local Usage Model

A persistent, local record of what the user does in MeshLab, and the small statistics
built on it: search results ranked by what this user actually runs, parameter defaults
that learn, a runtime estimate before a filter starts, a suggested next step, and
recognition of a pipeline being repeated. **Not implemented.** Nothing in it is ever
transmitted — the journal and every model derived from it stay on the machine that
produced them; the one-way relationship with the data that *does* leave is in
[Usage Statistics](usage_statistics.md).

See also: [Adding a Filter](../adding_a_filter.md) · [User Interface](../user_interface.md) ·
[Preferences](../preferences.md) · [Filter Search](../user_interface.md#filter-search)

## 1. Why this is mostly plumbing

Almost everything needed is already computed and then thrown away. The new subsystem is
a store and a few estimators, not a new source of data:

| Piece | Where | What it already provides |
|---|---|---|
| `ScriptAction` | `src/core/document_undo_types.h:38` | `kind`, `filterKey`, `params`, `filePaths`, `pythonCall`, `compactPythonCall` — the reproducible `pymeshlab2` equivalent of an action, already built for every undo node |
| Filter timing | `Document::runFilter`, `src/core/document_filters.cpp:48` | elapsed ms for every run from every entry point, already measured and already written to the log |
| Session parameter memory | `MeshFilterPanel::forgetRememberedParameters()`, `src/ui/meshfilterpanel.h:41` | the panel already remembers parameters — for one session, as one value, with no statistics |
| Search ranking | `SearchRank`, `src/ui/filterpresentation.h:29` | a lexicographic `(level, inexact, position, nameWords)` tuple that one more key extends |
| Scale-relative parameters | `resolveToken`, `src/plugins/filterdescriptorloader.cpp:393` | the `@bboxDiag`, face-count and quality-range token families: the existing way a value is expressed relative to the mesh rather than in absolute units |

So the work is: persist what `ScriptAction` already holds, count it, and feed four places
that already exist.

## 2. The journal

An append-only local file of action records. One record per action, written as it
happens, holding:

| Field | Why |
|---|---|
| `when` | local timestamp — this never leaves the machine, and recency is half the signal |
| `kind`, `filterKey` | what was done |
| `params` | the values as applied, **plus the ratio to the bound quantity** for any parameter whose descriptor declares a `@bboxDiag`/face-count token — the ratio is the part that transfers to the next mesh, the absolute number is not |
| `scale` | face count, vertex count and bbox diagonal of the input at the time, so cost and parameters can be conditioned on size |
| `elapsedMs`, `outcome` | `ok` / `failed` / `cancelled`, from the value `runFilter` already has |
| `entry` | menu, panel, search, Python, tool — which surface was used |
| `docId` | a random per-document id, so sequences can be segmented into pipelines without ever naming the document |
| `paths`, `format`, `ioPlugin` | for load/save actions only |

**This is not the Action History.** The [Action History view](../user_interface.md#the-action-history-view)
is per-document, in memory, and about the states of *this* document; it answers "where
am I and how did I get here". The journal is cross-document, persistent, and about
habits; it answers "what does this user usually do". Different lifetime, different
question, no shared storage — and the names must stay distinct in the UI for the same
reason.

Location is `QStandardPaths::AppDataLocation`, one rotating file, capped by size. A busy
day is a few hundred actions at well under a kilobyte each, so a 50 MB cap holds years
of heavy use; rotation drops the oldest file, and every model is rebuilt by a single
pass over what remains.

## 3. What it predicts, cheapest first

The order is deliberate: each stage is useful alone, and the first two are worth doing
even if the rest is never built.

### 3.1 Frequency and recency in filter search

Append one key to `SearchRank`'s comparison tuple: a bucketed score combining how often
and how recently this user ran the filter. 423 filters means many queries tie on the
existing four keys, and today those ties break arbitrarily.

**Deliberately a tie-break only, never a reorder across levels.** A usage score that can
promote a filter past a better textual match makes search unstable and unexplainable —
the same query stops giving the same answer, and the user cannot tell why. The same rule
rules out adaptive menus: menus never reorder, ever. Ranking applies only where the user
has already typed a query and is asking to be sorted.

Cost: one field, one comparison, one counter table. This is the whole stage.

### 3.2 Parameter defaults that learn

Generalize the panel's session memory into a persistent, per-filter estimate: the mode of
previous values for discrete and boolean parameters, the median for continuous ones —
and for any parameter with a bound token, the median **ratio**, re-resolved against the
current mesh. That last part is what makes a remembered value survive a change of
subject: a user who always simplifies to about a tenth of the input faces gets that
intent carried over to a mesh ten times bigger, where remembering the absolute number
would be actively wrong.

Needs a visible marker that a default came from history rather than from the descriptor,
and a one-click way back to the declared default — a learned value that cannot be
identified or undone is a bug report waiting to happen.

### 3.3 A runtime estimate before the run

Most of the cost here is already paid: `runFilter` measures every run. Start
non-parametric — the median elapsed time of this user's nearest runs of this filter in
log(face count) — and show a range, never a point, because the spread *is* the honest
uncertainty. A log-log least-squares fit of time against size is the refinement once
there are enough points to justify it.

The known limitation, worth stating before someone is surprised by it: for filters whose
cost is dominated by a parameter rather than by mesh size (iteration counts, sample
counts, solver tolerances) size alone predicts badly. Key those models on
(filter, coarse parameter bucket) or leave them out; a confidently wrong ETA is worse
than none.

### 3.4 The next filter

A first-order Markov model over `filterKey`: count the pairs the user produces, and
suggest the likely continuations of the action just completed. With additive smoothing
against a shipped prior,

    p(next | cur) = ( c_user(cur, next) + α · p_prior(next | cur) ) / ( c_user(cur) + α )

α around 5–10 means the user's own habits take over after a few dozen observations of a
context while new installs still get sensible suggestions on day one.

Surfaced in one dedicated place — a short "next" row in the filter panel — and nowhere
else. Suggestions that move existing controls are the failure mode of this whole
category of feature.

### 3.5 A pipeline being repeated

Count n-grams of consecutive actions within a `docId`. When a sequence of three or more
recurs often enough, offer to save it: the `compactPythonCall` strings are already stored
per action, so the generated script is a concatenation, not a code generator. This lands
in the script editor and Python console that already exist, which is most of why it is
cheap.

## 4. Cold start, and where the prior comes from

Stages 3.4 and 3.5 need data a new install does not have. The prior that fills the gap is
the **published aggregate** from [Usage Statistics](usage_statistics.md) §8 — the filter
and transition counts from the opt-in cohort, shipped as a small read-only table in the
application resources and refreshed at each release.

That closes the loop the statistics proposal promises in the abstract: data contributed
by the users who opted in comes back as a feature that works for everyone, including
those who did not, and no individual's data moves anywhere to make it happen.

## 5. Seeing it

Three different jobs hide under "show the user their data", and conflating them is how
this becomes a dashboard nobody opens twice:

- **transparency** — *what does MeshLab know about me?* Complete, plain, verifiable.
- **self-knowledge** — *what do I actually do?* Worth building only where it answers a
  question a user would really ask.
- **action** — *turn what I did into something I can run again.* Where most of the value
  is, and what makes the subsystem read as a tool rather than as surveillance.

### 5.1 Session recap, as a script

"Your last session: 23 actions across 3 documents", with the reconstructed
`pymeshlab2` script and one click into the script editor (`src/ui/scripteditor.h`).
Almost entirely reuse: `compactPythonCall` is already stored per action. Offered when
asked for and at the end of a session, never as a modal.

This is the view to build first. It makes the journal useful before a single statistic
exists, which is also the honest order to earn the feature's keep.

### 5.2 Per-document provenance

The journal filtered to one document: every session in which this mesh was worked on,
and exactly what was applied, in order.

For the cultural-heritage and archaeology users this is not a convenience. Documenting
what processing a scan has been through is a publication requirement, and today that
record lives in somebody's notebook or nowhere. Reachable from the layer's context menu
and from a `.mlp` project, and exportable (§6) as the provenance paragraph of a methods
section.

### 5.3 Your filters, against everyone's

A ranked list of what this user runs — whose interesting column is the comparison with
the shipped prior of §4: *people who use the filters you use also run X often, and you
have never run it.*

Across 423 filters, discovery is a real problem, and this is a discovery mechanism
wearing a statistics view's clothes. It needs no server: the prior is in the application
resources. It is also the view that most needs k-anonymity to have been done properly
upstream — a comparison is only publishable as a prior if no cell in it is traceable to
a handful of people.

### 5.4 Repeated pipelines

The n-grams of §3.5 as a list: the sequence, how often it recurred, and *Save as script*
on each row. Selecting rows and re-running them against the current layer is the same
machinery pointed the other way, and is the most direct answer to "what is this journal
for".

### 5.5 Where the time goes

Count × median runtime per filter, ranked. Answers *what is actually slow for me*, which
is a different question from what is slow in general, and it leads somewhere — simplify
before running that one. The numbers are already measured in `runFilter`.

### 5.6 Parameter drift

For one filter, the values this user has used for each parameter against the declared
default. "You have changed `targetFaceNum` every single time" is precisely the signal
that tells a maintainer a default is wrong; shown to the user, it tells them they could
pin their own — so the view carries *Make this my default*, which writes the learned
value of §3.2 as an explicit preference rather than an inferred one. For parameters with
a bound token the pinned value is the ratio, so it keeps working on the next mesh.

### 5.7 The transparency table

Mandatory, and deliberately plain: the journal as rows, newest first, filterable, with
record count, date range, *Clear* and *Export* — in Preferences, beside the switch,
because that is where someone goes to check.

The version that actually convinces people shows **both sides**: what is kept locally,
and beside it the aggregate that would be transmitted under
[Usage Statistics](usage_statistics.md). That diff is the most persuasive privacy
artifact this project can produce, and it is the same content that document's consent
dialog renders (its §3.10), so it is built once.

### 5.8 Every suggestion explains itself

Any prediction surfaced anywhere — a reordered search result, a learned default, a
suggested next filter — carries a one-line *why* ("because you ran this after that 7
times") that links into the journal view filtered to exactly those records.

This is what separates a helpful prediction from an arbitrary one, and once the journal
view exists it is nearly free. A model whose output cannot be traced back to the records
that produced it should not be shipped.

### 5.9 What not to build, and where these live

**Not** activity heatmaps, calendars, totals or streaks. Fun once, then dead, and they
drag the feature toward vanity metrics — which is also what makes users suspicious of a
thing that is watching them.

The transparency table lives in Preferences. The exploratory views share **one dialog**
reached from a menu, not a dock: they are not consulted while working, and a dock costs
screen space permanently. `src/ui/filterpluginsinfodialog.h` is the existing precedent
for a browse-a-table dialog. The session recap is contextual; the next-filter suggestion
stays in the filter panel where §3.4 puts it.

Most of these are ranked lists and tables and should stay so. Where a distribution
genuinely needs plotting — §5.6, perhaps §5.5 — note that `external/jkqtplotter` is
already a submodule, added `EXCLUDE_FROM_ALL`, with only `JKQTMathText6` linked today
(`CMakeLists.txt:334`). Linking `JKQTPlotter6::JKQTPlotter6` is one more line, so a real
chart costs a CMake line and not a new dependency. Tables first regardless.

## 6. Exporting it

Four artifacts, four different jobs:

| Artifact | For | Notes |
|---|---|---|
| The journal, JSONL | transparency, the user's own tooling, seeding another machine | **the stored file itself**, not a re-encoding, so "what is kept" and "what I exported" are provably the same bytes |
| A `pymeshlab2` script | re-running a session, a selection, or one document's history | the one real decision is paths — see below |
| CSV aggregate | pivoting in a spreadsheet: per filter, count, median ms, failure rate | costs nothing to allow, and someone always wants it |
| The contribution bundle | "exactly what would be sent", as a file | see below |

**Paths in an exported script.** Offer *paths as arguments* alongside the literal form:
a script whose inputs are parameters is reusable against other meshes, while one with
absolute paths baked in runs on exactly one machine and quietly carries a directory
layout to whoever receives it. Default to parameterized.

**The contribution bundle** is the interesting one. Beyond being the honest version of
the consent preview, it is an **air-gapped contribution path**: a lab with no outbound
connectivity — common enough in heritage and archaeology — can inspect an aggregate and
send it by hand. That reaches the cohort that will never enable a network feature at all,
and it costs nothing extra, because the bundle is the payload the digest builder already
produces.

**One caution worth a line in the export dialog:** an exported journal is the same data
outside the application's control, and the script export in particular carries file
paths by construction. Say so once, at the point of export, and not again.

## 7. The boundary with Usage Statistics

The journal holds file paths and real parameter values. Locally that is correct — it is
the user's own work — and in a transmitted payload it would be a disaster. So the
relationship is one-way and lossy by construction:

```
journal (paths, values, timestamps, local only)
   └─ aggregate (counts, histograms, ids from known vocabularies)
        └─ digest (allowlisted, bucketed, stripped)  ──> transmitted, opt-in
```

The digest builder takes the **aggregate**, never the journal. Enforced structurally
rather than by review: separate targets, with the telemetry target not depending on the
journal target, plus the serialization test in
[Usage Statistics](usage_statistics.md) §3.1 that asserts no path-shaped substring can
appear in a payload.

A useful side effect: the journal is what makes the digest testable, and Phase 0 of the
statistics proposal — local collection, no network code — is exactly this journal plus a
projection.

## 8. The journal is still a privacy surface

Nothing is transmitted, but the file records what the user worked on, by name. On a
shared machine, in a cloud-synced home directory, or in a backup, that is worth treating
seriously:

- a preference to disable it, which stops recording rather than merely hiding it;
- *Clear usage history*, which deletes the journal and every derived model;
- documented location, plain-text format, and no obfuscation — the user can read their
  own file, and the `pythonCall` strings make it genuinely useful to them;
- never collected in headless, Python-driven or `--generate-docs` runs, for the same
  reason the statistics are not: there is no one present whose habits these are.

## 9. Phasing

| Phase | Content | Depends on |
|---|---|---|
| 0 | The journal: record, write, rotate, clear, preference | nothing |
| 1 | Search ranking (§3.1) and learned parameter defaults (§3.2) | phase 0 |
| 2 | Runtime estimates (§3.3) | phase 0 |
| 3 | Next-filter suggestion (§3.4) with a shipped prior | phase 0, and Usage Statistics phase 3 for a real prior |
| 4 | Repeated-pipeline detection (§3.5) and its view (§5.4) | phase 0 |
| 5 | Session recap (§5.1), the transparency table (§5.7), JSONL and script export (§6) | phase 0 |
| 6 | Per-document provenance (§5.2), time breakdown (§5.5), parameter drift (§5.6) | phase 0; §5.6 also phase 1 |
| 7 | Comparison against the prior (§5.3), the contribution bundle (§6) | Usage Statistics phase 3 |

Phases 1 and 2 are small and independent; 3 and 4 are where the UI questions are, and
neither is required for the first two to be worth shipping. Phase 5 is the one to pull
forward if the feature needs to justify itself early — §5.1 and §5.7 together are what
make the journal read as a tool the user owns rather than as a record kept about them.

## 10. Open questions

1. **Names.** *Usage history*, *Clear usage history*, the "next" row — all user-visible,
   so they need [Vocabulary](../vocabulary.md) approval rather than being invented here.
   The constraint is distinctness from *Action History*, which already means something
   else.
2. **How much UI the suggestions deserve.** A row in the filter panel is the conservative
   answer; anything more prominent risks being the feature users turn off first.
3. **Whether exporting the journal verbatim (§6) pins its format** as a compatibility
   surface. Exporting the stored file unchanged is what makes the transparency claim
   checkable, and it is also a promise about the schema; a version field in every record
   is the cheap hedge.
4. **Script export defaults** — parameterized paths or literal ones, and whether a
   provenance export (§5.2) should be prose for a methods section rather than code.
5. **Whether the views justify linking the plotter** (§5.9), or whether tables carry
   all of them. Least code says tables; §5.6 is the one view with a real case for a
   distribution.
6. **Whether learned defaults apply silently or on confirmation.** Silent is smoother and
   harder to debug; the marker in §3.2 is the minimum either way.

## Related

[Usage Statistics](usage_statistics.md) is the other half: the same actions, aggregated
past the point of identification, and the only part that is ever transmitted.
[Filter Search](../user_interface.md#filter-search) owns the ranking that §3.1 extends.
