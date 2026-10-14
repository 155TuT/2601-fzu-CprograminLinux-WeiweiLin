#!/usr/bin/env python3
"""Isolated dashboard verification: read-only fixtures, real flock and real PTY."""

from pathlib import Path
import fcntl
import hashlib
import importlib.util
import json
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time
import traceback

sys.dont_write_bytecode = True
from verify import ROOT, Server, DELTA, receive, send_turn, wait_for

SCRIPT = ROOT / "scripts/server/dashboard.py"
spec = importlib.util.spec_from_file_location("task4_dashboard", SCRIPT)
dashboard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dashboard)
CHECKS = []
ANSI = re.compile(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b\][^\x07]*(?:\x07|\x1b\\)")


def passed(description):
    message = f"PASS D{len(CHECKS) + 1:02d}: {description}"
    CHECKS.append(message)
    print(message, flush=True)


def fixture(root, token, *, request=1, metrics=None, answer=b"", thinking=b"", terminal=None):
    session = root / token
    session.mkdir()
    (session / "session.lock").write_bytes(b"")
    (session / "events.log").write_text("pid=42 event=connected request=0 offset=0\n")
    if request:
        turn = session / f"turn-{request:08d}"
        turn.mkdir()
        (turn / "backend.txt").write_text("llama")
        (turn / "prompt.txt").write_text("PRIVATE question: 秘密问题🙂")
        (turn / "response.txt").write_bytes(answer)
        (turn / "thinking.txt").write_bytes(thinking)
        if metrics is not None:
            (turn / "metrics.json").write_text(json.dumps(metrics))
        if terminal:
            (turn / terminal).write_bytes(b"")
    return session


def index(document):
    return {item["session"]: item for item in document["sessions"]}


def fingerprint(root):
    result = {}
    for path in sorted(root.rglob("*")):
        info = path.lstat()
        if path.is_symlink():
            content = os.readlink(path)
        elif path.is_file():
            content = hashlib.sha256(path.read_bytes()).hexdigest()
        else:
            content = "nonregular"
        result[str(path.relative_to(root))] = (info.st_mode, info.st_size, info.st_mtime_ns,
                                              info.st_ino, content)
    return result


def run(root, *args):
    return subprocess.run([sys.executable, str(SCRIPT), "--state", str(root), *args],
                          capture_output=True, text=True, timeout=5,
                          env={**os.environ, "PYTHONDONTWRITEBYTECODE": "1"})


def test_latest_terminal_and_metrics():
    with tempfile.TemporaryDirectory(prefix="t4-dashboard-state-") as temporary:
        root = Path(temporary)
        token = "a" * 32
        session = fixture(root, token, metrics={"phase": "completed", "generated_tokens": 99}, terminal="done")
        turn = session / "turn-00000002"
        turn.mkdir()
        (turn / "backend.txt").write_text("llama")
        (turn / "prompt.txt").write_text("PRIVATE newest question")
        (turn / "thinking.txt").write_bytes(b"PRIVATE thought" * 20)
        (turn / "response.txt").write_bytes(b"")
        metrics = {"phase": "error", "generated_tokens": 2051, "prompt_tokens": 68,
                   "context_tokens": 4096, "tokens_per_second": 70.1,
                   "timings": {"predicted_n": 2048, "predicted_per_second": 70.65},
                   "stop_type": "limit", "error": "generation stopped at limit", "model": "Qwen3.5-4B"}
        (turn / "metrics.json").write_text(json.dumps(metrics))
        old = time.time() - 30
        os.utime(turn / "metrics.json", (old, old))
        os.utime(session / "turn-00000001", (time.time() + 100, time.time() + 100))
        fixture(root, "b" * 32, metrics={"phase": "generating"}, answer=b"partial", terminal="cancelled")
        fixture(root, "c" * 32, metrics={"phase": "error"}, answer=b"complete", terminal="done")
        fixture(root, "d" * 32, request=0)
        before = fingerprint(root)
        document = dashboard.snapshot(root)
        items = index(document)
        item = items[token]
        assert item["request"] == 2 and item["turn_count"] == 2, item
        assert item["phase"] == "error" and item["connection"] == "offline" and not item["terminal"]
        assert item["generated_tokens"] == 2048 and item["context_used_estimate"] == 2116
        assert item["tokens_per_second"] == 70.65 and item["stop_type"] == "limit"
        assert item["metrics_stale"] and item["metrics_age_seconds"] >= 29
        assert item["answer_bytes"] == 0 and item["thinking_bytes"] == len(b"PRIVATE thought" * 20)
        assert items["b" * 32]["phase"] == "cancelled" and items["b" * 32]["terminal"]
        assert items["c" * 32]["phase"] == "completed" and not items["c" * 32]["metrics_stale"]
        assert items["d" * 32]["phase"] == "idle"
        assert fingerprint(root) == before, "dashboard changed fixture contents or metadata"
    passed("numeric latest turn, terminal-marker precedence, offline/error snapshot and native token/context/rate metrics; state unchanged")


