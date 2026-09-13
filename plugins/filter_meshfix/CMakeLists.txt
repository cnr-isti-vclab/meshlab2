option(MESHLAB2_PLUGIN_FILTER_MESHFIX "Build MeshFix-based mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_MESHFIX)
    message(STATUS "Skipping filter_meshfix - plugin disabled")
    return()
endif()

set(MESHFIX_ROOT "${CMAKE_SOURCE_DIR}/external/meshfix")
if (NOT EXISTS "${MESHFIX_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "MeshFix submodule is missing. Run: git submodule update --init external/meshfix")
endif()

# MeshFix upstream exposes only an executable target. Build its untouched core
# source list here, deliberately excluding its final CLI source.
enable_language(C)
set(MESHFIX_CORE_SOURCES
    "${MESHFIX_ROOT}/src/TMesh/edge.cpp"
    "${MESHFIX_ROOT}/src/TMesh/vertex.cpp"
    "${MESHFIX_ROOT}/src/TMesh/io.cpp"
    "${MESHFIX_ROOT}/src/TMesh/triangle.cpp"
    "${MESHFIX_ROOT}/src/TMesh/tin.cpp"
    "${MESHFIX_ROOT}/src/Algorithms/detectIntersections.cpp"
    "${MESHFIX_ROOT}/src/Algorithms/holeFilling.cpp"
    "${MESHFIX_ROOT}/src/Algorithms/marchIntersections.cpp"
    "${MESHFIX_ROOT}/src/Algorithms/checkAndRepair.cpp"
    "${MESHFIX_ROOT}/src/Kernel/heap.cpp"
    "${MESHFIX_ROOT}/src/Kernel/matrix.cpp"
    "${MESHFIX_ROOT}/src/Kernel/orientation.c"
    "${MESHFIX_ROOT}/src/Kernel/list.cpp"
    "${MESHFIX_ROOT}/src/Kernel/coordinates.cpp"
    "${MESHFIX_ROOT}/src/Kernel/tmesh.cpp"
    "${MESHFIX_ROOT}/src/Kernel/graph.cpp"
    "${MESHFIX_ROOT}/src/Kernel/point.cpp"
    "${MESHFIX_ROOT}/src/Kernel/jqsort.cpp"
)

add_library(MeshLab2MeshFixCore STATIC ${MESHFIX_CORE_SOURCES})
target_include_directories(MeshLab2MeshFixCore PUBLIC
    "${MESHFIX_ROOT}/include/TMesh"
    "${MESHFIX_ROOT}/include/Kernel"
)
target_compile_features(MeshLab2MeshFixCore PUBLIC cxx_std_11)
if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(MeshLab2MeshFixCore PUBLIC IS64BITPLATFORM)
endif()

add_library(MeshLab2PluginFilterMeshFix STATIC
    meshfixfilterplugin.h
    meshfixfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterMeshFix
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterMeshFix
    PUBLIC
        MeshLab2Core
        MeshLab2MeshFixCore
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterMeshFix "filter_meshfix_descriptors"
    PREFIX "/filters/qmeshlab.filter.meshfix"
    FILES filters.json
)
