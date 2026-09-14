option(MESHLAB2_PLUGIN_FILTER_IGL "Build libigl-based mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_IGL)
    message(STATUS "Skipping filter_igl - plugin disabled")
    return()
endif()

find_package(libigl CONFIG QUIET)

set(MESHLAB2_IGL_CORE_TARGET "")
foreach(candidate IN ITEMS igl::core igl::igl_core)
    if (TARGET ${candidate})
        set(MESHLAB2_IGL_CORE_TARGET ${candidate})
        break()
    endif()
endforeach()

# The booleans need libigl's copyleft/CGAL component. One dependency, one plugin, so
# filter_igl requires the union rather than building a partial filter set: vcpkg.json
# already requests libigl[cgal], so both targets travel together.
set(MESHLAB2_IGL_COPYLEFT_CGAL_TARGET "")
foreach(candidate IN ITEMS igl_copyleft::cgal igl_copyleft::igl_copyleft_cgal igl::copyleft_cgal igl_copyleft_cgal)
    if (TARGET ${candidate})
        set(MESHLAB2_IGL_COPYLEFT_CGAL_TARGET ${candidate})
        break()
    endif()
endforeach()

if (MESHLAB2_IGL_CORE_TARGET STREQUAL "" OR MESHLAB2_IGL_COPYLEFT_CGAL_TARGET STREQUAL "")
    message(STATUS "Skipping filter_igl - libigl core and/or copyleft CGAL target not found")
    return()
endif()

add_library(MeshLab2PluginFilterIgl STATIC
    iglfilterplugin.h iglfilterplugin.cpp
    iglbooleans.h iglbooleans.cpp
    iglparametrization.h iglparametrization.cpp
    iglqueries.h iglqueries.cpp
    iglquantities.h iglquantities.cpp
    libiglmeshadapter.h libiglmeshadapter.cpp
)

target_include_directories(MeshLab2PluginFilterIgl
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterIgl
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
        ${MESHLAB2_IGL_CORE_TARGET}
        ${MESHLAB2_IGL_COPYLEFT_CGAL_TARGET}
)

qt_add_resources(MeshLab2PluginFilterIgl "filter_igl_descriptors"
    PREFIX "/filters/meshlab2.filter.igl"
    FILES filters.json
)
