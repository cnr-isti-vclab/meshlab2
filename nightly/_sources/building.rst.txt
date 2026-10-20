.. _building:

Building the documentation
==========================

Prerequisites
-------------

* MeshLab compiled with ``MESHLAB2_PYTHON_CONSOLE=ON`` (default).
* Python packages from ``docs/requirements.txt``.

Step 1 — Generate the API source files
--------------------------------------

.. code-block:: bash

   cmake -S . -B build -G Ninja \
     -DMESHLAB2_REQUIRE_VCPKG_DEPS=OFF \
     -DMESHLAB2_MACOS_BUNDLE_ICON=OFF \
     -DMESHLAB2_PYTHON_CONSOLE=ON \
     -Dnanobind_DIR="$(python -m nanobind --cmake_dir)" \
     -DMESHLAB2_PLUGIN_OBJ_RAPIDOBJ=OFF \
     -DMESHLAB2_PLUGIN_E57=OFF \
     -DMESHLAB2_PLUGIN_GLTF=OFF \
     -DMESHLAB2_PLUGIN_FILTER_FUNC=OFF \
     -DMESHLAB2_PLUGIN_FILTER_EMBREE=OFF
   cmake --build build --target MeshLab2 -j8
   QT_QPA_PLATFORM=offscreen ./build/MeshLab --generate-docs docs

This introspects the ``_meshlab`` module from within the running app,
reads all ``filters.json`` files, and writes the reStructuredText and MyST
Markdown sources into ``docs/api/``.

The ``MESHLAB2_REQUIRE_VCPKG_DEPS=OFF`` option is used here because the
documentation build only needs the app binary for Python/API introspection and
the CI job does not activate the vcpkg toolchain. The listed plugin options
disable dependency-gated plugins that otherwise require vcpkg-provided
libraries. For full local documentation with every dependency-gated plugin
enabled, configure with the normal vcpkg toolchain instead.

Step 2 — Build the HTML site
-----------------------------

.. code-block:: bash

   pip install -r docs/requirements.txt
   sphinx-build docs docs_build
   # open docs_build/index.html

CI pipeline
-----------

The GitHub Action at ``.github/workflows/docs.yml`` automates both steps
on every push to ``main`` and deploys the result to GitHub Pages.
