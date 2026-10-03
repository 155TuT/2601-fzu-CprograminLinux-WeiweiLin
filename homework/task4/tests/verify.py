#!/usr/bin/env python3
"""用真实 C 程序与独立 TCP 对端验证流式输出、会话隔离和持久恢复。

只使用 Python 标准库；语料、输入、状态和自定义故障生成器均位于临时目录。
"""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
import ctypes
import fcntl
import os
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import traceback


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from checkpoint import load as load_checkpoint

MAGIC = 0x54345331
HELLO, WELCOME, BEGIN, INPUT, COMMIT, DELTA, END, ERROR, HEARTBEAT = range(1, 10)
CHECKS = []


def passed(message):
    line = f"PASS {len(CHECKS) + 1:02d}: {message}"
    CHECKS.append(line)
    print(line, flush=True)


def wait_for(predicate, timeout=6, description="condition"):
    deadline = time.monotonic() + timeout
    while True:
        result = predicate()
        if result:
            return result
        assert time.monotonic() < deadline, f"timeout waiting for {description}"
        time.sleep(0.02)


def frame(kind, request=0, offset=0, payload=b"", *, magic=MAGIC, length=None):
    size = len(payload) if length is None else length
    return struct.pack("!IIIII", magic, kind, request, offset, size) + payload


def exact(sock, count):
    result = bytearray()
    while len(result) < count:
        data = sock.recv(count - len(result))
        if not data:
            raise EOFError(f"EOF after {len(result)} of {count} bytes")
        result.extend(data)
    return bytes(result)


def receive(sock):
    magic, kind, request, offset, count = struct.unpack("!IIIII", exact(sock, 20))
    assert magic == MAGIC and count <= 4096, (magic, count)
    return kind, request, offset, exact(sock, count)


def send_turn(sock, prompt, request=1, resume=0, chunks=128):
    messages = [frame(BEGIN, request, resume)]
    for start in range(0, len(prompt), chunks):
        messages.append(frame(INPUT, request, start, prompt[start:start + chunks]))
    messages.append(frame(COMMIT, request, len(prompt)))
    sock.sendall(b"".join(messages))


def answer(sock, request=1, offset=0):
    result, heartbeats = bytearray(), 0
    while True:
        kind, received_request, at, payload = receive(sock)
        assert received_request == request and at == offset, (kind, received_request, at, offset, payload)
        if kind == HEARTBEAT:
            assert not payload
            heartbeats += 1
        elif kind == DELTA:
            assert len(payload.decode("utf-8")) == 1, payload
            result.extend(payload)
            offset += len(payload)
        else:
            assert kind == END and not payload, (kind, payload)
            return bytes(result), heartbeats


def checkpoint(path):
    try:
        return load_checkpoint(path)
    except FileNotFoundError:
        return None


def child_running(pid):
    try:
        stat = Path(f"/proc/{pid}/stat").read_text()
    except FileNotFoundError:
        return False
    return stat.rsplit(")", 1)[1].split()[0] != "Z"


def lock_released(path):
    with Path(path).open("rb") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            return False


