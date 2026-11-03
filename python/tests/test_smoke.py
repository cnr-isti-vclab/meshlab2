import json
from pathlib import Path

import pytest

import pymeshlab2 as mod


ROOT = Path(__file__).resolve().parents[2]
SAMPLE_MESH = ROOT / "tests/data/simple.off"


@pytest.mark.smoke
def test_meshset_import_and_filter_listing():
    ms = mod.MeshSet()
    filters = ms.list_filters()

    assert len(filters) > 0
    mesh_info = next((f for f in filters if f.id == "measure_mesh_summary"), None)

    assert mesh_info is not None
    assert mesh_info.python_name == "measure_mesh_summary"


@pytest.mark.smoke
def test_meshlab2_filter_registry_is_linked():
    filters = mod.MeshSet().list_filters()
    filter_ids = {item.id for item in filters}
    plugin_ids = {item.plugin_id for item in filters}

    assert len(plugin_ids) >= 20
    assert {
        "create_hexahedron",
        "subdivide_by_midpoint",
        "remesh_isotropically_vcglib",
        "normalize_reference_frame",
        "select_all",
        "measure_geometric_properties",
    } <= filter_ids


@pytest.mark.smoke
def test_basic_filter_on_sample_mesh():
    ms = mod.MeshSet()
    ms.load_new_mesh(str(SAMPLE_MESH))
    result = ms.apply_filter("measure_mesh_summary", {"precision": 2})

    assert result.success is True
    assert result.document_modified is False
    assert len(result.info_messages) > 0


@pytest.mark.smoke
def test_raster_api_is_available():
    ms = mod.MeshSet()

    assert ms.raster_number() == 0
    assert ms.current_raster() == -1


@pytest.mark.smoke
def test_wheel_exposes_every_declared_filter():
    # The wheel is built from this tree with every plugin the application has, so
    # the two sets must match exactly: a filter missing here is a plugin that
    # dropped out of the wheel build, which is what a separate wheel repository
    # used to do silently.
    declared = {
        item["id"]
        for descriptor in (ROOT / "plugins").glob("*/filters.json")
        for item in json.loads(descriptor.read_text()).get("filters", [])
    }
    exposed = {item.id for item in mod.MeshSet().list_filters()}

    assert sorted(declared - exposed) == []
    assert sorted(exposed - declared) == []
