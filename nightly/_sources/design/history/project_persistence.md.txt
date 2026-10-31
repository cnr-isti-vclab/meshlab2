# Project persistence, applied 2026-10-31

How the user's work reaches the disk: whether each layer, texture and raster plane is saved,
how textures made by filters get written, how a project is saved, and the document's folder.
Planned on 2026-10-30 as a proposal (first called "texture storage"), approved on 2026-10-31
with the 39-row mapping table of section 8, and applied the same day in five stages plus a
closing step (section 10). The rules now in force are in [data_model.md](../data_model.md),
[adding_a_filter.md](../adding_a_filter.md) and
[filter_organization.md](../filter_organization.md#texturename); this records how they came to
be. Sections 2 to 7 are the design as approved; where the implementation differed, section 10
says so.

## 1. What was wrong


The texture data is already in memory. `MeshIOTextureAsset` carries the `QImage` beside its
`sourcePath` (`src/plugins/meshioplugin.h:10`), undo snapshots hold it, and memory
accounting counts it. But:

- **Filters that make a texture write it while running**, because old MeshLab kept only file
  names on a mesh, and **they can overwrite the user's files.** The default names
  `texture.png`, `projected_texture.png` and `consolidated_texture.png` are anchored next to
  the current mesh's file (`anchorOutputPath`, `src/plugins/meshfilterpluginmanager.cpp:810`)
  and written with `QImage::save` without looking (`TextureAssociationUtils::saveImages`,
  `src/core/textureassociationutils.cpp:322`). A scan whose texture is called `texture.png`
  loses it to *Transfer Color from Vertex to Texture* run with defaults. *Convert
  Object-Space Normal Map to Tangent Space* with "overwrite existing" writes over the source
  normal map itself (`plugins/filter_texture/texturefilterplugin.cpp:1373`). Undo restores
  the document, not the file, and leaves the files it wrote behind.
- **Other texture filters already work in memory.** Texture defragmentation, xatlas's dummy
  texture and Set Texture's dummy return an image with no file
  (`texturedefragfilterplugin.cpp:834`, `xatlasfilterplugin.cpp:283`,
  `texturefilterplugin.cpp:892`), and so do the glTF and 3MF readers for every texture they
  load, embedded or not (`gltfimportplugin.cpp:1123`, `threemfplugin.cpp:278`: name and
  image, no `sourcePath`). An empty path therefore cannot mean "unsaved".
- **The savers disagree on which copy wins.** PLY export copies a texture's source file
  whenever it exists and writes the in-memory image only when it does not
  (`plugins/io_vcg/vcgimportplugin.cpp:236`); glTF writes the image whenever there is one
  (`plugins/io_gltf/gltfimportplugin.cpp:1752`), so a JPEG it loaded is re-encoded. PLY
  writes textures at all only with `copyAssociatedTextures` (`:379`); without it an image
  with no file is not written and, reading `rebuildLegacyTextureAssociation`
  (`textureassociationutils.cpp:349`), is left out of the `TextureFile` list, so the
  faces' texture indices no longer match it. Not yet reproduced; stage 1 starts with the
  test that would.
- **The layer panel's provenance marks do not say whether anything is saved.** The *Src*
  column shows `D` (has a file) or `G` (no file), plus `*` when `MeshEntry::modified`
  (`src/ui/layerwidget.cpp:2092`, and `[D]`/`[G]` in the tree, `:1810`). But `modified` is
  set by selection changes too and cleared only by a reload (data_model.md, "Changed"):
  `D*` stays after the layer is saved, a selection makes it, and nearly every `G` is `G*`.
  Textures have no mark at all.
- **Save Mesh does not change the layer**, so after saving to a new file the layer still
  names the file it was loaded from.
- **Project save** (`Document::saveMeshLabProject`, `src/core/document_project.cpp:187`):
  writes a layer that has no file to `meshes/<layer name>.ply`, so two layers with the same
  name overwrite each other, and with default options (`:306`), which drops a texture that
  has no file; with *copy files*, copies a mesh without its textures and still records the
  original location (`:310`, recorded in data_model.md); and changes nothing in the
  document, so a layer written once is written as new every time.

## 2. Saved state: one model for layers and textures

Every layer and every texture has two facts:

- **where it is stored**: a file of its own; inside its mesh's file (a texture in a `.glb`
  or `.3mf`); or nowhere.
