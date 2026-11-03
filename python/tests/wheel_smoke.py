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

print(f"Loaded {len(filter_ids)} filters from {package_dir}")
