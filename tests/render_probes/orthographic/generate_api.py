# Does an orthographic camera render, through the same JSON a script or a filter would use?
# The projection lives in the camera payload, the shaders are where it takes effect, and the
# render path is in the MeshLab executable, which a Qt test cannot link -- so this is a probe,
# not a ctest.
#
# Run it with a real GPU and a real window server; QT_QPA_PLATFORM=offscreen gives no QRhi
# and every snapshot fails:
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/orthographic
#
# What it pins is the one quantity both projections share, H = 2 * distance * tan(fovY / 2),
# the world height visible at the orbit centre. With H held constant an orthographic sphere
# covers the same area whatever the field of view, a perspective one does not, and a
# perspective view at a small field of view converges on the orthographic one.
import json, math, os, tempfile
import _meshlab as ml


def find_fixture(name):
    """The hook execs this file as a string, so there is no __file__ to work back from."""
    here = os.path.abspath(os.getcwd())
    while True:
        candidate = os.path.join(here, "tests", "sample_mesh", name)
        if os.path.exists(candidate):
            return candidate
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit("cannot find tests/sample_mesh/%s above %s" % (name, os.getcwd()))
        here = parent


# A flat black background and no gizmos, so any lit pixel is mesh.
BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "scene_background_top_color": [0, 0, 0, 255],
        "scene_background_bottom_color": [0, 0, 0, 255]}

H = 2.911   # world height visible at the orbit centre, in every camera below


def distance_for(fov_deg):
    return H / (2.0 * math.tan(math.radians(fov_deg) / 2.0))


def shot(ms, projection, fov_deg, w=240, h=240):
    trackball = {"center": [0.0, 0.0, 0.0], "radius": 1.0,
                 "distance": distance_for(fov_deg), "fov_y_degrees": fov_deg}
    if projection == "orthographic":
        trackball["projection"] = "orthographic"
    state = {"trackball": trackball, "render_settings": BARE}
    buf = bytes(ms.render_snapshot(json.dumps(state), w, h))
    assert len(buf) == w * h * 4, len(buf)
    return buf


def coverage(buf, w, h):
    lit = 0
    for i in range(0, len(buf), 4):
        if buf[i] + buf[i + 1] + buf[i + 2] > 24:
            lit += 1
    return lit / float(w * h)


ok = True
def check(label, cond):
    global ok
    print("  [%s] %s" % ("PASS" if cond else "FAIL", label))
    ok = ok and cond


ms = ml.MeshSet()
ms.load_new_mesh(find_fixture("sphere_40kv.ply"))

# fov 10 is the smallest the camera accepts; 90 is wide enough to show perspective plainly.
cov = {(p, f): coverage(shot(ms, p, f), 240, 240)
       for p in ("perspective", "orthographic") for f in (10.0, 40.0, 90.0)}

print("\n  %-14s %8s %8s %8s" % ("projection", "fov 10", "fov 40", "fov 90"))
for p in ("perspective", "orthographic"):
    print("  %-14s %7.3f%% %7.3f%% %7.3f%%"
          % (p, cov[(p, 10.0)] * 100, cov[(p, 40.0)] * 100, cov[(p, 90.0)] * 100))
print()

o = [cov[("orthographic", f)] for f in (10.0, 40.0, 90.0)]
check("an orthographic sphere is on screen", 0.02 < o[1] < 0.9)
check("orthographic coverage does not depend on the field of view at constant H",
      max(o) - min(o) < 0.01 * o[1])
check("perspective coverage does depend on it at constant H (the near cap grows)",
      cov[("perspective", 90.0)] > cov[("perspective", 10.0)] * 1.05)
check("perspective at the smallest field of view is close to orthographic",
      abs(cov[("perspective", 10.0)] - o[0]) < 0.04 * o[0])
check("the two projections differ at a wide field of view",
      abs(cov[("perspective", 90.0)] - o[2]) > 0.05 * o[2])

