# Filter Classification — pass 1 record

> **Current registry (2026-08-31): 327 filters across 33 filter plugins.** Every
> 272-filter total and table below is a dated record of the 2026-07-29 migration,
> not a current implementation count.

This document records the `categories` applied to the **272-filter migration
baseline**. It was generated once, by rules, from the pre-migration descriptors and
the ontology in
[Vocabulary](../vocabulary.md) §1; see [Filter Organization](../filter_organization.md)
for the plugin-side mapping.

**Status: APPLIED** (pass 1, 2026-07-29). All 272 filters present on that date received
a `categories` array. The loader now validates every entry against the ontology and
reports no offenders in the current registry.
The tables below are the record of what was applied — the `Note` column flags each row
that moved somewhere other than a mechanical translation of the old `menuPath`.
The implementation has since grown to 327 filters across 33 filter plugins; newly
added filters are categorized in their `filters.json` descriptors, but this document
intentionally remains the pass-1 migration record rather than a regenerated live
catalog.

Representation (§2) is deliberately **out of scope** for this pass.

## How to read this record

1. The four ontology additions in the next section were the rulings that made
   the pass possible.
2. Rows with a **bold** note are the ones that moved somewhere other than where
   a naive `menuPath` translation would have put them.
3. Categories are **ordered**: the first is primary (canonical home); the rest
   are cross-listings.

## Ontology additions applied in this pass

Classifying the real filters exposed four gaps. These rulings are now implemented:

| Applied ontology addition | Filters | Why |
|---|---|---|
| `Meshing/Deletion` | 4 | *Delete Selected Faces/Vertices*, *Delete ALL Faces*. These **delete geometry**, so by the "what changed?" rule they are not `Selection` at all, and nothing is broken so they are not `Repair`. |
| `Selection/Set Operations` | 5 | *Select All/None*, *Invert*, *Erode*, *Dilate*. Selection algebra, not selection *by a criterion* — the other three subcategories all answer "selected how?". |
| `Attribute/Custom` | 4 | *Define New Per-Vertex/Face Custom Scalar/Point Attribute*. User-defined attributes are not normal/scalar/curvature/color. |
| `Geometry/Deformation` | 3 | *Random Vertex Displacement*, *Vertex Linear Morphing*, *Per Vertex Geometric Function*. They move vertices but are neither an affine `Transform` nor noise-reducing `Smoothing` — *Random Vertex Displacement* **adds** noise. |

A related rule this pass assumes, worth confirming: **`Geometry/Smoothing` covers
positions only.** Smoothing an *attribute* is classified by the attribute —
*Smooth Vertex Scalar* → `Attribute/Scalar`, *Smooth Vertex Color* →
`Attribute/Color`, *Smooth Face Normals* → `Attribute/Normal`.

## Resulting shape

Historical pass-1 shape on 2026-07-29: 272 filters → 43 distinct categories; 24
filters carried more than one.

| Category | Primary | Total (incl. cross-listed) |
|---|---|---|
| `Attribute/Color` | 28 | 33 |
| `Attribute/Curvature` | 4 | 4 |
| `Attribute/Custom` **(new)** | 4 | 4 |
| `Attribute/Normal` | 13 | 13 |
| `Attribute/Scalar` | 19 | 26 |
| `Creation/Primitives` | 21 | 21 |
| `Creation/Reconstruction` | 8 | 8 |
| `Creation/Sampling` | 9 | 9 |
| `Document/Camera` | 10 | 10 |
| `Document/Layer` | 11 | 11 |
| `Document/Render` | 1 | 1 |
| `Geometry/Alignment` | 2 | 2 |
| `Geometry/Deformation` **(new)** | 3 | 3 |
| `Geometry/Smoothing` | 11 | 11 |
| `Geometry/Transform` | 12 | 12 |
| `Measurement/Geometric` | 5 | 6 |
| `Measurement/Statistics` | 5 | 5 |
| `Measurement/Topological` | 3 | 3 |
| `Meshing/Boolean` | 4 | 4 |
| `Meshing/Deletion` **(new)** | 4 | 4 |
| `Meshing/Remeshing` | 5 | 5 |
| `Meshing/Simplification` | 6 | 6 |
| `Meshing/Quad` **(new)** | 4 | 4 |
| `Meshing/Subdivision` | 7 | 7 |
| `Parametrization` | 0 | 1 |
| `Parametrization/Atlas Packing` | 0 | 1 |
| `Parametrization/Defragmentation` | 2 | 2 |
| `Parametrization/UV Conversion` | 2 | 2 |
| `Parametrization/UV Creation` | 11 | 11 |
| `Repair/Degenerate` | 4 | 4 |
| `Repair/Duplicates` | 6 | 6 |
| `Repair/Holes and Borders` | 2 | 2 |
| `Repair/Topology` | 7 | 7 |
| `Selection/Set Operations` **(new)** | 5 | 5 |
| `Selection/by Attribute` | 8 | 8 |
| `Selection/by Topology` | 11 | 11 |
| `Selection/by Visibility` | 2 | 2 |
| `Texture` | 0 | 6 |
| `Texture/Assignment` | 1 | 1 |
| `Texture/Conversion` | 1 | 1 |
| `Texture/Packing` | 1 | 1 |
| `Transfer/Between Layers` | 3 | 3 |
| `Transfer/Within a Mesh` | 7 | 7 |
| `Transfer/From Rasters` | 0 | 4 |

