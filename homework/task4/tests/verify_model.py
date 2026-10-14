#!/usr/bin/env python3
"""C worker + real Python adapter against a controllable local llama HTTP fixture.

No weights/downloads are needed. The fixture checks protocol behavior rather
than model quality; scripts/model/smoke.py supplies separate real-GPU evidence.
"""
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ctypes
import json
import socket
import tempfile
import threading
import time

from verify import (Server, ROOT, frame, receive, send_turn, wait_for,
                    DELTA, END, ERROR, HEARTBEAT)

THINK, CANCEL, CANCELLED, STATUS = 10, 11, 12, 13


class LocalModel(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self):
        super().__init__(("127.0.0.1", 0), Handler)
        self.templates = []
        self.prompts = []
        self.completions = []
        self.lock = threading.Lock()
        self.identity = "fixture-Qwen3.5-4B.gguf"


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def json_response(self, data):
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        assert self.path == "/props", self.path
        self.json_response({"model_path": self.server.identity, "chat_template": "fixture",
            "build_info": "test-v1", "default_generation_settings": {"n_ctx": 4096}})

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path == "/apply-template":
            with self.server.lock:
                self.server.templates.append(data)
            prompt = json.dumps(data["messages"], ensure_ascii=False) + "\n<assistant>\n"
            if data.get("chat_template_kwargs", {}).get("enable_thinking", True):
                prompt += "<think>\n"
            self.json_response({"prompt": prompt})
        elif self.path == "/tokenize":
            self.json_response({"tokens": list(range(40))})
        elif self.path == "/completion":
            with self.server.lock:
                self.server.prompts.append(data["prompt"])
                self.server.completions.append(data)
                attempt = sum("cutoff" in p for p in self.server.prompts)
            prompt = data["prompt"]
            # The frozen generated prefix follows the final assistant opener.
            prefix = prompt.rsplit("<assistant>\n", 1)[1]
            thinking = prefix.startswith("<think>")
            generated = prefix[len("<think>\n"):] if thinking else prefix
            answer = "完整回答：你好🙂。" + ("续写完成。" if "resume" in prompt else "")
            reasoning = "PRIVATE_REASONING，检查上下文。\n"
            full = reasoning + "</think>\n\n" + answer if thinking else answer
            assert full.startswith(generated), (full, generated)
            remainder = full[len(generated):]
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            try:
                if "slow" in prompt and not generated:
                    time.sleep(1.7)  # C heartbeat must keep the client alive.
                for i, char in enumerate(remainder):
                    if "cutoff" in prompt and attempt == 1 and i == 8:
                        self.event({"content": "", "stop": True, "stop_type": "limit",
                                    "truncated": False})
                        return
                    self.event({"content": char, "tokens": [i + 1], "stop": False})
                    time.sleep(0.009)
                self.event({"content": "", "stop": True, "stop_type": "eos", "truncated": False,
                            "timings": {"predicted_n": len(remainder), "predicted_per_second": 100.0}})
            except (BrokenPipeError, ConnectionResetError):
                pass  # Closing adapter HTTP is the cancellation signal.
        else:
            raise AssertionError(self.path)

    def event(self, data):
        self.wfile.write(b"data: " + json.dumps(data, ensure_ascii=False).encode() + b"\n\n")
        self.wfile.flush()


@contextmanager
def fixture(**options):
    with tempfile.TemporaryDirectory(prefix="task4-model-check-") as temp:
        llm = LocalModel()
        thread = threading.Thread(target=llm.serve_forever, daemon=True)
        thread.start()
        path = Path(temp) / "model.json"
        path.write_text(json.dumps({"endpoint": f"http://127.0.0.1:{llm.server_port}",
            "model": "Qwen3.5-4B", "thinking": True, "max_tokens": 512, "context_tokens": 4096,
            **options}))
        server = Server(temp, model_config=path)
        try:
            yield server, llm, path
        finally:
            server.close()
            llm.shutdown()
            llm.server_close()
            thread.join(timeout=3)


def collect(sock, request=1, offset=0, stop_at=None):
    answer, thinking, heartbeats = bytearray(), bytearray(), 0
    while True:
        kind, req, at, body = receive(sock)
        if kind == ERROR:
            return kind, bytes(answer), bytes(thinking), heartbeats, body
        assert req == request
        if kind == DELTA:
            assert at == offset and len(body.decode()) == 1
            answer.extend(body)
            offset += len(body)
            if stop_at and len(answer) >= stop_at:
                return kind, bytes(answer), bytes(thinking), heartbeats, b""
        elif kind == THINK:
            thinking.extend(body)
        elif kind == STATUS:
            body.decode()
        elif kind == HEARTBEAT:
            heartbeats += 1
        else:
            assert kind in (END, CANCELLED) and at == offset and not body, (kind, at, offset, body)
            return kind, bytes(answer), bytes(thinking), heartbeats, b""