def test_real_flock_lifecycle():
    with tempfile.TemporaryDirectory(prefix="t4-dashboard-lock-") as temporary:
        root = Path(temporary)
        session = fixture(root, "e" * 32, metrics={"phase": "generating", "generated_tokens": 20}, thinking=b"private")
        code = "import fcntl,sys,time; f=open(sys.argv[1],'rb'); fcntl.flock(f,fcntl.LOCK_EX); print('LOCKED',flush=True); time.sleep(30)"
        process = subprocess.Popen([sys.executable, "-c", code, str(session / "session.lock")], stdout=subprocess.PIPE, text=True)
        try:
            assert process.stdout.readline().strip() == "LOCKED"
            item = index(dashboard.snapshot(root))["e" * 32]
            assert item["connection"] == "active" and item["worker_pid"] == process.pid, item
            assert item["phase"] == "thinking" and not item["metrics_stale"], item
            # Observing must not disturb the existing exclusive lock.
            with (session / "session.lock").open("rb") as handle:
                try:
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    raise AssertionError("dashboard released someone else's lock")
                except BlockingIOError:
                    pass
            os.kill(process.pid, signal.SIGSTOP)
            wait_for(lambda: dashboard.process_state(process.pid) == "T")
            item = index(dashboard.snapshot(root))["e" * 32]
            assert item["connection"] == "paused" and item["metrics_stale"], item
            os.kill(process.pid, signal.SIGCONT)
            wait_for(lambda: dashboard.process_state(process.pid) != "T")
            assert index(dashboard.snapshot(root))["e" * 32]["connection"] == "active"
        finally:
            os.kill(process.pid, signal.SIGCONT)
            process.terminate(); process.wait(timeout=3); process.stdout.close()
        assert index(dashboard.snapshot(root))["e" * 32]["connection"] == "offline"
    passed("real exclusive flock owner maps to active, stopped worker to paused, release to offline; observer preserves held lock")


