#!/usr/bin/env python3
"""Real PTY checks for local conversation navigation and recoverable failures.

All checkpoints, services, terminals and TCP peers belong to this test. The
fixtures exercise the actual C client without starting or modifying a model.
"""

from pathlib import Path
import fcntl
import select
import socket
import struct
import tempfile
import traceback

from verify import (Server, HELLO, WELCOME, DELTA, END, ERROR, frame, receive,
                    checkpoint, lock_released)
from verify_controls import CANCEL, CANCELLED, question, emit
from verify_terminal import TTY
from verify_tui import Screen


CHECKS = []


def passed(message):
    CHECKS.append(message)
    print(f"PASS M{len(CHECKS):02d}: {message}", flush=True)


class ManagedTTY(TTY):
    def __init__(self, port, sessions, *, idle=10000, cp=None):
        self.screen = Screen(24, 96)
        self.fed = 0
        super().__init__(port, cp=cp, session_dir=sessions, extra=("--idle-ms", str(idle)))

    def drain(self, delay=0):
        super().drain(delay)
        data = bytes(self.output[self.fed:])
        self.fed = len(self.output)
        self.screen.feed(data)

    def visible(self):
        self.drain()
        return self.screen.text()


def new_checkpoint(tty, sessions, excluded=()):
    excluded = set(excluded)
    def found():
        files = [path for path in sessions.glob("session-*/checkpoint.ckpt")
                 if path not in excluded and checkpoint(path)]
        return files[0] if len(files) == 1 else None
    path = tty.wait(found, "new managed checkpoint")
    tty.wait(lambda: "question" in tty.visible(), "managed editor ready")
    return path


def completed(tty, path, number):
    return tty.wait(lambda: (saved := checkpoint(path)) and
                    saved["request"] == number and saved["done"] == 1,
                    f"completed managed turn {number}")


def managed_roundtrip():
    with tempfile.TemporaryDirectory(prefix="task4-session-navigation-") as temporary:
        home = Path(temporary)
        sessions = home / "clients"
        service = Server(home, source=b"fixture answer\n")
        tty = ManagedTTY(service.port, sessions)
        other = None
        try:
            first = new_checkpoint(tty, sessions)
            first_id = first.parent.name
            tty.send("first conversation opening\r")
            completed(tty, first, 1)
            tty.send("first conversation followup\r")
            completed(tty, first, 2)
            first_token = checkpoint(first)["token"]
            tty.send("/new\r")
            second = new_checkpoint(tty, sessions, [first])
            second_id = second.parent.name
            tty.send("second conversation question\r")
            completed(tty, second, 1)
            second_token = checkpoint(second)["token"]
            assert first_token != second_token
            tty.send("/sessions\r")
            tty.wait(lambda: first_id in tty.visible() and second_id in tty.visible(),
                     "local session list")
            assert checkpoint(second)["request"] == 1
            tty.send("/switch " + first_id + "\r")
            tty.wait(lambda: not lock_released(str(first) + ".lock") and
                     lock_released(str(second) + ".lock"), "switch checkpoint ownership")
            tty.wait(lambda: "first conversation opening" in tty.visible() and
                     "first conversation followup" in tty.visible(),
                     "restored earlier conversation turns")
            assert "second conversation question" not in tty.visible()
            tty.send("/switch " + str(second) + "\r")
            tty.wait(lambda: not lock_released(str(second) + ".lock") and
                     lock_released(str(first) + ".lock"), "switch explicit checkpoint path")
            tty.send("/switch " + first_id + "\r")
            tty.wait(lambda: not lock_released(str(first) + ".lock") and
                     lock_released(str(second) + ".lock"), "return to first conversation")
            passed("/new and /switch by ID or checkpoint path keep independent tokens and restore the selected conversation's earlier turns")

            tty.send("/help\r")
            tty.wait(lambda: "/switch" in tty.visible() and "/delete" in tty.visible(),
                     "help lists conversation commands")
            tty.send("/not-a-command\r")
            tty.pause()
            assert checkpoint(first)["request"] == 2, "unknown command reached the model"
            tty.send("/switch session-NOFILE\r")
            tty.pause()
            assert not lock_released(str(first) + ".lock"), "failed navigation lost original checkpoint lock"
            assert tty.proc.poll() is None
            passed("/sessions, /help, unknown slash commands and a missing switch target leave the current model turn unchanged")

            other = ManagedTTY(service.port, sessions)
            third = new_checkpoint(other, sessions, [first, second])
            third_id = third.parent.name
            tty.send("/switch " + third_id + "\r")
            tty.pause()
            assert not lock_released(str(first) + ".lock") and third.exists()
            tty.send("/delete " + third_id + "\r")
            tty.pause()
            assert third.exists() and not lock_released(str(third) + ".lock")
            assert other.proc.poll() is None and tty.proc.poll() is None
            tty.send("/delete ../outside\r")
            tty.pause()
            assert first.exists() and second.exists() and third.exists()
            tty.send("still in first conversation\r")
            completed(tty, first, 3)
            assert checkpoint(third)["request"] == 0
            passed("another terminal's active conversation cannot be switched into or deleted; rejected paths preserve the usable current conversation")

            tty.send("/delete " + second_id + "\r")
            tty.wait(lambda: not second.parent.exists(), "inactive conversation soft deletion")
            trashed = list((sessions / ".trash").rglob("checkpoint.ckpt"))
            assert any(checkpoint(path)["token"] == second_token for path in trashed)
            assert (service.state / second_token).is_dir(), "local delete erased server recovery data"
            tty.send("/delete\r")
            fourth = new_checkpoint(tty, sessions, [first, second, third])
            assert not first.parent.exists() and checkpoint(fourth)["token"] not in (first_token, second_token)
            assert (service.state / first_token).is_dir()
            assert any(checkpoint(path)["token"] == first_token
                       for path in (sessions / ".trash").rglob("checkpoint.ckpt"))
            passed("/delete moves local conversations to .trash, retains server recovery data and opens a fresh conversation after deleting the current one")
            tty.send("/quit\r"); tty.finish()
            other.send("/quit\r"); other.finish()
        finally:
            if other is not None:
                other.close()
            tty.close()
            service.close()