# The front of an orthographic scene must be drawn. The renderer gives the backend the projection
# uncorrected, so a volume mapped to depth [-1, 1] on a backend that keeps [0, 1] loses its whole
# nearer half: the sphere's lit front is cut away and what is left is its dark inside, the
# object clipped the moment you switch. The middle of the sphere is its nearest point.
def centre_brightness(buf, w, h):
    i = ((h // 2) * w + w // 2) * 4
    return (buf[i] + buf[i + 1] + buf[i + 2]) / 3.0

lit_persp = centre_brightness(shot(ms, "perspective", 40.0), 240, 240)
lit_ortho = centre_brightness(shot(ms, "orthographic", 40.0), 240, 240)
print("  centre brightness: perspective %.0f, orthographic %.0f\n" % (lit_persp, lit_ortho))
check("the lit front of an orthographic sphere is drawn", lit_ortho > 0.6 * lit_persp and lit_ortho > 70)

# The view volume is as wide as the aspect makes it: the same sphere in a 2:1 frame covers
# half the fraction of it.
wide = coverage(shot(ms, "orthographic", 40.0, 320, 160), 320, 160)
check("a 2:1 frame shows the same sphere at the same scale", abs(wide - o[1] / 2.0) < 0.02 * o[1])

# A raster made from an orthographic view carries an orthographic camera, and the filters that
# project it back onto the mesh follow its parallel rays. Render the sphere into a raster, then
# colour the sphere from that raster: what faces the camera takes the raster's colours, what faces
# away stays blank. The second camera sits so close to the sphere that most of it is *behind* the
# orbit eye -- the raster's viewpoint has to be moved back for anything there to count as in front.
def projected_colours(projection, distance, fov_deg):
    trackball = {"center": [0.0, 0.0, 0.0], "radius": 1.0,
                 "distance": distance, "fov_y_degrees": fov_deg}
    if projection == "orthographic":
        trackball["projection"] = "orthographic"
    rs = ms  # the set that has the render hook
    rs.clear()
    rs.load_new_mesh(find_fixture("sphere_40kv.ply"))
    rs.apply_filter("render_from_render_state_json", {
        "camera_state": json.dumps({"kind": "MeshLab.CameraState", "version": 1,
                                    "trackball": trackball}),
        "render_state": json.dumps({"kind": "MeshLab.RenderState", "version": 1,
                                    "render_settings": BARE}),
        "output_width": 240, "output_height": 240,
        "add_as_raster": True, "raster_name": "probe"})
    rs.set_current_raster(rs.raster_number() - 1)
    rs.apply_filter("transfer_color_from_current_raster_to_vertex",
                    {"usedepth": True, "deptheta": 0.05, "preserveoccluded": False})
    # No numpy in the embedded interpreter, so read the result back through an ASCII file:
    # an OBJ line is "v x y z r g b" when the vertices carry colour.
    out = os.path.join(tempfile.mkdtemp(), "projected.obj")
    rs.save_current_mesh(out)
    pos, col = [], []
    for line in open(out):
        f = line.split()
        if len(f) >= 7 and f[0] == "v":
            pos.append([float(x) for x in f[1:4]])
            col.append([float(x) for x in f[4:7]])
    assert pos, "the saved OBJ has no coloured vertices: " + out
    return pos, col


def coloured(c):
    return c[0] + c[1] + c[2] > 0.05 * 3


for label, distance, fov in (("ortho, eye in front of the sphere", distance_for(40.0), 40.0),
                             ("ortho, eye inside the sphere", 0.5, 120.0)):
    pos, col = projected_colours("orthographic", distance, fov)
    near_axis_front = [i for i, p in enumerate(pos) if p[2] > 0.3 and abs(p[0]) < 0.4 and abs(p[1]) < 0.4]
    back = [i for i, p in enumerate(pos) if p[2] < -0.3]
    got = sum(1 for i in near_axis_front if coloured(col[i])) / float(max(1, len(near_axis_front)))
    stray = sum(1 for i in back if coloured(col[i])) / float(max(1, len(back)))
    print("  %s: %d front vertices %.0f%% coloured, %d back vertices %.0f%% coloured"
          % (label, len(near_axis_front), got * 100, len(back), stray * 100))
    check("%s: the face towards the camera takes the raster's colours" % label, got > 0.9)
    check("%s: the face away from it is left blank" % label, stray < 0.1)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
