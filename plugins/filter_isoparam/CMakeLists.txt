option(MESHLAB2_PLUGIN_FILTER_ISOPARAM "Build the isoparametrization filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_ISOPARAM)
    message(STATUS "Skipping filter_isoparam - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterIsoParam STATIC
    atlaslayout.h
    isoparamfilterplugin.h
    isoparamfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterIsoParam
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        # The vendored headers include their siblings with <angled> includes.
        ${CMAKE_CURRENT_SOURCE_DIR}/upstream
)

target_link_libraries(MeshLab2PluginFilterIsoParam
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

qt_add_resources(MeshLab2PluginFilterIsoParam "filter_isoparam_descriptors"
    PREFIX "/filters/meshlab2.filter.isoparam"
    FILES filters.json
)
