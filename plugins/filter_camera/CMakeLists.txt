option(MESHLAB2_PLUGIN_FILTER_CAMERA "Build camera filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_CAMERA)
    message(STATUS "Skipping filter_camera - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterCamera STATIC
    camerafilterplugin.h
    camerafilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterCamera
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterCamera
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

qt_add_resources(MeshLab2PluginFilterCamera "filter_camera_descriptors"
    PREFIX "/filters/meshlab2.filter.camera"
    FILES filters.json
)
