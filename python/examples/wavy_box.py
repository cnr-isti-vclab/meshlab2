from __future__ import annotations

import math
from pathlib import Path

import pymeshlab2


SIDE = 1.0


def main(argv: list[str] | None = None) -> pymeshlab2.MeshSet | Path:
    meshset = pymeshlab2.MeshSet()

    # Three waves per cube side. With SIDE=1, omega is simply 6*pi.
    omega = 2.0 * math.pi * 3.0 / SIDE

    # Start from a unit cube. No parameters are needed because size=1 is the default.
    meshset.apply_filter("create_hexahedron")

    # Uniform midpoint refinement: the default threshold is bboxDiag*0.01, so set it
    # to zero to refine every edge. Iterations keeps the MeshLab2 default value.
    meshset.apply_filter("subdivide_by_midpoint", {"Threshold": 0.0})

    # Move vertices along z with amplitude SIDE/5 and frequency 3 over x and y.
    # The x and y expressions are omitted because their defaults are "x" and "y".
    meshset.apply_filter(
        "compute_vertex_coordinates_by_expression",
        {"z": f"z + {SIDE / 5.0:.17g} * sin({omega:.17g} * x) * sin({omega:.17g} * y)"},
    )

    # Remesh with the isotropic-remeshing defaults.
    meshset.apply_filter("remesh_isotropically_vcglib")

    if argv:
        output = Path(argv[0])
        meshset.save_current_mesh(str(output))
        return output
    return meshset


if __name__ == "__main__":
    main()
