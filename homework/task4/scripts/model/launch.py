#!/usr/bin/env python3
"""Reuse a compatible local llama-server or own one on an actual free port.

b11429 server-http.cpp calls bind_to_any_port when --port is zero. We read its
actual listening URL and verify HTTP readiness before publishing it; no
bind-and-close guess is used. An explicitly supplied --port/URL stays fixed.
"""

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import fcntl
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import time
import uuid
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src" / "model"))
from discovery import (LEGACY_ENDPOINT, candidates, owned_regular, probe_service,
                       read_registry, registry_path, registry_records, runtime_directory, write_registry)
from transport import normalize_config


@dataclass
class Service:
    endpoint: str
    process: object
    record: dict
    log: Path | None = None


@contextmanager
def startup_lock(path, timeout):
    runtime_directory(Path(path).parent)
    descriptor = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        owned_regular(descriptor, "model launch lock")
        deadline = time.monotonic() + timeout
        while True:
            try:
                fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB); break
            except BlockingIOError:
                if time.monotonic() >= deadline: raise TimeoutError("another model launch is still starting; retry later")
                time.sleep(0.05)
        yield
    finally: os.close(descriptor)


def stop_owned(process):
    if process is None or process.poll() is not None: return
    process.send_signal(signal.SIGINT)
    try: process.wait(timeout=8)
    except subprocess.TimeoutExpired:
        process.terminate()
        try: process.wait(timeout=3)
        except subprocess.TimeoutExpired: process.kill(); process.wait()


