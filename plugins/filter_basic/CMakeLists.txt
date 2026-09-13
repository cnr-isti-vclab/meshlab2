option(MESHLAB2_PLUGIN_FILTER_BASIC "Build basic mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_BASIC)
    message(STATUS "Skipping filter_basic - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterBasic STATIC
    basicfilterplugin.h
    basicfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterBasic
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterBasic
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterBasic "filter_basic_descriptors"
    PREFIX "/filters/qmeshlab.filter.basic"
    FILES filters.json
)