**In the pre-migration descriptors, 47 filters were mis-filed**: their applied
category moved them to a different root from the one implied by their old `menuPath`.
That was the main finding of this pass.

## Applied classification, by pre-migration bucket

### `Camera` (10)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Set Camera from Direction | `camera` | `Document/Camera` |  |
| Set Camera to View Selection | `camera` | `Document/Camera` |  |
| Orient Vertex Normals by Cameras | `camera` | `Attribute/Normal` | **mis-filed** under `Camera` |
| Set Mesh Camera | `camera` | `Document/Camera` |  |
| Set Raster Camera | `camera` | `Document/Camera` |  |
| Transform Camera Extrinsics | `camera` | `Document/Camera` | transforms the **camera**, not the mesh |
| Rotate Cameras | `camera` | `Document/Camera` | transforms the **camera**, not the mesh |
| Scale Cameras | `camera` | `Document/Camera` | transforms the **camera**, not the mesh |
| Translate Cameras | `camera` | `Document/Camera` | transforms the **camera**, not the mesh |
| Compute Vertex Scalar from Camera | `camera` | `Attribute/Scalar` | **mis-filed** under `Camera` <sub>touches: color, scalar</sub> |

### `Cleaning` (15)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Merge Close Vertices | `clean` | `Repair/Duplicates` |  |
| Merge Close Wedge UVs | `clean` | `Repair/Duplicates` | <sub>touches: uv</sub> |
| Remove Duplicate Faces | `clean` | `Repair/Duplicates` |  |
| Remove Duplicate Vertices | `clean` | `Repair/Duplicates` |  |
| Remove Isolated Folded Faces by Edge Flip | `clean` | `Repair/Topology` |  |
| Remove Isolated Components by Diameter | `clean` | `Repair/Degenerate` |  |
| Remove Isolated Components by Face Count | `clean` | `Repair/Degenerate` |  |
| Remove T-Vertices | `clean` | `Repair/Topology` |  |
| Remove Unreferenced Vertices | `clean` | `Repair/Duplicates` |  |
| Remove Vertices by Scalar | `clean` | `Repair/Degenerate` |  |
| Remove Zero-Area Faces | `clean` | `Repair/Degenerate` |  |
| Repair Watertight Mesh (MeshFix) | `meshfix` | `Repair/Topology` |  |
| Repair Non-Manifold Edges | `clean` | `Repair/Topology` |  |
| Repair Non-Manifold Vertices by Splitting | `clean` | `Repair/Topology` |  |
| Repair Mismatched Borders | `clean` | `Repair/Holes and Borders` |  |

