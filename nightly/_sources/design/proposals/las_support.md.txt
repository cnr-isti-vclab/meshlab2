# LAS/LAZ Support

This document proposes reading and writing ASPRS LAS point clouds and their compressed form,
LAZ, in MeshLab. It also proposes the piece any georeferenced format needs and MeshLab lacks:
a **document origin**, so that coordinates in the millions keep their precision. **Nothing
described here is implemented** except the document origin (§2), done for E57.

See also: [Load Options](load_options.md) (how the user chooses what to read),
[Data Model](../data_model.md) (layers, units, provenance),
[Memory Accounting](../memory_accounting.md), [Third-Party Code](../third_party.md),
`plugins/io_e57` (the other point-cloud format, and the one that measured the problem).

## Status

| Stage | What | State |
|---|---|---|
| 1 | Document origin: offset subtracted in double precision on load, added back on save; E57 first | done for E57, 2026-11-28 |
| 2 | `io_las`: LAS and LAZ through LASzip, read and write, fields as in §4 and §5 | not started |
| 3 | Load options for LAS: point budget, fields to keep ([Load Options](load_options.md)) | not started; follows Load Options stage 1 |
| 4 | COPC: load a chosen level of detail or region of a cloud-optimized file | not started; optional |

## 1. What LAS is

The ASPRS LAS format (1.0 to 1.4) holds **one point cloud** and no faces. A header gives the
bounds, point count, **scale and offset**: each coordinate is a 32-bit integer `X`, and the
position is `X * scale + offset` in double precision. Point records come in formats 0 to 10:

- every format has intensity, return number and count, classification, scan angle, user
  data and point source ID;
- GPS time from format 1 on, RGB (16 bits per channel) in 2, 3, 5, 7, 8 and 10, near-infrared
  in 8 and 10, and waveform packets in 4, 5, 9 and 10;
- from 1.4 on, extra per-point fields of any type ("extra bytes", declared in a VLR). PDAL and
  others write normals there as `NormalX`/`NormalY`/`NormalZ`.

The coordinate system is a variable-length record (WKT in 1.4, GeoTIFF keys before), whose
linear unit is usually the meter and sometimes the US survey foot. LAZ is the same content
compressed losslessly by LASzip. COPC (cloud-optimized point cloud) is a LAZ 1.4 file whose
points are ordered in an octree described by an extra VLR. Any LAZ reader reads it whole.

## 2. The real problem: precision

`VCGVertex` stores positions in float (`vcg::vertex::Coord3f`, `src/core/vcgmesh.h`), and the
layer matrix is a float `QMatrix4x4` (`Document::meshTransform`). A float has a 24-bit mantissa.
At a UTM northing of 5,000,000 m it can only store multiples of 0.5 m, and at an easting of
500,000 m multiples of 3 cm. Most LAS files are in such coordinates. Moving the offset into the
layer matrix does not help, because the matrix is float too.

**Measured on E57**, which MeshLab already reads, 2026-11-25. A 100 × 100 grid with 1 mm
spacing was written as doubles with libE57Format and read back through `load_new_mesh`:

| File | Distinct x | Distinct y | Largest error |
|---|---|---|---|
| grid at (10, 20, 1) | 100 | 100 | 0.0 mm |
| grid at (500000, 5000000, 100), in the coordinates | 4 | 1 | 15.5 mm in x, 99.0 mm in y |
| grid at the origin, the scan pose translating it to (500000, 5000000, 100) | 4 | 1 | 15.5 mm in x, 99.0 mm in y |

The 10,000 points come back as 4 columns and 1 row: the survey is gone. The importer converts
each point to float after applying the scan pose
(`vcg::Point3f::Construct(rotation.Rotate(p) + translation)`, `plugins/io_e57/e57importplugin.cpp`)
and subtracts no offset, so a scan placed by its pose loses precision exactly as one stored in
absolute coordinates does.

**The document origin** (stage 1, implemented 2026-11-28 for E57). `Document` has an origin,
a `vcg::Point3d` in double precision (`Document::origin()`); every layer's coordinates are
relative to it.

- **Only georeferenced formats use it.** A format whose coordinates are true, survey ones
  (`MeshIOPlugin::isGeoreferenced`: E57 now, LAS next) subtracts the origin on load and adds
  it back on save. Every other format holds coordinates relative to it, both ways, so a
  project whose layers are float PLY files reopens where it was. Saving a non-georeferenced
  format with an origin set writes local coordinates and logs the origin to add.
