option(MESHLAB2_PLUGIN_FILTER_VERTEX_DISPLACEMENT "Build vertex displacement filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_VERTEX_DISPLACEMENT)
    message(STATUS "Skipping filter_vertex_displacement - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterVertexDisplacement STATIC
    vertexdisplacementfilterplugin.h
    vertexdisplacementfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterVertexDisplacement
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterVertexDisplacement
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterVertexDisplacement "filter_vertex_displacement_descriptors"
    PREFIX "/filters/qmeshlab.filter.vertex_displacement"
    FILES filters.json
)
