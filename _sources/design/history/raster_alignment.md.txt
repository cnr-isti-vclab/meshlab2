# Raster Alignment, applied 2026-11-03

Aligning a raster's camera to a mesh, so that a photo whose camera is missing or slightly
wrong can be registered on the model and then used by the filters that project through
rasters (color transfer, coverage, parametrization). It is Stage 3 of the
[Camera Lens Model](../proposals/camera_lens_model.md), which gave rasters a pinhole camera
plus a separate lens and a native COLMAP import. **Proposed and carried out on 2026-11-03,
in four stages: 3a, the correspondence fit; 3b, the tool; 3c, mutual information; 3d, global
refinement. The open questions were decided the same day** (see the end). How it works today
is in [Raster Alignment](../raster_alignment.md); this is the proposal as written, followed by
what was built at each stage and what was measured.

See also: [Camera Lens Model](../proposals/camera_lens_model.md) (the boundary this relies on),
[Data Model](../data_model.md#raster-data-type), [Rendering](../rendering.md) (the
`RasterImage` view), [TexMetro](../proposals/texmetro.md) (the case for a CPU rasterizer over
a QRhi one), [Vocabulary](../vocabulary.md).

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
[Camera Lens Model](../proposals/camera_lens_model.md) said**, which named Eigen's
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
- That is the [TexMetro](../proposals/texmetro.md) argument again. A GPU path would go through
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

  As built (2026-11-03), with open question 6 taken as recommended, both ends of a pair in the
  raster view:

  - **The view.** `InteractiveTool::supportsRasterView()`, like `supportsUvView()`, lets a tool
    have input, its overlay and surface picks in the raster view. `executePendingDepthPick`
    takes the matrices to project with from its caller: the trackball's in the 3D scene, and
    in the raster view the ones that frame was drawn with, the raster's camera behind the
    image's pan and zoom. Each raster-mode frame used to drop any pending pick, a guard from
    when nothing picked there; `scheduleDepthPick` now lets only a tool's pick through, so the
    guard went. `widgetToRasterImage` and `rasterImageToWidget` map the widget to the image
    and back.
  - **`RasterAlignTool`** (`src/render/tools/rasteraligntool.{h,cpp}`).
    - A left drag pairs the surface under the press with the image pixel under the release.
      The press's pick can report after the release, and the pair waits for both. A click is a
      pair too, of a point already in place; the first version dropped short drags, and the
      probe below lost a pair to that.
    - Each pair is drawn as a line from where the raster's camera projects its point to its
      pixel.
    - The controls are *Fit*, *Focal Length*, *Undo*, *Clear*, *Save* and *Load*; the files are
      the filter's JSON.
    - Pairs are kept per `rasterId` while the tool is engaged.
    - Engaging it switches the view to the raster when the raster has a camera.
    - Its icons are old MeshLab's `edit_mutualcorrs` ones.
  - **Checked on screen, not in ctest.** No Qt test can link `RenderWidget`, and a capture
    probe does not see the tool's overlay or picks. So a temporary hook in `main.cpp`, since
    removed, ran the real window:
    1. It loaded the gargoyle and moved camera 0 by 3° and 2% of the model, 9.03 px off.
    2. It engaged the tool and zoomed the view.
    3. It dragged from where the moved camera draws 9 visible vertices to where the true camera
       puts them, and pressed `F`.
    4. All 9 picks hit, the fit left 0.26 px root mean square on the pairs, and the camera came
       back to 0.69 px from the true one.
    The rest is the press being rounded to a whole widget pixel, which `requestSurfacePick`
    takes. The result is the same on Metal and on OpenGL, whose readback is flipped.