def test_unsafe_files_and_privacy():
    with tempfile.TemporaryDirectory(prefix="t4-dashboard-safe-") as temporary:
        home = Path(temporary)
        root = home / "state"; root.mkdir()
        outside = home / "outside"; outside.mkdir()
        (outside / "metrics.json").write_text('{"model":"DO_NOT_READ_OUTSIDE"}')
        os.symlink(outside, root / ("f" * 32))
        a = fixture(root, "1" * 32, metrics=None)
        os.symlink(outside / "metrics.json", a / "turn-00000001/metrics.json")
        b = fixture(root, "2" * 32, metrics=None)
        os.mkfifo(b / "turn-00000001/metrics.json")
        c = fixture(root, "3" * 32, metrics=None)
        (c / "turn-00000001/metrics.json").write_bytes(b"{" + b"x" * 70000)
        d = fixture(root, "4" * 32, metrics={"phase": "generating", "model": "Qwen\x1b[31m\nsecret",
            "generated_tokens": float("nan"), "prompt_tokens": -1, "tokens_per_second": float("inf")})
        (d / "events.log").write_bytes(b"x" * 90000 + b"\npid=88 event=disconnected request=0 offset=0\npartial")
        e = fixture(root, "5" * 32, metrics=None)
        (e / "turn-00000001/metrics.json").write_text("{broken")
        fixture(root, "9" * 32, metrics={"generated_tokens": 10**400, "tokens_per_second": 10**400})
        started = time.monotonic()
        document = dashboard.snapshot(root)
        assert time.monotonic() - started < 2, "FIFO or giant metadata blocked observation"
        serialized = json.dumps(document, allow_nan=False)
        assert "DO_NOT_READ_OUTSIDE" not in serialized and "PRIVATE" not in serialized
        items = index(document)
        assert "f" * 32 not in items and document["warnings"]
        assert all(items[token * 32]["warnings"] for token in "1235")
        assert items["4" * 32]["generated_tokens"] is None and items["4" * 32]["tokens_per_second"] is None
        assert items["9" * 32]["generated_tokens"] is None and items["9" * 32]["tokens_per_second"] is None
        assert "\x1b" not in items["4" * 32]["model"]
        assert items["4" * 32]["last_event"]["event"] == "disconnected"
        preview = dashboard.snapshot(root, preview=True)
        assert index(preview)["1" * 32]["prompt_preview"].startswith("PRIVATE question")
        os.symlink(root, home / "linked-root")
        result = run(home / "linked-root", "--json")
        assert result.returncode == 1 and not result.stdout
    passed("symlink roots/sessions/files, FIFO, oversize/torn JSON/logs, nonfinite metrics and escape text safely handled; private bodies omitted unless preview enabled")


def test_headless_cli_and_layout():
    with tempfile.TemporaryDirectory(prefix="t4-dashboard-cli-") as temporary:
        root = Path(temporary)
        fixture(root, "6" * 32, metrics={"phase": "completed", "model": "模型🙂", "generated_tokens": 40}, answer=b"done", terminal="done")
        document = dashboard.snapshot(root)
        for columns, rows in ((18, 4), (60, 18), (80, 24), (140, 30)):
            lines = dashboard.layout(document, columns, rows)
            assert len(lines) <= rows and all(dashboard.width(text) <= columns for _, text in lines)
        assert dashboard.fit("前🙂后", 4) == "前…"
        result = run(root, "--json")
        assert result.returncode == 0, result.stderr
        value = json.loads(result.stdout)
        assert value["schema"] == 1 and value["sessions"][0]["phase"] == "completed"
        assert "PRIVATE" not in result.stdout
        once = run(root, "--once")
        assert once.returncode == 0 and "read-only session dashboard" in once.stdout
        assert "completed" in once.stdout and "PRIVATE" not in once.stdout
        explicit = run(root, "--json", "--preview")
        assert "PRIVATE question" in explicit.stdout
        missing = root / "not-created"
        failure = run(missing, "--json")
        assert failure.returncode == 1 and not missing.exists()
        failure = run(root)
        assert failure.returncode == 2 and "--once or --json" in failure.stderr
    passed("--once/--json, opt-in preview, missing/non-TTY errors and cell-safe layouts at 18/60/80/140 columns")


def test_real_server_progress():
    with tempfile.TemporaryDirectory(prefix="t4-dashboard-cserver-") as temporary:
        home = Path(temporary)
        server = Server(home, source=("进度🙂" * 40).encode(), rate=30)
        try:
            with server.connection() as (client, token):
                initial = index(dashboard.snapshot(server.state))[token.decode()]
                assert initial["connection"] == "active" and initial["phase"] == "idle", initial
                send_turn(client, b"dashboard question")
                while receive(client)[0] != DELTA:
                    pass
                item = wait_for(lambda: next((s for s in dashboard.snapshot(server.state)["sessions"]
                                             if s["answer_bytes"] > 0), None))
                assert item["phase"] == "generating" and item["connection"] == "active" and not item["terminal"], item
                assert item["worker_pid"] is not None and item["backend"] == "llama"
            wait_for(lambda: index(dashboard.snapshot(server.state))[token.decode()]["connection"] == "offline")
            item = index(dashboard.snapshot(server.state))[token.decode()]
            assert item["phase"] == "generating" and not item["terminal"] and item["answer_bytes"] > 0
        finally:
            server.close()
    passed("real C worker idle/generating progress and disconnect remain distinct from completion; random-port/temp-state only")


