option(MESHLAB2_PLUGIN_FILTER_OPEN3D "Build Open3D-based scan processing plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_OPEN3D)
    message(STATUS "Skipping filter_open3d - plugin disabled")
    return()
endif()

find_package(Eigen3 CONFIG QUIET)
find_package(TBB CONFIG QUIET)
find_package(fmt CONFIG QUIET)
find_package(nanoflann CONFIG QUIET)
find_package(Qhull CONFIG QUIET)
find_package(jsoncpp CONFIG QUIET)

foreach(dep Eigen3::Eigen TBB::tbb fmt::fmt nanoflann::nanoflann Qhull::qhullcpp Qhull::qhull_r JsonCpp::JsonCpp)
    if (NOT TARGET ${dep})
        message(STATUS "Skipping filter_open3d - ${dep} not found")
        return()
    endif()
endforeach()

# The vendored subset of Open3D. Not the whole library: 51 of its files, the
# closure of the legacy geometry and registration entry points, with three
# include edges cut. The mapping table and the reasoning are in
# docs/design/proposals/open3d_scan_processing.md; the exact revision and the
# patches are in UPSTREAM.md beside this file.
file(GLOB_RECURSE MESHLAB2_OPEN3D_UPSTREAM_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream/open3d/*.cpp")

add_library(MeshLab2Open3DUpstream STATIC ${MESHLAB2_OPEN3D_UPSTREAM_SOURCES})

target_include_directories(MeshLab2Open3DUpstream
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream"
        # Open3D's own 3rdparty include root. Only Moller's triangle-triangle
        # test is vendored from it, for geometry/IntersectionTest.cpp.
        "${CMAKE_CURRENT_SOURCE_DIR}/upstream/3rdparty")

target_link_libraries(MeshLab2Open3DUpstream
    PUBLIC
        Eigen3::Eigen
        TBB::tbb
    PRIVATE
        fmt::fmt
        nanoflann::nanoflann
        Qhull::qhullcpp
        Qhull::qhull_r
        JsonCpp::JsonCpp
)

# OPEN3D_STATIC empties the dllimport/visibility macros in Macro.h, which is
# what a static library linked into the application wants.
target_compile_definitions(MeshLab2Open3DUpstream PUBLIC OPEN3D_STATIC)

# utility/CompilerInfo.cpp reports the toolchain that built the library and
# expects these from Open3D's own CMake, which we are not running.
target_compile_definitions(MeshLab2Open3DUpstream PRIVATE
    OPEN3D_CXX_STANDARD="${CMAKE_CXX_STANDARD}"
    OPEN3D_CXX_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    OPEN3D_CXX_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
)

add_library(MeshLab2PluginFilterOpen3D STATIC
    open3dfilterplugin.h open3dfilterplugin.cpp
    open3dmeshadapter.h open3dmeshadapter.cpp
    open3dpointcloud.h open3dpointcloud.cpp
    open3dreconstruction.h open3dreconstruction.cpp
    open3dregistration.h open3dregistration.cpp
)

target_include_directories(MeshLab2PluginFilterOpen3D
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterOpen3D
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
    PRIVATE
        MeshLab2Open3DUpstream
)

qt_add_resources(MeshLab2PluginFilterOpen3D "filter_open3d_descriptors"
    PREFIX "/filters/meshlab2.filter.open3d"
    FILES filters.json
)

# Upstream code, compiled as upstream wrote it: its warnings are not ours to
# fix, and a vendored tree that has to be re-patched on every update is a tree
# nobody updates.
if (NOT MSVC)
    target_compile_options(MeshLab2Open3DUpstream PRIVATE -w)
endif()
