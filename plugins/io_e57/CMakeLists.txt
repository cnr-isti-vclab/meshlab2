option(MESHLAB2_PLUGIN_E57 "Build E57 input plugin" ON)

if (NOT MESHLAB2_PLUGIN_E57)
    message(STATUS "Skipping io_e57 - plugin disabled")
    return()
endif()

find_package(E57Format CONFIG QUIET)
set(_e57format_target "")
if (TARGET E57Format)
    set(_e57format_target E57Format)
elseif(TARGET E57Format::E57Format)
    set(_e57format_target E57Format::E57Format)
endif()

if (_e57format_target STREQUAL "")
    message(FATAL_ERROR
        "io_e57 requires libE57Format from vcpkg. "
        "Install dependency in manifest mode and configure with vcpkg toolchain.")
endif()

add_library(MeshLab2PluginIOE57 STATIC
    e57importplugin.h
    e57importplugin.cpp
)

target_include_directories(MeshLab2PluginIOE57
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MESHLAB2_SOURCE_DIR}/vcglib
        ${MESHLAB2_SOURCE_DIR}/vcglib/eigenlib
)

target_link_libraries(MeshLab2PluginIOE57
    PUBLIC
        MeshLab2Core
        Qt6::Core
        ${_e57format_target}
)
