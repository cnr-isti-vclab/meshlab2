option(MESHLAB2_PLUGIN_GLTF "Build glTF/glb input plugin" ON)
option(MESHLAB2_PLUGIN_GLTF_DRACO "Enable Draco compression/decompression for glTF (requires vcpkg draco)" ON)

if (NOT MESHLAB2_PLUGIN_GLTF)
    message(STATUS "Skipping io_gltf - plugin disabled")
    return()
endif()

set(_tinygltf_target "")
set(TINYGLTF_INCLUDE_DIR "")
find_package(tinygltf CONFIG QUIET)
if (TARGET tinygltf::tinygltf)
    set(_tinygltf_target tinygltf::tinygltf)
elseif(TARGET tinygltf)
    set(_tinygltf_target tinygltf)
elseif(TARGET unofficial::tinygltf::tinygltf)
    set(_tinygltf_target unofficial::tinygltf::tinygltf)
endif()

if (NOT _tinygltf_target)
    # Some tinygltf package exports only include dir variables (no imported target).
    if (DEFINED TINYGLTF_INCLUDE_DIRS AND EXISTS "${TINYGLTF_INCLUDE_DIRS}/tiny_gltf.h")
        set(TINYGLTF_INCLUDE_DIR "${TINYGLTF_INCLUDE_DIRS}")
    elseif(DEFINED tinygltf_INCLUDE_DIRS AND EXISTS "${tinygltf_INCLUDE_DIRS}/tiny_gltf.h")
        set(TINYGLTF_INCLUDE_DIR "${tinygltf_INCLUDE_DIRS}")
    endif()

    set(_vcpkg_include_hints "")
    if (DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _vcpkg_include_hints
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include"
        )
    endif()
    if (DEFINED VCPKG_TARGET_TRIPLET)
        list(APPEND _vcpkg_include_hints
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
        )
    endif()

    if (NOT TINYGLTF_INCLUDE_DIR AND _vcpkg_include_hints)
        find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h
            PATHS ${_vcpkg_include_hints}
            PATH_SUFFIXES "" tinygltf
            NO_DEFAULT_PATH
        )
    elseif(NOT TINYGLTF_INCLUDE_DIR)
        find_path(TINYGLTF_INCLUDE_DIR tiny_gltf.h
            PATH_SUFFIXES "" tinygltf
        )
    endif()

    # Additional explicit fallback for vcpkg manifest layout variants.
    if (NOT TINYGLTF_INCLUDE_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        set(_tinygltf_manual_hints
            "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
            "${CMAKE_SOURCE_DIR}/build/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/include"
        )
        foreach(_hint IN LISTS _tinygltf_manual_hints)
            if (EXISTS "${_hint}/tiny_gltf.h")
                set(TINYGLTF_INCLUDE_DIR "${_hint}")
                break()
            endif()
            if (EXISTS "${_hint}/tinygltf/tiny_gltf.h")
                set(TINYGLTF_INCLUDE_DIR "${_hint}/tinygltf")
                break()
            endif()
        endforeach()
    endif()
endif()

if (NOT _tinygltf_target AND NOT TINYGLTF_INCLUDE_DIR)
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
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vcglib
        ${CMAKE_SOURCE_DIR}/vcglib/eigenlib
)

if (TINYGLTF_INCLUDE_DIR)
    target_include_directories(MeshLab2PluginIOGLTF PUBLIC ${TINYGLTF_INCLUDE_DIR})
endif()

target_link_libraries(MeshLab2PluginIOGLTF
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

if (_tinygltf_target)
    target_link_libraries(MeshLab2PluginIOGLTF PUBLIC ${_tinygltf_target})
endif()

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