### `Color` (24)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Clamp Vertex Scalar | `colorproc` | `Attribute/Scalar` | **scalar**, not color <sub>touches: scalar</sub> |
| Add Noise to Vertex Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Colorize Faces by Scalar | `colorproc` | `Attribute/Color` · `Attribute/Scalar` | scalar -> color <sub>touches: color</sub> |
| Colorize Vertices by Scalar | `colorproc` | `Attribute/Color` · `Attribute/Scalar` | scalar -> color <sub>touches: color</sub> |
| Equalize Vertex Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Set Random Layer Color | `colorproc` | `Attribute/Color` |  |
| Colorize Vertices by Perlin Noise | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Set Random Component Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Set Random Face Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Clamp Vertex Scalar Gradient | `colorproc` | `Attribute/Scalar` | **scalar**, not color <sub>touches: color, scalar</sub> |
| Set Mesh Color | `colorproc` | `Attribute/Color` |  |
| Smooth Face Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Smooth Vertex Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Transfer Color from Face to Vertex | `colorproc` | `Transfer/Within a Mesh` · `Attribute/Color` | **mis-filed** - it is a Transfer <sub>touches: color</sub> |
| Transfer Color from Mesh to Face | `colorproc` | `Transfer/Within a Mesh` · `Attribute/Color` | **mis-filed** - it is a Transfer <sub>touches: color</sub> |
| Transfer Color from Texture to Vertex | `colorproc` | `Transfer/Within a Mesh` · `Attribute/Color` | **mis-filed** - it is a Transfer <sub>touches: color</sub> |
| Transfer Color from Vertex to Face | `colorproc` | `Transfer/Within a Mesh` · `Attribute/Color` | **mis-filed** - it is a Transfer <sub>touches: color</sub> |
| Transfer Scalar from Face to Vertex | `colorproc` | `Transfer/Within a Mesh` · `Attribute/Scalar` | **mis-filed** - it is a Transfer <sub>touches: scalar</sub> |
| Transfer Scalar from Vertex to Face | `colorproc` | `Transfer/Within a Mesh` · `Attribute/Scalar` | **mis-filed** - it is a Transfer <sub>touches: scalar</sub> |
| Tint Vertex Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Desaturate Vertex Color | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Adjust Vertex Color Levels | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |
| Adjust Vertex Color White Balance | `colorproc` | `Attribute/Color` | <sub>touches: color</sub> |

### `Compute/Attributes` (4)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Define Custom Face Point Attribute | `func` | `Attribute/Custom` **(new)** |  |
| Define Custom Face Scalar Attribute | `func` | `Attribute/Custom` **(new)** |  |
| Define Custom Vertex Point Attribute | `func` | `Attribute/Custom` **(new)** |  |
| Define Custom Vertex Scalar Attribute | `func` | `Attribute/Custom` **(new)** |  |

### `Compute/Color` (2)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Compute Face Color by Expression | `func` | `Attribute/Color` | <sub>touches: color</sub> |
| Compute Vertex Color by Expression | `func` | `Attribute/Color` | <sub>touches: color</sub> |

### `Compute/Geometry` (1)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Per Vertex Geometric Function | `func` | `Geometry/Deformation` **(new)** | moves vertices by a formula |

### `Compute/Normals` (9)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Normalize Face Normals | `unsharp` | `Attribute/Normal` |  |
| Normalize Vertex Normals | `unsharp` | `Attribute/Normal` |  |
| Compute Face Normals by Expression | `func` | `Attribute/Normal` |  |
| Compute Vertex Normals by Expression | `func` | `Attribute/Normal` |  |
| Compute Face Normals | `unsharp` | `Attribute/Normal` |  |
| Compute Polygon Face Normals | `unsharp` | `Attribute/Normal` |  |
| Compute Vertex Normals | `unsharp` | `Attribute/Normal` |  |
| Smooth Face Normals | `unsharp` | `Attribute/Normal` |  |
| Sharpen Face Normals by Unsharp Mask | `unsharp` | `Attribute/Normal` |  |

### `Compute/Quality` (2)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Compute Face Scalar by Expression | `func` | `Attribute/Scalar` | <sub>touches: scalar</sub> |
| Compute Vertex Scalar by Expression | `func` | `Attribute/Scalar` | <sub>touches: scalar</sub> |

### `Compute/Texture` (2)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Parametrize per Vertex by Expression | `func` | `Parametrization/UV Creation` | **writes UVs**, not an image <sub>touches: uv</sub> |
| Parametrize per Wedge by Expression | `func` | `Parametrization/UV Creation` | **writes UVs**, not an image <sub>touches: uv</sub> |