class Server:
    def __init__(self, home, *, source="流式 A🙂B\n".encode(), rate=0, generator=None,
                 idle=600000, capacity=32, state=None, corpus=None):
        self.home = Path(home)
        self.state = Path(state) if state is not None else self.home / "sessions"
        self.corpus = Path(corpus) if corpus is not None else self.home / "corpus"
        self.corpus.mkdir(exist_ok=True)
        if corpus is None:
            (self.corpus / "only.txt").write_bytes(source)
        self.stdout = self.home / f"server-{time.monotonic_ns()}.log"
        self.output = self.stdout.open("wb")
        self.clients = []
        self.proc = subprocess.Popen([
            str(ROOT / "bin/stream_server"), "--port", "0", "--state", str(self.state),
            "--corpus", str(self.corpus), "--generator", str(generator or ROOT / "bin/text_generator"),
            "--rate", str(rate), "--idle-ms", str(idle), "--max-clients", str(capacity),
        ], cwd=ROOT, stdin=subprocess.DEVNULL, stdout=self.output,
           stderr=subprocess.STDOUT, start_new_session=True)

        def ready():
            text = self.stdout.read_text()
            assert self.proc.poll() is None, text
            match = re.search(r"listening on 127\.0\.0\.1:(\d+)", text)
            return int(match.group(1)) if match else None
        self.port = wait_for(ready, description="server listening")

    @contextmanager
    def connection(self, token=b""):
        if token:
            wait_for(lambda: lock_released(self.state / token.decode() / "session.lock"),
                     description="previous connection to release its session lock")
        with socket.create_connection(("127.0.0.1", self.port), timeout=5) as sock:
            sock.sendall(frame(HELLO, payload=token))
            kind, request, offset, data = receive(sock)
            assert kind == WELCOME and request == offset == 0 and re.fullmatch(rb"[0-9a-f]{32}", data), data
            yield sock, data

    def launch_client(self, name, content=b"", *, cp=None, idle=10000, host="127.0.0.1", resume_only=None):
        input_path = self.home / f"{name}.input"
        input_path.write_bytes(content)
        if resume_only is None:
            resume_only = cp is not None and not content
        cp = Path(cp) if cp is not None else self.home / f"{name}.ckpt"
        stdout, stderr = self.home / f"{name}.stdout", self.home / f"{name}.stderr"
        handles = stdout.open("wb"), stderr.open("wb")
        command = [
            str(ROOT / "bin/stream_client"), "--host", host, "--port", str(self.port),
            "--checkpoint", str(cp), "--input", str(input_path), "--idle-ms", str(idle),
        ]
        if resume_only:
            command.append("--resume-only")
        process = subprocess.Popen(command, cwd=ROOT, stdin=subprocess.DEVNULL,
                                   stdout=handles[0], stderr=handles[1])
        record = dict(process=process, cp=cp, stdout=stdout, stderr=stderr, handles=handles)
        self.clients.append(record)
        return record

    def finish_client(self, client, expected=0, timeout=10):
        process = client["process"]
        code = process.wait(timeout=timeout)
        assert code == expected, (code, client["stderr"].read_text())
        return client["stdout"].read_bytes(), client["stderr"].read_text(), checkpoint(client["cp"])

    def pids(self):
        text = self.stdout.read_text()
        result = {self.proc.pid}
        result.update(map(int, re.findall(r"\[worker (\d+)\]", text)))
        result.update(map(int, re.findall(r"generator=(\d+)", text)))
        return result

    def stop(self, crash=False):
        tracked = self.pids()
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGKILL if crash else signal.SIGTERM)
            assert self.proc.wait(timeout=6) == (-signal.SIGKILL if crash else 0), self.stdout.read_text()
        wait_for(lambda: all(not child_running(pid) for pid in tracked),
                 description="server workers and generators to stop")
        # Linux subreaper mode lets the verifier reap descendants orphaned by SIGKILL.
        for pid in tracked - {self.proc.pid}:
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass
        return tracked

    def close(self):
        for client in self.clients:
            process = client["process"]
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            for handle in client["handles"]:
                handle.close()
        if self.proc.poll() is None:
            try:
                self.stop()
            except (AssertionError, subprocess.TimeoutExpired):
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=3)
        for pid in self.pids() - {self.proc.pid}:
            try:
                os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                pass
        self.output.close()


@contextmanager
def server(**options):
    with tempfile.TemporaryDirectory(prefix="task4-verify-") as temp:
        instance = Server(temp, **options)
        try:
            yield instance
        finally:
            instance.close()


def test_multiturn():
    source = "中文🙂 streamed\n".encode()
    with server(source=source) as s:
        first = s.launch_client("alice", b"Alice secret\nsecond question\n", host="localhost")
        output, errors, cp = s.finish_client(first)
        assert output == (source + b"\n") * 2 and cp["request"] == 2 and cp["done"] == 1
        directory = s.state / cp["token"]
        assert (directory / "context.txt").read_bytes() == b"user: Alice secret\nassistant: " + source + b"\n"
        second = s.launch_client("bob", b"Bob private\nBob second\n")
        _, _, cp2 = s.finish_client(second)
        assert cp["token"] != cp2["token"]
        assert b"Alice" not in (s.state / cp2["token"] / "context.txt").read_bytes()
        assert b"Bob" not in (directory / "context.txt").read_bytes()
        for token in (cp["token"], cp2["token"]):
            d = s.state / token
            for index in (1, 2):
                turn = d / f"turn-{index:08d}"
                assert (turn / "source.txt").read_bytes() == (turn / "response.txt").read_bytes() == source
                assert (turn / "done").exists() and b"backend=corpus" in (turn / "generator.log").read_bytes()
            log = (d / "events.log").read_text()
            assert log.count("event=completed") == 2 and "event=connected" in log
        s.stop()
        assert "all workers reaped" in s.stdout.read_text()
    passed("real clients: multiple questions per connection; isolated tokens, context, turn files and logs")


