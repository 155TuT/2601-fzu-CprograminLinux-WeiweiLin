#!/usr/bin/env python3
"""Real C automatic-port/service-discovery tests, isolated from user services.

Every server uses an owned temporary state and registry; every listener uses a
kernel-selected loopback port. Tests never reserve 3340/8081 or touch real state.
"""

from pathlib import Path
import ctypes
import fcntl
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import traceback

sys.dont_write_bytecode = True
from verify import ROOT, checkpoint, child_running, fixture_config, frame, receive, wait_for

PROBE, SERVICE = 14, 15
CHECKS = []


def passed(description):
    line = f"PASS P{len(CHECKS) + 1:02d}: {description}"
    CHECKS.append(line)
    print(line, flush=True)


def endpoint(path):
    fields = Path(path).read_text().splitlines()
    assert len(fields) == 6 and fields[0] == "T4ENDPOINT1", fields
    assert re.fullmatch(r"[0-9a-f]{32}", fields[5]), fields
    return {"host": fields[1], "port": int(fields[2]), "state": fields[3],
            "identity": fields[4], "instance": fields[5], "fields": fields}


class AutoServer:
    def __init__(self, home, registry, *, state=None, source=b"discovered answer\n", rate=0):
        self.home, self.registry = Path(home), Path(registry)
        self.home.mkdir(parents=True, exist_ok=True)
        self.state = Path(state) if state else self.home / "server"
        self.model_config = fixture_config(self.home, source, rate)
        self.output_path = self.home / f"server-{time.monotonic_ns()}.log"
        self.handle = self.output_path.open("wb")
        self.command = [str(ROOT / "bin/stream_server"), "--state", str(self.state),
                        "--discovery-dir", str(self.registry),
                        "--adapter", str(ROOT / "tests/mock_adapter.py"),
                        "--model-config", str(self.model_config)]  # Deliberately omit --port.
        self.proc = subprocess.Popen(self.command, cwd=ROOT, stdin=subprocess.DEVNULL,
            stdout=self.handle, stderr=subprocess.STDOUT, start_new_session=True)

        def ready():
            text = self.output_path.read_text()
            assert self.proc.poll() is None, text
            match = re.search(r"listening on 127\.0\.0\.1:(\d+)", text)
            return int(match[1]) if match and (self.state / "server.endpoint").exists() else None
        self.port = wait_for(ready, description="automatic C server publishes endpoint")
        self.record = endpoint(self.state / "server.endpoint")
        assert self.record["port"] == self.port and self.record["state"] == str(self.state.resolve())

    def pids(self):
        text = self.output_path.read_text()
        return ({self.proc.pid} | set(map(int, re.findall(r"\[worker (\d+)\]", text))) |
                set(map(int, re.findall(r"generator=(\d+)", text))))

    def stop(self, crash=False):
        tracked = self.pids()
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            assert self.proc.wait(timeout=6) == (-signal.SIGKILL if crash else 0), self.output_path.read_text()
        wait_for(lambda: all(not child_running(pid) for pid in tracked),
                 description="test-owned automatic server descendants exit")
        for pid in tracked - {self.proc.pid}:
            try: os.waitpid(pid, os.WNOHANG)
            except ChildProcessError: pass

    def close(self):
        if self.proc.poll() is None:
            self.stop()
        self.handle.close()


def client(root, registry, name, content=b"question\n", *, cp=None, resume=False, extra=(), launch=False):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    cp = Path(cp) if cp else root / f"{name}.ckpt"
    command = [str(ROOT / "bin/stream_client"), "--discovery-dir", str(registry),
               "--resume" if resume else "--checkpoint", str(cp)]
    if content is not None:
        input_path = root / f"{name}.input"
        input_path.write_bytes(content)
        command.extend(["--input", str(input_path)])
    command.extend(map(str, extra))
    process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if launch:
        return process, cp
    stdout, stderr = process.communicate(timeout=8)
    return process.returncode, stdout, stderr.decode("utf-8", "replace"), cp


def succeed(result):
    code, output, errors, cp = result
    assert code == 0, (code, errors)
    saved = checkpoint(cp)
    assert saved and saved["done"] == 1, saved
    return output, saved


