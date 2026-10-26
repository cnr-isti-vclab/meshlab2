#!/usr/bin/env python3
"""Report the metrics of the TexMetro tool for a textured model, from MeshLab's filters.

TexMetro (https://github.com/cnr-isti-vclab/texmetro) computes the statistics of the benchmark
"Real-World Textured Things". MeshLab now has them as filters (see texture_metrics.md); this script
runs those filters on one model and writes them as TexMetro's own JSON file, so that the two can be
compared, or so that scripts written for TexMetro's output keep working.

    python texmetro_report.py model.obj                        # writes texmetro.json
    python texmetro_report.py model.obj -o model.json
    python texmetro_report.py model.obj -t 2048x1024           # the texture size, if it has no image
    python texmetro_report.py model.obj --compare reference.json   # against a file TexMetro wrote

It needs `pymeshlab`: the standalone package, or MeshLab's own Python console (where it is
already imported), with the model's texture images next to the model, as TexMetro does.

KEY_MAP below is where each key of TexMetro's JSON comes from; the table in texture_metrics.md is
this list, printed by `--key-table`. The keys that are not in it are handled in `build_report`:
`mesh`, `ntex` and `texture_N`. `orientable` and `textures` are not reported.
"""

import argparse
import json
import math
import os
import re
import sys

