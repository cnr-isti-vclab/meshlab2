"""Engine for the demo videos: drives MeshLab like a person and records the screen.

A video is a scenario in scenarios/ (see readme.md); run_demo.py is the command line.  macOS
only; the terminal needs Accessibility and Screen Recording permission.  There is no
calibration: MeshLab is started with the mesh as a command-line argument, its window is
placed, and the render-pass buttons are found by their accessible names (set in
src/render/renderoverlaypanel.cpp) through System Events.
"""
import re, subprocess, sys, tempfile, time
from pathlib import Path

import pyautogui

REPO = Path(__file__).resolve().parents[2]
APP = REPO / "build-release/MeshLab.app"
BUNDLE = "net.meshlab.MeshLab"
CAPTURE_LATENCY = 0.7    # s between starting ffmpeg and its first frame; tune if captions drift
WINDOW = (40, 60, 1280, 800)   # x, y, width, height in screen points


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


# The accessibility tree of meshlab2 is large and `entire contents` is far too slow on it,
# so the tree is walked to a limited depth, at most 40 children per node.
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


def find_buttons(buttons):
    """Centre of each render-pass button, in screen points, from the accessibility tree."""
    names = "{" + ", ".join(f'"{n}"' for n in buttons) + "}"
    out = osa(FIND + WINDOW_LINE + f"return my find(w, 0, 3, {names})")
    pts = {}
    for line in out.splitlines():
        n, x, y = line.split("|")
        pts[n] = (float(x.replace(",", ".")), float(y.replace(",", ".")))
    missing = [n for n in buttons if n not in pts]
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


def run(script, rest):
    """Play the script; returns the captions as (start, end, text), seconds from the start."""
    pyautogui.PAUSE = 0.05
    x0, y0, w0, h0 = WINDOW
    view = (x0 + w0 * 0.4, y0 + h0 * 0.55)          # inside the 3D view, left of the dock
    captions, cur, t0 = [], None, time.time()
    for n, (caption, action, arg) in enumerate(script, 1):
        print(f"  step {n}/{len(script)}: {action} {arg}", flush=True)
        now = time.time() - t0
        if caption:                     # a caption stays up until the next one replaces it
            if cur:
                captions.append((cur[0], now, cur[1]))
            cur = (now, caption)
        if action == "pause":
            nap(arg)
        elif action == "click":
            x, y = rest[arg]
            move(x, y, 0.5)
            nap(0.1)
            pyautogui.click()
            nap(0.6)
        elif action == "rotate":
            px, secs = arg
            x, y = view
            move(x, y, 0.4)
            pyautogui.mouseDown()
            move(x + px, y, (secs - 0.4) * 0.42)
            move(x - px, y, (secs - 0.4) * 0.58)
            pyautogui.mouseUp()
    if cur:
        captions.append((cur[0], time.time() - t0, cur[1]))
    return captions


def screen_device():
    """avfoundation index of the main display.  Cameras are listed first, so it varies."""
    r = subprocess.run(["ffmpeg", "-f", "avfoundation", "-list_devices", "true", "-i", ""],
                       capture_output=True, text=True)
    m = re.search(r"\[(\d+)\] Capture screen 0", r.stderr)
    if not m:
        sys.exit("No 'Capture screen' device found:\n" + r.stderr)
    return m.group(1)


