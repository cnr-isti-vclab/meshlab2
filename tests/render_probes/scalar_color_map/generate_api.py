# Scalar color mapping: the fill and point buffers carry the raw scalar, and the shaders map
# it through the view's range uniform (qualityMap: scale and offset for per-vertex values in
# xy, for per-face values in zw). Decided on the GPU, so a probe, not a ctest.
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

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
