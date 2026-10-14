#!/usr/bin/env python3
"""Replay an archived ANSI redraw in an owned xterm; capture only its window.

Optional evidence dependencies: xterm, Pillow and python-xlib. They are not
application/test dependencies. --xterm and --library-path permit temporary
unpacked packages; FONTCONFIG_FILE can select user-owned font configuration.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import termios
import time


class CellGeometry(Exception):
    def __init__(self, width, height):
        self.pixels = width, height


def child(log, temporary):
    temporary = Path(temporary)
    while not (temporary / "go").exists():
        rows, columns, xpixels, ypixels = struct.unpack("HHHH", fcntl.ioctl(0, termios.TIOCGWINSZ, b"\0" * 8))
        (temporary / "geometry").write_text(json.dumps({"rows": rows, "columns": columns,
                                                       "xpixels": xpixels, "ypixels": ypixels}))
        time.sleep(0.05)
    sys.stdout.buffer.write(Path(log).read_bytes()); sys.stdout.buffer.flush()
    (temporary / "rendered").write_text("ready")
    while True: time.sleep(1)


def capture(args):
    from Xlib import X, display
    from PIL import Image
    d = display.Display(); screen = d.screen()
    border = 5
    w = round(args.font_size * 0.8) * args.columns + 2 * border
    h = round(args.font_size * 1.6) * args.rows + 2 * border
    if getattr(args, "pixels", None): w, h = args.pixels
    if w > screen.width_in_pixels or h > screen.height_in_pixels:
        d.close(); raise RuntimeError("owned window exceeds display; reduce --font-size")
    parent = screen.root.create_window(0, 0, w, h, 0, screen.root_depth,
        X.InputOutput, X.CopyFromParent, background_pixel=screen.white_pixel,
        override_redirect=True, event_mask=X.StructureNotifyMask)
    parent.set_wm_name("Task4 owned ANSI evidence replay"); parent.map(); d.sync()
    env = os.environ.copy()
    if args.library_path: env["LD_LIBRARY_PATH"] = args.library_path
    if args.app_defaults: env["XAPPLRESDIR"] = args.app_defaults
    with tempfile.TemporaryDirectory(prefix="task4-capture-") as temporary:
        temporary = Path(temporary)
        command = [args.xterm, "-into", str(parent.id), "-bg", "white", "-fg", "black",
            "-fa", args.font, "-fs", str(args.font_size), "-geometry", f"{args.columns}x{args.rows}",
            "-b", str(border), "+sb", "-xrm", "XTerm*allowWindowOps: false"]
        if args.font_double: command.extend(["-fd", args.font_double])
        command.extend(["-e", sys.executable, str(Path(__file__).resolve()), "--child", args.log, str(temporary)])
        process = subprocess.Popen(command, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        try:
            deadline, geometry = time.monotonic() + 15, None
            while True:
                if process.poll() is not None: raise RuntimeError(process.stderr.read())
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"owned xterm geometry timed out: {geometry}; window={w}x{h}")
                try: geometry = json.loads((temporary / "geometry").read_text())
                except (FileNotFoundError, json.JSONDecodeError): time.sleep(0.05); continue
                if (geometry["columns"], geometry["rows"]) == (args.columns, args.rows): break
                if not geometry["xpixels"] or not geometry["ypixels"]:
                    raise RuntimeError("xterm did not report pixel/cell geometry")
                cw = (geometry["xpixels"] - 2 * border) // geometry["columns"]
                ch = (geometry["ypixels"] - 2 * border) // geometry["rows"]
                w, h = args.columns * cw + 2 * border, args.rows * ch + 2 * border
                if w > screen.width_in_pixels or h > screen.height_in_pixels:
                    raise RuntimeError("owned window exceeds display; reduce --font-size")
                # Embedded xterm keeps its original cell geometry when its
                # external parent changes. Recreate our own window at the
                # measured font size, before sending any archived output.
                raise CellGeometry(w, h)
            (temporary / "go").write_text("render")
            while not (temporary / "rendered").exists():
                if time.monotonic() >= deadline: raise TimeoutError("owned xterm redraw timed out")
                time.sleep(0.05)
            time.sleep(args.settle_seconds); d.sync()
            actual = parent.get_geometry(); w, h = actual.width, actual.height
            raw = parent.get_image(0, 0, w, h, X.ZPixmap, 0xffffffff)
            if screen.root_depth != 24 or len(raw.data) != w * h * 4:
                raise RuntimeError("unsupported X11 pixel format")
            destination = Path(args.output).resolve(); destination.parent.mkdir(parents=True, exist_ok=True)
            Image.frombytes("RGB", (w, h), raw.data, "raw", "BGRX").save(destination)
            metadata = {"output": str(destination), "pixels": [w, h], "terminal": geometry,
                "method": "verbatim actual ANSI redraw replay; only our owned xterm window captured",
                "font": args.font, "font_double": args.font_double, "font_size": args.font_size}
            if args.metadata:
                Path(args.metadata).write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n")
            print(json.dumps(metadata, ensure_ascii=False))
        finally:
            process.terminate()
            try: process.wait(timeout=3)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
            parent.destroy(); d.sync(); d.close()


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        child(*sys.argv[2:]); raise SystemExit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True); parser.add_argument("--output", required=True)
    parser.add_argument("--columns", type=int, required=True); parser.add_argument("--rows", type=int, required=True)
    parser.add_argument("--font-size", type=int, default=12)
    parser.add_argument("--font", default="DejaVu Sans Mono"); parser.add_argument("--font-double")
    parser.add_argument("--xterm", default="xterm"); parser.add_argument("--library-path")
    parser.add_argument("--app-defaults")
    parser.add_argument("--metadata", help="write measured cell/pixel capture metadata as JSON")
    parser.add_argument("--settle-seconds", type=float, default=1.0,
                        help="allow the owned xterm to finish font loading/rendering")
    args = parser.parse_args()
    for attempt in range(3):
        try:
            capture(args); break
        except CellGeometry as corrected:
            args.pixels = corrected.pixels
    else: raise RuntimeError("cannot obtain requested terminal geometry")
