# TrueForm is dual-licensed (PolyForm Noncommercial 1.0.0 / commercial agreement with
# XLAB) and QMeshLab has explicit permission from the owners to include it, which is why
# this is ON by default. That permission does not travel to third parties — see
# plugins/io_trueform/UPSTREAM.md.
option(MESHLAB2_PLUGIN_FILTER_TRUEFORM "Build TrueForm geometry filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_TRUEFORM)
    message(STATUS "Skipping filter_trueform - plugin disabled")
    return()
endif()

set(_trueform_include "${CMAKE_SOURCE_DIR}/external/trueform/include")
if (NOT EXISTS "${_trueform_include}/trueform/trueform.hpp")
    message(STATUS
        "Skipping filter_trueform - external/trueform not present. "
        "Run: git submodule update --init external/trueform")
    return()
endif()

find_package(TBB CONFIG QUIET)
if (NOT TARGET TBB::tbb)
    message(STATUS "Skipping filter_trueform - oneTBB not found")
    return()
endif()

add_library(MeshLab2PluginFilterTrueForm STATIC
    trueformfilterplugin.h
    trueformfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterTrueForm
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vcglib
        ${CMAKE_SOURCE_DIR}/vcglib/eigenlib
        ${_trueform_include}
)

target_link_libraries(MeshLab2PluginFilterTrueForm
    PUBLIC
        MeshLab2Core
        Qt6::Core
        TBB::tbb
)

target_compile_features(MeshLab2PluginFilterTrueForm PUBLIC cxx_std_17)
if (MSVC)
    # This template-heavy translation unit exceeds the section count supported
    # by MSVC's default object-file format.
    target_compile_options(MeshLab2PluginFilterTrueForm PRIVATE /bigobj)
endif()

qt_add_resources(MeshLab2PluginFilterTrueForm "filter_trueform_descriptors"
    PREFIX "/filters/qmeshlab.filter.trueform"
    FILES filters.json
)