def test_concurrent_stream():
    source = "甲🙂乙abcdefghijklmno".encode()
    with server(source=source, rate=12) as s:
        a = s.launch_client("a", b"A only\n")
        b = s.launch_client("b", b"B only\n")
        wait_for(lambda: a["stdout"].stat().st_size and b["stdout"].stat().st_size,
                 description="both clients to show partial streamed output")
        prefix_a, prefix_b = a["stdout"].read_bytes(), b["stdout"].read_bytes()
        assert 0 < len(prefix_a) < len(source) and 0 < len(prefix_b) < len(source)
        assert a["process"].poll() is None and b["process"].poll() is None
        a["process"].kill()
        a["process"].wait(timeout=3)
        previous = b["stdout"].stat().st_size
        wait_for(lambda: b["stdout"].stat().st_size > previous, description="survivor stream to keep progressing")
        output, _, cp_b = s.finish_client(b)
        assert output == source + b"\n" and cp_b["done"] == 1
        cp_a = checkpoint(a["cp"])
        assert cp_a["token"] != cp_b["token"] and cp_a["done"] == 0
        assert not (s.state / cp_a["token"] / "turn-00000001" / "done").exists()
        # Resume the killed client from durable bytes; only its unsaved suffix is displayed.
        wait_for(lambda: "event=disconnected" in (s.state / cp_a["token"] / "events.log").read_text())
        resumed = s.launch_client("a-resumed", b"A only\n", cp=a["cp"])
        suffix, _, cp_final = s.finish_client(resumed)
        assert suffix == source[cp_a["offset"]:] + b"\n" and cp_final["answer"] == source
        assert cp_final["request"] == 1 and cp_final["done"] == 1
    passed("two real clients stream concurrently; killing one preserves the other and its checkpoint resumes exactly")


def test_wire_streaming():
    source = "甲🙂乙abc".encode()
    with server(source=source, rate=8) as s:
        with socket.create_connection(("127.0.0.1", s.port), timeout=4) as sock:
            payload = frame(HELLO)
            for start in range(0, len(payload), 3):
                sock.sendall(payload[start:start + 3])
            kind, _, _, token = receive(sock)
            assert kind == WELCOME
            # Split both the header and UTF-8 prompt across transport writes / INPUT frames.
            prompt = "任意问题".encode()
            packet = frame(BEGIN, 1) + frame(INPUT, 1, 0, prompt[:2])
            sock.sendall(packet[:7]); sock.sendall(packet[7:])
            sock.settimeout(0.2)
            try:
                receive(sock)
                raise AssertionError("generation started before COMMIT")
            except socket.timeout:
                pass
            assert not list((s.state / token.decode()).glob("turn-*"))
            sock.settimeout(4)
            start = time.monotonic()
            sock.sendall(frame(INPUT, 1, 2, prompt[2:]) + frame(COMMIT, 1, len(prompt)))
            first = receive(sock)
            assert first == (DELTA, 1, 0, "甲".encode()), first
            assert time.monotonic() - start < 1.5
            tail, _ = answer(sock, offset=3)
            duration = time.monotonic() - start
            assert first[3] + tail == source and duration >= 0.45, duration
    passed("split/coalesced TCP frames, multi-frame UTF-8 input, COMMIT gate, complete-codepoint DELTAs and paced output")


