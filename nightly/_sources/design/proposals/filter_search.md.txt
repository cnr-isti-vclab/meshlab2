# Filter search: word starts, synonyms and ranking (draft, 2026-10-30)

**Status: implemented 2026-10-30**, in `FilterPresentation` (`src/ui/filterpresentation.cpp`),
which the filter panel and the Filter Plugins Info dialog share; the reference queries and the
alias coverage are `searchRanksTheReferenceQueries` and
`searchAliasesCoverTheRejectedSynonyms` in `test_filter_descriptors.cpp`. One change from
the draft: the alias table is a C++ table beside the matcher, not a JSON resource, and
vocabulary.md lists the noun aliases. The preferences dialog and the parameter search still
use their own substring matching.

## 1. What is wrong today

`FilterPresentation::matches` keeps a filter when every typed term occurs *anywhere* in its
name, Python name, descriptions, plugin, categories or tags, inside a word or not. The
panel then shows the filters whose **name** contains every term (`titleMatchesAll`, the same
substring rule), then the rest, each group in registry order. Measured on the 400 filters:

- **Inside-word matches flood the results.** `tex` matches 217 filters, because *ver**tex***
  contains it; `del` matches *mo**del***. For `set tex`, *Set Vertex Color* counts as a full
  name match exactly like *Set Texture*, and comes first only because it is registered first.
- **Rejected synonyms find nothing.** The vocabulary keeps one word per concept, so the word
  a user brings is often the one the names avoid: `delete` misses 11 of the 19 *Remove …*
  filters, `position` misses the 42 that say "coordinates", `denoise` finds none of the 47
  smoothing filters, `texcoord` none of the 47 that say "UV".
- **Ranking has two levels.** Inside each, there is no order but the registry's.

People type the starts of words -- `del tex`, `vert coord expr` -- so the fix has to work on
prefixes, synonyms included.

## 2. Matching

- Text is split into **words** at anything not a letter or digit (`remove_textures`,
  `Texture/Assignment` and `Per-Wedge` all split), lowercased. The typed query is split the
  same way.
- A term **matches a word** when it is the start of that word: `vert` matches *vertex*,
  `coord` *coordinates*. Every term must match somewhere.
- A term of **three letters or more** that is the start of an **alias** in the synonym table
  (section 4) also matches the alias's targets: `del` starts *delete*, so it also matches
  *remove*. Shorter terms do not expand -- `de` would reach delete, denoise and decimate at
  once.
- A term may also match by **word form**: dropping one common ending (-ing, -ion, -es, -ed,
  -e, -s) when at least four letters remain, so `decimate` matches *decimation* and
  `smoothing` *smooth*. Without it, *Simplify by Decimation* matched `decimate` only through
  the synonym and tied with every other *Simplify* filter.
