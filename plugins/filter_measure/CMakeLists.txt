option(MESHLAB2_PLUGIN_FILTER_MEASURE "Build measure filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_MEASURE)
    message(STATUS "Skipping filter_measure - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterMeasure STATIC
    measurefilterplugin.h
    measurefilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterMeasure
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterMeasure
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterMeasure "filter_measure_descriptors"
    PREFIX "/filters/meshlab2.filter.measure"
    FILES filters.json
)
