option(MESHLAB2_PLUGIN_FILTER_MESHING "Build meshing filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_MESHING)
    message(STATUS "Skipping filter_meshing - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterMeshing STATIC
    meshingfilterplugin.h
    meshingfilterplugin.cpp
)

# Temporary: line tables for this one translation unit, which holds the CoM template
# instantiation that segfaults under GCC (embedPolylineThenCutSplitsTheSurface). A whole-tree
# -g1 ran the CI build out of resources; one file does not. Remove once that is fixed.
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties(meshingfilterplugin.cpp PROPERTIES COMPILE_OPTIONS -g1)
endif()

target_include_directories(MeshLab2PluginFilterMeshing
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterMeshing
    PUBLIC
        MeshLab2Core
        Qt6::Core
)


qt_add_resources(MeshLab2PluginFilterMeshing "filter_meshing_descriptors"
    PREFIX "/filters/meshlab2.filter.meshing"
    FILES filters.json
)
