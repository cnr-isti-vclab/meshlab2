# Binding sources, shared by the two targets that compile them:
#  - MeshLab's `_meshlab2_bindings` static library (embedded console)
#  - pymeshlab's `_meshlab` nanobind module (standalone wheel)
set(MESHLAB2_PYTHON_BINDING_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/meshset_core.h
    ${CMAKE_CURRENT_LIST_DIR}/meshset_core.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pymesh.h
    ${CMAKE_CURRENT_LIST_DIR}/pymesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/module.cpp
)
