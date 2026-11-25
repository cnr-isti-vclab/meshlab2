# Do points drawn as quads look like points drawn as points? Direct3D cannot size a point
# primitive, so there MeshLab draws each sized point as a screen-aligned quad instead
# (shaders/point_quad.glsl, preference render.pointPrimitive). This renders the same scenes
# both ways, forced with MESHLAB2_POINT_PRIMITIVE, and wants the same pixels: the points pass
# at several sizes and in object space, selected vertices, and the non-manifold vertex markers.
# The point pipelines live in the MeshLab executable, which a Qt test cannot link, so this
# cannot be a ctest.
#
# Run it with a real GPU and a real window server; QT_QPA_PLATFORM=offscreen gives no QRhi
# and every snapshot fails:
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/point_quads
#
# On Direct3D the "points" renders are one pixel whatever the size, so run it where points
# can be sized (Metal, Vulkan, OpenGL): there both paths must agree.
import json, math, os, tempfile
import _meshlab as ml

W = H = 240
BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "scene_background_top_color": [0, 0, 0, 255],
        "scene_background_bottom_color": [0, 0, 0, 255]}
NOTHING = {"show_fill": False, "show_wire": False, "show_points": False, "show_edges": False,
           "show_bounding_box": False}

ok = True
def check(label, condition):
    global ok
    ok = ok and condition
    print("  %-66s %s" % (label, "ok" if condition else "FAILED"))


def write_ply(path, verts, faces=()):
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\nelement vertex %d\n" % len(verts))
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("element face %d\nproperty list uchar int vertex_indices\nend_header\n" % len(faces))
        for v in verts:
            f.write("%f %f %f\n" % v)
        for face in faces:
            f.write("3 %d %d %d\n" % face)


def shot(path, primitive, settings, select_all=False):
    os.environ["MESHLAB2_POINT_PRIMITIVE"] = primitive
    ms = ml.MeshSet()
    ms.load_new_mesh(path)
    if select_all:
        ms.apply_filter("select_all", {})
    mode = {"mesh_id": str(ms.current_mesh_id()), "settings": dict(NOTHING, **settings)}
    state = {"render_settings": BARE, "mesh_render_modes": [mode]}
    buf = bytes(ms.render_snapshot(json.dumps(state), W, H))
    assert len(buf) == W * H * 4, len(buf)
    return buf


def lit(buf):
    return sum(1 for i in range(0, len(buf), 4) if buf[i] + buf[i + 1] + buf[i + 2] > 60)


def differing(a, b):
    return sum(1 for i in range(0, len(a), 4)
               if max(abs(a[i + c] - b[i + c]) for c in range(3)) > 24)


def compare(label, path, settings, select_all=False, slack=0):
    native = shot(path, "points", settings, select_all)
    quads = shot(path, "quads", settings, select_all)
    n, q, d = lit(native), lit(quads), differing(native, quads)
    # The two rasterize the same squares, so the pixels must match, up to slack edge pixels.
    check("%s: points %d px lit, quads %d, %d differ" % (label, n, q, d),
          n > 0 and abs(n - q) <= slack and d <= slack)
    return q


tmp = tempfile.mkdtemp()

# A 6 x 6 grid of points in a plane, far enough apart for 14-pixel dots not to touch.
grid = os.path.join(tmp, "grid.ply")
write_ply(grid, [(x / 5.0, y / 5.0, 0.0) for x in range(6) for y in range(6)])

# A fractional size is where native points differ by driver: OpenGL on a Mac draws 5.5 px as 6,
# Metal quantizes finely, a quad takes it exactly. So whole sizes must match to the pixel, and a
# fractional one up to a ring of one pixel around each of the 36 dots.
RING = int(36 * math.pi * 6)

print("Points pass")
sizes = {}
for size in (1, 5, 5.5, 14):
    sizes[size] = compare("size %g px" % size, grid,
                          {"show_points": True, "point_size": float(size), "point_color": [255, 255, 255, 255]},
                          slack=0 if size == int(size) else RING)
check("quad dots grow with the size (%d < %d < %d)" % (sizes[1], sizes[5], sizes[14]),
      sizes[1] < sizes[5] < sizes[14])
# Quad dots are discs of exactly the size asked for: 36 of them cover 36 pi (size / 2)^2 pixels.
for size in (5.5, 14):
    disc = 36 * math.pi * (size / 2) ** 2
    check("quad dots of %g px cover what 36 such discs do (%d of ~%d)" % (size, sizes[size], int(disc)),
          abs(sizes[size] - disc) < 0.05 * disc)
# Object-space dots get fractional sizes (about 5.7 px here).
compare("object-space size", grid,
        {"show_points": True, "point_size_object_space": True, "point_object_size": 0.06,
         "point_color": [255, 255, 255, 255]}, slack=RING)

print("Selected vertices")
compare("all 36 selected", grid, {"show_selection": True, "show_selection_vertices": True}, select_all=True)

print("Decorator markers")
# Two open cones sharing their apex, the one non-manifold vertex the decorator marks.
n = 12
ring = lambda z: [(math.cos(2 * math.pi * i / n), math.sin(2 * math.pi * i / n), z) for i in range(n)]
cones = os.path.join(tmp, "cones.ply")
write_ply(cones, [(0.0, 0.0, 0.0)] + ring(1.0) + ring(-1.0),
          [(0, 1 + i, 1 + (i + 1) % n) for i in range(n)]
          + [(0, 1 + n + (i + 1) % n, 1 + n + i) for i in range(n)])
compare("non-manifold vertex marker", cones,
        {"decorator_boundary": True, "decorator_boundary_edges": False,
         "decorator_texture_seams": False, "decorator_non_manifold_edges": False,
         "decorator_non_manifold_vertices": True})

os.environ.pop("MESHLAB2_POINT_PRIMITIVE", None)
print("point_quads:", "all ok" if ok else "FAILURES")


def generate(*args, **kwargs):
    return None
