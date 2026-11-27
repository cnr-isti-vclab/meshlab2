option(MESHLAB2_PLUGIN_USD "Build USD (usda/usdc/usdz) import/export plugin" ON)

if (NOT MESHLAB2_PLUGIN_USD)
    message(STATUS "Skipping io_usd - plugin disabled")
    return()
endif()

set(_lightusd_dir ${MESHLAB2_SOURCE_DIR}/external/lightusd)
if (NOT EXISTS ${_lightusd_dir}/CMakeLists.txt)
    message(STATUS "Skipping io_usd - external/lightusd is missing "
                   "(git submodule update --init external/lightusd)")
    return()
endif()

# LightUSD is built as a subproject: only the library (its `next` core) and the render-scene
# converter Tydra Next. Its tools, tests, examples, C API, bundled importers (OBJ, FBX, VOX,
# volumes), audio and EXR are not needed to read and write USD meshes, and each one adds build
# time. Forced, because LightUSD declares them as options with other defaults.
foreach(_opt
        LIGHTUSD_WITH_AUDIO LIGHTUSD_WITH_USDOBJ LIGHTUSD_WITH_USDFBX LIGHTUSD_WITH_USDVOX
        LIGHTUSD_WITH_USDVOL LIGHTUSD_WITH_USD_TO_GLTF LIGHTUSD_WITH_EXR LIGHTUSD_WITH_TIFF
        LIGHTUSD_WITH_TEXTOOLS LIGHTUSD_USE_CCACHE LIGHTUSD_WITH_C_API LIGHTUSD_WITH_PXR_COMPAT_API
        LIGHTUSD_BUILD_TESTS LIGHTUSD_BUILD_EXAMPLES LIGHTUSD_BUILD_TOOLS
        LIGHTUSD_NEXT_BUILD_TOOLS LIGHTUSD_NEXT_BUILD_EXAMPLES LIGHTUSD_NEXT_BUILD_C_API_EXAMPLE)
    set(${_opt} OFF CACHE BOOL "Disabled by MeshLab's io_usd" FORCE)
endforeach()
set(LIGHTUSD_PRODUCTION_BUILD ON CACHE BOOL "Set by MeshLab's io_usd" FORCE)
set(LIGHTUSD_NATIVE_PRODUCT next CACHE STRING "Set by MeshLab's io_usd" FORCE)
add_subdirectory(${_lightusd_dir} ${CMAKE_CURRENT_BINARY_DIR}/lightusd EXCLUDE_FROM_ALL)

add_library(MeshLab2PluginIOUSD STATIC
    usdplugin.h
    usdplugin.cpp
)
target_include_directories(MeshLab2PluginIOUSD PUBLIC
    ${MESHLAB2_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_include_directories(MeshLab2PluginIOUSD PRIVATE ${_lightusd_dir}/src)
# LightUSD names members `slots`, which Qt's keyword macro would rewrite; the plugin uses
# no Qt keywords, and includes no header that does.
target_compile_definitions(MeshLab2PluginIOUSD PRIVATE QT_NO_KEYWORDS)
target_link_libraries(MeshLab2PluginIOUSD
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
    PRIVATE
        lightusd::next
        tydra_next
)
