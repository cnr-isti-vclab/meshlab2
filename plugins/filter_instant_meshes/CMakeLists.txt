option(MESHLAB2_PLUGIN_FILTER_INSTANT_MESHES "Build the Instant Meshes remeshing plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_INSTANT_MESHES)
    message(STATUS "Skipping filter_instant_meshes - plugin disabled")
    return()
endif()

find_package(Eigen3 CONFIG REQUIRED)
find_package(TBB CONFIG REQUIRED)

set(INSTANT_MESHES_SRC "${CMAKE_CURRENT_SOURCE_DIR}/upstream/src")
add_library(MeshLab2InstantMeshesCore STATIC
    "${INSTANT_MESHES_SRC}/adjacency.cpp"
    "${INSTANT_MESHES_SRC}/bvh.cpp"
    "${INSTANT_MESHES_SRC}/cleanup.cpp"
    "${INSTANT_MESHES_SRC}/dedge.cpp"
    "${INSTANT_MESHES_SRC}/extract.cpp"
    "${INSTANT_MESHES_SRC}/field.cpp"
    "${INSTANT_MESHES_SRC}/hierarchy.cpp"
    "${INSTANT_MESHES_SRC}/meshstats.cpp"
    "${INSTANT_MESHES_SRC}/normal.cpp"
    "${INSTANT_MESHES_SRC}/reorder.cpp"
    "${INSTANT_MESHES_SRC}/subdivide.cpp"
)
target_include_directories(MeshLab2InstantMeshesCore PUBLIC
    "${INSTANT_MESHES_SRC}"
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream/ext/dset"
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream/ext/pcg32"
)
target_link_libraries(MeshLab2InstantMeshesCore PUBLIC Eigen3::Eigen TBB::tbb)

# The adapter compiles against vcglib's bundled Eigen 3.4.0 (MeshLab2Core exports that
# include dir publicly), so the core must use the same one: otherwise the two halves of
# this plugin disagree on Eigen's internals and the matrices the adapter builds are
# reinterpreted with a different layout by the core.
target_include_directories(MeshLab2InstantMeshesCore BEFORE PUBLIC
    ${CMAKE_SOURCE_DIR}/vcglib/eigenlib)
target_compile_features(MeshLab2InstantMeshesCore PUBLIC cxx_std_17)
if (MSVC)
    # MSVC hides the non-standard M_PI constant used by the upstream sources
    # unless this is defined before any of the C/C++ math headers are included.
    target_compile_definitions(MeshLab2InstantMeshesCore PRIVATE _USE_MATH_DEFINES)
endif()
if (CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    target_compile_options(MeshLab2InstantMeshesCore PRIVATE
        -Wno-deprecated-declarations
        -Wno-unused-parameter
    )
endif()

add_library(MeshLab2PluginFilterInstantMeshes STATIC
    instantmeshes_adapter.h
    instantmeshes_adapter.cpp
    instantmeshesfilterplugin.h
    instantmeshesfilterplugin.cpp
)
target_include_directories(MeshLab2PluginFilterInstantMeshes PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(MeshLab2PluginFilterInstantMeshes PUBLIC
    MeshLab2Core
    MeshLab2InstantMeshesCore
    Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterInstantMeshes "filter_instant_meshes_descriptors"
    PREFIX "/filters/meshlab2.filter.instant_meshes"
    FILES filters.json
)
