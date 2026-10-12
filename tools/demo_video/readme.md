# Demo videos

Short screen recordings of MeshLab, made by a script that drives the real application like a
person: it moves the mouse, clicks the buttons and drags the view. Each video is one
**scenario**: a storyboard written by hand and a Python file generated from it. Finished videos
go to `output/demo_video/` (gitignored).

```
tools/demo_video/
  readme.md             this file
  run_demo.py           command line
  demo_lib.py           the engine, shared by every video
  scenarios/
    NAME.md             storyboard: the source of truth
    NAME.py             generated from NAME.md: the mesh and the steps
```

## Index of the videos

One line per storyboard. **Adding a storyboard means adding its line here**, in the same
change.

| Scenario | What it shows |
|---|---|
| [rendering_modes](scenarios/rendering_modes.md) | The render-pass buttons on a mesh: fill, wireframe, points, edges and bounding box (~30 s) |

## Workflow

1. **Write the storyboard** `scenarios/NAME.md` (template below). It defines what the video
   shows, scene by scene.
2. **An agent generates** `scenarios/NAME.py` from it. It translates the storyboard and
   invents nothing: no step, mesh or caption that the storyboard does not name. If a
   storyboard cannot be played, the agent says so and the storyboard is fixed first.
3. **The maintainer rehearses and records it** from a terminal that has the permissions
   below, then looks at the result. Agents cannot do this step: macOS gives them no
   Accessibility and no Screen Recording permission.
4. When the video is wrong, the storyboard is edited and `NAME.py` regenerated. A video is
   never fixed by editing `NAME.py` alone, or the two drift apart.

## Rules for storyboards

The aim is *a few tens of very small videos*, so each one stays small:

- **One idea per video**, 10 to 20 seconds. Two ideas are two videos.
- **`snake_case` name**, the same for `NAME.md`, `NAME.py` and the video.
- **Start state is MeshLab as it opens**: empty preferences aside, Wireframe and Fill on, no
  other layer. A storyboard that needs anything else says how it gets there, in its steps.
- **Meshes come from `tests/sample_mesh/`** (committed, small), so a video can be remade by
  anyone. A new mesh is committed there first.
- **The window is always 1280×800**, so the videos all match in size.
- **Captions are a few words**, appear when the action starts and stay until the next one.
  Use the product's own vocabulary ([vocabulary.md](../../docs/design/vocabulary.md)).
- **The whole interface is in frame**: the recording is the whole MeshLab window, with its
  layer panel, filters panel and log. So nothing personal may be on it: no file path outside
  the repository, no user name.
- **Timing is part of the storyboard**: the step times in the table are what the generated
  script must meet at normal speed.

### Template

```markdown
# Demo script: <title> (~<seconds> s)

Setup: <anything beyond "MeshLab as it opens">

| # | Time | Action | Caption |
|---|------|--------|---------|
| 1 | 0:00 | MeshLab starts with `<mesh>`; pause on it | "<caption>" |
| 2 | 0:02 | Drag in the view to rotate | |
| 3 | 0:05 | Click **<Button>** (on) | "<caption>" |

Notes
- <anything the agent or the next editor must know>
```

## The steps a scenario can use

A scenario is `MESH` (path from the repository root) and `SCRIPT`, a list of
`(caption, action, argument)`:

| Action | Argument | What it does |
|---|---|---|
| `pause` | seconds | Wait, with the cursor where it is |
| `click` | button name | Move the cursor to the button and click it |
| `rotate` | `(pixels, seconds)` | Drag in the view out and back, rotating the model |

Buttons are found by their **accessible name**, which is the name of the render pass in
`src/render/renderoverlaypanel.cpp`: Bounding Box, Points, Edges, Wireframe, Fill, Selection,
Normals, Boundaries, Scalar Histogram, Clipping Plane. A click takes about 1.2 s. To list the
names the running application really exposes, open MeshLab and run `run_demo.py --dump 5`.

`demo_lib.py` grows with the videos. A storyboard that needs a new kind of step gets the
action added to `demo_lib.run()` first, and to this table, so that scenarios stay data and
every video does the same thing the same way.

**Filters are always run the same way**, so every video shows the same gesture: `Cmd+F` opens
the Filter Browser, a few words of the filter's name are typed to find it, then it is chosen.
The storyboard therefore names the filter and the words typed (`filter`, *not yet implemented*:
it is written, with its exact keystrokes, for the first storyboard that needs a filter).

## One-time setup

- A **Release** build of the application in `build-release/` that is current: the buttons need
  the accessible names added in `renderoverlaypanel.cpp`, and an unoptimized build renders
  too slowly for a smooth video.
- A Python environment with the two dependencies, outside the repository:

  ```bash
  python3 -m venv ~/.venvs/meshlab-demo
  ~/.venvs/meshlab-demo/bin/python -m pip install pyautogui pillow
  ```

- `ffmpeg` and `ffprobe` (`brew install ffmpeg`).
- In System Settings → Privacy & Security, give the **terminal app you run it from**
  *Accessibility* (to click) and *Screen Recording* (to record), then quit and reopen the
  terminal. The first launch also asks to let MeshLab read your Documents folder: allow it.

## Running

Quit MeshLab first: the runner starts its own, so every video begins from the same state, and
it refuses to run while one is open. It quits it again at the end.

```bash
PY=~/.venvs/meshlab-demo/bin/python
$PY tools/demo_video/run_demo.py --list                      # the scenarios
$PY tools/demo_video/run_demo.py rendering_modes --rehearse  # play it, no recording
$PY tools/demo_video/run_demo.py rendering_modes             # output/demo_video/rendering_modes.mp4
$PY tools/demo_video/run_demo.py --all --speed 2             # every scenario, twice as fast
```

Leave the mouse and keyboard alone while it plays. `--speed N` divides every pause and cursor
movement by N, and the captions follow; above about 2 the application may not keep up.
`--no-captions` records a clean take.

## When it goes wrong

| What you see | Likely cause |
|---|---|
| `osascript is not allowed assistive access` | The terminal lacks Accessibility, or was not restarted after it was enabled |
| `ffmpeg recorded no frames` | The terminal lacks Screen Recording, or was not restarted. Then `pkill -9 ffmpeg` |
| `Buttons not found by name` | The build predates the accessible names, or the window is not the one expected. `--dump 5` shows the tree |
| The mesh does not load | The Documents permission prompt was answered *Don't Allow*; allow it in Privacy & Security → Files and Folders |
| `MeshLab is already running` | Quit it. Other MeshLab versions do not count: the runner finds ours by bundle id `net.meshlab.MeshLab` |
| The video is black | A camera was recorded instead of the screen: pass `--screen N` from `ffmpeg -f avfoundation -list_devices true -i ""` |
| Captions early or late | Tune `CAPTURE_LATENCY` in `demo_lib.py` |

The window position and size are fixed in `demo_lib.py` (`WINDOW`), and the recording is
cropped to it, so moving the window by hand during a take does no harm to the crop but will
move the cursor off the buttons.
