option(MESHLAB2_PLUGIN_FILTER_VORONOI "Build Voronoi sampling filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_VORONOI)
    message(STATUS "Skipping filter_voronoi - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterVoronoi STATIC
    voronoifilterplugin.h
    voronoifilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterVoronoi
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterVoronoi
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterVoronoi "filter_voronoi_descriptors"
    PREFIX "/filters/qmeshlab.filter.voronoi"
    FILES filters.json
)