# (TexMetro key, MeshLab filter, key of that filter's output values), in the order of TexMetro's JSON.
# One entry to a line: a test reads this list and checks that every filter does return its key.
KEY_MAP = [
    ("fn", "measure_topological_properties", "faces_number"),
    ("vn", "measure_topological_properties", "vertices_number"),
    ("en", "measure_topological_properties", "edges_number"),
    ("en_b", "measure_topological_properties", "boundary_edges"),
    ("area", "measure_geometric_properties", "surface_area"),
    ("boundary_len", "measure_geometric_properties", "boundary_length"),
    ("area_normalized", "measure_geometric_properties", "normalized_surface_area"),
    ("boundary_len_normalized", "measure_geometric_properties", "normalized_boundary_length"),
    ("boundary_loops", "measure_topological_properties", "number_holes"),
    ("nonmanif_edge", "measure_topological_properties", "non_two_manifold_edges"),
    ("nonmanif_vert", "measure_topological_properties", "non_two_manifold_vertices"),
    ("connected_components", "measure_topological_properties", "connected_components_number"),
    ("genus", "measure_topological_properties", "genus"),
    ("fndup", "measure_topological_properties", "duplicate_faces"),
    ("fnzero", "measure_topological_properties", "zero_area_faces"),
    ("fndegen", "measure_topological_properties", "degenerate_faces"),
    ("vnunref", "measure_topological_properties", "unreferenced_vertices"),
    ("oriented", "measure_topological_properties", "is_mesh_oriented"),
    ("en_uv", "measure_uv_mapping", "en_uv"),
    ("en_uv_b", "measure_uv_mapping", "en_uv_b"),
    ("area_uv", "measure_uv_mapping", "area_uv"),
    ("boundary_len_uv", "measure_uv_mapping", "boundary_len_uv"),
    ("num_charts", "measure_uv_mapping", "num_charts"),
    ("num_null_charts", "measure_uv_mapping", "num_null_charts"),
    ("mapped_fraction", "measure_uv_mapping", "mapped_fraction"),
    ("nfolds", "measure_uv_mapping", "nfolds"),
    ("occupancy", "measure_uv_mapping", "occupancy"),
    ("seam_discrepancy_avg", "measure_texture_seam_discrepancy", "seam_discrepancy_avg"),
    ("seam_discrepancy_min", "measure_texture_seam_discrepancy", "seam_discrepancy_min"),
    ("seam_discrepancy_max", "measure_texture_seam_discrepancy", "seam_discrepancy_max"),
    ("seam_discrepancy_rms", "measure_texture_seam_discrepancy", "seam_discrepancy_rms"),
    ("seam_discrepancy_var", "measure_texture_seam_discrepancy", "seam_discrepancy_var"),
    ("seam_discrepancy_stddev", "measure_texture_seam_discrepancy", "seam_discrepancy_stddev"),
    ("qcd_perc1", "measure_uv_mapping", "qcd_perc1"),
    ("qcd_perc10", "measure_uv_mapping", "qcd_perc10"),
    ("qcd_perc20", "measure_uv_mapping", "qcd_perc20"),
    ("qcd_perc30", "measure_uv_mapping", "qcd_perc30"),
    ("qcd_perc40", "measure_uv_mapping", "qcd_perc40"),
    ("qcd_perc50", "measure_uv_mapping", "qcd_perc50"),
    ("qcd_perc60", "measure_uv_mapping", "qcd_perc60"),
    ("qcd_perc70", "measure_uv_mapping", "qcd_perc70"),
    ("qcd_perc80", "measure_uv_mapping", "qcd_perc80"),
    ("qcd_perc90", "measure_uv_mapping", "qcd_perc90"),
    ("qcd_perc99", "measure_uv_mapping", "qcd_perc99"),
    ("qcd_min", "measure_uv_mapping", "qcd_min"),
    ("qcd_max", "measure_uv_mapping", "qcd_max"),
    ("qcd_avg", "measure_uv_mapping", "qcd_avg"),
    ("qcd_rms", "measure_uv_mapping", "qcd_rms"),
    ("qcd_var", "measure_uv_mapping", "qcd_var"),
    ("qcd_stddev", "measure_uv_mapping", "qcd_stddev"),
    ("qcd_discardedArea", "measure_uv_mapping", "qcd_discardedArea"),
    ("sf_perc1", "measure_uv_mapping", "sf_perc1"),
    ("sf_perc10", "measure_uv_mapping", "sf_perc10"),
    ("sf_perc20", "measure_uv_mapping", "sf_perc20"),
    ("sf_perc30", "measure_uv_mapping", "sf_perc30"),
    ("sf_perc40", "measure_uv_mapping", "sf_perc40"),
    ("sf_perc50", "measure_uv_mapping", "sf_perc50"),
    ("sf_perc60", "measure_uv_mapping", "sf_perc60"),
    ("sf_perc70", "measure_uv_mapping", "sf_perc70"),
    ("sf_perc80", "measure_uv_mapping", "sf_perc80"),
    ("sf_perc90", "measure_uv_mapping", "sf_perc90"),
    ("sf_perc99", "measure_uv_mapping", "sf_perc99"),
    ("sf_min", "measure_uv_mapping", "sf_min"),
    ("sf_max", "measure_uv_mapping", "sf_max"),
    ("sf_avg", "measure_uv_mapping", "sf_avg"),
    ("sf_rms", "measure_uv_mapping", "sf_rms"),
    ("sf_var", "measure_uv_mapping", "sf_var"),
    ("sf_stddev", "measure_uv_mapping", "sf_stddev"),
    ("sf_discardedArea", "measure_uv_mapping", "sf_discardedArea"),
]

# Where TexMetro's JSON has a key that is not a plain output value of a filter, for the key table.
OTHER_KEYS = [
    ("mesh", "the file name of the model", ""),
    ("ntex", "measure_texel_coverage", "ntex"),
    ("texture_N", "measure_texel_coverage", "texture_N: a list with an entry per mipmap level, with TexMetro's keys"),
    ("orientable", "not reported", "OrientCoherentlyMesh flips faces, so no report can use it"),
    ("textures", "not reported", "the layer knows its textures; ntex is their number"),
]

