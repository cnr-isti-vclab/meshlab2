option(MESHLAB2_PLUGIN_FILTER_COLORPROC "Build colorproc filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_COLORPROC)
    message(STATUS "Skipping filter_colorproc - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterColorProc STATIC
    colorprocfilterplugin.h
    colorprocfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterColorProc
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterColorProc
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

qt_add_resources(MeshLab2PluginFilterColorProc "filter_colorproc_descriptors"
    PREFIX "/filters/meshlab2.filter.colorproc"
    FILES filters.json
)
