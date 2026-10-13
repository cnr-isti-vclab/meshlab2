# Does the reference grid draw, in the colours of the axes, and does an orthographic view looking
# down an axis always get one? The grid is a quad ruled by its fragment shader, so what it looks
# like is decided on the GPU and a Qt test cannot see it -- a probe, not a ctest.
#
# Run it with a real GPU and a real window server (QT_QPA_PLATFORM=offscreen gives no QRhi):
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/reference_grid
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


# ReferenceGrid in renderingsettings.h: None 0, XY 1, XZ 2.
GRID_NONE, GRID_XY, GRID_XZ = 0, 1, 2

# A black background and no gizmos, so any coloured pixel is the grid.
BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "scene_background_top_color": [0, 0, 0, 255],
        "scene_background_bottom_color": [0, 0, 0, 255]}


def render(ms, grid, projection="perspective", w=300, h=300):
    # The default camera looks down -Z with +Y up, so the XY plane is face-on to it.
    trackball = {"center": [0.0, 0.0, 0.0], "radius": 1.0, "distance": 6.0, "fov_y_degrees": 40.0}
    if projection == "orthographic":
        trackball["projection"] = "orthographic"
    settings = dict(BARE)
    settings["reference_grid"] = grid
    state = {"trackball": trackball, "render_settings": settings}
    buf = bytes(ms.render_snapshot(json.dumps(state), w, h))
    assert len(buf) == w * h * 4, len(buf)
    return buf


def count(buf, pred):
    n = 0
    for i in range(0, len(buf), 4):
        if pred(buf[i], buf[i + 1], buf[i + 2]):
            n += 1
    return n


def red(r, g, b):   return r > 120 and r > g + 50 and r > b + 50
def green(r, g, b): return g > 120 and g > r + 30 and g > b + 50
def blue(r, g, b):  return b > 120 and b > r + 50 and b > g + 20

def drawn(buf, baseline):
    """How many pixels the grid changed: those that differ from the same view with no grid. The
    sphere is grey too, so grey pixels alone would count it."""
    n = 0
    for i in range(0, len(buf), 4):
        if (abs(buf[i] - baseline[i]) + abs(buf[i + 1] - baseline[i + 1])
                + abs(buf[i + 2] - baseline[i + 2])) > 12:
            n += 1
    return n


ok = True
def check(label, cond):
    global ok
    print("  [%s] %s" % ("PASS" if cond else "FAIL", label))
    ok = ok and cond


ms = ml.MeshSet()
ms.load_new_mesh(find_fixture("sphere_40kv.ply"))

off = render(ms, GRID_NONE)
off_ortho = render(ms, GRID_NONE, "orthographic")
check("with no grid there is nothing coloured", count(off, red) + count(off, green) + count(off, blue) == 0)

xy = render(ms, GRID_XY)
print("\n  XY, perspective: red %d green %d blue %d drawn %d\n"
      % (count(xy, red), count(xy, green), count(xy, blue), drawn(xy, off)))
check("XY face-on draws the X axis in red", count(xy, red) > 100)
check("XY face-on draws the Y axis in green", count(xy, green) > 100)
check("XY has no Z axis in it", count(xy, blue) < 20)
check("the rules between the axes are drawn", drawn(xy, off) > 2000)

# The XZ plane is seen edge-on from the default camera: its two axes, X and Z, are lines through
# the centre, but nothing like the face-on grid.
xz = render(ms, GRID_XZ)
check("XZ seen edge-on draws far less than XY face-on", drawn(xz, off) < drawn(xy, off) // 2)

# The rule for orthographic views: looking straight down Z with XZ chosen would see the plane edge-on
# and nothing to measure with, so the plane across the line of sight -- XY -- is drawn instead.
ortho = render(ms, GRID_XZ, "orthographic")
print("\n  XZ chosen, orthographic down Z: red %d green %d blue %d drawn %d\n"
      % (count(ortho, red), count(ortho, green), count(ortho, blue), drawn(ortho, off_ortho)))
check("an orthographic view down Z shows the XY grid whatever was chosen",
      count(ortho, red) > 100 and count(ortho, green) > 100 and count(ortho, blue) < 20)
check("and its rules", drawn(ortho, off_ortho) > 2000)

# An orthographic view fits its depth range to the scene, and the grid plane through the origin can
# be nowhere near it. Move the sphere 500 units behind the origin and look at it from just in
# front of it: the grid, half a thousand units behind the camera, must still be in the volume.
far = ms   # the set that has the render hook
far.clear()
far.load_new_mesh(find_fixture("sphere_40kv.ply"))
far.apply_filter("set_matrix_from_translation_rotation_scale", {"translationZ": -500.0})
def render_far(grid):
    trackball = {"center": [0.0, 0.0, -500.0], "radius": 1.0, "distance": 6.0,
                 "fov_y_degrees": 40.0, "projection": "orthographic"}
    settings = dict(BARE)
    settings["reference_grid"] = grid
    return bytes(far.render_snapshot(json.dumps({"trackball": trackball, "render_settings": settings}), 300, 300))
far_off = render_far(GRID_NONE)
far_on = render_far(GRID_XY)
print("\n  grid 500 units behind the eye: drawn %d, red %d green %d\n"
      % (drawn(far_on, far_off), count(far_on, red), count(far_on, green)))
check("an orthographic grid far from the scene is still in the view volume",
      drawn(far_on, far_off) > 2000 and count(far_on, red) > 100 and count(far_on, green) > 100)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
