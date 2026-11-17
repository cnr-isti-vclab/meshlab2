"""Inventory of every mesh under a folder, written as a Markdown report.

With no options the meshes are only listed (path, format, size) and never opened.
Options add columns by loading each mesh and running MeshLab's measure filters.

The input folder is treated as strictly read-only: nothing is written, renamed or
extracted inside it. Meshes found inside zip archives are unpacked into a temporary
directory outside the input tree and deleted right after measuring them, and the
report path is refused if it resolves inside the input tree.

    python inventory.py ROOT -o report.md                      # list only
    python inventory.py ROOT -o report.md --zip --open         # + zip contents, V/F counts
    python inventory.py ROOT -o report.md --zip --geometry --topology --limit 20
"""

from __future__ import annotations

import argparse
import json
import sys
import tempfile
import time
import zipfile
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path, PurePosixPath

MESH_EXTENSIONS = {"ply", "obj", "stl", "off", "vmi", "gltf", "glb", "3mf", "e57"}

GEOMETRY_COLUMNS = {  # output value of measure_geometric_properties -> column header
    "bbox_dim_x": "dim X",
    "bbox_dim_y": "dim Y",
    "bbox_dim_z": "dim Z",
    "surface_area": "area",
    "is_watertight": "watertight",
}
TOPOLOGY_COLUMNS = {  # output value of measure_topological_properties -> column header
    "connected_components_number": "components",
    "number_holes": "holes",
    "genus": "genus",
    "non_two_manifold_edges": "non-manifold E",
    "non_two_manifold_vertices": "non-manifold V",
}


@dataclass
class Entry:
    path: str  # relative to the root; "archive.zip!member.obj" for zip members
    ext: str
    size: int  # bytes on disk, with .gltf sidecars; uncompressed size for zip members
    group: str  # leading folders of the path, --depth of them
    source: Path  # file on disk (the archive, for zip members)
    member: str | None = None  # name inside the archive
    values: dict = field(default_factory=dict)
    error: str = ""


def is_inside(path: Path, root: Path) -> bool:
    return path.resolve() == root or root in path.resolve().parents


def file_size(path: Path) -> int:
    """Bytes on disk, counting a .gltf file's external buffers and images too."""
    size = path.stat().st_size
    if path.suffix.lower() == ".gltf":
        try:
            doc = json.loads(path.read_text(encoding="utf-8"))
            for item in doc.get("buffers", []) + doc.get("images", []):
                uri = item.get("uri", "")
                if uri and not uri.startswith("data:") and (path.parent / uri).is_file():
                    size += (path.parent / uri).stat().st_size
        except (OSError, ValueError):
            pass
    return size


def collect(root: Path, look_in_zips: bool, depth: int) -> list[Entry]:
    entries = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.name.startswith("."):
            continue
        rel = path.relative_to(root)
        group = "/".join(rel.parts[: min(depth, len(rel.parts) - 1)]) or "."
        ext = path.suffix.lower().lstrip(".")
        if ext in MESH_EXTENSIONS:
            entries.append(Entry(rel.as_posix(), ext, file_size(path), group, path))
        elif ext == "zip" and look_in_zips:
            try:
                with zipfile.ZipFile(path) as archive:  # reading the index only
                    for info in archive.infolist():
                        mext = PurePosixPath(info.filename).suffix.lower().lstrip(".")
                        if mext in MESH_EXTENSIONS and not info.is_dir():
                            entries.append(Entry(f"{rel.as_posix()}!{info.filename}", mext,
                                                 info.file_size, group, path, info.filename))
            except zipfile.BadZipFile as exc:
                entries.append(Entry(rel.as_posix(), ext, path.stat().st_size, group, path,
                                     error=f"bad zip: {exc}"))
    return entries


def measure(entry: Entry, args: argparse.Namespace, scratch: Path) -> None:
    import pymeshlab2

    target = entry.source
    workdir = None
    if entry.member is not None:
        # The whole archive is unpacked so an OBJ finds its MTL and textures.
        workdir = tempfile.TemporaryDirectory(dir=scratch)
        with zipfile.ZipFile(entry.source) as archive:
            archive.extractall(workdir.name)
        target = Path(workdir.name) / entry.member
    try:
        ms = pymeshlab2.MeshSet()
        start = time.perf_counter()
        ms.load_new_mesh(str(target))
        entry.values["load s"] = time.perf_counter() - start
        meshes = [ms.mesh(i) for i in range(ms.mesh_number())]
        entry.values["layers"] = len(meshes)
        entry.values["V"] = sum(m.vertex_number() for m in meshes)
        entry.values["F"] = sum(m.face_number() for m in meshes)
        mesh = ms.current_mesh()
        entry.values["UV"] = mesh.has_wedge_tex_coord() or mesh.has_vertex_tex_coord()
        for wanted, filter_id, columns in (
            (args.geometry, "measure_geometric_properties", GEOMETRY_COLUMNS),
            (args.topology, "measure_topological_properties", TOPOLOGY_COLUMNS),
        ):
            if not wanted:
                continue
            result = ms.apply_filter(filter_id, {})
            if not result.success:
                entry.error = f"{filter_id}: {result.error_message}"
                continue
            for key, header in columns.items():
                entry.values[header] = result.output_values.get(key)
    except Exception as exc:  # noqa: BLE001 - a broken file must not stop the sweep
        entry.error = str(exc).splitlines()[0] if str(exc) else type(exc).__name__
    finally:
        if workdir is not None:
            workdir.cleanup()


