# Raster Alignment

This document proposes aligning a raster's camera to a mesh, so that a photo whose camera is
missing or slightly wrong can be registered on the model and then used by the filters that
project through rasters (color transfer, coverage, parametrization). It is Stage 3 of the
[Camera Lens Model](camera_lens_model.md), which gave rasters a pinhole camera plus a separate
lens and a native COLMAP import. **Written 2026-11-03; Stage 3a, the correspondence fit,
implemented the same day. Stages 3b to 3d are not, and the open questions not about 3a are
still open.**

See also: [Camera Lens Model](camera_lens_model.md) (the boundary this relies on),
[Data Model](../data_model.md#raster-data-type), [Rendering](../rendering.md) (the
`RasterImage` view), [TexMetro](texmetro.md) (the case for a CPU rasterizer over a QRhi one),
[Vocabulary](../vocabulary.md).

## What old MeshLab has

Measured on 2026-11-03 in `.reference/meshlab/src/meshlabplugins`.

Three plugins, 10,846 lines, all built on one core that was copied into each of them and has
since diverged (`mutual.cpp`, `parameters.cpp`, `solver.cpp` and `alignset.cpp` differ
between the copies):

- **`edit_mutualcorrs`**, the interactive tool. The user picks pairs of a point on the mesh
  and a point on the photo, in the single 3D view with the photo shown over it, then
  presses Apply. With two or more pairs it fits the camera to them (levmar), and then, unless
  the slider is all the way to correspondences, refines it by mutual information. The pairs
  can be saved to and loaded from a text file.
- **`filter_mutualinfo`**, *Image alignment: Mutual Information*, the method of Corsini et
  al. 2009. Starting from a camera, it maximizes the mutual information between the
  photo in grey levels and a rendering of the mesh through the camera.
- **`filter_mutualglobal`**, *Global refinement using Mutual Information* (Dellepiane et
  al. 2013). It builds a graph of the visible rasters that overlap, and refines each camera
  against renderings of the mesh painted with its neighbours' photos, so that the images
  agree with each other as well as with the geometry.

How the core works:

- **Rendering is OpenGL.** `AlignSet` renders the mesh into an FBO with one of six GLSL 1.10
  programs, *Combined*, *Normal map*, *Color per vertex*, *Specular*, *Silhouette* and
  *Specular combined*, and reads back one channel. *Combined* blends the normal map with the
  vertex color by the weight `t = color.r²`. The vertex color is expected to hold ambient
  occlusion. The longest side is 800 px. The photo is scaled to that size ignoring its
  aspect, and turned to grey as `0.3 R + 0.59 G + 0.11 B`.
- **The metric** is the mutual information of a 128 × 128 joint histogram (`mutual.cpp`, 130
  lines). The row of background pixels is divided by a *background weight*, 2 by default.
- **The camera parameters** are 6, or 7 with the focal length. Three are a translation, and
  three are Euler angles of a rotation about the bounding-box centre, each scaled so that a
  unit step moves the projected vertices by about one pixel (`Parameters::initScale`). Which
  vertices are measured is chosen with `rand()`, so two runs can scale differently.
- **The optimizer** for mutual information is NEWUOA (Powell's derivative-free method,
  `vcglib/wrap/newuoa`, MIT). The correspondence fit is levmar (GPL-2.0, downloaded at
  configure time) on Euler angles and translation, followed by a separate fit of the focal
  length alone (`LevmarMethods::calibrate`). `filter_mutualinfo` and `filter_mutualglobal`
  link levmar too, but nothing in either filter calls it.

**What must not be carried over.** All three plugins finish by writing the camera back with
`CenterPx` forced to the image centre and `ViewportPx` and `PixelSizeMm` recomputed from
the image size. That throws away a calibrated principal point: run on a COLMAP camera, they
would move it by as much as the principal point is off centre. They also assume the photo is
pinhole, which since Stage 1 holds by construction.

## What meshlab2 already has

- **Rasters with a pinhole camera and a lens** (`RasterEntry::shot`, `RasterEntry::lens`). The
  image in memory is already undistorted (`Document::ensureRasterPlaneImage`), so a pick on
  the displayed image is a pick in the pinhole camera's pixels. `Document::setRasterShot` is
  the undoable way to change a camera.
