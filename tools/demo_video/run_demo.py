#!/usr/bin/env python3
"""Drive MeshLab like a person and record the screen.  See rendering_modes.md.

    pip install pyautogui
    python3 tools/demo_video/run_demo.py --rehearse        # run the script, no recording
    python3 tools/demo_video/run_demo.py --record out.mp4  # run it and record the screen

macOS only.  The terminal needs Accessibility and Screen Recording permission.  There is no
calibration: the script starts MeshLab, places its window, and finds the render-pass buttons
by their accessible names (set in src/render/renderoverlaypanel.cpp) through System Events.
MeshLab must not already be running, so that the document starts empty.
"""
import argparse, subprocess, sys, time
from pathlib import Path

import pyautogui

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
MESH = REPO / "tests/sample_mesh/gargoyle_small/gargoyle.ply"
APP = REPO / "build-release/MeshLab.app"
BUNDLE = "net.meshlab.MeshLab"
WINDOW = (40, 60, 1280, 800)   # x, y, width, height in screen points
BUTTONS = ["Bounding Box", "Points", "Edges", "Wireframe", "Fill"]

# (caption, action, argument).  Mirrors the table in rendering_modes.md.  MeshLab starts
# with Wireframe and Fill on.
#   pause seconds | click button | rotate pixels
SCRIPT = [
    ("MeshLab",                      "pause",  4),
    ("Fill and wireframe",           "rotate", 220),
    ("Fill: smooth shaded surface",  "click",  "Wireframe"),
    ("",                             "rotate", 220),
    ("Wireframe only",               "click",  "Wireframe"),
    ("",                             "click",  "Fill"),
    ("",                             "rotate", 220),
    ("Points",                       "click",  "Wireframe"),
    ("",                             "click",  "Points"),
    ("",                             "rotate", 220),
    ("Edges",                        "click",  "Points"),
    ("",                             "click",  "Edges"),
    ("",                             "click",  "Fill"),
    ("Bounding box",                 "click",  "Bounding Box"),
    ("",                             "pause",  3),
]


def osa(script):
    # Classic MeshLab builds are also called "MeshLab": address ours by bundle id.
    script = script.replace("BUNDLE", f'"{BUNDLE}"')
    try:
        r = subprocess.run(["osascript", "-e", script], capture_output=True, text=True,
                           timeout=120)
    except subprocess.TimeoutExpired:
        sys.exit("osascript took more than 120 s: the accessibility tree is too large for this query.")
    if r.returncode:
        sys.exit("osascript failed (Accessibility permission for this terminal?):\n" + r.stderr)
    return r.stdout.strip()


def place_window():
    x, y, w, h = WINDOW
    osa(f'''tell application "System Events" to tell (first process whose bundle identifier is BUNDLE)
        set position of window 1 to {{{x}, {y}}}
        set size of window 1 to {{{w}, {h}}}
    end tell''')


WINDOW_LINE = ('tell application "System Events" to set w to window 1 of '
               '(first process whose bundle identifier is BUNDLE)\n')

FIND = '''on find(e, d, maxd, wanted)
    tell application "System Events"
        set res to ""
        try
            set n to name of e
            if n is in wanted then
                set p to position of e
                set s to size of e
                set res to n & "|" & ((item 1 of p) + (item 1 of s) / 2) & "|" & ((item 2 of p) + (item 2 of s) / 2) & linefeed
            end if
        end try
        if d < maxd then
            try
                set i to 0
                repeat with k in (UI elements of e)
                    set i to i + 1
                    if i > 40 then exit repeat
                    set res to res & my find(k, d + 1, maxd, wanted)
                end repeat
            end try
        end if
        return res
    end tell
end find
'''


def find_buttons():
    """Centre of each render-pass button, in screen points, from the accessibility tree."""
    names = "{" + ", ".join(f'"{n}"' for n in BUTTONS) + "}"
    out = osa(FIND + WINDOW_LINE + f"return my find(w, 0, 3, {names})")
    pts = {}
    for line in out.splitlines():
        n, x, y = line.split("|")
        pts[n] = (float(x.replace(",", ".")), float(y.replace(",", ".")))
    missing = [n for n in BUTTONS if n not in pts]
    if missing:
        sys.exit(f"Buttons not found by name: {missing}.  Run with --dump to see the tree.")
    return pts


