option(MESHLAB2_PLUGIN_FILTER_TEXTURE "Build texture filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_TEXTURE)
    message(STATUS "Skipping filter_texture - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterTexture STATIC
    texturefilterplugin.h
    texturefilterplugin.cpp
    rastering.h
    texture_packer.hpp
    texture_packer.cpp
)

target_include_directories(MeshLab2PluginFilterTexture
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterTexture
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterTexture "filter_texture_descriptors"
    PREFIX "/filters/meshlab2.filter.texture"
    FILES filters.json
)
