option(MESHLAB2_PLUGIN_FILTER_EXPRESSION "Build expression-based mesh filter plugin (muparser)" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_EXPRESSION)
    message(STATUS "Skipping filter_expression - plugin disabled")
    return()
endif()

set(_muparser_target "")
find_package(muparser CONFIG QUIET)
if (TARGET muparser::muparser)
    set(_muparser_target muparser::muparser)
elseif(TARGET muparser)
    set(_muparser_target muparser)
elseif(TARGET unofficial::muparser::muparser)
    set(_muparser_target unofficial::muparser::muparser)
endif()

set(MUPARSER_INCLUDE_DIR "")
set(MUPARSER_LIBRARY "")
if (NOT _muparser_target)
    find_path(MUPARSER_INCLUDE_DIR muParser.h)
    find_library(MUPARSER_LIBRARY NAMES muparser)
endif()

if (NOT _muparser_target AND (NOT MUPARSER_INCLUDE_DIR OR NOT MUPARSER_LIBRARY))
    # Skip, do not fail: every other dependency-gated plugin degrades this way, and a
    # missing optional dependency must never break configure for everyone else. This
    # was a FATAL_ERROR, so a build without vcpkg (the docs workflow) died here.
    message(STATUS "Skipping filter_expression - muparser not found")
    return()
endif()

add_library(MeshLab2PluginFilterExpression STATIC
    expressionfilterplugin.h
    expressionfilterplugin.cpp
    filter_refine.h
)

target_include_directories(MeshLab2PluginFilterExpression
    PUBLIC
        ${MESHLAB2_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if (MUPARSER_INCLUDE_DIR)
    target_include_directories(MeshLab2PluginFilterExpression PUBLIC ${MUPARSER_INCLUDE_DIR})
endif()

target_link_libraries(MeshLab2PluginFilterExpression
    PUBLIC
        MeshLab2Core
        Qt6::Core
)

if (MSVC)
    # Qt exports _UNICODE on Windows, but the vcpkg muParser port is built with
    # ENABLE_WIDE_CHAR=OFF. Keep the consuming headers on the library's narrow
    # string ABI as well.
    target_compile_options(MeshLab2PluginFilterExpression PRIVATE /U_UNICODE)
endif()

if (_muparser_target)
    target_link_libraries(MeshLab2PluginFilterExpression PUBLIC ${_muparser_target})
else()
    target_link_libraries(MeshLab2PluginFilterExpression PUBLIC ${MUPARSER_LIBRARY})
endif()


qt_add_resources(MeshLab2PluginFilterExpression "filter_expression_descriptors"
    PREFIX "/filters/meshlab2.filter.expression"
    FILES filters.json
)
