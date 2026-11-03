from __future__ import annotations

import pymeshlab2


def main(argv: list[str] | None = None) -> tuple[int, int]:
    _ = argv
    meshset = pymeshlab2.MeshSet()
    raster_state = (meshset.raster_number(), meshset.current_raster())

    print(f"raster_number={raster_state[0]}")
    print(f"current_raster={raster_state[1]}")

    return raster_state


if __name__ == "__main__":
    main()
