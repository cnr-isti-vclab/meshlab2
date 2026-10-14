# Radiance scaling is an option of the plain fill material: the lighting is scaled by the surface's
# curvature in screen space. What it does is decided in the fill and points shaders and in a gradient pre-pass,
# on the GPU, so this is a probe, not a ctest.
#
# Run it with a real GPU and a real window server (QT_QPA_PLATFORM=offscreen gives no QRhi):
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/radiance_scaling
import json, os
import _meshlab as ml


def find_fixture(name):
    here = os.path.abspath(os.getcwd())
    while True:
        candidate = os.path.join(here, "tests", "sample_mesh", name)
        if os.path.exists(candidate):
            return candidate
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit("cannot find tests/sample_mesh/%s" % name)
        here = parent


BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "scene_background_top_color": [0, 0, 0, 255], "scene_background_bottom_color": [0, 0, 0, 255]}
W = H = 360


def render(ms, plain, material=0, trackball=None, size=None):
    """One render of every layer of `ms` with the given fill_plain settings (a dict)."""
    modes = [{"mesh_id": str(ms.mesh_id(i)),
              "settings": {"show_wire": False, "fill_material": material, "fill_plain": plain}}
             for i in range(ms.mesh_number())]
    tb = trackball or {"center": [0, 0, 0], "radius": 0.5, "distance": 1.8, "fov_y_degrees": 40.0}
    state = {"trackball": tb, "render_settings": BARE, "mesh_render_modes": modes}
    w, h = size or (W, H)
    return bytes(ms.render_snapshot(json.dumps(state), w, h))


def lit(buf):
    return [i for i in range(0, len(buf), 4) if buf[i] + buf[i + 1] + buf[i + 2] > 30]


def changed(a, b, tolerance=6):
    return sum(1 for i in range(0, len(a), 4)
               if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > tolerance)


ok = True
def check(label, cond):
    global ok
    print("  [%s] %s" % ("PASS" if cond else "FAIL", label))
    ok = ok and cond


RS = {"radiance_scaling": True, "rs_enhancement": 0.5}
ms = ml.MeshSet()
ms.load_new_mesh(find_fixture("gargoyle_small/gargoyle.ply"))
ms.apply_filter("normalize_reference_frame")   # centred, longest side 1, so the default camera frames it

off = render(ms, {})
on = render(ms, RS)
print("\n  pixels changed by radiance scaling on the gargoyle: %d of %d\n" % (changed(off, on), W * H))
check("the mesh is on screen", len(lit(off)) > 5000)
check("radiance scaling changes the lighting", changed(off, on) > 2000)
check("and only where there is mesh", changed(off, on) <= len(lit(off)) + len(lit(on)))

check("a zero gain is no scaling at all",
      changed(off, render(ms, {"radiance_scaling": True, "rs_enhancement": 0.0}), 3) < 300)

# It works with any colour source and flat shading: the option scales the lighting, not the colour.
flat_off = render(ms, {"shading": 1})
flat_on = render(ms, {"shading": 1, "radiance_scaling": True, "rs_enhancement": 0.5})
check("flat shading takes it too", changed(flat_off, flat_on) > 2000)

# Hue: radiance scaling multiplies by one number per pixel, so a pixel's colour keeps its proportions.
ms.clear()
ms.load_new_mesh(find_fixture("sphere_40kv.ply"))
tinted_off = render(ms, {"color_source": 0})
ms.clear()
ms.load_new_mesh(find_fixture("sphere_1.2kv_VC.ply"))
vc_off = render(ms, {"color_source": 1})
vc_on = render(ms, {"color_source": 1, "radiance_scaling": True, "rs_enhancement": 0.5})
worst = 0.0
for i in range(0, len(vc_off), 4):
    r0, g0, b0 = vc_off[i], vc_off[i + 1], vc_off[i + 2]
    r1, g1, b1 = vc_on[i], vc_on[i + 1], vc_on[i + 2]
    if min(r0 + g0 + b0, r1 + g1 + b1) > 150:
        # chroma as the share of each channel in the pixel
        for a, b, s0, s1 in ((r0, r1, r0 + g0 + b0, r1 + g1 + b1), (g0, g1, r0 + g0 + b0, r1 + g1 + b1)):
            worst = max(worst, abs(a / float(s0) - b / float(s1)))
print("  worst change of a pixel's colour proportions with vertex colours: %.3f" % worst)
check("the colour source survives: proportions of the colour stay", worst < 0.08)

# Flat shading: the curvature is the shading's own, so on a faceted solid it is zero inside a face and
# high along the convex edges, which is how the edges get marked. If the gradient pass used smooth normals
# for a flat-shaded layer the curvature would instead be spread over every face, and whiten it towards the
# silhouette, where the normal turns away.
ms.clear()
ms.apply_filter("create_icosahedron")
ms.apply_filter("normalize_reference_frame")
ico = {"center": [0, 0, 0], "radius": 0.5, "distance": 1.8, "fov_y_degrees": 40.0}
ico_off = render(ms, {"shading": 1}, trackball=ico)
ico_on = render(ms, {"shading": 1, "radiance_scaling": True, "rs_enhancement": 1.0}, trackball=ico)
solid = len(lit(ico_off))
marked = changed(ico_off, ico_on, 10)
print("  flat icosahedron: %d pixels of mesh, %d changed by radiance scaling (%.1f%%)\n"
      % (solid, marked, 100.0 * marked / max(1, solid)))