def test_input_boundaries():
    with server(source=b"ok") as s:
        content = b"\n" + b"x" * 4096 + b"\n" + b"x" * 4097 + b"\nbad\0line\nlast without LF"
        client = s.launch_client("limits", content)
        output, errors, cp = s.finish_client(client)
        assert output == b"ok\nok\nok\n" and errors.count("invalid line skipped") == 2
        directory = s.state / cp["token"]
        assert (directory / "turn-00000001/prompt.txt").read_bytes() == b""
        assert len((directory / "turn-00000002/prompt.txt").read_bytes()) == 4096
        assert (directory / "turn-00000003/prompt.txt").read_bytes() == b"last without LF"
        empty = s.launch_client("empty", b"")
        out, _, cp_empty = s.finish_client(empty)
        assert not out and cp_empty["request"] == 0 and cp_empty["done"] == 1
        quit_client = s.launch_client("quit", b"/quit\nignored\n")
        out, _, cp_quit = s.finish_client(quit_client)
        assert not out and cp_quit["request"] == 0
    passed("empty question / file, exact 4096-byte prompt, complete rejection of long/NUL lines, final line without LF and /quit")


def expect_rejected(sock):
    try:
        message = receive(sock)
        assert message[0] == ERROR, message
    except (EOFError, ConnectionResetError):
        pass


def test_protocol_errors():
    with server(source=b"ok", idle=1000) as s:
        prehello = [frame(HELLO, magic=0), frame(HELLO, length=4097),
                    frame(INPUT), frame(HELLO, payload=b"x" * 32), frame(HELLO)[:9]]
        for payload in prehello:
            with socket.create_connection(("127.0.0.1", s.port), timeout=3) as sock:
                sock.sendall(payload); sock.shutdown(socket.SHUT_WR)
                expect_rejected(sock)
        malformed = [
            frame(BEGIN, 0), frame(BEGIN, 1, payload=b"unexpected"),
            frame(BEGIN, 1) + frame(INPUT, 1, 1, b"x"),
            frame(BEGIN, 1) + frame(INPUT, 1, 0, b"x\0"),
            frame(BEGIN, 1) + frame(INPUT, 1, 0, b"x") + frame(COMMIT, 1, 2),
            frame(BEGIN, 1) + frame(COMMIT, 2),
            frame(BEGIN, 1) + frame(INPUT, 1, length=4, payload=b"a"),
            frame(BEGIN, 1) + frame(INPUT, 1, payload=b"a" * 4096) + frame(INPUT, 1, 4096, b"b"),
        ]
        for payload in malformed:
            with s.connection() as (sock, token):
                sock.sendall(payload); sock.shutdown(socket.SHUT_WR)
                expect_rejected(sock)
                assert not list((s.state / token.decode()).glob("turn-*"))
        with s.connection() as (sock, _):
            send_turn(sock, b"still alive")
            assert answer(sock)[0] == b"ok"
        # A slow partial header has a deadline and cannot monopolize a worker forever.
        with socket.create_connection(("127.0.0.1", s.port), timeout=3) as sock:
            sock.sendall(frame(HELLO)[:1])
            start = time.monotonic()
            expect_rejected(sock)
            assert time.monotonic() - start < 2.5
    passed("invalid magic/type/token/length/order/NUL/truncated frames and aggregate prompt overflow rejected; worker deadline and server survival")


def test_replay_and_lock():
    source = "甲🙂ABC".encode()
    with server(source=source) as s:
        with s.connection() as (sock, token):
            send_turn(sock, b"original")
            assert answer(sock)[0] == source
            before = (s.state / token.decode() / "turn-00000001/generator.log").read_bytes()
            send_turn(sock, b"original", resume=3)
            assert answer(sock, offset=3)[0] == source[3:]
            assert (s.state / token.decode() / "turn-00000001/generator.log").read_bytes() == before
            send_turn(sock, b"original", resume=len(source))
            assert answer(sock, offset=len(source))[0] == b""
            with socket.create_connection(("127.0.0.1", s.port), timeout=3) as busy:
                busy.sendall(frame(HELLO, payload=token))
                assert receive(busy)[0] == ERROR
            send_turn(sock, b"changed prompt")
            assert receive(sock)[0] == ERROR
        wait_for(lambda: "event=disconnected" in (s.state / token.decode() / "events.log").read_text())
        for resume in (1, len(source) + 1):
            with s.connection(token) as (sock, _):
                send_turn(sock, b"original", resume=resume)
                expect_rejected(sock)
            time.sleep(0.04)
        with s.connection() as (sock, token2):
            send_turn(sock, b"out of order", request=2)
            assert receive(sock)[0] == ERROR
            assert not list((s.state / token2.decode()).glob("turn-*"))
    passed("idempotent completed-request replay, UTF-8 resume boundaries, offset bounds, changed prompt, sequential IDs and exclusive session lock")