def accept_session(listener, tty, expected, token):
    sock, _ = listener.accept()
    sock.settimeout(6)
    assert receive(sock) == (HELLO, 0, 0, expected)
    sock.sendall(frame(WELCOME, payload=token))
    tty.pause(0.08)
    return sock


def recoverable_errors():
    with tempfile.TemporaryDirectory(prefix="task4-session-errors-") as temporary:
        sessions = Path(temporary) / "clients"
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); listener.listen(5); listener.settimeout(6)
            tty = ManagedTTY(listener.getsockname()[1], sessions)
            sockets = []
            try:
                token = b"a" * 32
                sock = accept_session(listener, tty, b"", token); sockets.append(sock)
                first = new_checkpoint(tty, sessions)
                tty.send("retain this exact pending question\r")
                prompt, offset = question(sock, 1)
                assert offset == 0
                prefix = "已经收到的正文".encode()
                total = emit(sock, prefix)
                sock.sendall(frame(ERROR, 1, total, b"fixture inference failed"))
                tty.wait(lambda: "fixture inference failed" in tty.visible(), "inline model error")
                saved = checkpoint(first)
                assert saved["done"] == 0 and saved["answer"] == prefix
                assert tty.proc.poll() is None
                tty.send("/retry\r")
                resumed = accept_session(listener, tty, token, token); sockets.append(resumed)
                assert question(resumed, 1) == (prompt, len(prefix))
                suffix = "，恢复后的正文。".encode()
                total = emit(resumed, suffix, offset=len(prefix))
                resumed.sendall(frame(END, 1, total))
                completed(tty, first, 1)
                assert checkpoint(first)["answer"] == prefix + suffix
                passed("a server ERROR stays inline; /retry reconnects with the same token, prompt, request and durable byte offset without duplicating the answer")

                tty.send("revise after an inference error\r")
                old_prompt, old_offset = question(resumed, 2)
                resumed.sendall(frame(ERROR, 2, 0, b"fixture correction failure"))
                tty.wait(lambda: "fixture correction failure" in tty.visible(), "error before steering")
                correction = "请改用两句话回答。".encode()
                tty.send(correction + b"\r")
                corrected = accept_session(listener, tty, token, token); sockets.append(corrected)
                assert question(corrected, 2) == (old_prompt, old_offset)
                assert receive(corrected) == (CANCEL, 2, 0, b"")
                corrected.sendall(frame(CANCELLED, 2, 0))
                new_prompt, new_offset = question(corrected, 3)
                assert new_offset == 0 and new_prompt == old_prompt + b"\n\nUser correction: " + correction
                corrected.sendall(frame(END, 3, 0))
                completed(tty, first, 3)
                assert checkpoint(first)["queued_prompt"] == b""
                passed("plain text after an error durably queues correction, reconnects and cancels the old request before submitting the corrected next turn")

                tty.send("disconnect this second turn\r")
                assert question(corrected, 4)[0] == b"disconnect this second turn"
                corrected.close()
                tty.wait(lambda: "error" in tty.visible() and "/retry" in tty.visible(),
                         "inline network disconnect")
                assert tty.proc.poll() is None and checkpoint(first)["done"] == 0
                tty.send("/new\r")
                fresh = accept_session(listener, tty, b"", b"b" * 32); sockets.append(fresh)
                second = new_checkpoint(tty, sessions, [first])
                assert checkpoint(first)["request"] == 4 and checkpoint(first)["done"] == 0
                tty.send("fresh after failed connection\r")
                assert question(fresh, 1)[0] == b"fresh after failed connection"
                fresh.sendall(frame(END, 1, 0))
                completed(tty, second, 1)
                passed("EOF preserves the failed pending conversation; /new stays in the same TUI process and starts an independent usable connection")

                tty.send("stop after failure\r")
                stop_prompt, _ = question(fresh, 2)
                fresh.sendall(frame(ERROR, 2, 0, b"fixture stop failure"))
                tty.wait(lambda: "fixture stop failure" in tty.visible(), "error before stop")
                tty.send("/stop\r")
                stopped = accept_session(listener, tty, b"b" * 32, b"b" * 32); sockets.append(stopped)
                assert question(stopped, 2) == (stop_prompt, 0)
                assert receive(stopped) == (CANCEL, 2, 0, b"")
                stopped.sendall(frame(CANCELLED, 2, 0))
                tty.wait(lambda: checkpoint(second)["done"] == 2, "offline pending turn cancelled")
                tty.send("normal question after stopped error\r")
                assert question(stopped, 3)[0] == b"normal question after stopped error"
                stopped.sendall(frame(END, 3, 0))
                completed(tty, second, 3)
                passed("/stop after an error reconnects and persists cancellation, leaving the same conversation ready for an ordinary next question")
                tty.send("/quit\r"); tty.finish()
            finally:
                tty.close()
                for sock in sockets:
                    sock.close()


