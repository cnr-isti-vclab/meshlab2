option(MESHLAB2_PLUGIN_FILTER_CGAL "Build CGAL-based mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_CGAL)
    message(STATUS "Skipping filter_cgal - plugin disabled")
    return()
endif()

find_package(CGAL QUIET)

if (NOT TARGET CGAL::CGAL)
    message(STATUS "Skipping filter_cgal - CGAL not found")
    return()
endif()

add_library(MeshLab2PluginFilterCGAL STATIC
    cgalfilterplugin.h
    cgalfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterCGAL
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterCGAL
    PUBLIC
        MeshLab2Core
        Qt6::Core
        CGAL::CGAL
)

qt_add_resources(MeshLab2PluginFilterCGAL "filter_cgal_descriptors"
    PREFIX "/filters/qmeshlab.filter.cgal"
    FILES filters.json
)
