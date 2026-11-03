from __future__ import annotations

import argparse
from pathlib import Path

import pymeshlab2


DEFAULT_MESH = Path(__file__).resolve().parents[2] / "tests/data/simple.off"


def main(argv: list[str] | None = None) -> pymeshlab2.FilterRunResult:
    parser = argparse.ArgumentParser()
    parser.add_argument("mesh", nargs="?", default=str(DEFAULT_MESH), help="mesh file to inspect")
    parser.add_argument("--precision", type=int, default=3, help="decimal precision for metrics")
    args = parser.parse_args(argv)

    meshset = pymeshlab2.MeshSet()
    meshset.load_new_mesh(args.mesh)
    params = {"precision": args.precision} if args.precision != 3 else {}
    result = meshset.apply_filter("measure_mesh_summary", params)

    for message in result.info_messages:
        print(message)

    return result


if __name__ == "__main__":
    main()