def test_default_ports_and_fresh_connection():
    with tempfile.TemporaryDirectory(prefix="t4-discovery-default-") as temporary:
        root = Path(temporary); registry = root / "services"
        # An unrelated occupied port remains untouched while default bind(0)
        # selects another address. No hardcoded/user service port is reserved.
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0)); occupied.listen(1)
            server = AutoServer(root / "a", registry, source="自动发现🙂\n".encode())
            try:
                assert 0 < server.port <= 65535 and server.port != occupied.getsockname()[1]
                # A real discovery-only connection has no conversation and its
                # waiting worker must not retain the master's state lease FD.
                with socket.create_connection(("127.0.0.1", server.port), timeout=3) as probe:
                    probe.sendall(frame(PROBE))
                    assert receive(probe) == (SERVICE, 0, 0, server.record["instance"].encode())
                    assert not any(re.fullmatch(r"[0-9a-f]{32}", p.name) for p in server.state.iterdir())
                    children = Path(f"/proc/{server.proc.pid}/task/{server.proc.pid}/children")
                    worker = wait_for(lambda: next((int(p) for p in children.read_text().split()
                                                    if child_running(int(p))), None))
                    lease = (server.state / "server.lock").stat()
                    for descriptor in Path(f"/proc/{worker}/fd").iterdir():
                        try: info = descriptor.stat()
                        except FileNotFoundError: continue
                        assert (info.st_dev, info.st_ino) != (lease.st_dev, lease.st_ino), "worker inherited master lease"
                output, saved = succeed(client(root, registry, "fresh"))
                assert output == "自动发现🙂\n\n".encode(), output
                turn = server.state / saved["token"] / "turn-00000001"
                assert (turn / "prompt.txt").read_bytes() == b"question"
                assert (turn / "done").is_file()
                assert list(registry.glob("*.endpoint")) and (server.state / "server.lock").is_file()
                assert occupied.getsockname()[1] != server.port
            finally: server.close()
    passed("default C bind(0) avoids occupied ports; read-only PROBE creates no session and worker drops master lease; no-port client discovers/streams a real UTF-8 turn")


def test_duplicate_state_and_explicit_port():
    with tempfile.TemporaryDirectory(prefix="t4-discovery-duplicate-") as temporary:
        root = Path(temporary); registry = root / "services"
        server = AutoServer(root / "a", registry)
        try:
            record = (server.state / "server.endpoint").read_bytes()
            duplicate = subprocess.run(server.command, cwd=ROOT, capture_output=True, text=True, timeout=5)
            assert duplicate.returncode == 0 and "already" in duplicate.stdout.lower(), duplicate
            assert (server.state / "server.endpoint").read_bytes() == record and server.proc.poll() is None
            output, _ = succeed(client(root, root / "nonexistent-registry", "explicit",
                                       extra=("--port", server.port, "--host", "127.0.0.1")))
            assert output == b"discovered answer\n\n"
            failure = client(root, registry, "host-only", extra=("--host", "127.0.0.1"))
            assert failure[0] != 0 and "port" in failure[2].lower()
            with socket.socket() as occupied:
                occupied.bind(("127.0.0.1", 0)); occupied.listen(1)
                command = [str(ROOT / "bin/stream_server"), "--port", str(occupied.getsockname()[1]),
                           "--state", str(root / "occupied-state"), "--discovery-dir", str(registry)]
                failed = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=5)
                assert failed.returncode != 0 and "listen" in failed.stderr.lower(), failed
                assert not (root / "occupied-state/server.endpoint").exists()
        finally: server.close()
    passed("same-state duplicate startup reports existing service without replacing its endpoint; explicit --port bypasses discovery, occupied port and --host-only fail clearly")


