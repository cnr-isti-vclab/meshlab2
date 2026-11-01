# Two-layer filters modify the current layer (draft, 2026-10-31)

**Status: approved 2026-11-01 (section 6). Stages 1 (the rule and the test) and 2 (the
inverted filters and the split) implemented 2026-11-01; stage 3 not yet.**

A filter that reads two layers and changes one should change the **current** layer, and
take the other through a single mesh parameter. That is how the single-mesh filters work
since 2026-09-29 and how the five Open3D aligners work since 2026-10-15, but it is written
down only for those cases, and a dozen filters do it some other way -- four of them the
opposite way. This draft states the rule, measures every filter against it, and proposes
splitting *Transfer Vertex Attributes to Texture by Closest Point* on the way.

## 1. What is there today

[filter_organization.md](../filter_organization.md), "A single-mesh filter works on the current
layer", says that a filter working on one mesh takes it from the current layer and that a
mesh parameter names a *second* input; `FilterDescriptorTests::singleMeshFiltersUseTheCurrentLayer`
enforces the single-mesh half. The Open3D paragraph of the same section applies the
two-layer form to the five pairwise aligners only, and notes that the TrueForm aligners were
"the same pattern and were left alone because the request named Open3D only".

Of the 47 filters with a mesh parameter, 25 read two layers and modify one of them: ten
already follow the rule, fifteen do not (section 4). The rest create new layers (the booleans, *Measure Hausdorff Distance*, *Create Surface
from Polyline Loop*), only report (*Measure Chamfer Distance*), act on the whole document
(*Align Meshes Globally*, *Align Meshes Sequentially*), or already follow the rule.