def caption_png(text, path):
    """Caption as a transparent PNG: this ffmpeg has no drawtext (built without freetype)."""
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype("/System/Library/Fonts/Helvetica.ttc", 44)
    l, t, r, b = ImageDraw.Draw(Image.new("RGBA", (1, 1))).textbbox((0, 0), text, font=font)
    img = Image.new("RGBA", (r - l + 56, b - t + 36), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((0, 0, img.width - 1, img.height - 1), 14, fill=(0, 0, 0, 150))
    d.text((28 - l, 18 - t), text, font=font, fill="white")
    img.save(path)


def finish(raw, captions, dst):
    """Crop the full-screen recording to MeshLab's window, scale it, and overlay captions."""
    width = int(subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width",
         "-of", "csv=p=0", str(raw)], capture_output=True, text=True, check=True).stdout)
    k = width / pyautogui.size().width                  # pixels per screen point
    x, y, w, h = (int(v * k) // 2 * 2 for v in WINDOW)
    tmp = Path(tempfile.mkdtemp())
    cmd = ["ffmpeg", "-y", "-i", str(raw)]
    graph = [f"[0:v]crop={w}:{h}:{x}:{y},scale={WINDOW[2]}:-2[v0]"]
    last = "v0"
    for i, (a, b, text) in enumerate(captions, 1):
        png = tmp / f"cap{i}.png"
        caption_png(text, png)
        cmd += ["-i", str(png)]
        graph.append(f"[{last}][{i}:v]overlay=x=(W-w)/2:y=H-h-40:"
                     f"enable='between(t,{a:.2f},{b:.2f})'[v{i}]")
        last = f"v{i}"
    cmd += ["-filter_complex", ";".join(graph), "-map", f"[{last}]", "-c:v", "libx264",
            "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p", str(dst)]
    subprocess.run(cmd, check=True)
    raw.unlink()


def meshlab_running():
    return osa('tell application "System Events" to return (count of (processes whose '
               'bundle identifier is BUNDLE))') != "0"


def quit_meshlab():
    """Quit the MeshLab this script started, and wait until it is gone."""
    if not meshlab_running():           # `quit` would otherwise start it just to quit it
        return
    osa("tell application id BUNDLE to quit")
    for _ in range(20):
        if not meshlab_running():
            return
        time.sleep(0.5)
    print("MeshLab did not quit (a dialog open?): close it by hand.", file=sys.stderr)


def start_recorder(out, screen):
    """Start ffmpeg on the display and wait until it is really writing frames."""
    raw = out.with_suffix(".raw.mp4")
    out.parent.mkdir(parents=True, exist_ok=True)
    rec = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "warning", "-nostats", "-y",
         "-f", "avfoundation", "-framerate", "30", "-capture_cursor", "1",
         "-i", f"{screen or screen_device()}:none", "-c:v", "libx264", "-preset", "ultrafast",
         "-pix_fmt", "yuv420p", str(raw)],
        stdin=subprocess.PIPE)
    t_rec = time.time()
    print("Recording", flush=True)
    for _ in range(20):                 # frames reach the file only once capture works
        time.sleep(0.5)
        if raw.exists() and raw.stat().st_size > 100_000:
            return raw, rec, t_rec
    rec.kill()
    rec.wait()
    sys.exit("ffmpeg recorded no frames in 10 s.  Is Screen Recording enabled for this "
             "terminal (System Settings > Privacy & Security), and the terminal restarted "
             "after enabling it?  Run `pkill -9 ffmpeg` before retrying.")


def play(mesh, script, out=None, speed=1.0, captions=True, screen=None):
    """Start MeshLab on `mesh`, play `script`; with `out`, record it to that .mp4 file."""
    global SPEED
    SPEED = speed
    if meshlab_running():
        sys.exit("MeshLab is already running: quit it so the demo starts from a known state.")
    buttons = sorted({arg for _, action, arg in script if action == "click"})
    print(f"Starting MeshLab with {Path(mesh).name}", flush=True)
    subprocess.run(["open", "-a", str(APP), "--args", str(mesh)], check=True)
    rec = None
    try:
        time.sleep(4)
        osa("tell application id BUNDLE to activate")   # keystrokes go to the frontmost app
        place_window()
        time.sleep(1)
        rest = find_buttons(buttons)
        print("Buttons found", flush=True)
        if out is None:
            run(script, rest)
            return
        raw, rec, t_rec = start_recorder(out, screen)
        t_script = time.time()
        timeline = run(script, rest)
    finally:
        if rec and rec.poll() is None:
            print("Stopping the recorder", flush=True)
            try:
                rec.communicate(b"q", timeout=30)       # ffmpeg stops cleanly on 'q'
            except subprocess.TimeoutExpired:
                rec.kill()
                rec.wait()
        quit_meshlab()
    print("Encoding", flush=True)
    shift = t_script - t_rec - CAPTURE_LATENCY
    finish(raw, [(s + shift, e + shift, t) for s, e, t in timeline] if captions else [], out)
    print(f"Wrote {out}", flush=True)
