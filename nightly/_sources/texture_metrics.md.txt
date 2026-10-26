# Texture and UV measures

MeshLab reports the statistics of [TexMetro](https://github.com/cnr-isti-vclab/texmetro), the tool
behind the benchmark *Real-World Textured Things*, as filters, with TexMetro's own output keys. They run
on the CPU, so they give the same numbers on every machine and need no graphics context, and they work
on a layer in the document, in the GUI and in scripts.

## The filters

| Filter | What it does |
|---|---|
| *Measure UV Mapping* | UV islands, their edges and boundary, folds, mapped fraction, occupancy, and the statistics of the quasi-conformal distortion and of the UV scaling (`qcd_*`, `sf_*`), with the UV taken in texels. |
| *Measure Texel Coverage* | How the UV triangles cover the texels of each texture, at its size and at every mipmap level (`texture_N`). |
| *Measure Texture Seam Color Discrepancy* | How much the colors of the texture differ across its seams (`seam_discrepancy_*`). |
| *Measure Topological Properties*, *Measure Geometric Properties* | The mesh keys: counts, boundary, genus, duplicate, zero-area and degenerate faces, orientation, normalized area and boundary length. |
| *Compute UV Distortion* | Quasi-conformal distortion and UV scaling per face, as face quality, to look at. |
| *Select Folded UV Faces*, *Select Overlapping UV Faces* | The faces behind `nfolds`, and the faces on the texels that `overwrittenTexels` and `texelClashes` count. |
| *Create Polyline from UV Seams* | The seams as a polyline, each edge carrying the discrepancy of its seam as its scalar. |

## TexMetro's report from a script

`docs/examples/texmetro_report.py` in the repository runs the filters on a model and writes TexMetro's JSON
file, key for key and in TexMetro's order, so that scripts written for its output keep working and the two
tools can be compared. It needs `pymeshlab`, or MeshLab's own Python console, and the model's texture images
next to the model:

```
python texmetro_report.py model.obj                          # writes texmetro.json
python texmetro_report.py model.obj -t 2048x1024             # a model with no image, as TexMetro's -t
python texmetro_report.py model.obj --compare reference.json # and compare with a file TexMetro wrote
```

`--compare` lists the keys that differ by more than a tolerance and exits with 1 if there are any. The
tolerances are exact for the counts of the mesh and of the UV layout, a few parts in ten thousand for the common
texel counters, a few percent for the rare ones (overwritten, lost, clash), and the six significant digits that
TexMetro writes for everything else. They are a starting point, from what is known of where the two differ, and
are meant to be set from real comparisons.

## The keys

Each key of TexMetro's JSON, and the filter that reports it. Most UV keys keep their names; the mesh keys, which
sit beside MeshLab's own, have MeshLab's. The table is printed by `texmetro_report.py --key-table`.

| TexMetro key | MeshLab filter | Output value |
|---|---|---|
| `fn` | `measure_topological_properties` | `faces_number` |
| `vn` | `measure_topological_properties` | `vertices_number` |
| `en` | `measure_topological_properties` | `edges_number` |
| `en_b` | `measure_topological_properties` | `boundary_edges` |
| `area` | `measure_geometric_properties` | `surface_area` |
| `boundary_len` | `measure_geometric_properties` | `boundary_length` |
| `area_normalized` | `measure_geometric_properties` | `normalized_surface_area` |
| `boundary_len_normalized` | `measure_geometric_properties` | `normalized_boundary_length` |
| `boundary_loops` | `measure_topological_properties` | `number_holes` |
| `nonmanif_edge` | `measure_topological_properties` | `non_two_manifold_edges` |
| `nonmanif_vert` | `measure_topological_properties` | `non_two_manifold_vertices` |
| `connected_components` | `measure_topological_properties` | `connected_components_number` |
| `genus` | `measure_topological_properties` | `genus` |
| `fndup` | `measure_topological_properties` | `duplicate_faces` |
| `fnzero` | `measure_topological_properties` | `zero_area_faces` |
| `fndegen` | `measure_topological_properties` | `degenerate_faces` |
| `vnunref` | `measure_topological_properties` | `unreferenced_vertices` |
| `oriented` | `measure_topological_properties` | `is_mesh_oriented` |
| `en_uv` | `measure_uv_mapping` | `en_uv` |
| `en_uv_b` | `measure_uv_mapping` | `en_uv_b` |
| `area_uv` | `measure_uv_mapping` | `area_uv` |
| `boundary_len_uv` | `measure_uv_mapping` | `boundary_len_uv` |
| `num_charts` | `measure_uv_mapping` | `num_charts` |
| `num_null_charts` | `measure_uv_mapping` | `num_null_charts` |
| `mapped_fraction` | `measure_uv_mapping` | `mapped_fraction` |
| `nfolds` | `measure_uv_mapping` | `nfolds` |
| `occupancy` | `measure_uv_mapping` | `occupancy` |
| `seam_discrepancy_avg` | `measure_texture_seam_discrepancy` | `seam_discrepancy_avg` |
| `seam_discrepancy_min` | `measure_texture_seam_discrepancy` | `seam_discrepancy_min` |
| `seam_discrepancy_max` | `measure_texture_seam_discrepancy` | `seam_discrepancy_max` |
| `seam_discrepancy_rms` | `measure_texture_seam_discrepancy` | `seam_discrepancy_rms` |
| `seam_discrepancy_var` | `measure_texture_seam_discrepancy` | `seam_discrepancy_var` |
| `seam_discrepancy_stddev` | `measure_texture_seam_discrepancy` | `seam_discrepancy_stddev` |
| `qcd_perc1` | `measure_uv_mapping` | `qcd_perc1` |
| `qcd_perc10` | `measure_uv_mapping` | `qcd_perc10` |
| `qcd_perc20` | `measure_uv_mapping` | `qcd_perc20` |
| `qcd_perc30` | `measure_uv_mapping` | `qcd_perc30` |
| `qcd_perc40` | `measure_uv_mapping` | `qcd_perc40` |
| `qcd_perc50` | `measure_uv_mapping` | `qcd_perc50` |
| `qcd_perc60` | `measure_uv_mapping` | `qcd_perc60` |
| `qcd_perc70` | `measure_uv_mapping` | `qcd_perc70` |
| `qcd_perc80` | `measure_uv_mapping` | `qcd_perc80` |
| `qcd_perc90` | `measure_uv_mapping` | `qcd_perc90` |
| `qcd_perc99` | `measure_uv_mapping` | `qcd_perc99` |
| `qcd_min` | `measure_uv_mapping` | `qcd_min` |
| `qcd_max` | `measure_uv_mapping` | `qcd_max` |
| `qcd_avg` | `measure_uv_mapping` | `qcd_avg` |
| `qcd_rms` | `measure_uv_mapping` | `qcd_rms` |
| `qcd_var` | `measure_uv_mapping` | `qcd_var` |
| `qcd_stddev` | `measure_uv_mapping` | `qcd_stddev` |
| `qcd_discardedArea` | `measure_uv_mapping` | `qcd_discardedArea` |
| `sf_perc1` | `measure_uv_mapping` | `sf_perc1` |
| `sf_perc10` | `measure_uv_mapping` | `sf_perc10` |
| `sf_perc20` | `measure_uv_mapping` | `sf_perc20` |
| `sf_perc30` | `measure_uv_mapping` | `sf_perc30` |
| `sf_perc40` | `measure_uv_mapping` | `sf_perc40` |
| `sf_perc50` | `measure_uv_mapping` | `sf_perc50` |
| `sf_perc60` | `measure_uv_mapping` | `sf_perc60` |
| `sf_perc70` | `measure_uv_mapping` | `sf_perc70` |
| `sf_perc80` | `measure_uv_mapping` | `sf_perc80` |
| `sf_perc90` | `measure_uv_mapping` | `sf_perc90` |
| `sf_perc99` | `measure_uv_mapping` | `sf_perc99` |
| `sf_min` | `measure_uv_mapping` | `sf_min` |
| `sf_max` | `measure_uv_mapping` | `sf_max` |
| `sf_avg` | `measure_uv_mapping` | `sf_avg` |
| `sf_rms` | `measure_uv_mapping` | `sf_rms` |
| `sf_var` | `measure_uv_mapping` | `sf_var` |
| `sf_stddev` | `measure_uv_mapping` | `sf_stddev` |
| `sf_discardedArea` | `measure_uv_mapping` | `sf_discardedArea` |
| `mesh` | the file name of the model |  |
| `ntex` | `measure_texel_coverage` | ntex |
| `texture_N` | `measure_texel_coverage` | texture_N: a list with an entry per mipmap level, with TexMetro's keys |
| `orientable` | not reported | OrientCoherentlyMesh flips faces, so no report can use it |
| `textures` | not reported | the layer knows its textures; ntex is their number |

## Where the numbers differ from TexMetro's

- **Texels are counted on the CPU.** TexMetro renders on a GPU, whose rounding of the triangles' vertices
  (its subpixel precision: 4 bits on some drivers, 8 on most hardware) moves the rare counters, the
  overwritten, lost and clash texels, by about 0.1 % to 1 %. MeshLab rounds to 8 bits by default, as most
  hardware does, and *Subpixel Bits* changes it. A texel centre exactly on an edge shared by two triangles
  goes to one of them only.
- **`nfolds`** does not count a face with no UV area as a fold. TexMetro counts it when the sign of its zero
  differs from its island's: 20 faces of 1,231 on one real atlas.
- **`texelClashes`** looks at the four diagonal neighbours of a texel, as TexMetro's code does, whichever
  was meant; *Clash Neighbours* offers all eight.
- **`seam_discrepancy_*`** compares, as TexMetro does, the i-th sample of each side's own edge. The two faces
  list a shared edge in opposite directions, so this compares a point with the mirrored point of the other side,
  and reads about 16 % higher than comparing the same point, which *Pairing* offers.
- **Not reproduced:** TexMetro's recentring of UVs whose box lies outside the unit square, and its zeros for a
  texture larger than its GPU's limit (MeshLab counts it, in bands if it is large). `orientable` is not reported,
  and `oriented` is absent for a mesh with non-manifold edges, where an orientation means nothing.
- **Precision.** The mesh areas and lengths are summed in single precision, as MeshLab stores the mesh. The
  distortion and texel measures are in double precision, on a copy of the layer in texels.
