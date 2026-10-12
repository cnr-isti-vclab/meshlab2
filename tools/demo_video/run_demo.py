#!/usr/bin/env python3
"""Run demo-video scenarios.  See readme.md.

    run_demo.py --list                       # the scenarios
    run_demo.py rendering_modes --rehearse   # play it, no recording
    run_demo.py rendering_modes              # record it to output/demo_video/rendering_modes.mp4
    run_demo.py --all --speed 2              # record every scenario, one after the other
    run_demo.py --dump 5                     # with MeshLab open: print its accessibility tree
"""
import argparse, importlib.util, sys
from pathlib import Path

import demo_lib

HERE = Path(__file__).resolve().parent
SCENARIOS = HERE / "scenarios"
OUTPUT = demo_lib.REPO / "output/demo_video"          # gitignored


def scenarios():
    return sorted(p.stem for p in SCENARIOS.glob("*.py"))


def load(name):
    path = SCENARIOS / f"{name}.py"
    if not path.exists():
        sys.exit(f"No scenario '{name}'.  Available: {', '.join(scenarios())}")
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", metavar="SCENARIO")
    ap.add_argument("--all", action="store_true", help="every scenario in scenarios/")
    ap.add_argument("--list", action="store_true", help="list the scenarios and exit")
    ap.add_argument("--rehearse", action="store_true", help="play without recording")
    ap.add_argument("--speed", type=float, default=1.0,
                    help="play this many times faster (default 1)")
    ap.add_argument("--no-captions", action="store_true", help="record without captions")
    ap.add_argument("--screen", help="avfoundation device index of the display "
                    "(default: found automatically)")
    ap.add_argument("--dump", type=int, nargs="?", const=5, metavar="DEPTH",
                    help="print MeshLab's accessibility tree down to DEPTH levels and exit")
    a = ap.parse_args()

    if a.list:
        print("\n".join(scenarios()))
        return
    if a.dump:
        return demo_lib.dump(a.dump)
    names = scenarios() if a.all else a.names
    if not names:
        ap.error("name a scenario, or give --all or --list")
    mods = [(n, load(n)) for n in names]           # load them all first: fail before playing
    for name, mod in mods:
        print(f"== {name}", flush=True)
        demo_lib.play(demo_lib.REPO / mod.MESH, mod.SCRIPT,
                      out=None if a.rehearse else OUTPUT / f"{name}.mp4",
                      speed=a.speed, captions=not a.no_captions, screen=a.screen)


if __name__ == "__main__":
    main()
