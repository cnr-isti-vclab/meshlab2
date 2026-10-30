# Citations moved to references, applied 2026-10-30

A filter's papers belong in its manifest's top-level `references`, attached with
`referenceIds` (see [adding_a_filter.md](../adding_a_filter.md)): the help citations, the
`[bib]` / `[doi]` / `[web]` buttons and the generated `references.bib` are all built from
them. 31 descriptions carried their citation as text instead, so none of that existed for
them. This records the move.

## How the citations were found

Every `longDescriptionMarkdown` was scanned for a year, "et al.", a venue or journal name, a
DOI, page or volume numbers, or words such as "paper"; the 36 hits were read one by one. Seven
needed nothing: Instant Meshes ("by Jakob et al.") and the texture consolidation filter ("by
the method of Liu, Wang, Kim and Kazhdan (2026)") already had their references and only name
the authors in prose, and five more -- the three Geogram parametrizations, Merge Texture
Islands, Taubin smoothing (TrueForm) -- matched on an ordinary word. A second pass for
"Reference:" headings found the two bounding-box filters, whose citation has no year.

## Evidence

Every entry was taken from its Crossref record, matched on title and authors, except two
with none: Kazhdan 2006 (its Eurographics DOI resolves to the Eurographics Digital Library)
and Takayama 2014 (JCGT issues no DOIs; the entry carries the journal's URL). Crossref's
lowercased DOIs were given their usual capitals, its "Thürrner" corrected to Thürmer, and two
titles it truncates or marks up restored (LoOP, Chang et al.).

Three papers were already declared in another manifest -- `bernardini1999` in
`filter_open3d`, `meyer2003discrete` and `crane2013heat` in `filter_igl` -- and were copied
verbatim: `docs/generate_api.py` refuses a reference id defined two ways.

## The mapping

| filter | cited in the text | reference now | evidence | text |
|---|---|---|---|---|
| bpa / `reconstruct_surface_by_ball_pivoting_gruber` | Bernardini et al., TVCG 1999 | `bernardini1999` | 10.1109/2945.817351 | "Reference:" block dropped; "the 1999 paper" kept |
| clean / `reconstruct_surface_by_ball_pivoting_vcglib` | same | `bernardini1999` | same | citation lines dropped |
| cgal / `reconstruct_surface_by_alpha_wrapping` | Portaneri et al., 2022 | `portaneri2022alpha` | 10.1145/3528223.3530152 | block dropped |
| cgal / `reconstruct_surface_by_alpha_shape` | Edelsbrunner, Mücke, TOG 1994 | `edelsbrunner1994alpha` | 10.1145/174462.156635 | block dropped |
| cgal / `reconstruct_surface_by_voronoi_filtering` | Amenta, Bern, DCG 1999 | `amenta1999voronoi` | 10.1007/PL00009475 | block dropped |
| cgal / `reconstruct_surface_by_scale_space` | van Lankveld et al., CGF 2013 | `digne2011scale` (changed) | see below | block dropped |
| cgal / `reconstruct_surface_by_advancing_front` | Cohen-Steiner, Da, TVC 2004 | `cohensteiner2004greedy` | 10.1007/s00371-003-0217-z | block dropped |
| cgal / `orient_point_cloud_normals` | Hoppe et al., SIGGRAPH 1992 | `hoppe1992surface` | 10.1145/133994.134011 | block dropped; "(Hoppe et al.)" kept |
| cgal / `reconstruct_surface_by_poisson_cgal` | Kazhdan et al., SGP 2006 | `kazhdan2006poisson` | 10.2312/SGP/SGP06/061-070 | block dropped |
| cgal / `reconstruct_surface_by_kinetic_partition` | "Oesau, Lafarge", TOG 2020 | `bauchet2020kinetic` (corrected) | see below | block dropped |
| cgal / `create_mesh_bounding_box`, `create_scene_bounding_box` | CGAL manual chapter | `chang2011obb` (changed) | see below | block dropped |
| create / `create_convex_hull` | Barber et al., TOMS 1996 | `barber1996quickhull` | 10.1145/235815.235821 | block dropped |
| embree / `compute_obscurance` | Iones et al., CG&A 2003 | `iones2003obscurance` | 10.1109/MCG.2003.1198263 | "For further details..." dropped |
| embree / `compute_shape_diameter_function` | Shapira et al., TVC 2008 | `shapira2008sdf` | 10.1007/s00371-007-0197-5 | same |
| embree / `orient_face_normals_by_ray_casting` | Takayama et al., JCGT 2014 | `takayama2014orientation` | jcgt.org/published/0003/04/02 | same |
| geodesic / `compute_heat_geodesic_distance_from_selection_vcglib` | "(Crane et al.)" | `crane2013heat` | 10.1145/2516971.2516977 | kept |
| isoparam / `parametrize_by_abstract_domain`, `create_atlased_mesh_from_abstract_domain` | the attached `pietroni2010isoparam`, repeated | unchanged | -- | repetition dropped; which partitions are the paper's kept |
| meshing / `subdivide_by_ls3_loop` | Boyé et al., CGF 2010; Barthe, Kobbelt, CAGD 2004 | `boye2010ls3`, `barthe2004tuning` | 10.1111/j.1467-8659.2010.01788.x; 10.1016/j.cagd.2004.04.003 | "See:" dropped; "the alternative weighting schemes are Barthe and Kobbelt's" |
| meshing / `simplify_by_vertex_clustering` | Rossignac, Borrel, 1993 | `rossignac1993clustering` | 10.1007/978-3-642-78114-8_29 | "See:" dropped |
| meshing / `remesh_isotropically_vcglib` | Hoppe et al., SIGGRAPH 1993 | `hoppe1993optimization` | 10.1145/166117.166119 | "loosely inspired by the mesh optimization of Hoppe et al." |
| meshing / `convert_to_quads_by_4_8_subdivision` | Velho, Zorin, CAGD 2001 | `velho2001quads` | 10.1016/S0167-8396(01)00039-5 | "See:" dropped |
| sampling / `sample_surface_by_poisson_disk`, `simplify_point_cloud` | Corsini et al., TVCG 2012 | `corsini2012blue` | 10.1109/TVCG.2012.34 | "the algorithm of Corsini, Cignoni and Scopigno" |
| select / `select_outliers` | Kriegel et al., CIKM 2009 | `kriegel2009loop` | 10.1145/1645953.1646195 | "the LoOP measure of Kriegel et al." |
| select / `select_visible_vertices` | Katz et al., TOG 2007 | `katz2007hpr` | 10.1145/1276377.1276407 | block dropped |
| texture / `parametrize_by_voronoi_atlas_vcglib` | Pinkall, Polthier, Exp. Math. 1993 | `pinkall1993minimal` | 10.1080/10586458.1993.10504266 | "the harmonic map ... of Pinkall and Polthier" |
| unsharp / `compute_vertex_normals` | Thürmer, Wüthrich, JGT 1998; Max, JGT 1999 | `thurmer1998normals`, `max1999normals` | 10.1080/10867651.1998.10487487; 10.1080/10867651.1999.10487501 | "[1]" / "[2]" became the authors' names; footnotes dropped |
| unsharp / `compute_harmonic_scalar_field` | Xu et al., C&G 2009 | `xu2009harmonic` | 10.1016/j.cag.2009.03.022 | "For more details see:" dropped |
| colorproc / `compute_curvature_discrete` | Meyer et al., 2003 | `meyer2003discrete` | 10.1007/978-3-662-05105-4_2 | "computed with the discrete operators of Meyer et al." |

## The three that are not plain moves

- **Scale space.** The text cited *Watertight Scenes from Urban LiDAR and Planar Surfaces*
  (CGF 2013), and with its first two authors swapped (Crossref: van Kreveld, van Lankveld,
  Veltkamp). CGAL's user manual credits the method to "Julie Digne *et al.* in 2011"
  (`cgal:dmsl-ssmrp-11`), which is *Scale Space Meshing of Raw Data Point Sets*, CGF 30(6),
  10.1111/j.1467-8659.2011.01848.x.
- **Kinetic reconstruction.** *Kinetic Shape Reconstruction*, TOG 39(5), 10.1145/3376918, is
  by Jean-Philippe Bauchet and Florent Lafarge. Sven Oesau, credited in the text, is an author
  of CGAL's implementation (`CGAL/Kinetic_surface_reconstruction_3.h`), not of the paper.
- **Optimal bounding box.** The text cited the CGAL manual chapter. CGAL's own code says "The
  algorithm is based on a paper by Chang, Gorissen, and Melchior"
  (`CGAL/Optimal_bounding_box/oriented_bounding_box.h`): *Fast Oriented Bounding Box
  Optimization on the Rotation Group SO(3,ℝ)*, TOG 30(5), 10.1145/2019627.2019641.

## Guard

`test_filter_descriptors.cpp` now rejects a DOI link, or a line opening with "Reference:",
in a description: 16 of the 31 carried one. Prose naming the authors is left alone.
