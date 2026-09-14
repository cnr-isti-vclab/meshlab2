option(MESHLAB2_PLUGIN_FILTER_SCREENED_POISSON "Build screened Poisson reconstruction filter plugin" ON)

if (NOT MESHLAB2_PLUGIN_FILTER_SCREENED_POISSON)
    message(STATUS "Skipping filter_screened_poisson - plugin disabled")
    return()
endif()

find_package(OpenMP QUIET COMPONENTS CXX)
if (NOT OpenMP_CXX_FOUND AND APPLE)
    set(_openmp_root_hints "")
    if (DEFINED OpenMP_ROOT AND OpenMP_ROOT)
        list(APPEND _openmp_root_hints "${OpenMP_ROOT}")
    endif()
    if (DEFINED ENV{OpenMP_ROOT} AND NOT "$ENV{OpenMP_ROOT}" STREQUAL "")
        list(APPEND _openmp_root_hints "$ENV{OpenMP_ROOT}")
    endif()
    list(APPEND _openmp_root_hints /opt/homebrew/opt/libomp /usr/local/opt/libomp)

    find_path(_openmp_include_dir
        NAMES omp.h
        HINTS ${_openmp_root_hints}
        PATH_SUFFIXES include
    )
    find_library(_openmp_library
        NAMES omp libomp
        HINTS ${_openmp_root_hints}
        PATH_SUFFIXES lib
    )

    if (_openmp_include_dir AND _openmp_library)
        add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
        set_target_properties(OpenMP::OpenMP_CXX PROPERTIES
            INTERFACE_COMPILE_OPTIONS "-Xclang;-fopenmp"
            INTERFACE_INCLUDE_DIRECTORIES "${_openmp_include_dir}"
            INTERFACE_LINK_LIBRARIES "${_openmp_library}"
        )
        set(OpenMP_CXX_FOUND TRUE)
    endif()
endif()

add_library(MeshLab2PluginFilterScreenedPoisson STATIC
    screenedpoissonfilterplugin.h
    screenedpoissonfilterplugin.cpp
    poissonrecon_backend.h
    poissonrecon_backend.cpp
    poissonrecon_adapter.h
    poissonrecon_adapter.cpp
)

target_include_directories(MeshLab2PluginFilterScreenedPoisson
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(MeshLab2PluginFilterScreenedPoisson
    PUBLIC
        MeshLab2Core
        Qt6::Core
        Qt6::Gui
)

target_compile_definitions(MeshLab2PluginFilterScreenedPoisson
    PRIVATE
        BRUNO_LEVY_FIX
        FOR_RELEASE
)

if(MSVC)
    # The bundled upstream PoissonRecon templates generate very large objects.
    target_compile_options(MeshLab2PluginFilterScreenedPoisson PRIVATE /bigobj)
endif()

if (OpenMP_CXX_FOUND)
    target_link_libraries(MeshLab2PluginFilterScreenedPoisson PUBLIC OpenMP::OpenMP_CXX)
endif()

qt_add_resources(MeshLab2PluginFilterScreenedPoisson "filter_screened_poisson_descriptors"
    PREFIX "/filters/meshlab2.filter.screened_poisson"
    FILES filters.json
)