def active_navigation():
    with tempfile.TemporaryDirectory(prefix="task4-session-stream-") as temporary:
        home = Path(temporary)
        sessions = home / "clients"
        source = ("长回答用于检查切换时保存断点。" * 10).encode()
        service = Server(home, source=source, rate=80)
        tty = ManagedTTY(service.port, sessions)
        try:
            first = new_checkpoint(tty, sessions)
            tty.send("switch during a streamed answer\r")
            tty.wait(lambda: checkpoint(first)["offset"] > 0 and checkpoint(first)["done"] == 0,
                     "partial durable streamed answer")
            tty.send("/new\r")
            second = new_checkpoint(tty, sessions, [first])
            saved = checkpoint(first)
            assert saved["done"] == 0 and 0 < saved["offset"] < len(source)
            tty.send("/switch " + first.parent.name + "\r")
            tty.wait(lambda: not lock_released(str(first) + ".lock") and
                     lock_released(str(second) + ".lock"), "pending conversation switched back")
            tty.pause(0.25)
            assert checkpoint(first) == saved, "switch silently resumed or discarded the pending answer"
            tty.send("/retry\r")
            completed(tty, first, 1)
            assert checkpoint(first)["answer"] == source
            tty.send("/quit\r"); tty.finish()
            passed("/new during streaming preserves the partial checkpoint; switching back waits for /retry and restores the exact final answer")
        finally:
            tty.close(); service.close()


