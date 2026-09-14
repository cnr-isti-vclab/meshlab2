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
        .def("current_mesh",       &MeshSetCore::currentMesh)
        .def("mesh",               &MeshSetCore::mesh,            nb::arg("index"))
        .def("current_mesh_id",    &MeshSetCore::currentMeshId)
        .def("set_current_mesh",   &MeshSetCore::setCurrentMesh,   nb::arg("index"))
        .def("mesh_id_exists",     &MeshSetCore::meshIdExists,     nb::arg("id"))
        .def("set_current_mesh_visibility",
             &MeshSetCore::setCurrentMeshVisibility, nb::arg("visibility"))
        .def("set_mesh_visibility",
             &MeshSetCore::setMeshVisibility, nb::arg("id"), nb::arg("visibility"))
        .def("is_current_mesh_visible", &MeshSetCore::isCurrentMeshVisible)
        .def("is_mesh_visible",         &MeshSetCore::isMeshVisible, nb::arg("id"))
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
