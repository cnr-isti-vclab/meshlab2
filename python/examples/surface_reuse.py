"""Experimental fixed-panel surface reuse using MeshLab's existing filters.

In MeshLab, load a source (layer 0) and target (layer 1), then run this file.
Edit the call at the bottom for your units and layer indices. See
docs/design/surface_reuse.md for assumptions, costs, and limitations.
"""
from __future__ import annotations

import json
from pathlib import Path
import tempfile

import numpy as np
import pymeshlab2


def _run(meshset, name, **params):
    result = meshset.apply_filter(name, params)
    if not result.success:
        raise RuntimeError(f"{name}: {result.error_message}")
    return result


def _remove_after(meshset, count):
    while meshset.mesh_number() > count:
        meshset.set_current_mesh(meshset.mesh_number() - 1)
        _run(meshset, "remove_current_mesh_layer")


def _write_off(path, vertices, faces):
    # The Python API has array readers, but no array-based Mesh constructor yet.
    # OFF is a geometry-only bridge; this copies existing faces without remeshing.
    used, indices = np.unique(faces, return_inverse=True)
    with Path(path).open("w") as stream:
        stream.write(f"OFF\n{len(used)} {len(faces)} 0\n")
        np.savetxt(stream, vertices[used], fmt="%.17g")
        np.savetxt(stream, np.column_stack((np.full(len(faces), 3),
                                           indices.reshape(-1, 3))), fmt="%d")


def _snapshot(meshset, index, path):
    count, current = meshset.mesh_number(), meshset.current_mesh_index()
    try:
        meshset.set_current_mesh(index)
        copy = _run(meshset, "duplicate_current_layer").new_mesh_indices[0]
        meshset.set_current_mesh(copy)
        _run(meshset, "freeze_matrix")
        mesh = meshset.current_mesh()
        _write_off(path, mesh.vertex_matrix(), mesh.face_matrix())
    finally:
        _remove_after(meshset, count)
        meshset.set_current_mesh(current)


def _partition(source, directory, pieces, seed):
    work = pymeshlab2.MeshSet()
    work.load_new_mesh(str(source))
    components = _run(work, "split_into_connected_components").new_mesh_indices
    if len(components) != 1:
        raise ValueError("Use a single connected source surface (split it first).")
    _remove_after(work, 1)
    work.set_current_mesh(0)
    _run(work, "sample_surface_by_voronoi_relaxation", sampleNum=pieces,
         iterNum=5, preprocessFlag=False, randomSeed=seed)
    work.set_current_mesh(0)
    seeds_index = _run(work, "extract_selected_vertices",
                       DeleteOriginal=False).new_mesh_indices[0]
    seeds = work.mesh(seeds_index).vertex_matrix()
    vertices = work.mesh(0).vertex_matrix()
    faces = work.mesh(0).face_matrix()
    _remove_after(work, 1)
    # Reuse vcglib's geodesic solver; Python only compares the returned fields.
    # Assign each whole triangle by its mean corner distance. This is a discrete
    # Voronoi partition along existing edges, not exact isocurve cutting.
    nearest = np.full(len(faces), np.inf)
    labels = np.full(len(faces), -1, dtype=int)
    for i, point in enumerate(seeds):
        work.set_current_mesh(0)
        _run(work, "compute_geodesic_distance_from_point",
             startPoint=point.tolist(), maxDistance=0.0)
        distance = work.mesh(0).vertex_scalar_array()[faces].mean(axis=1)
        take = distance < nearest
        nearest[take], labels[take] = distance[take], i

    patches = []
    for i in range(len(seeds)):
        selected = faces[labels == i]
        if not len(selected):
            continue
        cell_path = directory / f"cell_{i:03d}.off"
        _write_off(cell_path, vertices, selected)
        # Discrete cells can have disconnected slivers. Keep each as its own
        # physical panel; the connected-component implementation stays in vcglib.
        cell = pymeshlab2.MeshSet()
        cell.load_new_mesh(str(cell_path))
        parts = _run(cell, "split_into_connected_components").new_mesh_indices
        for j, index in enumerate(parts):
            cell.set_current_mesh(index)
            path = directory / f"piece_{i:03d}_{j:02d}.off"
            cell.save_current_mesh(str(path))
            patches.append(path)
        cell_path.unlink()
    return patches