def in_place_navigation_keeps_stream():
    with tempfile.TemporaryDirectory(prefix="task4-navigation-connection-") as temporary:
        sessions = Path(temporary) / "clients"
        busy = sessions / "session-BUSY01" / "checkpoint.ckpt"
        inactive = sessions / "session-DELETE" / "checkpoint.ckpt"
        damaged = sessions / "session-BAD001" / "checkpoint.ckpt"
        for path in (busy, inactive, damaged):
            checkpoint_fixture(path, b"b" * 32, b"independent fixture")
        with Path(str(busy) + ".lock").open("w") as owner, socket.socket() as listener:
            fcntl.flock(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
            listener.bind(("127.0.0.1", 0)); listener.listen(3); listener.settimeout(6)
            tty = ManagedTTY(listener.getsockname()[1], sessions)
            sockets = []
            try:
                token = b"a" * 32
                sock = accept_session(listener, tty, b"", token); sockets.append(sock)
                first = new_checkpoint(tty, sessions, [busy, inactive, damaged])
                damaged.write_bytes(b"invalid checkpoint\n")
                tty.send("keep streaming through local commands\r")
                _, offset = question(sock, 1)
                assert offset == 0
                commands = ["/switch", "/switch ", "/switch session-NOFILE",
                            "/switch " + busy.parent.name, "/switch " + damaged.parent.name,
                            "/switch " + first.parent.name, "/delete " + busy.parent.name,
                            "/delete " + str(first), "/delete " + inactive.parent.name]
                for index, command in enumerate(commands):
                    # A visible draft after the command proves its handler has
                    # finished, without guessing how long PTY scheduling takes.
                    marker = f"navigation-check-{index}"
                    tty.send(command + "\r" + marker)
                    tty.wait(lambda: marker in tty.visible(), "navigation command handled: " + command)
                    assert not select.select([sock], [], [], 0)[0], (command, "socket closed or request changed")
                    assert not lock_released(str(first) + ".lock")
                    tty.send("\x15")
                    offset = emit(sock, b"x", offset=offset)
                    tty.wait(lambda: checkpoint(first)["offset"] == offset,
                             "same stream continues after " + command)
                    assert checkpoint(first)["done"] == 0
                assert busy.exists() and not inactive.parent.exists()
                sock.sendall(frame(END, 1, offset))
                completed(tty, first, 1)
                assert checkpoint(first)["answer"] == b"x" * len(commands)
                tty.send("/quit\r"); tty.finish()
                passed("invalid, busy, damaged and current switch targets plus deletion of another session preserve the live socket and streamed answer")
            finally:
                tty.close()
                for sock in sockets:
                    sock.close()


def checkpoint_fixture(path, token, prompt, *, cancelled=False, queued=None):
    """Write the exact durable state at a crash boundary, using the public format."""
    path.parent.mkdir(parents=True, mode=0o700, exist_ok=True)
    kind = CANCELLED if cancelled else END
    record = struct.pack("!III", kind, 0, 0)
    digest = 2166136261
    for value in record:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    path.write_bytes(b"T4CHECK2\n" + token + b"\n1 " + str(len(prompt)).encode()
                     + b" 0\n" + prompt + record + struct.pack("!I", digest))
    path.chmod(0o600)
    if queued is not None:
        sidecar = Path(str(path) + ".steer")
        sidecar.write_bytes(b"T4STEER1\n" + token + b"\n1 "
                           + str(len(queued)).encode() + b"\n" + queued)
        sidecar.chmod(0o600)


def queued_after_cancelled_restart():
    with tempfile.TemporaryDirectory(prefix="task4-session-queued-crash-") as temporary:
        sessions = Path(temporary) / "clients"
        cp = sessions / "session-QUEUED" / "checkpoint.ckpt"
        token, prompt = b"c" * 32, b"original interrupted question"
        correction = prompt + b"\n\nUser correction: " + "保留这条已经确认的纠偏。".encode()
        checkpoint_fixture(cp, token, prompt, cancelled=True, queued=correction)
        initial = checkpoint(cp)
        assert initial["done"] == 2 and initial["queued_prompt"] == correction
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); listener.listen(2); listener.settimeout(6)
            tty = ManagedTTY(listener.getsockname()[1], sessions, cp=cp)
            try:
                with accept_session(listener, tty, token, token) as sock:
                    assert question(sock, 2) == (correction, 0)
                    sock.sendall(frame(END, 2, 0))
                    completed(tty, cp, 2)
                    assert checkpoint(cp)["queued_prompt"] == b""
                    assert not Path(str(cp) + ".steer").exists()
                    sock.settimeout(0.2)
                    try:
                        data = sock.recv(1)
                    except socket.timeout:
                        data = None
                    assert data is None, "restored correction was submitted more than once"
                    tty.send("/quit\r"); tty.finish()
            finally:
                tty.close()
        passed("restart after durable CANCELLED but before committing its queued correction submits the saved next turn exactly once")


