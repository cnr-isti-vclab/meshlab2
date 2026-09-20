option(MESHLAB2_PLUGIN_FILTER_GEOGRAM "Build geogram-based mesh filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_GEOGRAM)
    message(STATUS "Skipping filter_geogram - plugin disabled")
    return()
endif()

find_package(Geogram CONFIG QUIET)

set(MESHLAB2_GEOGRAM_TARGET "")
foreach(candidate IN ITEMS Geogram::geogram geogram::geogram geogram)
    if (TARGET ${candidate})
        set(MESHLAB2_GEOGRAM_TARGET ${candidate})
        break()
    endif()
endforeach()

if (MESHLAB2_GEOGRAM_TARGET STREQUAL "")
    message(STATUS "Skipping filter_geogram - geogram target not found")
    return()
endif()

add_library(MeshLab2PluginFilterGeogram STATIC
    geogramfilterplugin.h geogramfilterplugin.cpp
    geogrambooleans.h geogrambooleans.cpp
    geogramparametrization.h geogramparametrization.cpp
    geogramatlas.h geogramatlas.cpp
    geogrammeshadapter.h geogrammeshadapter.cpp
)

target_include_directories(MeshLab2PluginFilterGeogram
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterGeogram
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
        ${MESHLAB2_GEOGRAM_TARGET}
)

qt_add_resources(MeshLab2PluginFilterGeogram "filter_geogram_descriptors"
    PREFIX "/filters/meshlab2.filter.geogram"
    FILES filters.json
)
