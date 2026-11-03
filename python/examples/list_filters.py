from __future__ import annotations

import pymeshlab2


def main(argv: list[str] | None = None) -> list[pymeshlab2.FilterInfo]:
    _ = argv
    filters = pymeshlab2.MeshSet().list_filters()

    for item in filters:
        print(f"{item.key}\t{item.python_name}\t{item.name}")

    return filters


if __name__ == "__main__":
    main()
