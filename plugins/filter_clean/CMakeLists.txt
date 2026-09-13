option(MESHLAB2_PLUGIN_FILTER_CLEAN "Build cleaning mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_CLEAN)
    message(STATUS "Skipping filter_clean - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterClean STATIC
    cleanfilterplugin.h
    cleanfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterClean
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterClean
    PUBLIC
        MeshLab2Core
        Qt6::Core
)


qt_add_resources(MeshLab2PluginFilterClean "filter_clean_descriptors"
    PREFIX "/filters/qmeshlab.filter.clean"
    FILES filters.json
)