def test_multiple_states_and_token_routing():
    with tempfile.TemporaryDirectory(prefix="t4-discovery-routing-") as temporary:
        root = Path(temporary); registry = root / "services"
        a = AutoServer(root / "a", registry, source=b"A\n")
        b = None
        try:
            _, old = succeed(client(root, registry, "first-a"))
            cp = root / "first-a.ckpt"
            b = AutoServer(root / "b", registry, source=b"B\n")
            ambiguous = client(root, registry, "ambiguous")
            assert ambiguous[0] != 0 and not ambiguous[3].exists(), ambiguous
            assert "server-state" in ambiguous[2], ambiguous
            output, selected = succeed(client(root, registry, "direct-b", extra=("--server-state", b.state)))
            assert output == b"B\n\n" and (b.state / selected["token"]).is_dir()
            continued = client(root, registry, "resume-a", content=b"question\nsecond question\n", cp=cp, resume=True)
            output, saved = succeed(continued)
            assert output == b"A\n\n" and saved["token"] == old["token"] and saved["request"] == 2
            assert (a.state / old["token"] / "turn-00000002/prompt.txt").read_bytes() == b"second question"
            assert not (b.state / old["token"]).exists()
            # Copied state is intentionally ambiguous: the token is not enough
            # to choose one of two live roots that both possess this session.
            shutil.copytree(a.state / old["token"], b.state / old["token"])
            duplicate_token = client(root, registry, "copied-token", content=None, cp=cp, resume=True,
                                     extra=("--resume-only",))
            assert duplicate_token[0] != 0 and "server-state" in duplicate_token[2]
            selected_resume = client(root, registry, "copied-direct", content=None, cp=cp, resume=True,
                                     extra=("--server-state", a.state, "--resume-only"))
            assert selected_resume[0] == 0, selected_resume
        finally:
            if b: b.close()
            a.close()
    passed("multiple services require explicit state for new sessions; saved token selects its own root, while copied-token ambiguity is rejected instead of arbitrary routing")


def test_crash_restart_port_change_resume():
    with tempfile.TemporaryDirectory(prefix="t4-discovery-restart-") as temporary:
        root = Path(temporary); registry = root / "services"
        source = ("持久前缀🙂\n" * 40).encode()
        server = AutoServer(root / "a", registry, source=source, rate=80)
        restarted, process = None, None
        cp = root / "pending.ckpt"
        try:
            process, _ = client(root, registry, "pending", cp=cp, launch=True)
            wait_for(lambda: checkpoint(cp) and checkpoint(cp)["request"] == 1 and
                     0 < checkpoint(cp)["offset"] < len(source), description="pending streamed checkpoint")
            process.terminate(); _, errors = process.communicate(timeout=4)
            assert process.returncode == 143, errors
            saved = checkpoint(cp); prefix = saved["offset"]
            assert saved["done"] == 0 and source.startswith(saved["answer"])
            old_port, old_instance = server.port, server.record["instance"]
            server.stop(crash=True)
            assert (server.state / "server.endpoint").exists(), "SIGKILL should leave observable stale state"
            before = cp.read_bytes()
            failure = client(root, registry, "stale-only", content=None, cp=cp, resume=True,
                             extra=("--resume-only",))
            assert failure[0] != 0 and cp.read_bytes() == before, failure
            with socket.socket() as occupied_old:
                occupied_old.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                occupied_old.bind(("127.0.0.1", old_port)); occupied_old.listen(1)
                restarted = AutoServer(root / "a", registry, state=server.state,
                                       source=b"changed fixture must not replace frozen turn", rate=0)
                assert restarted.port != old_port and restarted.record["instance"] != old_instance
                output, completed = succeed(client(root, registry, "resume-new-port", content=None,
                                                   cp=cp, resume=True, extra=("--resume-only",)))
                assert output == source[prefix:] + b"\n", (len(output), prefix)
                assert completed["token"] == saved["token"] and completed["answer"] == source
                turn = server.state / saved["token"] / "turn-00000001"
                assert (turn / "response.txt").read_bytes() == source and (turn / "done").is_file()
        finally:
            if process and process.poll() is None:
                process.terminate(); process.communicate(timeout=3)
            if restarted: restarted.close()
            server.close()
    passed("SIGKILL stale registry is ignored; parent/worker leases release, restart changes port/nonce, and token-only checkpoint resumes its exact pinned UTF-8 answer")


