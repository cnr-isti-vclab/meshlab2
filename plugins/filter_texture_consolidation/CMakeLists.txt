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
# With OpenMP, as upstream builds it: the application already links and ships libomp for
# Screened Poisson, so the helpers add no runtime of their own.
#
# libpng and libjpeg come from MeshLab's own vcpkg tree, passed as a prefix path rather
# than as a toolchain so that the helper build does not run a vcpkg install of its own.
include(ExternalProject)

# Upstream's CMake requires OpenMP, and on Apple clang FindOpenMP finds a runtime only when
# pointed at one. So the helper build gets the root Screened Poisson's lookup also uses --
# an explicit OpenMP_ROOT, then Homebrew's libomp -- and without one the plugin is skipped
# rather than letting the helper build fail the whole build.
set(_texture_consolidation_openmp_root "")
if(APPLE)
    foreach(_candidate "${OpenMP_ROOT}" "$ENV{OpenMP_ROOT}" /opt/homebrew/opt/libomp /usr/local/opt/libomp)
        if(_candidate AND EXISTS "${_candidate}/include/omp.h")
            set(_texture_consolidation_openmp_root "${_candidate}")
            break()
        endif()
    endforeach()
    if(NOT _texture_consolidation_openmp_root)
        message(STATUS
            "Skipping filter_texture_consolidation - no OpenMP runtime found; install libomp "
            "or set OpenMP_ROOT")
        return()
    endif()
endif()

set(TEXTURE_CONSOLIDATION_BUILD_DIR "${PROJECT_BINARY_DIR}/texture-consolidation")
# Helpers kept by CI (MESHLAB2_HELPERS_CACHE_DIR in the root CMakeLists) replace the
# helper build below.
set(_texture_consolidation_cache_dir "${MESHLAB2_HELPERS_CACHE_DIR}/texture-consolidation")
set(_texture_consolidation_cached FALSE)
if(MESHLAB2_HELPERS_CACHE_DIR)
    set(_texture_consolidation_cached TRUE)
    foreach(_helper BackProjection TextureFlow TextureDomainStitching)
        if(NOT EXISTS "${_texture_consolidation_cache_dir}/${_helper}${CMAKE_EXECUTABLE_SUFFIX}")
            set(_texture_consolidation_cached FALSE)
        endif()
    endforeach()
endif()
if(_texture_consolidation_cached)
    set(TEXTURE_CONSOLIDATION_BIN_DIR "${_texture_consolidation_cache_dir}")
elseif(CMAKE_CONFIGURATION_TYPES)
    set(TEXTURE_CONSOLIDATION_BIN_DIR "${TEXTURE_CONSOLIDATION_BUILD_DIR}/$<CONFIG>")
else()
    set(TEXTURE_CONSOLIDATION_BIN_DIR "${TEXTURE_CONSOLIDATION_BUILD_DIR}")
endif()
set(TEXTURE_CONSOLIDATION_EXECUTABLES
    "${TEXTURE_CONSOLIDATION_BIN_DIR}/BackProjection${CMAKE_EXECUTABLE_SUFFIX}"
    "${TEXTURE_CONSOLIDATION_BIN_DIR}/TextureFlow${CMAKE_EXECUTABLE_SUFFIX}"
    "${TEXTURE_CONSOLIDATION_BIN_DIR}/TextureDomainStitching${CMAKE_EXECUTABLE_SUFFIX}"
    CACHE INTERNAL "TextureConsolidation helper programs")

# The helpers are built from four codebases, and every one's licence (BSD-3-Clause, and MIT
# for TextureSignalProcessing) requires its notice to accompany the binaries. The packages
# ship one LICENSE.txt beside the helpers, so it carries all four. Misha has no licence file:
# its notice is the comment every header opens with, the same terms under several copyright
# lines, so those lines are gathered and the terms taken once -- except Greg Turk's PLY code
# (PlyFile.h, PlyFile.inl), which carries Stanford's own, different notice, kept whole.
file(READ "${TEXTURE_CONSOLIDATION_ROOT}/LICENSE" _tc_notice)
file(READ "${TEXTURE_CONSOLIDATION_ROOT}/extern/PhongRodriguesVF/LICENSE" _phong_notice)
file(READ "${TEXTURE_CONSOLIDATION_ROOT}/extern/TextureSignalProcessing/LICENSE" _tsp_notice)
file(GLOB_RECURSE _misha_sources
    "${TEXTURE_CONSOLIDATION_ROOT}/extern/Misha/Misha/*.h"
    "${TEXTURE_CONSOLIDATION_ROOT}/extern/Misha/Misha/*.inl")
set(_misha_copyrights "")
foreach(_source IN LISTS _misha_sources)
    file(STRINGS "${_source}" _line REGEX "^Copyright \\(c\\)" LIMIT_COUNT 1)
    list(APPEND _misha_copyrights ${_line})
endforeach()
list(FILTER _misha_copyrights EXCLUDE REGEX "Stanford")
list(REMOVE_DUPLICATES _misha_copyrights)
list(SORT _misha_copyrights)
list(JOIN _misha_copyrights "\n" _misha_copyrights)
file(READ "${TEXTURE_CONSOLIDATION_ROOT}/extern/Misha/Misha/Geometry.h" _misha_header)
string(FIND "${_misha_header}" "Redistribution and use" _terms_begin)
string(FIND "${_misha_header}" "*/" _terms_end)
math(EXPR _terms_length "${_terms_end} - ${_terms_begin}")
string(SUBSTRING "${_misha_header}" ${_terms_begin} ${_terms_length} _misha_terms)
file(READ "${TEXTURE_CONSOLIDATION_ROOT}/extern/Misha/Misha/PlyFile.h" _ply_header)
string(FIND "${_ply_header}" "Copyright (c) 1994" _ply_begin)
string(SUBSTRING "${_ply_header}" ${_ply_begin} -1 _ply_notice)
string(FIND "${_ply_notice}" "*/" _ply_end)
string(SUBSTRING "${_ply_notice}" 0 ${_ply_end} _ply_notice)
set(TEXTURE_CONSOLIDATION_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt"
    CACHE INTERNAL "Licence notices of the TextureConsolidation helper programs")
