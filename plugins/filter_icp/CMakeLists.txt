option(MESHLAB2_PLUGIN_FILTER_ICP "Build ICP alignment filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_ICP)
    message(STATUS "Skipping filter_icp - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterICP STATIC
    icpfilterplugin.h
    icpfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterICP
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterICP
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterICP "filter_icp_descriptors"
    PREFIX "/filters/meshlab2.filter.icp"
    FILES filters.json
)
