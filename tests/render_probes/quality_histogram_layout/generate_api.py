# The quality histogram claims a strip on the left of the on-screen view instead of
# floating over the mesh, but an offscreen capture must not: the histogram is a native
# QLabel that no capture ever contains, and cameraShotForViewport() describes a capture as
# one full-frame camera, so a reserved strip would come out as an unexplained blank band --
# and a snapshot-to-raster taken with the histogram open would be misregistered against
# its own camera. This probe checks that showing the histogram leaves every capture
# unchanged, in both layer arrangements.
#
# The on-screen strip itself cannot be checked from here: every render a probe can ask for
# is an offscreen capture, which is exactly the case that keeps the full frame.
#
# Run it with a real GPU and a real window server; QT_QPA_PLATFORM=offscreen gives no QRhi
# and every snapshot fails:
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/quality_histogram_layout
import json, os
import _meshlab as ml

W, H = 480, 320
BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False}

ok = True
def check(label, condition):
    global ok
    ok = ok and condition
    print("  %-62s %s" % (label, "ok" if condition else "FAILED"))


def find_fixture(name):
    here = os.path.abspath(os.getcwd())
    while True:
        candidate = os.path.join(here, "tests", "sample_mesh", name)
        if os.path.exists(candidate):
            return candidate
        parent = os.path.dirname(here)
        if parent == here:
            raise SystemExit("cannot find tests/sample_mesh/%s above %s" % (name, os.getcwd()))
        here = parent


def shot(**global_settings):
    # show_quality_histogram is a GlobalRenderSettings field (per view, not per mesh), so it
    # goes straight into render_settings. current_mesh_index is not implied by "there is
    # only one mesh": a render-state snapshot is a hermetic description of what to draw, and
    # one that omits it renders with no current mesh (index -1) -- in which case the
    # histogram would have nothing to describe and this probe would pass for the wrong
    # reason.
    state = {"render_settings": dict(BARE, **global_settings), "current_mesh_index": 0}
    buf = bytes(ms.render_snapshot(json.dumps(state), W, H))
    assert len(buf) == W * H * 4, len(buf)
    return buf


def differing_pixels(a, b):
    return sum(1 for i in range(0, len(a), 4) if a[i:i + 3] != b[i:i + 3])


ms = ml.MeshSet()
ms.load_new_mesh(find_fixture("sphere_1.2kv.ply"))

print("quality histogram layout")
plain = shot()
with_hist = shot(show_quality_histogram=True)
check("a capture is identical with the histogram on or off",
      differing_pixels(plain, with_hist) == 0)

# The grid arrangement reaches the strip through a different branch of viewTiles(), so it
# needs its own look. LayerArrangement serializes as its underlying int: 1 = Grid.
ms.load_new_mesh(find_fixture("sphere_1.2kv.ply"))
grid_plain = shot(layer_arrangement=1)
grid_hist = shot(layer_arrangement=1, show_quality_histogram=True)
check("so is a capture of the grid arrangement",
      differing_pixels(grid_plain, grid_hist) == 0)
# Guards the two checks above against passing vacuously: if the second mesh had not made
# a grid, both pairs would be the same single-tile render.
check("and the grid capture really is a different layout",
      differing_pixels(plain, grid_plain) > 0)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
