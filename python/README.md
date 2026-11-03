# pymeshlab2

Python bindings for [MeshLab](https://github.com/cnr-isti-vclab/meshlab2): load
meshes and rasters into a `MeshSet` and run any of MeshLab's filters on them as
methods, without the application.

```python
import pymeshlab2

ms = pymeshlab2.MeshSet()
ms.load_new_mesh("bunny.ply")
print(len(ms.list_filters()), "filters")
```

The wheel is built from the same sources as the MeshLab application, so a
`pymeshlab2` release exposes the filters of the MeshLab release with the same
version. Scripts written in MeshLab's own Python console run here unchanged,
except for the `ms` and `mlgui` objects that only the application provides.

`pymeshlab2` is not the legacy [`pymeshlab`](https://pypi.org/project/pymeshlab/)
package: its API differs and it is not a drop-in replacement. The two can be
installed side by side.
