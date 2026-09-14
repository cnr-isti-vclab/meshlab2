option(MESHLAB2_PLUGIN_FILTER_IMG_PATCH_PARAM "Build image patch parametrization filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_IMG_PATCH_PARAM)
    message(STATUS "Skipping filter_img_patch_param - plugin disabled")
    return()
endif()

add_library(MeshLab2PluginFilterImgPatchParam STATIC
    imgpatchparamfilterplugin.h
    imgpatchparamfilterplugin.cpp
)

target_include_directories(MeshLab2PluginFilterImgPatchParam
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterImgPatchParam
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

qt_add_resources(MeshLab2PluginFilterImgPatchParam "filter_img_patch_param_descriptors"
    PREFIX "/filters/meshlab2.filter.img_patch_param"
    FILES filters.json
)