def test_curses_resize_and_restore():
    with tempfile.TemporaryDirectory(prefix="t4-dashboard-pty-") as temporary:
        root = Path(temporary)
        fixture(root, "7" * 32, metrics={"phase": "error", "model": "模型🙂"})
        fixture(root, "8" * 32, metrics={"phase": "completed"}, terminal="done")
        expected = dashboard.snapshot(root)["sessions"][1]["session"]
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 140, 0, 0))

        def session():
            os.setsid(); fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        process = subprocess.Popen([sys.executable, str(SCRIPT), "--state", str(root)],
            stdin=slave, stdout=slave, stderr=slave, preexec_fn=session,
            env={**os.environ, "TERM": "xterm-256color", "PYTHONDONTWRITEBYTECODE": "1"})
        output = bytearray()

        def drain():
            while select.select([master], [], [], 0.02)[0]:
                data = os.read(master, 65536)
                if not data:
                    break
                output.extend(data)
            return ANSI.sub(b"", bytes(output)).decode("utf-8", "replace")

        try:
            wait_for(lambda: "read-only session dashboard" in drain())
            assert not termios.tcgetattr(slave)[3] & termios.ICANON
            start = len(output)
            os.write(master, b"j")
            wait_for(lambda: expected in ANSI.sub(b"", bytes(output[start:])).decode("utf-8", "replace") if drain() else False)
            # Let one actual timed refresh observe a new terminal marker without
            # losing selection when the sort order changes underneath the user.
            selected_turn = root / expected / "turn-00000001"
            (selected_turn / "metrics.json").write_text(json.dumps({"phase": "completed", "generated_tokens": 321, "model": "REFRESH_VISIBLE"}))
            (selected_turn / "done").touch()
            start = len(output)
            wait_for(lambda: "REFRESH_VISIBLE" in ANSI.sub(b"", bytes(output[start:])).decode("utf-8", "replace") if drain() else False)
            for columns, rows in ((60, 18), (80, 24), (18, 4), (140, 30)):
                start = len(output)
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, columns, 0, 0))
                os.kill(process.pid, signal.SIGWINCH)
                wait_for(lambda: len(output) > start if drain() else False)
                assert process.poll() is None
            os.write(master, b"q")
            process.wait(timeout=3); drain()
            assert process.returncode == 0
            assert termios.tcgetattr(slave) == original, "curses did not restore termios"
            bytes(output).decode("utf-8", "strict")
            assert "PRIVATE" not in drain()
        finally:
            if process.poll() is None:
                process.terminate(); process.wait(timeout=3)
            os.close(master); os.close(slave)
    passed("all-stdio curses PTY selects/preserves selection across timed refresh, resizes 60/80/tiny/140 columns, preserves UTF-8 and restores termios on q")


def main():
    started = time.monotonic()
    log = ROOT / "logs/verification-dashboard.txt"
    try:
        for test in (test_latest_terminal_and_metrics, test_real_flock_lifecycle,
                     test_unsafe_files_and_privacy, test_headless_cli_and_layout,
                     test_real_server_progress, test_curses_resize_and_restore):
            test()
        summary = f"All {len(CHECKS)} dashboard checks passed ({time.monotonic() - started:.1f}s)."
        print(summary, flush=True)
        log.write_text("\n".join(CHECKS + [summary]) + "\n")
    except BaseException:
        log.write_text("\n".join(CHECKS) + "\nFAILED\n" + traceback.format_exc())
        raise


if __name__ == "__main__":
    main()