file(CONFIGURE OUTPUT "${TEXTURE_CONSOLIDATION_LICENSE}" @ONLY CONTENT
"The TextureConsolidation helper programs (BackProjection, TextureFlow and
TextureDomainStitching) are built from the four codebases below.

=== TextureConsolidation (https://github.com/hliuw/TextureConsolidation) ===

${_tc_notice}
=== PhongRodriguesVF (https://github.com/mkazhdan/PhongRodriguesVF) ===

${_phong_notice}
=== TextureSignalProcessing (https://github.com/mkazhdan/TextureSignalProcessing) ===

${_tsp_notice}
=== Misha (https://github.com/mkazhdan/Misha) ===

${_misha_copyrights}
All rights reserved.

${_misha_terms}
=== Misha's PLY reader (Misha/PlyFile.h, PlyFile.inl), by Greg Turk ===

${_ply_notice}")

if(_texture_consolidation_cached)
    message(STATUS "filter_texture_consolidation: using the cached helpers in ${TEXTURE_CONSOLIDATION_BIN_DIR}")
else()
    # The helpers use MeshLab's own compiler. Upstream is C++20 and tested with GCC 13.3, which
    # is why the Linux release builds with gcc-toolset-13 rather than AlmaLinux 9's GCC 11.
    set(_texture_consolidation_cmake_args "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}")
    if(_texture_consolidation_openmp_root)
        list(APPEND _texture_consolidation_cmake_args "-DOpenMP_ROOT=${_texture_consolidation_openmp_root}")
    endif()
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
    if(MESHLAB2_HELPERS_CACHE_DIR)
        ExternalProject_Add_Step(MeshLab2TextureConsolidationHelpers fill_cache
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_texture_consolidation_cache_dir}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${TEXTURE_CONSOLIDATION_EXECUTABLES} "${_texture_consolidation_cache_dir}/"
            DEPENDEES build
        )
    endif()
endif()

add_library(MeshLab2PluginFilterTextureConsolidation STATIC
    pinholecamera.h
    textureconsolidationfilterplugin.h
    textureconsolidationfilterplugin.cpp
)
# No add_dependencies on the helpers: Ninja would pass it on to every target that links
# the plugin, holding back the plugin registry's and all the tests' compiles behind the
# stitcher (minutes, under GCC). The helpers are part of `all`, which every build and
# ctest run goes through; a single-target build that skips them makes the filter report
# that its programs are missing.
target_include_directories(MeshLab2PluginFilterTextureConsolidation PUBLIC
    ${MESHLAB2_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(MeshLab2PluginFilterTextureConsolidation PUBLIC
    MeshLab2Core
    Qt6::Core
    Qt6::Gui
)
# The build tree, as the last place the plugin looks for its helpers, after where
# meshlab2_bundle_texture_consolidation() below puts them.
target_compile_definitions(MeshLab2PluginFilterTextureConsolidation PRIVATE
    "MESHLAB2_TEXTURE_CONSOLIDATION_BUILD_DIR=\"${TEXTURE_CONSOLIDATION_BIN_DIR}\"")

qt_add_resources(MeshLab2PluginFilterTextureConsolidation "filter_texture_consolidation_descriptors"
    PREFIX "/filters/meshlab2.filter.texture_consolidation"
    FILES filters.json
)

# Called after the MeshLab executable target exists. Copies the helpers to where the plugin
# looks first (helperDirectory()): Contents/Helpers on macOS, texture-consolidation/bin
# beside the executable elsewhere -- with a single-config generator that directory is also
# the helpers' own build tree, so packaging copies bin/ and LICENSE.txt out of it, not the
# whole directory. A target of its own rather than a POST_BUILD step as for QuadWild: that
# would make the application depend on the helpers, and hold back its compiles behind the
# stitcher (see add_library above).
function(meshlab2_bundle_texture_consolidation target)
    if(APPLE)
        set(_helper_dir "$<TARGET_BUNDLE_DIR:${target}>/Contents/Helpers/texture-consolidation")
        set(_resource_dir "$<TARGET_BUNDLE_DIR:${target}>/Contents/Resources/texture-consolidation")
    else()
        set(_resource_dir "$<TARGET_FILE_DIR:${target}>/texture-consolidation")
        set(_helper_dir "${_resource_dir}/bin")
    endif()
    add_custom_target(MeshLab2TextureConsolidationBundle ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_helper_dir}" "${_resource_dir}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${TEXTURE_CONSOLIDATION_EXECUTABLES} "${_helper_dir}/"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${TEXTURE_CONSOLIDATION_LICENSE}" "${_resource_dir}/LICENSE.txt"
        COMMENT "Bundling the TextureConsolidation helpers"
        VERBATIM
    )
    add_dependencies(MeshLab2TextureConsolidationBundle ${target})
    if(TARGET MeshLab2TextureConsolidationHelpers)
        add_dependencies(MeshLab2TextureConsolidationBundle MeshLab2TextureConsolidationHelpers)
    endif()
endfunction()