- **Inside-word matches** (today's rule) are kept as the last resort, so nothing found today
  disappears; they only rank last.

## 3. Ranking

Each term is scored by its best match. The filter's **level** is that of its *worst* term:

1. every term starts a word of the **name**;
2. … some only through a **synonym**, still in the name;
3. … in the **tags or categories** (or the Python name);
4. … in the **descriptions**;
5. … only **inside** a word.

Within a level:

1. an **exact** word beats a prefix, which beats a word form (`set` = *Set* over *Settings*);
2. matches **earlier** in the name, and in the typed **order**, rank higher;
3. **fewer extra words** in the name rank higher (*Set Texture* over *Set Texture Atlas*, were
   there one);
4. registry order breaks what is left, so the list is stable.

The ranking lives in `FilterPresentation`, so the filter panel and the Filter Plugins Info
dialog share it; the preferences dialog and the parameter search can take the matching rule.

## 4. Synonym table (draft)

An **alias** is a word a user may type; it adds its **targets**, never the reverse. The
direction is the point: `position` reaches *coordinates*, but `coordinates` does not reach
*position*, since texture coordinates would then flood a search for positions. A synonym
match ranks below a direct one (level 2, not 1), so an alias that is also a real word in some
name costs little.

**Source.** The verb rows come from the "Rejected synonyms" column of
[vocabulary.md](../vocabulary.md) §3, and a test fails when a rejected synonym there has no
alias to its verb, so the two cannot drift. The noun rows are vocabulary.md's section "Search
aliases". Added while implementing, because the test asked for them: `assign` → *set* and
`conform` → *embed*, both rejected in §3.

### Verbs (from vocabulary.md §3)

| typed | also matches | note |
|---|---|---|
| delete, erase, discard, purge | remove | |
| generate, make, build | create | |
| add | create | `Add` is also an admitted verb (*Add Noise*); a direct match still ranks first |
| rebuild | reconstruct, remesh | |
| resample | sample, remesh | |
| fix, heal, clean | repair | |
| weld, unify, join | merge | |
| pick, mark | select | |
| calculate, recompute, estimate | compute | `Estimate` is also admitted narrowly |
| inspect, report, analyze, analyse, info, stats | measure | `stats` is not in §3; added (2026-10-30) |
| paint, colourise, colourize | colorize, color | |
| bake | transfer, freeze | |
| move | translate, transform | |
| shift | translate | |
| turn | rotate | |
| resize | scale | |
| deform | transform, displace | |
| perturb | displace | |
| commit | freeze | |
| register, registration, fit | align | |
| decimate, decimation, reduce | simplify | |
| retriangulate | remesh | |
| denoise, fair, blur, relax | smooth | |
| unwrap, parameterize, parameterization | parametrize, parametrization | |
| flatten | parametrize, transfer | |
| layout | pack | `Bin` from §3 left out: it would match *binary* |
| cast | convert | |
| clone | duplicate | |
| separate, detach | split, extract | |
| reorient, re-orient | orient | |
| fill, patch, cap | close | "fill holes" → *Close Holes* |
| load, read | import | |
| save, write | export | |
| relabel | rename | |
| draw, snapshot, screenshot | render | |
| insert, imprint | embed | |

Dropped on purpose (2026-10-30), and listed as exceptions by the test that checks §3: `copy`,
`map`, `push`, `pull` and `apply` -- each also means something else common enough to flood a
search. Also left out of the verb rows: `Translate` (rejected for *Convert*, but an admitted
verb itself), `Flip` and `Split` (admitted verbs that §3 reserves), `Project` and `Define`
(admitted narrowly) -- each already matches its own filters directly.

### Nouns and spellings (new)

| typed | also matches | note |
|---|---|---|
| position, positions | coordinates | one way; see the direction rule above |
| quality | scalar | vocabulary: `scalar` user-facing, `quality` in code |
| texcoord, texcoords, uvs | uv | "texture coordinates" already matches by its words |
| colour, colours | color | |
| triangle, triangles, tri | face | |
| photo, photograph, picture | raster, image | without *image*, `load photo` ties *Import Cameras to Visible Rasters* with *Import Image to Current Raster* |
| image | raster | |
| model, object | mesh | |
| bbox, aabb | bounding box | `aabb` is already a tag on two filters |
| pointcloud, points | point cloud | |
| isoline, contour | isocontour | |
| hole, holes | boundary | |

## 5. Reference queries

A Python prototype of sections 2 to 4 gives the expected first result for all 24 (it is how
the word-form rule and the *photo* → *image* target were found). The test pins the **first** result of each. Every query here has one right answer under the
rules above; queries whose best answer is a tie between backends (`ball pivot`, `icp`) are
left out on purpose, or pinned to a backend word.

| query | first result | exercises |
|---|---|---|
| set tex | Set Texture | word starts (not *ver**tex***) |
| rem tex | Remove Textures | word starts |
| del tex | Remove Textures | prefix-triggered synonym |
| vert coord expr | Compute Vertex Coordinates by Expression | prefixes |
| vert pos expr | Compute Vertex Coordinates by Expression | directional noun synonym |
| delete selected faces | Remove Selected Faces | synonym; fewer extra words beat *… and Vertices* |
| fill holes | Close Holes | verb synonym |
| close holes | Close Holes | exact |
| decimate | Simplify by Decimation (TrueForm) | direct name match beats the synonym ones |
| weld vertices | Merge Close Vertices | verb synonym |
| erase current layer | Remove Current Mesh Layer | verb synonym |
| invert sel | Invert Selection | prefixes |
| sel border | Select Border | prefixes |
| merge layers | Merge Visible Layers | exact |
| load cameras | Import Cameras to Visible Rasters | verb synonym |
| save cameras | Export Cameras from Visible Rasters | verb synonym |
| screenshot | Render from Render-State JSON | verb synonym |
| unwrap xatlas | Parametrize by Atlas (xatlas) | synonym plus backend word |
| register icp vcg | Align by ICP (vcglib) | synonym, prefix of *vcglib* |
| denoise point cloud normals | Smooth Point Cloud Normals | verb synonym |
| texcoord distortion | Compute UV Distortion | noun synonym |
| colour vert by quality | Colorize Vertices by Scalar | spelling and noun synonyms, prefix *color* of *colorize* |
| load photo | Import Image to Current Raster | verb and noun synonyms |
| uv seams polyline | Create Polyline from UV Seams | order does not have to match |
| remove_textures | Remove Textures | Python name, split at `_` |

## 6. Decisions (2026-10-30)

1. `copy`, `map`, `push`/`pull` and `apply` are dropped; `commit` stays for *freeze*.
2. `stats` is an alias of *measure*.
3. `tri` → *face* stays; no `pc` alias (two letters never expand).
4. `hole` → *boundary*.
5. Queries whose best answers differ only in backend (`quadric`, `icp`, `ball pivot`, where
   registry order decides) are checked as a group: the first *N* results are those filters,
   in any order.
