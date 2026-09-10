# Python3 and nanobind are located by the root CMakeLists.txt before
# this subdirectory is processed.

# ---------------------------------------------------------------------------
# _qmeshlab_bindings — static library containing the nanobind binding code.
# Because we embed the module via PyImport_AppendInittab (not dlopen), we
# build it as a regular STATIC library so the linker can pull in the
# PyInit__qmeshlab symbol.  We use nanobind_build_library to get the
# nanobind-static runtime (no shared libnanobind needed), then compile our
# binding sources against it.
# ---------------------------------------------------------------------------

# Build the nanobind-static runtime (idempotent — nanobind_build_library
# guards against building it twice).
nanobind_build_library(nanobind-static)

include(${CMAKE_CURRENT_SOURCE_DIR}/bindings/sources.cmake)

add_library(_qmeshlab_bindings STATIC ${QMESHLAB_PYTHON_BINDING_SOURCES})

target_include_directories(_qmeshlab_bindings PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/bindings
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(_qmeshlab_bindings
    PUBLIC
        nanobind-static
        Python::Python
    PRIVATE
        QMeshLabCore
)

# The NB_MODULE macro uses NB_MODULE_EXPORT — make sure it resolves to nothing
# for a static build (symbol is always visible within the same binary).
target_compile_definitions(_qmeshlab_bindings PRIVATE NB_STATIC)

# ---------------------------------------------------------------------------
# QMeshLabPythonHost  — Qt-based host library that manages the interpreter
# lifetime and exposes PythonHost to the rest of the application.
# ---------------------------------------------------------------------------
add_library(QMeshLabPythonHost STATIC
    PythonHost.h
    PythonHost.cpp
    mlgui.h
    mlgui.cpp
)

target_include_directories(QMeshLabPythonHost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/bindings
)

target_link_libraries(QMeshLabPythonHost
    PUBLIC
        Qt6::Core
        Python::Python
        nanobind-static
    PRIVATE
        QMeshLabCore
        Qt6::Widgets
)
