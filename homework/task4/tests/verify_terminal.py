#!/usr/bin/env python3
"""All-stdio PTY regressions for the actual editable streaming client.

Tests own every terminal, port, input and session directory. No user's active
terminal, checkpoint, service or process is touched.
"""

from pathlib import Path
import errno
import fcntl
import os
import pty
import re
import select
import shlex
import signal
import struct
import subprocess
import tempfile
import termios
import time
import traceback
from unittest.mock import Mock

from verify import ROOT, Server, checkpoint, lock_released, wait_for

CHECKS = []
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07]*(?:\x07|\x1b\\)")


def passed(message):
    line = f"PASS T{len(CHECKS) + 1:02}: {message}"
    CHECKS.append(line)
    print(line, flush=True)


class TTY:
    def __init__(self, port, *, cp=None, session_dir=None, extra=(), shell=False):
        self.master, self.slave = pty.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 96, 0, 0))
        self.original = termios.tcgetattr(self.slave)
        self.output = bytearray()
        self.eof = False
        self.closed = False
        self.shell = shell
        args = [str(ROOT / "bin/stream_client"), "--port", str(port)]
        if cp is not None:
            args.extend(["--checkpoint", str(cp)])
        if session_dir is not None:
            args.extend(["--session-dir", str(session_dir)])
        args.extend(map(str, extra))

        def session():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        env = {**os.environ, "TERM": "xterm-256color", "PS1": "__PTY_SHELL__ ", "PROMPT_COMMAND": ""}
        self.proc = subprocess.Popen(["bash", "--noprofile", "--norc", "-i"] if shell else args,
            cwd=ROOT, stdin=self.slave, stdout=self.slave, stderr=self.slave,
            preexec_fn=session, env=env)
        self.client_pid = self.proc.pid
        if shell:
            self.wait(lambda: "__PTY_SHELL__" in self.plain(), "test shell prompt")
            self.send(shlex.join(args) + "\r")

            def child():
                path = Path(f"/proc/{self.proc.pid}/task/{self.proc.pid}/children")
                for item in path.read_text().split():
                    try:
                        if Path(f"/proc/{item}/comm").read_text().strip() == "stream_client":
                            return int(item)
                    except FileNotFoundError:
                        pass
                return None
            self.client_pid = self.wait(child, "shell client process")

    def drain(self, delay=0):
        ready, _, _ = select.select([self.master], [], [], delay)
        while ready:
            try:
                data = os.read(self.master, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    self.eof = True
                    return
                raise
            if not data:
                self.eof = True
                return
            self.output.extend(data)
            ready, _, _ = select.select([self.master], [], [], 0)

    def plain(self):
        self.drain()
        return ANSI.sub(b"", bytes(self.output)).decode("utf-8", "replace")

    def send(self, text):
        data = text.encode("utf-8") if isinstance(text, str) else text
        view = memoryview(data)
        while view:
            count = os.write(self.master, view)
            view = view[count:]

    def wait(self, predicate, description, timeout=8):
        end = time.monotonic() + timeout
        while True:
            # Observe exit before the final drain/predicate. The process may
            # exit while the condition is checked, including finish()'s poll.
            returncode = self.proc.poll()
            self.drain(0.02)
            result = predicate()
            if result:
                return result
            assert returncode is None, (description, self.plain()[-1800:])
            assert time.monotonic() < end, (f"timeout: {description}", self.plain()[-1800:])

    def pause(self, seconds=0.25):
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            self.drain(min(0.02, until - time.monotonic()))

    def finish(self, expected=0):
        self.wait(lambda: self.proc.poll() is not None, "client exit")
        self.drain()
        assert self.proc.returncode == expected, (self.proc.returncode, self.plain()[-1800:])
        bytes(self.output).decode("utf-8", "strict")  # Horizontal views must not start inside a character.
        assert termios.tcgetattr(self.slave) == self.original, "client did not restore original termios"

    def close(self):
        if self.closed:
            return
        self.closed = True
        if self.proc.poll() is None:
            if self.shell:
                # Only this test-owned job/session is stopped/terminated.
                try:
                    os.kill(self.client_pid, signal.SIGCONT)
                    os.kill(self.client_pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                self.send("exit\r")
            else:
                self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.wait(timeout=3)
        self.drain()
        os.close(self.master); os.close(self.slave)


def ready(tty, cp):
    return tty.wait(lambda: cp.exists() and checkpoint(cp), "initialized checkpoint")


def editor_row(tty, rows):
    """Read the last complete fixed editor paint, independent of transcript wrap."""
    tty.drain()
    data = bytes(tty.output)
    start = data.rfind(f"\x1b[{rows};1H".encode())
    if start < 0:
        return None
    tail = data[start + len(f"\x1b[{rows};1H"):]
    cursor = re.search(rb"\x1b\[" + str(rows).encode() + rb";[0-9]+H", tail)
    if cursor is None:
        return None
    return ANSI.sub(b"", tail[:cursor.start()]).decode("utf-8", "strict")


def state(tty, cp, request, done=1):
    def saved():
        value = checkpoint(cp)
        if value and value["request"] == request and value["done"] == done:
            return value
        return None
    return tty.wait(saved, f"turn {request} terminal={done}")


def submit(tty, cp, text, request):
    tty.send(text + "\r")
    return state(tty, cp, request)


def test_wait_exit_race():
    tty = TTY.__new__(TTY)
    tty.proc = Mock()
    tty.proc.poll.side_effect = [None, 0]
    tty.drain = Mock()
    tty.plain = Mock(return_value="")
    assert tty.wait(lambda: tty.proc.poll() is not None, "client exit")
    passed("waiter accepts a normal exit between process checks")


def test_unicode_editing_history():
    with tempfile.TemporaryDirectory(prefix="task4-tty-edit-") as temp:
        s = Server(temp, source=b"reply", rate=0)
        cp = Path(temp) / "edit.ckpt"
        tty = TTY(s.port, cp=cp)
        try:
            ready(tty, cp)
            tty.send("你好吗X")
            tty.send(b"\x7f\x1b[D\x1b[3~\x1b[H")  # erase X; left/delete a whole Chinese character; Home.
            tty.send("问：")
            tty.send(b"\x1b[F")
            tty.send("🙂\r")
            first = state(tty, cp, 1)
            assert first["prompt"] == "问：你好🙂".encode(), first["prompt"]
            tty.send(b"\x1b[A\x1b[D\x1b[3~")  # Recall history; delete the whole emoji.
            tty.send("！\r")
            second = state(tty, cp, 2)
            assert second["prompt"] == "问：你好！".encode(), second["prompt"]
            tty.send("草稿")
            tty.send(b"\x1b[A\x1b[B\r")  # History Down restores the editable draft.
            third = state(tty, cp, 3)
            assert third["prompt"] == "草稿".encode(), third["prompt"]
            tty.send("/quit\r"); tty.finish()
        finally:
            tty.close(); s.close()
    passed("all-stdio PTY: Unicode backspace/left/delete, Home/End and history edit whole characters; normal exit restores termios")


def test_async_draft_and_paste():
    with tempfile.TemporaryDirectory(prefix="task4-tty-stream-") as temp:
        s = Server(temp, source=b"abcdefghijklmnopqrstuvwx", rate=12)
        cp = Path(temp) / "draft.ckpt"
        tty = TTY(s.port, cp=cp)
        try:
            ready(tty, cp)
            tty.send("first streaming question\r")
            tty.wait(lambda: checkpoint(cp)["request"] == 1 and checkpoint(cp)["offset"] > 0,
                     "first response streaming")
            draft = "异步草稿X".encode()
            before = checkpoint(cp)["offset"]
            tty.send(draft[:1])  # Incomplete UTF-8 is still pending when output redraws.
            tty.wait(lambda: checkpoint(cp)["offset"] > before, "output between UTF-8 input bytes")
            tty.send(draft[1:] + b"\x7f")
            tty.send(b"\x1b[D")
            tty.send("🙂")
            expected = "异步草🙂稿".encode()
            fcntl.ioctl(tty.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 56, 0, 0))
            # The transcript viewport reflows instead of assigning a DEC
            # scrolling region. Its fixed editor must move to the new last
            # row; the checkpoint below verifies the same draft is preserved.
            tty.wait(lambda: b"\x1b[30;1H" in tty.output, "resized bottom editor row")
            state(tty, cp, 1)
            tty.pause()
            assert checkpoint(cp)["request"] == 1, "an unfinished draft was auto-submitted at END"
            # All model output and the END transition happened on the same tty.
            # A single Enter must submit precisely the draft that was being edited.
            tty.send("\r")
            assert state(tty, cp, 2)["prompt"] == expected
            tty.send(b"\x1b[200~" + "粘贴首行\n粘贴次行".encode() + b"\x1b[201~")
            tty.pause()
            assert checkpoint(cp)["request"] == 2, "pasted newline auto-submitted a question"
            tty.send("\r")
            pasted = state(tty, cp, 3)["prompt"]
            assert "粘贴首行".encode() in pasted and "粘贴次行".encode() in pasted and b"\x1b" not in pasted
            tty.send("/quit\r"); tty.finish()
        finally:
            tty.close(); s.close()
    passed("Unicode draft survives asynchronous streaming, resize and END; bracketed multiline paste stays editable and requires explicit Enter")


def test_ctrl_c_reuse_and_restore():
    with tempfile.TemporaryDirectory(prefix="task4-tty-cancel-") as temp:
        s = Server(temp, source=b"abcdefghijklmnopqrstuvwx", rate=12)
        cp = Path(temp) / "cancel.ckpt"
        tty = TTY(s.port, cp=cp)
        try:
            ready(tty, cp)
            tty.send("cancel this stream\r")
            tty.wait(lambda: checkpoint(cp)["request"] == 1 and checkpoint(cp)["offset"] > 0, "active answer")
            tty.send(b"\x03")
            saved = state(tty, cp, 1, done=2)
            assert tty.proc.poll() is None and 0 < saved["offset"] < 24
            assert submit(tty, cp, "reuse after Ctrl+C", 2)["prompt"] == b"reuse after Ctrl+C"
            tty.send("/quit\r"); tty.finish()
            # Also restore the tty on non-interactive termination during output.
            tty.close()
            tty = TTY(s.port, cp=Path(temp) / "terminate.ckpt")
            ready(tty, Path(temp) / "terminate.ckpt")
            tty.send("terminate while streaming\r")
            tty.wait(lambda: checkpoint(Path(temp) / "terminate.ckpt")["offset"] > 0, "termination answer")
            tty.proc.terminate(); tty.finish(expected=143)
        finally:
            tty.close(); s.close()
    passed("real tty Ctrl+C cancels and keeps the connection reusable; normal and SIGTERM exits restore terminal settings")


def test_default_sessions_resume_lock():
    with tempfile.TemporaryDirectory(prefix="task4-default-sessions-") as temp:
        home = Path(temp)
        s = Server(temp, source=b"reply", rate=0)
        directory = home / "clients"
        a = TTY(s.port, session_dir=directory)
        b = TTY(s.port, session_dir=directory)
        resumed = None
        try:
            a.wait(lambda: len(list(directory.glob("session-*/checkpoint.ckpt"))) == 2,
                   "two auto-created session checkpoints")
            paths = list(directory.glob("session-*/checkpoint.ckpt"))
            identities = [checkpoint(p) for p in paths]
            assert all(c["request"] == 0 for c in identities)
            assert identities[0]["token"] != identities[1]["token"]
            # Locate each client's path through its held lock inode, without
            # relying on screen output/layout or creation order.
            def owned(process):
                for fd in Path(f"/proc/{process.pid}/fd").iterdir():
                    try:
                        target = os.readlink(fd)
                    except FileNotFoundError:
                        continue
                    if target.endswith("checkpoint.ckpt.lock"):
                        return Path(target[:-5])
                return None
            ap = a.wait(lambda: owned(a.proc), "first client lock")
            bp = b.wait(lambda: owned(b.proc), "second client lock")
            assert ap != bp and ap in paths and bp in paths
            failed = subprocess.run([str(ROOT / "bin/stream_client"), "--port", str(s.port),
                                     "--resume", str(ap)], cwd=ROOT, stdin=subprocess.DEVNULL,
                                    capture_output=True, timeout=4)
            assert failed.returncode != 0
            message = failed.stderr.lower()
            assert b"checkpoint" in message and any(word in message for word in
                (b"busy", b"lock", b"in use", b"held", b"owns", b"another client")), message
            assert checkpoint(ap)["request"] == 0 and a.proc.poll() is None
            submit(a, ap, "session A question", 1)
            submit(b, bp, "session B question", 1)
            a.send("/quit\r"); a.finish()
            b.send("/quit\r"); b.finish()
            wait_for(lambda: lock_released(s.state / checkpoint(ap)["token"] / "session.lock"),
                     description="server session release")
            resumed = TTY(s.port, extra=("--resume", str(ap)))
            resumed.wait(lambda: "question" in resumed.plain().lower(), "explicit resumed prompt")
            again = submit(resumed, ap, "after resume", 2)
            assert again["token"] == identities[0]["token"] or again["token"] == identities[1]["token"]
            context = (s.state / again["token"] / "context.txt").read_bytes()
            assert b"session A question" in context and b"session B question" not in context
            resumed.send("/quit\r"); resumed.finish()
            absent = home / "must-not-be-created.ckpt"
            failed = subprocess.run([str(ROOT / "bin/stream_client"), "--port", str(s.port),
                                     "--resume", str(absent)], cwd=ROOT, stdin=subprocess.DEVNULL,
                                    capture_output=True, timeout=4)
            assert failed.returncode != 0 and not absent.exists()
        finally:
            if resumed:
                resumed.close()
            a.close(); b.close(); s.close()
    passed("identical default commands create independent auto sessions; explicit resume preserves history; held lock and missing resume fail without changing saved data")


def test_job_control_suspend_resume():
    with tempfile.TemporaryDirectory(prefix="task4-tty-jobs-") as temp:
        s = Server(temp, source=b"reply", rate=0)
        cp = Path(temp) / "jobs.ckpt"
        tty = TTY(s.port, cp=cp, shell=True)
        try:
            ready(tty, cp)
            tty.send("暂停后草稿")
            tty.pause(0.05)
            tty.send(b"\x1a")  # Real terminal SIGTSTP, in a non-orphaned shell job group.

            def stopped():
                text = Path(f"/proc/{tty.client_pid}/stat").read_text().rsplit(")", 1)[1].split()
                return text[0] == "T"
            tty.wait(stopped, "Ctrl+Z stopped job")
            assert not lock_released(Path(str(cp) + ".lock")), "suspend incorrectly released checkpoint lock"
            tty.wait(lambda: "Stopped" in tty.plain(), "shell job notification")
            tty.send("fg\r")
            tty.wait(lambda: not stopped(), "foreground resumed job")
            tty.send("\r")
            saved = state(tty, cp, 1)
            assert saved["prompt"] == "暂停后草稿".encode(), saved["prompt"]
            tty.send("/quit\r")
            tty.wait(lambda: not Path(f"/proc/{tty.client_pid}").exists(), "client exit after fg")
            # An ordinary subsequent shell command verifies the terminal was
            # restored to usable editing/echo after suspension and final exit.
            tty.send("printf '__SHELL_WORKS__\\n'\r")
            tty.wait(lambda: "__SHELL_WORKS__" in tty.plain(), "usable shell after client exit")
        finally:
            tty.close(); s.close()
    passed("real shell job-control Ctrl+Z/fg restores the input UI and Unicode draft while retaining the session lock; shell stays usable after exit")


def test_long_editing_and_idle_interrupt():
    with tempfile.TemporaryDirectory(prefix="task4-tty-long-edit-") as temp:
        s = Server(temp, source=b"reply", rate=0)
        cp = Path(temp) / "long.ckpt"
        tty = TTY(s.port, cp=cp)
        try:
            ready(tty, cp)
            long_text = "长文本🙂" * 30
            tty.send(long_text)
            tty.wait(lambda: b"<" in tty.output, "horizontal scroll indicator")
            fcntl.ioctl(tty.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 18, 40, 0, 0))
            tty.wait(lambda: b"\x1b[18;1H" in tty.output, "narrow bottom editor redraw")
            tty.send(b"\x01")  # Ctrl+A
            tty.send("前")
            tty.send(b"\x05")  # Ctrl+E
            tty.send("尾\r")
            assert state(tty, cp, 1)["prompt"] == ("前" + long_text + "尾").encode()
            tty.send("alpha 删除词 tail")
            tty.send(b"\x17\x17")  # Ctrl+W twice removes the ASCII and Unicode words.
            tty.send("keep suffix")
            tty.send(b"\x01" + b"\x1b[C" * 6 + b"\x15")  # Ctrl+U erases "alpha ".
            tty.send(b"\x05\x01" + b"\x1b[C" * 4 + b"\x0b")  # Ctrl+K erases " suffix".
            tty.send(b"!\x01<\x05>\r")
            assert state(tty, cp, 2)["prompt"] == b"<keep!>"
            tty.send("必须清掉的草稿")
            tty.wait(lambda: "必须清掉的草稿" in (editor_row(tty, 18) or ""), "idle draft displayed")
            tty.send(b"\x03")
            # No subsequent key is sent until the cancellation handler has
            # visibly cleared the idle draft; this catches the poll flag race.
            # Compare the editor itself: a 40-column transcript may reflow the
            # control notice in the middle of a word.
            tty.wait(lambda: (editor_row(tty, 18) or "").strip() == "question>",
                     "idle Ctrl+C clears without an extra wakeup key", timeout=3)
            assert checkpoint(cp)["request"] == 2
            assert submit(tty, cp, "清空后新问题", 3)["prompt"] == "清空后新问题".encode()
            tty.send("/quit\r"); tty.finish()
        finally:
            tty.close(); s.close()
    passed("long Unicode draft scrolls/resizes without loss; Ctrl+A/E/U/K/W edit correctly; idle Ctrl+C clears immediately without waiting for another key")


def main():
    log = ROOT / "logs/verification-terminal.txt"
    log.parent.mkdir(exist_ok=True)
    started = time.monotonic()
    try:
        for test in (test_wait_exit_race, test_unicode_editing_history, test_async_draft_and_paste,
                     test_ctrl_c_reuse_and_restore, test_default_sessions_resume_lock,
                     test_job_control_suspend_resume, test_long_editing_and_idle_interrupt):
            test()
        summary = f"All {len(CHECKS)} terminal checks passed ({time.monotonic() - started:.1f}s)."
        print(summary, flush=True)
        log.write_text("\n".join(CHECKS + [summary]) + "\n")
    except BaseException:
        log.write_text("\n".join(CHECKS) + "\nFAILED\n" + traceback.format_exc())
        raise


if __name__ == "__main__":
    main()