- **whether it differs** from what is stored there.

These are facts about the disk, not about the document's history, so they are kept **outside
the undo state**: undo after a save must show the layer as changed again, not as saved.

- **A layer** records, at load and at every save, its file and the
  `(geometryRevision, materialRevision, selectionRevision)` it had. It differs when its
  revisions are not those -- so undoing back to the saved state shows it saved again, with
  no extra bookkeeping, because undo restores revisions (to be confirmed for the selection
  delta path in stage 1). **A selection counts** (decided 2026-10-31): PLY keeps it in the
  vertex and face flags (`IOM_VERTFLAGS`, `IOM_FACEFLAGS`; `export_ply.h:808`), and a
  project re-saving changed layers must not skip a layer whose only change is a selection.
  Saving to a format that cannot hold it (STL) still records the layer as saved, as it
  does for anything else the format drops. This replaces `MeshEntry::modified` for this
  purpose.
- **A texture** is known to be stored when its image is the one last read or written:
  `QImage::cacheKey()`, which copies share and any change replaces, recorded with the place
  it was read from or written to. Filters need no flag: a new or changed image has a key
  nobody recorded.

## 3. Showing it

The *Src* column and the tree's prefix become one **state mark**, the same for layers and
for the texture rows under them:

| mark | meaning | tooltip |
|---|---|---|
| (none) | stored, and matches | `Saved in scans/a.ply` / `Inside scans/a.glb` |
| ● | stored, but changed since | `Changes not saved -- scans/a.ply` |
| ○ | not stored anywhere | `Not saved yet` |

Paths are shown relative to the document directory (section 6). `D`, `G` and `*` go.

## 4. Saving a mesh, without a project

The everyday workflow -- open a mesh, run filters, save -- stays as short as it is and never
asks about a project:

- **Save Mesh** writes the mesh and every texture that is not stored or has changed **next to
  the mesh file**, the convention every format and viewer expects; textures that are stored
  and unchanged are referenced or copied as today. Formats that pack textures inside
  (`.glb`, `.3mf`) keep them inside. No option has to be ticked for a new texture to be
  written.
- **Saving names the layer's file**, as in any document application (decided 2026-10-30):
  after Save Mesh to a new path the layer's file is the new one, and its textures are the
  files just written. The layer's label follows when it was still the old file name; a
  label the user typed is kept.
- **Names**: a texture written without a file takes its name as file name; a name taken in
  the folder gets a suffix (`uniqueTextureFileName`, `vcgimportplugin.cpp:152`, already does
  this), so nothing is overwritten. The texture filters' default name derives from the
  layer (`<layer>_color.png`, `<layer>_normal.png`), as defragmentation already does
  (`textureAssetName`), so the suffix is rarely needed.
- Filters that run external programs on files (Texture Consolidation) keep their scratch
  directory; only the result comes back, as an unsaved texture.

### Saving one texture

A texture can be saved on its own, in any format Qt can write (`QImageWriter::
supportedImageFormats()`: PNG, JPEG, TIFF, WebP, BMP ... as the installed image plugins
allow), chosen by the file's suffix. It is a **document operation, like Save Mesh, not a
filter**: a filter changes the document and becomes an undo step, a save changes the disk
and records it, outside the undo state (section 2).

- `Document::saveMeshTexture(meshIndex, textureIndex, path)`, offered as **Save Texture...**
  on a texture row of the layer panel, and in Python.
- **Saving names the texture's file**, by decision 3: the texture is now stored at the new
  path. When that differs from what the layer's mesh file refers to -- another name, or
  `.jpg` for a `.png` -- the layer differs from its file (●) until the mesh is saved too,
  because what the mesh file says about its textures has changed. Saving over the file the
  texture already has changes nothing but the texture's mark.
- Raster planes already have *Save Plane Image...*, written straight from the panel
  (`layerwidget.cpp:2458`) and not recorded anywhere. It becomes the same document
  function, so planes get the saved-state mark too.

## 5. Saving a project

A project writes only what is not already stored and unchanged, into one flat folder:

