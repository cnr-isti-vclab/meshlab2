# Orthographic Camera

This document proposes an orthographic projection for the 3D view and everything
that consumes a camera. **Nothing described here is implemented.** The 3D view is
perspective-only today.

See also: [Rendering](../rendering.md) (the QRhi pipeline, camera models, the
render-state JSON), [Data Model](../data_model.md) (what an undo state holds),
[Architecture](../architecture.md).

## Decisions

Taken 2026-10-11 and binding on what follows:

1. **Orthographic only.** No isometric, Cavalieri or other oblique camera types.
   The two enumerators that name them in `CameraShot::CameraType` are removed.
2. **The JSON is extended, and orthographic works everywhere** -- on screen, in
   snapshots, in every programmatic rendering and in every filter that reads a
   camera. A feature that cannot handle an orthographic camera is a defect of this
   work, not a documented limitation.
3. **The scale is stored so that switching projection is seamless.** The visible
   height at the orbit centre is the one quantity both projections share, and it is
   already what the trackball is built on (see [The invariant](#the-invariant)).

## Where things stand

The camera is `ViewTrackball`: an orbit centre, a rotation, a `distance`, a `radius`
and a vertical field of view clamped to 10-120 degrees. `projectionMatrix()` calls
`perspective()` and nothing else. The `trackball` JSON payload, the undo `ViewState`,
the view synchronization and the render-state JSON all carry that one state, and none
of them names a projection.

A second camera model exists for raster layers, `CameraShot`, which wraps
`vcg::Shot`. vcglib knows an `ORTHO` camera type, and `Camera::Project` and
`UnProject` honour it, but `Camera::GetMatrix` -- the function that builds the matrix
the GPU path uses -- always builds a *perspective* matrix, whatever the type
(`vcglib/vcg/math/camera.ipp:144`). The GL wrapper in the same library calls
`glOrtho` for `ORTHO` (`wrap/gl/camera.h:163`), so the library disagrees with
itself.

Much of the pipeline is already projection-agnostic because it goes through a matrix:
the depth pick unprojects through the inverse of the view-projection, the rectangle
selection projects through it, and the transform and measure tools unproject a
one-pixel offset. Those need no change.

## The invariant

Let `H = 2 * distance * tan(fovY / 2)`: **the world-space height visible at the orbit
centre.** In a perspective view it is what the camera frames; in an orthographic view
it is the height of the view volume.

Orthographic projection uses exactly that `H`. Consequences:

- **Switching is seamless.** The orbit centre, and everything on the plane through it,
  keeps its size and position on screen. Nothing is recomputed when toggling, and
  toggling back restores the perspective view exactly, because `distance` and
  `fovYDeg` are kept in both modes.
- **A small field of view is the limit of the orthographic view.** As `fovY` shrinks
  with `H` held constant, `distance` grows and the perspective image converges on the
  orthographic one. The two ends of the range are the same family, which is also what
  makes a probe for the implementation possible (see [Tests](#tests)).
- **The navigation arithmetic does not change.** Pan converts pixels to world units
  with `distance * tan(fovY/2)`, the wheel scales `distance`, the trackball gizmo's
  radius scales with `distance * tan(fovY/2)`, and every framing routine (reset,
  centre on selection, canonical views) computes `distance` from a radius and a field
  of view. All of them are statements about `H`, so all of them stay correct.

What does change is the eye. In perspective the eye is a point and rays fan out from
it; in orthographic the eye position only fixes where depth is measured from, and rays
are parallel. Everything below is a consequence of that.

## The camera state

`ViewTrackball::State` gains one field, `projection`, an enum with `Perspective`
(the default) and `Orthographic`. `distance`, `fovYDeg`, `radius` and the near-clip
ratio keep their meaning.

- **In orthographic, the wheel still changes `distance`**, so it still zooms, by
  changing `H`. Shift+wheel -- the dolly-zoom that trades field of view for distance
  at constant `H` -- has no visible effect in orthographic and is ignored there.
- **The projection matrix** is `ortho(-W/2, W/2, -H/2, H/2, near, far)` with
  `W = H * aspect`. The scene lies at view-space depth `distance +- sceneRadius`, so
  `near = distance - k * sceneRadius` and `far = distance + k * sceneRadius`, with `k`
  slightly over one. **`near` may be negative**: with the orbit centre inside the
  object the eye is inside it too, and an orthographic view still draws what is
  behind the eye. Perspective keeps `near = distance * nearClipRatio`.
- **`nearClipRatio` is a perspective depth-precision setting** and is ignored in
  orthographic, where depth is linear over `[near, far]`.
- `viewFrustumFarDistance()` widens the far plane to contain a peer view's camera
  glyph. In orthographic it widens the range on both sides, since a glyph may sit
  behind the eye.

### JSON

The `trackball` payload, shared by the camera JSON (`MeshLab.CameraState`), the
render-state JSON (`MeshLab.RenderState`) and the PNG metadata
(`MeshLab.CameraTrackballState`), gains

```json
"projection": "orthographic"
```

The writer omits it when it is `"perspective"`, as it omits every default-valued
field. The reader treats an absent key as perspective, accepts exactly the two
strings, and fails with a message naming the key for anything else.

The change is additive: `kind` and `version` stay as they are, as nothing checks
`version` today. A *build that predates this work* reading an orthographic file would
ignore the key and render it in perspective, silently. That is the cost of an
additive change; it is worth stating in the Python API documentation and nowhere else.

Everything that already round-trips the trackball state inherits the field with no
further work: the undo `ViewState`, view splitting and synchronization, the camera
copy/paste, `RenderWidget::cameraStateJson()` and `applyCameraStateJson()`, the
`camera_state` filter parameter, and the Python `camera_state_json`. The one place
that must be touched by hand is the equality test `fuzzyStateEqual`
(`renderwidget.cpp:196`), which lists the fields one by one and would otherwise miss a
projection change.

## Rays

A ray through a pixel is generated by the trackball (`viewRayFromWindow`), by the
`CameraShot` of a raster layer, by the select filter, and implicitly by every filter
that treats `viewPoint() - p` as the direction to the camera. Each assumes the origin
is the eye. This work gives them one answer.

**`ViewRay { origin, direction }`**, world space, `direction` a unit vector.

- `ViewTrackball::viewRay(pos, viewport)`: perspective, origin is the eye and the
  direction passes through the pixel; orthographic, the direction is the view
  direction and the origin is the pixel unprojected onto the near plane.
  `hitViewPlane`, `hitHyper` and `hitSphereLikeVcg` use it instead of
  `viewRayFromWindow`; their formulas are written in terms of `distance` and stay
  valid under the invariant, so the rotation feel is the same across a toggle.
- `CameraShot::viewRayThrough(pixel)` and `CameraShot::directionToCamera(worldPoint)`:
  the unit vector from a point *towards* the camera -- `normalize(viewPoint - p)` in
  perspective, `-viewDirection` in orthographic. It is what the raster filters need;
  the three call sites that write `viewPoint() - p` today
  (`filter_camera`, `filter_color_projection`) switch to it.
- **Occlusion** gets a direction form,
  `ViewpointOccluder::isOccludedAlong(sample, towardViewer)`, alongside the existing
  `isOccluded(sample, eye)`. `select_by_rectangle` with *visible only* picks the form
  from the camera's projection; for orthographic the direction is carried into the
  layer's local space with the inverse of the layer matrix. Both backends already
  answer a *bounded* query -- Embree a segment (`segmentUnoccluded`), the vcglib grid a
  ray whose hit distance is compared with the sample's -- so the direction form is the
  same query with its far end placed at the layer's bounding-box diagonal beyond the
  sample, which is outside the layer by construction.

The depth pick, the rectangle test, the transform tool (which unprojects a one-pixel
offset) and the measure tool (which projects points through the view-projection)
already work for either projection and are not touched.

## Shaders

Four fragment shaders light with the vector to the eye taken as `-viewPos`
(`fill_flat.frag:126`, `fill_smooth.frag:129` as the specular half-vector's `V`;
`fill_wire_overlay.frag:32` and `overlay_points.frag:41` as the light direction).
In orthographic that vector is the constant `(0, 0, 1)`.

The shaders need to know which projection is in use. **`lightDir.w`**, a component of
the main uniform block that is written as 0 and read by nothing
(`renderwidget_internal.h:52`), carries it: 0 for perspective, 1 for orthographic.
`overlay_points.frag`, whose block declaration stops short of `lightDir`, extends it.
No binding or buffer layout changes.

Unchanged, and worth recording why:

- **Object-space point size** projects an offset of the requested length and reads
  `w` from the result; in orthographic `w` is 1 and it gives the plain scale.
- **The trackball gizmo's back-hemisphere shading** uses the direction from the orbit
  centre to the eye, which is the view axis in both projections.
- **Radiance scaling** stores `log(-viewZ)`. It keeps working, but its parameters were
  tuned against a perspective depth where the logarithm normalizes for distance; in
  orthographic its strength follows the arbitrary `distance`. This is the one place
  the result may look different rather than wrong; see [Open questions](#open-questions).

## Camera glyphs

Both glyphs draw a pyramid: four edges from an apex to the corners of the far
rectangle, plus the near and far rectangles.

- **A peer view's camera** (`buildViewFrustumGizmo`) reads `cot(fovY/2)` out of
  `proj(1,1)`, which for an orthographic matrix is `2/H` and means nothing as a
  cotangent. It must recognize an orthographic matrix -- its last row is
  `(0, 0, 0, 1)` -- and draw a box: twelve segments, the four depth edges parallel,
  no apex. `PeerViewCamera` already carries the matrix, so nothing else is passed.
- **A raster layer's camera** (`renderwidget_raster.cpp`) builds its glyph from an
  apex and four unprojected corners. For an orthographic shot the apex lines become
  the four parallel edges of a box from the image plane to the glyph depth.

## Cameras as data

`CameraShot` is the camera of a raster layer and the form a view takes when it is
exported -- a snapshot added as a raster, `cameraShotForViewport()`, the
`Render from Render-State JSON` filter's output, `HeadlessRenderContext::cameraShot()`.

- **`CameraShot::CameraType`** is reduced to `Perspective` and `Orthographic`.
  `fromVcgShot` maps vcglib's `ISOMETRIC` and `CAVALIERI` to `Perspective`, and the
  project loader, which knows the file, logs a warning naming it: nothing downstream
  can honour those types, and they come only from older projects.
- **`cameraShotForViewport()`** (`renderwidget.cpp:1380`) hard-codes `PERSPECTIVE`. For
  an orthographic trackball it builds an `ORTHO` shot whose frustum is the view
  volume: `ViewportPx` the requested size, `CenterPx` its middle, `PixelSizeMm` the
  world units per pixel (`H / height`), and `FocalMm` unused. `Project`, `UnProject`
  and `Depth` already treat `ORTHO` correctly, so a snapshot added as a raster layer
  projects onto the mesh in the same place the view drew it.
- **vcglib, two changes in `vcg/math/camera.ipp`**, the only work outside this
  repository. It follows the four-part procedure (commit in `vcglib/`, push to
  `origin/devel`, bump the gitlink, land the dependent MeshLab code in the same bump).
  `GetMatrix` has exactly one caller in the tree, `CameraShot::projectionMatrix()`.
  - **`GetMatrix` gets the missing orthographic branch.** For an `ORTHO` camera it
    builds the `glOrtho` matrix from the frustum extents -- `2/(r-l)`, `2/(t-b)`,
    `-2/(f-n)` on the diagonal, the translations in the last column, a last row of
    `(0, 0, 0, 1)` -- with the same clip-space convention as the perspective branch.
    Today it builds the perspective matrix whatever the type: for an `ORTHO` camera
    the GPU path disagrees with `Shot::Project` by hundreds of pixels (measured: 399 px
    worst case over random points).
  - **`GetFrustum` has the horizontal extent mirrored.** Found while measuring the
    first change: `SetFrustum(left -1, right 3, ...)` followed by `GetFrustum` returns
    left -3, right 1. The vertical extents round-trip; the horizontal ones do not.
    `GetFrustum` returns `right = cx * p` and `left = -(W - cx) * p`, where `Project`
    and `SetFrustum` both mean `left = -cx * p` and `right = (W - cx) * p` (`cx` the
    principal point in pixels, `p` the pixel size, `W` the width). The two agree only
    when the principal point is centred, which is why nothing shows it on a synthetic
    camera. With an off-centre principal point -- every calibrated photogrammetry
    camera -- the existing perspective matrix places the optical axis at pixel
    `W - cx` instead of `cx`: an error of `2 * |W/2 - cx|` pixels, in x only. The fix
    is the same two lines in `GetFrustum`, and it corrects `GetMatrix` and the GL
    wrapper (`wrap/gl/camera.h`) for every user at once. It is a change of behaviour
    for off-centre cameras and must be said so in the commit message.

  With both changes, `GetMatrix` followed by the view matrix agrees with
  `Shot::Project` to under 1e-3 pixel for random points, for both camera types and for
  centred and off-centre principal points (measured on a scratch copy of the
  functions). `CameraShot::projectionMatrix()` then needs no change, and the raster
  overlay and the projected-raster pass, which both call it, become correct for an
  orthographic raster -- and for an off-centre perspective one, which they are not
  today.
- **The camera filters** (`filter_camera`) write a `trackball` payload and a shot with
  `PERSPECTIVE` hard-coded (`camerafilterplugin.cpp:633`, `:716`). They keep the
  projection of the camera they start from, and *Set Camera from Direction* and *Set
  Camera to View Selection* gain a `projection` parameter whose default is "current".

## Interface

Added last, so that nothing can be switched on before everything works:

- **View menu, "Orthographic Projection"**, checkable and per view, bound to the `5`
  key (the key Blender users expect, and the one Marco Callieri asked for). It sets
  `State::projection` and nothing else.
- **The on-view readout** that says `FOV: 45.0 deg` says `Orthographic` instead.
- **Preference `view.projection`** (`perspective` | `orthographic`, default
  perspective): the projection a new or reset view starts in, beside
  `view.fieldOfView`. *Reset camera* keeps the current projection.
- **Canonical views** (the numeric-keypad views of the same request) need nothing
  beyond this: `snapCameraToAxis` keeps the field of view and the projection, and a
  canonical view in orthographic is the usual way to read one.
- The quick-help overlay and the Python API documentation describe the new key and the
  new JSON field.

## Mapping table

Every place that handles a camera, measured against the source. *Unchanged* rows are
listed so that the audit can be checked: each says why it needs nothing.

| # | Area | Where | Today | Verdict |
|---|---|---|---|---|
| 1 | Camera state | `viewtrackball.h:20` | no projection field | **modify**: add `projection` |
| 2 | State JSON | `viewtrackball.cpp:51`, `renderwidget.cpp:196,419,1066` | reader, writer and equality list fields by hand | **modify**: add the key to all three |
| 3 | Undo view state | `viewstate.h` | holds `State` by value | unchanged: inherits the field |
| 4 | View split and sync | `mainwindow.cpp:1751,1842`, `applySynchronizedTrackballState` | copies `State` / camera JSON | unchanged: inherits |
| 5 | Render-state JSON | `renderwidget.cpp:1149` | embeds the trackball payload | unchanged: inherits |
| 6 | Projection matrix | `viewtrackball.cpp:431` | `perspective()` only | **modify**: orthographic branch |
| 7 | Near and far planes | `viewtrackball.cpp:230`, `.h:70` | ratios of `distance` | **modify**: extents-based in orthographic |
| 8 | Peer-glyph far plane | `renderwidget_frame_plan.cpp:401` | widens far only | **modify**: widen both sides |
| 9 | Pan | `viewtrackball.cpp:352` | `distance * tan(fov/2)` | unchanged: that is `H/2` |
| 10 | Wheel and Shift+wheel | `viewtrackball.cpp:366` | dolly; dolly-zoom with Shift | **modify**: Shift+wheel ignored in orthographic |
| 11 | Gizmo radius | `viewtrackball.cpp:219` | `distance * tan(fov/2)` scaling | unchanged: a statement about `H` |
| 12 | Framing | `setFrame`, `resetToFrame`, `renderwidget.cpp:1630` | `distance` from radius and field of view | unchanged: same |
| 13 | Canonical views | `renderwidget.cpp:2397` | `setFromLookAt` keeps field of view | unchanged: keeps projection too |
| 14 | Arcball hits | `viewtrackball.cpp:456,490,514,561` | rays start at the eye | **modify**: use `viewRay` |
| 15 | View ray | `ViewTrackball` | none | **new**: `viewRay`, `ViewRay` |
| 16 | Depth pick | `renderwidget_selection.cpp:181` | unprojects through the inverse MVP | unchanged: agnostic |
| 17 | Rectangle test | `selectfilterplugin.cpp:196` | projects through the MVP | unchanged: agnostic (`w` is 1) |
| 18 | Visible-only occlusion | `selectfilterplugin.cpp:197,321`, `viewpointoccluder.h` | one eye for all faces | **modify**: `isOccludedAlong` |
| 19 | Transform and measure tools | `transformtool.cpp:145`, `measuretool.cpp:215` | unproject a pixel offset; project through the MVP | unchanged: agnostic, and the transform tool says so |
| 20 | Rubber-band tool | `rubberbandselecttool.cpp:182` | sends `camera_state` JSON | unchanged: inherits |
| 21 | Specular eye vector | `fill_flat.frag:126`, `fill_smooth.frag:129` | `-viewPos` | **modify**: constant in orthographic |
| 22 | Eye-light | `overlay_points.frag:41`, `fill_wire_overlay.frag:32` | `-viewPos` | **modify**: same |
| 23 | Radiance scaling | `fill_radscale_buf.frag:70` | `log(-viewZ)` | unchanged in code; see open questions |
| 24 | Object-space point size | `overlay_points.vert`, `depth_pick.vert` | projects an offset | unchanged: `w` is 1 |
| 25 | Trackball gizmo shading | `overlay_trackball_gizmo.vert` | centre-to-eye direction | unchanged: the eye is on the axis |
| 26 | Projection flag | `renderwidget_internal.h:52` | `lightDir.w` unused | **new**: carries it |
| 27 | Peer camera glyph | `viewfrustumgizmo.cpp:12` | reads `cot(fov/2)` from the matrix | **modify**: box for an orthographic matrix |
| 28 | Raster camera glyph | `renderwidget_raster.cpp:170` | apex and four rays | **modify**: box for an `ORTHO` shot |
| 29 | Peer camera provider | `mainwindow.cpp:1613` | passes the matrix | unchanged: the glyph reads it |
| 30 | View to shot | `renderwidget.cpp:1380` | hard-codes `PERSPECTIVE` | **modify**: `ORTHO` shot |
| 31 | Offscreen, headless, Python, filter renders | `renderOffscreenToImage`, `HeadlessRenderContext`, `MlGui`, `render_from_render_state_json` | render through a `RenderWidget` from the JSON | unchanged: inherit (verified by a probe) |
| 32 | PNG camera metadata | `mainwindow.cpp:2661` | writes the camera JSON | unchanged: inherits |
| 33 | Python camera JSON | `module.cpp:135` | passes the JSON through | unchanged: inherits; docs gain the key |
| 34 | Camera filters | `camerafilterplugin.cpp:554-720` | trackball JSON and shot, `PERSPECTIVE` fixed | **modify**: keep or set the projection |
| 35 | vcglib projection matrix and frustum | `camera.ipp:144,184` | perspective whatever the type; horizontal frustum extent mirrored | **modify**: orthographic branch, and `GetFrustum` x (vcglib) |
| 36 | `CameraShot::projectionMatrix` | `camerashot.cpp:244` | forwards to row 35 | unchanged once 35 lands |
| 37 | Camera type enum | `camerashot.h:26`, `camerashot.cpp:23,35` | four types | **modify**: two |
| 38 | Shot rays | `CameraShot` | none | **new**: `viewRayThrough`, `directionToCamera` |
| 39 | Raster filters | `camerafilterplugin.cpp:508,527`, `colorprojectionfilterplugin.cpp:172,246,375` | `viewPoint() - p` | **modify**: `directionToCamera` |
| 40 | Raster overlay and projection pass | `renderwidget_render.cpp:453` | `shot.projectionMatrix` | unchanged once 35 lands |
| 41 | Layer camera tooltip | `layerwidget.cpp:661,673` | type name and eye point | unchanged: already names `Orthographic` |
| 42 | Interface | View menu, overlay readout, `view.projection` | none | **new** |
| 43 | Quick help and Python docs | `quick_help_overlay.html`, API docs | perspective only | **modify** |
| 44 | Reference docs | `rendering.md` camera models and render-state JSON, `data_model.md` | perspective only | **modify** |
| 45 | Tests | see below | none | **new** |

**45 rows: 21 unchanged, 19 modify, 5 new.** Of the 19, one (row 35) is in vcglib.

## Staging

The interface (row 42) lands last in every stage plan, so that no stage leaves a
reachable orthographic view that some consumer cannot draw.

1. **vcglib.** Row 35 alone, with a test in this repository (vcglib has no test suite
   of its own) that `CameraShot::projectionMatrix()` through the view matrix agrees with
   `CameraShot::project()` for both camera types and for centred and off-centre
   principal points. Independent of everything else and landed by the four-part
   procedure; the off-centre correction is worth reporting to whoever uses raster
   overlays before it lands, since it moves them.
2. **The view.** Rows 1, 2, 6, 7, 8, 10, 14, 15, 21, 22, 26. After it an orthographic
   state, set by JSON or the Python API, renders, navigates and lights correctly in a
   view, a snapshot and a headless render; there is no menu item yet.
3. **Cameras as data.** Rows 27, 28, 30, 34, 37, 38, 39 (and 36 and 40 follow from
   stage 1). Peer and raster camera glyphs, snapshot-to-raster, the camera filters
   and the raster filters.
4. **Occlusion.** Row 18.
5. **Interface and documentation.** Rows 42, 43, 44.

## Tests

`ViewTrackball` has no unit test today, so stage 2 adds one target for it.

- **The invariant.** For random states, a point on the plane through the orbit centre
  projects to the same normalized coordinates in both projections; a point `H/2`
  above the centre projects to `y = 1` in orthographic at any `distance`.
- **Rays.** Perspective rays through a state share one origin; orthographic rays share
  one direction and their origins lie on the near plane; `project(unproject(p)) == p`
  for both, through `CameraShot` as well.
- **JSON.** Round trip; `perspective` omitted from the output; an absent key reads as
  perspective; an unknown value fails naming the key.
- **Glyphs.** `buildViewFrustumGizmo` on an orthographic matrix gives twelve segments
  with the four depth edges parallel.
- **vcglib.** Through `CameraShot`: the projection matrix against `project()` and
  `depth()` for `ORTHO` and `PERSPECTIVE`, with the principal point centred and
  off-centre in x and in y; and the `SetFrustum` / `GetFrustum` round trip.
- **Selection.** `select_by_rectangle` with *visible only* in orthographic selects the
  face toward the viewer and not the one behind it.
- **Raster filters.** A normal orientation against an orthographic raster camera uses
  the view direction, not the direction to the camera's position.
- **Render probe** (`tests/render_probes/orthographic`, through the application's own
  `--generate-docs` hook like the existing probes): a unit cube drawn through the
  render-state JSON in both projections. In orthographic the silhouette of the
  front-facing cube is a square, with the back face hidden behind it; in perspective
  it is not. A perspective render at the minimum field of view with `H` held constant
  differs from the orthographic one by less than a stated bound, which is the
  small-field-of-view limit of [the invariant](#the-invariant) measured. A lit plane
  facing the camera has a uniform specular term in orthographic and a falling one in
  perspective. The same JSON through `HeadlessRenderContext` gives the same image.

## Open questions

- **Radiance scaling.** Its parameters follow `distance` in orthographic. Normalizing
  by `H` instead would make it independent of zoom; measure on a real scan before
  deciding, and leave it alone if the look is acceptable.
- **Near and far when zoomed far in.** With `distance` much smaller than the scene the
  orthographic `near` is deeply negative. It is correct, but depth resolution is
  spread over the whole scene extent; check it on a large scan.
- **`isOccludedAlong` and grazing rays.** The tolerance the grid backend applies is
  proportional to the sample's distance (`dist * 1e-3`); with the far end placed one
  bounding-box diagonal away that tolerance is larger than for a perspective eye close
  to the layer. Measure the false-negative rate on a real scan, and scale the tolerance
  with the layer rather than with the query if it is too loose.
