#!/usr/bin/env python3
"""真实启动 C 程序，检查收发、边界、会话隔离和 IPC 资源回收。"""

import ctypes
import errno
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent
LIBC = ctypes.CDLL(None, use_errno=True)
LIBC.ftok.argtypes = [ctypes.c_char_p, ctypes.c_int]
LIBC.shmget.argtypes = [ctypes.c_int, ctypes.c_size_t, ctypes.c_int]
LIBC.semget.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int]


def key_for(path):
    key = LIBC.ftok(os.fsencode(path), ord("2"))
    assert key != -1
    return key


def assert_removed(path):
    key = key_for(path)
    assert LIBC.shmget(key, 1, 0o600) == -1 and ctypes.get_errno() == errno.ENOENT
    assert LIBC.semget(key, 0, 0o600) == -1 and ctypes.get_errno() == errno.ENOENT


def wait_text(path, needle, process):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        text = path.read_text()
        if needle in text:
            return text
        if process.poll() is not None:
            raise AssertionError(f"Process exited before {needle!r}:\n{text}")
        time.sleep(0.01)
    raise AssertionError(f"Timed out waiting for {needle!r}:\n{path.read_text()}")


def run_client(path, text):
    return subprocess.run(
        [str(ROOT / "ipc_client"), str(path)], input=text,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5,
    )


class Session:
    def __enter__(self):
        self.temp = tempfile.TemporaryDirectory(prefix="task2-verify-")
        self.path = Path(self.temp.name)
        self.log = self.path / "server.log"
        self.output = self.log.open("w")
        self.server = subprocess.Popen(
            [str(ROOT / "ipc_server"), str(self.path)], stdin=subprocess.DEVNULL,
            stdout=self.output, stderr=subprocess.STDOUT,
        )
        try:
            wait_text(self.log, "ready; waiting", self.server)
        except BaseException:
            self.__exit__(None, None, None)
            raise
        return self

    def finish(self, expected=0):
        assert self.server.wait(timeout=5) == expected, self.log.read_text()
        assert_removed(self.path)
        return self.log.read_text()

    def __exit__(self, *unused):
        if self.server.poll() is None:
            self.server.send_signal(signal.SIGCONT)
            self.server.terminate()
            self.server.wait(timeout=5)
        self.output.close()
        assert_removed(self.path)
        self.temp.cleanup()


def show(label, output=""):
    print(f"PASS: {label}")
    if output:
        print(output.rstrip())
    print()


