# Does the renderer hand the backend a projection it can use as it is? The matrices are written
# for OpenGL (clip depth [-1, 1]); Metal and Direct3D keep depth in [0, 1], so every matrix that
# reaches the GPU has to go through QRhi::clipSpaceCorrMatrix(). Without it a perspective view
# loses the slice of space between its near plane and about twice that distance -- a sliver at the
# default near plane, but the whole front of the object once the near plane is pushed out.
#
# Run it with a real GPU and a real window server (QT_QPA_PLATFORM=offscreen gives no QRhi):
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/clip_space
#
# The camera sits 3 from a sphere of radius about 1, so its front is 2 away. A near plane at
# 0.45 * 3 = 1.35 is in front of it and cuts nothing -- but an uncorrected Metal matrix also loses
# everything nearer than about 2 * 1.35 = 2.7, which is the sphere's whole lit front.
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
        "scene_background_top_color": [0, 0, 0, 255],
        "scene_background_bottom_color": [0, 0, 0, 255]}


def centre_brightness(ms, near_ratio, projection="perspective", w=200, h=200):
    trackball = {"center": [0.0, 0.0, 0.0], "radius": 1.0, "distance": 3.0,
                 "fov_y_degrees": 40.0, "near_clip_ratio": near_ratio}
    if projection == "orthographic":
        trackball["projection"] = "orthographic"
    state = {"trackball": trackball, "render_settings": BARE}
    buf = bytes(ms.render_snapshot(json.dumps(state), w, h))
    i = ((h // 2) * w + w // 2) * 4
    return (buf[i] + buf[i + 1] + buf[i + 2]) / 3.0


ok = True
def check(label, cond):
    global ok
    print("  [%s] %s" % ("PASS" if cond else "FAIL", label))
    ok = ok and cond


ms = ml.MeshSet()
ms.load_new_mesh(find_fixture("sphere_40kv.ply"))

base = centre_brightness(ms, 0.0033)
pushed = centre_brightness(ms, 0.45)
print("\n  centre brightness: default near plane %.0f, near plane pushed out to 0.45 %.0f\n"
      % (base, pushed))
check("the sphere is lit at the centre", base > 40)
check("a near plane in front of the sphere does not cut its front",
      abs(pushed - base) < 0.1 * base)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