check("flat shading: the edges are marked", marked > 0.01 * solid)
check("flat shading: the faces themselves are left alone (curvature is zero inside a face)",
      marked < 0.2 * solid)

# Point clouds: the same switch scales the lighting of a layer's points, read from a gradient buffer the
# points' own normals are drawn into. Here the layer has no fill at all, so the buffer exists for the
# points alone.
def render_points(ms, plain, lighting=True, size=6, trackball=None, material=0):
    modes = [{"mesh_id": str(ms.mesh_id(i)),
              "settings": {"show_fill": False, "show_wire": False, "show_points": True,
                           "point_size": size, "point_lighting": lighting, "fill_material": material,
                           "fill_plain": plain}}
             for i in range(ms.mesh_number())]
    tb = trackball or {"center": [0, 0, 0], "radius": 0.5, "distance": 1.8, "fov_y_degrees": 40.0}
    state = {"trackball": tb, "render_settings": BARE, "mesh_render_modes": modes}
    return bytes(ms.render_snapshot(json.dumps(state), W, H))


ms.clear()
ms.load_new_mesh(find_fixture("gargoyle_small/gargoyle.ply"))
ms.apply_filter("normalize_reference_frame")
pts_off = render_points(ms, {})
pts_on = render_points(ms, RS)
print("  point cloud: %d pixels lit, %d changed by radiance scaling\n" % (len(lit(pts_off)), changed(pts_off, pts_on)))
check("the points are on screen", len(lit(pts_off)) > 2000)
check("radiance scaling changes the lighting of the points", changed(pts_off, pts_on) > 500)
check("a zero gain leaves the points alone", changed(pts_off, render_points(
    ms, {"radiance_scaling": True, "rs_enhancement": 0.0}), 3) < 100)
# It does not depend on the lighting of the points: the scaling needs a normal, not a light.
check("with the lighting of the points off the curvature is still scaled",
      changed(render_points(ms, {}, lighting=False), render_points(ms, RS, lighting=False)) > 500)

# The points take the option whatever the layer's surface is: with a PBR fill it is the points panel's
# switch that matters.
check("the points are scaled under a PBR fill too",
      changed(render_points(ms, {}, material=1), render_points(ms, RS, material=1)) > 500)

# The curvature is measured in pixels, so the same view is enhanced less when rendered bigger. The resolution
# bias undoes that: 0 leaves it, 1 makes the strength the same whatever the size (relative to 1000 px high).
def strength(ms, size, bias):
    """The mean change of the lit pixels when radiance scaling is on, at a snapshot size."""
    off = render(ms, {}, size=size)
    on = render(ms, {"radiance_scaling": True, "rs_enhancement": 0.5, "rs_resolution_bias": bias}, size=size)
    px = lit(off)
    return sum(abs(on[i] - off[i]) + abs(on[i + 1] - off[i + 1]) + abs(on[i + 2] - off[i + 2])
               for i in px) / float(max(1, len(px)))


ms.clear()
ms.load_new_mesh(find_fixture("gargoyle_small/gargoyle.ply"))
ms.apply_filter("normalize_reference_frame")
small, big = (240, 240), (720, 720)
s0 = (strength(ms, small, 0.0), strength(ms, big, 0.0))
s1 = (strength(ms, small, 1.0), strength(ms, big, 1.0))
print("  mean change at 240 / 720 px: bias 0: %.2f / %.2f   bias 1: %.2f / %.2f\n" % (s0 + s1))
check("without bias a bigger view is enhanced less", s0[1] < 0.8 * s0[0])
check("with bias 1 the two sizes are closer to each other",
      abs(s1[0] - s1[1]) < 0.6 * abs(s0[0] - s0[1]))

# The refusal of the old spelling.
refused = False
try:
    ms.render_snapshot(json.dumps({"mesh_render_modes": [
        {"mesh_id": str(ms.mesh_id(0)), "settings": {"fill_material": 2}}]}), 64, 64)
except Exception as exc:
    refused = "radiance" in str(exc).lower()
check("a state that names radiance scaling as a material is refused, and says why", refused)

# A project of dozens of layers: the gradient pass must survive the layer count growing under it,
# which the main uniform buffer does by being replaced.
ms.clear()
ms.load_new_mesh(find_fixture("sphere_1.2kv.ply"))
render(ms, RS)
for _ in range(80):
    ms.load_new_mesh(find_fixture("sphere_1.2kv.ply"))
check("81 layers with radiance scaling render after the buffer was replaced",
      len(render(ms, RS)) == W * H * 4)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
