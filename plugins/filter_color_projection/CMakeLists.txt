option(MESHLAB2_PLUGIN_FILTER_COLOR_PROJECTION "Build color projection filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_COLOR_PROJECTION)
    message(STATUS "Skipping filter_color_projection - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterColorProjection STATIC
    colorprojectionfilterplugin.h
    colorprojectionfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterColorProjection
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterColorProjection
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

qt_add_resources(MeshLab2PluginFilterColorProjection "filter_color_projection_descriptors"
    PREFIX "/filters/meshlab2.filter.color_projection"
    FILES filters.json
)