- a layer with no file goes to `meshes/` under its layer name, **suffixed when taken**, with
  its textures beside it by the Save Mesh rule above -- one mechanism, so collisions are
  handled the same way in both places. A folder per layer is not used: most layers are a
  single file, and packed formats are one file with their textures.
- *Save modified meshes* re-saves a changed layer over its own file by the same rule.
- *Copy files into the project* copies a mesh **with** its texture files into `meshes/`, and
  the project records the copies.
- After a successful save, the layers and textures written record it (section 2), so the
  next save writes only what changed since.

## 6. The document directory

`Document::directory()`:

- the project's folder when a project is open or has been saved;
- otherwise set by the **first mesh loaded into an empty document**, once, with a status-bar
  message; later loads leave it alone;
- shown in the **status bar, before the unit**, and changed from there.

It is the default location of *Save Mesh* for a layer with no file, of *Save Project*
(suggesting `<directory>/<first layer>.mlp`) and of exports; the base for a FileSave
parameter given as a bare name; and the reference for showing layer and texture paths
(`scans/a.ply`, `../ref/b.ply`, absolute on another volume). It never resolves a mesh's own
textures.

## 7. The texture name parameter

Six filters take, today, a **file path** for the texture they create. After the change none
of them writes a file, so the parameter becomes the texture's **name**, which is what Save
Mesh uses as its file name later. Today the six call it three different things (`textName`
in four, `textureFile` in one, `textureName` in one) and its Python name follows. The
id is `textureName` (Python `texture_name`; decided 2026-10-31), a plain string, empty by
default meaning "derive it from the layer" (section 4). As with every Python rename, no
alias for the old names.

## 8. Mapping table

Evidence is from the files as they stand on 2026-10-30 (`devel` at `0a9eefb`).

### 8.1 Filters that write a texture while running (6)

| # | filter (`id`) | parameter today | evidence | target |
|---|---|---|---|---|
| 1 | `transfer_color_from_vertex_to_texture` | `textName`, filesave, `texture.png` | `filters.json:424`, `texturefilterplugin.cpp:992,1044` | `textureName`; returns an unsaved texture |
| 2 | `transfer_vertex_attributes_to_texture_by_closest_point` | `textName`, filesave, `texture.png` | `filters.json:623`, `texturefilterplugin.cpp:1171,1310` | as 1 |
| 3 | `convert_object_space_normal_map_to_tangent_space` | `targetNormalMap`, textureoutputref (overwrite a slot, or a new file) | `filters.json:735`, `texturefilterplugin.cpp:1373,1439` | overwrite: replaces the slot's image, the file is left alone; new: a name |
| 4 | `transfer_color_from_visible_rasters_to_texture` | `textName`, filesave, `projected_texture.png` | `filter_color_projection/filters.json:151`, `colorprojectionfilterplugin.cpp:313,411` | as 1 |
| 5 | `transfer_color_from_visible_rasters_to_texture_texture_consolidation` | `textureFile`, filesave, `consolidated_texture.png` | `filter_texture_consolidation/filters.json:113`, `textureconsolidationfilterplugin.cpp:149,387` | as 1; helpers keep their scratch directory |
| 6 | `parametrize_from_registered_rasters_with_texture` | `textureName`, filesave, `texture.png` | `filter_img_patch_param/filters.json:124`, `imgpatchparamfilterplugin.cpp:1108,1121` | as 1 |

### 8.2 Already returning an unsaved texture (5, no change)

| # | source | evidence |
|---|---|---|
| 7 | Texture defragmentation | `texturedefragfilterplugin.cpp:834` |
| 8 | xatlas, dummy texture | `xatlasfilterplugin.cpp:283` |
| 9 | Set Texture, dummy | `texturefilterplugin.cpp:892` |
| 10 | glTF reader | `gltfimportplugin.cpp:1123` -- recorded as stored inside its file at load |
| 11 | 3MF reader | `threemfplugin.cpp:278` -- as 10 |

### 8.3 Framework