### `Create` (16)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Create Annulus | `create` | `Creation/Primitives` | bucket default |
| Create Box | `create` | `Creation/Primitives` | bucket default |
| Create Cone | `create` | `Creation/Primitives` | bucket default |
| Create Dodecahedron | `create` | `Creation/Primitives` | bucket default |
| Create Symmetric Dodecahedron | `create` | `Creation/Primitives` | bucket default |
| Create Plane from Selection | `create` | `Creation/Primitives` · `Measurement/Geometric` |  |
| Create Grid | `func` | `Creation/Primitives` |  |
| Create Icosahedron | `create` | `Creation/Primitives` | bucket default |
| Create Isosurface from Expression | `func` | `Creation/Primitives` |  |
| Create Isosurface from Perlin Noise | `basic` | `Creation/Primitives` |  |
| Create Octahedron | `create` | `Creation/Primitives` | bucket default |
| Create Points on a Sphere | `create` | `Creation/Primitives` | bucket default |
| Create Points on a Spherical Cap | `create` | `Creation/Primitives` | bucket default |
| Create Sphere | `create` | `Creation/Primitives` | bucket default |
| Create Sphere Cap | `create` | `Creation/Primitives` | bucket default |
| Create Tetrahedron | `create` | `Creation/Primitives` | bucket default |
| Create Torus | `create` | `Creation/Primitives` | bucket default |

### `Geometry/Transform` (1)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Normalize To Unit Box | `basic` | `Geometry/Transform` |  |

### `Inspection` (1)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Measure Mesh Summary | `basic` | `Measurement/Topological` |  |

### `Layer` (14)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Remove Current Mesh Layer | `layer` | `Document/Layer` | bucket default |
| Remove Current Raster | `layer` | `Document/Layer` | bucket default |
| Remove Hidden Rasters | `layer` | `Document/Layer` | bucket default |
| Remove Hidden Mesh Layers | `layer` | `Document/Layer` | bucket default |
| Duplicate Current Layer | `layer` | `Document/Layer` | bucket default |
| Export Cameras from Visible Rasters | `layer` | `Document/Camera` |  |
| Merge Visible Layers | `layer` | `Document/Layer` | bucket default |
| Import Cameras to Visible Rasters | `layer` | `Document/Camera` |  |
| Extract Selected Faces | `layer` | `Document/Layer` | bucket default <sub>touches: selection</sub> |
| Extract Selected Vertices | `layer` | `Document/Layer` | bucket default <sub>touches: selection</sub> |
| Rename Current Mesh Layer | `layer` | `Document/Layer` | bucket default |
| Rename Current Raster | `layer` | `Document/Layer` | bucket default |
| Render from Render-State JSON | `layer` | `Document/Render` |  |
| Split into Connected Components | `layer` | `Document/Layer` | bucket default |

### `Layer/Boolean` (4)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Mesh Boolean: Difference | `mesh_booleans` | `Meshing/Boolean` | was mis-filed under `Layer` <sub>touches: color, scalar</sub> |
| Mesh Boolean: Intersection | `mesh_booleans` | `Meshing/Boolean` | was mis-filed under `Layer` <sub>touches: color, scalar</sub> |
| Mesh Boolean: Symmetric Difference (XOR) | `mesh_booleans` | `Meshing/Boolean` | was mis-filed under `Layer` <sub>touches: color, scalar</sub> |
| Mesh Boolean: Union | `mesh_booleans` | `Meshing/Boolean` | was mis-filed under `Layer` <sub>touches: color, scalar</sub> |

### `MLS` (8)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Compute Curvature (APSS) | `mls` | `Attribute/Curvature` | <sub>touches: scalar</sub> |
| Compute Curvature (RIMLS) | `mls` | `Attribute/Curvature` | <sub>touches: scalar</sub> |
| Estimate Radius from Density | `mls` | `Measurement/Statistics` |  |
| MLS projection (APSS) | `mls` | `Geometry/Smoothing` |  |
| MLS projection (RIMLS) | `mls` | `Geometry/Smoothing` |  |
| Reconstruct Surface by Marching Cubes (APSS) | `mls` | `Creation/Reconstruction` |  |
| Reconstruct Surface by Marching Cubes (RIMLS) | `mls` | `Creation/Reconstruction` |  |
| Select small disconnected component | `mls` | `Selection/by Topology` | <sub>touches: selection</sub> |

### `Measure` (5)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Measure Selection Area and Perimeter | `measure` | `Measurement/Geometric` |  |
| Measure Geometric Properties | `measure` | `Measurement/Geometric` |  |
| Measure Topological Properties | `measure` | `Measurement/Topological` |  |
| Measure Topological Properties for Quad Mesh | `measure` | `Measurement/Topological` |  |
| Measure Layer Overlap | `icp` | `Measurement/Geometric` |  |

