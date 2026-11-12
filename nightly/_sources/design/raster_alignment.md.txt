# Raster Alignment

How a raster's camera is aligned to the mesh: by point pairs, by mutual information, both at
once, and globally over a set of rasters. How rasters, cameras and lenses are stored is in
[Data Model](data_model.md#raster-data-type); the interactive tool and picking in the raster
view are in [Rendering](rendering.md#interactive-tools). The record of how this was built,
with the measurements and the decisions behind it, is
[history/raster_alignment.md](history/raster_alignment.md).

## What changes and what does not

Every alignment changes **where a raster's camera stands and looks, and optionally its focal
length**, and nothing else. The principal point, the pixel size, the viewport and the lens
are the calibration's (from a project, COLMAP or a photogrammetry tool). Old MeshLab reset
the principal point to the image centre on every alignment; nothing here does.

All of it works on the **pinhole camera**. A raster's photo is undistorted when its plane is
decoded (`Document::ensureRasterPlaneImage`), so a pixel of the image MeshLab shows is a pixel
of `RasterEntry::shot`. Only perspective cameras are aligned; an orthographic one is refused.

**Pixels in point pairs** are in the camera's viewport, from the top-left corner, y down,
with pixel centres at k + 0.5. That is also the image's size whenever the image matches its
camera, the usual case. `vcg::Shot::Project` counts y up, so the filters convert:
v_vcg = height − v. The JSON is
`[{"point": [x, y, z], "pixel": [u, v]}, ...]`, with points in world space, read by
`parseCorrespondences` (`plugins/filter_camera/mutualinformation.h`).

## The filters

All four are in `plugins/filter_camera`, category `Document/Camera`.

| Filter | What it does |
|---|---|
| *Set Raster Camera* (`fromView`) | Gives the current raster the camera of a view (a `camerastate` parameter, the current 3D view by default), for the raster's own image size, roll and orthographic projection included: a starting camera for the others, old MeshLab's *Get Shot*. Without `fromView`, a camera from a viewpoint, a direction and a focal length. |
| *Align Current Raster to Mesh by Point Correspondences* | The camera that minimizes the RMS reprojection error of three or more pairs, four with the focal length (`vcg::ShotResection::Fit`). Returns `rmsBefore`, `rmsAfter`, `maxAfter`, `pairErrors`. |
| *Align Current Raster to Mesh by Mutual Information* | Refines the camera by maximizing the mutual information between the photo and a rendering of the visible layers (Corsini et al. 2009); optionally with point pairs as a soft constraint. Returns `informationBefore`, `informationAfter`, `evaluations`, `displacement`, and with pairs `pairsBefore`, `pairsAfter`. |
| *Align Visible Rasters Globally by Mutual Information* | Refines the visible rasters together, each against the mesh painted with its neighbours' photos (Dellepiane and Scopigno 2013); hidden rasters are anchors. Returns `iterations`, `variations`, `arcs`, `evaluations`. |

The *Raster Alignment Tool* drives the first two from the raster view: pairs are made by
dragging from the mesh to the photo, *Fit* runs the correspondence filter, and *Mutual
Information* runs the mutual-information filter with the tool's pairs.

## The parametrization

`vcg::ShotResection<S>::Parametrization` (`vcglib/vcg/math/shot_resection.h`) describes a
change of a camera for a derivative-free optimizer. The parameters are:

- a rotation vector and a translation, in the camera's own frame, about a pivot;
- a change of `FocalMm`, when the focal length is fitted.

Each is scaled so that a unit step moves the projections of the sample points by about one
pixel, root mean square. One trust radius in pixels therefore means the same for all of
them. The scales are measured on the samples, so the result is deterministic.

The correspondence fit pivots on the centroid of its points; mutual information pivots on the
centre of the visible layers' box, as old MeshLab did. Both minimize with **NEWUOA**
(`vcglib/wrap/newuoa`) in rounds, each starting where the last ended with the scales measured
again, until a round gains nothing.

## The rendering

Mutual information compares the photo with a rendering made **on the CPU**, so it is
deterministic and runs in `ctest` and `pymeshlab2` without a GPU:

- **The rasterizer.** `rasterizeTriangle` (`src/core/softdepthbuffer.h`) calls back each
  pixel a triangle covers, and the renderer shades where the triangle wins its depth test,
  with perspective-correct barycentrics, as the GPU interpolates.
- **Threads.** The faces are drawn by the threads of the global pool into one buffer that
  keeps, per pixel, the least key of depth and drawing order, by an atomic minimum; each
  pixel is then shaded once, from the face that won it, with the weights the rasterizer
  drew it with (`triangleWeights`). A pixel thus keeps the nearest face and, among faces
  equally near, the first in layer and face order, as a serial drawing does. So the image,
  and with it every result, is the same bit for bit whatever the number of threads. (Buffers
  per thread merged after the drawing, as it first was, took four fifths of each rendering
  whatever the mesh.)
- **What is rendered.** Every visible layer with faces, at a working resolution whose
  longest side is `resolution` (800). The photo is scaled to the same size and turned to grey
  as 0.3 R + 0.59 G + 0.11 B.
- **Renderings.** Each value is the one channel old MeshLab's OpenGL programs read back:
  - *Combined* (the default): the vertex color blended into the normal map by
    t = red². Where the vertex color is white, as on a mesh without color, it is the
    normal map.
  - *Normal Map*: the eye-space normal's y, mapped to 0 to 1.
  - *Vertex Color*: its red.
  - *Silhouette*: the layers white on black.
- **Mutual information** is computed over a 128 × 128 joint histogram, in bits:
  - the rendering's darkest bin, which holds the background, counts 1 / `backgroundWeight`
    (default 2);
  - the photo's transparent pixels, such as an undistorted photo's corners, do not count;
  - the pixels are counted in integers by the threads, a histogram per chunk of pixels,
    summed afterwards, and weighed once counted.
- **Cost.** About 1.2 ms a rendering and its information on 59k faces and 4.9 ms on 3M
  faces, at 800 × 535 on 16 threads of an M3 Max, measured 2026-11-12; the information
  is 0.2 ms of it. A refinement takes 100 to 300 renderings: 0.16 and 0.65 s for these two
  meshes, from 2.2 and 2.8 s before the single buffer and the threaded histogram.

**The rendering decides whether the method works** on a given photo. On a real colored scan
the *Normal Map* has a higher maximum 13 to 19 px away from the right camera, while *Vertex
Color* finds it within 1.3 px.

## Mutual information with point pairs

With point pairs, the single-raster filter minimizes old MeshLab's combined score:

(1 − w)(2 − MI) + w · d

- d is the pairs' mean distance in working pixels;
- w is `pairsWeight` (default 0.01);
- w = 0 is mutual information alone, and w = 1 the pairs alone.

The sum is of bits and pixels, so w is a matter of balance rather than of units. Near the
optimum, mutual information changes by hundredths of a bit while a pair moves by pixels, so
useful weights are small. At 0.01, a few pairs keep the camera in the right basin and leave
the last pixel to the information. Heavier weights hand the result to the pairs.

## Global refinement

*Align Visible Rasters Globally by Mutual Information* follows the paper:

- **Arcs.** The arc from raster j into raster i weighs w = MI(I_i, proj(I_j, I_i)) × OV,
  where OV is the share of the layers seen from i that j's photo paints. An arc is made only
  where OV > 0.2.
- **Painting.** A painting rendering projects each photo onto the surface, sampled bilinearly
  where the photo's own camera sees it (its depth buffer, within 1%). The photos are combined
  by their weights in grey levels, and the chosen rendering is used where none sees the
  surface.
- **A pass.** It refines one raster at a time: the one with the most arcs from refined
  rasters, ties going to the one with more arcs. The raster is refined against the mesh
  painted with the photos of all its arcs, weighted by them, and then the arcs that involve
  it are weighed again. A raster with no arcs is skipped.
- **Stopping.** Passes repeat until one moves the projections of 5,000 samples by less than
  `threshold` px on average (1.2), in each raster's own pixels, or after `maxIterations` (5).
- **Anchors.** Hidden rasters with a camera count as refined from the start. They paint the
  mesh and never move.
- **Focal length.** It is refined by default, as in the paper's seven parameters.

## Tests

In `tests/test_filters.cpp`, behavioural tier:

- **Set Raster Camera from a view:** every corner of a cube lands within 0.01 px of where the
  view draws it, perspective and orthographic, with a roll.
- **Point correspondences:**
  - On the gargoyle fixture's cameras, from exact pairs: back to under 1e-4 px, also with the
    focal length.
  - On the `2930Colmap` dataset, when present, from COLMAP's observations taken through the
    lens: 0.05 px from COLMAP's camera.
- **Mutual information:**
  - Gargoyle cameras from 2 and 5 degree starts: back to where a fit from the true camera
    ends, within 2 px (5 for camera 1, whose maximum is the flattest).
  - With six noisy pairs, camera 1 from 8 degrees, beyond the information's own reach: within
    1.5 px. The *Silhouette* rendering runs.
  - On the scan, when present, with *Vertex Color*: within 3 px of COLMAP's camera.
- **Global refinement:** the gargoyle's four cameras end within 1.5 px of the fixture's, and
  within 2 px with camera 0 as an anchor that does not move.

The tool, which needs the GUI, was checked in the real window, on Metal and OpenGL (see the
record).
