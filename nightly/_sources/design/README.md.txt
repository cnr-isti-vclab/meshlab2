# Design Documents

Three kinds of document live here, and the distinction is worth keeping:

- **Reference** (this directory) — how MeshLab works *today*. Read these to
  understand or change the code; correct them when the code changes.
- **[Proposals](proposals/)** — design settled before the code exists. Every one
  says "not implemented" in its opening paragraph. When one ships, its content
  moves into a reference document and the proposal is deleted, not left to rot.
- **[History](history/)** — dated records of work that is finished. They are
  evidence of how something came to be, not statements about the present.
  **Records are never edited after the fact**; if one turns out to be wrong,
  that is itself a fact about the record. Where a record's conclusions are still
  in force, the rule lives in a reference document and the record only shows the
  reasoning.

## Reference

| Document | What it covers |
| --- | --- |
| [Architecture](architecture.md) | Single-document, multi-view structure; ownership and signals. |
| [Data Model](data_model.md) | Layers, revisions, undo, what a snapshot holds. |
| [Rendering](rendering.md) | The QRhi pipeline, pass planning, render modes. |
| [Memory Accounting](memory_accounting.md) | Why there are several different numbers for "size". |
| [Preferences](preferences.md) | Application settings, sharing the filter-parameter schema. |
| [User Interface](user_interface.md) | How the panels, gestures, interactive tools and the Action History behave. |
| [Third-Party Code](third_party.md) | How libraries and vendored algorithm code enter the build, what a vendored tree records, the About dialog's lists. |
| [Adding a Filter](adding_a_filter.md) | The practical how-to: descriptor schema, plugin class, wiring. |
| [Vocabulary](vocabulary.md) | The controlled vocabulary: categories, verb lexicon, naming grammar, layer naming. |
| [Filter Organization](filter_organization.md) | Settled naming and organization principles, plus parameter conventions. |
| [Raster Alignment](raster_alignment.md) | Aligning a raster's camera to the mesh: by point pairs, by mutual information on a CPU rendering, both at once, and globally over a set; what changes and what does not, the pixel convention, the parametrization, the renderings. |
| [Surface Reuse Experiment](surface_reuse.md) | Python baseline for Voronoi panels, rigid alignment, distance rejection and sampled target coverage; differences from the SGP 2025 method. |
| [Test Data](test_data.md) | Where test inputs live by size, the fetched-dataset manifest, and the skip-when-absent rule. |
| [TrueForm Plugin](trueform_plugin.md) | How the TrueForm-backed plugins relate to the rest of the tree. |
| [Branches and Releases](release_process.md) | The `devel`/`main` model, version tags, the release workflow and what is still open. |

> **[Vocabulary](vocabulary.md) is read by the test suite at runtime.**
> `test_filter_descriptors.cpp` opens it by path and parses
> the `## 3. Verb lexicon` section. Renaming the file or that heading breaks the
> build. Adding or retiring a verb there changes what the tests enforce, which is
> the intent.

## Proposals