def test_unsafe_registry_and_state_records():
    with tempfile.TemporaryDirectory(prefix="t4-discovery-unsafe-") as temporary:
        root = Path(temporary); registry = root / "services"
        server = AutoServer(root / "a", registry)
        try:
            (registry / "1-1.endpoint").write_text("T4ENDPOINT1\nbroken\n")
            os.symlink(server.state / "server.endpoint", registry / "2-2.endpoint")
            os.mkfifo(registry / "3-3.endpoint")
            (registry / "4-4.endpoint").write_bytes(b"x" * 20000)
            outside = root / "outside"; outside.mkdir()
            (outside / "server.endpoint").write_text("do not interpret this")
            started = time.monotonic()
            succeed(client(root, registry, "safe-scan"))
            assert time.monotonic() - started < 3, "unsafe registry file blocked discovery"
            record = (server.state / "server.endpoint").read_bytes()
            (server.state / "server.endpoint").unlink()
            os.mkfifo(server.state / "server.endpoint")
            try:
                started = time.monotonic()
                failed = client(root, registry, "state-fifo", extra=("--server-state", server.state))
                assert failed[0] != 0 and time.monotonic() - started < 3
            finally:
                (server.state / "server.endpoint").unlink()
                (server.state / "server.endpoint").write_bytes(record)
            os.symlink(server.state, root / "linked-state")
            failed = client(root, registry, "state-symlink", extra=("--server-state", root / "linked-state"))
            assert failed[0] != 0, failed
            os.symlink(registry, root / "linked-registry")
            failed = client(root, root / "linked-registry", "registry-symlink")
            assert failed[0] != 0, failed
        finally: server.close()
    passed("malformed, symlink, FIFO and oversized registry entries are skipped nonblocking; unsafe explicit-state/registry paths are rejected")


def test_wrong_service_probe_before_hello():
    with tempfile.TemporaryDirectory(prefix="t4-discovery-probe-") as temporary:
        root = Path(temporary); registry = root / "services"
        server = AutoServer(root / "a", registry)
        fields = server.record["fields"].copy()
        registry_entry = next(registry.glob("*.endpoint"))
        server.stop()
        process = None
        try:
            with socket.socket() as fake:
                fake.bind(("127.0.0.1", 0)); fake.listen(1); fake.settimeout(5)
                fields[2] = str(fake.getsockname()[1])
                encoded = ("\n".join(fields) + "\n").encode()
                # This deliberately forged fixture holds a genuine flock and
                # consistent inode/lease/record. Only the TCP peer is different.
                with (server.state / "server.lock").open("rb") as lease:
                    fcntl.flock(lease, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    (server.state / "server.endpoint").write_bytes(encoded)
                    registry_entry.write_bytes(encoded)
                    process, cp = client(root, registry, "wrong-peer", launch=True)
                    with fake.accept()[0] as sock:
                        sock.settimeout(5)
                        assert receive(sock) == (PROBE, 0, 0, b""), "auto-client skipped the identity probe"
                        wrong = ("0" if fields[5] != "0" * 32 else "1") * 32
                        sock.sendall(frame(SERVICE, payload=wrong.encode()))
                        assert sock.recv(1) == b"", "client sent HELLO to a service with a wrong instance"
                    _, errors = process.communicate(timeout=3)
                    assert process.returncode != 0 and not cp.exists(), errors
        finally:
            if process and process.poll() is None:
                process.terminate(); process.communicate(timeout=3)
            server.close()
    passed("a forged live lease/endpoint leading to the wrong TCP service receives only PROBE; mismatched nonce prevents HELLO and checkpoint creation")


def main():
    libc = ctypes.CDLL(None, use_errno=True)
    assert libc.prctl(36, 1, 0, 0, 0) == 0, "cannot enable test-owned child subreaper"
    started = time.monotonic()
    log = ROOT / "logs/verification-discovery.txt"
    try:
        for test in (test_default_ports_and_fresh_connection, test_duplicate_state_and_explicit_port,
                     test_multiple_states_and_token_routing, test_crash_restart_port_change_resume,
                     test_unsafe_registry_and_state_records, test_wrong_service_probe_before_hello):
            test()
        summary = f"All {len(CHECKS)} discovery integration checks passed ({time.monotonic() - started:.1f}s)."
        print(summary, flush=True)
        log.write_text("\n".join(CHECKS + [summary]) + "\n")
    except BaseException:
        log.write_text("\n".join(CHECKS) + "\nFAILED\n" + traceback.format_exc())
        raise


if __name__ == "__main__":
    main()
