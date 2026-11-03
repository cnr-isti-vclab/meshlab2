import ctypes
import os
from pathlib import Path
import sys

import pymeshlab2


meshset = pymeshlab2.MeshSet()
filter_ids = {filter_info.id for filter_info in meshset.list_filters()}
assert "create_hexahedron" in filter_ids
assert "remesh_to_quads_quadwild_bimdf" in filter_ids
assert "mesh_union_geogram" in filter_ids

result = meshset.apply_filter("create_hexahedron")
assert result.success
assert meshset.mesh_number() == 1
assert meshset.apply_filter(
    "remesh_by_centroidal_voronoi_tessellation_geogram",
    {"targetVertexCount": 30},
).success

package_dir = Path(pymeshlab2.__file__).resolve().parent
if sys.platform == "darwin":
    assert (package_dir / "libgeogram_num_3rdparty.dylib").is_file()
    ctypes.CDLL("libgeogram_num_3rdparty.dylib")
elif os.name == "nt":
    assert (package_dir / "geogram_num_3rdparty.dll").is_file()
    ctypes.WinDLL("geogram_num_3rdparty.dll")
quadwild_dir = package_dir / "quadwild-bimdf"
suffix = ".exe" if os.name == "nt" else ""
assert (quadwild_dir / "bin" / f"quadwild{suffix}").is_file()
assert (quadwild_dir / "bin" / f"quad_from_patches{suffix}").is_file()
assert (quadwild_dir / "config" / "prep_config" / "basic_setup.txt").is_file()
assert os.environ["MESHLAB2_QUADWILD_ROOT"] == str(quadwild_dir)

# TextureConsolidation runs three helper programs whose libomp (macOS) or vcpkg DLLs
# (Windows) the wheel repair vendors, so only running the filter shows they load. About
# 20 s on an M-series Mac with a 256 px texture.
consolidation_bin = package_dir / "texture-consolidation" / "bin"
for helper in ("BackProjection", "TextureFlow", "TextureDomainStitching"):
	assert (consolidation_bin / f"{helper}{suffix}").is_file()
assert os.environ["MESHLAB2_TEXTURE_CONSOLIDATION_ROOT"] == str(consolidation_bin)
gargoyle = Path(__file__).resolve().parents[2] / "tests" / "sample_mesh" / "gargoyle_small" / "gargoyle.mlp"
rasters = pymeshlab2.MeshSet()
rasters.load_project(str(gargoyle))
assert rasters.apply_filter("parametrize_by_atlas_xatlas").success
result = rasters.apply_filter(
	"transfer_color_from_visible_rasters_to_texture_texture_consolidation",
	{"textureSize": 256},
)
assert result.success, result.error_message

print(f"Loaded {len(filter_ids)} filters from {package_dir}")
