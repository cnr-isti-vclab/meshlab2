option(MESHLAB2_PLUGIN_FILTER_TEXTURE_SIGNAL_PROCESSING
    "Build the TextureSignalProcessing texture filter plugin" ON)

if(NOT MESHLAB2_PLUGIN_FILTER_TEXTURE_SIGNAL_PROCESSING)
    message(STATUS "Skipping filter_texture_signal_processing - plugin disabled")
    return()
endif()

# TextureSignalProcessing is header-only, compiled in process from its own pinned submodule.
# Misha, the library it is written on, comes from TextureConsolidation's checkout: TSP's own
# copy under third_party/ does not compile with Apple clang, and the two plugins need the same
# Misha. Both are unmodified; skipped rather than fatal when absent, so that a checkout without
# them still configures. See UPSTREAM.md.
set(_tsp_root "${MESHLAB2_SOURCE_DIR}/external/TextureSignalProcessing")
set(_tsp_misha "${MESHLAB2_SOURCE_DIR}/external/texture-consolidation/extern/Misha")
if(NOT EXISTS "${_tsp_root}/include/Src/GradientDomain.h" OR NOT EXISTS "${_tsp_misha}/Misha/Geometry.h")
    message(STATUS
        "Skipping filter_texture_signal_processing - submodules missing. Run: "
        "git submodule update --init external/TextureSignalProcessing && "
        "git submodule update --init --recursive external/texture-consolidation")
    return()
endif()

add_library(MeshLab2PluginFilterTextureSignalProcessing STATIC
    texturesignalprocessingfilterplugin.h
    texturesignalprocessingfilterplugin.cpp
    tsp_adapter.h
    tsp_adapter.cpp
)

target_include_directories(MeshLab2PluginFilterTextureSignalProcessing
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# TSP and Misha are C++20, the rest of MeshLab C++17 (filter_bpa does the same).
target_compile_features(MeshLab2PluginFilterTextureSignalProcessing PRIVATE cxx_std_20)

# Only the adapter sees TSP and Misha. Without OpenMP, Misha's thread pool runs on std::thread.
set_source_files_properties(tsp_adapter.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${_tsp_root}/include;${_tsp_misha}")

target_link_libraries(MeshLab2PluginFilterTextureSignalProcessing
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

qt_add_resources(MeshLab2PluginFilterTextureSignalProcessing "filter_texture_signal_processing_descriptors"
    PREFIX "/filters/meshlab2.filter.texture_signal_processing"
    FILES filters.json
)
