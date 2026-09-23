# Does the clipping plane actually cut? Rendered through the real pipeline, because the
# shaders are the only place the answer lives -- and the render path is in the MeshLab
# executable, which a Qt test cannot link, so this cannot be a ctest.
#
# Run it with a real GPU and a real window server; QT_QPA_PLATFORM=offscreen gives no QRhi
# and every snapshot fails:
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/clip_plane
#
# The hook execs <dir>/generate_api.py with the `_meshlab` module importable, which is why
# the file has that name. render_snapshot returns raw RGBA8888.
import json, os
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


W = H = 240
AXIS = {"x": 0, "y": 1, "z": 2, "view": 3, "custom": 4}   # ClipPlaneAxis
REF = {"origin": 0, "center": 1, "min": 2, "max": 3}      # ClipPlaneReference

# The gizmos are deliberately NOT clipped -- they are the instruments, not the scene -- so
# they must be off or they contribute lit pixels to every measurement.
# A flat black background, so any lit pixel is mesh. The default gradient runs black to
# blue and a band of it reads as "lit" under any simple threshold.
BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "clip_plane_show_plane": False,
        "scene_background_top_color": [0, 0, 0, 255],
        "scene_background_bottom_color": [0, 0, 0, 255]}


def shot(ms, **clip):
    state = {"render_settings": dict(BARE, **clip)}
    buf = bytes(ms.render_snapshot(json.dumps(state), W, H))
    assert len(buf) == W * H * 4, len(buf)
    return buf


def coverage(buf):
    lit = 0
    for i in range(0, len(buf), 4):
        r, g, b = buf[i], buf[i + 1], buf[i + 2]
        if r + g + b > 24:
            lit += 1
    return lit / float(W * H)


def differing(a, b):
    return sum(1 for i in range(0, len(a), 4)
               if abs(a[i] - b[i]) > 8 or abs(a[i+1] - b[i+1]) > 8
               or abs(a[i+2] - b[i+2]) > 8) / float(W * H)


def clip(**kw):
    return dict(kw, clip_plane_enabled=True,
                clip_plane_relative_to=kw.pop("clip_plane_relative_to", REF["center"]))


ms = ml.MeshSet()
ms.load_new_mesh(find_fixture("sphere_40kv.ply"))

base = shot(ms)
cases = {
    "Z (toward camera)":  clip(clip_plane_axis=AXIS["z"]),
    "Z flipped":          clip(clip_plane_axis=AXIS["z"], clip_plane_flipped=True),
    "X":                  clip(clip_plane_axis=AXIS["x"]),
    "X flipped":          clip(clip_plane_axis=AXIS["x"], clip_plane_flipped=True),
    "view direction":     clip(clip_plane_axis=AXIS["view"]),
    "Z, offset +0.9":     clip(clip_plane_axis=AXIS["z"], clip_plane_offset=0.9),
    "Z, offset -0.9":     clip(clip_plane_axis=AXIS["z"], clip_plane_offset=-0.9),
    "custom (1,1,0)":     clip(clip_plane_axis=AXIS["custom"],
                               clip_plane_custom_axis=[1.0, 1.0, 0.0]),
    "disabled":           dict(clip_plane_enabled=False, clip_plane_axis=AXIS["x"]),
    "X, plane shown":     clip(clip_plane_axis=AXIS["x"], clip_plane_show_plane=True),
    "X, offset +0.2":     clip(clip_plane_axis=AXIS["x"], clip_plane_offset=0.2),
    "X flipped, off +0.2": clip(clip_plane_axis=AXIS["x"], clip_plane_offset=0.2,
                                clip_plane_flipped=True),
}
shots = {k: shot(ms, **v) for k, v in cases.items()}

print("\n  %-20s %9s  %s" % ("configuration", "coverage", "pixels changed vs baseline"))
print("  %-20s %8.3f%%" % ("no clipping", coverage(base) * 100.0))
for k in cases:
    print("  %-20s %8.3f%%  %7.3f%%"
          % (k, coverage(shots[k]) * 100.0, differing(base, shots[k]) * 100.0))

ok = True
def check(label, cond):
    global ok
    print("  [%s] %s" % ("PASS" if cond else "FAIL", label))
    ok = ok and cond

b = coverage(base)
print()
check("the baseline shows a sphere and nothing else", 0.05 < b < 0.95)
# Cutting away the HALF OF A SPHERE THAT WAS ALREADY HIDDEN must change nothing at all:
# the near hemisphere is what you were looking at either way. A cut that moved a pixel here
# would mean the plane is not where the settings say it is.
check("cutting away only hidden geometry changes nothing",
      differing(base, shots["Z (toward camera)"]) == 0.0)
check("an X cut removes about half the silhouette",
      0.3 * b < coverage(shots["X"]) < 0.8 * b)
check("flipping X keeps the other half",
      differing(shots["X"], shots["X flipped"]) > 0.2)
check("the two X halves together cover the whole sphere",
      abs((coverage(shots["X"]) + coverage(shots["X flipped"])) - b) < 0.35 * b)
check("view direction cuts like a Z plane",
      differing(shots["view direction"], shots["Z flipped"]) < 0.01)
check("an offset past the sphere leaves nothing", coverage(shots["Z, offset +0.9"]) < 0.002)
check("an offset behind the sphere leaves everything",
      differing(base, shots["Z, offset -0.9"]) < 0.002)
check("a custom normal cuts differently from any axis",
      differing(shots["custom (1,1,0)"], shots["X"]) > 0.05)
check("disabling it renders exactly the baseline", differing(base, shots["disabled"]) == 0.0)
# The plane gizmo is off in BARE, so turning it on must put something new on screen. It is
# a grid of thin lines, so it adds a fraction of a percent, not a visible slab.
check("the plane gizmo draws",
      coverage(shots["X, plane shown"]) > coverage(shots["X"]) + 0.002)
# A flip must swap the sides WITHOUT sliding the cut: the two offset halves have to add
# back up to the whole sphere, which they cannot if the plane moved.
check("flipping at an offset keeps the cut in place",
      abs((coverage(shots["X, offset +0.2"]) + coverage(shots["X flipped, off +0.2"])) - b)
      < 0.35 * b)
check("flipping at an offset shows a different half",
      differing(shots["X, offset +0.2"], shots["X flipped, off +0.2"]) > 0.2)
print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
