option(MESHLAB2_PLUGIN_FILTER_QSLIM "Build the original QSlim simplification plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_QSLIM)
    message(STATUS "Skipping filter_qslim - plugin disabled")
    return()
endif()

set(QSLIM_ROOT "${CMAKE_SOURCE_DIR}/external/qslim")
if (NOT EXISTS "${QSLIM_ROOT}/mixkit/src/MxQSlim.h")
    message(FATAL_ERROR
        "QSlim submodule is missing. Run: git submodule update --init external/qslim")
endif()

# Build only the original, callable MixKit simplification core. The QSlim
# command-line application, legacy GUI, renderers, and file I/O are not needed.
add_library(MeshLab2QSlimCore STATIC
    "${QSLIM_ROOT}/mixkit/src/mixmsg.cxx"
    "${QSLIM_ROOT}/mixkit/src/MxBlockModel.cxx"
    "${QSLIM_ROOT}/mixkit/src/MxStdModel.cxx"
    "${QSLIM_ROOT}/mixkit/src/MxQMetric3.cxx"
    "${QSLIM_ROOT}/mixkit/src/MxQSlim.cxx"
    "${QSLIM_ROOT}/mixkit/src/MxStdSlim.cxx"
    "${QSLIM_ROOT}/mixkit/src/MxHeap.cxx"
    "${QSLIM_ROOT}/libgfx/src/mat2.cxx"
    "${QSLIM_ROOT}/libgfx/src/mat3.cxx"
    "${QSLIM_ROOT}/libgfx/src/mat4.cxx"
)
target_include_directories(MeshLab2QSlimCore PUBLIC
    "${QSLIM_ROOT}/mixkit/src"
    "${QSLIM_ROOT}/libgfx/include"
)
target_compile_definitions(MeshLab2QSlimCore PUBLIC HAVE_CONFIG_H)
target_compile_features(MeshLab2QSlimCore PUBLIC cxx_std_17)
if (CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang")
    target_compile_options(MeshLab2QSlimCore PRIVATE
        -Wno-deprecated-declarations
        -Wno-nontrivial-memcall
        -Wno-writable-strings
    )
endif()

add_library(MeshLab2PluginFilterQSlim STATIC
    qslimfilterplugin.h
    qslimfilterplugin.cpp
)
target_include_directories(MeshLab2PluginFilterQSlim PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(MeshLab2PluginFilterQSlim PUBLIC
    MeshLab2Core
    MeshLab2QSlimCore
    Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterQSlim "filter_qslim_descriptors"
    PREFIX "/filters/meshlab2.filter.qslim"
    FILES filters.json
)
