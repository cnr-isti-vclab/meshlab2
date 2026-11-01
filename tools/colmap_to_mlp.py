#!/usr/bin/env python3
"""Convert a COLMAP text reconstruction into a MeshLab project (.mlp).

    python3 tools/colmap_to_mlp.py COLMAP_DIR [-o project.mlp] [--mesh model.ply]
                                   [--z-up] [--scale 0.5] [--no-points]

COLMAP_DIR holds `images/` and `sparse/0/` (or `sparse/`, or the three files at its top)
with cameras.txt, images.txt and points3D.txt, as RealityScan's COLMAP export writes them.
Binary COLMAP files are not read: export as text.

What is written next to the project:

- `undistorted/` -- every image resampled through its camera's lens model, so that a plain
  pinhole camera describes it: MeshLab's camera has no tangential or high-order terms.
  Pixels with no source become transparent. Skipped for cameras without distortion, whose
  images are referenced where they are (unless --scale resizes them).
- `points.ply` -- COLMAP's sparse points with their colors, as a layer to check the
  registration against (--no-points leaves it out).

--mesh adds a mesh layer. RealityScan's COLMAP export is not in the frame of its model
exports: it turns the scene's (x, y, z) into (x, -z, y), a quarter turn about x that puts
its +Z up axis on COLMAP's -Y. --z-up turns the cameras and points back, so that a model
exported from the same project lines up with no further transform. (Measured on a
35-image export: after the turn the model and the sparse points agree to the sampling
spacing, with no translation or scale left.)

The run ends by reprojecting the sparse points through the written cameras and comparing
with the 2D observations in images.txt: the median should be well under a pixel.

Conventions, against plugins/filter_texture_consolidation/pinholecamera.h, which is the
inverse of this conversion and is checked against vcg::Shot by
FilterTests::textureConsolidationCameraMatchesShotProjection:

- COLMAP stores world-to-camera R, t with the camera looking down +z and y pointing down
  (OpenCV axes). vcg's rotation rows are diag(1, -1, -1) R, and the .mlp
  TranslationVector is minus the camera centre C = -R^T t.
- COLMAP pixel coordinates put the centre of the top-left pixel at (0.5, 0.5); vcg's put
  pixel k's centre at k + 0.5 too, but count rows from the bottom. So CenterPx is
  (cx, height - cy), with no half-pixel shift.
"""

import argparse
import math
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# Parameter order of each COLMAP camera model, mapped onto FULL_OPENCV's
# fx fy cx cy k1 k2 p1 p2 k3 k4 k5 k6.
FULL = ["fx", "fy", "cx", "cy", "k1", "k2", "p1", "p2", "k3", "k4", "k5", "k6"]
MODELS = {
    "SIMPLE_PINHOLE": ["f", "cx", "cy"],
    "PINHOLE": ["fx", "fy", "cx", "cy"],
    "SIMPLE_RADIAL": ["f", "cx", "cy", "k1"],
    "RADIAL": ["f", "cx", "cy", "k1", "k2"],
    "OPENCV": ["fx", "fy", "cx", "cy", "k1", "k2", "p1", "p2"],
    "FULL_OPENCV": FULL,
}

PIXEL_SIZE_MM = 0.01  # arbitrary: FocalMm and PixelSizeMm only matter as a ratio


def find_sparse(root):
    for d in (root / "sparse" / "0", root / "sparse", root):
        if (d / "cameras.txt").exists():
            return d
    if any((d / "cameras.bin").exists() for d in (root / "sparse" / "0", root / "sparse", root)):
        sys.exit("binary COLMAP files found: export the registration as text")
    sys.exit(f"no cameras.txt under {root}")


def data_lines(path):
    with open(path) as f:
        for line in f:
            if line.strip() and not line.startswith("#"):
                yield line.rstrip("\n")


def read_cameras(path):
    cams = {}
    for line in data_lines(path):
        v = line.split()
        model = v[1]
        if model not in MODELS:
            sys.exit(f"camera model {model} is not supported")
        p = dict(zip(MODELS[model], map(float, v[4:])))
        if "f" in p:
            p["fx"] = p["fy"] = p.pop("f")
        cams[int(v[0])] = {"w": int(v[2]), "h": int(v[3]), **{k: p.get(k, 0.0) for k in FULL}}
    return cams


def read_images(path):
    # Two lines per image: the pose, then the 2D observations (which may be empty).
    with open(path) as f:
        lines = [l.rstrip("\n") for l in f if not l.startswith("#")]
    images = []
    for pose, obs in zip(lines[0::2], lines[1::2]):
        v = pose.split()
        q = np.array(list(map(float, v[1:5])))
        o = np.array(list(map(float, obs.split()))).reshape(-1, 3) if obs.strip() else np.zeros((0, 3))
        images.append({"q": q, "t": np.array(list(map(float, v[5:8]))), "cam": int(v[8]),
                       "name": " ".join(v[9:]), "obs": o})
    return images