WALK = '''on walk(e, d, maxd)
    tell application "System Events"
        set r to "-"
        set n to "-"
        set desc to "-"
        try
            set r to (role of e) as text
        end try
        try
            set n to (name of e) as text
        end try
        try
            set desc to (description of e) as text
        end try
        set pad to ""
        repeat d times
            set pad to pad & "  "
        end repeat
        set out to pad & r & " | " & n & " | " & desc & linefeed
        if d < maxd then
            try
                set i to 0
                repeat with k in (UI elements of e)
                    set i to i + 1
                    if i > 40 then exit repeat
                    set out to out & my walk(k, d + 1, maxd)
                end repeat
            end try
        end if
        return out
    end tell
end walk
'''


def dump(depth):
    """Print the accessibility tree (limited depth, 40 children per node) of MeshLab's window."""
    print(osa(WALK + WINDOW_LINE + f"return my walk(w, 0, {depth})"))


SPEED = 1.0     # --speed: every pause and cursor movement in the script is divided by this


def nap(secs):
    time.sleep(secs / SPEED)


def move(x, y, secs=0.8):
    pyautogui.moveTo(x, y, secs / SPEED, pyautogui.easeInOutQuad)


def run(rest):
    pyautogui.PAUSE = 0.05
    x0, y0, w0, h0 = WINDOW
    view = (x0 + w0 * 0.4, y0 + h0 * 0.55)          # inside the 3D view, left of the dock
    captions, cur, t0 = [], None, time.time()
    for caption, action, arg in SCRIPT:
        now = time.time() - t0
        if caption:                     # a caption stays up until the next one replaces it
            if cur:
                captions.append((cur[0], now, cur[1]))
            cur = (now, caption)
        if action == "pause":
            nap(arg)
        elif action == "click":
            x, y = rest[arg]
            move(x, y)
            nap(0.2)
            pyautogui.click()
            nap(1.0)
        elif action == "rotate":
            x, y = view
            move(x, y, 0.6)
            pyautogui.mouseDown()
            move(x + arg, y, 2.5)
            move(x - arg, y, 3.5)
            pyautogui.mouseUp()
    if cur:
        captions.append((cur[0], time.time() - t0, cur[1]))
    return captions


def burn_captions(src, captions, dst):
    f = ",".join(
        f"drawtext=text='{t.replace(':', chr(92) + ':')}':x=(w-text_w)/2:y=h-90:fontsize=44:"
        f"fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=14:"
        f"enable='between(t,{a:.2f},{b:.2f})'"
        for a, b, t in captions)
    subprocess.run(["ffmpeg", "-y", "-i", str(src), "-vf", f, str(dst)], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rehearse", action="store_true")
    ap.add_argument("--dump", type=int, nargs="?", const=5, metavar="DEPTH",
                    help="with MeshLab already open, print its accessibility tree down to "
                         "DEPTH levels (default 5) and exit")
    ap.add_argument("--record", metavar="OUT.mp4")
    ap.add_argument("--captions", action="store_true", help="burn captions after recording")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="play the script this many times faster (default 1)")
    ap.add_argument("--screen", default="1", help="avfoundation screen index "
                    "(ffmpeg -f avfoundation -list_devices true -i '')")
    a = ap.parse_args()
    global SPEED
    SPEED = a.speed
    if a.dump:
        return dump(a.dump)
    if not (a.rehearse or a.record):
        ap.error("give --rehearse or --record OUT.mp4")

    if osa('tell application "System Events" to return (count of (processes whose '
           'bundle identifier is BUNDLE))') != "0":
        sys.exit("MeshLab is already running: quit it so the demo starts from an empty document.")
    subprocess.run(["open", "-a", str(APP), "--args", str(MESH)], check=True)   # opens MESH
    time.sleep(4)
    osa("tell application id BUNDLE to activate")   # keystrokes go to the frontmost app
    place_window()
    time.sleep(1)
    rest = find_buttons()

    if not a.record:
        run(rest)
        return
    raw = Path(a.record).with_suffix(".raw.mp4")
    rec = subprocess.Popen(
        ["ffmpeg", "-y", "-f", "avfoundation", "-framerate", "30", "-capture_cursor", "1",
         "-i", f"{a.screen}:none", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(raw)],
        stdin=subprocess.PIPE)
    time.sleep(2)                       # let ffmpeg start
    try:
        captions = run(rest)
    finally:
        rec.communicate(b"q")           # ffmpeg stops cleanly on 'q'
    if a.captions:
        burn_captions(raw, [(s + 2, e + 2, t) for s, e, t in captions], a.record)
    else:
        raw.rename(a.record)


if __name__ == "__main__":
    main()
