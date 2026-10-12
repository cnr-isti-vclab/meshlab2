"""Rendering modes: fill, wireframe, points, edges, bounding box.  Storyboard: rendering_modes.md."""

MESH = "tests/sample_mesh/gargoyle_small/gargoyle.ply"      # relative to the repository root

# (caption, action, argument).  The step times in the storyboard are met at speed 1: a click
# takes ~1.2 s, a rotate takes the seconds given.  MeshLab starts with Wireframe and Fill on.
#   pause seconds | click button | rotate (pixels, seconds)
SCRIPT = [
    ("MeshLab",                      "pause",  2),
    ("Fill and wireframe",           "rotate", (220, 3.0)),
    ("Fill: smooth shaded surface",  "click",  "Wireframe"),
    ("",                             "rotate", (220, 3.8)),
    ("Wireframe only",               "click",  "Wireframe"),
    ("",                             "click",  "Fill"),
    ("",                             "rotate", (220, 2.6)),
    ("Points",                       "click",  "Wireframe"),
    ("",                             "click",  "Points"),
    ("",                             "rotate", (220, 2.6)),
    ("Edges",                        "click",  "Points"),
    ("",                             "click",  "Edges"),
    ("",                             "click",  "Fill"),
    ("",                             "pause",  1.4),
    ("Bounding box",                 "click",  "Bounding Box"),
    ("",                             "pause",  3.8),
    ("",                             "pause",  3),
]
