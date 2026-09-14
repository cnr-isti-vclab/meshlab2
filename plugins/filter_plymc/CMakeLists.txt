option(MESHLAB2_PLUGIN_FILTER_PLYMC "Build PLYMC filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_PLYMC)
    message(STATUS "Skipping filter_plymc - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterPlyMC STATIC
    plymcfilterplugin.h
    plymcfilterplugin.cpp
    ${CMAKE_SOURCE_DIR}/vcglib/wrap/ply/plylib.cpp
)

target_include_directories(MeshLab2PluginFilterPlyMC
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vcglib
)

target_link_libraries(MeshLab2PluginFilterPlyMC
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

qt_add_resources(MeshLab2PluginFilterPlyMC "filter_plymc_descriptors"
    PREFIX "/filters/meshlab2.filter.plymc"
    FILES filters.json
)