def remove_owned(path, record):
    current = read_registry(path)
    records = registry_records(current)
    remaining = [entry for entry in records
                 if (entry.get("owner"), entry["endpoint"]) != (record["owner"], record["endpoint"])]
    if len(remaining) == len(records):
        return
    if remaining:
        write_registry(path, {"version": 2, "services": remaining})
    else:
        Path(path).unlink()
        directory = os.open(Path(path).parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        try: os.fsync(directory)
        finally: os.close(directory)


def port_in_use(endpoint):
    parsed = urlsplit(endpoint)
    family = socket.AF_INET6 if ":" in parsed.hostname else socket.AF_INET
    with socket.socket(family) as connection:
        connection.settimeout(0.5)
        return connection.connect_ex((parsed.hostname, parsed.port)) == 0


def process_stamp(pid):
    if type(pid) is not int or pid <= 0: return None
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return fields[19] if fields[0] != "Z" else None
    except (OSError, IndexError): return None


def verified_mode(record):
    if (record and record.get("mode") in ("cpu", "gpu")
            and record.get("process_start") is not None
            and process_stamp(record.get("pid")) == record["process_start"]):
        return record["mode"]
    return None


def publish_service(path, record, previous):
    """Keep other live owned services discoverable, with this endpoint first.

    Records without a process identity may belong to externally started
    services; their health is checked during discovery, never guessed here.
    The caller holds the startup lock for the read/modify/write operation.
    """
    remaining = [entry for entry in previous if entry["endpoint"] != record["endpoint"]
                 and (entry.get("process_start") is None
                      or process_stamp(entry.get("pid")) == entry["process_start"])]
    write_registry(path, {"version": 2, "services": [record, *remaining]})


def record_for(endpoint, properties, owner, pid=None, mode=None, extra=()):
    return {"version": 1, "endpoint": endpoint, "model": properties["model_alias"],
            "model_path": properties["model_path"], "context_tokens": properties["default_generation_settings"]["n_ctx"],
            "slots": properties["total_slots"], "build_info": properties.get("build_info", "unknown"),
            "owner": owner, "pid": pid, "process_start": process_stamp(pid),
            "mode": mode, "extra": list(extra), "recorded_unix": time.time()}


def ensure_service(config, registry, binary, environment, *, mode="gpu", port=None,
                   extra=(), timeout=120, preferred=LEGACY_ENDPOINT):
    registry = Path(registry)
    # A config URL and --port are explicit, fail-closed transports. Default
    # auto first reuses a live registered/legacy service, then binds port zero.
    configured = None if config["endpoint"] == "auto" else config["endpoint"]
    if configured and port is not None and (port == 0 or port != urlsplit(configured).port):
        raise ValueError("--port conflicts with the explicit config endpoint; set endpoint=auto before overriding its port")
    fixed = configured or (f"http://127.0.0.1:{port}" if port else None)
    rejected = {"-m", "--model", "--alias", "--host", "--port", "--parallel", "-np", "--ctx-size", "-c",
                "--api-key", "--api-key-file", "--reuse-port", "--no-jinja", "--kv-unified",
                "--chat-template", "--chat-template-file", "--n-gpu-layers", "--gpu-layers", "-ngl"}
    if any(str(value).split("=", 1)[0] in rejected for value in extra):
        raise ValueError("model/host/port/slots/context/template/GPU options are controlled by config and launcher")
    with startup_lock(registry.with_name(registry.name + ".lock"), timeout):
        endpoints = [fixed] if fixed else candidates(config, registry, preferred)
        previous = registry_records(read_registry(registry))
        for endpoint in endpoints:
            properties = probe_service(endpoint, config)
            if properties is not None:
                known = next((entry for entry in previous if entry["endpoint"] == endpoint), None)
                known_mode = verified_mode(known)
                if (mode == "cpu" and known_mode != "cpu") or (mode == "gpu" and known_mode == "cpu"):
                    continue
                if extra and (known is None or known.get("extra") != list(extra) or known_mode is None):
                    raise RuntimeError("compatible service already exists, but requested extra startup options cannot be verified; reuse without extras or choose an explicit unused port")
                # Keep ownership of a service created by another launcher, so
                # its eventual exit can conditionally remove its own record.
                if known:
                    record = known
                else:
                    record = record_for(endpoint, properties, "reused-" + uuid.uuid4().hex)
                if not previous or previous[0] != record:
                    publish_service(registry, record, previous)
                return Service(endpoint, None, record)
        if fixed and port_in_use(fixed):
            raise RuntimeError(f"{fixed} is occupied by an incompatible service; omit --port and use endpoint=auto to choose another port")
        model = Path(config["model_file"])
        if not model.is_absolute(): model = ROOT / model
        if not model.is_file(): raise FileNotFoundError(f"model missing: {model}; run scripts/model/setup.py")
        if not Path(binary).is_file(): raise FileNotFoundError("llama-server missing; run scripts/model/setup.py")
        parsed = urlsplit(fixed) if fixed else None
        host, selected_port = (parsed.hostname, parsed.port) if parsed else ("127.0.0.1", 0)
        owner = uuid.uuid4().hex
        log = registry.parent / ("model-launch-" + owner + ".log")
        command = [str(binary), "--model", str(model.resolve()), "--alias", config["model"],
                   "--host", host, "--port", str(selected_port), "--parallel", str(config["slots"]),
                   "--ctx-size", str(config["context_tokens"] * config["slots"]), "--no-kv-unified",
                   "--n-gpu-layers", "99" if mode == "gpu" else "0", "--threads", "8", "--threads-batch", "8",
                   "--flash-attn", "on", "--jinja", "--no-ui", "--cache-ram", "0", *map(str, extra)]
        descriptor = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
        with os.fdopen(descriptor, "wb") as output:
            process = subprocess.Popen(command, cwd=ROOT, env=environment,
                                       stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            deadline, endpoint = time.monotonic() + timeout, None
            while time.monotonic() < deadline:
                content = log.read_text(errors="replace")
                match = re.search(r"listening on (http://(?:[0-9.]+|\[[0-9a-fA-F:]+\]):[0-9]+)", content)
                if match:
                    endpoint = normalize_config({"endpoint": match.group(1)})["endpoint"]
                    if fixed and endpoint != fixed:
                        raise RuntimeError("llama-server listened on an unexpected endpoint")
                    properties = probe_service(endpoint, config)
                    if properties is not None:
                        record = record_for(endpoint, properties, owner, process.pid, mode, extra)
                        publish_service(registry, record, previous)
                        return Service(endpoint, process, record, log)
                if process.poll() is not None:
                    raise RuntimeError(f"llama-server startup exited {process.returncode}; log={log}:\n{content[-3000:]}")
                time.sleep(0.05)
            raise TimeoutError(f"llama-server did not become compatible/healthy within {timeout}s; log={log}")
        except BaseException:
            stop_owned(process)
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", choices=("gpu", "cpu"), default="gpu")
    parser.add_argument("--config", type=Path, default=ROOT / "config" / "model.json")
    parser.add_argument("--port", type=int, help="fixed port 1..65535; 0 lets the kernel choose")
    parser.add_argument("--startup-timeout", type=float, default=120)
    args, extra = parser.parse_known_args()
    if args.port is not None and not 0 <= args.port <= 65535: parser.error("--port must be 0..65535")
    if args.startup_timeout <= 0: parser.error("--startup-timeout must be positive")
    config = normalize_config(json.loads(args.config.read_text()))
    runtime = ROOT / ".runtime" / "llama"
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = ":".join((str(runtime / "llama-b11429"),
        str(runtime / "cudart-llama-b11429-bin-ubuntu-cuda-13.4-x64"), "/usr/lib/wsl/lib",
        environment.get("LD_LIBRARY_PATH", ""))).rstrip(":")
    if args.mode == "cpu": environment["CUDA_VISIBLE_DEVICES"] = ""
    service = None
    def interrupted(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    try:
        service = ensure_service(config, registry_path(), runtime / "llama-b11429" / "llama-server",
                                 environment, mode=args.mode, port=args.port, extra=extra,
                                 timeout=args.startup_timeout)
        if service.process is None:
            print(f"[model] reusing compatible healthy service at {service.endpoint}; no new model process.", flush=True)
        else:
            print(f"[model] listening at {service.endpoint}; owned PID={service.process.pid}; log={service.log}", flush=True)
        print(f"[model] endpoint registry: {registry_path()}", flush=True)
        if service.process is not None:
            return service.process.wait()
        return 0
    except KeyboardInterrupt:
        return 0
    finally:
        if service is not None and service.process is not None:
            stop_owned(service.process)
            with startup_lock(registry_path().with_name(registry_path().name + ".lock"), 5):
                remove_owned(registry_path(), service.record)


if __name__ == "__main__":
    try: raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        print("[model] " + str(error), file=sys.stderr); raise SystemExit(1)
