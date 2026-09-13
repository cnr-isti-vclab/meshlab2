option(MESHLAB2_PLUGIN_FILTER_LAYER "Build layer filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_LAYER)
    message(STATUS "Skipping filter_layer - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterLayer STATIC
    layerfilterplugin.h
    layerfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterLayer
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterLayer
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterLayer "filter_layer_descriptors"
    PREFIX "/filters/qmeshlab.filter.layer"
    FILES filters.json
)
