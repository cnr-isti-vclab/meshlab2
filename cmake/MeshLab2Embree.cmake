# Reusable Intel Embree (v4) + OpenMP detection, shared by MeshLab2Core (the
# viewpoint occlusion service) and the filter_embree plugin.
#
# Creates an INTERFACE target `MeshLab2Embree` that consumers can link
# unconditionally. When embree and OpenMP are found the target carries the
# include dirs, libraries and the compile definition MESHLAB2_EMBREE_ENABLED=1,
# and MESHLAB2_EMBREE_FOUND is set TRUE. Otherwise the target still exists but
# adds nothing, and embree-dependent code paths compile out (falling back to the
# vcglib grid ray caster).

if (TARGET MeshLab2Embree)
    return()
endif()

option(MESHLAB2_WITH_EMBREE "Use Intel Embree for ray casting when available" ON)

add_library(MeshLab2Embree INTERFACE)
set(MESHLAB2_EMBREE_FOUND FALSE CACHE INTERNAL "Embree (v4) + OpenMP available")

if (NOT MESHLAB2_WITH_EMBREE)
    message(STATUS "Embree support disabled (MESHLAB2_WITH_EMBREE=OFF); using vcglib grid ray caster")
    return()
endif()

# --- locate embree4 ---
set(_embree_target "")
find_package(embree CONFIG QUIET)
if (TARGET embree::embree)
    set(_embree_target embree::embree)
elseif(TARGET Embree::embree)
    set(_embree_target Embree::embree)
elseif(TARGET embree)
    set(_embree_target embree)
elseif(TARGET embree4)
    set(_embree_target embree4)
endif()

set(_embree_include "")
set(_embree_lib "")
if (NOT _embree_target)
    find_path(_embree_include embree4/rtcore.h)
    find_library(_embree_lib NAMES embree4 embree)
endif()

if (NOT _embree_target AND (NOT _embree_include OR NOT _embree_lib))
    message(STATUS "Embree (v4) not found; using vcglib grid ray caster")
    return()
endif()

# --- locate OpenMP (vcglib's EmbreeAdaptor.h includes <omp.h>) ---
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

    find_path(_openmp_include_dir NAMES omp.h HINTS ${_openmp_root_hints} PATH_SUFFIXES include)
    find_library(_openmp_library NAMES omp libomp HINTS ${_openmp_root_hints} PATH_SUFFIXES lib)
    if (_openmp_include_dir AND _openmp_library)
        add_library(OpenMP::OpenMP_CXX INTERFACE IMPORTED)
        set_target_properties(OpenMP::OpenMP_CXX PROPERTIES
            INTERFACE_COMPILE_OPTIONS "-Xclang;-fopenmp"
            INTERFACE_INCLUDE_DIRECTORIES "${_openmp_include_dir}"
            INTERFACE_LINK_LIBRARIES "${_openmp_library}")
        set(OpenMP_CXX_FOUND TRUE)
    endif()
endif()

if (NOT OpenMP_CXX_FOUND)
    message(STATUS "OpenMP not found; embree disabled (EmbreeAdaptor requires omp.h). Using vcglib grid ray caster")
    return()
endif()

# --- wire up the interface target ---
if (_embree_target)
    target_link_libraries(MeshLab2Embree INTERFACE ${_embree_target})
else()
    target_include_directories(MeshLab2Embree INTERFACE ${_embree_include})
    target_link_libraries(MeshLab2Embree INTERFACE ${_embree_lib})
endif()
target_link_libraries(MeshLab2Embree INTERFACE OpenMP::OpenMP_CXX)
target_compile_definitions(MeshLab2Embree INTERFACE MESHLAB2_EMBREE_ENABLED=1)
set(MESHLAB2_EMBREE_FOUND TRUE CACHE INTERNAL "Embree (v4) + OpenMP available")
message(STATUS "Embree (v4) enabled for ray casting")
