#!/usr/bin/env python3
"""Run real C server/clients against the local llama-server, including two turns.

The existing model service is left running. Test C processes and state are owned
by this script and isolated in a temporary directory; no existing state is used.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "model"))
from transport import LocalServer, normalize_config


def checkpoint(path: Path) -> dict:
    spec = importlib.util.spec_from_file_location("task4_checkpoint", ROOT / "scripts/checkpoint.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.load(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=ROOT / "config/model.json")
    parser.add_argument("--log", type=Path, default=ROOT / "logs/model-smoke.txt")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    config = normalize_config(json.loads(args.config.read_text()))
    model_server = LocalServer(config, timeout=5)
    assert model_server.json_request("/health").get("status") == "ok", "local llama-server is not healthy"
    for path in [ROOT / "bin/stream_server", ROOT / "bin/stream_client", ROOT / "src/model/adapter.py"]:
        if not path.exists():
            parser.exit(1, f"Missing {path}; build the C programs and model adapter first.\n")
    report = ["$ python3 scripts/model/smoke.py", f"local model={config['model']}",
              f"endpoint={model_server.endpoint} thinking={config['thinking']}"]
    clients = []
    server = None
    started = time.monotonic()
    try:
        with tempfile.TemporaryDirectory(prefix="task4-model-smoke-") as temporary:
            work = Path(temporary)
            server_command = [str(ROOT / "bin/stream_server"),
                              "--discovery-dir", str(work / "services"),
                              "--backend", "llama", "--model-config", str(args.config.resolve()),
                              "--state", str(work / "server"), "--max-clients", "2"]
            server_log = work / "server.log"
            with server_log.open("w") as stream:
                server = subprocess.Popen(server_command, cwd=ROOT, stdout=stream,
                                          stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 10
            port = None
            while time.monotonic() < deadline:
                match = re.search(r"listening on 127\.0\.0\.1:(\d+)", server_log.read_text())
                if match:
                    port = match.group(1)
                    break
                if server.poll() is not None:
                    raise RuntimeError("C server failed: " + server_log.read_text())
                time.sleep(0.05)
            if port is None:
                raise TimeoutError("C server did not listen within 10 seconds")
            report.append("$ " + shlex.join(server_command))
            for code in ["MAPLE", "CEDAR"]:
                input_path = work / f"{code}.txt"
                input_path.write_text(f"记住项目代号 {code}。只回答已记住。\n"
                                      "我刚才说的项目代号是什么？只回答代号。\n")
                state = work / f"{code}.ckpt"
                command = [str(ROOT / "bin/stream_client"), "--discovery-dir", str(work / "services"),
                           "--input", str(input_path), "--checkpoint", str(state)]
                output = work / f"{code}.stdout"
                errors = work / f"{code}.stderr"
                with output.open("w") as out, errors.open("w") as err:
                    process = subprocess.Popen(command, cwd=ROOT, stdout=out, stderr=err)
                clients.append((code, process, state, output, errors))
                report.append("$ " + shlex.join(command))
            results = []
            for code, process, state, output, errors in clients:
                remaining = args.timeout - (time.monotonic() - started)
                status = process.wait(timeout=max(1, remaining))
                if status != 0:
                    raise RuntimeError(f"client {code} exit={status}: {errors.read_text()}")
                saved = checkpoint(state)
                answer = saved["answer"].decode("utf-8")
                other = "CEDAR" if code == "MAPLE" else "MAPLE"
                assert saved["request"] == 2 and saved["done"] == 1, f"{code}: two turns did not complete"
                assert code in answer and other not in answer, f"{code}: unexpected memory answer {answer!r}"
                session = work / "server" / saved["token"]
                context = (session / "context.txt").read_text()
                assert code in context and other not in context, f"{code}: context is not isolated"
                report.extend([f"{code}: session={saved['token']} request=2 done=1 exit=0",
                               f"{code}: final answer={answer!r}",
                               f"{code}: persisted answer={saved['offset']} bytes; isolated context verified"])
                results.append(saved)
            assert results[0]["token"] != results[1]["token"], "clients share a session token"
            report.append("PASS: two independent real C clients completed two turns through the local Qwen model.")
            report.append("PASS: each client recalled its own code; session tokens and contexts differ.")
            report.append(f"Elapsed: {time.monotonic() - started:.3f}s\n[exit status: 0]")
    finally:
        for _, process, _, _, _ in clients:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        if server is not None and server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=8)
            except subprocess.TimeoutExpired:
                server.kill()
                server.wait()
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_text("\n".join(report) + "\n")
    print("\n".join(report))


if __name__ == "__main__":
    main()