# How far a value of ours may be from TexMetro's before --compare calls it a difference: the first
# pattern that matches the key, as (relative, absolute). They are a starting point, from what is known
# of the sources of difference, and are meant to be set from real comparisons.
#  - counts of the mesh and of the UV layout are exact;
#  - the common texel counters follow the rasterizer, and differ only at the texels a GPU rounds
#    differently from the CPU (its subpixel precision is 8 bits on most hardware, 4 on some);
#  - the rare ones (overwritten, lost, clash) move by about 0.1 % to 1 % with that precision;
#  - TexMetro writes a real number with six significant digits, which is the floor of every other tolerance.
TOLERANCES = [
    (r"^(fn|vn|en|en_b|boundary_loops|nonmanif_edge|nonmanif_vert|connected_components|genus|fndup|fnzero|"
     r"fndegen|vnunref|oriented|en_uv|en_uv_b|num_charts|num_null_charts|nfolds|ntex)$", 0.0, 0.0),
    (r"^texture_\d+\[\d+\]\.(rw|rh|totalTexels|totalTexels_bilinear|boundaryTexels)$", 1e-4, 2.0),
    (r"^texture_\d+\[\d+\]\.(overwrittenTexels|lostTexels|texelClashes)$", 2e-2, 10.0),
    (r"^(occupancy|sf_.*|qcd_discardedArea)$", 1e-3, 1e-6),
    (r"^seam_discrepancy_", 1e-3, 1e-3),
    (r".*", 1e-4, 1e-6),
]
SIX_DIGITS = 2e-6  # the rounding of a number TexMetro writes, relative

NOT_COMPARED = ("mesh", "textures", "orientable")


def run(ms, name, params):
    """The output values of a filter on the current layer, or None if the filter refuses it."""
    try:
        return ms.apply_filter(name, params).output_values
    except RuntimeError as error:
        print("  %s: %s" % (name, str(error).replace("apply_filter failed: ", "")), file=sys.stderr)
        return None


def build_report(path, texture_size=None, pairing="edge_order", clash="diagonals"):
    """TexMetro's JSON, as a dictionary, for the model at `path`."""
    import pymeshlab

    ms = pymeshlab.MeshSet()
    ms.load_new_mesh(path)
    size = {}
    if texture_size:
        size = {"textureWidth": texture_size[0], "textureHeight": texture_size[1]}

    outputs = {
        "measure_topological_properties": run(ms, "measure_topological_properties", {}),
        "measure_geometric_properties": run(ms, "measure_geometric_properties", {}),
        "measure_uv_mapping": run(ms, "measure_uv_mapping", dict(size)),
        "measure_texel_coverage": run(
            ms, "measure_texel_coverage", dict(size, clashNeighbours=clash)),
        # The colors of the seams need the images: a size given with -t is not enough.
        "measure_texture_seam_discrepancy": run(
            ms, "measure_texture_seam_discrepancy", {"pairing": pairing}),
    }
    if outputs["measure_uv_mapping"] is None:
        sys.exit("The model has no texture coordinates, or could not be measured.")
    if outputs["measure_texel_coverage"] is None:
        sys.exit("The model has no texture image and no size was given: use -t WIDTHxHEIGHT.")

    report = {"mesh": os.path.basename(path)}
    for texmetro_key, name, key in KEY_MAP:
        values = outputs.get(name)
        if values is not None and key in values:
            report[texmetro_key] = values[key]
        # Where TexMetro's order puts the texture lists: after the seams, before the distortions.
        if texmetro_key == "seam_discrepancy_stddev":
            coverage = outputs["measure_texel_coverage"]
            report["ntex"] = coverage["ntex"]
            for t in range(coverage["ntex"]):
                report["texture_%d" % t] = coverage["texture_%d" % t]
    return report


def flatten(report):
    """The numbers of a report under one name each: the texture lists as `texture_0[3].lostTexels`."""
    flat = {}
    for key, value in report.items():
        if key.startswith("texture_") and isinstance(value, list):
            for level, counters in enumerate(value):
                for name, number in counters.items():
                    flat["%s[%d].%s" % (key, level, name)] = number
        elif isinstance(value, (bool, int, float)):
            flat[key] = value
    return flat


