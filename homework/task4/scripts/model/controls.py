#!/usr/bin/env python3
"""Exercise real C tty controls and final-answer recovery on a local GPU model.

Requires the already-running llama-server and built C programs. The script
starts only its own C servers/clients, leaves the model service running, and
keeps evidence under ignored state/model-controls-TIMESTAMP/. Human-readable
logs contain timings and state checks, rather than full thinking transcripts.
"""

from __future__ import annotations

import argparse
from datetime import datetime
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pty
import re
import select
import shlex
import signal
import subprocess
import sys
import termios
import time
from zoneinfo import ZoneInfo

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "model"))
from transport import LocalServer, normalize_config  # noqa: E402


def checkpoint(path):
    spec = importlib.util.spec_from_file_location("task4_controls_checkpoint", ROOT / "scripts" / "checkpoint.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.load(path)


def stop_process(process):
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


class Client:
    def __init__(self, command, name, work, interactive=False):
        self.name, self.work = name, work
        self.stdout_path = work / (name + ".stdout")
        self.stderr_path = work / (name + ".stderr")
        self.output = self.stdout_path.open("wb")
        self.master = None
        if interactive:
            self.master, slave = pty.openpty()
            settings = termios.tcgetattr(slave)
            settings[3] &= ~termios.ECHO
            termios.tcsetattr(slave, termios.TCSANOW, settings)
            input_fd = slave
        else:
            slave, input_fd = None, subprocess.DEVNULL
        try:
            self.process = subprocess.Popen(command, cwd=ROOT, stdin=input_fd,
                                            stdout=self.output, stderr=subprocess.PIPE)
        finally:
            if slave is not None:
                os.close(slave)
        self.diagnostics = bytearray()
        os.set_blocking(self.process.stderr.fileno(), False)

    def drain(self, timeout=0.02):
        if select.select([self.process.stderr], [], [], timeout)[0]:
            while True:
                try:
                    content = os.read(self.process.stderr.fileno(), 65536)
                except BlockingIOError:
                    break
                if not content:
                    break
                self.diagnostics.extend(content)

    def send(self, text):
        if self.master is None:
            raise RuntimeError("client has no interactive tty")
        os.write(self.master, (text + "\n").encode("utf-8"))

    def wait_marker(self, marker, deadline, since=0):
        needle = marker.encode("utf-8")
        while time.monotonic() < deadline:
            self.drain()
            found = self.diagnostics.find(needle, since)
            if found != -1:
                return found
            if self.process.poll() is not None:
                self.drain(0)
                raise RuntimeError(f"{self.name} exited {self.process.returncode} before {marker}: {self.error_summary()}")
        raise TimeoutError(f"{self.name} did not report {marker}")

    def wait_exit(self, deadline):
        while self.process.poll() is None and time.monotonic() < deadline:
            self.drain()
        if self.process.poll() is None:
            raise TimeoutError(f"{self.name} did not exit")
        self.drain(0)
        return self.process.returncode

    def error_summary(self):
        text = bytes(self.diagnostics).decode("utf-8", "replace")
        start = text.rfind("[server error]")
        return text[start:start + 700] if start != -1 else f"see {self.stderr_path}"

    def close(self):
        stop_process(self.process)
        self.drain(0)
        self.stderr_path.write_bytes(self.diagnostics)
        self.process.stderr.close()
        self.output.close()
        if self.master is not None:
            os.close(self.master)
            self.master = None


class Demo:
    def __init__(self, config, work, timeout, report):
        self.config, self.work, self.report = config, work, report
        self.deadline = time.monotonic() + timeout
        self.clients, self.servers = [], []

    def server(self, name, config_path):
        log = self.work / (name + ".server.log")
        state = self.work / (name + ".server")
        command = [str(ROOT / "bin" / "stream_server"), "--port", "0",
                   "--discovery-dir", str(self.work / "services"), "--backend", "llama",
                   "--model-config", str(config_path), "--state", str(state), "--max-clients", "2"]
        with log.open("wb") as output:
            process = subprocess.Popen(command, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        self.servers.append(process)
        self.report.append("$ " + shlex.join(command))
        deadline = min(self.deadline, time.monotonic() + 10)
        while time.monotonic() < deadline:
            text = log.read_text(errors="replace")
            match = re.search(r"listening on 127\.0\.0\.1:(\d+)", text)
            if match:
                return process, match.group(1), state
            if process.poll() is not None:
                raise RuntimeError("C server exited: " + text)
            time.sleep(0.02)
        raise TimeoutError("C server did not start")

    def client(self, port, path, name, interactive=False, extras=()):
        command = [str(ROOT / "bin" / "stream_client"), "--port", port,
                   "--checkpoint", str(path), *extras]
        self.report.append("$ " + shlex.join(command) + (" [stdin=pty]" if interactive else ""))
        client = Client(command, name, self.work, interactive)
        self.clients.append(client)
        return client

    def controls(self, config_path):
        _, port, state = self.server("controls", config_path)
        path = self.work / "controls.ckpt"
        client = self.client(port, path, "controls", interactive=True)
        client.wait_marker("question (", self.deadline)
        question = "请详细介绍 Linux 进程通信。"
        correction = "取消长篇解释，最终只回答单词 LINUX。"
        client.send(question)
        client.wait_marker("[thinking]", self.deadline)
        thought_start = time.monotonic()
        since = len(client.diagnostics)
        client.send("/steer " + correction)
        client.wait_marker("[client] cancelled turn=1", self.deadline, since)
        latency = time.monotonic() - thought_start
        self.report.extend(["thinking=true; initial question=" + question,
                            "$ /steer " + correction,
                            f"STEER/CANCEL -> CANCELLED acknowledgement: {latency * 1000:.3f} ms"])
        resumed = False
        try:
            client.wait_marker("[client] completed turn=2", self.deadline, since)
        except RuntimeError:
            if b"generation stopped at limit" not in client.diagnostics:
                raise
            self.report.append("Corrected turn reached its actual token quota; continuing the same raw prefix once.")
            client.close()
            client = self.client(port, path, "controls-resume", interactive=True, extras=("--resume-only",))
            client.wait_marker("[client] completed turn=2", self.deadline)
            resumed = True
        saved = checkpoint(path)
        assert saved["request"] == 2 and saved["done"] == 1 and saved["answer"].strip(), "corrected turn did not finish"
        session = state / saved["token"]
        old, new = session / "turn-00000001", session / "turn-00000002"
        assert (old / "cancelled").exists() and not (old / "done").exists(), "steered old turn incorrectly became completed history"
        combined = (new / "prompt.txt").read_text()
        assert question in combined and correction in combined, "correction lost the original prompt or new instruction"
        messages = json.loads((new / "model-snapshot.json").read_text())["messages"]
        assert messages == [{"role": "user", "content": combined}], "cancelled partial answer/thinking entered model history"
        metrics = json.loads((new / "metrics.json").read_text())
        self.report.extend(["PASS: turn 1 has cancelled, lacks done; turn 2 contains original question and correction.",
                            "PASS: turn 2 structured messages contain only the corrected user prompt; cancelled partial answer/thoughts are excluded.",
                            "Corrected final answer=" + repr(saved["answer"].decode("utf-8")),
                            f"Corrected completion: {metrics.get('elapsed_seconds', 0):.3f}s; "
                            f"tokens/s={metrics.get('tokens_per_second', 0):.3f}; quota_resume={resumed}"])
        if resumed:
            # --resume-only exits after completion; reconnect to demonstrate /stop.
            assert client.wait_exit(self.deadline) == 0
            client.close()
            client = self.client(port, path, "controls-stop", interactive=True)
            client.wait_marker("question (", self.deadline)
        since = len(client.diagnostics)
        client.send("请详细解释 Linux 中的信号机制。")
        at = client.wait_marker("[client] turn=3", self.deadline, since)
        client.wait_marker("[thinking]", self.deadline, at)
        since = len(client.diagnostics)
        started = time.monotonic()
        client.send("/stop")
        client.wait_marker("[client] cancelled turn=3", self.deadline, since)
        stop_latency = time.monotonic() - started
        terminal = checkpoint(path)
        assert terminal["done"] == 2 and terminal["request"] == 3
        assert (session / "turn-00000003" / "cancelled").exists()
        assert not (session / "turn-00000003" / "done").exists()
        self.report.extend(["$ /stop", f"STOP -> CANCELLED acknowledgement: {stop_latency * 1000:.3f} ms",
                            "PASS: explicit STOP marks turn 3 cancelled; session remains usable."])
        client.send("/quit")
        assert client.wait_exit(self.deadline) == 0
        client.close()

    def recovery(self):
        # Override only this isolated server's config, and record the different mode.
        config = {**self.config, "thinking": False}
        config_path = self.work / "recovery-model.json"
        config_path.write_text(json.dumps(config, ensure_ascii=False, indent=2) + "\n")
        _, port, state = self.server("recovery", config_path)
        path = self.work / "recovery.ckpt"
        question = self.work / "recovery-input.txt"
        question.write_text("不要解释，逐行输出 1 到 50 的整数，每行一个数字。\n")
        client = self.client(port, path, "recovery-before", extras=("--input", str(question)))
        while time.monotonic() < self.deadline:
            client.drain(0.005)
            try:
                saved = checkpoint(path)
            except (FileNotFoundError, ValueError):
                continue
            if saved["offset"] >= 8 and saved["done"] == 0:
                break
            if client.process.poll() is not None:
                raise RuntimeError("real answer completed before the recovery interruption could be observed")
        else:
            raise TimeoutError("did not observe a nonempty pending final-answer prefix")
        client.process.send_signal(signal.SIGTERM)
        status = client.wait_exit(self.deadline)
        client.close()
        saved = checkpoint(path)
        assert status == 143 and saved["done"] == 0 and saved["offset"] > 0, "SIGTERM did not leave a pending durable answer"
        prefix = saved["answer"]
        session = state / saved["token"]
        turn = session / "turn-00000001"
        events = session / "events.log"
        while time.monotonic() < self.deadline and "event=disconnected" not in events.read_text():
            time.sleep(0.005)
        raw_before = (turn / "raw-generation.jsonl").read_bytes()
        # A killed append may leave an uncommitted last line; the adapter will
        # truncate it on resume. Compare only newline-terminated durable records.
        records = [json.loads(line) for line in raw_before.splitlines(keepends=True)
                   if line.endswith(b"\n")]
        assert records and not any(record["kind"] == "finished" for record in records), "model had already completed before interruption"
        raw_prefix = "".join(row["text"] for row in records if row["kind"] == "raw")
        frozen_hash = hashlib.sha256((turn / "model-snapshot.json").read_bytes()).hexdigest()
        self.report.extend(["recovery demo uses isolated thinking=false config, while default config remains thinking=true.",
                            "$ SIGTERM stream_client during final-answer streaming",
                            f"Interrupted exit={status}; pending durable final prefix={len(prefix)} bytes; raw journal={len(raw_before)} bytes."])
        started = time.monotonic()
        client = self.client(port, path, "recovery-after", extras=("--resume-only",))
        status = client.wait_exit(self.deadline)
        if status != 0 and b"generation stopped at limit" in client.diagnostics:
            self.report.append("Recovery attempt reached its actual token quota; continuing the same turn once more.")
            client.close()
            client = self.client(port, path, "recovery-after-quota", extras=("--resume-only",))
            status = client.wait_exit(self.deadline)
        if status != 0:
            raise RuntimeError("real recovery failed: " + client.error_summary())
        client.close()
        completed = checkpoint(path)
        assert completed["done"] == 1 and completed["request"] == 1
        assert completed["answer"].startswith(prefix) and completed["offset"] > len(prefix)
        raw_after = [json.loads(line) for line in (turn / "raw-generation.jsonl").read_bytes().splitlines() if line]
        all_raw = "".join(row["text"] for row in raw_after if row["kind"] == "raw")
        assert all_raw.startswith(raw_prefix), "model resampled the saved raw prefix"
        assert hashlib.sha256((turn / "model-snapshot.json").read_bytes()).hexdigest() == frozen_hash
        assert any(row["kind"] == "finished" for row in raw_after) and (turn / "done").exists()
        metrics = json.loads((turn / "metrics.json").read_text())
        assert metrics.get("raw_prefix_bytes", 0) > 0, "recovery did not prefill the frozen raw generation"
        assert metrics.get("generated_tokens", 0) > 0 and not metrics.get("replay_only", False), "no new model tokens were generated after reconnect"
        self.report.extend([f"Recovered final answer={completed['offset']} bytes; suffix={completed['offset'] - len(prefix)} bytes; "
                            f"elapsed={time.monotonic() - started:.3f}s.",
                            f"Resumed inference prefilled raw_prefix_bytes={metrics['raw_prefix_bytes']}; "
                            f"new_tokens={metrics['generated_tokens']}; "
                            f"tokens/s={metrics.get('tokens_per_second', 0):.3f}.",
                            "PASS: completed answer preserves the exact durable prefix, same request/token and same model/template snapshot.",
                            "PASS: raw journal retains the exact old generation prefix; resumed C client completed turn 1 without a repeated question."])

    def close(self):
        for client in self.clients:
            if not client.output.closed:
                client.close()
        for process in self.servers:
            stop_process(process)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config" / "model.json")
    parser.add_argument("--log", type=Path, default=ROOT / "logs" / "model-controls.txt")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    now = datetime.now(ZoneInfo("Asia/Shanghai"))
    work = ROOT / "state" / ("model-controls-" + now.strftime("%Y%m%dT%H%M%S") + f"-{os.getpid()}")
    work.mkdir(parents=True, exist_ok=False)
    report = ["$ python3 scripts/model/controls.py", "Recorded: " + now.isoformat(), "Evidence: " + str(work)]
    demo = None
    started = time.monotonic()
    status = 1
    try:
        config = normalize_config(json.loads(args.config.read_text()))
        assert config["thinking"] is True, "the main demo requires the default thinking=true config"
        model_server = LocalServer(config, timeout=5)
        health = model_server.json_request("/health")
        assert health.get("status") == "ok", "local llama-server is not ready"
        for binary in ("stream_server", "stream_client"):
            assert (ROOT / "bin" / binary).exists(), "build C programs first"
        report.append(f"Real local model={config['model']}; endpoint={model_server.endpoint}; default thinking=true.")
        demo = Demo(config, work, args.timeout, report)
        demo.controls(args.config.resolve())
        demo.recovery()
        report.extend(["PASS: real tty STEER, STOP and final-answer disconnect recovery completed through C server/client and local model.",
                       f"Total elapsed: {time.monotonic() - started:.3f}s", "[exit status: 0]"])
        status = 0
    except Exception as error:
        report.extend(["FAIL: " + str(error), f"Elapsed before failure: {time.monotonic() - started:.3f}s", "[exit status: 1]"])
    finally:
        if demo is not None:
            demo.close()
        args.log.parent.mkdir(parents=True, exist_ok=True)
        args.log.write_text("\n".join(report) + "\n")
    print("\n".join(report))
    return status


if __name__ == "__main__":
    sys.exit(main())
