#!/usr/bin/env python3
"""真实 PTY / C 客户端 / TCP 对端验证 thinking、打断、纠偏和恢复。

测试语料、终端、checkpoint 和输入全部位于临时目录；仅依赖标准库。
"""

from contextlib import contextmanager
from pathlib import Path
import fcntl
import os
import pty
import re
import socket
import subprocess
import tempfile
import termios
import time
import traceback

from verify import (ROOT, HELLO, WELCOME, BEGIN, INPUT, COMMIT, DELTA, END,
                    HEARTBEAT, frame, receive, checkpoint, wait_for, Server,
                    lock_released, send_turn, child_running)

THINK, CANCEL, CANCELLED, STATUS = 10, 11, 12, 13
CHECKS = []


def passed(message):
    line = f"PASS {len(CHECKS) + 1:02d}: {message}"
    CHECKS.append(line)
    print(line, flush=True)


class Terminal:
    def __init__(self, home, port, name="tty", cp=None, extra=()):
        self.home = Path(home)
        self.cp = Path(cp) if cp else self.home / f"{name}.ckpt"
        self.out = self.home / f"{name}.stdout"
        self.err = self.home / f"{name}.stderr"
        self.handles = self.out.open("wb"), self.err.open("wb")
        self.master, slave = pty.openpty()
        attributes = termios.tcgetattr(slave)
        attributes[3] &= ~(termios.ECHO | termios.ECHONL)
        termios.tcsetattr(slave, termios.TCSANOW, attributes)

        def terminal_session():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.proc = subprocess.Popen([
            str(ROOT / "bin/stream_client"), "--port", str(port),
            "--checkpoint", str(self.cp),
            *extra,
        ], cwd=ROOT, stdin=slave, stdout=self.handles[0], stderr=self.handles[1],
           preexec_fn=terminal_session)
        os.close(slave)

    def send(self, text):
        data = text.encode() if isinstance(text, str) else text
        os.write(self.master, data)

    def wait(self, text):
        def observed():
            errors = self.err.read_text()
            if text in errors:
                return errors
            assert self.proc.poll() is None, errors
            return False
        return wait_for(observed, description=text)

    def finish(self, code=0):
        actual = self.proc.wait(timeout=8)
        assert actual == code, (actual, self.err.read_text())
        return self.out.read_bytes(), self.err.read_text(), checkpoint(self.cp)

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill(); self.proc.wait(timeout=3)
        os.close(self.master)
        for handle in self.handles:
            handle.close()


@contextmanager
def peer(extra=()):
    with tempfile.TemporaryDirectory(prefix="task4-controls-") as temp:
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(2)
            listener.settimeout(6)
            tty = Terminal(temp, listener.getsockname()[1], extra=extra)
            try:
                with listener.accept()[0] as sock:
                    sock.settimeout(6)
                    assert receive(sock)[0] == HELLO
                    sock.sendall(frame(WELCOME, payload=b"a" * 32))
                    tty.wait("question (")
                    yield Path(temp), listener, sock, tty
            finally:
                tty.close()


def question(sock, expected_id):
    kind, request, offset, payload = receive(sock)
    assert kind == BEGIN and request == expected_id and not payload
    data = bytearray()
    while True:
        kind, request, at, payload = receive(sock)
        assert request == expected_id and at == len(data), (kind, request, at, data)
        if kind == COMMIT:
            assert not payload
            return bytes(data), offset
        assert kind == INPUT and payload
        data.extend(payload)


def emit(sock, data, request=1, offset=0):
    for character in data.decode():
        raw = character.encode()
        sock.sendall(frame(DELTA, request, offset, raw))
        offset += len(raw)
    return offset


