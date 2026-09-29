# Selection scope — audit and migration

**Applied 2026-09-28.** Every filter that can confine itself to the selection now declares
a `selectionScope` instead of a toggle of its own, and the framework injects the one
`selectedOnly` parameter, presents it as a scope control in the filter panel, refuses an
empty restriction, derives a missing kind of selection and gives the user's selection back.
How that works today is in [Adding a Filter](../adding_a_filter.md) and the label ruling in
[Vocabulary](../vocabulary.md) §4; this is the record of what was found and changed.

## What the audit found

Fifty filters can restrict themselves to the current selection. The restriction is a
framework-level idea — *which part of the mesh does this run on* — but every plugin
declares it by hand, and no two plugins agree on how:

| | |
|---|---|
| Parameter ids | 9 for one concept: `onselection`, `onSelected`, `onselected`, `Selected`, `SelectedOnly`, `onlySelected`, `SelectionOnly`, `selection`, `selectedOnly`. The id [Filter Organization](../filter_organization.md#parameter-naming-rules) already names, `selectedOnly`, is used by 2 filters. |
| Labels | 11: *Only on selection* (13), *Only On Selection* (11), *Affect only selected faces* (7), *Affect only selection* (5), *Selection only* (4), *Only on Selection* (2), *Simplify only selected faces* (2), *Selected Only* (2), *Remesh only selected faces*, *Flip only selected faces*, *Close holes with selected faces*. Only 12 of them say which kind of element they mean. |
| Placement | 8 behind *Show advanced parameters* — the five vcglib smoothers, *Adjust Vertex Color Levels*, and both sampling filters — although the toggle decides what the filter touches at all. |
| Default | 13 switch on when faces are selected (`@hasSelectedFaces`); 36 are always off. None uses `@hasSelectedVerts`, although 24 read the vertex selection. |
| Empty selection, toggle on | Three different outcomes: refuse with a message (expression, sampling, trioptimize), succeed having done nothing (color, subdivision, simplification, smoothing), or process the **whole** mesh (two unsharp smoothers, face flip). |
| Behaviour | 2 toggles are never read, 1 filter restricts silently with no toggle, 1 is ignored in one branch, 9 "only selected" filters still change unselected elements, and 18 change the user's selection as a side effect. See [Defects and their fixes](#defects-and-their-fixes). |

The labels are the visible symptom; the table below shows that the behaviour behind them
is no more consistent.

## Decisions

Taken by the maintainer on the audit (2026-09-28):

- **The framework version**, not a manifest-only normalization.
- **Default on when selected**: omitted, `selectedOnly` is on exactly when there is
  something to confine the filter to.
- **Only the selection is affected.** A filter that moves vertices under a face scope moves
  only the interior ones, or it would change unselected faces; a refinement leaves the
  edges of unselected faces alone, so only edges inside the selection are subdivided.

What followed from that rule rather than from a separate ruling:

- **Cross-kind selections are derived strictly, both ways**: a vertex scope with only faces
  selected works on the vertices whose faces are all selected (a rectangle selects faces by
  default, so this is the everyday case for the color filters), and a face scope with only
  vertices selected on the faces whose vertices all are. The derivation lasts one run.
- **MLS projection is vertex-scoped** (its usual input is a point cloud); its optional
  refinement refines the faces whose vertices are all selected and projects the new
  vertices too.
- ***Close Holes*** keeps its own `Selected`: it chooses which holes to fill, it does not
  confine an operation, and the tier-1 rule allows it by name.

## Mapping table

One row per filter. *Reads* is what the code actually honours, from reading it:
**V** vertex selection, **F** face selection, **E** edge selection, **F→V** the strict
conversion to the vertices whose faces are all selected (`VertexFromFaceStrict`, which
also **erases** any vertex selection first, `update/selection.h:405`). *Target* is the
`selectionScope` each filter now declares. Every current row is `group: main`, default `false`, unless
marked **adv** (advanced) or **@f** (`@hasSelectedFaces`).

Plugin paths: `cpj` color_projection, `cpc` colorproc, `exp` expression, `msh` meshing,
`mls` mls, `smp` sampling, `tri` trioptimize, `tf` trueform, `uns` unsharp — each the
plugin's `*filterplugin.cpp`. vcglib paths are under `vcglib/vcg/complex/algorithms/`.

| # | Filter | Current id · label | Reads (evidence) | Target | Changes beyond id/label/default |
|---|---|---|---|---|---|
| 1 | transfer_color_from_current_raster_to_vertex | `onselection` · Only on selection | V (cpj:141) | vertices | — |
| 2 | transfer_color_from_visible_rasters_to_vertex | `onselection` · Only on selection | V (cpj:194) | vertices | — |
| 3 | set_vertex_color | `onSelected` · Only on selection | V (cpc:381 → update/color.h:90) | vertices | — |
| 4 | threshold_vertex_color | `onSelected` · Only on selection | V (cpc:394 → color.h:537) | vertices | — |
| 5 | adjust_vertex_color_brightness_contrast_gamma | `onSelected` · Only on selection | V (cpc:406 → color.h:700) | vertices | — |
| 6 | invert_vertex_color | `onSelected` · Only on selection | V (cpc:413 → color.h:675) | vertices | — |
| 7 | adjust_vertex_color_levels | `onSelected` · Only on selection · **adv** | V (cpc:428 → color.h:746) | vertices | out of advanced |
| 8 | tint_vertex_color | `onSelected` · Only on selection | V (cpc:445 → color.h:795) | vertices | — |
| 9 | desaturate_vertex_color | `onSelected` · Only on selection | V (cpc:456 → color.h:842) | vertices | — |
| 10 | equalize_vertex_color | `onSelected` · Only on selection | V, histogram too (cpc:463 → color.h:907, 930) | vertices | — |
| 11 | adjust_vertex_color_white_balance | `onSelected` · Only on selection | V (cpc:473 → color.h:979) | vertices | — |
| 12 | colorize_vertices_by_perlin_noise | `onSelected` · Only on selection | V (cpc:489 → color.h:485) | vertices | — |
| 13 | add_noise_to_vertex_color | `onSelected` · Only on selection | V (cpc:500 → color.h:515) | vertices | — |
| 14 | compute_edge_color_by_expression | `onselected` · Only On Selection | E (exp:1199) | edges | — |
| 15 | compute_edge_scalar_by_expression | `onselected` · Only On Selection | E, but Normalize and Map rewrite **all** edges (exp:1286-1301) | edges | restrict Normalize/Map |
| 16 | compute_vertex_coordinates_by_expression | `onselected` · Only On Selection | V; with only faces selected, derives vertices into the live mesh (exp:883-886) | vertices | drop own fallback (Q2) |
| 17 | compute_vertex_normals_by_expression | `onselected` · Only On Selection | as 16 | vertices | as 16 |
| 18 | compute_vertex_color_by_expression | `onselected` · Only On Selection | as 16 | vertices | as 16 |
| 19 | compute_vertex_scalar_by_expression | `onselected` · Only On Selection | as 16; Normalize and Map act on **all** vertices (exp:1492-1495) | vertices | as 16; restrict Normalize/Map |
| 20 | parametrize_per_vertex_by_expression | `onselected` · Only On Selection | as 16 | vertices | as 16 |
| 21 | compute_face_normals_by_expression | `onselected` · Only On Selection | F (exp:1628) | faces | — |
| 22 | compute_face_color_by_expression | `onselected` · Only On Selection | F (exp:1675) | faces | — |
| 23 | compute_face_scalar_by_expression | `onselected` · Only On Selection | F; Normalize and Map act on **all** faces (exp:1752-1755) | faces | restrict Normalize/Map |
| 24 | parametrize_per_wedge_by_expression | `onselected` · Only On Selection | F (exp:1564) | faces | — |
| 25 | subdivide_by_loop | `Selected` · Affect only selected faces · **@f** | F (refine.h:356, 363); the even rule also moves the selection's border vertices, deforming the ring of unselected faces (refine_loop.h:581) | faces | border leak (Q3b) |
| 26 | subdivide_by_butterfly | `Selected` · Affect only selected faces · **@f** | F (refine.h:356, 363) | faces | — |
| 27 | subdivide_by_midpoint | `Selected` · Affect only selected faces · **@f** | F (refine.h:356, 363) | faces | — |
| 28 | subdivide_by_ls3_loop | `Selected` · Affect only selected faces · **@f** | F; border leak as 25 (refine_loop.h:581, 607) | faces | border leak (Q3b) |
| 29 | simplify_by_quadric_edge_collapse_vcglib | `Selected` · Simplify only selected faces · **@f** | F→V as the writable flag (msh:441-448); clears the vertex selection afterwards (msh:480-486) | faces | `TargetPerc` is taken from the whole mesh (msh:750-751) but applied to the selection (msh:465) |
| 30 | simplify_by_quadric_edge_collapse_with_texture_vcglib | `Selected` · Simplify only selected faces · **@f** | as 29 (msh:509-546) | faces | as 29 (msh:788-789, 525) |
| 31 | remesh_isotropically_vcglib | `SelectedOnly` · Remesh only selected faces | F in every pass, but ProjectToSurface moves **every** vertex (isotropic_remeshing.h:1364-1378) | faces | restrict projection |
| 32 | invert_face_orientation | `onlySelected` · Flip only selected faces | F only with Force Flip on (msh:737-741, clean.h:1555); off, the toggle is ignored and the whole mesh may flip (clean.h:1612) | faces | honour it in both branches |
| 33 | close_holes | `Selected` · Close holes with selected faces | fills only holes reached from a selected border face (hole.h:615) | *(none)* | not a scope: it chooses holes, it does not confine an operation (Q3d) |
| 34 | project_vertices_onto_mls_surface_apss | `SelectionOnly` · Selection only | F→V on the proxy (mls:198-199), then V (mls:222); on a point cloud the conversion empties the selection, so **nothing** is ever projected | vertices | honour the vertex selection (Q3c) |
| 35 | project_vertices_onto_mls_surface_rimls | `SelectionOnly` · Selection only | as 34 | vertices | as 34 |
| 36 | compute_curvature_apss | `SelectionOnly` · Selection only | V (mls:480 → 242) | vertices | help says "projected" |
| 37 | compute_curvature_rimls | `SelectionOnly` · Selection only | as 36 | vertices | as 36 |
| 38 | sample_vertices_by_clustering | `Selected` · Only on Selection · **adv** | V; with only faces selected, strict F→V on a copy (smp:515-518) | vertices | out of advanced; drop own fallback (Q2) |
| 39 | transfer_vertex_attributes_by_closest_point | `onSelected` · Only on Selection · **adv** | V on the target; with only faces selected, loose F→V on a copy (smp:1227-1232) | vertices | out of advanced; drop own fallback (Q2) |
| 40 | flip_edges_by_planarity | `selection` · Affect only selection · **@f** | F (tri:149-152), but the relax step smooths the *loose* vertex set, moving the selection border (tri:155, 269-274); overwrites the vertex selection (tri:278-279) | faces | border leak; vcglib defect 8 |
| 41 | flip_edges_by_curvature | `selection` · Affect only selection · **@f** | F (curvedgeflip.h:193); overwrites the vertex selection (tri:321-322) | faces | — |
| 42 | smooth_vertices_by_surface_preserving_laplacian_vcglib | `selection` · Affect only selection | F→V (tri:343) | faces | vcglib defect 8 |
| 43 | smooth_vertices_by_laplacian_trueform | `selectedOnly` · Selected Only | V, applied as a mask **after** smoothing the whole mesh every iteration, so selected vertices see their unselected neighbours move (tf:1785-1804) | vertices | hold unselected fixed during iterations |
| 44 | smooth_vertices_by_taubin_trueform | `selectedOnly` · Selected Only | as 43 | vertices | as 43 |
| 45 | smooth_vertices_by_laplacian_vcglib | `Selected` · Affect only selection · **adv @f** | F→V (uns:248 → 106) | faces | out of advanced |
| 46 | smooth_vertices_along_one_direction | `Selected` · Affect only selection · **adv @f** | F→V (uns:264-268) | faces | out of advanced |
| 47 | smooth_vertices_by_scale_dependent_laplacian | `Selected` · Affect only selected faces · **adv @f** | **never read**: the whole mesh is smoothed and the selected count reported (uns:305-316) | faces | pass `SmoothSelected` (supported, smooth.h:127) |
| 48 | smooth_vertices_by_two_step_normal_fitting | `Selected` · Affect only selected faces · **adv @f** | F→V, run even with the toggle off (uns:330) | faces | convert only when on |
| 49 | smooth_vertices_by_taubin_vcglib | `Selected` · Affect only selected faces · **adv @f** | **never read**: restricts whenever faces are selected, whole mesh otherwise (uns:348-359) | faces | read the toggle (smooth.h:447 supports it) |
| 50 | smooth_vertices_by_hc_laplacian | *(no toggle)* | restricts silently whenever faces are selected (uns:318-326) | faces | gains the toggle (smooth.h:552 supports it) |

**Totals.** 50 filters: 49 with a toggle today, 1 without (row 50).

| Target | Rows | Count |
|---|---|---|
| `vertices` | 1–13, 16–20, 34–39, 43–44 | 13 + 5 + 6 + 2 = 26 |
| `faces` | 21–32 except 33, 40–42, 45–50 | 4 + 8 + 3 + 6 = 21 |
| `edges` | 14–15 | 2 |
| kept out | 33 | 1 |
| | | **50** |

What the code reads today: V 24 (1–13, 16–20, 36–39, 43–44), F 13 (21–28, 31–33, 40–41),
F→V 8 (29–30, 34–35, 42, 45–46, 48), E 2 (14–15), never read 2 (47, 49), no toggle 1
(50) — also 50. Group: 41 main, 8 advanced. Default: 13 `@hasSelectedFaces`, 36 `false`.

## Defects and their fixes

Found by the audit:

1. **Toggles that did nothing** (rows 47, 49): now read and passed to vcglib, which
   supported them all along.
2. **Restriction with no toggle** (row 50): now scoped like its siblings.
3. **Ignored in one branch** (row 32): with Force Flip off, automatic orientation decides
   for the whole mesh, so that combination is refused with a reason.
4. **Label said vertices, code read faces** (rows 34–35): vertex-scoped, as above.
5. **"Only selected" leaking to unselected elements**:
   - Normalize and Map (rows 15, 19, 23) now span and rewrite the selection alone.
   - Subdivision (rows 25, 28): vcglib's `RefineOddEvenE` no longer applies the even rule
     to vertices touched by an unselected face.
   - Remeshing (row 31): vcglib's `IsotropicRemeshing::ProjectToSurface` moves only the
     interior vertices, the set its smoothing pass already used.
   - Planarity flip (row 40): its relax step uses the strict vertex set, not the loose one.
   - TrueForm smoothing (rows 43–44): unselected vertices are held in place between
     iterations instead of being masked out at the end.
6. **Wrong target on a selection** (rows 29–30): `TargetPerc` is a fraction of what is
   simplified.
7. **Selection destroyed as a side effect**: the framework now restores it after every
   scoped run, so the plugins' own conversions and fallbacks were removed; the attribute
   transfer reports the selection it transfers alongside geometry, so that one stands.
8. **vcglib `Smooth::VertexCoordPlanarLaplacian`**: an unselected vertex's candidate
   position in the orientation tests is its current one, no longer a raw accumulated sum.

Found by the contract test (`FilterTests::scopedFiltersConfineThemselvesToTheSelection`),
unrelated to selection but in its way:

9. *Compute Vertex Normals by Expression* failed on every run: it shares a branch with the
   color filter, which parsed an alpha expression the normal filter does not declare. The
   smoke test had listed it as "needs an expression".
10. vcglib's Butterfly and Loop odd rules set only the color of a new vertex; its quality
    and texture coordinates kept whatever the allocation held. They are interpolated now,
    as `MidPoint` already did.
11. *Flip Edges by Curvature* keeps each vertex's curvature in its quality, border vertices
    of the selection included, where the metric needs it; their own quality is put back.

## Left for later

- `incremental_selection`, the other framework-injected selection parameter, is the only
  snake_case parameter id. And three selection filters hand-roll their own combine mode
  instead of using it: TrueForm's `replaceSelection` (3 filters), and the `mode` enums of
  *Select by Screen Rectangle* and *Select Vertices Inside Mesh (TrueForm)*, which also
  offer *subtract*.
- Filters that are not scoped still change the selection without saying so: *Remove
  T-Vertices* (edge-flip method) and *Compute Harmonic Scalar Field* through
  `CountNonManifoldVertexFF`, *Split into Connected Components*, *Orient Face Normals by
  Ray Casting*, *Repair Mismatched Borders* and *Sample Surface by Voronoi Relaxation*.
  The run guard could cover every filter that does not declare `VS`/`FS`.
- Quadric simplification's `TargetFaceNum` defaults to half the selected faces
  (`@selOrFaceCountHalf`) whenever faces are selected; unticking *Only selected faces*
  keeps that small target for the whole mesh.
- *Compute Vertex Coordinates by Expression* declares an alpha expression it never reads.
