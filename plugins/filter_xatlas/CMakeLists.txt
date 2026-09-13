option(MESHLAB2_PLUGIN_FILTER_XATLAS "Build xatlas parametrization filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_XATLAS)
    message(STATUS "Skipping filter_xatlas - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterXAtlas STATIC
    xatlasfilterplugin.h
    xatlasfilterplugin.cpp
    upstream/xatlas.cpp
)

target_include_directories(MeshLab2PluginFilterXAtlas
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/upstream
)

target_link_libraries(MeshLab2PluginFilterXAtlas
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterXAtlas "filter_xatlas_descriptors"
    PREFIX "/filters/qmeshlab.filter.xatlas"
    FILES filters.json
)
