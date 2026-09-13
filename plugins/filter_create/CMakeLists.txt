option(MESHLAB2_PLUGIN_FILTER_CREATE "Build mesh creation filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_CREATE)
    message(STATUS "Skipping filter_create - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterCreate STATIC
    createfilterplugin.h
    createfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterCreate
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterCreate
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterCreate "filter_create_descriptors"
    PREFIX "/filters/qmeshlab.filter.create"
    FILES filters.json
)
