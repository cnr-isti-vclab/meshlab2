# Demo script: rendering modes (~30 s)

Human-readable storyboard. `rendering_modes.py` (same folder) is generated from this table and
executed by `run_demo.py rendering_modes`. Edit the wording here first, then regenerate it.

Setup: nothing. The runner launches MeshLab (it must not be running), places the window
at 1280x800, and finds the render buttons by accessible name. MeshLab starts with Wireframe
and Fill on.

| # | Time | Action | Caption |
|---|------|--------|---------|
| 1 | 0:00 | MeshLab is started with `gargoyle.ply` as a command-line argument; pause on it | "MeshLab" |
| 2 | 0:02 | Drag in the view to rotate , left then right | "Fill and wireframe" |
| 3 | 0:05 | Click **Wireframe** (off), rotate | "Fill: smooth shaded surface" |
| 4 | 0:10 | Click **Wireframe** (on), **Fill** (off), rotate | "Wireframe only" |
| 5 | 0:15 | Click **Wireframe** (off), **Points** (on), rotate | "Points" |
| 6 | 0:20 | Click **Points** (off), **Edges** (on), **Fill** (on) | "Edges" |
| 7 | 0:25 | Click **Bounding Box** (on) | "Bounding box" |
| 8 | 0:30 | Pause on the final frame | |

Notes
- Button names are the pass names in `src/render/renderoverlaypanel.cpp`:
  Bounding Box, Points, Edges, Wireframe, Fill.
- Captions are optional on-screen text, added in post by `run_demo.py`
  (PNG captions overlaid by `ffmpeg`, needs Pillow), so the take itself stays clean.