def test_thinking_stop():
    with peer() as (_, _, sock, tty):
        tty.send("initial question\n")
        assert question(sock, 1)[0] == b"initial question"
        sock.sendall(frame(STATUS, 1, 0, b"queued locally") +
                     frame(THINK, 1, 7, "想".encode()) +
                     frame(THINK, 1, 10, "🙂".encode()) + frame(DELTA, 1, 0, b"A"))
        tty.wait("[answer]")
        assert checkpoint(tty.cp)["answer"] == b"A"
        tty.send("/stop\n")
        assert receive(sock) == (CANCEL, 1, 1, b"")
        # A final delta already in flight remains part of the durable prefix.
        sock.sendall(frame(DELTA, 1, 1, b"B") + frame(CANCELLED, 1, 2))
        tty.wait("cancelled turn=1")
        saved = checkpoint(tty.cp)
        assert saved["done"] == 2 and saved["answer"] == b"AB" and saved["offset"] == 2
        assert tty.proc.poll() is None
        tty.send("next question\n")
        assert question(sock, 2)[0] == b"next question"
        sock.sendall(frame(END, 2, 0))
        tty.wait("completed turn=2")
        tty.send("/quit\n")
        out, errors, saved = tty.finish()
        assert out == b"AB\n\n" and saved["done"] == 1 and saved["request"] == 2
        assert "想🙂" in errors and "[thinking]" in errors
        assert "queued locally" not in errors and "[status]" not in errors
    passed("PTY /stop preserves the connection; THINK and STATUS remain outside final checkpoint; in-flight answer prefix and CANCELLED persist")


def test_status_display_policy():
    legacy = b"legacy progress marker"
    structured = b'{"phase":"generating","model":"fixture","generated_tokens":3}'
    for extra in ((), ("--verbose-status",), ("--plain",), ("--plain", "--verbose-status")):
        with peer(extra=extra) as (_, _, sock, tty):
            tty.send("status display policy\n")
            assert question(sock, 1)[0] == b"status display policy"
            sock.sendall(frame(STATUS, 1, 0, legacy) + frame(STATUS, 1, 0, structured)
                         + frame(THINK, 1, 0, "推理".encode()) + frame(DELTA, 1, 0, b"R")
                         + frame(END, 1, 1))
            tty.wait("completed turn=1")
            tty.send("/quit\n")
            out, errors, saved = tty.finish()
            assert out == b"R\n" and b"\x1b" not in out, (extra, out)
            assert saved["done"] == 1 and saved["answer"] == b"R" and saved["offset"] == 1
            assert "推理" in errors and "[thinking]" in errors
            if "--verbose-status" in extra:
                assert legacy.decode() in errors and structured.decode() in errors, (extra, errors)
                assert errors.count("[status]") == 2, (extra, errors)
            else:
                assert legacy.decode() not in errors and structured.decode() not in errors, (extra, errors)
                assert "[status]" not in errors, (extra, errors)
    # Also exercise --plain when every stream really is a terminal. It must
    # bypass alternate-screen rendering rather than merely rely on redirection.
    from verify_terminal import TTY
    for extra in (("--plain",), ("--plain", "--verbose-status")):
        with tempfile.TemporaryDirectory(prefix="task4-status-plain-") as temp:
            cp = Path(temp) / "plain.ckpt"
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(6)
                tty = TTY(listener.getsockname()[1], cp=cp, extra=extra)
                try:
                    with listener.accept()[0] as sock:
                        sock.settimeout(6)
                        assert receive(sock)[0] == HELLO
                        sock.sendall(frame(WELCOME, payload=b"a" * 32))
                        tty.wait(lambda: cp.exists() and checkpoint(cp), "plain checkpoint ready")
                        tty.send("all-stdio plain question\r")
                        assert question(sock, 1)[0] == b"all-stdio plain question"
                        sock.sendall(frame(STATUS, 1, 0, legacy) + frame(STATUS, 1, 0, structured)
                                     + frame(THINK, 1, 0, "推理".encode()) + frame(DELTA, 1, 0, b"R")
                                     + frame(END, 1, 1))
                        tty.wait(lambda: checkpoint(cp)["done"] == 1, "plain turn complete")
                        tty.send("/quit\r"); tty.finish()
                        text = tty.plain()
                        assert b"\x1b[?1049h" not in tty.output and b"\x1b[?2004h" not in tty.output
                        assert "推理" in text and "[thinking]" in text
                        assert checkpoint(cp)["answer"] == b"R" and checkpoint(cp)["done"] == 1
                        if "--verbose-status" in extra:
                            assert legacy.decode() in text and structured.decode() in text and text.count("[status]") == 2
                        else:
                            assert legacy.decode() not in text and structured.decode() not in text and "[status]" not in text
                finally:
                    tty.close()
    passed("redirected stdout is final-only; all-stdio --plain bypasses TUI; legacy/JSON STATUS are hidden by default and both appear only with --verbose-status")


