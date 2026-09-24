# Clipping Plane

An interactive plane that cuts away part of the scene so you can see inside it, integrated
into the shaders rather than faked with the near clipping plane. **Implemented**; this
document is kept until its content moves into [rendering.md](../rendering.md), at which
point it goes away.

Today the only way to cut into an object is `view.nearClipRatio`, which slides the near
plane towards the eye. That works, and people use it, but it has three limitations that
are not fixable where it lives:

- The near plane is **perpendicular to the view direction** by construction. You cannot cut
  along an anatomical axis, or along a plane you fitted to the data, and you cannot orbit
  the object while keeping the cut where it was — the cut turns with the camera.
- It is **invisible**. Nothing on screen says where the plane is, so you discover the cut
  by overshooting it. (The peer-view camera gizmo now draws the near plane truthfully, but
  only for *another* view, and only as a rectangle at the frustum's mouth.)
- It **ties the cut to the depth buffer**. Depth resolution at distance Z goes as Z²/near,
  so the one control sets both. Cutting in happens to *improve* precision — at the default
  framing, pushing the ratio from 0.00333 to 0.66 takes resolution at the object from
  5.4e-5 to 1.9e-7 scene radii — but scrubbing back out toward the 1e-5 floor collapses it
  to 1.8e-2, two percent of the object's radius, which is where wireframe-over-fill starts
  to shimmer. You cannot choose the cut and the precision separately.

This document proposes replacing that use of the near plane with a real clipping plane,
and keeps the near plane for what it is for.

## Status

As of 2026-09-24: **all four phases are implemented**. The plane cuts every Scene3D pass
including picking, marks where it meets the surface, shows its own grid while being moved,
is driven by `Ctrl`+wheel and `Alt`+drag as well as from a saved render state, and has its
own button on the render panel's pass bar.

Three questions were settled the same day: the OpenGL floor **moves to 3.2 core**, there is
**no per-layer exemption**, and **raster mode is out of scope**.

One decision below changed during implementation and the text has been updated to match:
the plane rides in the uniform slices that already exist rather than in a new block at
binding 6. The reasoning is under *Where the plane lives*.

## What this is, and what it is not

**Not an interactive tool.** [The tool contract](../architecture.md) reserves
`InteractiveTool` for things that are explicitly engaged, act on document content, and
commit exactly one `runFilter` per gesture. A clipping plane mutates nothing and produces
no undo node. It belongs with the trackball and the headlight: ambient per-view state that
the camera-level input handling owns. Concretely it lives in `GlobalRenderSettings`, it is
saved and restored by `rendersettingsjson.cpp`, and it reaches a scripted
`render_snapshot()` for free.

**Not a filter.** Making a cut permanent is a separate, already-partly-solved problem:
`create_polyline_from_planar_section` (filter_meshing) gives the cross-section as a
polyline layer today. A "discard everything on one side" filter does not exist and would
be a real addition — out of scope here, noted in the open questions.

**Per view, not per document.** Each 3D view can cut differently, exactly as each view has
its own camera and its own headlight. `MainWindow::splitView*` already copies the source
view's settings into the new view, so a split inherits the cut and then diverges.

## How the cut is made: `gl_ClipDistance`

Hardware clip distances, written in the vertex shader. The shader-side cost is two lines
per pass: declare the plane in a uniform block, and write one dot product. Hardware
clipping is also the *right* mechanism rather than merely the cheap one — a triangle or a
line is cut exactly at the plane rather than dropped whole, and it costs nothing per
fragment. (A point has no extent, so a point either survives or it does not; that is what
cutting a point cloud means anyway.)

`discard` in the fragment shader is the fallback if a backend ever refuses clip distances.
It is not the default: it would need a varying carrying position into every fragment
shader, and it cannot cut a line cleanly.

### Measured: two of the six baked targets cannot take it

`qt_add_shaders` is called with no language options, so it bakes Qt's default set — and
Qt's default includes two legacy GLSL profiles that predate `gl_ClipDistance`. Compiling a
one-line vertex shader that writes `gl_ClipDistance[0]` through this tree's `qsb` (6.11.2),
against exactly the targets a baked `.qsb` in `build-release/.qsb/shaders/` contains:

| Baked target | Emitted for `gl_ClipDistance[0] = …` |
| --- | --- |
| SPIR-V 100 | `ClipDistance` reflected in `outBuiltins` — fine |
| GLSL 100 es | `#version 100` … `out float gl_ClipDistance[1];` — **invalid**; ES 1.00 has no `out` and no clip distances |
| GLSL 120 | `#version 120` … `out float gl_ClipDistance[1];` — **invalid**; global `out` is GLSL 1.30+ |
| GLSL 150 | `out float gl_ClipDistance[1];` — fine, core since GLSL 1.30 |
| HLSL 50 | `float gl_ClipDistance0 : SV_ClipDistance0` — fine |
| MSL 12 | `float gl_ClipDistance [[clip_distance]] [1]` — fine |

`qsb` exits 0 for all six. It does not warn about the two it breaks: SPIRV-Cross correctly
downgrades ordinary varyings to `varying`/`attribute` for those profiles — the existing
`fill_smooth.vert` proves it — but there is no legacy lowering for a clip distance, because
none exists. Those two variants would fail when the OpenGL backend compiles them at run
time, on the machines least able to report why.

**So shipping this means narrowing the GL floor.** One line:

```cmake
qt_add_shaders(MeshLab2 "shaders"
    GLSL "150"
    …
```

which drops OpenGL 2.1 and OpenGL ES 2.0 from what the GL backend can run on, and requires
OpenGL 3.2 core. Metal, D3D11 and Vulkan are untouched — on macOS and Windows nothing
changes at all. This is a decision to take deliberately rather than discover: it is the one
externally visible cost in the whole proposal, and it is why the `discard` fallback is
worth keeping in mind rather than dismissing.

If GLSL ES is ever wanted back, `300es` emits `#extension GL_EXT_clip_cull_distance :
require` — an extension that is not core until ES 3.2, so it would be a per-device gate
rather than a guarantee.

## Where the plane lives: mesh-local space

The plane is authored in world space, but a vertex shader sees **local** positions — `mvp`
is `proj · view · meshTransform`. Testing a local position against a world plane would be
wrong for every layer that has a transform, which in a multi-layer alignment session is
most of them.

The conversion is one line. A plane transforms as a row vector: if `x_world = M · x_local`
then `p_local = Mᵀ · p_world`. So one world plane becomes a different `vec4` per layer —
which is what makes the per-mesh uniform slice its natural home, and settles the question
the section below opens.

`ClipPlane::world()` resolves the settings into the world plane and `ClipPlane::toLocal()`
does the transpose; both live in `src/render/clipplane.h`, outside RenderWidget, so they
can be tested. `tests/test_clipplane.cpp` pins them, including that a flip swaps sides
without sliding the cut — the one thing the first implementation got wrong, because folding
the flip into the normal before applying the offset moves the plane to its mirror image.

## What changes, measured

The plane is per-layer data with the same lifetime and cadence as `mvp` and `modelView`, so
it goes where those go: the per-mesh uniform slice each pass already allocates and writes.
That is one `vec4` appended to two existing blocks — `kUbufSize` 352 → 368, and the
decorator block 80 → 96 with the fat variant 96 → 112 — and no new buffer, no new binding,
and no change to any of the twelve shader resource bindings.

An earlier draft put it in a second block at binding 6 instead, because the main block is
declared as a *prefix* by the shaders that use it — `fill_wire.vert` declared 3 of its 15
fields, `selection_mask.vert` just one — and appending forces each of them to spell out the
whole prefix first. That cost turned out to be nine mechanical copy-paste edits, against a
new buffer, a second dynamic offset threaded through every `setShaderResources` call, and
twelve SRBs to re-create. The prefix padding is noise in the source; the alternative was
machinery. Padding won.

```glsl
    vec4 lightDir;
    // Mesh-LOCAL clipping plane: a vertex survives when dot(vec4(pos,1), clipPlane) >= 0.
    // All zero disables clipping. Written at kUbufClipPlaneOffset.
    vec4 clipPlane;
```

Eleven vertex shaders write the clip distance:

| Shader | Position it clips against | Why it is in the list |
| --- | --- | --- |
| `fill_smooth.vert`, `fill_flat.vert` | `position` / `inPos` | the surface itself |
| `fill_wire.vert` | `inPos` | wireframe over the surface |
| `overlay_edges.vert` | `inPos` | polyline layers and edge meshes |
| `overlay_fat_edges.vert` | `mix(inP0, inP1, inAlong)` | a fat line is a quad expanded from two endpoints; the point being drawn is the one along the segment, not either end |
| `overlay_points.vert` | `inPos` | point clouds |
| `overlay_bbox.vert` | `inPos` | a bounding box that ignored the cut would sit outside it |
| `selection_mask.vert` | `position` | the four current-layer highlight-mask pipelines |
| `depth_pick.vert` | `position` | **required for correctness**: without it you pick surfaces that are not on screen |
| `overlay_decorator.vert` | `inPos` | normals, boundaries, seams — and, sharing the shader, the selection overlay |
| `overlay_fat_decorator.vert` | `mix(inP0, inP1, inAlong)` | the thick-line decorator variant |

Three of them (`overlay_points`, `depth_pick`, `selection_mask`) already redeclare
`gl_PerVertex` for `gl_PointSize`; a redeclared block must list every built-in the shader
writes, so `float gl_ClipDistance[1]` joins them there. Without it `qsb` fails the build
with *member of nameless block was not redeclared*, which is at least a loud failure.

Every clipped shader now writes a clip distance on every vertex, whether or not there is a
plane. That is the price of not carrying a second pipeline variant per pass, and it has
**not been measured**: on a tile-based GPU it should be free, and if it ever shows up on a
desktop part the answer is a variant, not a branch.

Deliberately **not** clipped: the trackball, axis, light, peer-camera and clip-plane gizmos,
the scene background, the raster backplate and projected-raster passes, and
`uv_fill_texture.vert` with the rest of UV mode. A clipping plane is a statement about the
3D scene, not about the instruments you look at it with. Those passes leave the `vec4` zero,
which is a clip distance of 0 — on the plane, and kept — so nothing needs a second pipeline
or a branch.

## State

In `GlobalRenderSettings`, reusing the parameter vocabulary
`create_polyline_from_planar_section` already established so the view control and the
filter describe a plane the same way:

| Field | Type | Notes |
| --- | --- | --- |
| `clipPlaneEnabled` | `bool` | off by default |
| `clipPlaneAxis` | enum `x` / `y` / `z` / `view` / `custom` | `view` reproduces today's near-plane behaviour exactly |
| `clipPlaneCustomAxis` | `QVector3D` | the normal when `custom` |
| `clipPlaneRelativeTo` | enum `origin` / `center` / `min` / `max` | same four references as the filter |
| `clipPlaneOffset` | `float` | signed, along the normal, from the reference |
| `clipPlaneFlipped` | `bool` | which side is kept |
| `clipPlaneShowPlane` | `bool` | draw the plane itself |

Plus `clipPlaneRimColor` and `clipPlaneRimWidth` for the cut rim. Each is one line in the
`rendersettingsjson.cpp` field-list macro, which generates the JSON conversion and the
equality operator together. `kGlobalSettingsFieldCount` went 34 → 43.

**They live on their own page, behind their own button on the pass bar**
(`RenderPass::ClipPlane`, appended to the enum rather than inserted, since the pass is
stored as an integer in every saved render state). The button is the enable and the arrow
under it opens the page, which is the idiom every other pass already uses. That is what
makes the feature findable: buried in the Viewer Settings page, the axis presets existed but
nobody would meet them, and a cut you cannot find is a cut you do not have. `QVector3D` was not a supported field type and gained
the same pair of conversions `QColor` has. `kGlobalSettingsFieldCount` went 34 → 41.

`clipPlaneCustomAxis` has no spin boxes yet; it is reached by **Freeze to View**, which
writes the direction the plane has this instant into it and switches the axis to `Custom`.
That is the "cut here, then orbit around the cut" gesture in one button, and it is the
reason `View` and `Custom` are separate settings rather than one.

`view` is not a stored normal: it is resolved per frame from the camera, which is what
makes it behave like the near plane does today. Switching from `view` to `custom` freezes
the current direction, so "cut here, then orbit to look at it" is two clicks.

## Seeing where the plane is

Two different things, and both shipped.

**The plane's own grid**, a bordered grid lying on it with a short stem along the normal
marking the side that survives. It is a gizmo, so it is not clipped by itself, and it rides
the line-gizmo pipeline that already draws the trackball, the light and the peer cameras.

It **shows while the plane is being moved and for 1200 ms after** — the same dwell the
interaction status overlay uses, so the grid and the readout describing it come and go
together. `clipPlaneShowPlane` keeps it up permanently, and is off by default: a grid across
the object is how you place a cut and the last thing you want once it is placed.

**The cut rim**: where the surface runs into the plane, coloured in the fill shaders.
`fill_smooth.vert` and `fill_flat.vert` hand the clip distance to the fragment stage as a
varying, and `fill_smooth.frag`, `fill_flat.frag` and `fill_radscale.frag` shade the sliver
just above zero:

```glsl
float band = smoothstep(ub.clipRim.a * fwidth(v_clipDistance), 0.0, v_clipDistance);
color = mix(color, ub.clipRim.rgb, band);
```

`fwidth` is what makes the band a constant number of pixels. Without it the rim would be a
hairline where the surface runs into the plane head-on and a wide smear where it meets it
at a glancing angle — exactly backwards, since the glancing case is the one that needs
least marking. The colour and the width in pixels ride in `vec4 clipRim`, so a width of zero
turns it off without a second pipeline.

The band does spread where the surface is nearly *tangent* to the plane, which is inherent
to marking a surface by its distance to a plane rather than an artefact of `fwidth`.
Measured: at the offset where the plane is about to touch a framed sphere, it is **four
pixels** out of 57600. Not worth a uniform to bound it.

A stencil cap — filling the cut as if the object were solid — is still not built, and still
looks like the wrong default: it is exact for a closed manifold and produces garbage for
anything else, which is most of what MeshLab is used on. The live *outline* of the cut as
real geometry is a different thing again: `vcg::IntersectionPlaneMesh` computes it exactly
and `create_polyline_from_planar_section` already wraps that when you want it as a layer.

## Interaction

Two chords, both of which turn clipping on if it is off — which is how anyone finds the
feature, since nobody reads a panel first.

- **`Ctrl`+wheel slides** the plane along its own normal, at 0.02 diagonals a notch. On the
  notch that enables it the plane starts where it just touches the scene
  (`ClipPlane::offsetClearOfScene`), so the very next notch cuts. Starting from a safe
  guess instead — half a diagonal back — would spend eight or so notches doing nothing,
  because a box's diagonal is longer than its extent along any one direction.
- **`Alt`+drag tips** it, switching the axis to `Custom` as it goes. Dragging right swings
  the normal towards the camera's right and dragging down towards its floor;
  `ClipPlane::rotateNormal` is a free function so those two signs are pinned by a test
  rather than by trying it.

`Ctrl` rather than `Alt` for the drag would have read better beside `Ctrl`+wheel, but macOS
turns a `Ctrl`+click into a right-click before Qt sees it, so `Ctrl`+drag is not a chord
this application can rely on. `Ctrl`+wheel is untouched by that and keeps the gesture people
already used for cutting.

Both route through `setRenderSettings`, so the panel's controls follow the gesture live and
a gesture is saved in a render state like any other setting. Both sit after the
interactive-tool dispatch in the event handlers, so an engaged tool keeps priority.

Not built: a grabbable handle on the gizmo's normal arrow. A modifier chord gives the same
capability without screen-space hit-testing, and the arrow stays what it is — the thing that
says which side survives.

The panel keeps the numeric route, because a plane fitted by `create_plane_from_selection`
or read off a paper has numbers, not a drag.

## What happens to `nearClipRatio`

It stays, and goes back to being what its name says. The help text has already been
corrected — it said "fraction of the scene radius" while the code multiplies the
eye-to-target distance. When phase 3 lands, the "raise it to cut into the object" sentence
comes back out and the control returns to its depth-precision job, with its default left
alone. Nothing is removed: a saved view state that raised `nearClipRatio` keeps working.

**This is not a depth-precision win, and should not be sold as one.** Holding near at its
default while the shader does the cutting is much better than scrubbing near to its floor,
but it is worse than a deep near-plane cut, which pushes near out and sharpens the depth
buffer as a side effect. A wireframe that shimmers at the default near plane will still
shimmer with the clipping plane. What is gained is a cut that can face any way, stays put
when the camera moves, says where it is, and is obeyed by picking, decorators and the
selection overlay.

## Phases

1. **Cut and show — done.** `GLSL "150"` on `qt_add_shaders`; `vec4 clipPlane` in the main
   and decorator uniform blocks; the nine main vertex shaders; the `GlobalRenderSettings`
   fields and the render-panel controls; the plane's own grid wireframe, drawn on the
   line-gizmo pipeline the peer cameras already use (`kClipPlaneGizmoRasterIndex`).
2. **Picking, decorators, selection — done.** `depth_pick.vert` and the two decorator
   shaders. Picking through a cut is a bug, so this was never optional.
3. **Direct manipulation — done.** `Ctrl`+wheel to slide, `Alt`+drag to tip, both with a
   status overlay, and `ViewTrackball::wheel` no longer scales `nearClipRatio` — that chord
   now belongs to the plane, and the ratio is a depth-precision setting reachable from the
   preferences and nowhere else. A grabbable arrow handle was dropped in favour of the
   chord; see *Interaction*.
4. **Cut appearance — done.** The rim, above. MeshLab renders two-sided, so the cut also
   shows the lit interior of the far shell behind it; a stencil cap remains unbuilt and
   unwanted as a default.

A slab — two parallel planes, for looking at a slice rather than a half — is
`gl_ClipDistance[1]` and a second `vec4`.

## Verifying it

The render path lives in the `MeshLab` executable, which a Qt test cannot link, so the
shader half of this cannot be a ctest. `tests/render_probes/clip_plane/generate_api.py`
drives it through the real pipeline instead, via the application's own `--generate-docs`
hook:

```
build-release/MeshLab.app/Contents/MacOS/MeshLab \
    --generate-docs tests/render_probes/clip_plane
```

Thirteen checks on a rendered sphere: that each axis cuts, that a flip swaps sides without
sliding the cut, that an offset past the object leaves nothing and one behind it leaves
everything, that disabling it is pixel-identical to no clipping, and that the plane gizmo
draws. It needs a real GPU and a window server — under `QT_QPA_PLATFORM=offscreen` there is
no QRhi and every snapshot fails.

Two of its assertions are worth keeping in mind when reading it, because both looked like
bugs first: a plane perpendicular to the view cuts away only what was already hidden, so it
must change **nothing**; and the default background gradient passes through a band that any
naive "is this pixel lit" test counts as geometry, so the probe renders on black.

## Open questions

1. **Naming.** "Clipping plane" is the graphics term and matches the existing `near clip`
   wording, but [vocabulary](../vocabulary.md) §3 has `Cut` (split along a curve, keep both
   sides) and `Trim` (cut along a scalar isovalue, discard one side). If a filter that
   makes the cut permanent is ever added, neither verb fits a plane exactly and the lexicon
   needs a ruling before the filter is named.
2. **Does the Metal backend accept the extra vertex output** when the fragment shader does
   not declare the matching `[[user(clip0)]]` input? SPIRV-Cross emits both, Metal allows
   unused vertex outputs, and nothing suggests a problem — but it is the one step of the
   measured chain above that was checked at translation time and not at run time.

## Settled

**The OpenGL floor moves to 3.2 core** (2026-09-23). `qt_add_shaders` gets `GLSL "150"`,
dropping the GLSL 100 es and 120 variants that cannot express a clip distance. Metal, D3D11
and Vulkan are unaffected. This closes the door on a WebGL build without further work —
WebGL 1 *is* GLSL 100 es, and WebGL 2 needs `EXT_clip_cull_distance`, which browsers do not
all expose — but nothing in this tree targets WebAssembly, so the door was not open.

**No per-layer exemption** (2026-09-23). The cut applies to every visible layer. A
`clipExempt` flag on `PerMeshRenderSettings` would be cheap, since the plane is already
written per layer, but it is another checkbox in a crowded panel for a case nobody has
asked for yet.

**Raster mode is out of scope** (2026-09-23). The cut applies in Scene3D only, alongside
UV mode which was never a candidate.
