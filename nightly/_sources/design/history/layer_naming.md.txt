# Layer naming, applied 2026-09-19

Every filter that created a layer named it itself, in six incompatible shapes, and
about half recorded no provenance at all. The rule that replaced them is in
[vocabulary.md](../vocabulary.md) section 7; this records the migration.

## Measured before and after

Both columns were read back from a real run against a source layer called `bunny`
(and `cube` for two-operand filters), not from the source literals. Filters the probe
could not set up on its fixtures are absent from the table; their tags came from the
literal and are visible in the descriptor diff.

| filter | before | after |
|---|---|---|
| `create_annulus` | Annulus | Annulus |
| `create_circle` | Circle | Circle |
| `create_cone` | Cone | Cone |
| `create_convex_hull` | Convex Hull | bunny (hull) |
| `create_cylinder` | Cylinder | Cylinder |
| `create_dodecahedron` | Dodecahedron | Dodecahedron |
| `create_grid` | Grid | Grid |
| `create_hexahedron` | Hexahedron | Hexahedron |
| `create_icosahedron` | Icosahedron | Icosahedron |
| `create_isosurface_from_expression` | Implicit Surface | Implicit Surface |
| `create_isosurface_from_perlin_noise` | Noisy Isosurface | Noisy Isosurface |
| `create_mesh_bounding_box` | bunny bounding box | bunny (bbox) |
| `create_octahedron` | Octahedron | Octahedron |
| `create_plane_from_selection` | Fitted Plane | bunny (fitted plane) |
| `create_points_on_sphere` | Points on Sphere | Points on Sphere |
| `create_points_on_spherical_cap` | Points on Spherical Cap | Points on Spherical Cap |
| `create_polyline_from_mesh_intersection_trueform` | Intersection Curve | bunny (intersection curve cube) |
| `create_polyline_from_planar_section` | bunny_sect | bunny (section) |
| `create_polyline_from_scalar_isocontour_trueform` | Isocontours | bunny (isocontours) |
| `create_polyline_from_selection_perimeter` | bunny_perimeter | bunny (perimeter) |
| `create_scene_bounding_box` | Scene bounding box | bunny (bbox) |
| `create_solid_wireframe` | Shell Mesh | bunny (wireframe) |
| `create_sphere` | Sphere | Sphere |
| `create_sphere_cap` | Sphere Cap | Sphere Cap |
| `create_square` | Square | Square |
| `create_symmetric_dodecahedron` | Dodecahedron (sym) | Symmetric Dodecahedron |
| `create_tetrahedron` | Tetrahedron | Tetrahedron |
| `create_torus` | Torus | Torus |
| `create_voronoi_scaffolding` | Scaffolding   [+2: Montecarlo Volume, Poisson-disk Samples] | bunny (scaffolding)   [+2: bunny (volume samples), bunny (poisson samples)] |
| `cut_along_scalar_isocontour_trueform` | Isobands | bunny (isobands) |
| `duplicate_current_layer` | bunny copy | bunny (copy) |
| `extract_outer_shell_trueform` | Outer Shell | bunny (outer shell) |
| `extract_selected_faces` | SelectedFacesSubset | bunny (selected faces) |
| `mesh_csg_expression_trueform` | CSG | bunny (csg) |
| `mesh_difference_libigl` | Boolean difference | bunny (difference cube) |
| `mesh_difference_trueform` | Difference | bunny (difference cube) |
| `mesh_intersection_libigl` | Boolean intersection | bunny (intersection cube) |
| `mesh_intersection_trueform` | Intersection | bunny (intersection cube) |
| `mesh_symmetric_difference_libigl` | Boolean xor | bunny (xor cube) |
| `mesh_symmetric_difference_trueform` | Symmetric Difference | bunny (xor cube) |
| `mesh_union_libigl` | Boolean union | bunny (union cube) |
| `mesh_union_trueform` | Union | bunny (union cube) |
| `parametrize_by_cylindrical_projection` | Unrolled Mesh | bunny (unrolled) |
| `parametrize_by_voronoi_atlas_vcglib` | VoroAtlas | bunny (voronoi atlas) |
| `reconstruct_surface_by_advancing_front` | Advancing Front | bunny (advancing front) |
| `reconstruct_surface_by_alpha_wrapping` | Alpha wrap | bunny (alpha wrap) |
| `reconstruct_surface_by_marching_cubes_apss` | bunny APSS MC | bunny (apss) |
| `reconstruct_surface_by_marching_cubes_rimls` | bunny RIMLS MC | bunny (rimls) |
| `reconstruct_surface_by_screened_poisson` | Poisson mesh | bunny (poisson) |
| `reconstruct_surface_by_smooth_signed_distance` | SSD mesh | bunny (ssd) |
| `reconstruct_surface_by_volumetric_merging` | Reconstruction_0 | bunny (volumetric merge) |
| `reconstruct_surface_by_voronoi_filtering` | Voronoi Filtering | bunny (crust) |
| `remesh_to_quads_instant_meshes` | Instant Meshes - bunny | bunny (instant quads) |
| `remesh_to_quads_quadwild_bimdf` | QuadWild-BiMDF - bunny | bunny (quadwild) |
| `remesh_uniformly_by_volumetric_resampling` | Offset Mesh | bunny (resampled) |
| `repair_self_intersections_trueform` | Resolved | bunny (resolved) |
| `repair_watertight_mesh_meshfix` | MeshFix - bunny | bunny (meshfix) |
| `sample_mesh_elements` | Element Samples | bunny (element samples) |
| `sample_offset_surface_recursively` | Recursive Samples | bunny (offset samples) |
| `sample_surface_by_monte_carlo` | Montecarlo Samples | bunny (montecarlo samples) |
| `sample_surface_by_poisson_disk` | Poisson-disk Samples | bunny (poisson samples) |
| `sample_surface_by_stratified_triangles` | Stratified Samples | bunny (stratified samples) |
| `sample_surface_by_voronoi_relaxation` | voro   [+1: poly] | bunny (voronoi samples)   [+1: bunny (voronoi cells)] |
| `sample_vertices_by_clustering` | Cluster Samples | bunny (cluster samples) |
| `sample_volume` | Montecarlo Volume   [+2: Poisson Sampling, Surface Sampling] | bunny (volume samples)   [+2: bunny (poisson samples), bunny (surface samples)] |
| `simplify_by_quadric_edge_collapse_qslim` | QSlim - bunny | bunny (qslim) |
| `simplify_by_vertex_clustering` | bunny_clustered | bunny (clustered) |
| `simplify_point_cloud` | Simplified Cloud | bunny (simplified) |
| `split_into_connected_components` | CC 0 | bunny (part 1) |
| `split_into_connected_components_trueform` | Component 0 | bunny (part 1) |
| `split_into_solid_domains_trueform` | Domain 1   [+7: Domain 2, Domain 3, Domain 4] | bunny (domain 1)   [+7: bunny (domain 2), bunny (domain 3), bunny (domain 4)] |

## Totals

| | |
|---|---|
| filters declared `NewMeshes` before | 86 |
| `measure_hausdorff_distance`, declared `Information` but creating two layers | +1 |
| filters carrying an `outputTag` after | 87 |
| of those, observed end to end in the sweep | 71 |

## Two descriptor faults the sweep exposed

`measure_hausdorff_distance` was declared `outputDomain: Information` while creating
two layers whenever its **Save Samples** option was on. Declared `NewMeshes`.

`reconstruct_surface_by_volumetric_merging` never populated `newMeshIndices`, so the
layers it created were invisible to the framework -- not only to naming, but to every
other thing keyed off that list. Now reported.