def main():
    with tempfile.TemporaryDirectory(prefix="task2-no-server-") as path:
        client = run_client(path, "quit\n")
        assert client.returncode == 1 and "start the server first" in client.stdout
        assert_removed(path)
        show("client started before server returns 1 and creates no IPC objects", client.stdout)

    with Session() as session:
        messages = [f"message-{i:03d}" for i in range(1, 101)]
        messages += ["quit123", "", "linux 中文"]
        client = run_client(session.path, "\n".join(messages + ["quit"]) + "\n")
        assert client.returncode == 0, client.stdout
        replies = [line for line in client.stdout.splitlines() if "reply #" in line]
        expected = [f"[client] reply #{i}: {text.upper()}"
                    for i, text in enumerate(messages, 1)]
        assert replies == expected
        server_output = session.finish()
        assert server_output.count("[server] request #") == 103
        assert "client acknowledged the final reply" in server_output
        final_reply = [line for line in client.stdout.splitlines() if "final reply:" in line]
        assert final_reply == ["[client] final reply: bye"]
        show("103 ordered round trips; quit prefix, empty text and UTF-8; cleanup",
             "\n".join(replies[:2] + replies[-3:] + final_reply))

    with Session() as session:
        client = run_client(session.path, "a" * 255 + "\n" + "b" * 256 + "\nafter limit\nquit\n")
        assert client.returncode == 0, client.stdout
        assert "reply #1: " + "A" * 255 + "\n" in client.stdout
        assert client.stdout.count("Input too long") == 1
        assert "reply #2: AFTER LIMIT\n" in client.stdout
        assert "reply #3:" not in client.stdout
        session.finish()
        show("255-byte line accepted; 256-byte line fully discarded; next line intact")

    with Session() as session:
        client = run_client(session.path, "last line without newline")
        assert client.returncode == 0, client.stdout
        assert "reply #1: LAST LINE WITHOUT NEWLINE" in client.stdout
        assert "EOF -> request shutdown" in client.stdout
        session.finish()
        show("EOF without a trailing newline sends last line, then shuts down", client.stdout)

    with Session() as session:
        duplicate = subprocess.run(
            [str(ROOT / "ipc_server"), str(session.path)], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5,
        )
        assert duplicate.returncode == 1 and "File exists" in duplicate.stdout
        client = run_client(session.path, "still alive\nquit\n")
        assert client.returncode == 0 and "STILL ALIVE" in client.stdout
        session.finish()
        show("duplicate server rejected without deleting first server's resources", duplicate.stdout)

    with Session() as session:
        client_log = session.path / "client.log"
        with client_log.open("w") as output:
            first = subprocess.Popen(
                [str(ROOT / "ipc_client"), str(session.path)], stdin=subprocess.PIPE,
                stdout=output, stderr=subprocess.STDOUT, text=True,
            )
            try:
                wait_text(client_log, "enter text", first)
                second = run_client(session.path, "quit\n")
                assert second.returncode == 1 and "already claimed" in second.stdout
                first.communicate("original client\nquit\n", timeout=5)
                assert first.returncode == 0
            finally:
                if first.poll() is None:
                    first.terminate()
                    first.communicate(timeout=5)
        assert "ORIGINAL CLIENT" in client_log.read_text()
        session.finish()
        show("second client rejected; original client session remains usable", second.stdout)

    with Session() as session:
        session.server.send_signal(signal.SIGINT)
        output = session.finish(130)
        show("SIGINT interrupts idle server; exit 130; both IPC objects removed", output)

    with Session() as session:
        # 暂停服务器，确保客户端正在等待回复，然后终止并恢复服务器。
        session.server.send_signal(signal.SIGSTOP)
        client_log = session.path / "waiting-client.log"
        with client_log.open("w") as output:
            client = subprocess.Popen(
                [str(ROOT / "ipc_client"), str(session.path)], stdin=subprocess.PIPE,
                stdout=output, stderr=subprocess.STDOUT, text=True,
            )
            try:
                client.stdin.write("waiting request\n")
                client.stdin.flush()
                wait_text(client_log, "[client] send:", client)
                session.server.terminate()
                session.server.send_signal(signal.SIGCONT)
                session.finish(143)
                client.communicate(timeout=5)
                assert client.returncode == 1
                assert "request/reply:" in client_log.read_text()
            finally:
                if client.poll() is None:
                    client.terminate()
                    client.communicate(timeout=5)
        show("server SIGTERM releases waiting client with error; no hang or IPC leak",
             client_log.read_text())

    with tempfile.TemporaryDirectory(prefix="task2-partial-") as path:
        key = key_for(path)
        semid = LIBC.semget(key, 3, 0o1000 | 0o2000 | 0o600)
        assert semid >= 0
        try:
            server = subprocess.run(
                [str(ROOT / "ipc_server"), path], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=5,
            )
            assert server.returncode == 1 and "semget (exclusive create)" in server.stdout
            assert LIBC.shmget(key, 1, 0o600) == -1 and ctypes.get_errno() == errno.ENOENT
            assert LIBC.semget(key, 3, 0o600) == semid
            show("partial setup failure removes owned shared memory, preserves existing semaphore",
                 server.stdout)
        finally:
            assert LIBC.semctl(semid, 0, 0) == 0  # IPC_RMID = 0 on Linux.
        assert_removed(path)

    print("All 9 integration checks passed; all test-owned IPC objects removed.")


if __name__ == "__main__":
    main()