def archive_failure_preserves_latest_turn():
    with tempfile.TemporaryDirectory(prefix="task4-session-archive-failure-") as temporary:
        home = Path(temporary)
        sessions = home / "clients"
        service = Server(home, source=b"archive failure fixture\n")
        tty = ManagedTTY(service.port, sessions)
        try:
            cp = new_checkpoint(tty, sessions)
            history = Path(str(cp) + ".history")
            history.write_text("test-owned obstruction: this must be a directory\n")
            tty.send("keep this first completed question\r")
            completed(tty, cp, 1)
            saved = cp.read_bytes()
            tty.send("do not overwrite the only saved copy\r")
            tty.wait(lambda: "could not archive" in tty.visible(), "archive failure stays inline")
            tty.pause()
            assert cp.read_bytes() == saved and tty.proc.poll() is None
            assert not (service.state / checkpoint(cp)["token"] / "turn-00000002").exists()
            history.unlink()
            tty.send("now the history directory can be saved\r")
            completed(tty, cp, 2)
            assert (history / "turn-00000001.ckpt").is_file()
            tty.send("/quit\r"); tty.finish()
        finally:
            tty.close(); service.close()
        passed("an archive failure keeps the latest checkpoint unchanged and blocks overwriting it until its previous turn can be safely archived")


def delete_current_checks_exact_checkpoint():
    with tempfile.TemporaryDirectory(prefix="task4-session-delete-selection-") as temporary:
        home = Path(temporary)
        sessions = home / "clients"
        managed = sessions / "session-SAMEID" / "checkpoint.ckpt"
        outside = home / "elsewhere" / "session-SAMEID" / "checkpoint.ckpt"
        checkpoint_fixture(managed, b"d" * 32, b"managed conversation must survive")
        checkpoint_fixture(outside, b"e" * 32, b"external conversation with same folder name")
        before = managed.read_bytes()
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0)); listener.listen(2); listener.settimeout(6)
            tty = ManagedTTY(listener.getsockname()[1], sessions, cp=outside)
            try:
                with accept_session(listener, tty, b"e" * 32, b"e" * 32):
                    tty.send("/delete\r")
                    tty.wait(lambda: "outside this managed list" in tty.visible(),
                             "implicit deletion rejects a different checkpoint with the same ID")
                    assert managed.read_bytes() == before and outside.exists()
                    assert not (sessions / ".trash").exists()
                    assert not lock_released(str(outside) + ".lock")
                    tty.send("/quit\r"); tty.finish()
            finally:
                tty.close()
        passed("/delete without an ID checks the exact current checkpoint and never deletes a same-named conversation in another directory")


def run():
    managed_roundtrip()
    recoverable_errors()
    active_navigation()
    in_place_navigation_keeps_stream()
    queued_after_cancelled_restart()
    archive_failure_preserves_latest_turn()
    delete_current_checks_exact_checkpoint()
    print(f"All {len(CHECKS)} client conversation checks passed.", flush=True)


if __name__ == "__main__":
    try:
        run()
    except Exception:
        traceback.print_exc()
        raise SystemExit(1)