def _distances(work, measured, reference, tolerance):
    work.set_current_mesh(measured)
    # Unlike the Hausdorff filter, this retains out-of-range samples: the current
    # filter returns twice MaxDist for a miss, which must FAIL the tolerance test.
    _run(work, "compute_distance_from_reference_mesh", referenceMesh=reference,
         SignedDist=False, MaxDist=2.0 * tolerance)
    values = work.mesh(measured).vertex_scalar_array()
    if not len(values) or not np.isfinite(values).all() or (values < 0).any():
        raise RuntimeError("Distance filter returned invalid unsigned distances.")
    return values


def _sample(work, index, count, seed):
    work.set_current_mesh(index)
    return _run(work, "sample_surface_by_monte_carlo", SampleNum=count,
                Weighted=False, ExactNum=True, randomSeed=seed).new_mesh_indices[0]


def _choose(candidates, sample_count, max_overlap, min_new_samples):
    covered = np.zeros(sample_count, dtype=bool)
    used, chosen = set(), []
    while True:
        best, best_key = None, None
        for candidate in candidates:
            if candidate["piece"] in used:
                continue
            mask = candidate["coverage_mask"]
            size = int(mask.sum())
            gain = int((mask & ~covered).sum())
            if gain < min_new_samples or (size - gain) / max(size, 1) > max_overlap:
                continue
            key = (gain, -candidate["rms"])
            if best_key is None or key > best_key:
                best, best_key = candidate, key
        if best is None:
            return chosen, covered
        covered |= best["coverage_mask"]
        used.add(best["piece"])
        chosen.append(best)