def test_steer_terminal_race():
    with peer() as (_, _, sock, tty):
        tty.send("Original problem\n")
        assert question(sock, 1)[0] == b"Original problem"
        sock.sendall(frame(THINK, 1, 0, b"considering") + frame(DELTA, 1, 0, b"A"))
        tty.wait("[answer]")
        tty.send("/steer Use Chinese only\n")
        assert receive(sock) == (CANCEL, 1, 1, b"")
        queued = checkpoint(tty.cp)["queued_prompt"]
        assert b"Original problem" in queued and b"Use Chinese only" in queued
        # Natural completion wins; a late duplicate cancellation ACK must not
        # corrupt the following request's state machine.
        sock.sendall(frame(END, 1, 1) + frame(END, 1, 1))
        corrected, _ = question(sock, 2)
        assert corrected == queued
        assert not Path(str(tty.cp) + ".steer").exists()
        total = emit(sock, "完成".encode(), request=2)
        sock.sendall(frame(END, 2, total))
        tty.wait("completed turn=2")
        tty.send("/quit\n")
        out, _, saved = tty.finish()
        assert out == b"A\n" + "完成\n".encode()
        assert saved["prompt"] == corrected and saved["answer"] == "完成".encode()
    passed("steering is durable before CANCEL; merged correction starts the next turn; END/CANCEL race and duplicate old terminal do not desynchronize")


def test_ctrl_c_and_quit():
    with peer() as (_, _, sock, tty):
        tty.send("think slowly\n")
        question(sock, 1)
        sock.sendall(frame(THINK, 1, 0, b"thinking"))
        tty.wait("[thinking]")
        tty.send(b"\x03")  # The PTY line discipline sends SIGINT to its foreground process.
        assert receive(sock) == (CANCEL, 1, 0, b"")
        sock.sendall(frame(CANCELLED, 1, 0))
        tty.wait("cancelled turn=1")
        assert tty.proc.poll() is None
        tty.send("another question\n")
        question(sock, 2)
        sock.sendall(frame(DELTA, 2, 0, b"x"))
        wait_for(lambda: checkpoint(tty.cp)["answer"] == b"x")
        tty.send("/quit\n")
        assert receive(sock) == (CANCEL, 2, 1, b"")
        sock.sendall(frame(CANCELLED, 2, 1))
        out, _, saved = tty.finish()
        assert out == b"\nx\n" and saved["done"] == 2 and saved["request"] == 2
    passed("actual PTY Ctrl+C cancels thinking and remains connected; /quit during an answer waits for cancellation ACK then exits normally")


def test_steer_limit():
    with peer() as (_, _, sock, tty):
        tty.send(b"x" * 4050 + b"\n")
        assert len(question(sock, 1)[0]) == 4050
        sock.sendall(frame(THINK, 1, 0, b"thinking"))
        tty.wait("[thinking]")
        tty.send(b"/steer " + b"y" * 100 + b"\n")
        tty.wait("correction rejected")
        sock.settimeout(0.2)
        try:
            receive(sock)
            raise AssertionError("oversize steering cancelled the active turn")
        except socket.timeout:
            pass
        sock.settimeout(6)
        assert not Path(str(tty.cp) + ".steer").exists()
        sock.sendall(frame(END, 1, 0))
        tty.wait("completed turn=1")
        tty.send("/quit\n")
        assert tty.finish()[2]["done"] == 1
    passed("combined steering length is checked before cancellation; rejected correction leaves the active turn and checkpoint intact")