### `Measure/Quality` (4)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Measure Face Scalar Histogram | `measure` | `Measurement/Statistics` |  |
| Measure Face Scalar Statistics | `measure` | `Measurement/Statistics` |  |
| Measure Vertex Scalar Histogram | `measure` | `Measurement/Statistics` |  |
| Measure Vertex Scalar Statistics | `measure` | `Measurement/Statistics` |  |

### `Meshing` (38)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Create Polyline from Selected Edges | `meshing` | `Creation/Primitives` | creates a new polyline layer |
| Close Holes | `meshing` | `Repair/Holes and Borders` |  |
| Create Polyline from Planar Section | `meshing` | `Creation/Primitives` | creates a new polyline layer |
| Compute Principal Curvature Directions | `meshing` | `Attribute/Curvature` | <sub>touches: scalar</sub> |
| Compute Point Cloud Normals | `meshing` | `Attribute/Normal` |  |
| Create Polyline from Selection Perimeter | `meshing` | `Creation/Primitives` | creates a new polyline layer |
| Parametrize by Cylindrical Projection | `meshing` | `Parametrization/UV Creation` |  |
| Invert Face Orientation | `meshing` | `Repair/Topology` | orientation is topological |
| Matrix: Freeze Current Matrix | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Matrix: Invert Current Matrix | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Matrix: Reset Current Matrix | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Matrix: Set from translation/rotation/scale | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Matrix: Set/Copy Transformation | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Orient Faces Consistently (vcglib) | `meshing` | `Repair/Topology` | orientation is topological |
| Remeshing: Isotropic Explicit Remeshing | `meshing` | `Meshing/Remeshing` |  |
| Select Crease Edges | `meshing` | `Selection/by Topology` |  |
| Simplification: Clustering Decimation | `meshing` | `Meshing/Simplification` |  |
| Simplification: Original QSlim Quadric Edge Collapse | `qslim` | `Meshing/Simplification` |  |
| Simplification: Quadric Edge Collapse Decimation | `meshing` | `Meshing/Simplification` |  |
| Simplification: Quadric Edge Collapse Decimation (with texture) | `meshing` | `Meshing/Simplification` · `Texture` | preserves texture <sub>touches: uv</sub> |
| Smooth Point Cloud Normals | `meshing` | `Attribute/Normal` |  |
| Subdivision Surfaces: Butterfly Subdivision | `meshing` | `Meshing/Subdivision` |  |
| Subdivision Surfaces: Catmull-Clark | `meshing` | `Meshing/Subdivision` |  |
| Subdivision Surfaces: Doo Sabin | `meshing` | `Meshing/Subdivision` |  |
| Subdivision Surfaces: LS3 Loop | `meshing` | `Meshing/Subdivision` |  |
| Subdivision Surfaces: Loop | `meshing` | `Meshing/Subdivision` |  |
| Subdivision Surfaces: Midpoint | `meshing` | `Meshing/Subdivision` |  |
| Transform: Align to Principal Axis | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Transform: Flip and/or swap axis | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Transform: Rotate | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Transform: Rotate to Fit to a plane | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Transform: Scale, Normalize | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Transform: Translate, Center, set Origin | `meshing` | `Geometry/Transform` | <sub>touches: texture</sub> |
| Tri to Quad by 4-8 Subdivision | `meshing` | `Meshing/Quad` |  |
| Tri to Quad by smart triangle pairing | `meshing` | `Meshing/Quad` |  |
| Turn into Quad-Dominant mesh | `meshing` | `Meshing/Quad` |  |
| Turn into a Pure-Triangular mesh | `meshing` | `Meshing/Quad` |  |
| Split Vertices by Attribute Seam | `meshing` | `Repair/Duplicates` | splits vertices on attribute discontinuity <sub>touches: uv, color</sub> |

### `Normals/Embree` (1)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Orient Face Normals by Ray Casting | `embree` | `Attribute/Normal` |  |

### `Parameterization` (2)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Parametrize from Registered Rasters with Texture | `img_patch_param` | `Parametrization/UV Creation` · `Texture` · `Transfer/From Rasters` | <sub>touches: texture, uv</sub> |
| Parametrize from Registered Rasters | `img_patch_param` | `Parametrization/UV Creation` · `Transfer/From Rasters` | <sub>touches: uv</sub> |

