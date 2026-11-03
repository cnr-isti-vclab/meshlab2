# Python3 and nanobind are located by the root CMakeLists.txt before
# this subdirectory is processed.

include(${CMAKE_CURRENT_SOURCE_DIR}/bindings/sources.cmake)

# ---------------------------------------------------------------------------
# _meshlab — the extension module of the pymeshlab2 wheel, compiled from the
# same binding sources without the GUI-only types (PYMESHLAB_STANDALONE).
# pyproject.toml builds only this target and installs only the `pymeshlab2`
# component, so nothing in this block reaches an application build.
# ---------------------------------------------------------------------------
if(MESHLAB2_PYTHON_WHEEL)
    nanobind_add_module(_meshlab NB_STATIC ${MESHLAB2_PYTHON_BINDING_SOURCES})
    target_compile_definitions(_meshlab PRIVATE PYMESHLAB_STANDALONE=1)
    target_include_directories(_meshlab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/bindings)
    target_link_libraries(_meshlab PRIVATE MeshLab2Plugins)

    install(TARGETS _meshlab LIBRARY DESTINATION pymeshlab2 COMPONENT pymeshlab2)

    # The remesher runs as two helper executables; python/pymeshlab2/__init__.py
    # points MESHLAB2_QUADWILD_ROOT at this directory.
    if(TARGET MeshLab2PluginFilterQuadWild)
        add_dependencies(_meshlab MeshLab2QuadWildHelpers)
        set(_quadwild_root "${MESHLAB2_SOURCE_DIR}/external/quadwild-bimdf")
        install(PROGRAMS "${QUADWILD_EXECUTABLE}" "${QUAD_FROM_PATCHES_EXECUTABLE}"
            DESTINATION pymeshlab2/quadwild-bimdf/bin COMPONENT pymeshlab2)
        install(DIRECTORY "${_quadwild_root}/config"
            DESTINATION pymeshlab2/quadwild-bimdf COMPONENT pymeshlab2)
        install(FILES "${_quadwild_root}/LICENSE"
            DESTINATION pymeshlab2/quadwild-bimdf RENAME LICENSE.txt COMPONENT pymeshlab2)
    endif()

    # Loaded by leaf name at runtime, never linked, so wheel repair cannot find it
    # on Windows; see the root CMakeLists.txt on libgeogram_num_3rdparty.
    if(WIN32 AND TARGET MeshLab2PluginFilterGeogram)
        set(_geogram_num_dll
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/geogram_num_3rdparty.dll")
        if(NOT EXISTS "${_geogram_num_dll}")
            message(FATAL_ERROR "Geogram runtime library not found: ${_geogram_num_dll}")
        endif()
        install(FILES "${_geogram_num_dll}" DESTINATION pymeshlab2 COMPONENT pymeshlab2)
    endif()
endif()

# Everything below is the embedded console, linked into the application.
if(NOT MESHLAB2_PYTHON_CONSOLE)
    return()
endif()

# ---------------------------------------------------------------------------
# _meshlab2_bindings — static library containing the nanobind binding code.
# Because we embed the module via PyImport_AppendInittab (not dlopen), we
# build it as a regular STATIC library so the linker can pull in the
# PyInit__meshlab symbol.  We use nanobind_build_library to get the
# nanobind-static runtime (no shared libnanobind needed), then compile our
# binding sources against it.
# ---------------------------------------------------------------------------

# Build the nanobind-static runtime (idempotent — nanobind_build_library
# guards against building it twice).
nanobind_build_library(nanobind-static)

add_library(_meshlab2_bindings STATIC ${MESHLAB2_PYTHON_BINDING_SOURCES})

target_include_directories(_meshlab2_bindings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bindings
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(_meshlab2_bindings
    PUBLIC
        nanobind-static
        Python::Python
    PRIVATE
        MeshLab2Core
)

# The NB_MODULE macro uses NB_MODULE_EXPORT — make sure it resolves to nothing
# for a static build (symbol is always visible within the same binary).
target_compile_definitions(_meshlab2_bindings PRIVATE NB_STATIC)

# ---------------------------------------------------------------------------
# MeshLab2PythonHost  — Qt-based host library that manages the interpreter
# lifetime and exposes PythonHost to the rest of the application.
# ---------------------------------------------------------------------------
add_library(MeshLab2PythonHost STATIC
    PythonHost.h
    PythonHost.cpp
    mlgui.h
    mlgui.cpp
)

# Where this build's standard library lives, for PythonHost to fall back on when
# it is running from the build tree rather than from a bundle that ships one.
# The interpreter is asked for its prefix rather than deriving one from
# Python_STDLIB: the prefix is exactly what an interpreter home is, while the
# layout underneath it differs between platforms.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import sys; print(sys.base_prefix)"
    OUTPUT_VARIABLE MESHLAB2_PYTHON_HOME
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Backslashes would not survive being pasted into a C string literal.
file(TO_CMAKE_PATH "${MESHLAB2_PYTHON_HOME}" MESHLAB2_PYTHON_HOME)
target_compile_definitions(MeshLab2PythonHost PRIVATE
    MESHLAB2_PYTHON_HOME="${MESHLAB2_PYTHON_HOME}"
)

target_include_directories(MeshLab2PythonHost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/bindings
)

target_link_libraries(MeshLab2PythonHost
    PUBLIC
        Qt6::Core
        Python::Python
        nanobind-static
    PRIVATE
        MeshLab2Core
        Qt6::Widgets
)
