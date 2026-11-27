# Scalar color mapping: the fill, point and edge buffers carry the raw scalar, and the shaders
# map it through the view's range uniform (qualityMap: scale and offset for per-vertex values
# in xy, for per-face values in zw; edgeQualityMap.xy for per-edge values). Decided on the
# GPU, so a probe, not a ctest.
#
# Run it with a real GPU and a real window server (QT_QPA_PLATFORM=offscreen gives no QRhi):
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/scalar_color_map
import json
import _meshlab as ml

BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "scene_background_top_color": [0, 0, 0, 255], "scene_background_bottom_color": [0, 0, 0, 255],
        "quality_histogram_colormap_id": "gray"}
W = H = 200
CONSTANT, PER_VERTEX_SCALAR, PER_FACE_SCALAR = 0, 3, 4
EDGE_CONSTANT, EDGE_PER_VERTEX_SCALAR, EDGE_PER_EDGE_SCALAR = 0, 3, 4
RED = [255, 0, 0, 255]


def render(fill_source=PER_VERTEX_SCALAR, points=False, **global_settings):
    """The unit sphere, unlit, its scalar through the gray map; non-scalar fill is red."""
    settings = {"show_wire": False, "fill_lighting": False, "point_lighting": False,
                "fill_color": RED, "point_color": RED,
                "fill_plain": {"color_source": fill_source, "shading": 0}}
    if points:
        settings.update({"show_fill": False, "show_points": True, "point_size": 6.0,
                         "point_color_source": 2})
    tb = {"center": [0, 0, 0], "radius": 0.5, "distance": 1.4, "fov_y_degrees": 60.0}
    state = {"trackball": tb, "render_settings": dict(BARE, **global_settings),
             "current_mesh_index": 0,
             "mesh_render_modes": [{"mesh_id": str(ms.mesh_id(0)), "settings": settings}]}
    return bytes(ms.render_snapshot(json.dumps(state), W, H))


def mean_rgb(buf, rows=(0, H)):
    px = [(buf[i], buf[i + 1], buf[i + 2])
          for r in range(rows[0], rows[1]) for i in range(r * W * 4, (r + 1) * W * 4, 4)
          if buf[i] + buf[i + 1] + buf[i + 2] > 20]
    n = float(max(1, len(px)))
    return [round(sum(p[c] for p in px) / n, 1) for c in range(3)]


def differing_pixels(a, b):
    return sum(1 for i in range(0, len(a), 4) if max(abs(a[i + c] - b[i + c]) for c in range(3)) > 2)


ok = True
def check(label, cond, detail=""):
    global ok
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", label, detail))
    ok = ok and cond


def near(rgb, value, tol=4):
    return all(abs(c - value) <= tol for c in rgb)


ms = ml.MeshSet()
ms.apply_filter("create_sphere")
ms.apply_filter("normalize_reference_frame")
fixed01 = {"quality_histogram_fixed_range": True, "quality_histogram_min": 0.0, "quality_histogram_max": 1.0}

print("scalar color map")
# Both halves of the uniform: a per-vertex value reads xy, a per-face value zw.
ms.apply_filter("compute_vertex_scalar_by_expression", {"q": "0.25"})
ms.apply_filter("compute_face_scalar_by_expression", {"q": "0.75"})
v = mean_rgb(render(PER_VERTEX_SCALAR, **fixed01))
f = mean_rgb(render(PER_FACE_SCALAR, **fixed01))
p = mean_rgb(render(points=True, **fixed01))
check("per-vertex 0.25 in [0, 1] is a quarter gray", near(v, 64), v)
check("per-face 0.75 in [0, 1] is three quarters gray", near(f, 191), f)
check("points: per-vertex 0.25 is a quarter gray", near(p, 64), p)

# A gradient: the automatic range of y on the sphere, and the same range set by hand.
ms.apply_filter("compute_vertex_scalar_by_expression", {"q": "y"})
auto = render()
manual = render(quality_histogram_fixed_range=True, quality_histogram_min=-0.5, quality_histogram_max=0.5)
check("the automatic range draws what the same fixed range draws",
      differing_pixels(auto, manual) == 0, differing_pixels(auto, manual))
