# Demo script: rendering modes (~45 s)

Human-readable storyboard. `run_demo.py` executes exactly these steps, in this order.
Edit the wording here first, then mirror the change in the `SCRIPT` list of `run_demo.py`.

Setup: nothing. `run_demo.py` launches MeshLab (it must not be running), places the window
at 1280x800, and finds the render buttons by accessible name. MeshLab starts with Wireframe
and Fill on.

| # | Time | Action | Caption |
|---|------|--------|---------|
| 1 | 0:00 | MeshLab is started with `gargoyle.ply` as a command-line argument; pause on it | "MeshLab" |
| 2 | 0:04 | Drag in the view to rotate slowly, left then right | "Fill and wireframe" |
| 3 | 0:10 | Click **Wireframe** (off), rotate | "Fill: smooth shaded surface" |
| 4 | 0:17 | Click **Wireframe** (on), **Fill** (off), rotate | "Wireframe only" |
| 5 | 0:25 | Click **Wireframe** (off), **Points** (on), rotate | "Points" |
| 6 | 0:33 | Click **Points** (off), **Edges** (on), **Fill** (on) | "Edges" |
| 7 | 0:37 | Click **Bounding Box** (on) | "Bounding box" |
| 8 | 0:40 | Pause on the final frame | |

Notes
- Button names are the pass names in `src/render/renderoverlaypanel.cpp`:
  Bounding Box, Points, Edges, Wireframe, Fill.
- Captions are optional on-screen text, added in post by `run_demo.py --captions`
  (an `ffmpeg drawtext` pass over the recording), so the take itself stays clean.
