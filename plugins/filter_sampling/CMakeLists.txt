option(MESHLAB2_PLUGIN_FILTER_SAMPLING "Build sampling filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_SAMPLING)
    message(STATUS "Skipping filter_sampling - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterSampling STATIC
    samplingfilterplugin.h
    samplingfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterSampling
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterSampling
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterSampling "filter_sampling_descriptors"
    PREFIX "/filters/meshlab2.filter.sampling"
    FILES filters.json
)
