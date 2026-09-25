# Does the boundary decorator draw what its info panel counts? The point and line buffers
# are built inside the GPU resource cache, which a Qt test cannot link, so this cannot be a
# ctest.
#
# Run it with a real GPU and a real window server; QT_QPA_PLATFORM=offscreen gives no QRhi
# and every snapshot fails:
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/boundary_decorators
#
# The hook execs <dir>/generate_api.py with the `_meshlab` module importable, which is why
# the file has that name. render_snapshot returns raw RGBA8888.
import json, math, os, tempfile
import _meshlab as ml

W = H = 200
# Nothing but the decorator on a flat black background, so every lit pixel is decorator.
BARE = {"show_trackball_gizmo": False, "show_axis_gizmo": False, "show_view_cameras": False,
        "scene_background_top_color": [0, 0, 0, 255],
        "scene_background_bottom_color": [0, 0, 0, 255]}
ONLY = {"show_fill": False, "show_wire": False, "show_points": False, "show_edges": False,
        "decorator_boundary": True, "decorator_boundary_edges": False,
        "decorator_texture_seams": False, "decorator_non_manifold_edges": False,
        "decorator_non_manifold_vertices": False}

ok = True
def check(label, condition):
    global ok
    ok = ok and condition
    print("  %-62s %s" % (label, "ok" if condition else "FAILED"))


def ply(path, verts, faces, texcoords=None):
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\nelement vertex %d\n" % len(verts))
        f.write("property float x\nproperty float y\nproperty float z\n")
        f.write("element face %d\nproperty list uchar int vertex_indices\n" % len(faces))
        if texcoords:
            f.write("property list uchar float texcoord\n")
        f.write("end_header\n")
        for v in verts:
            f.write("%f %f %f\n" % v)
        for i, face in enumerate(faces):
            line = "3 %d %d %d" % face
            if texcoords:
                line += " 6 " + " ".join("%f" % t for t in texcoords[i])
            f.write(line + "\n")


def shot(path, **decorators):
    ms = ml.MeshSet()
    ms.load_new_mesh(path)
    mode = {"mesh_id": str(ms.current_mesh_id()), "settings": dict(ONLY, **decorators)}
    state = {"render_settings": BARE, "mesh_render_modes": [mode]}
    buf = bytes(ms.render_snapshot(json.dumps(state), W, H))
    assert len(buf) == W * H * 4, len(buf)
    return buf


def count(buf, test):
    return sum(1 for i in range(0, len(buf), 4) if test(buf[i], buf[i + 1], buf[i + 2]))

# The three default colors: green boundaries, blue seams, one red-purple for both kinds of
# non-manifoldness. Loose bounds, since lines and points are blended at their edges.
lit = lambda r, g, b: r + g + b > 60
green = lambda r, g, b: g > 150 and r < 100 and b < 100
blue = lambda r, g, b: b > 150 and r < 100 and g < 170
redpurple = lambda r, g, b: r > 150 and g < 90 and 60 < b < 190

tmp = tempfile.mkdtemp()

# Two open cones sharing their apex: the apex has two fans of faces and lies on no
# non-manifold edge, which is the one kind of vertex the decorator should mark.
n = 12
ring = lambda z: [(math.cos(2 * math.pi * i / n), math.sin(2 * math.pi * i / n), z) for i in range(n)]
cones = os.path.join(tmp, "cones.ply")
ply(cones, [(0.0, 0.0, 0.0)] + ring(1.0) + ring(-1.0),
    [(0, 1 + i, 1 + (i + 1) % n) for i in range(n)]
    + [(0, 1 + n + (i + 1) % n, 1 + n + i) for i in range(n)])

# Three triangles on one edge: a non-manifold edge, whose two ends are shown by the edge
# decorator and must not be marked again as non-manifold vertices. The edge runs across the
# view, which looks down z; along z it would be seen end-on and draw nothing.
fin = os.path.join(tmp, "fin.ply")
wings = [(0.0, math.cos(a), math.sin(a)) for a in (0.5, 2.6, 4.7)]
ply(fin, [(-1.0, 0.0, 0.0), (1.0, 0.0, 0.0)] + wings, [(0, 1, 2), (0, 1, 3), (0, 1, 4)])

# A quad whose two triangles disagree on the texture coordinates of their shared edge: a seam.
seam = os.path.join(tmp, "seam.ply")
ply(seam, [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (0.0, 1.0, 0.0)],
    [(0, 1, 2), (0, 2, 3)],
    [(0.0, 0.0, 0.4, 0.0, 0.4, 0.4), (0.6, 0.0, 1.0, 0.4, 0.6, 0.4)])

print("boundary decorators")
# Presence is checked by color, absence by any lit pixel: a dot drawn in the wrong color is
# still a dot that should not be there.
cone_dots = shot(cones, decorator_non_manifold_vertices=True)
fin_dots = shot(fin, decorator_non_manifold_vertices=True)
print("  lit pixels, vertex decorator: cones %d, fin %d" % (count(cone_dots, lit), count(fin_dots, lit)))
check("the cones' shared apex is marked, in red-purple", count(cone_dots, redpurple) > 0)
check("the ends of a non-manifold edge are not marked as vertices", count(fin_dots, lit) == 0)
check("the fin's non-manifold edge is drawn, in the same red-purple",
      count(shot(fin, decorator_non_manifold_edges=True), redpurple) > 0)
check("the cones have no non-manifold edge",
      count(shot(cones, decorator_non_manifold_edges=True), lit) == 0)
check("boundaries are green", count(shot(cones, decorator_boundary_edges=True), green) > 0)
check("texture seams are blue", count(shot(seam, decorator_texture_seams=True), blue) > 0)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
