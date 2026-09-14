option(MESHLAB2_PLUGIN_FILTER_GEODESIC "Build geodesic filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_GEODESIC)
    message(STATUS "Skipping filter_geodesic - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterGeodesic STATIC
    geodesicfilterplugin.h
    geodesicfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterGeodesic
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterGeodesic
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterGeodesic "filter_geodesic_descriptors"
    PREFIX "/filters/meshlab2.filter.geodesic"
    FILES filters.json
)
