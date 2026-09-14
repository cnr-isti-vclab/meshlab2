option(MESHLAB2_PLUGIN_FILTER_SELECT "Build selection mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_SELECT)
    message(STATUS "Skipping filter_select - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterSelect STATIC
    selectfilterplugin.h
    selectfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterSelect
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterSelect
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterSelect "filter_select_descriptors"
    PREFIX "/filters/meshlab2.filter.select"
    FILES filters.json
)
