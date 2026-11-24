# Local Usage Model

A persistent, local record of what the user does in MeshLab, and the small statistics
built on it: search results ranked by what this user actually runs, parameter defaults
that learn, a runtime estimate before a filter starts, a suggested next step, and
recognition of a pipeline being repeated. **Not implemented.** Nothing in it is ever
transmitted — the journal and every model derived from it stay on the machine that
produced them; the one-way relationship with the data that *does* leave is in
[Usage Statistics](usage_statistics.md).

See also: [Adding a Filter](../adding_a_filter.md) · [User Interface](../user_interface.md) ·
[Preferences](../preferences.md) · [Filter Search](filter_search.md)

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

## 5. The boundary with Usage Statistics

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

## 6. The journal is still a privacy surface

Nothing is transmitted, but the file records what the user worked on, by name. On a
shared machine, in a cloud-synced home directory, or in a backup, that is worth treating
seriously:

- a preference to disable it, which stops recording rather than merely hiding it;
- *Clear usage history*, which deletes the journal and every derived model;
- documented location, plain-text format, and no obfuscation — the user can read their
  own file, and the `pythonCall` strings make it genuinely useful to them;
- never collected in headless, Python-driven or `--generate-docs` runs, for the same
  reason the statistics are not: there is no one present whose habits these are.

## 7. Phasing

| Phase | Content | Depends on |
|---|---|---|
| 0 | The journal: record, write, rotate, clear, preference | nothing |
| 1 | Search ranking (§3.1) and learned parameter defaults (§3.2) | phase 0 |
| 2 | Runtime estimates (§3.3) | phase 0 |
| 3 | Next-filter suggestion (§3.4) with a shipped prior | phase 0, and Usage Statistics phase 2 for a real prior |
| 4 | Repeated-pipeline detection (§3.5) | phase 0 |

Phases 1 and 2 are small and independent; 3 and 4 are where the UI questions are, and
neither is required for the first two to be worth shipping.

## 8. Open questions

1. **Names.** *Usage history*, *Clear usage history*, the "next" row — all user-visible,
   so they need [Vocabulary](../vocabulary.md) approval rather than being invented here.
   The constraint is distinctness from *Action History*, which already means something
   else.
2. **How much UI the suggestions deserve.** A row in the filter panel is the conservative
   answer; anything more prominent risks being the feature users turn off first.
3. **Whether the journal should be user-exportable** — probably yes, since it is a mine of
   ready-made `pymeshlab2` scripts, but that makes its format a compatibility surface.
4. **Whether learned defaults apply silently or on confirmation.** Silent is smoother and
   harder to debug; the marker in §3.2 is the minimum either way.

## Related

[Usage Statistics](usage_statistics.md) is the other half: the same actions, aggregated
past the point of identification, and the only part that is ever transmitted.
[Filter Search](filter_search.md) owns the ranking that §3.1 extends.