| # | code | evidence | target |
|---|---|---|---|
| 12 | `TextureAssociationUtils::saveImages` | only callers are 1-6 | **remove** |
| 13 | `makeTextureAssetsFromSavedImages` | callers are 1, 4, 5 | **remove**; `makeTextureAssetFromImage` exists |
| 14 | `anchorOutputPath`, FileSave branch | `meshfilterpluginmanager.cpp:810,1450` | **keep** (FileSave stays, decided 2026-10-30); base becomes `directory()` |
| 15 | `anchorOutputPath`, TextureOutputRef branch | `:1454` | **remove**: no path left |
| 16 | TextureOutputRef "new" branch | `meshfilterplugin.h:111`, a file path | a texture name |

### 8.4 Saved state and its display

| # | code | evidence | target |
|---|---|---|---|
| 17 | `MeshEntry::modified` as "changed" | data_model.md "Changed"; set by selection, cleared by reload only | replaced, for display and for *save modified meshes*, by the saved revisions of section 2 |
| 18 | saved state of a layer | none | file + revisions at load/save, outside the undo state |
| 19 | saved state of a texture | none | image `cacheKey` + where, at load/save, outside the undo state |
| 20 | *Src* column | `layerwidget.cpp:2092`: `D`/`G`/`*` | state mark, section 3 |
| 21 | tree prefix | `layerwidget.cpp:1810`: `[D]`/`[G]`/`*` | the same mark |
| 22 | texture rows | `collectLayerTextures`, no state | the same mark |
| 23 | layer tooltip "Source:" | `layerwidget.cpp:733`: absolute path | relative to `directory()` |

### 8.5 Saving

| # | code | evidence | target |
|---|---|---|---|
| 24 | PLY texture export | `vcgimportplugin.cpp:236` (file wins), `:379` (only with `copyAssociatedTextures`) | the rule of section 4 |
| 25 | OBJ texture export | measured in stage 1: wrote `mesh.textures` as they were, so a texture with no file was missing and the faces after it pointed at the wrong one | the rule of section 4 |
| 26 | glTF texture export | `gltfimportplugin.cpp:1752`: image wins | unchanged image copied from its file, not re-encoded |
| 27 | `Document::saveMesh` | `document_io.cpp:603`: changes nothing in the layer | records file and saved state (rows 18, 19); repoints the layer |
| 28 | project: layer without a file | `document_project.cpp:236`: `meshes/<name>.ply`, overwrites, default options | unique name in `meshes/`, textures written |
| 29 | project: *copy files* | `document_project.cpp:310`: mesh only, original recorded | mesh and textures copied, copies recorded |
| 30 | project: after save | data_model.md: "changes nothing" | records saved state |

### 8.6 Document directory

| # | code | evidence | target |
|---|---|---|---|
| 31 | `Document::projectFilePath` | `document.h:297,621` | `directory()` beside it, section 6 |
| 32 | first load into an empty document | `Document::loadMesh`, `document_io.cpp:126` | sets `directory()` when unset |
| 33 | status bar | unit shown there today | directory before the unit, settable |
| 34 | Save Mesh default path | `mainwindow.cpp:2624`: the layer's file | unchanged when it has one; else `directory()` |
| 35 | Save Project default | `mainwindow.cpp:2700` | `directory()/<first layer>.mlp` |

### 8.7 Saving one texture

| # | code | evidence | target |
|---|---|---|---|
| 36 | `Document::saveMeshTexture` | none | new: writes by suffix, records saved state, repoints the texture |
| 37 | texture rows of the layer panel | `textureInfoWidget`, `layerwidget.cpp:863`; no context menu | *Save Texture...* |
| 38 | *Save Plane Image...* | `layerwidget.cpp:2458`: `QImage::save` from the panel, nothing recorded | the same document function, saved state recorded |
| 39 | Python | `save_current_mesh`, `save_project` (`src/python/bindings/module.cpp:128,135`) | `save_current_mesh_texture(texture_index, path)` |

**Totals:** 6 filters to change + 5 sources already conforming + 5 framework items (3
removed) + 7 saved-state items + 7 saving items + 5 directory items + 4 single-texture
items = **39 rows**.

## 9. Decisions

Taken 2026-10-30:

1. No filter writes a texture while it runs; saving is the only way to disk.
2. FileSave parameters stay, for filters that write other files.
3. Saving names the layer's file, as in any document application.
4. The directory is shown in the status bar, before the unit; the first mesh loaded sets it.
5. No folder per layer.

