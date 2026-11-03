option(MESHLAB2_PLUGIN_GLTF "Build glTF/glb input plugin" ON)
option(MESHLAB2_PLUGIN_GLTF_DRACO "Enable Draco compression/decompression for glTF (requires vcpkg draco)" ON)

if (NOT MESHLAB2_PLUGIN_GLTF)
    message(STATUS "Skipping io_gltf - plugin disabled")
    return()
endif()

# vcpkg's tinygltf port is header-only and ships no CMake package; its usage notes
# prescribe find_path, which searches the vcpkg tree the toolchain was given.
find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h PATH_SUFFIXES tinygltf)
if (NOT TINYGLTF_INCLUDE_DIR)
    message(FATAL_ERROR
        "io_gltf requires tinygltf from vcpkg. "
        "Install dependency in manifest mode and configure with vcpkg toolchain.")
endif()

add_library(MeshLab2PluginIOGLTF STATIC
    gltfimportplugin.h
    gltfimportplugin.cpp
)

target_include_directories(MeshLab2PluginIOGLTF
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${MESHLAB2_SOURCE_DIR}/vcglib
        ${MESHLAB2_SOURCE_DIR}/vcglib/eigenlib
)

target_include_directories(MeshLab2PluginIOGLTF PUBLIC ${TINYGLTF_INCLUDE_DIR})

target_link_libraries(MeshLab2PluginIOGLTF
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

if (MESHLAB2_PLUGIN_GLTF_DRACO)
    set(_draco_target "")
    find_package(draco CONFIG QUIET)
    if (draco_FOUND)
        if (TARGET draco::draco)
            set(_draco_target draco::draco)
        elseif (TARGET draco)
            set(_draco_target draco)
        endif()
    endif()

    if (_draco_target STREQUAL "")
        message(FATAL_ERROR
            "io_gltf Draco support is enabled but draco package was not found from vcpkg.")
    endif()

    target_compile_definitions(MeshLab2PluginIOGLTF PRIVATE
        TINYGLTF_ENABLE_DRACO
        MESHLAB2_GLTF_HAS_DRACO
    )
    target_link_libraries(MeshLab2PluginIOGLTF PUBLIC ${_draco_target})
    message(STATUS "io_gltf: Draco support enabled (${_draco_target})")
endif()