def fmt(value) -> str:
    if value is None:
        return ""
    if isinstance(value, bool):
        return "yes" if value else "no"
    if isinstance(value, int):
        return f"{value:,}"
    if isinstance(value, float):
        return f"{value:.4g}"
    return str(value).replace("|", "\\|")


def human_size(size: int) -> str:
    for unit in ("B", "KiB", "MiB", "GiB"):
        if size < 1024 or unit == "GiB":
            return f"{size:.0f} {unit}" if unit == "B" else f"{size:.1f} {unit}"
        size /= 1024
    return ""


def table(headers: list[str], rows: list[list[str]], right: set[int]) -> list[str]:
    lines = ["| " + " | ".join(headers) + " |",
             "|" + "|".join("---:" if i in right else "---" for i in range(len(headers))) + "|"]
    lines += ["| " + " | ".join(row) + " |" for row in rows]
    return lines


def summary_rows(entries: list[Entry], key, opened: bool, args) -> list[list[str]]:
    groups = defaultdict(list)
    for entry in entries:
        groups[key(entry)].append(entry)
    rows = []
    for name in sorted(groups) + ["**total**"]:
        items = entries if name == "**total**" else groups[name]
        row = [fmt(name), fmt(len(items)), human_size(sum(e.size for e in items))]
        if opened:
            ok = [e for e in items if not e.error]
            faces = [e.values.get("F", 0) for e in ok]
            row += [fmt(len(items) - len(ok)), fmt(sum(e.values.get("V", 0) for e in ok)),
                    fmt(sum(faces)), fmt(min(faces) if faces else None),
                    fmt(max(faces) if faces else None)]
            if args.geometry:
                row.append(fmt(sum(1 for e in ok if e.values.get("watertight"))))
        rows.append(row)
    return rows


def write_report(path: Path, root: Path, entries: list[Entry], args, elapsed: float) -> None:
    opened = args.open or args.geometry or args.topology
    lines = [f"# Mesh inventory of `{root.name}`", "",
             f"- root: `{root}`",
             f"- generated: {time.strftime('%Y-%m-%d %H:%M')} in {elapsed:.1f} s",
             f"- options: {' '.join(sys.argv[1:]) or '(none)'}",
             f"- meshes: {len(entries)}" + (" (meshes were not opened)" if not opened else ""),
             ""]

    sum_headers = ["files", "size"]
    if opened:
        sum_headers += ["failed", "vertices", "faces", "min F", "max F"]
        if args.geometry:
            sum_headers.append("watertight")
    right = set(range(1, len(sum_headers) + 1))
    for title, key in (("folder", lambda e: e.group), ("format", lambda e: e.ext)):
        lines += [f"## Summary by {title}", ""]
        lines += table([title] + sum_headers, summary_rows(entries, key, opened, args), right)
        lines.append("")

    value_headers = []
    if opened:
        value_headers += ["layers", "V", "F", "UV", "load s"]
        if args.geometry:
            value_headers += list(GEOMETRY_COLUMNS.values())
        if args.topology:
            value_headers += list(TOPOLOGY_COLUMNS.values())
    headers = ["file", "format", "size"] + value_headers + (["error"] if opened or
                                                             any(e.error for e in entries) else [])
    rows = []
    for entry in entries:
        row = [f"`{fmt(entry.path)}`", entry.ext, human_size(entry.size)]
        row += [fmt(entry.values.get(h)) for h in value_headers]
        if len(headers) > len(row):
            row.append(fmt(entry.error))
        rows.append(row)
    lines += ["## Files", ""]
    lines += table(headers, rows, set(range(2, 3 + len(value_headers))))
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> list[Entry]:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("root", type=Path, help="folder to scan recursively (never modified)")
    parser.add_argument("-o", "--output", type=Path, required=True,
                        help="Markdown report to write; must be outside the scanned folder")
    parser.add_argument("--zip", action="store_true",
                        help="also list meshes inside zip archives (read without extracting)")
    parser.add_argument("--open", action="store_true",
                        help="load every mesh: layers, vertex/face counts, UVs, load time")
    parser.add_argument("--geometry", action="store_true",
                        help="bounding box, surface area, watertightness (implies --open)")
    parser.add_argument("--topology", action="store_true",
                        help="components, holes, genus, non-manifoldness (implies --open)")
    parser.add_argument("--depth", type=int, default=1,
                        help="folder levels below the root used to group the summary")
    parser.add_argument("--limit", type=int, default=0, help="stop after this many meshes")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    if not root.is_dir():
        parser.error(f"not a folder: {root}")
    if is_inside(args.output, root):
        parser.error(f"the report must be written outside the scanned folder: {args.output}")
    scratch = Path(tempfile.gettempdir()).resolve()
    if is_inside(scratch, root):
        parser.error(f"the temporary directory {scratch} is inside the scanned folder")

    start = time.perf_counter()
    entries = collect(root, args.zip, max(args.depth, 1))
    if args.limit:
        entries = entries[: args.limit]
    if args.open or args.geometry or args.topology:
        for i, entry in enumerate(entries, 1):
            if not entry.error:
                print(f"[{i}/{len(entries)}] {entry.path}", file=sys.stderr, flush=True)
                measure(entry, args, scratch)
    write_report(args.output, root, entries, args, time.perf_counter() - start)
    print(f"{len(entries)} meshes -> {args.output}", file=sys.stderr)
    return entries


if __name__ == "__main__":
    main()