- **On load**, the importer subtracts the origin **in double precision, before converting to
  float**: once the values are float the precision is already lost. The origin goes in and
  out through `MeshIOLoadContext` (`src/plugins/meshioplugin.h`), which replaced the
  `LengthUnit *outUnit` of the last `load` overload and is where load options will go. The
  photos' cameras (`loadRasters`) are relative to the same origin.
- **Which origin.** If the document has one, every new layer uses it, so layers still align;
  one that is still far from zero relative to it is reported with the precision it keeps.
  If it has none and the file is far from zero, the file proposes one, and the document adopts
  it when the layer is its only one (no other layer or raster, not inside a project). Otherwise
  the layer is moved back to the file's coordinates, as precise as float allows there, and the
  log says so: open it in a document of its own. **Far** (`isFarFromZero`) means the float step
  at the bounding box's centre is coarser than a millionth of its diagonal, about 16 diagonals
  from zero. The first rule written here, "the centre farther from zero than the diagonal is
  long", gave an origin to a 2 m scan 5 m from zero, which loses nothing to float. E57
  proposes the corner of its bounding box rounded down to whole units; it reads the points
  relative to the first one, rounded, so it needs no second pass over the file.
- **Shown**: the status bar shows the origin when one is set, and the measuring tool's printed
  and saved points include it. Lengths are unaffected.
- **Kept** in the project as `<Origin x="..." y="..." z="..."/>`, a sibling of `<LengthUnit>`,
  which legacy MeshLab skips. A project's origin is taken only into an empty document;
  otherwise the log says the layers will be misplaced. *File > New* clears it.
- **Undone with the load that set it**: it is part of the undo state, like the unit. It is
  never edited by a filter.
- **Python**: `ms.document_origin()`, `(x, y, z)` or `None`.

**Per document, not per layer.** A per-layer origin would serve one case better, layers far
apart (tiles 50 km apart, two sites in one document), but it breaks the invariant every
multi-layer algorithm, the renderer and the trackball rely on: a layer's world position is its
matrix times its vertex. Each would have to reconcile origins, and folding their difference
into the float layer matrix only works when it is small, which is when one origin suffices.
CloudCompare's per-entity "global shift" offers to reuse the previous shift for the same
reason. Layers far from the document origin are reported instead; a per-layer origin would be
a proposal of its own, and nothing here prevents it.

The regression test is the grid above (`e57FarFromZeroKeepsItsPrecision`): 10,000 distinct
points, each within 0.01 mm, in the coordinates and placed by the pose, its camera too, and
true coordinates written back on save. `theDocumentOriginTravelsWithTheProject` covers the
project.

## 3. The library: LASzip

| | LASzip 3.5 | PDAL | LAStools (LASlib) | libLAS |
|---|---|---|---|---|
| Licence | Apache-2.0 | BSD | mixed, some parts proprietary | BSD |
| Dependencies | none | GDAL, PROJ, GeoTIFF, libxml2... | none | Boost, GDAL optional |
| Reads/writes LAS and LAZ, 1.4 | yes, through its C API (`laszip_api.h`) | yes | yes | no 1.4; abandoned |
| Extra bytes | yes | yes | yes | no |
| vcpkg | `laszip` 3.5.0 | `pdal` | `lastools` | `liblas` |

**LASzip.** It is a small library that reads and writes both plain LAS and LAZ through one API,
which is all an importer needs. PDAL is a pipeline framework, and its dependency chain is
far heavier than one file format justifies. The coordinate-system VLR is
kept as text (WKT) for the round trip, so no PROJ is needed: MeshLab does not reproject.

## 4. Import

