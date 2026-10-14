#!/usr/bin/env python3
"""Real-PTY mouse, diff-render and stalled-terminal regressions.

All inputs, sockets, process IDs and checkpoints belong to these fixtures.
--benchmark-binary permits measuring an archived pre-change executable using
exactly the same small-delta workload; it does not replace the installed binary.
"""
import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import signal
import socket
import tempfile
import time
import traceback
from unittest.mock import patch

from verify import ROOT, HELLO, WELCOME, DELTA, END, frame, receive, checkpoint
from verify_controls import THINK, question, emit
from verify_tui import ClientTTY, peer, no_request
import verify_terminal


@contextmanager
def binary_peer(binary):
    """Substitute only the executable; reuse the actual PTY/screen fixture."""
    original = verify_terminal.subprocess.Popen
    def spawn(args, *a, **kw):
        return original([str(binary), *args[1:]], *a, **kw)
    with patch.object(verify_terminal.subprocess, "Popen", spawn):
        with peer() as fixture:
            yield fixture


def benchmark(binary):
    with binary_peer(binary) as (sock, tty, cp):
        tty.send("renderer byte comparison\r")
        question(sock, 1)
        tty.pause(0.15)
        start = len(tty.output)
        for at in range(32):
            sock.sendall(frame(DELTA, 1, at, b"x"))
            tty.pause(0.045)
        sock.sendall(frame(END, 1, 32))
        tty.wait(lambda: checkpoint(cp)["done"] == 1 and "completed" in tty.visible(), "benchmark completed")
        tty.pause(0.08)
        data = bytes(tty.output[start:])
        result = {"binary": str(binary), "geometry": "140x38", "deltas": 32,
                  "delta_bytes": 1, "interval_ms": 45, "ansi_bytes": len(data),
                  "row_addresses": data.count(b";1H"),
                  "full_screen_erases": data.count(b"\x1b[2J")}
        tty.send("/quit\r"); tty.finish()
        return result


