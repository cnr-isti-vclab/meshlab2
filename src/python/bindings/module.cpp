#include "meshset_core.h"
#include "pymesh.h"

#if !defined(PYMESHLAB_STANDALONE)
#include "mlgui.h"
#endif

#include <QVector3D>
#include <QMetaType>

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

namespace nb = nanobind;

NB_MODULE(_meshlab, m)
{
    m.doc() = "MeshLab Python bindings — MeshSet wraps the MeshLab Document.";

    nb::class_<FilterInfoRecord>(m, "FilterInfo")
        .def_ro("key",                  &FilterInfoRecord::key)
        .def_ro("id",                   &FilterInfoRecord::id)
        .def_ro("plugin_id",            &FilterInfoRecord::plugin_id)
        .def_ro("plugin_name",          &FilterInfoRecord::plugin_name)
        .def_ro("name",                 &FilterInfoRecord::name)
        .def_ro("python_name",          &FilterInfoRecord::python_name)
        .def_ro("applicable",           &FilterInfoRecord::applicable)
        .def_ro("applicability_error",  &FilterInfoRecord::applicability_error)
        .def("__repr__", [](const FilterInfoRecord &f) {
            return "FilterInfo(python_name='" + f.python_name
                 + "', key='" + f.key
                 + "', name='" + f.name + "')";
        });

    nb::class_<FilterRunRecord>(m, "FilterRunResult")
        .def_ro("success",           &FilterRunRecord::success)
        .def_ro("document_modified", &FilterRunRecord::document_modified)
        .def_ro("error_message",     &FilterRunRecord::error_message)
        .def_ro("info_messages",     &FilterRunRecord::info_messages)
        .def_ro("new_mesh_indices",  &FilterRunRecord::new_mesh_indices)
        .def_prop_ro("output_values", [](const FilterRunRecord &r) {
            nb::dict d;
            for (auto it = r.output_values.cbegin(); it != r.output_values.cend(); ++it) {
                const QVariant &v = it.value();
                switch (v.typeId()) {
                case QMetaType::Int:
                case QMetaType::LongLong:
                    d[nb::str(it.key().toStdString().c_str())] = v.toLongLong();
                    break;
                case QMetaType::Double:
                    d[nb::str(it.key().toStdString().c_str())] = v.toDouble();
                    break;
                case QMetaType::Bool:
                    d[nb::str(it.key().toStdString().c_str())] = v.toBool();
                    break;
                case QMetaType::QString:
                    d[nb::str(it.key().toStdString().c_str())] = nb::str(v.toString().toStdString().c_str());
                    break;
                case QMetaType::Float:
                    d[nb::str(it.key().toStdString().c_str())] = v.toFloat();
                    break;
                default:
                    if (v.canConvert<QVector3D>()) {
                        QVector3D p = v.value<QVector3D>();
                        d[nb::str(it.key().toStdString().c_str())] = nb::make_tuple(p.x(), p.y(), p.z());
                    } else {
                        d[nb::str(it.key().toStdString().c_str())] = nb::none();
                    }
                    break;
                }
            }
            return d;
        });

    registerPyMesh(m);

    nb::class_<MeshSetCore>(m, "MeshSet")
        .def(nb::init<>())
        .def("__len__",            &MeshSetCore::meshCount)
        .def("mesh_number",        &MeshSetCore::meshCount)
        // Meshes are addressed by *index* (0-based position in the layer list, what
        // every operation here takes) or by *id* (a persistent opaque handle, minted
        // from 1 and never reused, which the render-state JSON keys mesh_render_modes
        // by). They are not interchangeable: the first mesh loaded has index 0 and id 1.
        .def("current_mesh",       &MeshSetCore::currentMesh,
             "The current Mesh object. Raises if the MeshSet has no current mesh.")
        .def("mesh",               &MeshSetCore::mesh,            nb::arg("index"),
             "Mesh at this 0-based position in the layer list.")
        .def("current_mesh_index", &MeshSetCore::currentMeshIndex,
             "0-based position of the current mesh in the layer list, or -1 if there "
             "is none.")
        .def("current_mesh_id",    &MeshSetCore::currentMeshId,
             "Persistent id of the current mesh -- NOT its index. Raises if the MeshSet "
             "has no current mesh. Render-state JSON wants this value as a string: "
             "{'mesh_id': str(ms.current_mesh_id()), 'settings': {...}}.")
        .def("mesh_id",            &MeshSetCore::meshId,          nb::arg("index"),
             "Persistent id of the mesh at this 0-based position, for building "
             "render-state JSON covering meshes other than the current one.")
        .def("set_current_mesh",   &MeshSetCore::setCurrentMesh,   nb::arg("index"),
             "Make the mesh at this 0-based position current.")
        .def("mesh_id_exists",     &MeshSetCore::meshIdExists,     nb::arg("id"),
             "Whether a mesh with this persistent id is still in the document. Takes an "
             "id, not an index: ids are never reused, so a stale one is simply False.")
        .def("set_current_mesh_visibility",
             &MeshSetCore::setCurrentMeshVisibility, nb::arg("visibility"))
        .def("set_mesh_visibility",
             &MeshSetCore::setMeshVisibility, nb::arg("index"), nb::arg("visibility"),
             "Show or hide the mesh at this 0-based position. Takes an index, matching "
             "the positional 'mesh_visibility' array in render-state JSON.")
        .def("is_current_mesh_visible", &MeshSetCore::isCurrentMeshVisible)
        .def("is_mesh_visible",         &MeshSetCore::isMeshVisible, nb::arg("index"),
             "Whether the mesh at this 0-based position is visible.")
        .def("load_new_mesh",      &MeshSetCore::loadNewMesh,       nb::arg("path"))
        .def("save_current_mesh",  &MeshSetCore::saveCurrentMesh,   nb::arg("path"))
        .def("raster_number",      &MeshSetCore::rasterCount)
        .def("current_raster",     &MeshSetCore::currentRasterIndex)
        .def("set_current_raster", &MeshSetCore::setCurrentRaster,  nb::arg("index"))
        .def("load_new_raster",    &MeshSetCore::loadRasterImage,   nb::arg("file_name"))
        .def("clear",              &MeshSetCore::clear)
        .def("load_project",       &MeshSetCore::loadProject,       nb::arg("file_name"))
        .def("save_project",       &MeshSetCore::saveProject,       nb::arg("file_name"))
        .def("filter_list",        &MeshSetCore::filterList)
        .def("list_filters",       &MeshSetCore::listFilters)
        .def("apply_filter",       &MeshSetCore::applyFilter,
             nb::arg("filter"), nb::arg("params") = nb::dict())
#if !defined(PYMESHLAB_STANDALONE)
        .def("render_snapshot",    &MeshSetCore::renderSnapshot,
             nb::arg("render_state_json"), nb::arg("width"), nb::arg("height"))
#endif
        ;

#if !defined(PYMESHLAB_STANDALONE)
    nb::class_<MlGui>(m, "MlGui")
        .def("camera_state_json",       &MlGui::cameraStateJson)
        .def("render_state_json",       &MlGui::renderStateJson)
        .def("apply_camera_state_json", [](MlGui &g, const std::string &json) -> bool {
            std::string err;
            return g.applyCameraStateJson(json, &err);
        }, nb::arg("json"))
        .def("apply_render_state_json", [](MlGui &g, const std::string &json) -> bool {
            std::string err;
            return g.applyRenderStateJson(json, &err);
        }, nb::arg("json"))
        .def("render_snapshot",         &MlGui::renderSnapshot,
             nb::arg("render_state_json"), nb::arg("width"), nb::arg("height"))
        .def("save_snapshot",           &MlGui::saveSnapshot,
             nb::arg("path"), nb::arg("width"), nb::arg("height"),
             nb::arg("render_state_json") = nb::str(""));
#endif
}