| LAS | MeshLab |
|---|---|
| `X * scale + offset`, in double | position minus the document origin (§2), then float |
| RGB, 16 bits per channel | vertex color, scaled to 8 bits. Some writers store 0–255 in the 16-bit fields, so the scale is decided by the file's maximum, as the E57 importer uses color limits |
| intensity | vertex scalar (quality), so the color-map tools work on it immediately |
| classification | named `int` attribute `classification`, selectable by expression (`classification == 2` selects ground) |
| return number, number of returns, scan angle, user data, point source ID | named `int` attributes |
| GPS time | named attribute; `float` loses it (seconds since an epoch), so it is offset by its minimum like the coordinates, or dropped by default (open question 3) |
| near-infrared | named `int` attribute |
| extra bytes | named attributes; three named as normals become vertex normals |
| synthetic, key-point, withheld and overlap flags | withheld points dropped (the standard says to ignore them); the others as one named `int` attribute of bits |
| waveform packets | dropped, with a log line |
| coordinate system VLR | the length unit through `MeshIOLoadContext::unit` (meter, foot, US survey foot as foot, with a log line); the WKT kept as layer metadata, written back on save |

Expression filters already read named per-vertex `int`, `float` and `Point3f` attributes
(`plugins/filter_expression/expressionfilterplugin.cpp`). Before relying on them, check
that named attributes survive undo snapshots and `tri::Append`.

**Size.** One airborne tile is often 10 to 100 million points, a mobile or terrestrial
survey more. A vertex takes about 40 bytes before named attributes, so 100 million points is
about 4 GB. The default load is therefore not "everything" but **at most a point budget**,
30 million points to start, by an even stride. The stride keeps the scan pattern and needs no
second pass. Above the budget the log says how many points were skipped; *Reload with Options...* on
the layer loads them all ([Load Options](load_options.md)). A spatially even subset is a filter's job once the
points are in.

## 5. Export

| MeshLab | LAS |
|---|---|
| a point-cloud layer (a mesh layer writes its vertices only, with a log line) | one file; `.laz` compressed, `.las` not |
| position plus the document origin | `X` with scale 0.001 file units by default, offset the document origin, or the bounding box minimum when there is none |
| vertex color | RGB, 8 bits scaled to 16 |
| scalar | intensity when the layer's scalar came from intensity, otherwise a `float` extra field `scalar` |
| normals | extra fields `NormalX/Y/Z` |
| named attributes with the names of §4 | their standard fields |
| other named attributes | extra fields |
| unit and kept WKT | the coordinate-system VLR |

The point format is the smallest that holds what is written: 6 without color, 7 with it, 8
with near-infrared. This means LAS 1.4, which old readers do not open (open question 1).

## 6. COPC (stage 4)

A COPC file stores its points in octree order, and its hierarchy VLR gives the point range of
each node. Reading a level of detail, or the nodes inside a box, is then a seek and a read per
node, through LASzip's seek. That turns the point budget of §4 from a stride into a spatially
even subset, and lets a region of a national survey be opened without reading the rest. It is
a few hundred lines of hierarchy parsing, worth doing once stage 2 is in use.

## 7. Validation

- Fixtures in `tests/sample_mesh/las/`: small LAS and LAZ files in formats 1, 3, 6, 7 and 8,
  one with extra bytes and one with RGB stored in 8 bits. Synthetic ones are generated with
  LASzip; one real airborne tile goes in the dataset manifest, not the repository
  ([Test Data](../test_data.md)).
- `tests/test_document.cpp`: a georeferenced round trip (load, save, load) that keeps every
  point within half the file's scale, run on LAS and on E57; the E57 grid above as the
  regression test of stage 1.
- Descriptor and smoke tiers are unaffected, as for any importer.

## 8. Cost

| Piece | Estimate |
|---|---|
| Document origin, project element, Python, E57 adoption | 150–250 lines |
| `io_las` read and write | 500–700 lines |
| Load options for LAS (with [Load Options](load_options.md) in place) | under 100 lines |
| COPC level of detail | 300 lines |

One new vcpkg dependency, LASzip, with no further dependencies.

## 9. Open questions

1. **LAS 1.4 or 1.2 on export.** 1.4 holds 8-bit classification, extra bytes and the WKT, but
   some older tools refuse it. Should 1.4 be written always, or 1.2 when nothing needs 1.4?
2. ~~**Origin on non-georeferenced formats.**~~ Settled 2026-11-28: they write local
   coordinates and the log gives the origin. A save option to write true ones can come later.
3. **GPS time.** Kept by default, offset by its minimum, or dropped unless asked for? It is
   8 bytes a point in double and useless to most users.
4. **The point budget.** Is 30 million points right, and should it follow the memory
   preferences rather than be fixed?