def test_restart_recovery():
    source = "恢复🙂0123456789abcdefghij".encode()
    for crash in (False, True):
        with tempfile.TemporaryDirectory(prefix="task4-restart-") as temp:
            first = Server(temp, source=source, rate=15)
            second = None
            try:
                active = first.launch_client("before", b"keep this question\n")
                wait_for(lambda: checkpoint(active["cp"]) and checkpoint(active["cp"])["offset"] >= 6,
                         description="durable partial answer")
                first.stop(crash=crash)
                assert active["process"].wait(timeout=4) != 0
                cp_before = checkpoint(active["cp"])
                assert cp_before["done"] == 0 and 0 < cp_before["offset"] < len(source)
                second = Server(temp, source=b"NEW corpus must not replace pinned source", rate=0,
                                state=first.state, corpus=first.corpus)
                (first.corpus / "only.txt").write_bytes(b"NEW corpus must not replace pinned source")
                resumed = second.launch_client("after", cp=active["cp"])
                suffix, _, cp_after = second.finish_client(resumed)
                assert cp_after["answer"] == source and cp_after["done"] == 1 and cp_after["request"] == 1
                assert suffix == source[cp_before["offset"]:] + b"\n"
                turn = second.state / cp_after["token"] / "turn-00000001"
                assert (turn / "response.txt").read_bytes() == (turn / "source.txt").read_bytes() == source
                assert len(list((second.state / cp_after["token"]).glob("turn-*"))) == 1
            finally:
                if second:
                    second.close()
                first.close()
    passed("graceful server shutdown and SIGKILL both stop descendants; restart resumes saved turn without loss/duplication or corpus reselection")


def fake_generator(home, name, body):
    path = Path(home) / name
    path.write_text(f"#!{sys.executable}\nimport pathlib, sys, time\n"
                    "args = dict(zip(sys.argv[1::2], sys.argv[2::2]))\n" + body)
    path.chmod(0o700)
    return path


def test_heartbeat_and_faults():
    with tempfile.TemporaryDirectory(prefix="task4-generators-") as temp:
        slow = fake_generator(temp, "slow", "time.sleep(3.2)\n"
                              "data=pathlib.Path(args['--source']).read_bytes()\n"
                              "sys.stdout.buffer.write(data[int(args['--offset']):])\n"
                              "sys.stdout.buffer.flush()\n")
        with server(generator=slow, source=b"slow answer") as s:
            with s.connection() as (sock, _):
                send_turn(sock, b"slow")
                start = time.monotonic()
                response, heartbeats = answer(sock)
                assert response == b"slow answer" and heartbeats >= 2 and time.monotonic() - start >= 3
            real = s.launch_client("slow-real", b"question\n", idle=1500)
            out, _, cp = s.finish_client(real)
            assert out == b"slow answer\n" and cp["done"] == 1
        failure = fake_generator(temp, "failure", "print('deliberate generator failure', file=sys.stderr)\n"
                                 "sys.stdout.buffer.write(b'valid-prefix')\n"
                                 "sys.stdout.buffer.flush()\nsys.exit(7)\n")
        invalid = fake_generator(temp, "invalid", "sys.stdout.buffer.write(b'\\xed\\xa0\\x80')\n"
                                 "sys.stdout.buffer.flush()\n")
        for generator in (failure, invalid, Path(temp) / "missing-generator"):
            with server(generator=generator, source=b"unrelated") as s:
                client = s.launch_client("failure", b"question\n")
                _, errors, cp = s.finish_client(client, expected=1)
                assert "generation interrupted" in errors and cp["done"] == 0
                turn = s.state / cp["token"] / "turn-00000001"
                assert not (turn / "done").exists()
                if generator == failure:
                    assert cp["answer"] == b"valid-prefix"
                    assert "deliberate generator failure" in (turn / "generator.log").read_text()
                elif generator == invalid:
                    assert cp["offset"] == 0
                else:
                    assert "exec generator" in (turn / "generator.log").read_text()
    passed("slow first token remains live via non-progress heartbeats; real client idle timeout survives; failed/missing/invalid-UTF-8 generators retain recoverable state")


