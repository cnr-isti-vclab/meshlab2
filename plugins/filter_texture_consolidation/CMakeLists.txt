option(MESHLAB2_PLUGIN_FILTER_TEXTURE_CONSOLIDATION
    "Build the TextureConsolidation raster-to-texture plugin" ON)

if(NOT MESHLAB2_PLUGIN_FILTER_TEXTURE_CONSOLIDATION)
    message(STATUS "Skipping filter_texture_consolidation - plugin disabled")
    return()
endif()

# The programs come from a pinned, unmodified submodule. It is skipped rather than fatal
# when absent, so that a checkout without it still configures.
set(TEXTURE_CONSOLIDATION_ROOT "${MESHLAB2_SOURCE_DIR}/external/texture-consolidation")
if(NOT EXISTS "${TEXTURE_CONSOLIDATION_ROOT}/extern/Misha/Misha/Geometry.h")
    message(STATUS
        "Skipping filter_texture_consolidation - submodule missing. Run: "
        "git submodule update --init --recursive external/texture-consolidation")
    return()
endif()

# Built as a separate project, like QuadWild: its C++20, its compile flags and any exit()
# or assertion stay in helper processes rather than in the application.
#
# Without OpenMP: the code parallelises through Misha's ThreadPool, which falls back to
# std::thread, and an OpenMP build would make the helpers depend on a runtime the bundle
# would then have to ship. Measured on the authors' vase example: 28 s without OpenMP
# against 27.5 s with it.
#
# libpng and libjpeg come from MeshLab's own vcpkg tree, passed as a prefix path rather
# than as a toolchain so that the helper build does not run a vcpkg install of its own.
include(ExternalProject)

set(TEXTURE_CONSOLIDATION_BUILD_DIR "${PROJECT_BINARY_DIR}/texture-consolidation")
if(CMAKE_CONFIGURATION_TYPES)
    set(TEXTURE_CONSOLIDATION_BIN_DIR "${TEXTURE_CONSOLIDATION_BUILD_DIR}/$<CONFIG>")
else()
    set(TEXTURE_CONSOLIDATION_BIN_DIR "${TEXTURE_CONSOLIDATION_BUILD_DIR}")
endif()
set(TEXTURE_CONSOLIDATION_EXECUTABLES
    "${TEXTURE_CONSOLIDATION_BIN_DIR}/BackProjection${CMAKE_EXECUTABLE_SUFFIX}"
    "${TEXTURE_CONSOLIDATION_BIN_DIR}/TextureFlow${CMAKE_EXECUTABLE_SUFFIX}"
    "${TEXTURE_CONSOLIDATION_BIN_DIR}/TextureDomainStitching${CMAKE_EXECUTABLE_SUFFIX}"
    CACHE INTERNAL "TextureConsolidation helper programs")

set(_texture_consolidation_cmake_args
    "-DTEXTURE_CONSOLIDATION_USE_OPENMP=OFF"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
)
if(CMAKE_BUILD_TYPE)
    list(APPEND _texture_consolidation_cmake_args "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}")
endif()
if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    list(APPEND _texture_consolidation_cmake_args
        "-DCMAKE_PREFIX_PATH=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
foreach(_forwarded CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET)
    if(${_forwarded})
        list(APPEND _texture_consolidation_cmake_args "-D${_forwarded}=${${_forwarded}}")
    endif()
endforeach()

set(_texture_consolidation_build_command
    "${CMAKE_COMMAND}" --build <BINARY_DIR>
    --target BackProjection TextureFlow TextureDomainStitching)
if(CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE)
    list(APPEND _texture_consolidation_build_command --config $<CONFIG>)
endif()

ExternalProject_Add(MeshLab2TextureConsolidationHelpers
    SOURCE_DIR "${TEXTURE_CONSOLIDATION_ROOT}"
    BINARY_DIR "${TEXTURE_CONSOLIDATION_BUILD_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    CMAKE_ARGS ${_texture_consolidation_cmake_args}
    BUILD_COMMAND ${_texture_consolidation_build_command}
    BUILD_BYPRODUCTS ${TEXTURE_CONSOLIDATION_EXECUTABLES}
    INSTALL_COMMAND ""
    # The submodule's sources are not tracked by ExternalProject's stamps, so without this
    # an edit there -- the fork's pending fixes are exactly that -- never reaches the
    # helpers. The inner build is incremental, so an up-to-date run costs a second.
    BUILD_ALWAYS TRUE
    USES_TERMINAL_BUILD TRUE
)

add_library(MeshLab2PluginFilterTextureConsolidation STATIC
    pinholecamera.h
    textureconsolidationfilterplugin.h
    textureconsolidationfilterplugin.cpp
)
add_dependencies(MeshLab2PluginFilterTextureConsolidation MeshLab2TextureConsolidationHelpers)
target_include_directories(MeshLab2PluginFilterTextureConsolidation PUBLIC
    ${MESHLAB2_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(MeshLab2PluginFilterTextureConsolidation PUBLIC
    MeshLab2Core
    Qt6::Core
    Qt6::Gui
)
# The build tree, as the last place the plugin looks for its helpers: what a developer
# running from the build directory has. Bundling them into the application is a
# separate step, not done yet.
target_compile_definitions(MeshLab2PluginFilterTextureConsolidation PRIVATE
    "MESHLAB2_TEXTURE_CONSOLIDATION_BUILD_DIR=\"${TEXTURE_CONSOLIDATION_BIN_DIR}\"")

qt_add_resources(MeshLab2PluginFilterTextureConsolidation "filter_texture_consolidation_descriptors"
    PREFIX "/filters/meshlab2.filter.texture_consolidation"
    FILES filters.json
)
