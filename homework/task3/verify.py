#!/usr/bin/env python3
"""启动真实 C 程序，检查 TCP 字节流边界、文件结果和错误处理。"""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent
CHECKS = 0


def run(program, *args):
    return subprocess.run(
        [str(ROOT / program), *map(str, args)], cwd=ROOT,
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, timeout=8,
    )


def check(label):
    global CHECKS
    CHECKS += 1
    print(f"PASS {CHECKS:02d}: {label}", flush=True)


@contextmanager
def session():
    with tempfile.TemporaryDirectory(prefix="task3-verify-") as temp:
        path = Path(temp)
        stdout = path / "server.stdout"
        records = path / "records.txt"
        records.write_text("previous session preserved\n")
        with stdout.open("w") as out:
            server = subprocess.Popen(
                [str(ROOT / "tcp_server"), "0", str(records)],
                stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 5
                while True:
                    text = stdout.read_text()
                    match = re.search(r"listening on 127\.0\.0\.1:(\d+)", text)
                    if match:
                        break
                    assert server.poll() is None, text
                    assert time.monotonic() < deadline, text
                    time.sleep(0.01)
                yield path, server, int(match.group(1))
            finally:
                if server.poll() is None:
                    server.terminate()
                    try:
                        server.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        server.kill()
                        server.wait(timeout=3)


def finish(path, server, expected=0):
    assert server.wait(timeout=5) == expected, (path / "server.stdout").read_text()
    return (path / "records.txt").read_bytes()


def client(path, port, content, host="127.0.0.1"):
    (path / "input.txt").write_bytes(content)
    return run("tcp_client", host, port, path / "input.txt", path / "output.txt")


def frame(content):
    return struct.pack("!I", len(content)) + content


def exact(sock, length):
    result = b""
    while len(result) < length:
        data = sock.recv(length - len(result))
        assert data, "unexpected socket EOF"
        result += data
    return result


def receive(sock):
    length = struct.unpack("!I", exact(sock, 4))[0]
    assert length <= 1024
    return exact(sock, length)


def malformed_request(payload):
    with session() as (path, server, port):
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            sock.sendall(payload)
            sock.shutdown(socket.SHUT_WR)
            finish(path, server, 1)
        assert "receive request:" in (path / "server.stdout").read_text()


def fake_reply(payload, reset=False):
    with tempfile.TemporaryDirectory(prefix="task3-fake-") as temp:
        path = Path(temp)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            listener.settimeout(5)

            def serve():
                connection, _ = listener.accept()
                with connection:
                    connection.settimeout(3)
                    assert receive(connection) == b"hello"
                    if reset:
                        connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                              struct.pack("ii", 1, 0))
                    elif payload:
                        connection.sendall(payload)

            with ThreadPoolExecutor(max_workers=1) as pool:
                future = pool.submit(serve)
                result = client(path, listener.getsockname()[1], b"hello\n")
                future.result(timeout=5)
            assert result.returncode == 1, result.stdout
            assert "receive reply:" in result.stdout, result.stdout
            assert (path / "output.txt").read_bytes() == b""