def test_checkpoint_errors():
    with server(source=b"answer", rate=5) as s:
        active = s.launch_client("held", b"question\n")
        wait_for(lambda: active["stdout"].stat().st_size > 0)
        competing = s.launch_client("competing", cp=active["cp"])
        _, errors, _ = s.finish_client(competing, expected=1)
        assert "checkpoint open/lock" in errors
        s.finish_client(active)
        bad = s.home / "bad.ckpt"
        bad.write_bytes(b"T4CHECK2\nnot-a-token\n1 0 0\n")
        client = s.launch_client("corrupt", cp=bad)
        assert client["process"].wait(timeout=3) == 1
        assert "checkpoint open/lock" in client["stderr"].read_text()
        missing = s.launch_client("bad-parent", b"question\n", cp=s.home / "missing/answer.ckpt")
        assert missing["process"].wait(timeout=3) == 1
        target = s.home / "preserved"
        target.write_bytes(b"keep this file")
        link = s.home / "link.ckpt"
        link.symlink_to(target)
        linked = s.launch_client("linked", cp=link)
        assert linked["process"].wait(timeout=3) == 1 and target.read_bytes() == b"keep this file"
    passed("checkpoint exclusive lock, malformed file, missing parent and symlink rejection protect saved client data")


def test_corpus_and_parameters():
    with tempfile.TemporaryDirectory(prefix="task4-corpus-") as temp:
        home = Path(temp)
        corpus = home / "choices"
        corpus.mkdir()
        choices = {b"first", "第二🙂".encode(), b"third"}
        for index, data in enumerate(choices):
            (corpus / f"choice-{index}.txt").write_bytes(data)
        (corpus / "directory.txt").mkdir()
        (corpus / "symlink.txt").symlink_to(corpus / "choice-0.txt")
        (corpus / "ignored.bin").write_bytes(b"not eligible")
        with server(corpus=corpus) as s:
            selected = set()
            for _ in range(16):
                with s.connection() as (sock, token):
                    send_turn(sock, b"irrelevant")
                    result, _ = answer(sock)
                    assert result in choices
                    selected.add(result)
                    assert (s.state / token.decode() / "turn-00000001/source.txt").read_bytes() == result
            assert len(selected) >= 2, "16 independently randomized selections unexpectedly all equal"
        for content in (b"", b"bad\0corpus", b"\xc0\xaf", b"\xf4\x90\x80\x80", b"\xe4\xb8"):
            with server(source=content) as s:
                client = s.launch_client("corpus-boundary", b"q\n")
                out, _, cp = s.finish_client(client, expected=0 if not content else 1)
                assert cp["done"] == (0 if content else 1)
                if not content:
                    assert out == b"\n" and cp["offset"] == 0
        source = home / "direct.txt"
        source.write_bytes("甲🙂B".encode())
        direct = subprocess.run([str(ROOT / "bin/text_generator"), "--source", str(source),
                                 "--offset", "3", "--rate", "0"], capture_output=True, timeout=3)
        assert direct.returncode == 0 and direct.stdout == "🙂B".encode()
        for args in (["--offset", "1"], ["--offset", "99"], ["--rate", "-1"], ["--rate", "1x"]):
            result = subprocess.run([str(ROOT / "bin/text_generator"), "--source", str(source), *args],
                                    capture_output=True, timeout=3)
            assert result.returncode == 2, (args, result.stderr)
        invalid_args = {
            "stream_server": [["--port", "-1"], ["--port", "65536"], ["--rate", " 1"],
                              ["--rate", "10001"], ["--max-clients", "0"], ["--bad", "1"], ["--port"]],
            "stream_client": [["--port", "0"], ["--port", "1x"], ["--idle-ms", "1499"], ["--bad", "1"]],
        }
        for executable, variants in invalid_args.items():
            for args in variants:
                result = subprocess.run([str(ROOT / "bin" / executable), *args], capture_output=True, timeout=3)
                assert result.returncode == 2, (executable, args, result.stderr)
    passed("independent random regular-.txt selection; empty/invalid corpus handling; standalone UTF-8 resume; strict program argument validation")


