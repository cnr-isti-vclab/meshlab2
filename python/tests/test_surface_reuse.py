"""Behavioral checks for the fixed-panel example, using the compiled filters."""
import importlib.util
from pathlib import Path

import numpy as np
import pytest
import pymeshlab2


pytestmark = pytest.mark.examples


@pytest.fixture
def reuse():
    path = Path(__file__).resolve().parents[1] / "examples" / "surface_reuse.py"
    spec = importlib.util.spec_from_file_location("surface_reuse", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_identical_surfaces_recover_panels_without_changing_inputs(reuse, tmp_path):
    ms = pymeshlab2.MeshSet()
    reuse._run(ms, "create_sphere", subdiv=3)
    reuse._run(ms, "duplicate_current_layer")
    before = [ms.mesh(i).vertex_matrix().copy() for i in range(2)]
    report = reuse.surface_reuse(ms, pieces=8, tolerance=0.01, trials=0,
                                 samples=500, output_dir=tmp_path / "result")
    assert report["accepted"]
    assert report["target_sample_coverage"] > 0.5
    source_end = 2 + report["source_pieces"]
    assert ms.mesh_number() == source_end + len(report["accepted"])
    assert all(not ms.is_mesh_visible(i) for i in range(2, source_end))
    assert all(ms.mesh(i).label().startswith("Source piece_") for i in range(2, source_end))
    assert all(ms.is_mesh_visible(i) for i in range(source_end, ms.mesh_number()))
    assert len({item["piece"] for item in report["accepted"]}) == len(report["accepted"])
    assert all(item["max_distance"] <= report["tolerance"] for item in report["accepted"])
    for i in range(2):
        np.testing.assert_array_equal(ms.mesh(i).vertex_matrix(), before[i])
    # Every source triangle belongs to exactly one output piece, including slivers.
    assert sum(ms.mesh(i).face_number() for i in range(2, source_end)) == ms.mesh(0).face_number()


def test_failed_distance_queries_are_not_good_fits(reuse):
    ms = pymeshlab2.MeshSet()
    reuse._run(ms, "create_sphere", subdiv=2)
    copy = reuse._run(ms, "duplicate_current_layer").new_mesh_indices[0]
    ms.set_current_mesh(copy)
    reuse._run(ms, "translate", axis=(10.0, 0.0, 0.0))
    distances = reuse._distances(ms, copy, 0, tolerance=0.01)
    assert len(distances) == ms.mesh(copy).vertex_number()
    assert np.all(distances > 0.01)


def test_selection_rewards_new_coverage_and_uses_each_piece_once(reuse):
    candidates = [
        dict(piece="a", rms=0.1, coverage_mask=np.array([1, 1, 0, 0], bool)),
        dict(piece="b", rms=0.01, coverage_mask=np.array([1, 0, 0, 0], bool)),
        dict(piece="a", rms=0.2, coverage_mask=np.array([0, 0, 1, 1], bool)),
        dict(piece="c", rms=0.3, coverage_mask=np.array([0, 0, 1, 0], bool)),
    ]
    chosen, covered = reuse._choose(candidates, 4, max_overlap=0.1, min_new_samples=1)
    assert [item["piece"] for item in chosen] == ["a", "c"]
    np.testing.assert_array_equal(covered, [True, True, True, False])