def test_durable_steer_recovery():
    with peer() as (home, listener, sock, tty):
        tty.send("Original resumable question\n")
        question(sock, 1)
        sock.sendall(frame(THINK, 1, 0, b"thinking"))
        tty.wait("[thinking]")
        tty.send("/steer Keep it short\n")
        assert receive(sock)[0] == CANCEL
        queued = checkpoint(tty.cp)["queued_prompt"]
        tty.proc.kill(); tty.proc.wait(timeout=3)
        sock.close()
        restarted = subprocess.Popen([
            str(ROOT / "bin/stream_client"), "--port", str(listener.getsockname()[1]),
            "--checkpoint", str(tty.cp), "--cancel-pending",
        ], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            with listener.accept()[0] as restored:
                restored.settimeout(6)
                assert receive(restored) == (HELLO, 0, 0, b"a" * 32)
                restored.sendall(frame(WELCOME, payload=b"a" * 32))
                old, _ = question(restored, 1)
                assert old == b"Original resumable question"
                # Recovery must cancel the old pending turn immediately instead
                # of waiting for it to generate an unwanted full answer.
                assert receive(restored) == (CANCEL, 1, 0, b"")
                restored.sendall(frame(CANCELLED, 1, 0))
                assert question(restored, 2)[0] == queued
                emit(restored, b"R", request=2)
                restored.sendall(frame(END, 2, 1))
            out, errors = restarted.communicate(timeout=6)
            assert restarted.returncode == 0, errors
            saved = checkpoint(tty.cp)
            assert out == b"\nR\n" and saved["done"] == 1 and saved["request"] == 2
            assert saved["prompt"] == queued and not Path(str(tty.cp) + ".steer").exists()
            # A stale sidecar after the new checkpoint was committed is safe to
            # remove on load and must never create an extra turn.
            sidecar = Path(str(tty.cp) + ".steer")
            sidecar.write_bytes(b"T4STEER1\n" + b"a" * 32 + b"\n1 " + str(len(queued)).encode() + b"\n" + queued)
            again = subprocess.run([
                str(ROOT / "bin/stream_client"), "--port", "1", "--checkpoint", str(tty.cp), "--resume-only",
            ], capture_output=True, timeout=4)
            assert again.returncode == 1 and not sidecar.exists()
            assert checkpoint(tty.cp)["request"] == 2
            preserved = tty.cp.read_bytes()
            wrong_session = b"T4STEER1\n" + b"b" * 32 + b"\n2 1\nx"
            sidecar.write_bytes(wrong_session)
            rejected = subprocess.run([
                str(ROOT / "bin/stream_client"), "--port", "1", "--checkpoint", str(tty.cp), "--resume-only",
            ], capture_output=True, timeout=4)
            assert rejected.returncode == 1 and b"checkpoint open/lock" in rejected.stderr
            assert tty.cp.read_bytes() == preserved and sidecar.read_bytes() == wrong_session
            sidecar.unlink()
            victim = home / "sidecar-target"
            victim.write_bytes(b"preserve target")
            sidecar.symlink_to(victim)
            rejected = subprocess.run([
                str(ROOT / "bin/stream_client"), "--port", "1", "--checkpoint", str(tty.cp), "--resume-only",
            ], capture_output=True, timeout=4)
            assert rejected.returncode == 1 and b"checkpoint open/lock" in rejected.stderr
            assert tty.cp.read_bytes() == preserved and victim.read_bytes() == b"preserve target"
            sidecar.unlink()
        finally:
            if restarted.poll() is None:
                restarted.kill(); restarted.wait(timeout=3)
    passed("durable steering survives client death, cancels old turn first, submits once, recognizes stale sidecar and rejects wrong-session/symlink queues without data loss")


def test_cancelled_checkpoint_reconnect():
    with peer() as (home, listener, sock, tty):
        tty.send("cancelled request\n"); question(sock, 1)
        emit(sock, b"A")
        wait_for(lambda: checkpoint(tty.cp)["answer"] == b"A")
        tty.send("/stop\n")
        assert receive(sock)[0] == CANCEL
        sock.sendall(frame(CANCELLED, 1, 1)); tty.wait("cancelled turn=1")
        tty.send("/quit\n"); tty.finish()
        sock.close()
        resumed = subprocess.Popen([
            str(ROOT / "bin/stream_client"), "--port", str(listener.getsockname()[1]),
            "--checkpoint", str(tty.cp), "--resume-only",
        ], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            with listener.accept()[0] as restored:
                restored.settimeout(4)
                assert receive(restored)[0] == HELLO
                restored.sendall(frame(WELCOME, payload=b"a" * 32))
                assert restored.recv(1) == b"", "cancelled request unexpectedly restarted"
            out, errors = resumed.communicate(timeout=4)
            assert resumed.returncode == 0 and out == b"", errors
            assert checkpoint(tty.cp)["done"] == 2
        finally:
            if resumed.poll() is None:
                resumed.kill(); resumed.wait(timeout=3)
    passed("CANCELLED journal survives reconnect and --resume-only does not restart an explicitly cancelled turn")


def test_invalid_control_events():
    events = [
        frame(THINK, 1, 0, b"\xed\xa0\x80"),
        frame(THINK, 1, 0, b"a") + frame(THINK, 1, 9, b"b"),
        frame(THINK, 1, 16777217, b"a"),
        frame(STATUS, 1, 1, b"bad final offset"),
        frame(CANCELLED, 1, 1), frame(CANCELLED, 2, 0),
    ]
    for event in events:
        with peer() as (_, _, sock, tty):
            tty.send("question\n"); question(sock, 1)
            sock.sendall(event)
            out, _, saved = tty.finish(code=1)
            assert out == b"" and saved["offset"] == 0 and saved["done"] == 0
    passed("client rejects invalid thinking UTF-8/offsets, STATUS answer offset and mismatched cancellation terminal without advancing its final checkpoint")


def test_cancel_pending_flag():
    with peer() as (_, listener, sock, tty):
        tty.proc.kill(); tty.proc.wait(timeout=3)
        sock.close()
        prompt = b"failed saved turn"
        tty.cp.write_bytes(b"T4CHECK2\n" + b"a" * 32 + b"\n1 " + str(len(prompt)).encode() + b" 0\n" + prompt)
        for pending in (True, False):
            child = subprocess.Popen([
                str(ROOT / "bin/stream_client"), "--port", str(listener.getsockname()[1]),
                "--checkpoint", str(tty.cp), "--cancel-pending",
            ], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                with listener.accept()[0] as restored:
                    restored.settimeout(4)
                    assert receive(restored)[0] == HELLO
                    restored.sendall(frame(WELCOME, payload=b"a" * 32))
                    if pending:
                        assert question(restored, 1)[0] == prompt
                        assert receive(restored) == (CANCEL, 1, 0, b"")
                        restored.sendall(frame(CANCELLED, 1, 0))
                    else:
                        assert restored.recv(1) == b"", "already cancelled turn was restarted"
                out, errors = child.communicate(timeout=4)
                assert child.returncode == 0 and out == (b"\n" if pending else b""), errors
                assert checkpoint(tty.cp)["done"] == 2
            finally:
                if child.poll() is None:
                    child.kill(); child.wait(timeout=3)
        tty.cp.unlink()
        child = subprocess.Popen([
            str(ROOT / "bin/stream_client"), "--port", str(listener.getsockname()[1]),
            "--checkpoint", str(tty.cp), "--cancel-pending",
        ], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            with listener.accept()[0] as restored:
                restored.settimeout(4)
                assert receive(restored) == (HELLO, 0, 0, b"")
                restored.sendall(frame(WELCOME, payload=b"a" * 32))
                assert restored.recv(1) == b""
            out, errors = child.communicate(timeout=4)
            assert child.returncode == 0 and out == b"", errors
            assert checkpoint(tty.cp)["request"] == 0
        finally:
            if child.poll() is None:
                child.kill(); child.wait(timeout=3)
    passed("batch --cancel-pending immediately cancels a saved failed turn; cancelled and brand-new checkpoints send no spurious turn")


def test_real_server_isolation():
    with tempfile.TemporaryDirectory(prefix="task4-real-controls-") as temp:
        source = b"abcdefghijklmno"
        s = Server(temp, source=source, rate=12)
        tty = Terminal(temp, s.port)
        try:
            tty.wait("question (")
            tty.send("excluded cancelled secret\n")
            other = s.launch_client("survivor", b"other session\n")
            wait_for(lambda: tty.out.stat().st_size and other["stdout"].stat().st_size)
            tty.send("/stop\n"); tty.wait("cancelled turn=1")
            cancelled = checkpoint(tty.cp)
            assert cancelled["done"] == 2 and tty.proc.poll() is None
            survivor, _, other_cp = s.finish_client(other)
            assert survivor == source + b"\n" and other_cp["done"] == 1
            tty.send("completed question\n"); tty.wait("completed turn=2")
            directory = s.state / cancelled["token"]
            assert (directory / "context.txt").read_bytes() == b""
            assert (directory / "turn-00000001/cancelled").exists()
            assert not (directory / "turn-00000001/done").exists()
            tty.send("third question\n"); tty.wait("completed turn=3")
            context = (directory / "context.txt").read_bytes()
            assert b"completed question" in context and b"excluded cancelled secret" not in context
            assert b"other session" not in context
            tty.send("/quit\n"); tty.finish()
        finally:
            tty.close(); s.close()
    passed("real C server: one cancelled client remains connected, concurrent client finishes and cancelled half-turn never enters later context")


def test_generator_eof_cancel():
    # EOF is not child termination. An adapter with no output or one that has
    # already emitted END must remain cancellable until exit.
    for emitted_end in (False, True):
        with tempfile.TemporaryDirectory(prefix="task4-adapter-eof-") as temp:
            home = Path(temp)
            adapter = home / "close-output.py"
            ending = "os.write(1, struct.pack('!IIIII', 0x54345331, 7, 0, 0, 0))\n" if emitted_end else ""
            adapter.write_text("#!/usr/bin/python3\nimport os, struct, sys, time\n" + ending +
                                 "os.close(1)\n"
                                 "sys.stderr.write('stdout closed; child still alive\\n')\n"
                                 "sys.stderr.flush()\ntime.sleep(30)\n")
            adapter.chmod(0o700)
            config = home / "config.json"
            config.write_text('{}')  # Custom framed adapter does not perform model HTTP.
            s = Server(home, adapter=adapter, model_config=config)
            try:
                with s.connection() as (sock, token):
                    send_turn(sock, b"must not complete just because stdout closed")
                    directory = s.state / token.decode()
                    first = directory / "turn-00000001"
                    wait_for(lambda: (first / "generator.log").exists() and
                             b"child still alive" in (first / "generator.log").read_bytes())
                    identities = wait_for(lambda: re.findall(r"generator=(\d+)", s.stdout.read_text()))
                    generator_pid = int(identities[-1])
                    assert child_running(generator_pid) and not (first / "done").exists()
                    sock.settimeout(3)
                    assert receive(sock) == (HEARTBEAT, 1, 0, b"")
                    assert child_running(generator_pid) and not (first / "done").exists()
                    started = time.monotonic()
                    sock.sendall(frame(CANCEL, 1))
                    assert receive(sock) == (CANCELLED, 1, 0, b"")
                    assert time.monotonic() - started < 2
                    assert (first / "cancelled").exists() and not (first / "done").exists()
                    wait_for(lambda: not child_running(generator_pid))
                    # The same worker / TCP session accepts and cancels another turn.
                    send_turn(sock, b"next turn on the same connection", request=2)
                    second = directory / "turn-00000002"
                    wait_for(lambda: (second / "generator.log").exists() and
                             b"child still alive" in (second / "generator.log").read_bytes())
                    sock.sendall(frame(CANCEL, 2))
                    assert receive(sock) == (CANCELLED, 2, 0, b"")
                    assert (second / "cancelled").exists() and not (second / "done").exists()
                    assert (directory / "context.txt").read_bytes() == b""
            finally:
                s.close()
    passed("adapter stdout EOF (with or without END) before child exit stays live and cancellable; no false done, child reaped and same session reusable")


def main():
    log = ROOT / "logs/verification-controls.txt"
    log.parent.mkdir(exist_ok=True)
    started = time.monotonic()
    try:
        for test in (test_thinking_stop, test_status_display_policy,
                     test_steer_terminal_race, test_ctrl_c_and_quit,
                     test_steer_limit, test_durable_steer_recovery,
                     test_cancelled_checkpoint_reconnect, test_invalid_control_events,
                     test_cancel_pending_flag, test_real_server_isolation,
                     test_generator_eof_cancel):
            test()
        summary = f"All {len(CHECKS)} control integration checks passed ({time.monotonic() - started:.1f}s)."
        print(summary, flush=True)
        log.write_text("\n".join(CHECKS + [summary]) + "\n")
    except BaseException:
        log.write_text("\n".join(CHECKS) + "\nFAILED\n" + traceback.format_exc())
        raise


if __name__ == "__main__":
    main()
