#!/usr/bin/env python3
"""Screen-level TUI checks using the real C client and a test-owned TCP peer.

The fixtures are deterministic layout data, not output attributed to Qwen.
Only the standard library is needed. --capture also archives the actual ANSI
redraws from one PTY resized successively; these can be replayed in an owned
xterm for reviewable screenshots without inspecting another desktop window.
"""

import argparse
import codecs
from contextlib import contextmanager
from datetime import datetime
import fcntl
import json
import os
from pathlib import Path
import signal
import socket
import struct
import tempfile
import termios
import time
import traceback
import unicodedata
from zoneinfo import ZoneInfo

from verify import ROOT, HELLO, WELCOME, DELTA, END, HEARTBEAT, frame, receive, checkpoint
from verify_controls import THINK, STATUS, question, emit
from verify_terminal import TTY


class Screen:
    """Small VT cell grid: addresses, erases, scroll regions and truecolor SGR.

    It intentionally models the escape sequences emitted by this client; it
    does not claim to be a complete emulator. Assertions examine visible cells
    and their attributes, so superseded redraws cannot satisfy a check.
    """
    def __init__(self, rows, columns):
        self.rows, self.columns = rows, columns
        self.decoder = codecs.getincrementaldecoder("utf-8")("strict")
        self.mode, self.escape = "text", ""
        self.fg, self.bg = None, None
        self.row, self.column = 0, 0
        self.top, self.bottom = 0, rows - 1
        self.saved = (0, 0)
        self.cells = [[(" ", None, None) for _ in range(columns)] for _ in range(rows)]

    def blank(self):
        return [(" ", self.fg, self.bg) for _ in range(self.columns)]

    def linefeed(self):
        if self.row == self.bottom:
            self.cells.pop(self.top)
            self.cells.insert(self.bottom, self.blank())
        else:
            self.row = min(self.rows - 1, self.row + 1)

    def put(self, character):
        if unicodedata.combining(character):
            at = max(0, self.column - 1)
            old, fg, bg = self.cells[self.row][at]
            self.cells[self.row][at] = (old + character, fg, bg)
            return
        width = 2 if unicodedata.east_asian_width(character) in "WF" else 1
        if self.column + width > self.columns:
            self.column = 0
            self.linefeed()
        self.cells[self.row][self.column] = (character, self.fg, self.bg)
        if width == 2:
            self.cells[self.row][self.column + 1] = ("", self.fg, self.bg)
        self.column += width

    def csi(self, final):
        private = self.escape.startswith("?")
        raw = self.escape[1:] if private else self.escape
        args = [int(value or 0) for value in raw.split(";")] if raw else [0]
        if private:
            if final == "h" and 1049 in args:
                self.cells = [self.blank() for _ in range(self.rows)]
                self.row = self.column = 0
            return  # Private mouse/cursor mode save/restore is not DECSTBM.
        n = args[0] or 1
        if final in "Hf":
            self.row = min(self.rows - 1, max(0, (args[0] or 1) - 1))
            self.column = min(self.columns - 1, max(0, (args[1] if len(args) > 1 else 1) - 1))
        elif final == "A": self.row = max(0, self.row - n)
        elif final == "B": self.row = min(self.rows - 1, self.row + n)
        elif final == "C": self.column = min(self.columns - 1, self.column + n)
        elif final == "D": self.column = max(0, self.column - n)
        elif final == "G": self.column = min(self.columns - 1, n - 1)
        elif final == "J":
            if args[0] in (2, 3): self.cells = [self.blank() for _ in range(self.rows)]
            elif args[0] == 0:
                self.cells[self.row][self.column:] = self.blank()[self.column:]
                for row in range(self.row + 1, self.rows): self.cells[row] = self.blank()
        elif final == "K":
            if args[0] == 2: self.cells[self.row] = self.blank()
            elif args[0] == 1: self.cells[self.row][:self.column + 1] = self.blank()[:self.column + 1]
            else: self.cells[self.row][self.column:] = self.blank()[self.column:]
        elif final == "r":
            self.top = max(0, (args[0] or 1) - 1)
            self.bottom = min(self.rows - 1, (args[1] if len(args) > 1 and args[1] else self.rows) - 1)
            self.row = self.column = 0
        elif final == "s": self.saved = self.row, self.column
        elif final == "u": self.row, self.column = self.saved
        elif final == "m":
            at = 0
            while at < len(args):
                code = args[at]
                if code == 0: self.fg = self.bg = None
                elif code in (38, 48) and at + 4 < len(args) and args[at + 1] == 2:
                    color = tuple(args[at + 2:at + 5])
                    if code == 38: self.fg = color
                    else: self.bg = color
                    at += 4
                elif code == 39: self.fg = None
                elif code == 49: self.bg = None
                at += 1
        elif private and final == "h" and 1049 in args:
            self.cells = [self.blank() for _ in range(self.rows)]
            self.row = self.column = 0

    def feed(self, data):
        for char in self.decoder.decode(data):
            if self.mode == "csi":
                if "@" <= char <= "~":
                    self.csi(char); self.mode, self.escape = "text", ""
                else: self.escape += char
            elif self.mode == "osc":
                if char == "\x07": self.mode = "text"
                elif char == "\x1b": self.mode = "osc_end"
            elif self.mode == "osc_end": self.mode = "text" if char == "\\" else "osc"
            elif self.mode == "escape":
                if char == "[": self.mode, self.escape = "csi", ""
                elif char == "]": self.mode = "osc"
                else: self.mode = "text"
            elif char == "\x1b": self.mode = "escape"
            elif char == "\r": self.column = 0
            elif char == "\n": self.linefeed()
            elif char == "\b": self.column = max(0, self.column - 1)
            elif char == "\t": self.column = min(self.columns - 1, (self.column // 8 + 1) * 8)
            elif ord(char) >= 32 and char != "\x7f": self.put(char)

    def lines(self):
        return ["".join(cell[0] for cell in row).rstrip() for row in self.cells]

    def text(self):
        return "\n".join(self.lines())

    def color_of(self, fragment):
        for row in self.cells:
            text = "".join(cell[0] for cell in row)
            if fragment in text:
                start = text.index(fragment)
                end, at, colors = start + len(fragment), 0, []
                for value, foreground, _ in row:
                    after = at + len(value)
                    if value and after > start and at < end:
                        colors.append(foreground)
                    at = after
                return colors
        raise AssertionError(f"missing visible fragment: {fragment}\n{self.text()}")


class ClientTTY(TTY):
    def __init__(self, port, cp, idle_ms=30000):
        self.screen = Screen(24, 96)
        self.fed = 0
        super().__init__(port, cp=cp, extra=("--idle-ms", str(idle_ms)))

    def drain(self, delay=0):
        super().drain(delay)
        data = bytes(self.output[self.fed:])
        self.fed = len(self.output)
        self.screen.feed(data)

    def visible(self):
        self.drain()
        return self.screen.text()

    def resize(self, columns, rows):
        self.drain()
        mark = len(self.output)
        unchanged = (rows, columns) == (self.screen.rows, self.screen.columns)
        self.screen = Screen(rows, columns)
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
        if unchanged:
            self.send(b"\x0c")  # An unchanged TIOCSWINSZ does not deliver SIGWINCH.
        self.wait(lambda: "TASK4" in self.visible() and "question" in self.screen.lines()[-1],
                  f"complete {columns}x{rows} redraw")
        self.pause(0.12)
        return mark


@contextmanager
def peer(idle_ms=30000):
    with tempfile.TemporaryDirectory(prefix="task4-tui-") as temp:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(6)
            cp = Path(temp) / "layout.ckpt"
            tty = ClientTTY(listener.getsockname()[1], cp, idle_ms)
            try:
                with listener.accept()[0] as sock:
                    sock.settimeout(6)
                    assert receive(sock)[0] == HELLO
                    sock.sendall(frame(WELCOME, payload=b"c" * 32))
                    tty.wait(lambda: cp.exists() and "question" in tty.visible(), "TUI welcome")
                    tty.resize(140, 38)
                    yield sock, tty, cp
            finally: tty.close()


def status(sock, request, phase="generating", generated=48, rate=76.5, elapsed=0.7, offset=0):
    payload = json.dumps({"phase": phase, "model": "Qwen3.5-4B / layout fixture",
        "prompt_tokens": 81, "context_tokens": 16384, "generated_tokens": generated,
        "tokens_per_second": rate, "elapsed_seconds": elapsed,
        "ui_private_marker": "RAW_STATUS_MUST_NOT_APPEAR"}).encode()
    sock.sendall(frame(STATUS, request, offset, payload))


def no_request(sock):
    old = sock.gettimeout(); sock.settimeout(0.15)
    try:
        while True:
            try: kind, request, offset, payload = receive(sock)
            except socket.timeout: return
            assert kind == HEARTBEAT, ("presentation control sent a model request", kind, request, payload)
    finally: sock.settimeout(old)


def capture(tty, columns, rows, theme, index, enabled):
    mark = tty.resize(columns, rows)
    text = tty.visible()
    assert "RAW_STATUS_MUST_NOT_APPEAR" not in text and '"phase"' not in text
    assert "fgets" in tty.screen.lines()[-1], ("draft viewport lost on resize", text)
    assert "completed" in text, ("final metadata missing", text)
    if enabled:
        stem = f"tui-{theme}-{columns}x{rows}"
        # One real PTY is resized, with the actual full redraw archived verbatim.
        (ROOT / "logs" / f"{stem}.ansi").write_bytes(bytes(tty.output[mark:]))
        (ROOT / "logs" / f"{stem}.screen.txt").write_text(text + "\n")
    return {"index": index, "columns": columns, "rows": rows, "theme": theme,
            "ansi": f"logs/tui-{theme}-{columns}x{rows}.ansi",
            "screenshot": f"screenshots/{index:02d}-tui-{theme}-{columns}x{rows}.png"}


def run(captures=False):
    checks, manifest = [], []
    def passed(text):
        checks.append(text); print(f"PASS U{len(checks):02d}: {text}", flush=True)
    with peer() as (sock, tty, cp):
        tty.send("请给出逐行读取文件的 C 示例。\r")
        assert question(sock, 1)[0] == "请给出逐行读取文件的 C 示例。".encode()
        status(sock, 1, "prefill", 0, 11.1, 0.1)
        tty.wait(lambda: "11.1 tok/s" in tty.visible(), "first cached status")
        before = tty.screen.text().count("tok/s")
        for rate in (12.2, 13.3, 22.2): status(sock, 1, "prefill", 0, rate, 0.2)
        tty.wait(lambda: "22.2 tok/s" in tty.visible(), "replacement cached status")
        visible = tty.visible()
        assert before == 1 and visible.count("tok/s") == 1, visible
        assert "11.1 tok/s" not in visible and "RAW_STATUS_MUST_NOT_APPEAR" not in visible and '"phase"' not in visible
        passed("STATUS JSON stays outside chat; successive updates replace one visible metadata line")

        thoughts = "先检查打开失败，再逐行读取。Check fopen before fgets."
        sock.sendall(frame(THINK, 1, 0, thoughts.encode()))
        status(sock, 1)
        tty.wait(lambda: "Check fopen before fgets." in tty.visible(), "visible reasoning")
        assert all(color == (102, 109, 121) for color in tty.screen.color_of("Check fopen before fgets."))
        answer = ("下面的例子保留错误检查：\n```c\n#include <stdio.h>\nint main(void) {\n"
                  "    FILE *fp = fopen(\"notes.txt\", \"r\");\n"
                  "    if (!fp) { perror(\"fopen\"); return 1; }\n"
                  "    char line[256];\n    while (fgets(line, sizeof line, fp))\n"
                  "        fputs(line, stdout);\n    fclose(fp);\n    return 0;\n}\n```\n"
                  "编译：cc -Wall -Wextra read.c -o read\n运行前请创建 notes.txt。")
        total = emit(sock, answer.encode())
        status(sock, 1, "completed", 128, 76.5, 1.7, offset=total)
        tty.wait(lambda: "finishing" in tty.visible(), "upstream completion before END")
        assert "completed" not in tty.visible() and checkpoint(cp)["done"] == 0, tty.visible()
        passed("upstream completed STATUS remains finishing until the C protocol emits END")
        sock.sendall(frame(END, 1, total))
        tty.wait(lambda: checkpoint(cp)["done"] == 1 and "completed" in tty.visible(), "first completed turn")
        tty.pause()
        assert all(color == (0, 0, 0) for color in tty.screen.color_of("#include <stdio.h>"))
        assert tty.visible().count("completed  /") == 1 and tty.visible().count("128 tok") == 1, tty.visible()
        passed("light theme renders THINK gray and formal answer black; END leaves one per-turn summary")

        tty.send("为什么必须检查 fopen？\r")
        assert question(sock, 2)[0] == "为什么必须检查 fopen？".encode()
        status(sock, 2, generated=24, rate=72.8, elapsed=0.4)
        total = emit(sock, "打开失败时 fp 是 NULL；检查后才能安全使用它。\n同一会话可以继续追问。".encode(), request=2)
        status(sock, 2, "completed", 24, 72.8, 0.4, offset=total)
        sock.sendall(frame(END, 2, total))
        tty.wait(lambda: checkpoint(cp)["request"] == 2 and checkpoint(cp)["done"] == 1, "second completed turn")
        tty.pause()
        visible = tty.visible()
        assert "YOU  /  1" in visible and "YOU  /  2" in visible, visible
        assert visible.count("completed  /") == 2 and visible.count("tok/s") == 2, visible
        passed("two turns retain distinct prompts, answers and exactly one final summary each")

        tty.send("/thinking\r")
        tty.wait(lambda: "thinking hidden" in tty.visible(), "hide reasoning")
        assert "Check fopen before fgets." not in tty.visible()
        no_request(sock)
        tty.send("/thinking\r")
        tty.wait(lambda: "Check fopen before fgets." in tty.visible(), "show reasoning again")
        no_request(sock)
        passed("/thinking changes only transcript presentation and restores the cached reasoning")

        draft = "草稿：请解释 fgets 的返回值🙂"
        tty.send(draft)
        tty.wait(lambda: draft in tty.screen.lines()[-1], "editable Unicode draft")
        for index, columns, rows in ((14, 140, 38), (15, 96, 28), (16, 60, 18), (17, 40, 10)):
            manifest.append(capture(tty, columns, rows, "light", index, captures))
            if columns == 40:
                assert "fgets" in tty.visible(), tty.visible()
        passed("one live PTY reflows at 140x38, 96x28, 60x18 and 40x10 while preserving its editable Unicode draft")

        before = tty.visible()
        tty.send(b"\x1b[5~" * 32)
        tty.wait(lambda: tty.visible() != before and "YOU  /  1" in tty.visible(), "PageUp transcript")
        assert "fgets" in tty.screen.lines()[-1]
        tty.send(b"\x1b[6~" * 32)
        tty.wait(lambda: "同一会话" in tty.visible(), "PageDown latest transcript")
        no_request(sock)
        passed("PgUp/PgDn scroll transcript history while preserving the draft and sending no request")

        # Clear only this test's draft before issuing the local command.
        tty.send(b"\x1b[H" + b"\x1b[3~" * len(draft))
        tty.pause()
        tty.send("/theme dark\r")
        tty.pause(); no_request(sock)
        tty.send(draft)
        manifest.append(capture(tty, 96, 28, "dark", 18, captures))
        assert all(color == (227, 230, 237) for color in tty.screen.color_of("NULL"))
        assert tty.screen.cells[-1][-1][2] == (22, 25, 31)
        passed("/theme dark is local; cached transcript and draft use the dark foreground/background palette")

        tty.send("\r")
        assert question(sock, 3)[0] == draft.encode(), "resizing or scrolling changed the actual submitted draft"
        sock.sendall(frame(END, 3, 0))
        tty.wait(lambda: checkpoint(cp)["request"] == 3 and checkpoint(cp)["done"] == 1, "draft third turn")
        tty.send("/quit\r"); tty.finish()
        passed("after all resizes and controls, Enter submits the exact original UTF-8 draft; exit restores termios")
        full_ansi = bytes(tty.output)
    with peer(idle_ms=1500) as (sock, tty, cp):
        tty.send("quiet peer timeout check\r")
        assert question(sock, 1)[0] == b"quiet peer timeout check"
        start = time.monotonic()
        tty.wait(lambda: "waiting for server data (1.5 s network idle limit)" in tty.plain(), "inline idle error")
        elapsed = time.monotonic() - start
        saved = checkpoint(cp)
        assert 1.25 <= elapsed <= 4.5, ("UI repaint ticks changed network idle deadline", elapsed)
        assert saved["done"] == 0 and saved["request"] == 1 and saved["offset"] == 0
        assert "waiting for server data (1.5 s network idle limit)" in tty.plain(), tty.plain()[-1000:]
        assert tty.proc.poll() is None
        tty.send("/quit\r"); tty.finish()
    passed("33ms UI ticks do not refresh the 1500ms network deadline; a silent peer leaves a pending checkpoint and an editable error screen")
    with peer(idle_ms=1500) as (sock, tty, cp):
        tty.send("job suspension with an already queued answer\r")
        question(sock, 1)
        tty.pause(0.08)
        # Exercise the same handler and foreground stop as Ctrl+Z, rather than
        # a SIGSTOP inside ppoll which can return queued events immediately.
        os.kill(tty.proc.pid, signal.SIGTSTP)
        try:
            tty.wait(lambda: Path(f"/proc/{tty.proc.pid}/stat").read_text().rsplit(")", 1)[1].split()[0] == "T",
                     "active client suspended")
            sock.sendall(frame(HEARTBEAT, 1, 0) + frame(DELTA, 1, 0, b"X") + frame(END, 1, 1))
            time.sleep(1.8)  # Longer than the controlled network idle limit.
        finally:
            os.kill(tty.proc.pid, signal.SIGCONT)
        tty.wait(lambda: checkpoint(cp)["done"] == 1, "queued answer after foreground resume")
        assert checkpoint(cp)["answer"] == b"X"
        assert "timed out" not in tty.plain()
        tty.send("/quit\r"); tty.finish()
    passed("active Ctrl+Z/foreground resume beyond the idle limit drains queued HEARTBEAT, DELTA and END without a false timeout")
    with peer(idle_ms=1500) as (sock, tty, cp):
        tty.send("partial frames still have a deadline\r")
        question(sock, 1)
        sock.sendall(frame(HEARTBEAT, 1, 0)[:7])  # Readable is not a complete frame.
        start = time.monotonic()
        tty.wait(lambda: "receiving a complete server frame (1.5 s frame deadline)" in tty.plain(), "inline frame error")
        elapsed = time.monotonic() - start
        assert 1.25 <= elapsed <= 4.5, ("partial frame bypassed transfer deadline", elapsed)
        assert checkpoint(cp)["done"] == 0 and checkpoint(cp)["offset"] == 0
        assert "receiving a complete server frame (1.5 s frame deadline)" in tty.plain(), tty.plain()[-1000:]
        assert tty.proc.poll() is None
        tty.send("/quit\r"); tty.finish()
    passed("a readable but incomplete frame retains its bounded transfer deadline and reports a frame timeout distinctly from network silence")
    if captures:
        (ROOT / "logs" / "tui-full.ansi").write_bytes(full_ansi)
        (ROOT / "logs" / "tui-layout-manifest.json").write_text(json.dumps({
            "captured_at": datetime.now(ZoneInfo("Asia/Shanghai")).isoformat(),
            "client_binary": str(ROOT / "bin/stream_client"),
            "client_size": (ROOT / "bin/stream_client").stat().st_size,
            "client_mtime_ns": (ROOT / "bin/stream_client").stat().st_mtime_ns,
            "method": "One actual all-stdio C-client PTY, controlled TCP fixture, successive TIOCSWINSZ; screenshots replay each actual redraw in an owned xterm.",
            "fixture": "Deterministic layout test data, not Qwen model output.",
            "checks": checks, "captures": manifest}, ensure_ascii=False, indent=2) + "\n")
        (ROOT / "logs" / "tui-layout.txt").write_text(
            "Command: python3 tests/verify_tui.py --capture\n"
            "Fixture: deterministic layout data; real C client, one all-stdio PTY, owned TCP peer.\n"
            + "\n".join(f"PASS U{index:02d}: {check}" for index, check in enumerate(checks, 1))
            + f"\nAll {len(checks)} TUI screen checks passed.\n")
    print(f"All {len(checks)} TUI screen checks passed.", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true", help="archive actual resize redraws under logs/")
    args = parser.parse_args()
    try: run(args.capture)
    except Exception:
        traceback.print_exc(); raise SystemExit(1)