def test_client_protocol_validation():
    replies = [
        frame(DELTA, 1, 1, b"x"), frame(DELTA, 2, 0, b"x"),
        frame(DELTA, 1, 0, b"\xe4"), frame(DELTA, 1, 0, b"xy"),
        frame(END, 1, 9), frame(DELTA, 1, 0, length=4097),
        frame(DELTA, 1, 0, b"x", magic=1), frame(DELTA, 1, 0, b"x")[:10], b"",
    ]
    with tempfile.TemporaryDirectory(prefix="task4-client-wire-") as temp:
        home = Path(temp)
        for index, reply in enumerate(replies):
            with socket.socket() as listener:
                listener.bind(("127.0.0.1", 0)); listener.listen(1); listener.settimeout(4)

                def serve():
                    with listener.accept()[0] as sock:
                        sock.settimeout(3)
                        assert receive(sock)[0] == HELLO
                        sock.sendall(frame(WELCOME, payload=b"a" * 32))
                        assert receive(sock)[0] == BEGIN
                        while receive(sock)[0] != COMMIT:
                            pass
                        if reply:
                            sock.sendall(reply)

                input_path, cp = home / f"input-{index}", home / f"cp-{index}"
                input_path.write_bytes(b"q\n")
                with ThreadPoolExecutor(max_workers=1) as pool:
                    future = pool.submit(serve)
                    result = subprocess.run([
                        str(ROOT / "bin/stream_client"), "--port", str(listener.getsockname()[1]),
                        "--checkpoint", str(cp), "--input", str(input_path),
                    ], capture_output=True, timeout=6)
                    future.result(timeout=5)
                assert result.returncode == 1 and result.stdout == b"", (index, result)
                saved = checkpoint(cp)
                assert saved["done"] == 0 and saved["offset"] == 0
    passed("independent fake server: client rejects wrong request/offset, invalid DELTA, bad END/magic/length and missing/truncated replies without saving partial output")


def test_capacity_and_storage():
    with server(capacity=1, source=b"healthy") as s:
        with s.connection() as (sock, _):
            with socket.create_connection(("127.0.0.1", s.port), timeout=3) as extra:
                message = receive(extra)
                assert message[0] == ERROR and b"capacity" in message[3]
            send_turn(sock, b"admitted connection continues")
            assert answer(sock)[0] == b"healthy"
        wait_for(lambda: all(not child_running(pid) for pid in s.pids() - {s.proc.pid}))
        with s.connection() as (sock, token):
            send_turn(sock, b"next connection")
            assert answer(sock)[0] == b"healthy"
        lock = s.state / token.decode() / "session.lock"
        wait_for(lambda: lock_released(lock))
        # Refusing a forged log path must not open/truncate the symlink target.
        log = s.state / token.decode() / "events.log"
        log.unlink()
        preserved = s.home / "preserved-log-target"
        preserved.write_bytes(b"preserve me")
        log.symlink_to(preserved)
        with socket.create_connection(("127.0.0.1", s.port), timeout=3) as forged:
            forged.sendall(frame(HELLO, payload=token))
            assert receive(forged)[0] == ERROR
        assert preserved.read_bytes() == b"preserve me"
    with tempfile.TemporaryDirectory(prefix="task4-storage-errors-") as temp:
        home = Path(temp)
        invalid_state = home / "state-is-file"
        invalid_state.write_bytes(b"keep original data")
        s = Server(home, state=invalid_state)
        try:
            with socket.create_connection(("127.0.0.1", s.port), timeout=3) as sock:
                sock.sendall(frame(HELLO))
                assert receive(sock)[0] == ERROR
            assert invalid_state.read_bytes() == b"keep original data"
        finally:
            s.close()
        corpus = home / "empty-corpus"
        corpus.mkdir()
        with server(corpus=corpus) as s:
            client = s.launch_client("no-corpus", b"question\n")
            _, errors, cp = s.finish_client(client, expected=1)
            assert "request rejected" in errors and cp["done"] == 0
            assert not list((s.state / cp["token"]).glob("turn-*"))
    passed("connection capacity leaves admitted client healthy; slots are reusable; forged log/state paths preserve files; missing eligible corpus fails cleanly")