def tolerance(key):
    for pattern, relative, absolute in TOLERANCES:
        if re.match(pattern, key):
            return relative, absolute


def compare(ours, reference, show_all=False):
    """Print how a report differs from a reference file of TexMetro's. Returns the number of differences."""
    a, b = flatten(ours), flatten(reference)
    rows, counts = [], {"ok": 0, "DIFF": 0, "missing": 0}
    for key, expected in b.items():
        if key.split("[")[0] in NOT_COMPARED:
            continue
        if key not in a:
            counts["missing"] += 1
            rows.append(("missing", key, None, expected, None))
            continue
        value = a[key]
        difference = abs(float(value) - float(expected))
        scale = max(abs(float(value)), abs(float(expected)))
        relative, absolute = tolerance(key)
        if isinstance(expected, float) and not float(expected).is_integer():
            relative = max(relative, SIX_DIGITS)
        status = "ok" if difference <= max(absolute, relative * scale) else "DIFF"
        counts[status] += 1
        rows.append((status, key, value, expected, difference / scale if scale else 0.0))
    for status, key, value, expected, relative in rows:
        if show_all or status != "ok":
            if value is None:
                print("%-8s %-40s %-16s %-16s" % (status, key, "-", expected))
            else:
                print("%-8s %-40s %-16.10g %-16.10g %.2e" % (status, key, value, expected, relative))
    print("%d compared: %d ok, %d differ, %d missing from this report"
          % (len(rows), counts["ok"], counts["DIFF"], counts["missing"]))
    return counts["DIFF"]


def key_table():
    """The mapping of TexMetro's keys to MeshLab's, as a Markdown table."""
    lines = ["| TexMetro key | MeshLab filter | Output value |", "|---|---|---|"]
    for texmetro_key, name, key in KEY_MAP:
        lines.append("| `%s` | `%s` | `%s` |" % (texmetro_key, name, key))
    for texmetro_key, name, key in OTHER_KEYS:
        lines.append("| `%s` | %s | %s |" % (texmetro_key, name if " " in name else "`%s`" % name, key))
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="TexMetro's report of a textured model, from MeshLab.")
    parser.add_argument("model", nargs="?", help="the model, with its texture images")
    parser.add_argument("-o", "--json", default="texmetro.json", help="the file to write (default texmetro.json)")
    parser.add_argument("-t", "--texsize", metavar="WxH", help="the texture size, for a model without images")
    parser.add_argument("--pairing", choices=["edge_order", "position"], default="edge_order",
                        help="seam samples: TexMetro's (the default) or the same point on both sides")
    parser.add_argument("--clash", choices=["diagonals", "all"], default="diagonals",
                        help="texel clashes: TexMetro's diagonal neighbours (the default) or all eight")
    parser.add_argument("--compare", metavar="REFERENCE", help="also compare with a JSON file TexMetro wrote")
    parser.add_argument("--all", action="store_true", help="with --compare, list the keys that agree too")
    parser.add_argument("--key-table", action="store_true", help="print the key mapping as Markdown and stop")
    args = parser.parse_args(argv)

    if args.key_table:
        print(key_table())
        return 0
    if not args.model:
        parser.error("a model is needed")
    texture_size = None
    if args.texsize:
        match = re.fullmatch(r"(\d+)x(\d+)", args.texsize)
        if not match:
            parser.error("the texture size is WIDTHxHEIGHT, such as 2048x1024")
        texture_size = (int(match.group(1)), int(match.group(2)))

    report = build_report(args.model, texture_size, args.pairing, args.clash)
    with open(args.json, "w") as out:
        json.dump(report, out, indent=2)
    print("wrote %s (%d keys)" % (args.json, len(report)))
    if args.compare:
        with open(args.compare) as reference:
            return 1 if compare(report, json.load(reference), args.all) else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