def surface_reuse(meshset, source_index=0, target_index=1, *, pieces=24,
                  tolerance=None, trials=2, samples=2000, seed=17,
                  max_overlap=0.15, min_new_samples=5, output_dir=None):
    """Append accepted rigid panels and return a JSON-compatible report.

    tolerance is an absolute distance in the shared input units; None uses 0.5%
    of the target diagonal. trials counts FPFH/RANSAC restarts per source piece;
    an ICP-only trial from the input pose is also tried. No scaling is allowed.
    max_overlap bounds overlap on a common target sample set, not exact area.
    Each source piece is used at most once. Output files are retained for review.
    """
    if source_index == target_index:
        raise ValueError("Source and target must be different layers.")
    if not (0 <= source_index < meshset.mesh_number()
            and 0 <= target_index < meshset.mesh_number()):
        raise ValueError("Load source and target, then pass their layer indices.")
    if (pieces < 2 or trials < 0 or samples < 100 or min_new_samples < 1
            or not 0 <= max_overlap < 1 or not 0 < seed < 2**30):
        raise ValueError("Invalid pieces/trials/samples/overlap/seed settings.")
    for index in (source_index, target_index):
        if meshset.mesh(index).face_number() == 0:
            raise ValueError("Both inputs must be triangle surfaces.")
    required = {"duplicate_current_layer", "remove_current_mesh_layer", "freeze_matrix",
                "rename_current_mesh_layer", "extract_selected_vertices",
                "split_into_connected_components", "sample_surface_by_monte_carlo",
                "sample_surface_by_voronoi_relaxation",
                "compute_geodesic_distance_from_point",
                "align_layer_to_reference_by_icp_vcglib",
                "compute_distance_from_reference_mesh"}
    if trials:
        required.add("align_layer_to_reference_by_ransac_feature_matching_open3d")
        required.add("align_layer_to_reference_by_point_to_plane_icp_open3d")
    missing = required - {info.python_name for info in meshset.list_filters()}
    if missing:
        raise RuntimeError(f"This build is missing filters: {sorted(missing)}")

    directory = (Path(tempfile.mkdtemp(prefix="meshlab-surface-reuse-"))
                 if output_dir is None else Path(output_dir).expanduser().resolve())
    if output_dir is not None:
        directory.mkdir(parents=True, exist_ok=False)
    source, target = directory / "source.off", directory / "target.off"
    _snapshot(meshset, source_index, source)
    _snapshot(meshset, target_index, target)
    work = pymeshlab2.MeshSet()
    work.load_new_mesh(str(target))  # target is always 0
    diagonal = float(np.linalg.norm(np.ptp(work.mesh(0).vertex_matrix(), axis=0)))
    tolerance = 0.005 * diagonal if tolerance is None else float(tolerance)
    if not np.isfinite(tolerance) or tolerance <= 0:
        raise ValueError("tolerance must be a finite positive distance.")
    target_samples = _sample(work, 0, samples, seed)  # common area-uniform probes
    sample_count = work.mesh(target_samples).vertex_number()
    patches = _partition(source, directory, pieces, seed)
    candidates, attempts = [], []
    print(f"Surface reuse: {len(patches)} pieces; tolerance {tolerance:g}; {directory}")

    for number, patch in enumerate(patches):
        print(f"Fitting piece {number + 1}/{len(patches)}: {patch.name}", flush=True)
        for trial in range(trials + 1):
            record = {"piece": patch.name, "trial": trial}
            try:
                work.load_new_mesh(str(patch))
                moving = work.current_mesh_index()
                if work.mesh(moving).vertex_number() < 30:
                    record["rejected"] = "Fewer than 30 vertices; refine input or use fewer pieces."
                    attempts.append(record)
                    break
                extent = float(np.linalg.norm(np.ptp(work.mesh(moving).vertex_matrix(), axis=0)))
                random_seed = seed + number * (trials + 1) + trial
                if trial:
                    _run(work, "align_layer_to_reference_by_ransac_feature_matching_open3d",
                         referenceMesh=0, registrationVoxelSize=extent / 30,
                         featureRadius=extent / 8, maxCorrespondenceDistance=extent / 10,
                         mutualFilter=False, ransacIterations=20000, randomSeed=random_seed)
                    # Point-to-plane refinement helps curved panels slide into
                    # place; point-to-point ICP alone often stalls on smooth skin.
                    _run(work, "align_layer_to_reference_by_point_to_plane_icp_open3d",
                         referenceMesh=0, maxCorrespondenceDistance=extent / 5,
                         maxIterations=100)
                _run(work, "align_layer_to_reference_by_icp_vcglib", referenceMesh=0,
                     MinDistAbs=max(extent / 5, tolerance * 4), TrgDistAbs=tolerance / 10,
                     MaxIterNum=60, SampleNum=2000, MatchMode=True,
                     randomSeed=random_seed)
                _run(work, "freeze_matrix")
                vertex_max = float(_distances(work, moving, 0, tolerance).max())
                sampled = _sample(work, moving, samples, seed)
                distances = _distances(work, sampled, 0, tolerance)
                maximum = max(vertex_max, float(distances.max()))
                # Statistics for rejected trials can be capped at the search
                # radius's miss sentinel; accepted trials have uncensored values.
                record.update(max_distance=maximum,
                              rms=float(np.sqrt(np.mean(distances**2))))
                if maximum > tolerance:
                    record["rejected"] = "A vertex or surface sample exceeds tolerance."
                else:
                    coverage = _distances(work, target_samples, moving, tolerance) <= tolerance
                    record["covered_samples"] = int(coverage.sum())
                    placed = directory / f"placed_{number:03d}_{trial:02d}.off"
                    work.set_current_mesh(moving)
                    work.save_current_mesh(str(placed))
                    candidates.append(dict(record, placed=placed.name, coverage_mask=coverage))
                attempts.append(record)
            except RuntimeError as error:
                record["rejected"] = str(error)
                attempts.append(record)
            finally:
                _remove_after(work, 2)

    chosen, covered = _choose(candidates, sample_count, max_overlap, min_new_samples)
    accepted = [{k: v for k, v in candidate.items() if k != "coverage_mask"}
                for candidate in chosen]
    report = dict(tolerance=tolerance, source_pieces=len(patches),
                  accepted=accepted, target_sample_coverage=float(covered.mean()),
                  target_samples=sample_count, seed=seed, trials=trials,
                  max_overlap=max_overlap, min_new_samples=min_new_samples,
                  attempts=attempts, output_directory=str(directory))
    (directory / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    for candidate in accepted:
        meshset.load_new_mesh(str(directory / candidate["placed"]))
        _run(meshset, "rename_current_mesh_layer", newName=(
            f"Reuse {candidate['piece']} RMS={candidate['rms']:.4g}"))
    print(f"Accepted {len(accepted)}/{len(patches)} pieces; "
          f"sampled target coverage {covered.mean():.1%}. Report: {directory / 'report.json'}")
    return report


if __name__ == "__main__" and "ms" in globals():
    reuse_result = surface_reuse(ms, source_index=0, target_index=1,
                                 pieces=24, tolerance=None, trials=2)