def test_checkpoint_journal():
    source = "甲🙂Z".encode()
    content = b"unchanged first question\nsecond question\n"
    with server(source=source) as s:
        original = s.launch_client("journal-original", content)
        _, _, cp = s.finish_client(original)
        directory = s.state / cp["token"]
        wait_for(lambda: lock_released(directory / "session.lock"))
        repeated = s.launch_client("journal-repeat", content, cp=original["cp"])
        out, _, repeated_cp = s.finish_client(repeated)
        assert out == b"" and repeated_cp["request"] == 2 and repeated_cp["file_lines"] == 2
        assert len(list(directory.glob("turn-*"))) == 2
        wait_for(lambda: lock_released(directory / "session.lock"))
        saved = original["cp"].read_bytes()
        # The final ASCII DELTA body is incomplete and END is absent: only that
        # last record rolls back; the complete Chinese / emoji prefix remains.
        original["cp"].write_bytes(saved[:-17])
        partial = checkpoint(original["cp"])
        assert partial["answer"] == source[:-1] and partial["torn_bytes"] == 16 and not partial["done"]
        recovered = s.launch_client("journal-torn-body", content, cp=original["cp"])
        out, _, recovered_cp = s.finish_client(recovered)
        assert out == b"Z\n" and recovered_cp["answer"] == source and recovered_cp["done"]
        assert recovered_cp["request"] == 2 and recovered_cp["torn_bytes"] == 0
        wait_for(lambda: lock_released(directory / "session.lock"))
        final = original["cp"].read_bytes()
        original["cp"].write_bytes(final + b"\x06\0\0")
        assert checkpoint(original["cp"])["torn_bytes"] == 3
        tail = s.launch_client("journal-torn-header", content, cp=original["cp"])
        out, _, clean_cp = s.finish_client(tail)
        assert out == b"" and clean_cp["done"] and clean_cp["torn_bytes"] == 0
        assert original["cp"].read_bytes() == final
        corrupted = bytearray(final)
        corrupted[-1] ^= 1  # The complete END checksum is wrong; no rollback is allowed.
        original["cp"].write_bytes(corrupted)
        bad = s.launch_client("journal-bad-checksum", cp=original["cp"])
        assert bad["process"].wait(timeout=3) == 1
        assert "checkpoint open/lock" in bad["stderr"].read_text()
        assert original["cp"].read_bytes() == corrupted
        original["cp"].write_bytes(final)
        shorter = s.launch_client("journal-shorter-input", b"one line\n", cp=original["cp"])
        assert shorter["process"].wait(timeout=3) == 1
        assert original["cp"].read_bytes() == final
    passed("incremental checkpoint journal: same input rerun creates no extra turns, torn body/header recover, complete checksum corruption and shorter input fail without overwriting")


def main():
    # Keep crash-test grandchildren reaped even when the surrounding init is minimal.
    libc = ctypes.CDLL(None, use_errno=True)
    assert libc.prctl(36, 1, 0, 0, 0) == 0, "cannot enable Linux child subreaper"
    started = time.monotonic()
    log = ROOT / "logs/verification.txt"
    log.parent.mkdir(exist_ok=True)
    try:
        for test in (test_multiturn, test_concurrent_stream, test_wire_streaming,
                     test_input_boundaries, test_protocol_errors, test_replay_and_lock,
                     test_restart_recovery, test_heartbeat_and_faults, test_checkpoint_errors,
                     test_corpus_and_parameters, test_client_protocol_validation,
                     test_capacity_and_storage, test_checkpoint_journal):
            test()
        summary = f"All {len(CHECKS)} integration checks passed ({time.monotonic() - started:.1f}s)."
        print(summary, flush=True)
        log.write_text("\n".join(CHECKS + [summary]) + "\n")
    except BaseException:
        details = traceback.format_exc()
        log.write_text("\n".join(CHECKS) + "\nFAILED\n" + details)
        raise


if __name__ == "__main__":
    main()