- **3c. Mutual information.**
  - First, measure one rendering and one evaluation on gargoyle and on `2930_35.ply`.
  - Then the item buffer, the metric and the filter.
  - Tests: perturb each gargoyle camera by known amounts (rotation and translation that move
    the projection by 2, 5 and 10 px), run, and require the projected displacement from the
    true camera to fall below a threshold. The thresholds and the convergence basin are
    measured, not chosen in advance. The same test runs on the 2930 dataset, which is also the
    performance case.

  As built (2026-11-03):

  - **Code.**
    - **vcglib.** The resection's parametrization is public now, as
      `vcg::ShotResection<S>::Parametrization`: the reference shot, sample points, a pivot and
      `fitFocal` give `Size()` parameters and `ToShot(x)`. `Fit` uses it as before (the stress
      run is unchanged: 5,991 of 6,000 random starts converge), and mutual information
      minimizes over the same numbers, pivoting on the centre of the visible layers' box, as
      old MeshLab did.
    - **The rasterizer.** `rasterizeTriangleDepth` calls `onPixel(index, b0, b1, b2)` where a
      triangle wins the depth test, which is the item buffer of the proposal in a few lines.
      The depth buffers pass a no-op.
    - **The filter.** `plugins/filter_camera/mutualinformation.cpp`, about 300 lines, renders
      **the visible layers**, not only the current one as old MeshLab did, since that is what
      the raster view shows. It shades as the old GLSL programs did, using the channel they
      read back: *Normal Map* (the eye-space normal's y, normalized per pixel), *Combined*
      (vertex color blended into that by `t = color.r²`, read from the color as old MeshLab
      did, not from the scalar) and *Vertex Color* (red). The histogram is 128 × 128 bins
      against the photo in grey levels, the rendered background is weighted
      1 / `backgroundWeight`, and the photo's transparent pixels (an undistorted photo's
      corners) are left out. NEWUOA runs in rounds, starting with `expectedError` (2) and
      stopping at `tolerance` (0.1) pixels of the working resolution, within `maxEvaluations`
      renderings (300). The run reports progress and can be cancelled.
  - **Measured on the gargoyle** (750 × 502, 20k faces, Normal Map; it has no vertex color):
    - About 3.8 ms a rendering, so 0.4 to 1 s a run.
    - The fixture's cameras are not at the maximum: started on them, the fit moves 0.5 to
      1.6 px and raises the information (0.6030 to 0.6056 bits on camera 0).
    - Started 0.5 to 8 degrees off (1 to 21 px), cameras 0, 2 and 3 end within 1.3 px of the
      fit from the true camera.
    - Camera 1, whose information is the lowest (0.45 bits), ends within 3.6 px, once at a
      higher maximum than the fit from the truth.
    - The test keeps the 2 and 8 degree starts, with bounds of 2 px (5 for camera 1).
  - **Measured on the RealityScan scan** (`2930Colmap/project.mlp`: 3.2 M faces, its sparse
    points hidden, raster 7, working at 800 px):
    - About 50 ms a rendering, as estimated; 7 to 14 s a run.
    - With *Vertex Color*, from the scan's own colors, a camera moved 1 degree (62 px in the
      full photo) comes back to 1.24 px from COLMAP's camera, and to 1.09 px when started on
      it.
    - With *Normal Map* it walks away: 13 px when started on COLMAP's camera, 19 px from the
      1 degree start, each time to a higher maximum of the information. On a photo of a
      real, colored object, shading alone is not enough here, and the rendering decides
      whether the method works. The optional test checks *Vertex Color* only.
- **3d. Global refinement.** Decided after 3c, from its measurements (open question 7).

  As built (2026-11-03), from the paper (Dellepiane and Scopigno 2013, kept in
  `.reference/papers/GlobalMutual_Final.pdf`) rather than from old MeshLab's code:

  - **Where the old code and the paper disagree, the port follows the paper:**

    | | Paper | Old `filter_mutualglobal` | Port |
    |---|---|---|---|
    | Which images paint the node | all images connected to it | the three largest overlaps: its shader had three slots | all, from the arcs into the node |
    | Weight of each painting image | the arc's value, MI × OV | the arc's MI alone | MI × OV |
    | The painting | the images' colors combined | `vertex color × photos / w`, read back in the blue channel | the photos' grey levels, weighted, as the target is |
    | Ties in the node order | the most arcs | the larger summed weight | the most arcs |
    | Convergence variation | mean absolute projection change, below 1.2 px | mean of the squared changes, compared with 1.2 | mean absolute change, in each raster's own pixels |
    | Anchors | nodes the user marks | hidden rasters | hidden rasters |

    The old code's connected components are not needed: the greedy order runs over all the
    nodes, and a node with no arcs has nothing to guide it, so it is skipped, as the old code
    skipped it. Its *Pre-alignment* option is *Align Current Raster to Mesh by Mutual
    Information* on each raster first, which can be run as such.
  - **Code.** `mutualinformation.cpp` is now 633 lines for both filters, up from 304. Shared:
    - a *view*, which is a raster at the working resolution with its photo in grey;
    - a renderer that paints the mesh with *projectors*, other views' photos, bilinear, where
      their own depth buffer says they see the surface, within 1%;
    - the refinement loop.

    The renderer now interpolates with **perspective-correct** barycentrics, as the GPU did;
    it needs the exact surface point under each pixel to project into the other cameras.
    That changed Stage 3c's results a little, which are re-measured below.
  - **The filter**, *Align Visible Rasters Globally by Mutual Information*:
    - It measures every arc into a visible raster: n × (n − 1) renderings at most.
    - It then refines one raster at a time, in the greedy order, against all its arcs,
      weighted. After each raster it weighs again the arcs that involve it.
    - It repeats the passes until one moves the projections of 5,000 samples by less than
      1.2 px on average, or after 5 passes.
    - The focal length is refined by default, as in the paper's seven parameters and old
      MeshLab's default.
    - The parameters and their defaults are otherwise Stage 3c's. Both filters now cite their
      papers (`referenceIds`).
  - **Measured on the gargoyle**, with the Normal Map:
    - All four cameras were moved by 1 to 2.5 degrees about different axes, 1.6 to 5.2 px
      off. They end 0.4 to 1.4 px from the fixture's cameras, in two passes (3.58 then
      0.38 px of variation), 1,391 renderings and about 10 s.
    - With camera 0 hidden at its true place, as an anchor, it does not move, and the others
      end 0.45 to 1.6 px off, in about 7 s.
    - The test asserts 1.5 and 2 px.
  - **Stage 3c, re-measured with perspective-correct rendering:**
    - Cameras 0, 2 and 3 end within 1.4 px of the fit from the truth from every start up to
      21 px off; before, it was 1.3 px.
    - Camera 1 ends within 4.9 px up to 6 degrees (15 px). From 7 degrees on it stops at lower
      maxima 10 to 12 px away, where before the change it had come back from 8 degrees. Its
      test now starts at 2 and 5 degrees.
    - The real scan comes back to 1.33 px of COLMAP's camera, against 1.24 before.
  - **Not measured: the global filter on the 35-photo scan.** At 50 ms a rendering, its
    1,190 arcs alone take a minute, and a pass about 7 minutes. A test that size does not
    belong in the suite.

