option(MESHLAB2_PLUGIN_OBJ_RAPIDOBJ "Build rapidobj OBJ input plugin" ON)

if (NOT MESHLAB2_PLUGIN_OBJ_RAPIDOBJ)
    message(STATUS "Skipping io_obj_rapidobj - plugin disabled")
    return()
endif()

set(_rapidobj_target "")
set(RAPIDOBJ_INCLUDE_DIR "")

find_package(RapidObj CONFIG QUIET)
if (TARGET rapidobj::rapidobj)
    set(_rapidobj_target rapidobj::rapidobj)
elseif(TARGET RapidObj::RapidObj)
    set(_rapidobj_target RapidObj::RapidObj)
endif()

if (NOT _rapidobj_target)
    set(_vcpkg_include_hints "")
    if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _vcpkg_include_hints
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
        )
    endif()
    if (DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _vcpkg_include_hints
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
            "${MESHLAB2_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
        )
    endif()

    if (_vcpkg_include_hints)
        find_path(RAPIDOBJ_INCLUDE_DIR rapidobj/rapidobj.hpp
            PATHS ${_vcpkg_include_hints}
            NO_DEFAULT_PATH
        )
    else()
        find_path(RAPIDOBJ_INCLUDE_DIR rapidobj/rapidobj.hpp)
    endif()
endif()

if (NOT _rapidobj_target AND NOT RAPIDOBJ_INCLUDE_DIR)
    message(FATAL_ERROR
        "io_obj_rapidobj requires rapidobj from vcpkg. "
        "Install dependency in manifest mode and configure with vcpkg toolchain.")
endif()

add_library(MeshLab2PluginIORapidObj STATIC
    rapidobjimportplugin.h
    rapidobjimportplugin.cpp
)

target_include_directories(MeshLab2PluginIORapidObj
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MESHLAB2_SOURCE_DIR}/vcglib
        ${MESHLAB2_SOURCE_DIR}/vcglib/eigenlib
)

if (RAPIDOBJ_INCLUDE_DIR)
    target_include_directories(MeshLab2PluginIORapidObj PUBLIC ${RAPIDOBJ_INCLUDE_DIR})
endif()

target_link_libraries(MeshLab2PluginIORapidObj
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

if (_rapidobj_target)
    target_link_libraries(MeshLab2PluginIORapidObj PUBLIC ${_rapidobj_target})
else()
    find_package(Threads QUIET)
    if (Threads_FOUND)
        target_link_libraries(MeshLab2PluginIORapidObj PUBLIC Threads::Threads)
    endif()
endif()