Taken 2026-10-31:

6. Save Mesh (Ctrl+S) keeps opening the file dialog every time; there is no separate
   *Save Mesh As*.
7. A selection counts as a change (section 2).
8. `textureName` is the one name of section 7. (Recorded first as `textureName` /
   `texture_name`: a parameter id is also its Python keyword, so it is `textureName` there
   too, lowerCamelCase as vocabulary.md asks of parameter ids.)
9. The Python name of row 39 is `save_current_mesh_texture(texture_index, path)`, after
   `save_current_mesh`.

Taken 2026-10-31, closing:

10. A *Rename Texture* filter, since a texture's name is the file name a save writes.
11. `MeshEntry::modified`, read by nothing once stages 2 and 4 had replaced it, is removed.

## 10. How it was applied

1. **Savers** (rows 24-27, 18-19). **Done 2026-10-31.** `Document::meshStoredState` and
   `meshTextureStoredState`, recorded at load, reload and save outside the undo state;
   `MeshIOTextureContext::savedTexturePaths`, through which a saver reports where each
   texture went; one rule in the vcg saver for PLY and OBJ (`prepareTextureReferences`,
   which swaps the mesh's texture list for the save instead of copying the whole mesh)
   and in glTF; Save Mesh names the layer's file. Tests: `savingWritesTheTexturesNoFileHolds`
   (PLY, OBJ and glTF, reopened face by face; written once over two saves) and
   `savingNamesTheLayersFileAndUndoKeepsIt`. Found on the way:
   - glTF dropped every texture of a layer whose textures had no file, since it counted
     slots from `mesh.textures`, which leaves those out; PLY, when it fell back to that
     list by index, could give a face another texture's file.
   - Material and selection revisions were per-layer counters, so after an undo a new
     change could reach a revision a save had recorded. They now draw from the document's
     geometry counter (one test expected that counter to step by exactly one).
   - PLY and OBJ now refer to a texture that has a file by a path relative to the mesh;
     they wrote the absolute path. A missing texture file no longer fails the save: its
     declared name is kept.
   - vcglib's PLY and OBJ readers cannot take a texture name with spaces (PLY drops them,
     OBJ ignores the line), so MeshLab could not reopen a mesh whose texture was called
     "xatlas Dummy Checkerboard". A texture file the vcg saver writes or copies now keeps
     to `[A-Za-z0-9_.-]`, as project save does for mesh files. A texture referred to where
     it is keeps its path, spaces and all, as before.
   - A selection undone through the delta path gets a new revision, so it shows changed
     even when undone back to what was saved. Conservative; left as is.
   - "Re-save modified meshes" in project save still writes nothing (data_model.md); it is
     row 28's neighbour and belongs to stage 4.
2. **Display and single saves** (rows 17, 20-23, 36-39). **Done 2026-10-31.** The mark
   of section 3 on layers (tree prefix and *Src* column), texture rows and raster planes,
   with the file in the tooltip; `Document::saveMeshTexture` and `saveRasterPlaneImage`,
   *Save Texture...* on a texture row (a new right-click menu), *Save Plane Image...* now
   through the document and recording the save, both offering every format
   `QImageWriter` can write; Python `save_current_mesh_texture`. A selection change
   rebuilds the panel only when it turns a mark. Tests: `savingOneTextureOrPlaneNamesItsFile`
   (JPEG and TIFF, the layer changed until its mesh is saved),
   `theMarkSaysWhetherAFileHoldsTheLayer` (LayerWidget). Two parts wait for stage 5: paths
   in tooltips are absolute until the document directory exists (row 23), and the
   project's *save modified meshes* still reads `MeshEntry::modified` (row 17, with stage 4).
   Raster planes use the same interim rule as textures: an image no save recorded is
   trusted to match the file it came from.