def main():
    ctypes.CDLL(None, use_errno=True).prctl(36, 1, 0, 0, 0)
    count = 0
    checks = []

    def passed(label):
        nonlocal count
        count += 1
        line = f"PASS M{count:02}: {label}"
        checks.append(line)
        print(line, flush=True)

    with fixture() as (server, llm, _):
        with server.connection() as (sock, token):
            send_turn(sock, b"first secret")
            end, text, thought, _, _ = collect(sock)
            assert end == END and "你好🙂" in text.decode() and b"PRIVATE_REASONING" in thought
            send_turn(sock, b"second question", 2)
            assert collect(sock, 2)[0] == END
            messages = llm.templates[-1]["messages"]
            assert [m["role"] for m in messages][-3:] == ["user", "assistant", "user"]
            assert messages[-2]["content"].encode() == text
            assert "PRIVATE_REASONING" not in json.dumps(messages)
            turn = server.state / token.decode() / "turn-00000001"
            assert b"PRIVATE_REASONING" in (turn / "thinking.txt").read_bytes()
        with server.connection() as (other, _):
            send_turn(other, b"separate")
            assert collect(other)[0] == END
            assert "first secret" not in json.dumps(llm.templates[-1])
        passed("split thinking tags, UTF-8 final stream, completed-only multi-turn history, session isolation")

    with fixture() as (server, llm, _):
        with server.connection() as (a, token), server.connection() as (b, _):
            send_turn(a, b"slow cancel this")
            send_turn(b, b"another independent question")
            seen_heartbeat = False
            while True:
                kind, _, _, _ = receive(a)
                seen_heartbeat |= kind == HEARTBEAT
                if kind == THINK:
                    break
                assert kind != ERROR
            a.sendall(frame(CANCEL, 1))
            assert collect(a)[0] == CANCELLED
            assert seen_heartbeat and collect(b)[0] == END
            turn = server.state / token.decode() / "turn-00000001"
            assert (turn / "cancelled").exists() and not (turn / "done").exists()
            send_turn(a, b"corrected question", 2)
            assert collect(a, 2)[0] == END
            assert "cancel this" not in json.dumps(llm.templates[-1])
        with server.connection(token) as (a, _):
            send_turn(a, b"slow cancel this", 1)
            assert collect(a)[0] == CANCELLED
        passed("cancel during thinking, durable cancellation replay, slow-first-token heartbeat, other client continues")

    with fixture() as (server, llm, config):
        with server.connection() as (sock, token):
            send_turn(sock, b"resume exact prefix")
            partial = collect(sock, stop_at=8)[1]
        wait_for(lambda: b"disconnected" in (server.state / token.decode() / "events.log").read_bytes())
        turn = server.state / token.decode() / "turn-00000001"
        frozen = (turn / "model-snapshot.json").read_bytes()
        saved_config = (turn / "model-config.json").read_bytes()
        # A runtime endpoint is now authoritative for routing, including its
        # loopback restriction. Reject a bad address without losing the prefix.
        config.write_text('{"endpoint":"http://example.org:1234"}')
        with server.connection(token) as (sock, _):
            send_turn(sock, b"resume exact prefix", resume=len(partial))
            end, _, _, _, error = collect(sock, offset=len(partial))
            assert end == ERROR and b"loopback" in error, error
        wait_for(lambda: b"disconnected" in (server.state / token.decode() / "events.log").read_bytes())
        replacement = LocalModel()
        replacement_thread = threading.Thread(target=replacement.serve_forever, daemon=True)
        replacement_thread.start()
        try:
            assert replacement.server_port != llm.server_port
            llm.shutdown()
            llm.server_close()
            current = json.loads(saved_config)
            current.update(endpoint=f"http://127.0.0.1:{replacement.server_port}",
                           max_tokens=1, thinking=False, temperature=1.5)
            config.write_text(json.dumps(current))
            with server.connection(token) as (sock, _):
                send_turn(sock, b"resume exact prefix", resume=len(partial))
                end, suffix, _, _, error = collect(sock, offset=len(partial))
                assert end == END, error
                combined = partial + suffix
                assert combined == (turn / "response.txt").read_bytes()
                assert combined.decode() == "\n\n完整回答：你好🙂。续写完成。"
                assert partial.decode() in replacement.prompts[-1]
            assert replacement.completions[-1]["n_predict"] == 512
            assert replacement.completions[-1]["temperature"] == llm.completions[0]["temperature"]
            assert (turn / "model-snapshot.json").read_bytes() == frozen
            assert (turn / "model-config.json").read_bytes() == saved_config
        finally:
            replacement.shutdown()
            replacement.server_close()
            replacement_thread.join(timeout=3)
        passed("model port change resumes exact frozen prefix/budget/settings; invalid runtime endpoint is rejected without losing checkpoint")

    with fixture() as (server, _, _):
        with server.connection() as (sock, token):
            send_turn(sock, b"cutoff test")
            assert collect(sock)[0] == ERROR
        turn = server.state / token.decode() / "turn-00000001"
        assert not (turn / "done").exists()
        with server.connection(token) as (sock, _):
            send_turn(sock, b"cutoff test")
            assert collect(sock)[0] == END
        passed("token-limit interruption is resumable and never falsely marked complete")

    with fixture(thinking=False) as (server, _, _):
        client = server.launch_client("nonthinking", b"short question\n")
        output, _, cp = server.finish_client(client)
        assert output.decode() == "完整回答：你好🙂。\n" and cp["done"] == 1
        passed("real C client receives non-thinking model final output only")

    with fixture(endpoint="http://example.org:8081") as (server, llm, _):
        with server.connection() as (sock, _):
            send_turn(sock, b"local only")
            assert collect(sock)[0] == ERROR
        assert not llm.prompts
        passed("non-loopback model endpoint is rejected before any remote request")
    checks.append(f"All {count} model integration checks passed.")
    print(checks[-1], flush=True)
    (ROOT / "logs/verification-model.txt").write_text("\n".join(checks) + "\n")


if __name__ == "__main__":
    main()