- **The `RasterImage` view** (`renderwidget_raster.cpp`) shows the current raster with the
  visible meshes rendered through its camera on top, at an adjustable opacity
  (`Ctrl+Wheel`), with pan and zoom. `rasterScreenToImage` already maps a screen position to
  image coordinates. That is the overlay the old tool built by hand. Depth picking is off in
  this mode today.
- **Interactive tools** (`src/render/interactivetool.h`): engage and disengage, a tool strip,
  an asynchronous GPU surface pick (`requestSurfacePick` → `onSurfacePicked`), and one
  `runFilter` per committed gesture, so that each commit is one undo step and one scriptable
  call. `MeasureTool` is the nearest model, since it picks points and commits a filter.
- **A CPU rasterizer through a raster camera**: `src/core/softdepthbuffer.h`, 137 lines,
  `buildDepthBuffer(shot, mesh, transform)`. *Transfer Color*, the coverage filters and
  *Parametrize from Registered Rasters* already use it.
- **NEWUOA**, in `vcglib/wrap/newuoa/include/newuoa.h`. vcglib uses it in
  `point_matching_scale.h`, and `filter_isoparam` uses it where its upstream had levmar
  ([UPSTREAM.md](../../../plugins/filter_isoparam/UPSTREAM.md)). Eigen's
  `LevenbergMarquardt` is in `vcglib/eigenlib/unsupported`, but nothing in meshlab2 uses
  Eigen's unsupported modules.
- **No camera-resection code** (camera from 2D–3D pairs, PnP or DLT) anywhere in vcglib.
- **Camera filters** (`plugins/filter_camera`, 11 filters, `Document/Camera`). *Set Raster
  Camera* takes a viewpoint, a direction and a focal length, but cannot take the current
  view. Old MeshLab's *Get Shot* button had no equivalent until now.
- **Test data with ground truth.** The gargoyle fixture (4 photos of 750 × 502, 20k faces,
  normals but no color) has calibrated cameras. The optional RealityScan dataset
  `2930Colmap` has 35 photos with COLMAP cameras, 261,316 2D–3D observations, and a
  3.2 M-face textured mesh (`2930_35.ply`) in the same frame (`project.mlp`).

## Proposal

**One optimizer.** Use NEWUOA for both the correspondence fit and mutual information. Mutual
information needs a derivative-free method anyway, and NEWUOA is already vendored and
already used. For the correspondence fit, a few hundred evaluations of a 7-parameter
reprojection error take microseconds. This **changes what
[Camera Lens Model](camera_lens_model.md) said**, which named Eigen's
`LevenbergMarquardt`: that would be a second optimizer for the smaller of the two problems.

**One parametrization**, shared by both:

- A rotation increment as a rotation vector, not Euler angles, applied about the centre of
  the mesh's bounding box.
- A translation, plus optionally the focal length.
- The same per-parameter scaling, so that one unit moves the projection by about one pixel,
  measured on a deterministic sample of vertices (a fixed stride, not `rand()`).
- The principal point, `ViewportPx` and the lens are **never touched**. The result changes
  only what the parameters say it changes.

**Mutual information renders on the CPU**, through the existing rasterizer:

- Extend `rasterizeTriangleDepth` with an optional per-pixel face index, an item buffer of a
  few lines. Mutual information shades from that buffer: the normal map, or vertex color or
  vertex scalar. The renderer is then deterministic, runs in `ctest` and `pymeshlab` without
  a GPU, and needs no GL or QRhi context inside a filter.
- That is the [TexMetro](texmetro.md) argument again. A GPU path would go through
  `HeadlessRenderContext`, which needs a `RenderWidget`, and no Qt test can link one.
- **Cost, estimated, not measured:** one 800 × 533 rendering of a 3.2 M-face mesh is
  dominated by triangle setup, on the order of 50–100 ms single-threaded, and NEWUOA takes
  200–600 evaluations. The first step of Stage 3c measures it. Tiling across threads is the
  first remedy if it is too slow.

