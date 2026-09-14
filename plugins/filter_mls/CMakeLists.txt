option(MESHLAB2_PLUGIN_FILTER_MLS "Build MLS filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_MLS)
    message(STATUS "Skipping filter_mls - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterMLS STATIC
    mlsfilterplugin.h
    mlsfilterplugin.cpp
    apss.cpp
    balltree.cpp
    rimls.cpp
)

target_include_directories(MeshLab2PluginFilterMLS
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterMLS
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterMLS "filter_mls_descriptors"
    PREFIX "/filters/meshlab2.filter.mls"
    FILES filters.json
)
