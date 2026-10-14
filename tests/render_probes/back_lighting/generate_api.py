# Back lighting of the fill: Single lights a surface only from the light, Double also from the opposite
# one, Fancy does so in two tones (bluish for the light, pinkish for the opposite one) so the side a
# surface shows can be read from its colour. Decided in the fill shaders, on the GPU: a probe, not a ctest.
#
# Run it with a real GPU and a real window server (QT_QPA_PLATFORM=offscreen gives no QRhi):
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/back_lighting
import json, os
import _meshlab as ml

BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "scene_background_top_color": [0, 0, 0, 255], "scene_background_bottom_color": [0, 0, 0, 255]}
W = H = 200
SINGLE, DOUBLE, FANCY = 0, 1, 2


def render(ms, back, distance, shading=0):
    """The sphere from `distance` along the view axis (inside it when smaller than its radius), white."""
    modes = [{"mesh_id": str(ms.mesh_id(0)),
              "settings": {"show_wire": False, "fill_back_lighting": back, "fill_backface_culling": False,
                           "fill_color": [255, 255, 255, 255],
                           "fill_plain": {"color_source": 0, "shading": shading}}}]
    tb = {"center": [0, 0, 0], "radius": 0.5, "distance": distance, "fov_y_degrees": 60.0}
    state = {"trackball": tb, "render_settings": BARE, "mesh_render_modes": modes}
    return bytes(ms.render_snapshot(json.dumps(state), W, H))


def mean_rgb(buf):
    px = [(buf[i], buf[i + 1], buf[i + 2]) for i in range(0, len(buf), 4) if buf[i] + buf[i + 1] + buf[i + 2] > 20]
    n = float(max(1, len(px)))
    return [sum(p[c] for p in px) / n for c in range(3)], len(px)


ok = True
def check(label, cond):
    global ok
    print("  [%s] %s" % ("PASS" if cond else "FAIL", label))
    ok = ok and cond


ms = ml.MeshSet()
ms.apply_filter("create_sphere")
ms.apply_filter("normalize_reference_frame")

# From inside the sphere every surface turns its back to the viewer and to a headlight.
single, _ = mean_rgb(render(ms, SINGLE, 0.1))
double, _ = mean_rgb(render(ms, DOUBLE, 0.1))
fancy, _ = mean_rgb(render(ms, FANCY, 0.1))
print("\n  inside: single %s  double %s  fancy %s\n" % (single, double, fancy))
check("single: the back of a surface is dark (ambient only)", sum(single) < 0.5 * sum(double))
check("double: lit from the other side", sum(double) > 1.5 * sum(single))
check("fancy: pinkish where only the opposite light reaches (red over blue, unlike double)",
      fancy[0] / fancy[2] > 1.15 * (double[0] / double[2]))

# From outside the surface faces the light, and fancy tints it the other way.
out_double, _ = mean_rgb(render(ms, DOUBLE, 1.8))
out_fancy, _ = mean_rgb(render(ms, FANCY, 1.8))
print("  outside: double %s  fancy %s\n" % (out_double, out_fancy))
check("fancy: bluish where the light reaches (blue over red)",
      out_fancy[2] / out_fancy[0] > 1.15 * (out_double[2] / out_double[0]))
single_out, _ = mean_rgb(render(ms, SINGLE, 1.8))
check("single and double agree on a surface facing the light",
      all(abs(a - b) < 6 for a, b in zip(single_out, out_double)))

# The same with flat shading, whose normal comes from the triangle rather than the vertices: the back of a
# surface must still be told from its front.
FLAT = 1
f_single, _ = mean_rgb(render(ms, SINGLE, 0.1, FLAT))
f_double, _ = mean_rgb(render(ms, DOUBLE, 0.1, FLAT))
f_fancy, _ = mean_rgb(render(ms, FANCY, 0.1, FLAT))
print("  flat, inside: single %s  double %s  fancy %s\n" % (f_single, f_double, f_fancy))
check("flat: single leaves the back of a surface dark", sum(f_single) < 0.5 * sum(f_double))
check("flat: fancy tints it pink", f_fancy[0] / f_fancy[2] > 1.15 * (f_double[0] / f_double[2]))
f_out_single, _ = mean_rgb(render(ms, SINGLE, 1.8, FLAT))
f_out_fancy, _ = mean_rgb(render(ms, FANCY, 1.8, FLAT))
check("flat: the front is lit under single and blue under fancy",
      sum(f_out_single) > 400 and f_out_fancy[2] > 1.15 * f_out_fancy[0])

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