The inverted ones matter beyond convenience. The three closest-point transfers and
*Transfer Abstract Domain to Another Layer* declare `outputDomain: ModifyCurrentMesh` while
writing the layer named by `targetMesh`, whose default is the *other* layer. The framework
reads that declaration for its requirement checks and to decide what the undo step and the
stored state concern; the 2026-09-29 migration found exactly this mismatch in ten TrueForm
filters ("the requirement checks had been validating the current layer while the filter
worked on another"). Whether it shows today has not been reproduced; stage 1 starts with
the test that would.

## 2. The rule

To go into filter_organization.md, replacing the Open3D-only paragraph:

> **A filter that reads two layers and changes one changes the current layer.** The other
> layer is a single mesh parameter named for its role -- `sourceMesh` (it is read from),
> `referenceMesh` (it is matched or measured against), or a role-specific name the
> vocabulary approves (`occluder`, `surface`) -- defaulting to `@otherMeshIndex`. There is no
> parameter for the layer that changes: it is the current one, as for a single-mesh filter.
> Such a filter declares `inputDomain: SingleMesh` and `outputDomain: ModifyCurrentMesh`.
> A filter that changes neither layer (it creates one, or only reports) is not bound by it.

And a descriptor test beside `singleMeshFiltersUseTheCurrentLayer`: a `ModifyCurrentMesh`
filter may not have a mesh parameter that defaults to `@currentMeshIndex`, nor two mesh
parameters. Both are how a filter names the layer it changes as a parameter.

**What the user sees.** For a transfer: choose the layer that receives, then pick where it
comes from. For an alignment: choose the layer that moves, then pick what it moves onto.
For a distance or a selection: choose the layer that gets the values or the selection, then
pick what it is measured against. The verb's object is always the current layer.

## 3. Splitting Transfer Vertex Attributes to Texture by Closest Point

The filter has one sampler -- for each texel of the target, its 3D position, the closest
point of the source, and a barycentric interpolation there -- and an *Color Data Source*
enum that picks what is interpolated: vertex color, vertex normal, vertex scalar, or the
source's texture color. With texture color it is a texture-to-texture resampler, the usual
way to re-texture a mesh after remeshing, simplification or a new parametrization, and the
name does not say so: a search for "transfer texture" ranks it by its description, below
every filter with *Texture* in the name. *Source Texture* only means something in that mode,
and sits in the advanced group.

The closest-point family is a source x target grid, and the split completes it:

| | to vertices | to a texture |
|---|---|---|
| **from vertex attributes** | *Transfer Vertex Attributes by Closest Point* (filter_sampling) | *Transfer Vertex Attributes to Texture by Closest Point* -- color, normal, scalar |
| **from a texture** | *Transfer Color from Texture to Vertex by Closest Point* | **new:** *Transfer Texture to Texture by Closest Point* -- texture color, *Source Texture* in the main group |

Two descriptors, two dispatch branches, one sampler: no algorithm code is added. The new
filter takes the id `transfer_texture_to_texture_by_closest_point`; the old one keeps its id
and loses the texture option. Python scripts that passed `AttributeEnum="texture_color"`
move to the new filter, without an alias.

Open: whether *Transfer Color from Texture to Vertex by Closest Point* should then read
*Transfer Texture to Vertex Color by Closest Point*, to match the new name. Not proposed
here; it is a rename for the vocabulary pass, not part of this change.

## 4. Mapping table

Measured from the code on 2026-10-31 (`devel` after the project-persistence commits): the
layer each filter actually changes is the index passed to `markMesh*Changed` or
`setMeshTransform`, not what the manifest says.

### 4.1 Inverted: they change the layer named by a parameter, defaulting to the other one (5)

| # | filter (plugin) | parameters today | changes | evidence | target |
|---|---|---|---|---|---|
| 1 | Transfer Vertex Attributes to Texture by Closest Point (texture) | `sourceMesh` = current, `targetMesh` = other | `targetMesh` | `texturefilterplugin.cpp:1172-1190,1332` | current; `sourceMesh` = other; `targetMesh` dropped; split (section 3) |
| 2 | Transfer Color from Texture to Vertex by Closest Point (texture) | `sourceMesh` = current, `targetMesh` = other | `targetMesh` | `texturefilterplugin.cpp:1093-1096,1149` | current; `sourceMesh` = other |
| 3 | Transfer Vertex Attributes by Closest Point (sampling) | `SourceMesh` = current, `TargetMesh` = other | `TargetMesh` | `samplingfilterplugin.cpp:1187-1188,1295-1310` | current; `sourceMesh` = other |
| 4 | Align by ICP (vcglib) (icp) | `ReferenceMesh` = current, `SourceMesh` = other | `SourceMesh` (`setMeshTransform`) | `icpfilterplugin.cpp:302-303,350` | current moves; `referenceMesh` = other |
| 5 | Transfer Abstract Domain to Another Layer (isoparam) | `sourceMesh`, `targetMesh`, no defaults | `targetMesh` | `isoparamfilterplugin.cpp:607-608,668` | current; `sourceMesh` = other |

### 4.2 Right layer, but it is a parameter (10)

| # | filter (plugin) | parameters today | changes | evidence | target |
|---|---|---|---|---|---|
| 6 | Align by Bounding Box (TrueForm) | `sourceMesh` = current, `referenceMesh` = other | `sourceMesh` | `trueformfilterplugin.cpp:192-193,217` | drop `sourceMesh` |
| 7 | Align by ICP (TrueForm) | same | `sourceMesh` | same | drop `sourceMesh` |
| 8 | Align to Corresponding Points (TrueForm) | same | `sourceMesh` | same | drop `sourceMesh` |
| 9 | Compute Signed Distance to Mesh (TrueForm) | `sourceMesh` = current, `referenceMesh` = other | `sourceMesh` | `trueformfilterplugin.cpp:1549-1550,1601` | drop `sourceMesh` |
| 10 | Select Vertices Inside Mesh (TrueForm) | same | `sourceMesh` | `trueformfilterplugin.cpp:1625-1626,1674` | drop `sourceMesh` |
| 11 | Compute Distance from Reference Mesh (sampling) | `MeasureMesh` = current, `RefMesh` = other | `MeasureMesh` | `samplingfilterplugin.cpp:1168` | drop `MeasureMesh`; `RefMesh` → `referenceMesh` |
| 12 | Colorize Vertices by Voronoi Regions (sampling) | `ColoredMesh` = current, `VertexMesh` = other | `ColoredMesh` | `samplingfilterplugin.cpp:1405` | drop `ColoredMesh`; `VertexMesh` → `sourceMesh` |
| 13 | Colorize Vertices by Disk Distance (sampling) | same | `ColoredMesh` | `samplingfilterplugin.cpp:1456` | same |
| 14 | Project Vertices onto MLS Surface (APSS) (mls) | `ControlMesh` = current, `ProxyMesh` = current | `ProxyMesh` | `mlsfilterplugin.cpp:400-401,429` | current is projected; `ControlMesh` → `referenceMesh`, default current (the self-projection case) |
| 15 | Project Vertices onto MLS Surface (RIMLS) (mls) | same | `ProxyMesh` | same | same |

Rows 14-15 change what the user does: today the point cloud is current and the mesh to
project is picked; after, the mesh to project is current and the point cloud is picked.
Both defaults being the current layer means today's default run projects a cloud onto
itself, which is the common case for these filters (smoothing a cloud by its own MLS
surface); so `referenceMesh` keeps `@currentMeshIndex` as its default (decision 3), and the
test of section 2 lists these two as its exceptions.

### 4.3 Already conform: no change (row 16, ten filters)

*Compute Point Cloud Ambient Occlusion* (`occluder_mesh`), *Compute Generalized Winding
Number* (`surface_mesh`), *Displace Vertices Toward Target Mesh* (`TargetMesh`, read only),
*Smooth Polyline on Surface* (`surface`), *Embed Polyline in Surface* (`polyline`) and the
five Open3D pairwise aligners (`referenceMesh`): each changes the current layer and has one
mesh parameter for the other. Their parameter names are left for the vocabulary pass.

### 4.4 Framework and documentation

| # | item | target |
|---|---|---|
| 17 | filter_organization.md | the rule of section 2, replacing the Open3D-only paragraph |
| 18 | `FilterDescriptorTests` | the test of section 2, with rows 14-15 as its stated exceptions if their default stays current |
| 19 | `inputDomain` of rows 1-5, 11-15 | `WholeDocument` → `SingleMesh`, as their outputs already say `ModifyCurrentMesh` |
| 20 | tests | rows 1, 2, 11-13 have no behavioural test today (no test names them); each gets one that runs with the current layer as target, as the migration's measurement |

**Totals:** 5 inverted + 10 parameterised + 1 row for the ten that conform + 4 framework
items = **20 rows**; plus the split of row 1 into two filters.

Python: every script that passes the dropped parameters changes. The parameter ids are the
Python keywords, so `TargetMesh=`, `MeasureMesh=`, `ColoredMesh=`, `SourceMesh=` on ICP and
the `sourceMesh=` of rows 6-10 go; a script sets the current layer instead
(`set_current_mesh`). No aliases, as agreed for the Python API.

## 5. Staging

1. **The rule and the test** (rows 17-18). **Done 2026-11-01.** filter_organization.md
   states the rule; `FilterDescriptorTests::twoLayerFiltersChangeTheCurrentLayer` enforces
   it, with a pending table of the 15 filters of 4.1-4.2 and an exception table, both
   failing in both directions. Run without the tables it flagged exactly those 15 and four
   more the table had missed, because section 1 counted them as whole-document or
   single-parameter filters: *Remesh Isotropically* (`ReferenceMesh`, the surface it projects
   onto, by default its own) and the three whole-document aligners (*Align Meshes Globally*
   vcglib and Open3D, *Align Meshes Sequentially* Open3D, whose parameter names the layer
   held fixed). None names the layer that changes, so they are exceptions, with the two MLS
   projections. The aligners move every layer while declaring `ModifyCurrentMesh`, an
   inaccuracy of their own, outside this proposal. Resolved defaults are compared with the
   fixture's current layer, since the test sees `@currentMeshIndex` already resolved.
2. **Inverted filters** (rows 1-5, with 19-20 for them), and the split of row 1. **Done
   2026-11-01.** The five write the current layer and read `sourceMesh` (`referenceMesh`
   for ICP), defaulting to the other layer; their target parameter is gone, they declare
   `SingleMesh`, and the target's requirements moved to `inputRequirements`. *Transfer
   Texture to Texture by Closest Point* is new, sharing the branch and sampler of *Transfer
   Vertex Attributes to Texture by Closest Point*, which lost its texture option (its
   *Vertex Quality* option reads *Vertex Scalar*). The sampling transfer's
   `selectionScopeMesh` went with its target parameter. Found on the way:
   - The closest-point texture samplers culled each texel by the source's bounding box with
     `IsInEx`, an exclusive test that a box of no thickness fails everywhere: a planar source
     transferred nothing into a texture. They now test the box grown by the search distance
     (`rastering.h`).
   - No visible failure came from the declared-versus-written mismatch itself: these filters
     mark the layer they change, and the framework's bookkeeping follows the marks.
   - `singleMeshFiltersUseTheCurrentLayer` read any lone `sourceMesh` as a filter taking its
     own mesh; it now does so only when that parameter defaults to the current layer.
   Tests: `closestPointTransfersWriteTheCurrentLayer` (the three texture transfers, on flat
   layers, the source left stored); the ICP, abstract-domain and selection-scope tests set
   the current layer instead of a target parameter.

   Open, for the vocabulary: *Transfer Abstract Domain to Another Layer* now transfers *from*
   another layer into the current one, so its name reads backwards; and *Align by ICP
   (vcglib)* now works as the Open3D *Align Layer to Reference by ...* filters do, under a
   different name.
3. **Parameterised filters** (rows 6-15). With the MLS projections migrated nothing uses
   `selectionScopeMesh` any more, so the field, its loader check and the filter panel's
   handling go too.

## 6. Decisions (2026-11-01)

1. The rule as worded in section 2.
2. The split of section 3, and the name *Transfer Texture to Texture by Closest Point*.
3. Rows 14-15: the projected mesh becomes current, and the point cloud whose MLS surface it
   is projected onto is `referenceMesh` -- what the layer is projected onto, not what it
   reads from. Its default stays `@currentMeshIndex`, so a cloud is still projected onto its
   own surface with the defaults; the test of section 2 lists the two filters as its
   exceptions for that.
4. The other layer is `sourceMesh` when it is read from and `referenceMesh` when it is
   matched, measured or projected onto, as per row.
