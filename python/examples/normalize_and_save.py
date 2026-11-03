from __future__ import annotations

import argparse
from pathlib import Path

import pymeshlab2


DEFAULT_MESH = Path(__file__).resolve().parents[2] / "tests/data/simple.off"


def main(argv: list[str] | None = None) -> Path:
    parser = argparse.ArgumentParser()
    parser.add_argument("mesh", nargs="?", default=str(DEFAULT_MESH), help="mesh file to normalize")
    parser.add_argument("--output", default="normalized.ply", help="where to write the normalized mesh")
    args = parser.parse_args(argv)

    output = Path(args.output)

    meshset = pymeshlab2.MeshSet()
    meshset.load_new_mesh(args.mesh)

    # The defaults center the bounding box, align its principal axes, and scale
    # its longest side to one, so this call needs no explicit parameters.
    meshset.apply_filter("normalize_reference_frame")
    meshset.save_current_mesh(str(output))

    print(output)
    return output


if __name__ == "__main__":
    main()
