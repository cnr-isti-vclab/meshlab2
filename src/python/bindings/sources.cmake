# Binding sources, shared by the two targets that compile them:
#  - QMeshLab's `_qmeshlab_bindings` static library (embedded console)
#  - pymeshlab2's `_qmeshlab` nanobind module (standalone wheel)
set(QMESHLAB_PYTHON_BINDING_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/meshset_core.h
    ${CMAKE_CURRENT_LIST_DIR}/meshset_core.cpp
    ${CMAKE_CURRENT_LIST_DIR}/pymesh.h
    ${CMAKE_CURRENT_LIST_DIR}/pymesh.cpp
    ${CMAKE_CURRENT_LIST_DIR}/module.cpp
)
