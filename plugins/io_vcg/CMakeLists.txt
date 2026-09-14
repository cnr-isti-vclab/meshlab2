add_library(MeshLab2PluginIOVCG STATIC
    vcgimportplugin.h
    vcgimportplugin.cpp
    ${CMAKE_SOURCE_DIR}/vcglib/wrap/ply/plylib.cpp
)

target_include_directories(MeshLab2PluginIOVCG
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/vcglib
        ${CMAKE_SOURCE_DIR}/vcglib/eigenlib
)

target_link_libraries(MeshLab2PluginIOVCG
    PUBLIC
        MeshLab2Core
        Qt6::Core
)
