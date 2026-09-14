# TrueForm is dual-licensed: PolyForm Noncommercial 1.0.0, or a commercial agreement with
# XLAB — neither GPL-compatible. MeshLab has explicit permission from the owners to
# include it, which is why this is ON by default. That permission covers this project and
# does not travel to third parties: redistributing a binary containing these components
# needs a separate agreement with XLAB. See plugins/io_trueform/UPSTREAM.md.
option(MESHLAB2_PLUGIN_IO_TRUEFORM "Build TrueForm OBJ/STL I/O plugin" ON)

if (NOT MESHLAB2_PLUGIN_IO_TRUEFORM)
    message(STATUS "Skipping io_trueform - plugin disabled")
    return()
endif()

set(_trueform_include "${CMAKE_SOURCE_DIR}/external/trueform/include")
if (NOT EXISTS "${_trueform_include}/trueform/io.hpp")
    message(STATUS
        "Skipping io_trueform - external/trueform not present. "
        "Run: git submodule update --init external/trueform")
    return()
endif()

find_package(TBB CONFIG QUIET)
if (NOT TARGET TBB::tbb)
    message(STATUS "Skipping io_trueform - oneTBB not found")
    return()
endif()

add_library(MeshLab2PluginIOTrueForm STATIC
    trueformioplugin.h
    trueformioplugin.cpp
)

target_include_directories(MeshLab2PluginIOTrueForm
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vcglib
        ${CMAKE_SOURCE_DIR}/vcglib/eigenlib
        ${_trueform_include}
)

target_link_libraries(MeshLab2PluginIOTrueForm
    PUBLIC
        MeshLab2Core
        Qt6::Core
        TBB::tbb
)

target_compile_features(MeshLab2PluginIOTrueForm PUBLIC cxx_std_17)