def read_points(path):
    pts = {}
    for line in data_lines(path):
        v = line.split()
        pts[int(v[0])] = (np.array(list(map(float, v[1:4]))), tuple(int(c) for c in v[4:7]))
    return pts


def quat_to_rot(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
        [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
        [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]])


def distort(c, x, y):
    """FULL_OPENCV lens model on normalized coordinates."""
    r2 = x * x + y * y
    radial = (1 + r2 * (c["k1"] + r2 * (c["k2"] + r2 * c["k3"]))) / \
             (1 + r2 * (c["k4"] + r2 * (c["k5"] + r2 * c["k6"])))
    xd = x * radial + 2 * c["p1"] * x * y + c["p2"] * (r2 + 2 * x * x)
    yd = y * radial + c["p1"] * (r2 + 2 * y * y) + 2 * c["p2"] * x * y
    return xd, yd


def has_distortion(c):
    return any(c[k] != 0.0 for k in FULL[4:])


def undistort(img, c):
    """Resample img so that the pinhole camera fx fy cx cy describes it."""
    src = np.asarray(img.convert("RGBA"), dtype=np.float32)
    h, w = src.shape[:2]
    u, v = np.meshgrid(np.arange(w, dtype=np.float64) + 0.5, np.arange(h, dtype=np.float64) + 0.5)
    xd, yd = distort(c, (u - c["cx"]) / c["fx"], (v - c["cy"]) / c["fy"])
    # Source position in array indices, whose integers are pixel centres.
    sx = xd * c["fx"] + c["cx"] - 0.5
    sy = yd * c["fy"] + c["cy"] - 0.5
    inside = (sx >= 0) & (sy >= 0) & (sx <= w - 1) & (sy <= h - 1)
    sx = np.clip(sx, 0, w - 1)
    sy = np.clip(sy, 0, h - 1)
    x0 = np.minimum(sx.astype(np.int32), w - 2)
    y0 = np.minimum(sy.astype(np.int32), h - 2)
    fx = (sx - x0)[..., None].astype(np.float32)
    fy = (sy - y0)[..., None].astype(np.float32)
    out = (src[y0, x0] * (1 - fx) * (1 - fy) + src[y0, x0 + 1] * fx * (1 - fy) +
           src[y0 + 1, x0] * (1 - fx) * fy + src[y0 + 1, x0 + 1] * fx * fy)
    out[~inside] = 0
    return Image.fromarray(np.clip(out + 0.5, 0, 255).astype(np.uint8), "RGBA")


# RealityScan's Z-up frame from its COLMAP export's: (x, y, z) -> (x, z, -y).
Z_UP = np.array([[1.0, 0.0, 0.0], [0.0, 0.0, 1.0], [0.0, -1.0, 0.0]])


def vcg_camera(im, c, scale, world=np.eye(3)):
    """The .mlp VCGCamera attributes of one image, at the given image scale, with the scene
    turned by the rotation `world`."""
    r = quat_to_rot(im["q"]) @ world.T
    centre = -r.T @ im["t"]
    rot = np.diag([1.0, -1.0, -1.0]) @ r
    w, h = round(c["w"] * scale), round(c["h"] * scale)
    fx, fy = c["fx"] * w / c["w"], c["fy"] * h / c["h"]
    cx, cy = c["cx"] * w / c["w"], c["cy"] * h / c["h"]
    m = np.eye(4)
    m[:3, :3] = rot
    return {
        "TranslationVector": " ".join(f"{-x:.9g}" for x in centre) + " 1",
        "RotationMatrix": " ".join(f"{x:.9g}" for x in m.flatten()),
        "CameraType": "0",
        "FocalMm": f"{fx * PIXEL_SIZE_MM:.9g}",
        "LensDistortion": "0 0",
        "PixelSizeMm": f"{PIXEL_SIZE_MM:.9g} {PIXEL_SIZE_MM * fx / fy:.9g}",
        "ViewportPx": f"{w} {h}",
        "CenterPx": f"{cx:.6f} {h - cy:.6f}",
        "BinaryData": "0",
    }, (rot, centre, fx, fy, cx, h - cy, w, h)


def project_vcg(cam, p):
    """Project as vcg::Shot does, from the written values: pixel with y from the bottom."""
    rot, centre, fx, fy, cxv, cyv, _, _ = cam
    q = rot @ (p - centre)  # vcg frame: x right, y up, looking down -z
    return cxv + fx * q[0] / -q[2], cyv + fy * q[1] / -q[2]


