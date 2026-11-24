# Run by ctest through `MeshLab --script` (MeshLab2ScriptModeTests in CMakeLists.txt):
# the batch mode binds `ms` to a fresh document with the per-filter methods, passes its
# arguments as sys.argv, and exits nonzero when the script fails.
import sys

import pymeshlab2

mesh_path, mode = sys.argv[1], sys.argv[2]

assert isinstance(ms, pymeshlab2.MeshSet)
assert len(ms) == 0
ms.load_new_mesh(mesh_path)
faces = ms.current_mesh().face_number()
result = ms.subdivide_by_midpoint()
assert result.success, result.error_message
assert ms.current_mesh().face_number() > faces

if mode == "fail":
    raise RuntimeError("failing on purpose")
