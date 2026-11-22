# Camera Lens Model

This document proposes lens distortion for raster cameras, so that photos calibrated with a
real lens model (COLMAP, RealityScan, Metashape) can be used without undistorting them by
hand, and so that the image-to-mesh alignment tools that follow have a camera they can
refine. **Stages 0 (removing vcglib's distortion), 1 (the lens) and 2 (COLMAP import)
implemented 2026-11-02; Stage 3, raster alignment, was proposed and carried out on its own: [Raster Alignment](../raster_alignment.md), recorded in [history](../history/raster_alignment.md).** The tables and the open questions were approved the same
day. How the lens works today is in [Data Model](../data_model.md#raster-data-type); this
document keeps the plan and the record of how it was carried out.

See also: [Data Model](../data_model.md) (rasters, planes, the `.mlp` file),
[Rendering](../rendering.md), [Orthographic Camera](orthographic_camera.md) (the last
change to `CameraShot`), [Test Data](../test_data.md).

## Decisions

Taken 2026-11-02:

1. **A hard boundary between distorted and undistorted.** A raster's `CameraShot` is a
   pinhole camera, always, and everything that projects -- filters, depth buffers, the
   GPU raster view -- sees only that. The lens is a separate member of the raster that
   relates the pixels of the photo *as shot* (the file) to the pixels of the pinhole
   camera (the image in memory). It is applied in exactly two places: when a plane is
   decoded, and when a point picked on the photo as shot must be related to the camera
   (correspondences, imported keypoints). This is way A of the discussion of 2026-11-01;
   way B, a lens applied by every consumer, was rejected because the GPU path cannot
   apply it in a matrix and would need the undistorted image anyway.
2. **vcglib's distortion is removed entirely**, not kept beside the new model: the
   fields, the two conversion functions and the branches in `Camera::Project` and
   `UnProject`. `vcg::Camera` becomes a pure pinhole/orthographic camera.

## Where things stand

Measured on 2026-11-02.

**vcglib** has a one-parameter radial model in `vcg::Camera`: `std::array<S,4> k`
(only `k[0]` used) and `DistorCenterPx`. `Camera::Project` applies
`UndistortedToDistorted` when `k[0] != 0`, on the camera plane in millimetres, against a
centre converted to [-1, 1] coordinates -- two different frames. The cubic solver
(`camera.ipp:393`) forms Cardano's discriminant as `Q^3 + sqrt(R)` where the formula has
`Q^3 + R^2`, and takes one cube root of `abs((int)(R - D))`, marked *MODIFICATO DA ME*,
which truncates its argument to an integer. `DistortedToUndistorted` is a first-order approximation, so the two are not
inverses.

**Nothing uses it.** Of the 39 `.mlp` files in the repository, `tests/local` and
`.reference` (every MeshLab sample project), none has a non-zero `LensDistortion`. The two
vcglib importers that could fill it, `import_out.h` (Bundler) and `import_nvm.h`, set it to
0 on purpose ("To be uncommented when distortion is taken into account reliably").
MeshLab's own `.mlp` writer writes `"0 0"`. The only code that *sets* a non-zero `k[0]`
is `wrap/tsai/tsaimethods.cpp`, a Tsai calibration wrapper that nothing compiles: old
MeshLab's three mutual-information plugins once included it, and the include and the call
are commented out in all three (see [Old MeshLab](#old-meshlab)).

**It is already inconsistent in MeshLab.** `CameraShot::project` goes through
`vcg::Shot::Project`, so a project with a non-zero `LensDistortion` would project with
distortion on the CPU while the GPU raster view (`projectionMatrix`) and the interiors of
the depth buffer's triangles stay pinhole. No file triggers it today.

**Who projects through a raster camera** (`project`, `unproject`, `projectionMatrix`,
`viewRayThrough`, `buildDepthBuffer`): `filter_color_projection` (7 calls),
`filter_img_patch_param` (2), `filter_camera` (1), `src/core/softdepthbuffer.h`, and the
raster view in `src/render/renderwidget{,_raster,_render,_frame_plan}.cpp`. Under the
boundary none of them changes.

**Who reads a raster image**: everything goes through `Document::ensureRasterPlaneImage`
(`src/core/document_layers.cpp:813`), the one lazy decode point, called from the document,
the renderer and the color projection and texture consolidation filters. The layer panel's
thumbnails read the file directly with `QImageReader` and so show the photo as shot, which
is right for a thumbnail.

## The lens

A lens maps **normalized** camera coordinates, `(x/z, y/z)` of a point in front of the
camera, from undistorted to distorted. The pinhole intrinsics stay in `CameraShot` and are
the same on both sides:

```
file pixel    = K · distort(n)
working pixel = K · n
```

So the image in memory has **the same size and the same `K`** as the file; pixels that
fall outside the photo after undistortion are transparent (alpha 0, which
`filter_img_patch_param`'s alpha weight already honours). Being normalized, the lens is
independent of resolution: *Import Image to Current Raster* rescaling the camera
(`RescaleViewportPx`) leaves it alone.

**Models.** One rational Brown-Conrady model, OpenCV's: radial `k1..k6`, tangential `p1,
p2`. It covers COLMAP's `SIMPLE_RADIAL`, `RADIAL`, `OPENCV` and `FULL_OPENCV`, and
RealityScan's and Metashape's Brown export (Metashape's `b1` is an aspect change and goes
into `K`; its skew `b2` is not representable and is refused). The type carries a model
enum so that an equidistant fisheye (`OPENCV_FISHEYE`, `k1..k4`) can be added when a
dataset needs one.

**Directions.**

- `Distort(n)`: closed form. Undistorting an image needs only this one: for each working
  pixel, find the file pixel it comes from and sample it bilinearly.
- `Undistort(d)`: Newton iteration on `Distort`, from `d`, with a convergence flag; at most
  a few iterations for real lenses. Needed only for points picked or imported in the photo
  as shot.

**Where it lives.** A new vcglib header, `vcg/math/lens_distortion.h`, templated on the
scalar, with a short sample in `apps/sample/`. It is general camera geometry with no
MeshLab dependency, so it belongs in vcglib by the repository's own rule; it composes with
`vcg::Camera` rather than living inside it, so the pinhole camera stays what it is.

## Mapping table 1 -- vcglib: removing the old distortion

Evidence is from `grep -rn "DistorCenterPx\|Intrinsics\.k\|UndistortedToDistorted\|
DistortedToUndistorted\|cam\.k\["` over `vcglib/` (12 files) and over `src plugins tests`
(4 files), and the file reads cited.

| # | Current | Evidence | Target | Verdict |
|---|---|---|---|---|
| V1 | `Camera::k`, `Camera::DistorCenterPx` | `camera.h:191-193` | removed | remove |
| V2 | `UndistortedToDistorted`, `DistortedToUndistorted` | decl `camera.h:178-179`, def `camera.ipp:393-466` | removed | remove |
| V3 | the `k[0] != 0` branches of `Camera::Project` / `UnProject` | `camera.ipp:279-283`, `298-303` | pinhole only | remove |
| V4 | constructor init, `Import`, `operator==`, `RescaleViewportPx` of the two fields | `camera.ipp:34,37`, `49-54`, `477-478`, `116,132-133` | the lines go | remove |
| V5 | `.mlp`/`.aln` XML read of `LensDistortion` / `LensDist` | `wrap/qt/shot_qt.h:46,75-76,119-120` | attribute read and ignored | edit |
| V6 | `.mlp` XML write of `LensDistortion` | `shot_qt.h:161-162,206-207` | writes `"0 0"`, so old readers that require the attribute still parse | edit |
| V7 | PLY `camera` element read of `k1..k4` | `wrap/io_trimesh/import_ply.h:164-167,330-333,725-728,1152-1155` | properties still declared (files keep parsing), values discarded | edit |
| V8 | PLY `camera` element write of `k1..k4` | `wrap/io_trimesh/export_ply.h:193-196,397-400,425-428` | written as 0, so the element layout is unchanged | edit |
| V9 | nanoply camera `k1..k4` descriptors | `wrap/nanoply/include/nanoplyWrapper.hpp:954-957,1323-1326` | read into and written from a local zeroed array, layout unchanged | edit |
| V10 | Bundler / NVM importers zeroing `k` | `import_out.h:128-129`, `import_nvm.h:148-149` | lines go | remove |
| V11 | Tsai wrapper writing `kappa1` and the distortion centre into the shot | `wrap/tsai/tsaimethods.cpp:154-163,199` | `wrap/tsai/` deleted: nothing compiles it ([Old MeshLab](#old-meshlab)) | remove |
| V12 | commented-out `k` code | `wrap/gl/camera.h:329-330`; `wrap/io_tetramesh/import_ply.h:136-139,554-557`; `export_ply.h:266-269,294-297` | left as they are: they sit inside commented-out blocks (in `wrap/gl/camera.h`, a whole legacy camera class, lines 212-1101) that touch nothing; removing the blocks is a separate cleanup | keep |
| V13 | the camera test zeroing `k` | `apps/test/camerashot/camerashot_test.cpp:435-437,469-471` | lines go | remove |
| V14 | Doxygen field list | `docs/Doxygen/shot.dox:69-70` | lines go | remove |

14 rows: 8 remove (V1-V4, V10, V11, V13, V14), 5 edit (V5-V9), 1 keep (V12). The edits
keep both file formats byte-compatible: a PLY or `.mlp` written after the change reads in
old MeshLab, and an old one reads here, with any distortion it held dropped.

### Old MeshLab

Audited 2026-11-02 on `.reference/meshlab`, the current `devel` of `cnr-isti-vclab/meshlab`
(`8aaedcfdd`, 2026-08-25, which pins vcglib at `3aac63d`; a first pass on the older
`684885ade` found the same, and none of the 20 source files changed between the two touches
a camera), by grepping its sources outside `src/vcglib`
and `src/external` for `Intrinsics.k`, `k[0..3]`, `DistorCenterPx`, `LensDistortion`,
`UndistortedToDistorted`, `DistortedToUndistorted`, `kappa` and `tsai`.

**No code of old MeshLab computes or uses a distortion value.** What it does with the
fields:

| Where | What | Live? |
|---|---|---|
| `src/meshlab/glarea.cpp:2003-2004, 2125-2126` | sets `DistorCenterPx` to the viewport centre when it builds a shot from the view; `k` stays 0, so the centre has no effect | live, inert |
| `filter_layer/filter_layer.cpp:767-769` | Bundler import sets `k[0] = k[1] = 0` ("To be uncommented when distortion is taken into account reliably") | live, inert |
| `edit_mutualcorrs`, `filter_mutualinfo`, `filter_mutualglobal`: `solver.cpp` | `qDebug` prints of `DistorCenterPx` and `k[0..1]`, 8 live lines in each | live, debug output only |
| the same three: `levmarmethods.cpp:147-193` | `Levmar2Shot` / `Shot2Levmar` writing `kappa1` and the distortion centre | commented out |
| the same three: `solver.cpp:11-12`, `Solver::tsai` and its `TsaiMethods::calibrate` call | `wrap/tsai` | commented out; no build file names `tsai`. Each `solver.h` still declares `bool tsai(AlignSet *, Shot &)`, never defined or called, and unaffected |

The only way a non-zero `k` reaches old MeshLab is passively, from a file it reads:
`LensDistortion` in an `.mlp` (`io_base/load_project.cpp:301`, `ReadShotFromQDomNode`) or a
shot pasted from the clipboard (`glarea.cpp:2163`), and `k1..k4` of a PLY `camera` element
through vcglib's PLY importer. Every such file in the corpora holds zeros: the 39 `.mlp`
above, and all 34 PLY files with a `camera` element (`.reference/meshlab/sample/rangemaps`,
`tests/local`; read with a script that decodes the element). A non-zero value would only
have bent the CPU projections, while old MeshLab's raster rendering, which uses the GL
matrix, stayed pinhole.

PyMeshLab's own sources (outside its `meshlab` submodule) never touch the fields.

**Removing the fields breaks only compilation**, in 31 lines of 5 files:
`glarea.cpp` (4), `filter_layer.cpp` (3) and the three `solver.cpp` (8 each, the prints of
the removed fields; the `FocalMm`, `CenterPx` prints around them stay). Behaviour does not
change. The patch is prepared with Stage 0 and lands in old MeshLab before or with its next
vcglib bump; until then its pinned vcglib keeps building.

## Mapping table 2 -- MeshLab

| # | Current | Evidence | Target | Verdict |
|---|---|---|---|---|
| M1 | `CameraShot::distortionCenterPx/setDistortionCenterPx/distortionK/setDistortionK` | `camerashot.h:62-66`, `camerashot.cpp:154-177` | removed | remove |
| M2 | `fromVcgShot` and the trackball snapshot setting `DistorCenterPx = CenterPx` | `camerashot.cpp:89`, `renderwidget.cpp:1444`, `camerafilterplugin.cpp:194` | lines go | remove |
| M3 | the `.mlp` camera parse, twice: `Document` and `filter_layer` (*Import Cameras to Visible Rasters*) | `document_internal.cpp:125-250` (`parseMeshLabProjectCamera`), `layerfilterplugin.cpp:234-354` (`parseVcgCamera`, `writeVcgCamera`) | one parser and one writer in core, taking and giving a shot **and a lens**; `filter_layer` calls them | merge (duplication found while measuring) |
| M4 | `.mlp` writer | `document_project.cpp:425` writes `LensDistortion="0 0"` | unchanged, plus the new lens attributes when the lens is not identity | edit |
| M5 | a legacy non-zero `LensDistortion` read | `document_internal.cpp:192,239-240` load it into `k` | ignored with a warning naming the raster (no file has one) | edit |
| M6 | `RasterEntry` | `document.h:93-117` | `+ CameraLens lens;` | add |
| M7 | `RasterSnapshot` (undo) | `document_undo_types.h:81-88` holds `shot` | holds `lens` too | add |
| M8 | `setRasterShot` | `document.h:389` | `setRasterCamera(index, shot, lens)`: a lens change bumps `cameraRevision` *and* `imageRevision` and drops the decoded planes | edit |
| M9 | `ensureRasterPlaneImage` | `document_layers.cpp:813-822` decodes the file | decodes, then undistorts when the raster's lens is not identity | edit |
| M10 | texture consolidation refusing distorted cameras | `textureconsolidationfilterplugin.cpp:175-176` | check goes: it now sees a pinhole camera and an undistorted image | remove |
| M11 | `data_model.md` raster section | `data_model.md:38-42` | describes the boundary and the lens | edit |

11 rows: 4 remove (M1, M2, M10, and the second parser of M3), 5 edit, 2 add. No consumer
of the camera appears in the table: that is the point of the boundary.

## Planes in memory and on disk

Every plane **file** of a raster is in the frame of the photo as shot, and is undistorted on
decode. A plane made in memory -- a snapshot, a mask or depth a filter writes -- is in the
working frame. When such a plane is saved, it is written as it is and the `.mlp` marks it
`undistorted="1"`, so that it is not undistorted twice on reload; old MeshLab ignores the
attribute. The alternative, distorting it back on save, resamples twice for nothing. See
[Q3](#open-questions).

## The `.mlp` extension

```xml
<VCGCamera ... LensDistortion="0 0"
           LensModel="brown" LensParams="k1 k2 p1 p2 k3 k4 k5 k6"/>
```

The parameter order is OpenCV's, so that COLMAP's and OpenCV's numbers copy across
unchanged. `LensDistortion` stays, as `"0 0"`, because our reader and old MeshLab's
require it. Old MeshLab reads such a project as pinhole cameras on distorted photos, so
it misprojects by the distortion. Nothing can prevent that short of not writing the
project, and it is the same error it makes today on any undistorted-by-hand workflow done
wrong.

## Staging

**Stage 0 -- vcglib removal** (table 1, M1, M2, M5, M10). Implemented 2026-11-02:
-452/+31 lines in vcglib (285 of them `wrap/tsai`), -48/+18 in MeshLab. `filter_layer`'s
copy of the camera parser warns about a non-zero `LensDistortion` as the core one does,
until M3 merges the two in Stage 1. Verified: the full suite (only the known
`fileDialogsRememberWhereYouWere` fails), all 62 vcglib sample targets, and a PLY camera
written in binary and ASCII by the old and the new vcglib, byte for byte identical and
read back the same. MeshLab's `.mlp` writers already wrote `"0 0"` / `"0.000000 0.000000"`,
and still do. `apps/test/camerashot` (qmake only) fails to compile on two unrelated calls
before and after. The 31-line patch for old MeshLab applies cleanly to its `devel`.

Found while verifying, not caused by this change: vcglib's PLY camera does not round-trip
the same way in binary and ASCII -- a shot written both ways and read back projects one
point to (530, 220) from the binary file and to (470, 180) from the ASCII one, with the
old vcglib as with the new.

**Stage 1 -- the lens** (vcglib header and sample, M3, M4, M6-M9, M11). Implemented
2026-11-02; what follows the planned tests below. Planned: about 150 lines in vcglib and 300
in MeshLab, of which M3's merge removes about 80.
Tests, in `tests/test_document.cpp`:

- `Distort` / `Undistort` round trip under 1e-4 px over the whole image, for the 35
  `FULL_OPENCV` cameras of the RealityScan export (`k1` about 0.25, `k3` up to 1.6: a
  strong lens, the hard case for Newton).
- `.mlp` save and load keep the lens; a `LensDistortion` of `"0 0"` with no `LensModel`
  reads as the identity, a non-zero one warns.
- **End to end on the gargoyle fixture**: distort its four photos with a known lens, give
  that lens to the cameras, load: the undistorted planes match the original photos (PSNR
  above 40 dB away from the transparent border), and *Compute Face Scalar from Rasters*
  reports the same counts as on the original fixture. This needs four more images in
  `tests/sample_mesh/` (about 0.9 MB) or generating them in the test; generating is
  preferred.
- Undo of a lens change restores the lens and the image.

As built: `vcg/math/lens_distortion.h` (156 lines) and the sample `space_lens_distortion`;
in MeshLab +348/-380 lines of source, so the merge of M3 more than paid for the lens, and 249
lines of tests. The two `.mlp` camera parsers and two writers became one of each,
`CameraShot::readVcgCameraXml` / `writeVcgCameraXml`; `filter_layer`'s copies and its
private number parsers are gone, and so are the two pair parsers of `document_internal.cpp`.
Where it departs from the plan:

- **No model enum.** `LensDistortion` holds the eight Brown-Conrady parameters and nothing
  else; the enum is added with the fisheye model (Q4), when there is a second model to tell
  apart. The `.mlp` already names the model (`LensModel="brown"`), and refuses one it does
  not know rather than reading the photo as pinhole.
- **`setRasterLens` beside `setRasterShot`** rather than M8's `setRasterCamera(shot,
  lens)`: the seven callers of `setRasterShot` stay as they are. *Import Cameras to Visible
  Rasters* calls both, inside the filter's one undo step.
- **`ensureRasterPlaneImage(raster, plane)`**: it needs the raster's camera and lens, so its
  13 callers outside `Document` (renderer, four filters, two tests) pass the raster they
  already held. *Import Image to Current Raster*, which reads its file itself, undistorts it
  too.
- **Folds.** A calibrated model can fold back on itself beyond the part of the image its
  photos constrained. Camera 21 of the RealityScan export (`k3` -0.43) peaks at a normalized
  radius of 0.7046 while its photo's corners reach 0.7079: 13 corner pixels (of every fourth)
  have no undistorted point, and `Undistort` reports them as not converged. Past a fold
  `Distort` lands inside the photo again, so `IsInvertibleAt` (the sign of the Jacobian, which
  Newton already computed) leaves such pixels transparent rather than repeating the photo
  mirrored. No working pixel of this dataset reaches a fold; the guard is for lenses that do.
- **The face-scalar check of the end-to-end test was dropped**: *Compute Face Scalar from
  Rasters* reads cameras and depth only, never an image, so its counts cannot depend on the
  lens. The test checks the planes alone: PSNR 41.9-46.3 dB against the original photos over
  the four rasters, after two bilinear resamplings, with a pincushion lens (k1 0.12), whose
  undistorted corners are left transparent. The distorted photos are generated in the test.
- **Round trip** measured on all 35 cameras rather than committed: the export is not under
  the dataset roots (`.reference/TestDataset/2930Colmap`). Over every fourth pixel the worst
  error is 8e-13 px; the test carries cameras 11 (the strongest, `k3` 1.78) and 21 (the fold)
  as literal values.
- **Checked against `tools/colmap_to_mlp.py`**, written separately with the same conventions:
  three of the export's photos decoded through their lenses match the script's undistorted
  images to RMS 0.003 and at most 1 level per channel; the only difference is a half-pixel
  ring at the border, which MeshLab keeps (a border pixel reaches half a pixel past its centre)
  and the script drops. A 3840x2160 photo decodes through its lens in about 0.2 s, single
  threaded, on top of reading the file.

**Stage 2 -- COLMAP import.** A reader for a COLMAP sparse model (`cameras`, `images`,
`points3D`, text and binary) beside `MeshLabProject*`, producing the same raster entries,
with the images found as `tools/colmap_to_mlp.py` finds them, `points3D` as a point cloud
layer with colors, and every COLMAP model in Stage 1's set imported natively. *Open
Project* accepts `cameras.txt` / `cameras.bin`. About 300 lines. Tests: a small committed
COLMAP text model of the gargoyle fixture (the four cameras with the Stage 1 lens, its
points), and the RealityScan export as an optional dataset
(`MESHLAB2_REQUIRE_DATASET`) checking the reprojection error of the points' tracks under
1 px. `tools/colmap_to_mlp.py` is then removed; its `--z-up` turn is a transform of all
layers and rasters, which *Transform* already does. See [Q5](#open-questions).

As built (2026-11-02): `src/core/document_colmap.cpp`, 447 lines with its comments, reads
text and binary models into the `MeshLabProjectRasterEntry` records the `.mlp` parser gives
(`DocumentInternal::parseColmapModel`), so `loadMeshLabProject` opens either with the same
raster loop, dispatching on the file name (`Document::isProjectFile`): *Open*, a drop, the
command line and Python `load_project` all take a `cameras.txt` / `cameras.bin`. The images
are sorted by name, COLMAP's ids saying nothing about how they were taken. The image folder is
the first of `images/` or the folder itself, from the model's folder up two levels, holding
the first image. The points become a colored point cloud layer, *COLMAP points*. The load logs
the reprojection of COLMAP's observations through the cameras and lenses read
(`colmapReprojectionErrors`), as the Python converter's run did. A COLMAP model is not a
project to save over: *Save Project* starts a new `.mlp`. An unsupported camera model (the
fisheye ones, FOV) refuses the whole model, naming it. `isMeshLabProjectExtension`, defined
and never called, went.

- **The committed model is pinhole**, not the Stage 1 lens on the gargoyle cameras that the
  plan named: the fixture's photos are pinhole, and a lens in the model would make it say
  something false about them. The lens mapping of each COLMAP model is tested on a model the
  test writes, and the lens itself end to end by Stage 1's test. The gargoyle model
  (`tests/sample_mesh/gargoyle_small/sparse/0/`, text and binary, 78 KB: the four cameras as
  `PINHOLE`, 201 mesh vertices as points, seen by all four) was written once by a numpy script
  inverting the Python converter's conversion, independently of the reader; it reopens as
  gargoyle.mlp's cameras, within 0.01 px.
- **The RealityScan export as an optional dataset**, `2930Colmap/sparse/0/cameras.txt` under
  the dataset roots (`tests/local/2930Colmap` on a maintainer's machine): 261,316
  observations reproject with a median of 0.393 px, 90% under 1.02 px, at most 2.00 px.
- **The z-up turn is two filters, not one.** *Transform* moves mesh layers only; the cameras
  turn with *Rotate Cameras* (all rasters, axis X, centre the origin). RealityScan's COLMAP
  export turns its scene's (x, y, z) into (x, -z, y); the same quarter turn on both, mapping
  (x, y, z) to (x, z, -y), lines a model exported from the same RealityScan project up with
  the cameras and points.

**Stage 3 -- image-to-mesh alignment.** Carried out as [Raster Alignment](../history/raster_alignment.md), which uses NEWUOA rather than the Levenberg–Marquardt solver named below. The plan as first written:
correspondence alignment (old `edit_mutualcorrs`, 3.2k lines, levmar) and mutual-
information refinement (`filter_mutualinfo`, 2.6k; `filter_mutualglobal`, 4.6k). What this
proposal fixes for it: picks are made on the displayed image, which is the working frame,
and are mapped to the photo as shot with `Distort` when the lens is refined; the solver can
be Eigen's `LevenbergMarquardt` (in `vcglib/eigenlib/unsupported`), replacing levmar; and
mutual information renders the mesh through the pinhole camera, which is exactly what it
needs.

## Open questions

1. **Old MeshLab and `wrap/tsai`.** Old MeshLab must not break: audited under
   [Old MeshLab](#old-meshlab) -- it never uses the fields, so a 31-line patch on its side
   keeps it building with unchanged behaviour. `wrap/tsai` is compiled by nobody, so
   removing it (V11) breaks nothing. Decided 2026-11-02: the fields are removed, and the
   patch goes to old MeshLab.
2. **Legacy `LensDistortion`.** Decided 2026-11-02: read, ignored with a warning.
3. **Planes saved from memory.** Decided 2026-11-02: written undistorted and marked
   `undistorted="1"`.
4. **Fisheye.** Decided 2026-11-02: later, when a dataset needs it; the enum leaves room.
5. **`tools/colmap_to_mlp.py`.** Decided 2026-11-02: removed at the end of Stage 2. Removed.
