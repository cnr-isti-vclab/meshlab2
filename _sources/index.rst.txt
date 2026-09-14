QMeshLab Python API
=====================

QMeshLab is a mesh processing application with an embedded Python console.
This documentation covers the embedded Python environment and the public
``pymeshlab`` facade backed by the private ``_meshlab`` module.

For a practical overview of the in-app Python environment, see
`Python scripting <python_scripting.md>`_.

The documentation is shared on purpose: the core Python API is the
``pymeshlab.MeshSet`` interface and the filter set built on top of it. QMeshLab
adds desktop-specific conveniences such as the predefined live ``ms`` object and
the ``mlgui`` helper for view snapshots and render-state access.

.. toctree::
   :maxdepth: 2
   :caption: API Reference

   api/meshset
   api/mlgui
   api/filters
   api/types

.. toctree::
   :maxdepth: 1
   :caption: Examples

   examples/first_steps

.. toctree::
   :maxdepth: 1
   :caption: Development

   building

Design documents describing the internal architecture are available at
`docs/design/ <https://github.com/cnr-isti-vclab/meshlab2/tree/main/docs/design>`_.