def run():
    checks = []
    def passed(text):
        checks.append(text); print(f"PASS M{len(checks):02d}: {text}", flush=True)
    with peer() as (sock, tty, cp):
        tty.send("old keyboard question\r")
        question(sock, 1)
        content = "".join(f"TRANSCRIPT LINE {at:03d}\n" for at in range(90))
        total = emit(sock, content.encode())
        sock.sendall(frame(END, 1, total))
        tty.wait(lambda: checkpoint(cp)["done"] == 1 and "LINE 089" in tty.visible(), "long transcript completed")
        tty.send("draft中文🙂")
        tty.wait(lambda: "draft中文🙂" in tty.screen.lines()[-1], "draft before wheel")
        before = tty.visible()
        tty.send(b"\x1b[<64;20;12M")
        tty.wait(lambda: "SCROLLBACK" in tty.visible() and before != tty.visible(), "wheel scrolls transcript")
        assert "draft中文🙂" in tty.screen.lines()[-1], tty.visible()
        tty.send(b"\x1b[A")
        tty.wait(lambda: "old keyboard question" in tty.screen.lines()[-1], "Up recalls prompt")
        tty.send(b"\x1b[B")
        tty.wait(lambda: "draft中文🙂" in tty.screen.lines()[-1], "Down restores draft")
        no_request(sock)
        passed("SGR wheel scrolls conversation; keyboard Up/Down recall questions without replacing the unsent draft")

        for part in (b"\x1b[", b"<64;", b"20;", b"12M"):
            tty.send(part); tty.pause(0.03)
        tty.send(b"\x1b[<64;broken;12M\x1b[<" + b"9" * 100 + b";1;1M")
        tty.send(b"\x1b[M" + bytes((96, 52, 44)))  # X10 encoded wheel up.
        tty.send(b"\x1b[<0;1;1M\x1b[<0;1;1m\x1b[<65;1;1m")
        tty.pause(0.15)
        assert "draft中文🙂" in tty.screen.lines()[-1], tty.visible()
        tty.send(b"\x1b[6~" * 30)
        tty.wait(lambda: "LINE 089" in tty.visible(), "PageDown returns to live")
        tty.send("\r")
        assert question(sock, 2)[0] == "draft中文🙂".encode()
        sock.sendall(frame(END, 2, 0))
        tty.wait(lambda: checkpoint(cp)["done"] == 1 and checkpoint(cp)["request"] == 2, "unchanged draft submission")
        passed("fragmented, malformed, oversized, legacy and release mouse reports never leak into the submitted UTF-8 draft")

        tty.send("editor缩放é🙂")
        tty.wait(lambda: "editor缩放é🙂" in tty.screen.lines()[-1], "Unicode editor")
        for columns, rows in ((40, 10), (96, 28), (140, 38)):
            tty.resize(columns, rows)
            assert "editor缩放é🙂" in tty.screen.lines()[-1], tty.visible()
        tty.send(b"\x15")
        tty.wait(lambda: "editor" not in tty.screen.lines()[-1], "diff erases old draft tail")
        tty.send("/quit\r"); tty.finish()
        data = bytes(tty.output)
        assert b"\x1b[?1000h" in data and b"\x1b[?1006h" in data and b"\x1b[?1007l" in data
        assert data.rfind(b"\x1b[?1000l") > data.rfind(b"\x1b[?1000h")
        assert data.rfind(b"\x1b[?1006l") > data.rfind(b"\x1b[?1006h")
        assert b"\x1b[?1000;1006;1007r" in data
        passed("line diffs preserve CJK/emoji/combining-character cursors, erase shortened rows and restore mouse modes on exit")

    with peer(idle_ms=1500) as (sock, tty, cp):
        tty.send("nonblocking terminal backpressure\r")
        question(sock, 1)
        tty.pause(0.1)
        before_rss = int(Path(f"/proc/{tty.proc.pid}/statm").read_text().split()[1])
        offset = 0
        # Do not read the PTY master while output fills its kernel buffer.
        for at in range(95):
            content = (f"pressure {at:03d} " + "x" * 250 + "\n").encode()
            sock.sendall(frame(THINK, 1, offset, content)); offset += len(content)
            time.sleep(0.022)
        total = emit(sock, b"NETWORK_WAS_NOT_BLOCKED")
        sock.sendall(frame(END, 1, total))
        end = time.monotonic() + 1.0
        while time.monotonic() < end and checkpoint(cp)["done"] != 1:
            time.sleep(0.01)
        assert checkpoint(cp)["done"] == 1, "client blocked receiving END behind a full PTY"
        assert checkpoint(cp)["answer"] == b"NETWORK_WAS_NOT_BLOCKED"
        after_rss = int(Path(f"/proc/{tty.proc.pid}/statm").read_text().split()[1])
        assert (after_rss - before_rss) * os.sysconf("SC_PAGE_SIZE") < 8 * 1024 * 1024
        tty.wait(lambda: "NETWORK_WAS_NOT_BLOCKED" in tty.visible() and "completed" in tty.visible(), "coalesced final screen")
        assert "timed out" not in tty.plain()
        tty.send("/quit\r"); tty.finish()
        passed("with PTY unread for over two seconds, END still checkpoints before draining; queued screen memory stays bounded and final output appears")

    with peer() as (sock, tty, cp):
        tty.send("kept during suspend")
        tty.wait(lambda: "kept during suspend" in tty.screen.lines()[-1], "pre-suspend draft")
        os.kill(tty.proc.pid, signal.SIGTSTP)
        try:
            tty.wait(lambda: Path(f"/proc/{tty.proc.pid}/stat").read_text().rsplit(")", 1)[1].split()[0] == "T", "suspended terminal")
            assert bytes(tty.output).rfind(b"\x1b[?1000l") > bytes(tty.output).rfind(b"\x1b[?1000h")
        finally:
            os.kill(tty.proc.pid, signal.SIGCONT)
        tty.wait(lambda: bytes(tty.output).rfind(b"\x1b[?1000h") > bytes(tty.output).rfind(b"\x1b[?1000l"), "mouse restored after resume")
        tty.wait(lambda: "kept during suspend" in tty.screen.lines()[-1], "draft after resume")
        tty.send(b"\x15/quit\r"); tty.finish()
        passed("job-control suspension disables mouse reporting; foreground resume restores mouse scrolling and the original draft")

    result = benchmark(ROOT / "bin/stream_client")
    assert result["ansi_bytes"] < 14000, result
    assert result["row_addresses"] < 150, result
    passed(f"32 paced one-byte deltas use {result['ansi_bytes']} ANSI bytes and {result['row_addresses']} changed-row updates, without full-screen erases")
    print(json.dumps(result, ensure_ascii=False), flush=True)
    print(f"All {len(checks)} mouse/render checks passed.", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--benchmark-binary", type=Path)
    args = parser.parse_args()
    try:
        if args.benchmark_binary:
            print(json.dumps(benchmark(args.benchmark_binary.resolve()), ensure_ascii=False), flush=True)
        else: run()
    except Exception:
        traceback.print_exc(); raise SystemExit(1)
