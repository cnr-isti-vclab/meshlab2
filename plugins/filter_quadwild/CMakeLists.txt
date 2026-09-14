option(MESHLAB2_PLUGIN_FILTER_QUADWILD "Build the QuadWild-BiMDF remeshing plugin" ON)

if(NOT MESHLAB2_PLUGIN_FILTER_QUADWILD)
    message(STATUS "Skipping filter_quadwild - plugin disabled")
    return()
endif()

set(QUADWILD_ROOT "${CMAKE_SOURCE_DIR}/external/quadwild-bimdf")
if(NOT EXISTS "${QUADWILD_ROOT}/quadwild/CMakeLists.txt")
    message(FATAL_ERROR
        "QuadWild-BiMDF submodule is missing. Run: git submodule update --init --recursive external/quadwild-bimdf")
endif()

# Build upstream as a separate project. Besides preventing its many global CMake
# settings and target names from leaking into MeshLab, this keeps calls to exit()
# and assertions inside helper processes rather than the application.
include(FetchContent)
include(ExternalProject)

# Upstream pins this archive by hash but still downloads it over plain HTTP.
# Prefetching the same archive over HTTPS lets the pristine submodule find GMM
# immediately and avoids a fragile network fallback in its configure step.
FetchContent_Declare(meshlab2_quadwild_gmm
    URL https://download-mirror.savannah.gnu.org/releases/getfem/stable/gmm-5.4.2.tar.gz
    URL_HASH SHA224=8f4951901a55a1d1987d8199ed0e0b36ff2da26c870a4c3d55694a14
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(meshlab2_quadwild_gmm)

set(QUADWILD_BUILD_DIR "${CMAKE_BINARY_DIR}/quadwild-bimdf")
set(QUADWILD_BIN_DIR "${QUADWILD_BUILD_DIR}/Build/bin")
set(QUADWILD_EXECUTABLE "${QUADWILD_BIN_DIR}/quadwild${CMAKE_EXECUTABLE_SUFFIX}")
set(QUAD_FROM_PATCHES_EXECUTABLE
    "${QUADWILD_BIN_DIR}/quad_from_patches${CMAKE_EXECUTABLE_SUFFIX}")

set(_quadwild_cmake_args
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DGMM_INCLUDE_DIR=${meshlab2_quadwild_gmm_SOURCE_DIR}/include"
    "-DQUADRETOPOLOGY_WITH_GUROBI=OFF"
    "-DSATSUMA_ENABLE_BLOSSOM5=OFF"
)
if(CMAKE_OSX_ARCHITECTURES)
    list(APPEND _quadwild_cmake_args
        "-DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}")
endif()

set(_quadwild_build_command
    "${CMAKE_COMMAND}" --build <BINARY_DIR>
    --target quadwild quad_from_patches)
if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE)
    list(APPEND _quadwild_build_command --config $<CONFIG>)
endif()

ExternalProject_Add(MeshLab2QuadWildHelpers
    SOURCE_DIR "${QUADWILD_ROOT}"
    BINARY_DIR "${QUADWILD_BUILD_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_ARGS ${_quadwild_cmake_args}
    BUILD_COMMAND ${_quadwild_build_command}
    BUILD_BYPRODUCTS
        "${QUADWILD_EXECUTABLE}"
        "${QUAD_FROM_PATCHES_EXECUTABLE}"
    INSTALL_COMMAND ""
    USES_TERMINAL_BUILD TRUE
)

add_library(MeshLab2PluginFilterQuadWild STATIC
    quadwildfilterplugin.h
    quadwildfilterplugin.cpp
)
add_dependencies(MeshLab2PluginFilterQuadWild MeshLab2QuadWildHelpers)
target_include_directories(MeshLab2PluginFilterQuadWild PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(MeshLab2PluginFilterQuadWild PUBLIC
    MeshLab2Core
    Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterQuadWild "filter_quadwild_descriptors"
    PREFIX "/filters/meshlab2.filter.quadwild"
    FILES filters.json
)

# Called after the MeshLab executable target exists. The configuration files
# stay verbatim so updating the upstream submodule automatically updates them.
function(meshlab2_bundle_quadwild target)
    add_dependencies(${target} MeshLab2QuadWildHelpers)
    set(_quadwild_root "${CMAKE_SOURCE_DIR}/external/quadwild-bimdf")
    set(_quadwild_bin_dir "${CMAKE_BINARY_DIR}/quadwild-bimdf/Build/bin")
    set(_quadwild_executable
        "${_quadwild_bin_dir}/quadwild${CMAKE_EXECUTABLE_SUFFIX}")
    set(_quad_from_patches_executable
        "${_quadwild_bin_dir}/quad_from_patches${CMAKE_EXECUTABLE_SUFFIX}")
    if(APPLE)
        set(_helper_dir "$<TARGET_BUNDLE_DIR:${target}>/Contents/Helpers/quadwild-bimdf")
        set(_resource_dir "$<TARGET_BUNDLE_DIR:${target}>/Contents/Resources/quadwild-bimdf")
    else()
        set(_resource_dir "$<TARGET_FILE_DIR:${target}>/quadwild-bimdf")
        set(_helper_dir "${_resource_dir}/bin")
    endif()

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_helper_dir}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_resource_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_quadwild_executable}" "${_helper_dir}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_quad_from_patches_executable}" "${_helper_dir}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${_quadwild_root}/config" "${_resource_dir}/config"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_quadwild_root}/LICENSE" "${_resource_dir}/LICENSE.txt"
        COMMENT "Bundling QuadWild-BiMDF helpers and configuration"
        VERBATIM
    )
endfunction()
