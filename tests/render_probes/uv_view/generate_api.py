# The UV view draws a layer's texture group from the shared GPU cache, through a copy of the
# group laid out in UV space (one vertex per vertex and UV pair). This probe renders the UV
# view of a parametrized gargoyle and checks that the fill covers the charts, that scalars
# go through the same range uniform as in 3D, that a selection change alone reaches the
# overlay, and that seams show.
#
# Run it with a real GPU and a real window server (QT_QPA_PLATFORM=offscreen gives no QRhi):
#
#     build-release/MeshLab.app/Contents/MacOS/MeshLab \
#         --generate-docs tests/render_probes/uv_view
#
# Set MESHLAB2_PROBE_OUT to a directory to also get the renders as PPM images.
import json, os
import _meshlab as ml

W = H = 400
CONSTANT, PER_VERTEX_SCALAR = 0, 3
OUT = os.environ.get("MESHLAB2_PROBE_OUT")


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


def render(name, fill_source=CONSTANT, **mesh_settings):
    settings = {"show_wire": False, "show_fill": True, "fill_color": [0, 0, 255, 255],
                "fill_plain": {"color_source": fill_source}}
    settings.update(mesh_settings)
    state = {"view_mode": "ParametrizationUV", "current_mesh_index": 0,
             "render_settings": {"uv_show_full_texture": False, "uv_show_reference_frame": False,
                                 "quality_histogram_colormap_id": "gray",
                                 "quality_histogram_fixed_range": True,
                                 "quality_histogram_min": 0.0, "quality_histogram_max": 1.0},
             "mesh_render_modes": [{"mesh_id": str(ms.mesh_id(0)), "settings": settings}]}
    buf = bytes(ms.render_snapshot(json.dumps(state), W, H))
    if OUT:
        with open(os.path.join(OUT, name + ".ppm"), "wb") as f:
            f.write(b"P6 %d %d 255\n" % (W, H))
            f.write(b"".join(buf[i:i + 3] for i in range(0, len(buf), 4)))
    return buf


def pixels(buf, pred):
    return sum(1 for i in range(0, len(buf), 4) if pred(buf[i], buf[i + 1], buf[i + 2]))


def differing(a, b):
    return sum(1 for i in range(0, len(a), 4) if max(abs(a[i + c] - b[i + c]) for c in range(3)) > 8)


ok = True
def check(label, cond, detail=""):
    global ok
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", label, detail))
    ok = ok and cond


ms = ml.MeshSet()
ms.load_new_mesh(find_fixture(os.path.join("gargoyle_small", "gargoyle.ply")))
ms.apply_filter("parametrize_by_atlas_xatlas", {"use_dummy_texture": True, "dummy_type": "uv_grid"})

print("uv view")
blue = render("fill")
charts = pixels(blue, lambda r, g, b: b > 200 and r < 60 and g < 60)
check("the fill covers the charts in the fill color", charts > W * H // 10, charts)

ms.apply_filter("compute_vertex_scalar_by_expression", {"q": "0.25"})
gray = render("scalar", PER_VERTEX_SCALAR)
quarter = pixels(gray, lambda r, g, b: abs(r - 64) <= 4 and abs(g - 64) <= 4 and abs(b - 64) <= 4)
check("a per-vertex 0.25 in [0, 1] is a quarter gray over the same area",
      abs(quarter - charts) < charts // 50, (quarter, charts))

ms.apply_filter("select_all")
selected = render("selected", show_selection=True, show_selection_faces=True)
check("select all tints the charts", differing(selected, blue) > charts * 0.9, differing(selected, blue))
ms.apply_filter("select_none")
cleared = render("cleared", show_selection=True, show_selection_faces=True)
check("select none, a selection change alone, clears the tint", differing(cleared, blue) == 0,
      differing(cleared, blue))

wire = render("wire", show_wire=True, wire_color=[255, 255, 255, 255])
check("the wire is drawn over the fill", differing(wire, blue) > charts // 4, differing(wire, blue))

seams = render("seams", decorator_boundary=True, decorator_texture_seams=True,
               decorator_texture_seam_color=[255, 255, 0, 255], decorator_boundary_width=1.0)
seam_pixels = pixels(seams, lambda r, g, b: r > 200 and g > 200 and b < 80)
check("the seams outline the charts", seam_pixels > 1000, seam_pixels)

print("\nRESULT:", "all checks passed" if ok else "FAILURES ABOVE")


def generate(*args, **kwargs):
    return None