def check(images, cams, points, written):
    """Reproject the sparse points through the written cameras and compare to COLMAP's
    observations, which are in the original, distorted, full-size image."""
    errors = []
    for im, cam in zip(images, written):
        c = cams[im["cam"]]
        scale = cam[6] / c["w"]
        for x, y, pid in im["obs"]:
            if pid < 0 or int(pid) not in points:
                continue
            u, v = project_vcg(cam, points[int(pid)][0])
            # Back to COLMAP's unscaled, top-down pixels, then through the lens.
            u, v = u / scale, c["h"] - v / scale
            xd, yd = distort(c, (u - c["cx"]) / c["fx"], (v - c["cy"]) / c["fy"])
            errors.append(math.hypot(xd * c["fx"] + c["cx"] - x, yd * c["fy"] + c["cy"] - y))
    if not errors:
        print("no observations to check against")
        return
    e = np.array(errors)
    print(f"reprojection of {len(e)} observations, in original pixels: median {np.median(e):.3f}, "
          f"90% {np.percentile(e, 90):.3f}, max {e.max():.2f}")


def write_points(path, points):
    with open(path, "w") as f:
        f.write("ply\nformat ascii 1.0\n"
                f"element vertex {len(points)}\n"
                "property float x\nproperty float y\nproperty float z\n"
                "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n")
        for p, (r, g, b) in points.values():
            f.write(f"{p[0]:.9g} {p[1]:.9g} {p[2]:.9g} {r} {g} {b}\n")


def xml_attr(s):
    return s.replace("&", "&amp;").replace('"', "&quot;").replace("<", "&lt;")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("colmap_dir", type=Path)
    ap.add_argument("-o", "--output", type=Path, help="project to write (default COLMAP_DIR/project.mlp)")
    ap.add_argument("--mesh", type=Path, help="mesh layer to add, in the registration's coordinates")
    ap.add_argument("--z-up", action="store_true",
                    help="undo RealityScan's quarter turn, to match its model exports")
    ap.add_argument("--scale", type=float, default=1.0, help="resize the written images by this factor")
    ap.add_argument("--no-points", action="store_true", help="leave the sparse points out")
    args = ap.parse_args()

    root = args.colmap_dir.resolve()
    sparse = find_sparse(root)
    out = (args.output or root / "project.mlp").resolve()
    out_dir = out.parent
    cams = read_cameras(sparse / "cameras.txt")
    images = read_images(sparse / "images.txt")
    points = read_points(sparse / "points3D.txt") if (sparse / "points3D.txt").exists() else {}
    image_dir = root / "images"
    world = Z_UP if args.z_up else np.eye(3)
    points = {k: (world @ p, rgb) for k, (p, rgb) in points.items()}

    layers = []
    if args.mesh:
        layers.append((args.mesh.stem, Path(args.mesh).resolve()))
    if points and not args.no_points:
        write_points(out_dir / "points.ply", points)
        layers.append(("sparse points", out_dir / "points.ply"))

    rasters, written = [], []
    resampled = out_dir / "undistorted"
    for i, im in enumerate(images):
        c = cams[im["cam"]]
        attrs, cam = vcg_camera(im, c, args.scale, world)
        src = image_dir / im["name"]
        if has_distortion(c) or args.scale != 1.0:
            dst = resampled / Path(im["name"]).with_suffix(".png")
            dst.parent.mkdir(parents=True, exist_ok=True)
            img = Image.open(src)
            if has_distortion(c):
                img = undistort(img, c)
            if args.scale != 1.0:
                img = img.resize((cam[6], cam[7]), Image.LANCZOS)
            img.save(dst)
            src = dst
        print(f"[{i + 1}/{len(images)}] {im['name']}", flush=True)
        rasters.append((im["name"], src, attrs))
        written.append(cam)

    def rel(p):
        try:
            return p.relative_to(out_dir).as_posix()
        except ValueError:
            return p.as_posix()

    with open(out, "w") as f:
        f.write("<!DOCTYPE MeshLabDocument>\n<MeshLabProject>\n <MeshGroup>\n")
        for label, path in layers:
            f.write(f'  <MLMesh label="{xml_attr(label)}" filename="{xml_attr(rel(path))}" visible="1">\n'
                    "   <MLMatrix44>\n1 0 0 0 \n0 1 0 0 \n0 0 1 0 \n0 0 0 1 \n</MLMatrix44>\n  </MLMesh>\n")
        f.write(" </MeshGroup>\n <RasterGroup>\n")
        for name, path, attrs in rasters:
            a = " ".join(f'{k}="{v}"' for k, v in attrs.items())
            f.write(f'  <MLRaster label="{xml_attr(name)}">\n   <VCGCamera {a}/>\n'
                    f'   <Plane fileName="{xml_attr(rel(path))}" semantic="RGBA"/>\n  </MLRaster>\n')
        f.write(" </RasterGroup>\n</MeshLabProject>\n")
    print(f"wrote {out}: {len(layers)} layers, {len(rasters)} rasters")
    check(images, cams, points, written)


if __name__ == "__main__":
    main()