top, bottom = mean_rgb(auto, (0, H // 4)), mean_rgb(auto, (3 * H // 4, H))
check("the gradient runs dark to light upwards", top[0] > bottom[0] + 100, (top, bottom))
upper = render(quality_histogram_fixed_range=True, quality_histogram_min=-0.5, quality_histogram_max=0.0)
check("a range clamped at 0 saturates the upper half to white",
      near(mean_rgb(upper, (0, H // 3)), 255, 2), mean_rgb(upper, (0, H // 3)))

# No data: a value that is not finite draws in the plain fill color, never on the map.
ms.apply_filter("compute_vertex_scalar_by_expression", {"q": "sqrt(-1)"})
ms.apply_filter("compute_face_scalar_by_expression", {"q": "sqrt(-1)"})
plain = render(CONSTANT)
check("non-finite per-vertex scalars draw as the fill color",
      differing_pixels(render(PER_VERTEX_SCALAR), plain) == 0, mean_rgb(render(PER_VERTEX_SCALAR)))
check("non-finite per-face scalars draw as the fill color",
      differing_pixels(render(PER_FACE_SCALAR), plain) == 0, mean_rgb(render(PER_FACE_SCALAR)))

# Edges: a circle cut from the sphere, seen face on, with the sphere itself hidden. Edges are
# drawn as quads (overlay_fat_edges); the line pipeline (overlay_edges) is only the fallback
# for a layer whose quad buffer could not be made, which a probe cannot arrange.
print("edges")
ms.apply_filter("create_polyline_from_planar_section", {"planeAxis": "z"})
line = ms.mesh_number() - 1
ms.set_current_mesh(line)
HIDDEN = {"show_fill": False, "show_wire": False, "show_points": False, "show_edges": False}


def render_edges(source, **global_settings):
    settings = {"show_fill": False, "show_wire": False, "show_points": False, "show_edges": True,
                "edge_size": 6.0, "edge_color": RED, "edge_color_source": source}
    tb = {"center": [0, 0, 0], "radius": 0.5, "distance": 1.4, "fov_y_degrees": 60.0}
    # The polyline is the current layer: no outline around it, which is drawn in its own color.
    state = {"trackball": tb,
             "render_settings": dict(BARE, highlight_current_mesh=False, **global_settings),
             "current_mesh_index": line,
             "mesh_render_modes": [{"mesh_id": str(ms.mesh_id(0)), "settings": HIDDEN},
                                   {"mesh_id": str(ms.mesh_id(line)), "settings": settings}]}
    return bytes(ms.render_snapshot(json.dumps(state), W, H))


def peak_rgb(buf):
    """The brightest pixel: a line's antialiased fringe would pull a mean towards black."""
    best = max(range(0, len(buf), 4), key=lambda i: buf[i] + buf[i + 1] + buf[i + 2])
    return [buf[best], buf[best + 1], buf[best + 2]]


check("the circle is drawn in the edge color", peak_rgb(render_edges(EDGE_CONSTANT)) == [255, 0, 0],
      peak_rgb(render_edges(EDGE_CONSTANT)))
ms.apply_filter("compute_vertex_scalar_by_expression", {"q": "0.25"})
ms.apply_filter("compute_edge_scalar_by_expression", {"q": "0.75"})
ve = peak_rgb(render_edges(EDGE_PER_VERTEX_SCALAR, **fixed01))
ee = peak_rgb(render_edges(EDGE_PER_EDGE_SCALAR, **fixed01))
check("per-vertex 0.25 on the edges is a quarter gray", near(ve, 64), ve)
check("per-edge 0.75 is three quarters gray", near(ee, 191), ee)

# Per-vertex values blend along each edge; a per-edge value is one color per edge.
ms.apply_filter("compute_vertex_scalar_by_expression", {"q": "y"})
ms.apply_filter("compute_edge_scalar_by_expression", {"q": "emy"})
for source, name in ((EDGE_PER_VERTEX_SCALAR, "per-vertex"), (EDGE_PER_EDGE_SCALAR, "per-edge")):
    img = render_edges(source)
    top, bottom = mean_rgb(img, (0, H // 4)), mean_rgb(img, (3 * H // 4, H))
    check("%s y on the circle runs dark to light upwards" % name, top[0] > bottom[0] + 100, (top, bottom))

ms.apply_filter("compute_vertex_scalar_by_expression", {"q": "sqrt(-1)"})
ms.apply_filter("compute_edge_scalar_by_expression", {"q": "sqrt(-1)"})
plain = render_edges(EDGE_CONSTANT)
for source, name in ((EDGE_PER_VERTEX_SCALAR, "per-vertex"), (EDGE_PER_EDGE_SCALAR, "per-edge")):
    img = render_edges(source)
    check("non-finite %s scalars draw edges in the edge color" % name,
          differing_pixels(img, plain) == 0, mean_rgb(img))

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