3. **Filters** (rows 1-6, 12-16). **Done 2026-10-31.** The six filters return a texture with
   a name and no file; the five `filesave` outputs became a `string` `textureName`, empty by
   default for `<layer>_<role>.png` (`TextureAssociationUtils::outputTextureNames`), and the
   "new" choice of `textureoutputref` is a name (a line edit, map key `name`, no anchoring).
   Overwriting a texture -- Transfer Color from Vertex to Texture and the closest-point
   transfer with *overwrite*, the tangent-space conversion onto an existing slot -- replaces
   its image and keeps its name and path: the file is left alone, the texture shows changed,
   and a save writes the new image beside the mesh under a free name (`texture_1.png`), never
   over the file. `saveImages` is gone; `makeTextureAssetsFromSavedImages` became
   `makeTextureAssetsFromImages` (names, no paths). The interim rule of stage 1 is flipped:
   an image no load or save recorded is a changed texture, and saving writes it. The
   tangent-space result also records its asset index in the PBR normal slot, which a texture
   with no file needs. `anchorOutputPath` stays for FileSave, which no filter uses now.
   Tests: `textureFiltersWriteNoFile` (nothing written in the layer's folder or the working
   directory, the default name, an overwritten file byte-identical for both filters, the
   save writing beside it); the consolidation test takes the new parameter; the old
   anchoring test went with the behaviour it tested.
4. **Project save** (rows 28-30, and row 17 for *save modified meshes*). **Done
   2026-10-31.** `saveMeshLabProject` plans each layer as referenced, saved over its own
   file (changed, by `meshStoredState`, with the option on -- it wrote nothing before), saved
   new into `meshes/`, or saved as a copy into `meshes/` when outside the project; every
   write goes through `saveMesh`, so textures follow the rule of stage 1 and the stored
   state is recorded. Names in `meshes/` and `images/` are unique on disk and within the
   save. A copy is a save into the project rather than a byte copy, because a mesh's texture
   references are relative to it: what MeshLab does not read from the file is not carried
   over. Raster copies are recorded too, and a snapshot plane goes through
   `saveRasterPlaneImage`. The dialog's "Re-save modified meshes" became "Re-save changed
   meshes", with tooltips. Tests: `saveMeshLabProjectWritesWhatChangedUnderUniqueNames` (fails
   on the old code); `importImageToCurrentRasterKeepsTheCamera` asserted the old behaviour,
   that a copied plane's project still named the original, and now checks the copy.
5. **Document directory** (rows 31-35, and 14, 23). **Done 2026-10-31.**
   `Document::directory()`, `setDirectory`, `displayPath` and the `directoryChanged` signal.
   The first mesh loaded sets it, and so does the first save of a document made in memory (a
   small extension of section 6, which named loads only); a project loaded or saved replaces
   it; `clearAllLayers` clears it; it is not in the undo state. A status-bar button before
   the unit shows its name, the full path in the tooltip, and changes it on a click; the
   status bar says when it is set. Save Mesh for a layer with no file and Save Project start
   there (Save Project at the project already saved, or `<first layer>.mlp`). The layer
   panel's tooltips show paths through `displayPath`: relative inside the folder or one
   level up, absolute otherwise -- "absolute on another volume" of section 6, widened to
   anything two levels up, where a relative path stops being easier to read.
   `anchorOutputPath` uses the folder alone. Test:
   `theDocumentFolderComesFromTheFirstFileThenTheProject`.
6. **Closing.** *Rename Texture* (`rename_texture`, `Texture/Assignment`): a texture's name
   is the file name a save writes, so renaming one gives it a fresh copy of its image, which
   no save has recorded -- the texture shows changed and the next save writes it under the
   new name; its old file is left alone. vocabulary.md's `Rename` now covers textures. It
   refuses an empty name, so the smoke ladder lists it as an expected refusal;
   `textureFiltersWriteNoFile` drives it. `Document::referencesFilesOutside` replaced the
   Save Project dialog's own copy of the outside-the-folder check. `MeshEntry::modified`
   was removed with its copies in the undo snapshot. The `textureName` convention went into
   filter_organization.md, the texture-saving behaviour into python_scripting.md. The
   generated API reference (`docs/api/`, gitignored, rebuilt by the docs workflow) was
   regenerated locally to check that it lists `save_current_mesh_texture` and
   `rename_texture`.

Known and left as they are: a selection undone through the delta path gets a new revision,
so a layer undone back to a saved selection shows changed (conservative, never the
reverse); no filter has a FileSave parameter now, so anchoring a bare name in the
document's folder has no test.