### `Quality` (7)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Compute Curvature (Discrete) | `colorproc` | `Attribute/Curvature` | <sub>touches: scalar</sub> |
| Compute Face Scalar from Geometry | `colorproc` | `Attribute/Scalar` | <sub>touches: scalar</sub> |
| Compute UV Distortion | `colorproc` | `Attribute/Scalar` | <sub>touches: scalar</sub> |
| Adjust Vertex Color Brightness/Contrast/Gamma | `colorproc` | `Attribute/Color` | **mis-filed** under `Quality` <sub>touches: color</sub> |
| Set Vertex Color | `colorproc` | `Attribute/Color` | **mis-filed** under `Quality` <sub>touches: color</sub> |
| Invert Vertex Color | `colorproc` | `Attribute/Color` | **mis-filed** under `Quality` <sub>touches: color</sub> |
| Threshold Vertex Color | `colorproc` | `Attribute/Color` | **mis-filed** under `Quality` <sub>touches: color</sub> |

### `Quality/Embree` (3)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Compute Ambient Occlusion | `embree` | `Attribute/Scalar` | backend dropped <sub>touches: scalar</sub> |
| Compute Obscurance | `embree` | `Attribute/Scalar` | backend dropped <sub>touches: scalar</sub> |
| Compute Shape Diameter Function | `embree` | `Attribute/Scalar` | backend dropped <sub>touches: scalar</sub> |

### `Quality/Geodesic` (4)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Compute Geodesic Distance from Border | `geodesic` | `Attribute/Scalar` | backend dropped <sub>touches: scalar</sub> |
| Compute Geodesic Distance from Point | `geodesic` | `Attribute/Scalar` | backend dropped <sub>touches: scalar</sub> |
| Compute Geodesic Distance from Selection | `geodesic` | `Attribute/Scalar` | backend dropped <sub>touches: scalar</sub> |
| Compute Geodesic Distance from Selection (Heat Method) | `geodesic` | `Attribute/Scalar` | backend dropped <sub>touches: scalar</sub> |

### `Raster` (5)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Transfer Color from Visible Rasters to Vertex | `color_projection` | `Transfer/From Rasters` · `Attribute/Color` | <sub>touches: color</sub> |
| Transfer Color from Visible Rasters to Texture | `color_projection` | `Transfer/From Rasters` · `Texture` | <sub>touches: uv</sub> |
| Transfer Color from Current Raster to Vertex | `color_projection` | `Transfer/From Rasters` · `Attribute/Color` | <sub>touches: color</sub> |
| Compute Face Scalar from Raster Coverage | `img_patch_param` | `Attribute/Scalar` · `Transfer/From Rasters` | **mis-filed** as `Raster` <sub>touches: scalar</sub> |
| Compute Vertex Scalar from Raster Coverage | `img_patch_param` | `Attribute/Scalar` · `Transfer/From Rasters` | **mis-filed** as `Raster` <sub>touches: scalar</sub> |

### `Remeshing` (8)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Reconstruct Surface by Alpha Wrapping | `cgal` | `Creation/Reconstruction` | **mis-filed** as Remeshing |
| Create Solid Wireframe | `voronoi` | `Creation/Primitives` | builds a new structure |
| Curvature flipping optimization | `trioptimize` | `Meshing/Remeshing` | <sub>touches: scalar</sub> |
| Global Align Meshes | `icp` | `Geometry/Alignment` | **mis-filed** as Remeshing <sub>touches: texture</sub> |
| ICP Between Meshes | `icp` | `Geometry/Alignment` | **mis-filed** as Remeshing <sub>touches: texture</sub> |
| Planar flipping optimization | `trioptimize` | `Meshing/Remeshing` | <sub>touches: scalar</sub> |
| Refine User-Defined | `func` | `Meshing/Subdivision` |  |
| Reconstruct Surface by Ball Pivoting (vcglib) | `clean` | `Creation/Reconstruction` | **not cleaning** - reconstruction |

### `Remeshing, Smoothing and Resampling` (2)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Simplification: Edge Collapse for Marching Cube meshes | `plymc` | `Meshing/Simplification` |  |
| Reconstruct Surface by Volumetric Merging | `plymc` | `Creation/Reconstruction` |  |

