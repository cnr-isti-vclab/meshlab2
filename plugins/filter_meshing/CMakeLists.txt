option(MESHLAB2_PLUGIN_FILTER_MESHING "Build meshing filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_MESHING)
    message(STATUS "Skipping filter_meshing - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterMeshing STATIC
    meshingfilterplugin.h
    meshingfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterMeshing
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterMeshing
    PUBLIC
        MeshLab2Core
        Qt6::Core
)


qt_add_resources(MeshLab2PluginFilterMeshing "filter_meshing_descriptors"
    PREFIX "/filters/qmeshlab.filter.meshing"
    FILES filters.json
)