| Document | Status |
| --- | --- |
| [LLM Integration](proposals/llm_integration.md) | Option space for driving MeshLab from a model. Not implemented. |
| [Filter Representation](proposals/filter_representation.md) | What a filter works on and produces (`Mesh/Triangle`, `Point Cloud`, ...) as a classification facet beside the category; moved out of Vocabulary. Not implemented. |
| [Usage Statistics](proposals/usage_statistics.md) | Aggregate usage collection in two channels: an update check that counts installs by version and platform without an identifier, and an opt-in session digest the census lets you reweight. Privacy by construction — no free text, no timestamps, a budgeted fingerprint, country counted unlinked. Not implemented. |
| [Local Usage Model](proposals/local_usage_model.md) | The other half: a local journal of the user's own actions, never transmitted, and the statistics built on it — search ranked by use, parameter defaults that learn, runtime estimates, next-filter and repeated-pipeline suggestions; the views that show a user their own record (session recap as a script, per-document provenance, comparison against the shipped prior) and four exports, including an air-gapped contribution bundle. Not implemented. |
| [Gaussian Splatting](proposals/gaussian_splatting.md) | Splat loading, rendering and editing. Not implemented; some decisions taken, the renderer plan revised for Open3D 0.20's MIT splat pipeline, and the core prerequisite (named attributes surviving undo) landed for `float`, `Point3f` and `int`. |
| [Edge Support](proposals/edge_support.md) | Every stage has shipped; open questions only, ready for history. Stages 1-3 shipped 2026-09-16 (in history), stage 4, the polyline colorize, transfer and custom-attribute filters, 2026-11-26, and arc length along a chain, the edge option of Select All/None/Invert and the rubber band's edge mode 2026-11-27. |
| [Coral Branch Phenotyping](proposals/coral_branch_phenotyping.md) | Mapping table for porting Yuri Andraccio's curve-skeleton branch segmentation and measurement plugin (and its three vcglib additions, redone and landed 2026-09-28). Plugin not implemented; awaiting approval; `curve skeleton` approved, five issues open. |
| [CinoLib Port](proposals/cinolib_port.md) | Nine proposed surface filters in three stages: UV mapping, polyharmonic fields, topology and polygon tools; source-verified integration constraints and a deferred-candidate survey. Not implemented. |
| [Open3D RGB-D Reconstruction](proposals/open3d_scan_processing.md) | RGB-D sequence reconstruction with Open3D (odometry, pose refinement, TSDF integration of raster layers with a depth plane), plus the Open3D candidates deliberately not taken (alpha shape, Poisson, persisting the pose graph). Not implemented; its share of the vendored Open3D code is not yet traced. Phases 0-2 shipped as `filter_open3d` and are in history. |
| [Layer Selection Model](proposals/layer_selection_model.md) | Merging *visible* and *current* into one *selected* set with a last-selected member, from the Tarini and Callieri sessions: the 136-reference mapping table, seven open decisions and a seven-step staging. Not implemented; awaiting approval of the table and the names. |
| [Directional and Frame Fields](proposals/directional_port.md) | Ten proposed field synthesis, visualization, analysis, seamless parametrization and polygon remeshing filters, plus QuadWild and Instant Meshes consumers. Not implemented. |
| [DiWR through QRhi Compute](proposals/diwr_qrhi.md) | Feasibility of porting the CUDA DiWR reconstructor (joint orientation, area weight and confidence for unoriented, noisy point clouds, extracted by weighted Screened Poisson) to QRhi compute: a measured source inventory, what CUDA, Thrust and cudaKDTree leave to rewrite, and a first experiment. Assessed 2026-11-15; not implemented, nothing approved. |
| [TexMetro Reference Values](proposals/texmetro.md) | What remains of bringing TexMetro's metrics into `Measurement`: Stage 0, checking our numbers against JSON written by TexMetro itself (it cannot run on a Mac), six questions for its authors and one about publishing the gargoyle atlases. The script and its `--compare` are ready. Not implemented; stages 1 to 5 shipped 2026-10-23 to 26 (see `docs/texture_metrics.md`) and are in history. |
| [USD Support](proposals/usd_support.md) | Reading and writing USD (`.usda`, `.usdc`, `.usdz`) through LightUSD (formerly TinyUSDZ) as a pinned submodule, not OpenUSD, whose vcpkg port is shared-only: a flattened import first, as glTF's, with UsdPreviewSurface mapped to MeshLab's four material channels and `upAxis`/`metersPerUnit` handled; then scene I/O (*Export Visible Layers as USD*, layer matrices kept as prim transforms, and *Import as Separate Layers*); then one material-model extension shared with glTF. Library and flattened import decided 2026-11-23. Stage 0 done 2026-11-27: go, on LightUSD 1.0's new `next` core and Tydra Next (the older core is deprecated). Stage 1 implemented 2026-11-27 (`plugins/io_usd`): flattened import and one-layer export, files pass all 30 OpenUSD validators; departures from the plan (faceVarying normals averaged, Y-up baked, centimeters by default) listed in §4.4. Stages 2 and 3 not implemented. |
| [Load Options](proposals/load_options.md) | Options an importer declares with the filter parameter schema (its third client after filters and preferences), resolved from defaults and remembered choices, recorded in the `load_mesh` action and in Python, kept on the layer so *Reload* repeats them. Opening never asks: the layer loads with the defaults and *Reload with Options...* in its context menu reads the file again with others. STL's merge preference became the first option, `io_vcg`'s. Revives legacy MeshLab's `initPreOpenParameter` without its dialog on every load. Stage 1 implemented (2026-11-28); options in the project are not. |
| [LAS/LAZ Support](proposals/las_support.md) | Reading and writing LAS and LAZ through LASzip (Apache-2.0, no dependencies), and a document origin kept in double precision, subtracted before the float conversion, so georeferenced coordinates survive: measured 2026-11-25, an E57 grid 1 mm apart at UTM coordinates collapses to 4 × 1 distinct positions. Classification and other fields as named attributes, a point budget by default, COPC level of detail later. The document origin is implemented for E57 (2026-11-28); LAS itself is not. |

