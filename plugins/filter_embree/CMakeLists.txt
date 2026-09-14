option(MESHLAB2_PLUGIN_FILTER_EMBREE "Build Embree-based mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_EMBREE)
    message(STATUS "Skipping filter_embree - plugin disabled")
    return()
endif()

# Embree + OpenMP detection is shared with MeshLab2Core (see cmake/MeshLab2Embree.cmake).
if (NOT MESHLAB2_EMBREE_FOUND)
    message(STATUS "Skipping filter_embree - embree (v4) / OpenMP not available")
    return()
endif()

add_library(MeshLab2PluginFilterEmbree STATIC
    embreefilterplugin.h
    embreefilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterEmbree
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterEmbree
    PUBLIC
        MeshLab2Core
        Qt6::Core
        MeshLab2Embree
)

qt_add_resources(MeshLab2PluginFilterEmbree "filter_embree_descriptors"
    PREFIX "/filters/meshlab2.filter.embree"
    FILES filters.json
)
