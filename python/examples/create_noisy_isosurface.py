from __future__ import annotations

import argparse

import pymeshlab2


def main(argv: list[str] | None = None) -> pymeshlab2.FilterRunResult:
    parser = argparse.ArgumentParser()
    parser.add_argument("--resolution", type=int, default=64, help="volume resolution")
    args = parser.parse_args(argv)

    meshset = pymeshlab2.MeshSet()
    params = {"resolution": args.resolution} if args.resolution != 64 else {}
    result = meshset.apply_filter("create_isosurface_from_perlin_noise", params)
    if not result.new_mesh_indices:
        raise RuntimeError("create_isosurface_from_perlin_noise did not create a mesh")

    print(f"mesh_number={meshset.mesh_number()}")
    print(f"new_mesh_indices={result.new_mesh_indices}")
    return result


if __name__ == "__main__":
    main()
