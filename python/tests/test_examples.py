from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

import pymeshlab2


pytestmark = pytest.mark.examples

EXAMPLES = Path(__file__).resolve().parents[1] / "examples"


def _example(name: str):
    # Loaded by path: putting python/ on sys.path would make `import pymeshlab2`
    # find the source package, which has no compiled module, instead of the wheel.
    spec = importlib.util.spec_from_file_location(name, EXAMPLES / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module  # dataclasses look their module up while it executes
    spec.loader.exec_module(module)
    return module


def test_list_filters_example():
    filters = _example("list_filters").main([])

    assert filters
    assert all(isinstance(item, pymeshlab2.FilterInfo) for item in filters)
    assert any(item.id == "measure_mesh_summary" for item in filters)


def test_mesh_info_example():
    result = _example("mesh_info").main([])

    assert isinstance(result, pymeshlab2.FilterRunResult)
    assert result.success is True
    assert result.info_messages


def test_normalize_and_save_example(tmp_path: Path):
    output = tmp_path / "normalized.ply"

    written = _example("normalize_and_save").main(["--output", str(output)])

    assert written == output
    assert output.exists()
    assert output.stat().st_size > 0


def test_create_noisy_isosurface_example():
    result = _example("create_noisy_isosurface").main(["--resolution", "8"])

    assert isinstance(result, pymeshlab2.FilterRunResult)
    assert result.success is True
    assert result.new_mesh_indices


def test_raster_basics_example():
    raster_number, current_raster = _example("raster_basics").main([])

    assert raster_number == 0
    assert current_raster == -1


def test_wavy_box_example(tmp_path: Path):
    output = tmp_path / "wavy_box.ply"
    written = _example("wavy_box").main([str(output)])

    assert written == output
    assert output.exists()
    assert output.stat().st_size > 0


def test_inventory_example(tmp_path: Path):
    data = Path(__file__).resolve().parents[2] / "tests/data"
    report = tmp_path / "inventory.md"
    module = _example("inventory")

    listed = module.main([str(data), "-o", str(report)])
    assert len(listed) == 5
    assert all(not entry.values for entry in listed)  # list mode never opens a mesh

    measured = module.main([str(data), "-o", str(report), "--geometry", "--topology"])
    assert all(not entry.error for entry in measured)
    simple = next(entry for entry in measured if entry.path == "simple.off")
    assert (simple.values["V"], simple.values["F"], simple.values["holes"]) == (3, 1, 1)
    assert "## Files" in report.read_text()

    with pytest.raises(SystemExit):  # the report may not land inside the scanned folder
        module.main([str(data), "-o", str(data / "inventory.md")])
