option(MESHLAB2_PLUGIN_FILTER_TRIOPTIMIZE "Build TriOptimize mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_TRIOPTIMIZE)
    message(STATUS "Skipping filter_trioptimize - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterTriOptimize STATIC
    curvdata.h
    curvedgeflip.h
    trioptimizefilterplugin.h
    trioptimizefilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterTriOptimize
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterTriOptimize
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterTriOptimize "filter_trioptimize_descriptors"
    PREFIX "/filters/meshlab2.filter.trioptimize"
    FILES filters.json
)