## History

| Document | Records |
| --- | --- |
| [Filter Classification](history/filter_classification.md) | The 2026-07-29 pass-1 migration of 272 filters. |
| [Filter Names](history/filter_names.md) | The pass-1 renaming rounds, verb by verb, with the rulings made along the way. |
| [Category Migration](history/category_migration.md) | The 32 old `menuPath` values mapped onto the category ontology, and the search change that made dropping category prefixes safe. |
| [Plugin Reorganization](history/plugin_reorganization.md) | The 2026-08 plan to fold the filter plugins into user-facing families, with its mapping table and rulings; not carried out. |
| [Pass 2 Identifier Map](history/pass2_identifier_map.md) | The `id` / `pythonName` mapping applied 2026-09-03. |
| [Repository Rename](history/repository_rename.md) | The QMeshLab → meshlab2 rename, six surfaces in five phases, applied 2026-09-13. |
| [Layer Naming](history/layer_naming.md) | The 87 layer-creating filters, measured before and after, applied 2026-09-19. |
| [Geogram Port](history/geogram_port.md) | The `filter_geogram` plugin: four phases, eight rulings and four measured corrections, completed 2026-09-20; the one deferred item, the OpenSCAD compiler, was dropped 2026-11-26. |
| [OpenSCAD CSG](history/openscad_csg.md) | One filter that would have exposed geogram's OpenSCAD-subset compiler to build a mesh from a script. Split out of the geogram port, then dropped 2026-11-26 as not useful enough; never implemented. |
| [Clipping Plane](history/clipping_plane.md) | Replacing the near-plane cutting trick with a real plane in the shaders: the OpenGL 3.2 floor and the measurements behind it, applied 2026-09-24. |
| [Selection Scope Audit](history/selection_scope_audit.md) | The 50 filters that honour a selection, measured, and their migration to one framework `selectedOnly` with a single contract; eleven defects fixed, applied 2026-09-28. |
| [Transform Result Audit](history/transform_result_audit.md) | The 20 filters that write a layer matrix, and the migration of nine Freeze Matrix toggles and five alignments to one framework *Assign matrix* / *Bake positions* choice; the ignored `compose` and the camera filters' double move fixed, applied 2026-09-29. |
| [Citations to References](history/citations_to_references.md) | The 31 descriptions that typed their papers out, moved to the manifests' `references` with Crossref evidence; three citations corrected, applied 2026-10-30. |
| [Project Persistence](history/project_persistence.md) | How the user's work reaches the disk: a stored state for layers, textures and planes kept outside undo and shown as one mark in the layer panel; texture filters that write no file; Save Texture and Rename Texture; project save by stored state under unique names; the document folder. The 39-row mapping table and eleven decisions, applied 2026-10-31 in five stages. |
| [Two-Layer Filters](history/two_layer_filters.md) | The rule that a filter reading two layers and changing one changes the current layer: the 25 filters measured, the 15 migrated (five had written the other layer), *Transfer Texture from Another Layer* split out, five aligner and transfer renames, and `selectionScopeMesh` removed. Applied 2026-11-01 in three stages. |
| [Face Scalar from Rasters](history/face_scalar_from_rasters.md) | Turning *Compute Face Scalar from Raster Coverage* into a measure of how well a photo set covers each face: count, best resolution in pixels per unit, best angle; the majority-of-three-interior-points visibility rule; the 8-row mapping table, and the measured sensitivity of the absolute depth tolerance. Applied 2026-11-01. |
| [Raster Alignment](history/raster_alignment.md) | Stage 3 of the lens model: old MeshLab's three alignment plugins (10,846 lines, a 9-row table) replaced by about 1,700 lines with their comments, carried out in four stages: point correspondences with `vcg::ShotResection`, the drag-to-pair tool in the raster view, mutual information on a CPU rendering, and Dellepiane and Scopigno's global refinement, ported from the paper where it and the old code disagree. The measurements behind each stage, the false maximum of the normal map on a real scan, and the nine decisions. Applied 2026-11-03. |
| [Texture Consolidation](history/texture_consolidation.md) | Liu, Wang, Kim and Kazhdan's registration-and-stitching texture builder as a second *Transfer Color from Visible Rasters to Texture*, run as three helper programs from a recursive submodule on four forks. The measured upstream, the camera conversion and its test, two out-of-bounds reads and an uninitialized read that had corrupted every texture, the scheduling fixes that made the pipeline repeatable, the stitcher's compile cost cut from 151 s to 57 s, and the move of the Linux build to Ubuntu 24.04. Completed 2026-11-07; what is still open, the upstream pull requests among it, is in `plugins/filter_texture_consolidation/UPSTREAM.md`. |
| [Documentation Reorganization](history/documentation_reorganization.md) | The 139-section mapping table that gave each reference document one audience: UI behaviour to User Interface, third-party policy to its own document, finished migrations here, unbuilt designs to proposals, duplicates merged. Moves and trims applied 2026-11-15. |
| [Folding the pymeshlab2 Wheel](history/pymeshlab_merge.md) | Retiring the separate `pymeshlab2` repository and building the wheel here: the 23-file mapping table, the in-app facade renamed from `pymeshlab` to `pymeshlab2`, vcpkg's `python3` port hijacking `find_package(Python)`, and the wheels joining `release.yml`. Completed 2026-11-22 with the `2026.11` release; the old repository is archived. |
| [Filter Search](history/filter_search.md) | The proposal behind word-start matching, one-way search aliases and five-level ranking, implemented 2026-10-30: the old search measured, the first alias table and the 24 reference queries. How it works now is in User Interface. |
| [Orthographic Camera](history/orthographic_camera.md) | The orthographic projection for the view and every camera consumer, in five stages (2026-10): the visible-height invariant, the 45-row mapping table, and the vcglib `GetMatrix` and `GetFrustum` fixes with their measurements. How it works now is in Rendering. |
| [Camera Lens Model](history/camera_lens_model.md) | Lens distortion behind a hard boundary, implemented 2026-11-02: vcglib's old radial model and old MeshLab audited and removed (a 14-row table), the Brown-Conrady lens on rasters (an 11-row table), and native COLMAP import. How it works now is in Data Model. |
| [Local Flattening](history/local_flattening.md) | Pietroni et al.'s local flattening operator, steps 1 to 5, implemented 2026-11-15 to 17: why its equation 1 needs a conformal energy, the decisions, the `PoissonSolver` clean-up and the measurements on cylinders and scans. The rules are in vcglib's `local_flattening.h` and the filter's description. |
| [Edge Support, Stages 1-3](history/edge_support.md) | The proposal as it stood when stages 1-3 shipped (2026-09-16): per-edge colour and scalar, the edge expression and colorize filters, selected edges in the overlay, stamped isocontour levels. |
| [Open3D Scan Processing, Phases 0-2](history/open3d_scan_processing.md) | The proposal behind `filter_open3d` (2026-10-14 to 10-30): the VTK build route rejected, the 116-file vendored subset and its three cuts, and the measurements behind the registration filters' choices (FGR's orientation limit, sequential chaining, restart, `absperc` lengths). |
| [TexMetro, Stages 1-5](history/texmetro.md) | The case for CPU metrics and a deterministic texel rasterizer in vcglib over vendoring TexMetro or a QRhi raster, the measurements behind it, the 68-key mapping, the quirks of TexMetro's code, and stages 1 to 5 as shipped 2026-10-23 to 26. |
| [Texture Signal Processing Filters](history/texture_signal_processing.md) | The plan behind `filter_texture_signal_processing`'s nine filters (seam repair and dilation, smoothing and sharpening, inpainting, detail transfer, per-face curvature, line-integral convolution, reaction-diffusion), in process on a fork of TSP: the stage-0 memory and crash findings, the upstream pull requests, and *Detail Size* and *Pattern Size* as lengths. Implemented 2026-11-12 to 14; source-aware stitching not pursued. |