## Open questions

All decided on 2026-11-03. Questions 2, 4, 5, 6 and 8 were carried out as recommended in
Stages 3a and 3b and not revisited; questions 1, 3 and 9 were answered by the maintainer
after Stage 3d.

1. **The rasterizer in vcglib?** Decided: it stays in MeshLab's core (`softdepthbuffer.h`),
   extended with the per-pixel callback; the correspondence fit is the only vcglib addition.
2. **NEWUOA for the correspondence fit.** Taken as recommended: one optimizer for both fits.
3. **Which renderings.** Decided:
   - **The default stays *Combined*.** The measurement against it is that on the 2930 scan
     the *Normal Map*, which is *Combined* on a mesh without color, finds a wrong maximum 13
     to 19 px away, while *Vertex Color* finds the right one.
   - ***Silhouette* is ported**: the layers white on black.
   - ***Specular* and *Specular combined* are not.**
   - **The blend weight `t = color.r²` is kept as old MeshLab had it.** What it was meant to
     do is not known. Read literally, open areas show the normal map and occluded ones fade
     to their occlusion.
   - **The AO source is unchanged.** *Combined* reads the occlusion from the vertex color's
     red, as before; meshlab2's ambient occlusion goes to the vertex scalar, so a
     scalar-to-color step comes first.
   - **Reading textures in *Vertex Color* is not done.**
4. **Category and names.** Taken as recommended: `Document/Camera`, *Align Current Raster to
   Mesh by …*, and for the global filter *Align Visible Rasters Globally by Mutual
   Information*, after *Align Meshes Globally*.
5. **Correspondences as a JSON string parameter.** Taken as recommended; the tool saves and
   loads them as files.
6. **Picking in one view.** Taken as recommended: both ends of a pair in the raster view.
7. **Global refinement.** Decided: ported as a third filter, from the paper where it and the
   old code disagree (Stage 3d).
8. **Which intrinsics.** Taken as recommended: the focal length at most; the principal point
   and the lens stay the calibration's.
9. **A combined objective.** Decided: wanted, in the filter and in the tool. Built as below.

## After the questions: Silhouette and mutual information with point pairs

- ***Silhouette*** joins the renderings of both mutual-information filters.
- **The point pairs.** *Align Current Raster to Mesh by Mutual Information* takes the same
  JSON point pairs as the correspondence filter (`correspondences`), parsed by one shared
  `parseCorrespondences`. With pairs it minimizes old MeshLab's score,
  (1 − w)(2 − MI) + w · d, where d is the pairs' mean distance in working pixels and w is
  `pairsWeight`.
- **The weight, measured on gargoyle camera 1 started 8 degrees off (20.6 px).** That start
  is beyond the reach of mutual information alone, which stops 12.95 px from the truth. Six
  pairs picked with up to 1 px of error were added, and the weight swept from 0 to 1:
  - w = 0.001: 6.5 px from the truth;
  - w = 0.01 to 0.5: 0.5 to 0.8 px from the truth;
  - w = 1, the pairs alone: 1.2 px.

  On camera 0, which mutual information reaches alone, w = 0.01 left the result 0.44 px from
  the information's optimum, against 0.40 without pairs, while 0.1 and above handed it to the
  pairs (0.7 to 0.8 px). The default is **0.01**, and the test holds camera 1 to 1.5 px
  (0.59 measured).
- **The tool** gained *Mutual Information* (`M`), which runs that filter with the tool's
  pairs, or without pairs when there are none. It only calls the filter the test covers, and
  was not run again in the window.
- **Leftovers.** The three `QJson` includes of `camerafilterplugin.cpp`, unused once the
  parsing moved, went. The unused `f01` in `rasterizeTriangleDepth` predates this work and is
  still there.
