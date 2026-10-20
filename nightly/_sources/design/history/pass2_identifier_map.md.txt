# Pass 2 — identifier map

**Applied 2026-09-03.** `id` and `pythonName` both become the snake_case of the display
name: punctuation collapses to `_`, `(Backend)` becomes a trailing `_backend`, and the
articles `the`/`a`/`an` drop. Nothing else is dropped -- connectors included -- so the
menu name and the API name are the same string in two spellings.

328 filters; 281 ids and 259 pythonNames change. All 328 derived names are
distinct, so no filter collides with another.

| Display name | current id | current pythonName | new id = pythonName |
|---|---|---|---|
| Measure Mesh Summary | `mesh_info` | `get_info` | `measure_mesh_summary` |
| Create Isosurface from Perlin Noise | `create_noisy_isosurface` | `generate_noisy_isosurface` | `create_isosurface_from_perlin_noise` |
| Set Mesh Camera | `set_camera_per_mesh` | `set_camera_per_mesh` | `set_mesh_camera` |
| Set Raster Camera | `set_camera_per_raster` | `set_camera_per_raster` | `set_raster_camera` |
| Compute Vertex Scalar from Camera | `compute_scalar_from_camera_per_vertex` | `compute_scalar_from_camera_per_vertex` | `compute_vertex_scalar_from_camera` |
| Rotate Cameras | `apply_cameras_rotation` | `apply_cameras_rotation` | `rotate_cameras` |
| Scale Cameras | `apply_cameras_scaling` | `apply_cameras_scaling` | `scale_cameras` |
| Translate Cameras | `apply_cameras_translation` | `apply_cameras_translation` | `translate_cameras` |
| Transform Camera Extrinsics | `apply_cameras_extrinsics_transformation` | `apply_cameras_extrinsics_transformation` | `transform_camera_extrinsics` |
| Orient Vertex Normals by Cameras | `compute_normal_from_cameras_per_vertex` | `compute_normal_from_cameras_per_vertex` | `orient_vertex_normals_by_cameras` |
| Set Camera to View Selection | `set_camera_to_view_selection` | `set_camera_to_view_selection` | `set_camera_to_view_selection` |
| Set Camera from Direction | `set_camera_from_direction` | `set_camera_from_direction` | `set_camera_from_direction` |
| Reconstruct Surface by Alpha Wrapping | `generate_alpha_wrap` | `generate_alpha_wrap` | `reconstruct_surface_by_alpha_wrapping` |
| Reconstruct Surface by Alpha Shape | `generate_alpha_shape` | `generate_alpha_shape` | `reconstruct_surface_by_alpha_shape` |
| Reconstruct Surface by Voronoi Filtering | `generate_voronoi_filtering` | `generate_voronoi_filtering` | `reconstruct_surface_by_voronoi_filtering` |
| Reconstruct Surface by Scale Space | `generate_scale_space_reconstruction` | `generate_scale_space_reconstruction` | `reconstruct_surface_by_scale_space` |
| Reconstruct Surface by Advancing Front | `generate_advancing_front_reconstruction` | `generate_advancing_front_reconstruction` | `reconstruct_surface_by_advancing_front` |
| Orient Point Cloud Normals | `compute_normal_orientation_per_vertex` | `compute_normal_orientation_per_vertex` | `orient_point_cloud_normals` |
| Reconstruct Surface by Poisson (CGAL) | `generate_poisson_reconstruction_cgal` | `generate_poisson_reconstruction_cgal` | `reconstruct_surface_by_poisson_cgal` |
| Reconstruct Surface by Kinetic Partition | `generate_kinetic_reconstruction` | `generate_kinetic_reconstruction` | `reconstruct_surface_by_kinetic_partition` |
| Reconstruct Surface by Ball Pivoting (vcglib) | `surface_reconstruction_ball_pivoting` | `generate_surface_reconstruction_ball_pivoting` | `reconstruct_surface_by_ball_pivoting_vcglib` |
| Remove Vertices by Scalar | `remove_vertices_wrt_quality` | `remove_vertices_wrt_quality` | `remove_vertices_by_scalar` |
| Remove Isolated Components by Face Count | `remove_isolated_pieces_face_num` | `remove_isolated_pieces_by_face_num` | `remove_isolated_components_by_face_count` |
| Remove Isolated Components by Diameter | `remove_isolated_pieces_diameter` | `remove_isolated_pieces_by_diameter` | `remove_isolated_components_by_diameter` |
| Remove T-Vertices | `remove_t_vertices` | `remove_t_vertices` | `remove_t_vertices` |
| Repair Mismatched Borders | `snap_mismatched_borders` | `snap_mismatched_borders` | `repair_mismatched_borders` |
| Merge Close Vertices | `merge_close_vertices` | `merge_close_vertices` | `merge_close_vertices` |
| Merge Close Wedge UVs | `merge_wedge_texture_coord` | `merge_wedge_texture_coords` | `merge_close_wedge_uvs` |
| Remove Duplicate Faces | `remove_duplicate_faces` | `remove_duplicate_faces` | `remove_duplicate_faces` |
| Remove Isolated Folded Faces by Edge Flip | `remove_folded_faces_by_edge_flip` | `remove_isolated_folded_faces` | `remove_isolated_folded_faces_by_edge_flip` |
| Repair Non-Manifold Edges | `repair_non_manifold_edges` | `repair_non_manifold_edges` | `repair_non_manifold_edges` |
| Repair Non-Manifold Vertices by Splitting | `repair_non_manifold_vertices_split` | `repair_non_manifold_vertices` | `repair_non_manifold_vertices_by_splitting` |
| Remove Unreferenced Vertices | `remove_unreferenced_vertices` | `remove_unreferenced_vertices` | `remove_unreferenced_vertices` |
| Remove Duplicate Vertices (vcglib) | `remove_duplicated_vertices` | `remove_duplicate_vertices` | `remove_duplicate_vertices_vcglib` |
| Remove Zero-Area Faces | `remove_zero_area_faces` | `remove_zero_area_faces` | `remove_zero_area_faces` |
| Transfer Color from Current Raster to Vertex | `compute_color_from_current_raster_projection` | `compute_color_from_current_raster_projection` | `transfer_color_from_current_raster_to_vertex` |
| Transfer Color from Visible Rasters to Vertex | `compute_color_from_active_rasters_projection` | `compute_color_from_active_rasters_projection` | `transfer_color_from_visible_rasters_to_vertex` |
| Transfer Color from Visible Rasters to Texture | `compute_color_and_texture_from_active_rasters_projection` | `compute_color_and_texture_from_active_rasters_projection` | `transfer_color_from_visible_rasters_to_texture` |
| Set Vertex Color | `set_color_per_vertex` | `vertex_color_filling` | `set_vertex_color` |
| Threshold Vertex Color | `apply_color_thresholding_per_vertex` | `vertex_color_thresholding` | `threshold_vertex_color` |
| Adjust Vertex Color Brightness/Contrast/Gamma | `apply_color_brightness_contrast_gamma_per_vertex` | `vertex_color_brightness_contrast_gamma` | `adjust_vertex_color_brightness_contrast_gamma` |
| Invert Vertex Color | `apply_color_inverse_per_vertex` | `vertex_color_invert` | `invert_vertex_color` |
| Adjust Vertex Color Levels | `apply_color_level_adjustment_per_vertex` | `vertex_color_levels_adjustment` | `adjust_vertex_color_levels` |
| Tint Vertex Color | `apply_color_intensity_colourisation_per_vertex` | `vertex_color_colourisation` | `tint_vertex_color` |
| Desaturate Vertex Color | `apply_color_desaturation_per_vertex` | `vertex_color_desaturation` | `desaturate_vertex_color` |
| Equalize Vertex Color | `apply_color_equalization_per_vertex` | `equalize_vertex_color` | `equalize_vertex_color` |
| Adjust Vertex Color White Balance | `apply_color_white_balance_per_vertex` | `vertex_color_white_balance` | `adjust_vertex_color_white_balance` |
| Colorize Vertices by Perlin Noise | `compute_color_perlin_noise_per_vertex` | `perlin_color` | `colorize_vertices_by_perlin_noise` |
| Add Noise to Vertex Color | `apply_color_noising_per_vertex` | `color_noise` | `add_noise_to_vertex_color` |
| Set Random Layer Color | `compute_color_scattering_per_mesh` | `color_scattering` | `set_random_layer_color` |
| Set Mesh Color | `compute_set_per_mesh_color` | `set_per_mesh_color` | `set_mesh_color` |
| Clamp Vertex Scalar | `apply_scalar_clamping_per_vertex` | `clamp_vertex_quality` | `clamp_vertex_scalar` |
| Clamp Vertex Scalar Gradient | `apply_scalar_saturation_per_vertex` | `saturate_vertex_quality` | `clamp_vertex_scalar_gradient` |
| Colorize Vertices by Scalar | `compute_color_from_scalar_per_vertex` | `colorize_by_vertex_quality` | `colorize_vertices_by_scalar` |
| Colorize Faces by Scalar | `compute_color_from_scalar_per_face` | `colorize_by_face_quality` | `colorize_faces_by_scalar` |
| Compute Curvature (Discrete) | `compute_scalar_by_discrete_curvature_per_vertex` | `discrete_curvatures` | `compute_curvature_discrete` |
| Compute Face Scalar from Geometry | `compute_scalar_by_geometric_measure_per_face` | `per_face_quality_by_geometric_measure` | `compute_face_scalar_from_geometry` |
| Compute UV Distortion | `compute_scalar_by_texture_distortion_per_face` | `per_face_quality_by_texture_distortion` | `compute_uv_distortion` |
| Smooth Vertex Color | `apply_color_laplacian_smoothing_per_vertex` | `smooth_laplacian_vertex_color` | `smooth_vertex_color` |
| Smooth Face Color | `apply_color_laplacian_smoothing_per_face` | `smooth_laplacian_face_color` | `smooth_face_color` |
| Transfer Color from Vertex to Face | `compute_color_transfer_vertex_to_face` | `transfer_color_vertex_to_face` | `transfer_color_from_vertex_to_face` |
| Transfer Color from Mesh to Face | `compute_color_transfer_mesh_to_face` | `transfer_color_mesh_to_face` | `transfer_color_from_mesh_to_face` |
| Transfer Color from Face to Vertex | `compute_color_transfer_face_to_vertex` | `transfer_color_face_to_vertex` | `transfer_color_from_face_to_vertex` |
| Transfer Color from Texture to Vertex | `compute_color_from_texture_per_vertex` | `transfer_color_texture_to_vertex` | `transfer_color_from_texture_to_vertex` |
| Set Random Face Color | `compute_color_random_per_face` | `random_face_color` | `set_random_face_color` |
| Set Random Component Color | `compute_color_by_conntected_component_per_face` | `random_component_color` | `set_random_component_color` |
| Transfer Scalar from Vertex to Face | `compute_scalar_transfer_vertex_to_face` | `transfer_quality_vertex_to_face` | `transfer_scalar_from_vertex_to_face` |
| Transfer Scalar from Face to Vertex | `compute_scalar_transfer_face_to_vertex` | `transfer_quality_face_to_vertex` | `transfer_scalar_from_face_to_vertex` |
| Create Tetrahedron | `create_tetrahedron` | `create_tetrahedron` | `create_tetrahedron` |
| Create Icosahedron | `create_icosahedron` | `create_icosahedron` | `create_icosahedron` |
| Create Dodecahedron | `create_dodecahedron` | `create_dodecahedron` | `create_dodecahedron` |
| Create Symmetric Dodecahedron | `create_dodecahedron_sym` | `create_dodecahedron_symmetric` | `create_symmetric_dodecahedron` |
| Create Octahedron | `create_octahedron` | `create_octahedron` | `create_octahedron` |
| Create Box | `create_box` | `create_box` | `create_box` |
| Create Annulus | `create_annulus` | `create_annulus` | `create_annulus` |
| Create Sphere | `create_sphere` | `create_sphere` | `create_sphere` |
| Create Sphere Cap | `create_sphere_cap` | `create_sphere_cap` | `create_sphere_cap` |
| Create Points on a Sphere | `create_sphere_points` | `create_points_on_sphere` | `create_points_on_sphere` |
| Create Points on a Spherical Cap | `create_points_on_a_spherical_cap` | `create_points_on_a_spherical_cap` | `create_points_on_spherical_cap` |
| Create Cone | `create_cone` | `create_cone` | `create_cone` |
| Create Torus | `create_torus` | `create_torus` | `create_torus` |
| Create Plane from Selection | `fit_plane_to_selection` | `fit_plane_to_selection` | `create_plane_from_selection` |
| Create Convex Hull | `create_convex_hull` | `create_convex_hull` | `create_convex_hull` |
| Compute Obscurance | `compute_obscurance` | `compute_obscurance` | `compute_obscurance` |
| Compute Face Ambient Occlusion | `compute_face_ambient_occlusion` | `compute_face_ambient_occlusion` | `compute_face_ambient_occlusion` |
| Compute Point Cloud Ambient Occlusion | `compute_point_cloud_ambient_occlusion` | `compute_point_cloud_ambient_occlusion` | `compute_point_cloud_ambient_occlusion` |
| Compute Shape Diameter Function | `compute_shape_diameter_function` | `compute_shape_diameter_function` | `compute_shape_diameter_function` |
| Select Visible Faces | `select_visible_faces` | `select_visible_faces` | `select_visible_faces` |
| Orient Face Normals by Ray Casting | `analyze_normals` | `reorient_face_normals` | `orient_face_normals_by_ray_casting` |
| Select Vertices by Expression | `conditional_vertex_selection` | `select_vertices_by_condition` | `select_vertices_by_expression` |
| Select Faces by Expression | `conditional_face_selection` | `select_faces_by_condition` | `select_faces_by_expression` |
| Compute Vertex Coordinates by Expression | `per_vertex_geometric_function` | `apply_vertex_geometric_function` | `compute_vertex_coordinates_by_expression` |
| Compute Vertex Normals by Expression | `per_vertex_normal_function` | `apply_vertex_normal_function` | `compute_vertex_normals_by_expression` |
| Compute Face Normals by Expression | `per_face_normal_function` | `apply_face_normal_function` | `compute_face_normals_by_expression` |
| Compute Vertex Color by Expression | `per_vertex_color_function` | `apply_vertex_color_function` | `compute_vertex_color_by_expression` |
| Compute Face Color by Expression | `per_face_color_function` | `apply_face_color_function` | `compute_face_color_by_expression` |
| Compute Vertex Scalar by Expression | `per_vertex_quality_function` | `apply_vertex_quality_function` | `compute_vertex_scalar_by_expression` |
| Compute Face Scalar by Expression | `per_face_quality_function` | `apply_face_quality_function` | `compute_face_scalar_by_expression` |
| Parametrize per Vertex by Expression | `per_vertex_texture_function` | `apply_vertex_texture_function` | `parametrize_per_vertex_by_expression` |
| Parametrize per Wedge by Expression | `per_wedge_texture_function` | `apply_wedge_texture_function` | `parametrize_per_wedge_by_expression` |
| Define Custom Vertex Scalar Attribute | `define_per_vertex_scalar_attribute` | `define_vertex_scalar_attribute` | `define_custom_vertex_scalar_attribute` |
| Define Custom Face Scalar Attribute | `define_per_face_scalar_attribute` | `define_face_scalar_attribute` | `define_custom_face_scalar_attribute` |
| Define Custom Vertex Point Attribute | `define_per_vertex_point_attribute` | `define_vertex_point_attribute` | `define_custom_vertex_point_attribute` |
| Define Custom Face Point Attribute | `define_per_face_point_attribute` | `define_face_point_attribute` | `define_custom_face_point_attribute` |
| Create Grid | `grid_generator` | `create_grid` | `create_grid` |
| Create Isosurface from Expression | `implicit_surface` | `create_implicit_surface` | `create_isosurface_from_expression` |
| Refine by User Expression | `refine_user_defined` | `refine_user_defined` | `refine_by_user_expression` |
| Compute Geodesic Distance from Border | `compute_scalar_by_border_distance_per_vertex` | `compute_border_distance_quality` | `compute_geodesic_distance_from_border` |
| Compute Geodesic Distance from Point | `compute_scalar_by_geodesic_distance_from_given_point_per_vertex` | `compute_geodesic_distance_from_point` | `compute_geodesic_distance_from_point` |
| Compute Geodesic Distance from Selection (vcglib) | `compute_scalar_by_geodesic_distance_from_selection_per_vertex` | `compute_geodesic_distance_from_selection` | `compute_geodesic_distance_from_selection_vcglib` |
| Compute Heat Geodesic Distance from Selection (vcglib) | `compute_scalar_by_heat_geodesic_distance_from_selection_per_vertex` | `compute_heat_geodesic_distance` | `compute_heat_geodesic_distance_from_selection_vcglib` |
| Align by ICP (vcglib) | `compute_matrix_by_icp_between_meshes` | `compute_matrix_by_icp_between_meshes` | `align_by_icp_vcglib` |
| Align Meshes Globally | `compute_matrix_by_mesh_global_alignment` | `compute_matrix_by_mesh_global_alignment` | `align_meshes_globally` |
| Measure Layer Overlap | `get_overlapping_meshes_graph` | `get_overlapping_meshes_graph` | `measure_layer_overlap` |
| Mesh Intersection (libigl) | `generate_boolean_intersection` | `generate_boolean_intersection` | `mesh_intersection_libigl` |
| Mesh Union (libigl) | `generate_boolean_union` | `generate_boolean_union` | `mesh_union_libigl` |
| Mesh Difference (libigl) | `generate_boolean_difference` | `generate_boolean_difference` | `mesh_difference_libigl` |
| Mesh Symmetric Difference (libigl) | `generate_boolean_xor` | `generate_boolean_xor` | `mesh_symmetric_difference_libigl` |
| Parametrize by Harmonic Map (libigl) | `compute_texcoord_parametrization_harmonic` | `compute_texcoord_parametrization_harmonic` | `parametrize_by_harmonic_map_libigl` |
| Parametrize by Least Squares Conformal Maps (libigl) | `compute_texcoord_parametrization_least_squares_conformal_maps` | `compute_texcoord_parametrization_least_squares_conformal_maps` | `parametrize_by_least_squares_conformal_maps_libigl` |
| Compute Gaussian Curvature (libigl) | `compute_gaussian_curvature_per_vertex_libigl` | `compute_gaussian_curvature_per_vertex_libigl` | `compute_gaussian_curvature_libigl` |
| Compute Principal Curvature Directions (libigl) | `compute_curvature_principal_directions_per_vertex_libigl` | `compute_curvature_principal_directions_per_vertex_libigl` | `compute_principal_curvature_directions_libigl` |
| Compute Exact Geodesic Distance from Selection (libigl) | `compute_exact_geodesic_distance_from_selection_per_vertex_libigl` | `compute_exact_geodesic_distance_from_selection_per_vertex_libigl` | `compute_exact_geodesic_distance_from_selection_libigl` |
| Compute Heat Geodesic Distance from Selection (libigl) | `compute_heat_geodesic_distance_from_selection_per_vertex_libigl` | `compute_heat_geodesic_distance_from_selection_per_vertex_libigl` | `compute_heat_geodesic_distance_from_selection_libigl` |
| Parametrize by As-Rigid-As-Possible (libigl) | `compute_texcoord_parametrization_as_rigid_as_possible_libigl` | `compute_texcoord_parametrization_as_rigid_as_possible_libigl` | `parametrize_by_as_rigid_as_possible_libigl` |
| Parametrize by SLIM (libigl) | `compute_texcoord_parametrization_slim_libigl` | `compute_texcoord_parametrization_slim_libigl` | `parametrize_by_slim_libigl` |
| Compute Generalized Winding Number (libigl) | `compute_generalized_winding_number_per_vertex_libigl` | `compute_generalized_winding_number_per_vertex_libigl` | `compute_generalized_winding_number_libigl` |
| Smooth Vertex Scalar by Hessian Energy (libigl) | `apply_scalar_hessian_smoothing_per_vertex_libigl` | `apply_scalar_hessian_smoothing_per_vertex_libigl` | `smooth_vertex_scalar_by_hessian_energy_libigl` |
| Parametrize from Registered Rasters | `compute_texcoord_parametrization_from_registered_rasters` | `compute_texcoord_parametrization_from_registered_rasters` | `parametrize_from_registered_rasters` |
| Parametrize from Registered Rasters with Texture | `compute_texcoord_parametrization_and_texture_from_registered_rasters` | `compute_texcoord_parametrization_and_texture_from_registered_rasters` | `parametrize_from_registered_rasters_with_texture` |
| Compute Vertex Scalar from Raster Coverage | `compute_scalar_from_raster_coverage_per_vertex` | `compute_scalar_from_raster_coverage_per_vertex` | `compute_vertex_scalar_from_raster_coverage` |
| Compute Face Scalar from Raster Coverage | `compute_scalar_from_raster_coverage_per_face` | `compute_scalar_from_raster_coverage_per_face` | `compute_face_scalar_from_raster_coverage` |
| Remesh to Quads (Instant Meshes) | `remesh_to_quads_instant_meshes` | `remesh_to_quads_instant_meshes` | `remesh_to_quads_instant_meshes` |
| Extract Selected Faces | `generate_from_selected_faces` | `move_faces_to_layer` | `extract_selected_faces` |
| Extract Selected Vertices | `generate_from_selected_vertices` | `move_vertices_to_layer` | `extract_selected_vertices` |
| Split into Connected Components | `generate_splitting_by_connected_components` | `split_in_connected_components` | `split_into_connected_components` |
| Duplicate Current Layer | `generate_copy_of_current_mesh` | `duplicate_layer` | `duplicate_current_layer` |
| Remove Current Mesh Layer | `delete_current_mesh` | `delete_current_mesh` | `remove_current_mesh_layer` |
| Remove Hidden Mesh Layers | `delete_non_visible_meshes` | `delete_hidden_meshes` | `remove_hidden_mesh_layers` |
| Remove Current Raster | `delete_current_raster` | `delete_current_raster` | `remove_current_raster` |
| Remove Hidden Rasters | `delete_non_active_rasters` | `delete_non_active_rasters` | `remove_hidden_rasters` |
| Merge Visible Layers | `generate_by_merging_visible_meshes` | `flatten_visible_layers` | `merge_visible_layers` |
| Rename Current Mesh Layer | `set_mesh_name` | `rename_mesh` | `rename_current_mesh_layer` |
| Rename Current Raster | `set_raster_name` | `set_raster_name` | `rename_current_raster` |
| Export Cameras from Visible Rasters | `save_active_raster_cameras` | `save_active_raster_cameras` | `export_cameras_from_visible_rasters` |
| Import Cameras to Visible Rasters | `load_active_raster_cameras` | `load_active_raster_cameras` | `import_cameras_to_visible_rasters` |
| Render from Render-State JSON | `render_from_render_state_json` | `render_from_render_state_json` | `render_from_render_state_json` |
| Measure Topological Properties | `compute_topological_measures` | `compute_topological_measures` | `measure_topological_properties` |
| Measure Topological Properties for Quad Mesh | `compute_topological_measures_quad_meshes` | `compute_topological_measures_quad` | `measure_topological_properties_for_quad_mesh` |
| Measure Geometric Properties | `compute_geometric_measures` | `compute_geometric_measures` | `measure_geometric_properties` |
| Measure Selection Area and Perimeter | `compute_area_perimeter_selection` | `compute_selection_area_perimeter` | `measure_selection_area_and_perimeter` |
| Measure Vertex Scalar Statistics | `per_vertex_quality_stat` | `compute_vertex_quality_stat` | `measure_vertex_scalar_statistics` |
| Measure Face Scalar Statistics | `per_face_quality_stat` | `compute_face_quality_stat` | `measure_face_scalar_statistics` |
| Measure Vertex Scalar Histogram | `per_vertex_quality_histogram` | `compute_vertex_quality_histogram` | `measure_vertex_scalar_histogram` |
| Measure Face Scalar Histogram | `per_face_quality_histogram` | `compute_face_quality_histogram` | `measure_face_scalar_histogram` |
| Repair Watertight Mesh (MeshFix) | `repair_watertight_mesh` | `repair_watertight_mesh_meshfix` | `repair_watertight_mesh_meshfix` |
| Subdivide by Loop | `meshing_surface_subdivision_loop` | `subdivision_loop` | `subdivide_by_loop` |
| Subdivide by Butterfly | `meshing_surface_subdivision_butterfly` | `subdivision_butterfly` | `subdivide_by_butterfly` |
| Subdivide by Midpoint | `meshing_surface_subdivision_midpoint` | `subdivision_midpoint` | `subdivide_by_midpoint` |
| Subdivide by LS3 Loop | `meshing_surface_subdivision_ls3_loop` | `subdivision_ls3_loop` | `subdivide_by_ls3_loop` |
| Simplify by Vertex Clustering | `meshing_decimation_clustering` | `simplification_clustering` | `simplify_by_vertex_clustering` |
| Simplify by Quadric Edge Collapse (vcglib) | `meshing_decimation_quadric_edge_collapse` | `simplification_quadric_edge_collapse` | `simplify_by_quadric_edge_collapse_vcglib` |
| Simplify by Quadric Edge Collapse with Texture (vcglib) | `meshing_decimation_quadric_edge_collapse_with_texture` | `simplification_quadric_edge_collapse_with_texture` | `simplify_by_quadric_edge_collapse_with_texture_vcglib` |
| Remesh Isotropically (vcglib) | `meshing_isotropic_explicit_remeshing` | `remeshing_isotropic` | `remesh_isotropically_vcglib` |
| Orient Faces Consistently (vcglib) | `meshing_re_orient_faces_coherently` | `reorient_all_faces` | `orient_faces_consistently_vcglib` |
| Invert Face Orientation | `meshing_invert_face_orientation` | `invert_faces_orientation` | `invert_face_orientation` |
| Mirror or Swap Axes | `apply_matrix_flip_or_swap_axis` | `transform_flip_axis` | `mirror_or_swap_axes` |
| Rotate | `compute_matrix_from_rotation` | `transform_rotate` | `rotate` |
| Rotate to Fitted Plane | `compute_matrix_by_fitting_to_plane` | `transform_rotate_to_fit_plane` | `rotate_to_fitted_plane` |
| Normalize Reference Frame | `compute_matrix_by_reference_frame_normalization` | `normalize_reference_frame` | `normalize_reference_frame` |
| Scale | `compute_matrix_from_scaling_or_normalization` | `transform_scale` | `scale` |
| Translate | `compute_matrix_from_translation` | `transform_translate` | `translate` |
| Set Matrix to Identity | `set_matrix_identity` | `matrix_reset` | `set_matrix_to_identity` |
| Freeze Matrix | `apply_matrix_freeze` | `matrix_freeze` | `freeze_matrix` |
| Invert Matrix | `apply_matrix_inverse` | `matrix_invert` | `invert_matrix` |
| Set Matrix from Translation/Rotation/Scale | `compute_matrix_from_translation_rotation_scale` | `matrix_set_from_trs` | `set_matrix_from_translation_rotation_scale` |
| Set Matrix from Values or Layer | `set_matrix` | `matrix_set_copy` | `set_matrix_from_values_or_layer` |
| Compute Point Cloud Normals | `compute_normal_for_point_clouds` | `compute_point_cloud_normals` | `compute_point_cloud_normals` |
| Smooth Point Cloud Normals | `apply_normal_point_cloud_smoothing` | `smooth_point_cloud_normals` | `smooth_point_cloud_normals` |
| Compute Principal Curvature Directions (vcglib) | `compute_curvature_principal_directions_per_vertex` | `compute_curvature_principal_directions` | `compute_principal_curvature_directions_vcglib` |
| Close Holes | `meshing_close_holes` | `close_holes` | `close_holes` |
| Parametrize by Cylindrical Projection | `generate_cylindrical_unwrapping` | `geometric_cylindrical_unwrapping` | `parametrize_by_cylindrical_projection` |
| Subdivide by Catmull-Clark | `meshing_surface_subdivision_catmull_clark` | `subdivision_catmull_clark` | `subdivide_by_catmull_clark` |
| Convert to Quads by 4-8 Subdivision | `meshing_tri_to_quad_by_4_8_subdivision` | `tri_to_quad_4_8_subdivision` | `convert_to_quads_by_4_8_subdivision` |
| Subdivide by Doo-Sabin | `meshing_surface_subdivision_doo_sabin` | `subdivision_doo_sabin` | `subdivide_by_doo_sabin` |
| Convert to Quad-Dominant Mesh | `meshing_tri_to_quad_dominant` | `convert_to_quad_dominant` | `convert_to_quad_dominant_mesh` |
| Convert to Pure Triangles | `meshing_poly_to_tri` | `convert_to_triangular` | `convert_to_pure_triangles` |
| Convert to Quads by Triangle Pairing | `meshing_tri_to_quad_by_smart_triangle_pairing` | `tri_to_quad_by_pairing` | `convert_to_quads_by_triangle_pairing` |
| Select Crease Edges (vcglib) | `compute_selection_crease_per_edge` | `select_crease_edges` | `select_crease_edges_vcglib` |
| Create Polyline from Selected Edges | `generate_polyline_from_selected_edges` | `build_polyline_from_selection` | `create_polyline_from_selected_edges` |
| Split Vertices by Attribute Seam | `meshing_vertex_attribute_seam` | `vertex_attribute_seam` | `split_vertices_by_attribute_seam` |
| Create Polyline from Selection Perimeter | `generate_polyline_from_selection_perimeter` | `create_selection_perimeter` | `create_polyline_from_selection_perimeter` |
| Create Polyline from Planar Section | `generate_polyline_from_planar_section` | `compute_planar_section` | `create_polyline_from_planar_section` |
| Project Vertices onto MLS Surface (APSS) | `compute_mls_projection_apss` | `apply_mls_apss` | `project_vertices_onto_mls_surface_apss` |
| Project Vertices onto MLS Surface (RIMLS) | `compute_mls_projection_rimls` | `apply_mls_rimls` | `project_vertices_onto_mls_surface_rimls` |
| Reconstruct Surface by Marching Cubes (APSS) | `generate_marching_cubes_apss` | `generate_marching_cubes_apss` | `reconstruct_surface_by_marching_cubes_apss` |
| Reconstruct Surface by Marching Cubes (RIMLS) | `generate_marching_cubes_rimls` | `generate_marching_cubes_rimls` | `reconstruct_surface_by_marching_cubes_rimls` |
| Compute Curvature (APSS) | `compute_curvature_and_color_apss_per_vertex` | `compute_apss_curvature` | `compute_curvature_apss` |
| Compute Curvature (RIMLS) | `compute_curvature_and_color_rimls_per_vertex` | `compute_rimls_curvature` | `compute_curvature_rimls` |
| Estimate Radius from Density | `compute_custom_radius_scalar_attribute_per_vertex` | `estimate_radius_from_density` | `estimate_radius_from_density` |
| Select Small Disconnected Components | `compute_selection_by_small_disconnected_components_per_face` | `select_small_disconnected_component` | `select_small_disconnected_components` |
| Reconstruct Surface by Volumetric Merging | `generate_surface_reconstruction_vcg` | `generate_surface_reconstruction_vcg` | `reconstruct_surface_by_volumetric_merging` |
| Simplify Marching-Cubes Mesh by Edge Collapse | `meshing_decimation_edge_collapse_for_marching_cube_meshes` | `meshing_decimation_edge_collapse_for_marching_cube_meshes` | `simplify_marching_cubes_mesh_by_edge_collapse` |
| Simplify by Quadric Edge Collapse (QSlim) | `simplification_quadric_edge_collapse_qslim` | `simplification_quadric_edge_collapse_qslim` | `simplify_by_quadric_edge_collapse_qslim` |
| Remesh to Quads (QuadWild-BiMDF) | `remesh_to_quads_quadwild_bimdf` | `remesh_to_quads_quadwild_bimdf` | `remesh_to_quads_quadwild_bimdf` |
| Sample Mesh Elements | `generate_sampling_element` | `sample_mesh_elements` | `sample_mesh_elements` |
| Sample Surface by Monte Carlo | `generate_sampling_montecarlo` | `sample_montecarlo` | `sample_surface_by_monte_carlo` |
| Sample Surface by Stratified Triangles | `generate_sampling_stratified_triangle` | `sample_stratified_triangles` | `sample_surface_by_stratified_triangles` |
| Sample Vertices by Clustering | `generate_sampling_clustered_vertex` | `sample_clustered_vertices` | `sample_vertices_by_clustering` |
| Sample Surface by Poisson Disk | `generate_sampling_poisson_disk` | `sample_poisson_disk` | `sample_surface_by_poisson_disk` |
| Measure Hausdorff Distance | `get_hausdorff_distance` | `compute_hausdorff_distance` | `measure_hausdorff_distance` |
| Compute Distance from Reference Mesh | `compute_scalar_by_distance_from_another_mesh_per_vertex` | `compute_distance_from_reference` | `compute_distance_from_reference_mesh` |
| Sample Texels | `generate_sampling_texel` | `sample_texels` | `sample_texels` |
| Transfer Vertex Attributes by Closest Point | `transfer_attributes_per_vertex` | `transfer_vertex_attributes` | `transfer_vertex_attributes_by_closest_point` |
| Remesh Uniformly by Volumetric Resampling | `generate_resampled_uniform_mesh` | `resample_uniform` | `remesh_uniformly_by_volumetric_resampling` |
| Colorize Vertices by Voronoi Regions | `compute_color_by_point_cloud_voronoi_projection` | `voronoi_vertex_coloring` | `colorize_vertices_by_voronoi_regions` |
| Colorize Vertices by Disk Distance | `compute_scalar_by_distance_from_point_cloud_per_vertex` | `disk_vertex_coloring` | `colorize_vertices_by_disk_distance` |
| Sample Offset Surface Recursively | `generate_sampling_regular_recursive` | `sample_regular_recursive` | `sample_offset_surface_recursively` |
| Simplify Point Cloud | `generate_simplified_point_cloud` | `simplify_point_cloud` | `simplify_point_cloud` |
| Reconstruct Surface by Screened Poisson | `surface_reconstruction_screened_poisson` | `generate_screened_poisson` | `reconstruct_surface_by_screened_poisson` |
| Reconstruct Surface by Smooth Signed Distance | `surface_reconstruction_ssd` | `generate_ssd_reconstruction` | `reconstruct_surface_by_smooth_signed_distance` |
| Trim Surface by Scalar Isovalue | `surface_reconstruction_surface_trimmer` | `trim_reconstructed_surface` | `trim_surface_by_scalar_isovalue` |
| Select All | `select_all` | `select_all` | `select_all` |
| Select None | `select_none` | `select_none` | `select_none` |
| Select Faces by View Angle | `select_by_view_angle` | `select_faces_by_view_angle` | `select_faces_by_view_angle` |
| Select Ill-Shaped Faces | `select_problematic_faces` | `select_problematic_faces` | `select_ill_shaped_faces` |
| Invert Selection | `select_invert` | `invert_selection` | `invert_selection` |
| Select Connected Faces | `select_connected_faces` | `select_connected_faces` | `select_connected_faces` |
| Select Faces from Vertices | `select_faces_from_vertices` | `select_faces_from_vertices` | `select_faces_from_vertices` |
| Select Vertices from Faces | `select_vertices_from_faces` | `select_vertices_from_faces` | `select_vertices_from_faces` |
| Remove Selected Vertices | `delete_selected_vertices` | `delete_selected_vertices` | `remove_selected_vertices` |
| Remove All Faces | `delete_all_faces` | `delete_all_faces` | `remove_all_faces` |
| Remove Selected Faces | `delete_selected_faces` | `delete_selected_faces` | `remove_selected_faces` |
| Remove Selected Faces and Vertices | `delete_selected_faces_and_vertices` | `delete_selected_faces_and_vertices` | `remove_selected_faces_and_vertices` |
| Erode Selection | `select_erode` | `erode_selection` | `erode_selection` |
| Dilate Selection | `select_dilate` | `dilate_selection` | `dilate_selection` |
| Select Border | `select_border` | `select_border` | `select_border` |
| Select Faces by Scalar | `select_by_face_quality` | `select_by_face_quality` | `select_faces_by_scalar` |
| Select Vertices by Scalar | `select_by_vertex_quality` | `select_by_vertex_quality` | `select_vertices_by_scalar` |
| Select Faces by Color | `select_by_color` | `select_faces_by_color` | `select_faces_by_color` |
| Select Self-Intersecting Faces | `select_self_intersecting_faces` | `select_self_intersecting_faces` | `select_self_intersecting_faces` |
| Select Vertex Texture Seams | `select_vertex_texture_seams` | `select_vertex_texture_seams` | `select_vertex_texture_seams` |
| Select Non-Manifold Edges (vcglib) | `select_non_manifold_edges` | `select_non_manifold_edges` | `select_non_manifold_edges_vcglib` |
| Select Non-Manifold Vertices | `select_non_manifold_vertices` | `select_non_manifold_vertices` | `select_non_manifold_vertices` |
| Select Faces by Edge Length | `select_faces_by_edge_length` | `select_faces_with_edges_longer_than` | `select_faces_by_edge_length` |
| Select Outliers | `select_outliers` | `select_outliers` | `select_outliers` |
| Select by Screen Rectangle | `select_by_rectangle` | `select_by_rectangle` | `select_by_screen_rectangle` |
| Select Visible Vertices | `select_visible_vertices` | `select_visible_vertices` | `select_visible_vertices` |
| Parametrize by Voronoi Atlas (vcglib) | `generate_voronoi_atlas_parametrization` | `parametrize_voronoi_atlas` | `parametrize_by_voronoi_atlas_vcglib` |
| Convert Per-Wedge UV to Per-Vertex UV | `compute_texcoord_transfer_wedge_to_vertex` | `convert_wedge_uv_to_vertex_uv` | `convert_per_wedge_uv_to_per_vertex_uv` |
| Convert Per-Vertex UV to Per-Wedge UV | `compute_texcoord_transfer_vertex_to_wedge` | `convert_vertex_uv_to_wedge_uv` | `convert_per_vertex_uv_to_per_wedge_uv` |
| Parametrize by Flat Plane | `compute_texcoord_parametrization_flat_plane_per_wedge` | `parametrize_flat_plane` | `parametrize_by_flat_plane` |
| Parametrize by Trivial Per-Triangle Layout | `compute_texcoord_parametrization_triangle_trivial_per_wedge` | `parametrize_trivial_per_triangle` | `parametrize_by_trivial_per_triangle_layout` |
| Set Texture | `set_texture_per_mesh` | `set_texture` | `set_texture` |
| Transfer Color from Vertex to Texture | `compute_texmap_from_color` | `transfer_vertex_color_to_texture` | `transfer_color_from_vertex_to_texture` |
| Transfer Color from Texture to Vertex by Closest Point | `transfer_texture_to_color_per_vertex` | `transfer_texture_to_vertex_color` | `transfer_color_from_texture_to_vertex_by_closest_point` |
| Transfer Vertex Attributes to Texture by Closest Point | `transfer_attributes_to_texture_per_vertex` | `transfer_vertex_attributes_to_texture` | `transfer_vertex_attributes_to_texture_by_closest_point` |
| Convert Object-Space Normal Map to Tangent Space | `convert_object_space_normal_map_to_tangent_space` | `convert_normal_map_to_tangent_space` | `convert_object_space_normal_map_to_tangent_space` |
| Pack Texture Images | `pack_textures` | `pack_texture_per_mesh` | `pack_texture_images` |
| Defragment Texture Atlas | `apply_texmap_defragmentation` | `defragment_texture_map` | `defragment_texture_atlas` |
| Merge Texture Islands | `apply_small_islands_remover` | `small_islands_remover` | `merge_texture_islands` |
| Pack UV Charts | `pack_uv_charts` | `pack_uv_charts` | `pack_uv_charts` |
| Flip Edges by Planarity | `meshing_edge_flip_by_planar_optimization` | `meshing_edge_flip_by_planar_optimization` | `flip_edges_by_planarity` |
| Flip Edges by Curvature | `meshing_edge_flip_by_curvature_optimization` | `meshing_edge_flip_by_curvature_optimization` | `flip_edges_by_curvature` |
| Smooth Vertices by Surface-Preserving Laplacian (vcglib) | `apply_coord_laplacian_smoothing_surface_preserving` | `apply_coord_laplacian_smoothing_surface_preserving` | `smooth_vertices_by_surface_preserving_laplacian_vcglib` |
| Align by Bounding Box (TrueForm) | `compute_matrix_by_obb_alignment` | `compute_matrix_by_obb_alignment` | `align_by_bounding_box_trueform` |
| Align by ICP (TrueForm) | `compute_matrix_by_icp_trueform` | `compute_matrix_by_icp_trueform` | `align_by_icp_trueform` |
| Align to Corresponding Points (TrueForm) | `compute_matrix_by_corresponding_points` | `compute_matrix_by_corresponding_points` | `align_to_corresponding_points_trueform` |
| Mesh Union (TrueForm) | `generate_boolean_union_trueform` | `generate_boolean_union_trueform` | `mesh_union_trueform` |
| Mesh Intersection (TrueForm) | `generate_boolean_intersection_trueform` | `generate_boolean_intersection_trueform` | `mesh_intersection_trueform` |
| Mesh Difference (TrueForm) | `generate_boolean_difference_trueform` | `generate_boolean_difference_trueform` | `mesh_difference_trueform` |
| Mesh Symmetric Difference (TrueForm) | `generate_boolean_xor_trueform` | `generate_boolean_xor_trueform` | `mesh_symmetric_difference_trueform` |
| Mesh CSG Expression (TrueForm) | `generate_csg_expression` | `generate_csg_expression` | `mesh_csg_expression_trueform` |
| Extract Outer Shell (TrueForm) | `generate_outer_shell` | `generate_outer_shell` | `extract_outer_shell_trueform` |
| Create Polyline from Self-Intersections (TrueForm) | `generate_polyline_from_self_intersections` | `generate_polyline_from_self_intersections` | `create_polyline_from_self_intersections_trueform` |
| Create Polyline from Mesh Intersection (TrueForm) | `generate_polyline_from_mesh_intersection` | `generate_polyline_from_mesh_intersection` | `create_polyline_from_mesh_intersection_trueform` |
| Create Polyline from Scalar Isocontour (TrueForm) | `generate_polyline_from_scalar_isocontour` | `generate_polyline_from_scalar_isocontour` | `create_polyline_from_scalar_isocontour_trueform` |
| Create Tube from Polyline (TrueForm) | `generate_tube_from_polyline` | `generate_tube_from_polyline` | `create_tube_from_polyline_trueform` |
| Compute Signed Distance to Mesh (TrueForm) | `compute_scalar_by_signed_distance_per_vertex` | `compute_scalar_by_signed_distance_per_vertex` | `compute_signed_distance_to_mesh_trueform` |
| Select Vertices Inside Mesh (TrueForm) | `select_vertices_inside_mesh` | `select_vertices_inside_mesh` | `select_vertices_inside_mesh_trueform` |
| Measure Chamfer Distance (TrueForm) | `compute_chamfer_distance` | `compute_chamfer_distance` | `measure_chamfer_distance_trueform` |
| Smooth Vertices by Laplacian (TrueForm) | `apply_laplacian_smoothing_trueform` | `apply_laplacian_smoothing_trueform` | `smooth_vertices_by_laplacian_trueform` |
| Smooth Vertices by Taubin (TrueForm) | `apply_taubin_smoothing_trueform` | `apply_taubin_smoothing_trueform` | `smooth_vertices_by_taubin_trueform` |
| Compute Curvature (TrueForm) | `compute_scalar_by_curvature_trueform` | `compute_scalar_by_curvature_trueform` | `compute_curvature_trueform` |
| Compute Normals (TrueForm) | `compute_normals_trueform` | `compute_normals_trueform` | `compute_normals_trueform` |
| Remesh Isotropically (TrueForm) | `remeshing_isotropic_trueform` | `remeshing_isotropic_trueform` | `remesh_isotropically_trueform` |
| Simplify by Error Bound (TrueForm) | `simplification_by_error_trueform` | `simplification_by_error_trueform` | `simplify_by_error_bound_trueform` |
| Simplify by Decimation (TrueForm) | `simplification_by_decimation_trueform` | `simplification_by_decimation_trueform` | `simplify_by_decimation_trueform` |
| Orient Faces Consistently (TrueForm) | `orient_faces_coherently_trueform` | `orient_faces_coherently_trueform` | `orient_faces_consistently_trueform` |
| Orient Faces Outward (TrueForm) | `orient_faces_outward_trueform` | `orient_faces_outward_trueform` | `orient_faces_outward_trueform` |
| Select Crease Edges (TrueForm) | `select_crease_edges_trueform` | `select_crease_edges_trueform` | `select_crease_edges_trueform` |
| Select Non-Manifold Edges (TrueForm) | `select_non_manifold_edges_trueform` | `select_non_manifold_edges_trueform` | `select_non_manifold_edges_trueform` |
| Repair Self-Intersections (TrueForm) | `repair_self_intersections` | `repair_self_intersections` | `repair_self_intersections_trueform` |
| Cut Along Scalar Isocontour (TrueForm) | `cut_along_scalar_isocontour` | `cut_along_scalar_isocontour` | `cut_along_scalar_isocontour_trueform` |
| Remove Duplicate Vertices (TrueForm) | `remove_duplicate_vertices_trueform` | `remove_duplicate_vertices_trueform` | `remove_duplicate_vertices_trueform` |
| Remesh by Edge Flipping (TrueForm) | `improve_triangulation_trueform` | `improve_triangulation_trueform` | `remesh_by_edge_flipping_trueform` |
| Cut Along Crease Edges | `meshing_cut_along_crease_edges` | `cut_mesh_along_crease_edges` | `cut_along_crease_edges` |
| Smooth Vertices by Laplacian (vcglib) | `apply_coord_laplacian_smoothing` | `smooth_laplacian` | `smooth_vertices_by_laplacian_vcglib` |
| Smooth Vertices by HC Laplacian | `apply_coord_hc_laplacian_smoothing` | `smooth_hc_laplacian` | `smooth_vertices_by_hc_laplacian` |
| Smooth Vertices by Scale-Dependent Laplacian | `apply_coord_laplacian_smoothing_scale_dependent` | `smooth_scale_dependent_laplacian` | `smooth_vertices_by_scale_dependent_laplacian` |
| Smooth Vertices by Two-Step Normal Fitting | `apply_coord_two_steps_smoothing` | `smooth_two_step` | `smooth_vertices_by_two_step_normal_fitting` |
| Smooth Vertices by Taubin (vcglib) | `apply_coord_taubin_smoothing` | `smooth_taubin` | `smooth_vertices_by_taubin_vcglib` |
| Smooth Vertices along One Direction | `apply_coord_depth_smoothing` | `smooth_depth` | `smooth_vertices_along_one_direction` |
| Project Vertices onto the Line of Sight | `apply_coord_directional_preservation` | `smooth_directional` | `project_vertices_onto_line_of_sight` |
| Smooth Vertex Scalar | `apply_scalar_smoothing_per_vertex` | `smooth_vertex_quality` | `smooth_vertex_scalar` |
| Smooth Face Normals | `apply_normal_smoothing_per_face` | `smooth_face_normals` | `smooth_face_normals` |
| Sharpen Face Normals by Unsharp Mask | `apply_normal_unsharp_mask_per_vertex` | `unsharp_mask_normals` | `sharpen_face_normals_by_unsharp_mask` |
| Sharpen Vertices by Unsharp Mask | `apply_coord_unsharp_mask` | `unsharp_mask_geometry` | `sharpen_vertices_by_unsharp_mask` |
| Sharpen Vertex Scalar by Unsharp Mask | `apply_scalar_unsharp_mask_per_vertex` | `unsharp_mask_quality` | `sharpen_vertex_scalar_by_unsharp_mask` |
| Sharpen Vertex Color by Unsharp Mask | `apply_color_unsharp_mask_per_vertex` | `unsharp_mask_color` | `sharpen_vertex_color_by_unsharp_mask` |
| Compute Vertex Normals | `compute_normal_per_vertex` | `compute_vertex_normals` | `compute_vertex_normals` |
| Compute Face Normals | `compute_normal_per_face` | `compute_face_normals` | `compute_face_normals` |
| Compute Polygon Face Normals | `compute_normal_polygon_mesh_per_face` | `compute_per_polygon_face_normals` | `compute_polygon_face_normals` |
| Normalize Face Normals | `apply_normal_normalization_per_face` | `normalize_face_normals` | `normalize_face_normals` |
| Normalize Vertex Normals | `apply_normal_normalization_per_vertex` | `normalize_vertex_normals` | `normalize_vertex_normals` |
| Displace Vertices toward Target Mesh | `compute_coord_linear_morphing` | `vertex_linear_morphing` | `displace_vertices_toward_target_mesh` |
| Compute Harmonic Scalar Field | `compute_scalar_by_scalar_harmonic_field_per_vertex` | `generate_scalar_harmonic_field` | `compute_harmonic_scalar_field` |
| Displace Vertices Randomly | `displace_vertices_randomly` | `displace_vertices_randomly` | `displace_vertices_randomly` |
| Displace Vertices by Fractal Brownian Motion | `displace_by_fractal_brownian_motion` | `displace_by_fractal_brownian_motion` | `displace_vertices_by_fractal_brownian_motion` |
| Displace Vertices by Standard Multifractal Noise | `displace_by_standard_multifractal_noise` | `displace_by_standard_multifractal_noise` | `displace_vertices_by_standard_multifractal_noise` |
| Displace Vertices by Heterogeneous Multifractal Noise | `displace_by_heterogeneous_multifractal_noise` | `displace_by_heterogeneous_multifractal_noise` | `displace_vertices_by_heterogeneous_multifractal_noise` |
| Displace Vertices by Hybrid Multifractal Noise | `displace_by_hybrid_multifractal_noise` | `displace_by_hybrid_multifractal_noise` | `displace_vertices_by_hybrid_multifractal_noise` |
| Displace Vertices by Ridged Multifractal Noise | `displace_by_ridged_multifractal_noise` | `displace_by_ridged_multifractal_noise` | `displace_vertices_by_ridged_multifractal_noise` |
| Sample Surface by Voronoi Relaxation | `generate_sampling_voronoi` | `generate_sampling_voronoi` | `sample_surface_by_voronoi_relaxation` |
| Sample Volume | `generate_sampling_volumetric` | `generate_sampling_volumetric` | `sample_volume` |
| Create Voronoi Scaffolding | `generate_voronoi_scaffolding` | `generate_voronoi_scaffolding` | `create_voronoi_scaffolding` |
| Create Solid Wireframe | `generate_solid_wireframe` | `generate_solid_wireframe` | `create_solid_wireframe` |
| Parametrize by Atlas (xatlas) | `generate_xatlas_parametrization` | `parametrize_xatlas` | `parametrize_by_atlas_xatlas` |
