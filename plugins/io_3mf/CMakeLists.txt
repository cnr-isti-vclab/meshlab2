option(MESHLAB2_PLUGIN_3MF "Build 3MF import/export plugin" ON)

if (NOT MESHLAB2_PLUGIN_3MF)
    message(STATUS "Skipping io_3mf - plugin disabled")
    return()
endif()

find_package(lib3mf CONFIG QUIET)
if (NOT TARGET lib3mf::Cpp)
    message(STATUS "Skipping io_3mf - lib3mf not found")
    return()
endif()

add_library(MeshLab2PluginIO3MF STATIC
    threemfplugin.h
    threemfplugin.cpp
)
target_include_directories(MeshLab2PluginIO3MF PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(MeshLab2PluginIO3MF PUBLIC
    MeshLab2Core
    Qt6::Core
    Qt6::Gui
    lib3mf::Cpp
)
