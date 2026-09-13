option(MESHLAB2_PLUGIN_FILTER_UNSHARP "Build unsharp filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_UNSHARP)
    message(STATUS "Skipping filter_unsharp - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterUnsharp STATIC
    unsharpfilterplugin.h
    unsharpfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterUnsharp
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterUnsharp
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterUnsharp "filter_unsharp_descriptors"
    PREFIX "/filters/qmeshlab.filter.unsharp"
    FILES filters.json
)