**Correspondences are a filter parameter, not document state.** The pairs travel as a
`string` parameter holding JSON: an array of `{ "point": [x, y, z], "pixel": [u, v] }`, with
the pixel in the working image, y down, centres at k + 0.5. That is all it takes for the fit
to be one undoable, scriptable, replayable `runFilter`. The tool keeps the list while it is
being edited. It writes the list to a file and reads it back, as the old tool did, through
the file dialogs and not as filters.

**The tool works in the `RasterImage` view.** The user drags from where a feature is on the
rendered mesh to where it is on the photo. The press is a surface pick through the raster
camera, which becomes the 3D point. The release is the image point. The tool draws the pairs
as short lines over the image. Fit commits the filter, and the overlay updates as soon as the
camera changes. This needs depth picking in `RasterImage` mode, which reuses the frame's
projection and pan and zoom, and a rough camera for the mesh to appear at all. A raster with
no camera gets one from the 3D view through *Set Raster Camera* (below).

**Filters**, in `filter_camera`, category `Document/Camera` (see open question 4):

| Name | Python | Parameters | Result |
|---|---|---|---|
| *Set Raster Camera* (extended) | `set_raster_camera` | new `source`: *Parameters* (today's behaviour) or *View*, plus a `camerastate` parameter defaulting to the current view | the current raster gets the view's camera, fitted to its image size: old MeshLab's *Get Shot* |
| *Align Current Raster to Mesh by Point Correspondences* | `align_current_raster_to_mesh_by_point_correspondences` | `correspondences` (JSON), `estimateFocal` | the camera that minimizes the RMS reprojection error, which is reported (RMS, max, per pair) in `outputValues`; refuses with fewer than 3 pairs, or 4 with the focal length |
| *Align Current Raster to Mesh by Mutual Information* | `align_current_raster_to_mesh_by_mutual_information` | `rendering` (*Normal Map*, *Combined*, *Vertex Color*; see open question 3), `estimateFocal`, `maxIterations`, `resolution` (longest side, 800), `backgroundWeight` (2) | the refined camera; the mutual information before and after, and the projected displacement of the bounding box, in `outputValues` |

**Where the code goes**, as AGENTS.md asks:

- **The correspondence fit goes to vcglib**: `vcg/math/shot_resection.h`, about 100 lines.
  Given a `Shot` and 2D–3D pairs, it returns the `Shot` that minimizes the reprojection
  error with NEWUOA, with the parametrization above. It depends on nothing of MeshLab's, and
  the same parametrization serves mutual information, which includes it.
- **Mutual information stays in the plugin.** It depends on the rasterizer, which is
  MeshLab's (`CameraShot`, `QMatrix4x4`). Moving the rasterizer into vcglib would make it
  general too, but that is a migration of three consumers and needs its own table. It is
  not part of this proposal (open question 1).

## Mapping table

What happens to each piece of old MeshLab's three plugins. Lines are from `wc -l` on
2026-11-03. *Front end* is the plugin class, the dialog, the factory and the `.ui` file.

| Component | `edit_mutualcorrs` | `filter_mutualinfo` | `filter_mutualglobal` | Total | Verdict |
|---|---:|---:|---:|---:|---|
| Front end | 1,757 | 393 | 1,260 | 3,410 | **Rewritten**: the tool (corrs) and two filter descriptors (mutualinfo); global refinement deferred (Stage 3d) |
| `alignset` (GL rendering, FBO, shaders) | 447 | 421 | 1,416 | 2,284 | **Replaced** by the CPU item buffer plus shading |
| `solver` | 605 | 790 | 891 | 2,286 | **Replaced** by NEWUOA calls; the commented-out levmar paths are not ported |
| `parameters` | 334 | 335 | 333 | 1,002 | **Rewritten** in `shot_resection.h`: rotation vector, deterministic scaling |
| `levmarmethods` | 288 | 288 | 288 | 864 | **Dropped**, together with the levmar dependency |
| `mutual` (joint histogram, MI) | 156 | 156 | 156 | 468 | **Ported**, about 60 lines, in the plugin |
| `shutils.h` (GLSL helpers) | 104 | 104 | 104 | 312 | **Dropped** |
| `pointCorrespondence`, `pointOnLayer` | 0 | 110 | 110 | 220 | **Dropped**: the JSON parameter |
| **Total** | **3,691** | **2,597** | **4,558** | **10,846** | |

Estimate of the new code: the vcglib header (≈100), the item buffer (≈20), the
mutual-information filter (≈300), *Set Raster Camera* from the view (≈30), the
correspondence filter (≈80), depth picking in `RasterImage` (≈40), the tool (≈400). That
is **about 1,000 lines** against 6,288 for the two plugins it replaces. Global refinement is
not counted.

## Stages

Each stage is one commit, landed and verified before the next.

- **3a. Correspondence fit.**
  - Add `shot_resection.h` to vcglib with a sample, plus *Align … by Point Correspondences*
    and *Set Raster Camera* from the view.
  - Tests, behavioural tier:
    - **Exact pairs.** Project gargoyle vertices through the true camera, perturb the camera,
      and expect it recovered to 1e-3 px, with and without the focal length.
    - **Real pairs.** On `2930Colmap`, when present, use COLMAP's own observations of one
      camera as the pairs, perturb the camera, and compare the fit with COLMAP's camera.
  - A vcglib change, so the four-part landing applies.

  As built (2026-11-03), with open questions 2, 4, 5 and 8 taken as recommended: NEWUOA, the
  name in `Document/Camera`, the JSON parameter, and the focal length as the only intrinsic:

  - **`vcg/math/shot_resection.h`**, 214 lines with its comments: `ShotResection<S>::Fit` and
    `RmsError`, in viewport pixels as `Shot::Project` gives them. The rotation and translation
    are in the camera's own frame, about the **centroid of the points**, not the mesh's
    bounding box: the fit sees only the points. NEWUOA runs in rounds, each measuring the
    scales again where the last ended, until a round gains less than the tolerance. It throws
    `std::invalid_argument` for too few points, points all in one place, or an orthographic
    shot. Sample `apps/sample/space_shot_resection`.
  - **Measured robustness**, on a synthetic 4000 × 3000 camera with random points and 200
    random starts per case: with 8 or 30 points, every start converges to 1e-12 px, up to
    turns of about 17° and moves of 30% of the distance, with or without the focal length.
    With 4 points, the near-minimal case, 1 to 3 starts in 200 fail at the larger
    perturbations. A fit takes 0.3 to 2 ms.
  - **`vcg::Camera<S>::Construct` from `Camera<Q>`**, which never compiled when the scalar
    types differed: `CameraType` is a distinct nested enum in each. It is cast now, the one
    line the filter needed to make a `Shotd` of MeshLab's `Shotf`.
  - **The filter** turns the JSON pixels (y down) into vcg's, fits in double, and writes the
    camera through `setRasterShot`. It refuses an orthographic camera, a raster with no camera
    or too few pairs. It returns `rmsBefore`, `rmsAfter`, `maxAfter` and `pairErrors`. The
    pixels are in the camera's viewport, which is the image's size whenever the image matches
    its camera, the usual case.
  - ***Set Raster Camera* from a view** is a bool, `fromView`, not the `source` enum the table
    named: `enabledWhen` can only follow a bool, and this way it greys out the five other
    parameters. The camera keeps the view's rotation, roll included, through
    `rasterShotFromView`. The framing filters' raster camera now comes from the same helper,
    in place of a second `LookAt` that rebuilt it from the eye and centre. *Set Raster Camera*
    used to assign the shot directly, so no camera revision was bumped and no view heard of
    it; both of its paths now go through `setRasterShot`.
  - **Tests** (`test_filters.cpp`):
    - *Set Raster Camera* from a view, perspective and orthographic with a 25° roll, puts
      every cube corner within 0.01 px of where the view draws it.
    - On the gargoyle, every vertex seen by one camera (one in 397), with the camera turned
      5° and moved 5% of the model's size (16 to 38 px off): each of the four cameras comes
      back to under 1e-4 px of the true one, also with the focal length 10% off. The
      principal point and pixel size are untouched, one undo restores the start, and bad
      JSON, malformed pairs and too few pairs are refused.
    - On `2930Colmap`, raster 7: its 5,433 COLMAP observations are undistorted through its
      lens and the camera is started 2° and 2% away (83.5 px off). It lands 0.050 px from
      COLMAP's camera, with an error of 0.5917 px against COLMAP's 0.5937, in 25 ms. The fit
      may come out slightly lower because COLMAP adjusted the points too.
- **3b. The tool.** Depth picking in `RasterImage` mode and the drag-to-pair tool. Picking is
  in the GUI, so the checks are the tool's unit logic plus a hand test on gargoyle. A render
  probe if picking in this mode needs one.
- **3c. Mutual information.**
  - First, measure one rendering and one evaluation on gargoyle and on `2930_35.ply`.
  - Then the item buffer, the metric and the filter.
  - Tests: perturb each gargoyle camera by known amounts (rotation and translation that move
    the projection by 2, 5 and 10 px), run, and require the projected displacement from the
    true camera to fall below a threshold. The thresholds and the convergence basin are
    measured, not chosen in advance. The same test runs on the 2930 dataset, which is also the
    performance case.
- **3d. Global refinement.** Decided after 3c, from its measurements (open question 7).

## Open questions

1. **The rasterizer in vcglib?** The recommendation is to keep `softdepthbuffer.h` in core
   and extend it, so that the correspondence fit is the only vcglib addition. The
   alternative moves the rasterizer into vcglib (with a mapping table for its three
   consumers), after which mutual information could go there too.
2. **NEWUOA for the correspondence fit.** This reverses the lens proposal's mention of
   Eigen's Levenberg–Marquardt, for the reasons above. Agreed?
3. **Which renderings.** Old MeshLab offered six. *Normal map* needs nothing but normals.
   *Combined* needs ambient occlusion: in meshlab2, *Compute Mesh Ambient Occlusion* writes
   it to the vertex scalar, so *Combined* would read the scalar where old MeshLab read
   `color.r`.
   - What is the blend weight `t = color.r²` meant to do? Read literally, open areas show
     the normal map and occluded ones fade to their occlusion. Is that the paper's intent,
     and should the weight be kept as it is?
   - Do *Specular*, *Specular combined* and *Silhouette* earn a place?
   - Should *Vertex Color* become *Mesh Color*, so that it also reads the texture? The 2930
     mesh is textured and has no vertex color.
4. **Category and names.** `Geometry/Alignment` is the vocabulary's home for algorithmic
   alignment, but its root says *changes vertex positions or the layer matrix*, and a raster
   camera is neither. The proposal uses `Document/Camera`, beside the other camera filters,
   and *Align Current Raster to Mesh by …*, following *Align Layer to Reference by
   <Method>*. If the vocabulary prefers otherwise, it should be extended rather than
   bent.
5. **Correspondences as a JSON string parameter.** An alternative is to keep them in the
   document, for example as the planned annotations, so that they are saved with the
   project. The proposal keeps them out of the document until a need appears.
6. **Picking in one view.** The proposal picks both ends of a pair in the `RasterImage` view,
   which needs a rough camera first. The alternative picks 3D points in a 3D view and 2D
   points in a `RasterImage` view, which works with no camera at all, but would take the
   tool model (one tool pinned to one view) across two views.
7. **Global refinement.** Port Dellepiane 2013 as a third filter, defer it, or replace it
   with something else? It is 4,558 lines old, with the most GL in it (1,416 lines of
   `alignset`), and its graph of overlapping images is new code whatever happens.
8. **Which intrinsics.** The old code refines the focal length at most. Should the principal
   point be refinable when there are enough pairs? The lens stays fixed in this proposal;
   refining it would need `Distort` on every pick, as the lens proposal foresaw.
9. **A combined objective.** The old tool weighted mutual information against the
   correspondence error with a slider. It could be a weight parameter of the
   mutual-information filter, taking the same JSON pairs. Is it wanted, or is fitting first
   and refining after enough?