### `Remeshing/Surface Reconstruction` (3)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Reconstruct Surface by Smooth Signed Distance | `screened_poisson` | `Creation/Reconstruction` |  |
| Reconstruct Surface by Screened Poisson | `screened_poisson` | `Creation/Reconstruction` |  |
| Trim Surface by Scalar Isovalue | `screened_poisson` | `Creation/Reconstruction` |  |

### `Sampling` (17)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Sample Vertices by Clustering | `sampling` | `Creation/Sampling` |  |
| Colorize Vertices by Disk Distance | `sampling` | `Attribute/Color` | <sub>touches: scalar</sub> |
| Compute Distance from Reference Mesh | `sampling` | `Measurement/Geometric` · `Attribute/Scalar` | **mis-filed** - it measures <sub>touches: scalar</sub> |
| Measure Hausdorff Distance | `sampling` | `Measurement/Geometric` · `Attribute/Scalar` | **mis-filed** - it measures |
| Sample Mesh Elements | `sampling` | `Creation/Sampling` |  |
| Sample Surface by Monte Carlo | `sampling` | `Creation/Sampling` |  |
| Point Cloud Simplification | `sampling` | `Meshing/Simplification` | **mis-filed** as Sampling |
| Sample Surface by Poisson Disk | `sampling` | `Creation/Sampling` |  |
| Sample Offset Surface Recursively | `sampling` | `Creation/Sampling` |  |
| Sample Surface by Stratified Triangles | `sampling` | `Creation/Sampling` |  |
| Sample Texels | `sampling` | `Creation/Sampling` |  |
| Uniform Mesh Resampling | `sampling` | `Meshing/Remeshing` | **mis-filed** as Sampling |
| Transfer Vertex Attributes by Closest Point | `sampling` | `Transfer/Within a Mesh` | **mis-filed** as Sampling <sub>touches: color, scalar, selection</sub> |
| Sample Volume | `voronoi` | `Creation/Sampling` |  |
| Sample Surface by Voronoi Relaxation | `voronoi` | `Creation/Sampling` | <sub>touches: color, scalar, selection</sub> |
| Create Voronoi Scaffolding | `voronoi` | `Creation/Primitives` | builds a new structure |
| Colorize Vertices by Voronoi Regions | `sampling` | `Attribute/Color` | <sub>touches: color</sub> |

### `Selection` (27)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Conditional Face Selection | `func` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Conditional Vertex Selection | `func` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Delete ALL Faces | `select` | `Meshing/Deletion` **(new)** | **deletes geometry** - not a Selection op |
| Delete Selected Faces | `select` | `Meshing/Deletion` **(new)** | **deletes geometry** - not a Selection op |
| Delete Selected Faces and Vertices | `select` | `Meshing/Deletion` **(new)** | **deletes geometry** - not a Selection op |
| Delete Selected Vertices | `select` | `Meshing/Deletion` **(new)** | **deletes geometry** - not a Selection op |
| Dilate Selection | `select` | `Selection/Set Operations` **(new)** | selection algebra, not a criterion <sub>touches: selection</sub> |
| Erode Selection | `select` | `Selection/Set Operations` **(new)** | selection algebra, not a criterion <sub>touches: selection</sub> |
| Invert Selection | `select` | `Selection/Set Operations` **(new)** | selection algebra, not a criterion <sub>touches: selection</sub> |
| Select 'Problematic' Faces | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |
| Select All | `select` | `Selection/Set Operations` **(new)** | selection algebra, not a criterion <sub>touches: selection</sub> |
| Select Border | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |
| Select Connected Faces | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |
| Select Faces by Color | `select` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Select Faces by View Angle | `select` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Select Faces from Vertices | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |
| Select Faces with Edges Longer Than... | `select` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Select None | `select` | `Selection/Set Operations` **(new)** | selection algebra, not a criterion <sub>touches: selection</sub> |
| Select Outliers | `select` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Select Self Intersecting Faces | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |
| Select Vertex Texture Seams | `select` | `Selection/by Topology` · `Parametrization` | seam = UV topology <sub>touches: selection</sub> |
| Select Vertices from Faces | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |
| Select by Face Quality | `select` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Select by Rectangle (Screen) | `select` | `Selection/by Visibility` | <sub>touches: selection</sub> |
| Select by Vertex Quality | `select` | `Selection/by Attribute` | <sub>touches: selection</sub> |
| Select non Manifold Edges | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |
| Select non Manifold Vertices | `select` | `Selection/by Topology` | <sub>touches: selection</sub> |