def main():
    with session() as (path, server, port):
        lines = [f"message-{i:03d}".encode() for i in range(1, 101)]
        lines += [b"", b"quit", "linux 中文".encode()]
        result = client(path, port, b"\n".join(lines), host="localhost")
        assert result.returncode == 0, result.stdout
        assert (path / "output.txt").read_bytes() == b"\n".join(s.upper() for s in lines) + b"\n"
        records = finish(path, server)
        assert records.startswith(b"previous session preserved\n")
        for index, line in enumerate(lines, 1):
            assert f"recv #{index} ({len(line)} bytes): ".encode() + line + b"\n" in records
            assert f"send #{index} ({len(line)} bytes): ".encode() + line.upper() + b"\n" in records
        assert result.stdout.count("[client] recv #") == 103
        check("103 file round trips via localhost; UTF-8, empty line, literal quit, final line without LF; append log")

    with session() as (path, server, port):
        result = client(path, port, b"a" * 1024 + b"\n" + b"b" * 1025 + b"\nbad\0line\nafter limit\n")
        assert result.returncode == 0, result.stdout
        assert result.stdout.count("invalid (max 1024 bytes, no NUL); skipped.") == 2
        assert (path / "output.txt").read_bytes() == b"A" * 1024 + b"\nAFTER LIMIT\n"
        finish(path, server)
        check("1024 bytes accepted; 1025-byte and NUL lines fully skipped; following line intact")

    with session() as (path, server, port):
        result = client(path, port, b"")
        assert result.returncode == 0, result.stdout
        assert (path / "output.txt").read_bytes() == b""
        assert b"recv #" not in finish(path, server)
        check("empty file closes both ends normally and creates an empty reply file")

    with session() as (path, server, port):
        with socket.create_connection(("127.0.0.1", port), timeout=3) as sock:
            # 先发送不完整报头，确认服务器不会提前把它当作一条消息。
            data = frame(b"fragmented")
            sock.sendall(data[:2])
            sock.settimeout(0.15)
            try:
                data_before_header = sock.recv(1)
                raise AssertionError(f"reply/EOF before complete header: {data_before_header!r}")
            except socket.timeout:
                pass
            sock.settimeout(3)
            for byte in data[2:]:
                sock.sendall(bytes([byte]))
            assert receive(sock) == b"FRAGMENTED"
            # 一次发送多个完整帧，不要求一次 recv 恰好返回一个帧。
            sock.sendall(frame(b"one") + frame(b"") + frame(b"three"))
            assert [receive(sock) for _ in range(3)] == [b"ONE", b"", b"THREE"]
            sock.shutdown(socket.SHUT_WR)
            assert sock.recv(1) == b""
        finish(path, server)
        check("fragmented header/body and coalesced frames preserve message boundaries, including zero length")

    for payload in [b"\0\0", struct.pack("!I", 5), frame(b"hello")[:-2],
                    struct.pack("!I", 1025), frame(b"a\0b")]:
        malformed_request(payload)
    check("truncated header/body, absent body, oversize length and embedded NUL rejected with exit 1")

    for payload in [b"", b"\0\0", struct.pack("!I", 4), frame(b"hello")[:-1],
                    struct.pack("!I", 1025), frame(b"a\0b")]:
        fake_reply(payload)
    fake_reply(b"", reset=True)
    check("client rejects missing/truncated/oversize/NUL replies and reset connections; saves no partial reply")

    with session() as (path, server, port):
        duplicate = run("tcp_server", port, path / "duplicate.txt")
        assert duplicate.returncode == 1 and "Address already in use" in duplicate.stdout
        assert not (path / "duplicate.txt").exists()
        result = client(path, port, b"still alive\n")
        assert result.returncode == 0, result.stdout
        assert (path / "output.txt").read_bytes() == b"STILL ALIVE\n"
        finish(path, server)
        check("duplicate bind fails without changing original server or its files")

    with tempfile.TemporaryDirectory(prefix="task3-errors-") as temp:
        path = Path(temp)
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))  # 保留但不监听，避免选端口后的竞争。
            result = client(path, unused.getsockname()[1], b"hello\n")
        assert result.returncode == 1 and "connect (start the server first)" in result.stdout
        check("no listening server returns exit 1 rather than waiting for a reply")

        result = run("tcp_client", "127.0.0.1", 3339, path / "missing", path / "new.txt")
        assert result.returncode == 1 and "fopen (input)" in result.stdout
        assert not (path / "new.txt").exists()
        result = run("tcp_client", "127.0.0.1", 3339, path / "input.txt", path / "missing/output")
        assert result.returncode == 1 and "fopen (new output" in result.stdout
        result = run("tcp_server", 0, path / "missing/records")
        assert result.returncode == 1 and "fopen (server log)" in result.stdout
        check("missing input and invalid output/log directories reported without success")

        source = path / "input.txt"
        alias = path / "alias.txt"
        alias.symlink_to(source)
        for destination in [source, alias, path / "output.txt"]:
            before = destination.read_bytes()
            result = run("tcp_client", "127.0.0.1", 3339, source, destination)
            assert result.returncode == 1 and "choose a non-existing file" in result.stdout
            assert destination.read_bytes() == before
        check("existing output, input-as-output and symlink alias refused without truncating files")

        result = run("tcp_server", 0, "/dev/full")
        assert result.returncode == 1 and "write server log" in result.stdout
        check("buffered file write failure detected by fflush using /dev/full")

    for bad_port in ["", "-1", "+5", "12x", "65536", "99999999999999999999999"]:
        assert run("tcp_server", bad_port).returncode == 2
        assert run("tcp_client", "127.0.0.1", bad_port).returncode == 2
    assert run("tcp_client", "127.0.0.1", 0).returncode == 2
    assert run("tcp_server", 0, "unused.txt", "not-an-ip").returncode == 2
    check("invalid ports and bind addresses rejected with usage exit 2")

    print(f"All {CHECKS} integration checks passed; test processes and temporary files cleaned up.")


if __name__ == "__main__":
    main()
