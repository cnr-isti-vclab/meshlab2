# Is the current layer's outline drawn around the current layer? The outline is found in an
# offscreen mask and composited back over the view, sampling the mask at each pixel; a
# sampling that assumes the wrong vertical orientation mirrors it top to bottom. That happened
# on OpenGL only (its framebuffers count rows from the bottom), so run this on both:
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/current_layer_outline
#     MESHLAB2_GRAPHICS_API=opengl build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/current_layer_outline
#
# A real GPU and a window server are needed; QT_QPA_PLATFORM=offscreen gives no QRhi.
import json, os, tempfile
import _meshlab as ml

W = H = 200
MAGENTA = [255, 0, 255, 255]
# A render state says which layer is current, and one that does not leaves none.
STATE = {"current_mesh_index": 1, "current_layer_kind": "Mesh", "render_settings": {
    "show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
    "scene_background_top_color": [0, 0, 0, 255],
    "scene_background_bottom_color": [0, 0, 0, 255],
    "highlight_current_mesh": True,
    "current_mesh_outline_color": MAGENTA,
    "current_mesh_outline_width": 3}}

ok = True
def check(label, condition):
    global ok
    ok = ok and condition
    print("  %-62s %s" % (label, "ok" if condition else "FAILED"))


def quad(path, y0, y1):
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\nelement vertex 4\n")
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("element face 2\nproperty list uchar int vertex_indices\nend_header\n")
        for x, y in ((-1, y0), (1, y0), (1, y1), (-1, y1)):
            f.write("%f %f 0\n" % (x, y))
        f.write("3 0 1 2\n3 0 2 3\n")


tmp = tempfile.mkdtemp()
lower = os.path.join(tmp, "lower.ply")
upper = os.path.join(tmp, "upper.ply")
quad(lower, -1.0, -0.6)   # a strip at the bottom, only so that there are two layers
quad(upper, 0.2, 1.0)     # the current layer: the top part of the view

ms = ml.MeshSet()
ms.load_new_mesh(lower)
ms.load_new_mesh(upper)   # layer 1, made current by the render state below
buf = bytes(ms.render_snapshot(json.dumps(STATE), W, H))
assert len(buf) == W * H * 4, len(buf)

magenta_rows = [i // 4 // W for i in range(0, len(buf), 4)
                if buf[i] > 180 and buf[i + 1] < 90 and buf[i + 2] > 180]
print("current-layer outline: %d magenta pixels" % len(magenta_rows))
check("the outline is drawn", len(magenta_rows) > 50)
if magenta_rows:
    mean_row = sum(magenta_rows) / len(magenta_rows)
    print("  mean row %.1f of %d (row 0 is the top)" % (mean_row, H))
    # The current layer fills the upper part of the frame; mirrored, its outline would be in
    # the lower part, round the empty space under it.
    check("the outline is round the upper (current) layer", mean_row < H / 2)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
