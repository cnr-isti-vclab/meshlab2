option(MESHLAB2_PLUGIN_FILTER_BPA "Build the reference ball pivoting filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_BPA)
    message(STATUS "Skipping filter_bpa - plugin disabled")
    return()
endif()

find_package(glm CONFIG QUIET)

if (NOT TARGET glm::glm)
    message(STATUS "Skipping filter_bpa - glm not found")
    return()
endif()

add_library(MeshLab2PluginFilterBpa STATIC
    bpafilterplugin.h
    bpafilterplugin.cpp
    upstream/bpa.h
    upstream/bpa.cpp
)

target_include_directories(MeshLab2PluginFilterBpa
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# The vendored tree uses std::numbers::pi_v in one place, its only requirement above C++17.
# Asking for C++20 here keeps that file unpatched and touches nothing else.
target_compile_features(MeshLab2PluginFilterBpa PRIVATE cxx_std_20)

# The vendored tree reaches into glm's gtx headers -- length2 from gtx/norm.hpp, and
# gtx/io.hpp for its debug printing -- which glm gates behind this define.
target_compile_definitions(MeshLab2PluginFilterBpa PRIVATE GLM_ENABLE_EXPERIMENTAL)

target_link_libraries(MeshLab2PluginFilterBpa
    PUBLIC
        MeshLab2Core
        Qt6::Core
    PRIVATE
        glm::glm
)

qt_add_resources(MeshLab2PluginFilterBpa "filter_bpa_descriptors"
    PREFIX "/filters/meshlab2.filter.bpa"
    FILES filters.json
)