### `Selection/Embree` (1)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Select Visible Faces | `embree` | `Selection/by Visibility` | <sub>touches: selection</sub> |

### `Smoothing` (16)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Cut mesh along crease edges | `unsharp` | `Meshing/Remeshing` | **topology change**, not smoothing |
| Depth Smooth | `unsharp` | `Geometry/Smoothing` |  |
| Directional Geometry Preservation | `unsharp` | `Geometry/Smoothing` |  |
| Compute Harmonic Scalar Field | `unsharp` | `Attribute/Scalar` | <sub>touches: scalar</sub> |
| HC Laplacian Smooth | `unsharp` | `Geometry/Smoothing` |  |
| Laplacian Smooth | `unsharp` | `Geometry/Smoothing` |  |
| Laplacian Smooth (surface preserving) | `trioptimize` | `Geometry/Smoothing` |  |
| Random Vertex Displacement | `sample` | `Geometry/Deformation` **(new)** | **adds** noise - not smoothing |
| ScaleDependent Laplacian Smooth | `unsharp` | `Geometry/Smoothing` |  |
| Smooth Vertex Scalar | `unsharp` | `Attribute/Scalar` | **scalar**, not color <sub>touches: scalar</sub> |
| Taubin Smooth | `unsharp` | `Geometry/Smoothing` |  |
| TwoStep Smooth | `unsharp` | `Geometry/Smoothing` |  |
| Sharpen Vertex Color by Unsharp Mask | `unsharp` | `Attribute/Color` | <sub>touches: color</sub> |
| UnSharp Mask Geometry | `unsharp` | `Geometry/Smoothing` |  |
| Sharpen Vertex Scalar by Unsharp Mask | `unsharp` | `Attribute/Scalar` | <sub>touches: scalar</sub> |
| Vertex Linear Morphing | `unsharp` | `Geometry/Deformation` **(new)** |  |

### `Texture` (16)

| Filter | Plugin | Applied categories | Note |
|---|---|---|---|
| Convert Per-Vertex UV to Per-Wedge UV | `texture` | `Parametrization/UV Conversion` | <sub>touches: uv</sub> |
| Convert Per-Wedge UV to Per-Vertex UV | `texture` | `Parametrization/UV Conversion` | <sub>touches: uv</sub> |
| Convert Object-Space Normal Map to Tangent Space | `texture` | `Texture/Conversion` | <sub>touches: texture</sub> |
| Parametrize by Harmonic Map (libigl) | `parametrization` | `Parametrization/UV Creation` | <sub>touches: uv</sub> |
| Parametrize by Least Squares Conformal Maps (libigl) | `parametrization` | `Parametrization/UV Creation` | <sub>touches: uv</sub> |
| Pack Texture Images | `texture` | `Texture/Packing` · `Parametrization/Atlas Packing` |  |
| Parametrize by Flat Plane | `texture` | `Parametrization/UV Creation` | <sub>touches: uv</sub> |
| Parametrize by Trivial Per-Triangle Layout | `texture` | `Parametrization/UV Creation` | <sub>touches: uv</sub> |
| Parametrize by Voronoi Atlas (vcglib) | `texture` | `Parametrization/UV Creation` |  |
| Parametrize by Atlas (xatlas) | `xatlas` | `Parametrization/UV Creation` | <sub>touches: texture, uv</sub> |
| Set Texture | `texture` | `Texture/Assignment` | <sub>touches: texture</sub> |
| Merge Texture Islands | `texture_defragmentation` | `Parametrization/Defragmentation` · `Texture` | chart surgery; image resample is a consequence |
| Defragment Texture Atlas | `texture_defragmentation` | `Parametrization/Defragmentation` · `Texture` | chart surgery; image resample is a consequence |
| Transfer Color from Texture to Vertex by Closest Point | `texture` | `Transfer/Between Layers` · `Attribute/Color` | <sub>touches: color</sub> |
| Transfer Vertex Attributes to Texture by Closest Point | `texture` | `Transfer/Between Layers` · `Texture` | <sub>touches: texture</sub> |
| Transfer Color from Vertex to Texture | `texture` | `Transfer/Between Layers` · `Texture` | <sub>touches: texture</sub> |
