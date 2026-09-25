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
        # The cut rim is checked on its own below. Left on, it would put a few dozen
        # coloured pixels into every geometry comparison, which is real output but not
        # what those checks are about.
        "clip_plane_rim_width": 0.0,
        # The solid cut is on by default and would light the cross-section in every
        # geometry comparison below; it is checked on its own further down.
        "clip_plane_solid_cut": False,
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
    # Brackets where Ctrl+wheel starts from: offsetClearOfScene puts the plane on the
    # nearest bounding-box corner, which for a unit-ish sphere is about -0.29 diagonals.
    "view, offset -0.32":  clip(clip_plane_axis=AXIS["view"], clip_plane_offset=-0.32),
    "view, offset -0.24":  clip(clip_plane_axis=AXIS["view"], clip_plane_offset=-0.24),
    "X flipped, off +0.2": clip(clip_plane_axis=AXIS["x"], clip_plane_offset=0.2,
                                clip_plane_flipped=True),
    # The cut rim, in a colour nothing else on screen can be mistaken for.
    "X, rim off":          clip(clip_plane_axis=AXIS["x"], clip_plane_rim_width=0.0),
    "X, rim on":           clip(clip_plane_axis=AXIS["x"], clip_plane_rim_width=4.0,
                                clip_plane_rim_color=[255, 0, 255, 255]),
    "no cut, rim on":      dict(clip_plane_enabled=False, clip_plane_rim_width=4.0,
                                clip_plane_rim_color=[255, 0, 255, 255]),
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
# Ctrl+wheel enables clipping at the offset where the plane touches the scene, so a notch
# either side of it must straddle "nothing cut" and "something cut". If it did not, the
# gesture would spend its first several notches doing nothing.
check("just behind the touch point nothing is cut",
      differing(base, shots["view, offset -0.32"]) == 0.0)
check("just past it the cut has started",
      differing(base, shots["view, offset -0.24"]) > 0.0)
# The rim: where the surface runs into the plane, in the fill shaders. Counted by colour,
# because it is the one thing on screen that is neither lit grey nor background.
def magenta(buf):
    """Pixels carrying a magenta tint. The rim is blended into the shaded colour rather
    than replacing it, so most rim pixels are pink-grey, not pure magenta -- testing for
    the pure colour finds only the sliver exactly on the plane."""
    return sum(1 for i in range(0, len(buf), 4)
               if min(buf[i] - buf[i + 1], buf[i + 2] - buf[i + 1]) > 30) / float(W * H)

check("the cut is marked in the rim colour", magenta(shots["X, rim on"]) > 0.002)
check("zero width draws no rim", magenta(shots["X, rim off"]) == 0.0)
check("no cut, no rim", magenta(shots["no cut, rim on"]) == 0.0)
check("the rim does not move the cut",
      abs(coverage(shots["X, rim on"]) - coverage(shots["X, rim off"])) < 0.01)

check("flipping at an offset shows a different half",
      differing(shots["X, offset +0.2"], shots["X flipped, off +0.2"]) > 0.2)
# --- The solid cut: a stencil winding count, then the plane where the count is positive.
# Looking into a cut that faces the camera, the cross-section is the dark far wall
# without it and a lit face with it; everything outside the section must not change.
def pixel(buf, x, y):
    i = (y * W + x) * 4
    return buf[i], buf[i + 1], buf[i + 2]

def brightness(rgb):
    return sum(rgb) / 3.0

facing = clip(clip_plane_axis=AXIS["z"], clip_plane_flipped=True, clip_plane_offset=0.05)
open_cut = shot(ms, **facing)
solid_cut = shot(ms, **dict(facing, clip_plane_solid_cut=True,
                            clip_plane_solid_cut_color=[200, 200, 205, 255]))
centre_open = brightness(pixel(open_cut, W // 2, H // 2))
centre_solid = brightness(pixel(solid_cut, W // 2, H // 2))
print("\n  cut centre: %.0f without the solid cut, %.0f with it" % (centre_open, centre_solid))
check("the solid cut lights the cross-section", centre_solid > centre_open + 60)
check("a cap facing the headlight is near its full colour", centre_solid > 180)
check("the solid cut leaves the silhouette alone",
      abs(coverage(solid_cut) - coverage(open_cut)) < 0.01)
check("the corner, outside the section, is untouched",
      pixel(solid_cut, 2, 2) == pixel(open_cut, 2, 2))

# A plane that misses the sphere cuts nothing, so nothing may be capped either.
missing = clip(clip_plane_axis=AXIS["z"], clip_plane_offset=-0.9)
check("no cut, no cap",
      differing(shot(ms, **missing),
                shot(ms, **dict(missing, clip_plane_solid_cut=True))) == 0.0)

# A torus cut through the plane of its central circle leaves two concentric outlines; the
# cap must be a ring, so the hole in the middle stays background. Parity alone would get
# this right too -- what it would get wrong is an overlap, which the winding count sums.
torus = ml.MeshSet()
torus.apply_filter("create_torus", {})
looking_down = {"center": [0, 0, 0], "rotation_xyzw": [0, 0, 0, 1], "distance": 14.0,
                "radius": 4.5, "fov_y_degrees": 45.0, "near_clip_ratio": 0.0033}
def torus_shot(**rs):
    state = {"render_settings": dict(BARE, **rs), "trackball": looking_down}
    return bytes(torus.render_snapshot(json.dumps(state), W, H))
ring_cut = dict(clip_plane_enabled=True, clip_plane_axis=AXIS["z"], clip_plane_flipped=True,
                clip_plane_offset=0.0)
ring_open = torus_shot(**ring_cut)
ring_solid = torus_shot(**dict(ring_cut, clip_plane_solid_cut=True))
# The tube's centre line is at radius 3 of a scene 8 wide; at this framing that is about
# a fifth of the image out from the middle.
tube_x = W // 2 + int(0.19 * W)
print("  torus: tube %.0f -> %.0f, hole %.0f -> %.0f"
      % (brightness(pixel(ring_open, tube_x, H // 2)), brightness(pixel(ring_solid, tube_x, H // 2)),
         brightness(pixel(ring_open, W // 2, H // 2)), brightness(pixel(ring_solid, W // 2, H // 2))))
check("a concentric cut is capped across the tube",
      brightness(pixel(ring_solid, tube_x, H // 2)) > brightness(pixel(ring_open, tube_x, H // 2)) + 60)
check("and the hole in the middle is left open",
      pixel(ring_solid, W // 2, H // 2) == pixel(ring_open, W // 2, H // 2))

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
