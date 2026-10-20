.. _first-steps:

Getting started
===============

Open the Python console in MeshLab (Window > Python Console). The predefined
``ms`` variable holds a :ref:`MeshSet <meshset-api>` object linked to the
current document. In the desktop app, ``mlgui`` gives access to the current
view state and snapshot helpers.

This page shows three small examples that are useful as first scripts.

Example 1 - Inspect the currently loaded meshes
-----------------------------------------------

This script prints basic information about the meshes that are already loaded
in the current MeshLab document.

.. code-block:: python

   print("Mesh count:", ms.mesh_number())

   if ms.mesh_number() == 0:
       print("No meshes are currently loaded.")
   else:
       print("Current mesh index:", ms.current_mesh_index())
       print("Current mesh id:", ms.current_mesh_id())
       print()
       print("Available filters (first 10):")
       for info in ms.list_filters()[:10]:
           print(" ", info.python_name, "-", info.name)

Example 2 - Create and process a noisy isosurface
-------------------------------------------------

This script creates a synthetic noisy isosurface, then applies marching-cubes
decimation and Loop subdivision.

.. code-block:: python

   result = ms.apply_filter("create_noisy_isosurface", {
       "resolution": 32,
   })

   if not result.success:
       raise RuntimeError(result.error_message)

   ms.meshing_decimation_edge_collapse_for_marching_cube_meshes(
       cellError=0.25,
       preserveBB=False,
   )

   ms.subdivision_loop(
       Iterations=1,
       LoopWeight="loop",
       Threshold=0.0,
   )

   print("Created and processed noisy isosurface.")

The exact parameter names and defaults of a filter can be copied directly from
the filter panel using the "Copy Python call to console" button.

Example 3 - Capture a snapshot of the current view
--------------------------------------------------

This example uses ``mlgui`` to save the current desktop view as a PNG image.

.. code-block:: python

   from pathlib import Path

   output_path = Path.home() / "Downloads" / "meshlab_snapshot.png"
   mlgui.save_snapshot(str(output_path), 1600, 1200)
   print(f"Saved {output_path}")

See also
--------

- :doc:`../python_scripting`
- :ref:`meshset-api`
- :ref:`mlgui-api`
