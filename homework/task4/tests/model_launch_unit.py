#!/usr/bin/env python3
"""Owned HTTP/process checks for model reuse, real binds and transport discovery.

No model/GPU is loaded. The executable fixture implements just health/props and
actually binds the requested socket. All files, services and children are ours.
"""

from concurrent.futures import ThreadPoolExecutor
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "model"))
sys.path.insert(0, str(ROOT / "src" / "model"))
from discovery import read_registry, registry_records, resolve_endpoint, write_registry
from launch import ensure_service, record_for, remove_owned, stop_owned
from transport import LocalServer, normalize_config


FAKE_BINARY = r'''#!/usr/bin/env python3
import argparse, http.server, json, os, signal
p=argparse.ArgumentParser()
for name in ('model','alias','host','port','parallel','ctx-size'): p.add_argument('--'+name,required=True)
a,_=p.parse_known_args()
with open(os.environ['TASK4_FAKE_LAUNCH_COUNTER'],'a') as f: f.write(str(os.getpid())+'\n')
class Handler(http.server.BaseHTTPRequestHandler):
 def do_GET(self):
  value={'status':'ok'} if self.path=='/health' else {
   'model_path':a.model,'model_alias':a.alias,'chat_template':'fixture-jinja','build_info':'fixture-build',
   'total_slots':int(a.parallel),'default_generation_settings':{'n_ctx':int(a.ctx_size)//int(a.parallel)}}
  raw=json.dumps(value).encode(); self.send_response(200); self.send_header('Content-Length',str(len(raw)))
  self.end_headers(); self.wfile.write(raw)
 def log_message(self,*args): pass
s=http.server.ThreadingHTTPServer((a.host,int(a.port)),Handler)
print('srv llama_server: listening on http://127.0.0.1:'+str(s.server_port),flush=True)
try: s.serve_forever(poll_interval=.05)
except KeyboardInterrupt: pass
finally: s.server_close()
'''


class HTTPFixture:
    def __init__(self, config, **overrides):
        self.properties = {"model_path": config["model_file"], "model_alias": config["model"],
            "chat_template": "fixture-jinja", "build_info": "fixture-build", "total_slots": config["slots"],
            "default_generation_settings": {"n_ctx": config["context_tokens"]}, **overrides}
        fixture = self
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                value = {"status": "ok"} if self.path == "/health" else fixture.properties
                content = json.dumps(value).encode()
                self.send_response(200); self.send_header("Content-Length", str(len(content)))
                self.end_headers(); self.wfile.write(content)
            def log_message(self, *args): pass
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.endpoint = f"http://127.0.0.1:{self.server.server_port}"
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
        self.thread.start()
    def close(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(timeout=2)


class ModelLaunchChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="task4-model-launch-")
        self.home = Path(self.temp.name)
        model = self.home / "fixture.gguf"; model.write_bytes(b"layout test model fixture")
        self.config = normalize_config({"endpoint": "auto", "model": "owned-fixture",
            "model_file": str(model), "context_tokens": 4096, "slots": 2})
        self.registry = self.home / "runtime" / "model-service.json"
        self.binary = self.home / "fake-llama-server"
        self.binary.write_text(FAKE_BINARY); self.binary.chmod(0o700)
        self.counter = self.home / "launch-count"
        self.environment = {**os.environ, "TASK4_FAKE_LAUNCH_COUNTER": str(self.counter)}
        self.services, self.fixtures = [], []

    def tearDown(self):
        for service in self.services:
            if service.process is not None:
                stop_owned(service.process)
                if self.registry.exists(): remove_owned(self.registry, service.record)
        for fixture in self.fixtures: fixture.close()
        self.temp.cleanup()

    def fixture(self, **overrides):
        result = HTTPFixture(self.config, **overrides); self.fixtures.append(result); return result

    def start(self, **kwargs):
        service = ensure_service(self.config, self.registry, self.binary, self.environment,
                                 preferred=None, timeout=4, **kwargs)
        self.services.append(service); return service

    def publish(self, fixture):
        record = record_for(fixture.endpoint, fixture.properties, "fixture-owner")
        write_registry(self.registry, record); return record

    def count(self):
        return len(self.counter.read_text().splitlines()) if self.counter.exists() else 0

    def records(self):
        return registry_records(read_registry(self.registry))

    def test_registered_healthy_service_reused_without_launch(self):
        fixture = self.fixture(); before = self.publish(fixture)
        service = self.start()
        self.assertIsNone(service.process); self.assertEqual(service.endpoint, fixture.endpoint)
        self.assertEqual(read_registry(self.registry), before); self.assertEqual(self.count(), 0)
        self.assertEqual(LocalServer({**self.config, "endpoint": fixture.endpoint}).json_request("/health"), {"status": "ok"})

    def test_foreign_occupied_port_preserved_and_actual_port_zero_published(self):
        fixture = self.fixture(model_alias="foreign-service")
        service = ensure_service(self.config, self.registry, self.binary, self.environment,
                                 preferred=fixture.endpoint, timeout=4)
        self.services.append(service)
        self.assertIsNotNone(service.process); self.assertNotEqual(service.endpoint, fixture.endpoint)
        self.assertEqual(self.records()[0]["endpoint"], service.endpoint)
        self.assertIn("--port", service.process.args)
        self.assertEqual(service.process.args[service.process.args.index("--port") + 1], "0")
        self.assertEqual(LocalServer({**self.config, "endpoint": fixture.endpoint}).json_request("/props")["model_alias"], "foreign-service")
        self.assertEqual(self.count(), 1)

    def test_explicit_occupied_port_fails_without_touching_foreign_service(self):
        fixture = self.fixture(model_alias="foreign-service")
        with self.assertRaisesRegex(RuntimeError, "occupied by an incompatible service"):
            self.start(port=fixture.server.server_port)
        self.assertEqual(self.count(), 0); self.assertFalse(self.registry.exists())
        self.assertEqual(LocalServer({**self.config, "endpoint": fixture.endpoint}).json_request("/health")["status"], "ok")

    def test_concurrent_launchers_load_only_one_service(self):
        with ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(self.start) for _ in range(2)]
            services = [future.result(timeout=8) for future in futures]
        self.assertEqual(self.count(), 1)
        self.assertEqual(services[0].endpoint, services[1].endpoint)
        self.assertEqual(sum(service.process is not None for service in services), 1)
        self.assertEqual(self.records()[0]["pid"], next(service.process.pid for service in services if service.process))

    def test_stale_endpoint_replaced_and_only_owned_record_removed(self):
        fixture = self.fixture(); self.publish(fixture)
        fixture.close(); self.fixtures.remove(fixture)
        service = self.start()
        self.assertEqual(self.records()[0]["owner"], service.record["owner"])
        replacement = {**service.record, "owner": "another-launcher"}
        write_registry(self.registry, replacement)
        remove_owned(self.registry, service.record)
        self.assertEqual(self.records()[0]["owner"], "another-launcher")

    def test_wrong_model_context_or_slots_are_never_auto_selected(self):
        for override in ({"model_alias": "another-model"}, {"model_path": str(self.home / "other.gguf")},
                         {"total_slots": 1}, {"default_generation_settings": {"n_ctx": 2048}}):
            with self.subTest(override=override):
                fixture = self.fixture(**override); self.publish(fixture)
                with self.assertRaisesRegex(RuntimeError, "no compatible healthy"):
                    resolve_endpoint(self.config, self.registry, preferred=None)
                fixture.close(); self.fixtures.remove(fixture)

    def test_new_transport_resolves_new_registry_but_explicit_url_bypasses_it(self):
        first = self.fixture(); self.publish(first)
        with patch.dict(os.environ, {"TASK4_MODEL_SERVICE_FILE": str(self.registry)}):
            one = LocalServer(self.config); one.json_request("/health")
            second = self.fixture(); self.publish(second)
            two = LocalServer(self.config); two.json_request("/health")
        self.assertEqual(one.endpoint, first.endpoint); self.assertEqual(two.endpoint, second.endpoint)
        # Even an unsafe unrelated registry cannot redirect a concrete URL.
        self.registry.chmod(0o666)
        with patch.dict(os.environ, {"TASK4_MODEL_SERVICE_FILE": str(self.registry)}):
            direct = LocalServer({**self.config, "endpoint": first.endpoint})
            self.assertEqual(direct.json_request("/health")["status"], "ok")
        self.registry.chmod(0o600)

    def test_registry_symlink_permissions_and_remote_endpoint_fail_closed(self):
        fixture = self.fixture(); self.publish(fixture)
        self.registry.chmod(0o666)
        with self.assertRaisesRegex(ValueError, "private regular file"): read_registry(self.registry)
        self.registry.chmod(0o600)
        document = json.loads(self.registry.read_text()); document["endpoint"] = "http://8.8.8.8:8081"
        self.registry.write_text(json.dumps(document))
        with self.assertRaisesRegex(ValueError, "loopback"): read_registry(self.registry)
        self.registry.unlink(); self.registry.symlink_to(self.home / "fixture.gguf")
        with self.assertRaises(OSError): read_registry(self.registry)
        self.registry.unlink()

    def test_actual_bind_failure_never_publishes_readiness(self):
        fixture = self.fixture(model_alias="foreign-service")
        # Simulate a port becoming occupied between the advisory checks and
        # child exec: the executable's actual bind is the final authority.
        with patch("launch.port_in_use", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "startup exited"):
                self.start(port=fixture.server.server_port)
        self.assertFalse(self.registry.exists()); self.assertEqual(self.count(), 1)
        self.assertEqual(LocalServer({**self.config, "endpoint": fixture.endpoint}).json_request("/health")["status"], "ok")

    def test_explicit_cpu_does_not_reuse_unknown_gpu_and_reuses_verified_cpu(self):
        fixture = self.fixture(); self.publish(fixture)
        cpu = self.start(mode="cpu")
        self.assertIsNotNone(cpu.process); self.assertNotEqual(cpu.endpoint, fixture.endpoint)
        self.assertEqual(self.records()[0]["mode"], "cpu")
        self.assertEqual(cpu.process.args[cpu.process.args.index("--n-gpu-layers") + 1], "0")
        again = self.start(mode="cpu")
        self.assertIsNone(again.process); self.assertEqual(again.endpoint, cpu.endpoint)
        self.assertEqual(self.count(), 1)
        self.assertEqual(LocalServer({**self.config, "endpoint": fixture.endpoint}).json_request("/health")["status"], "ok")

    def test_cpu_and_gpu_stay_discoverable_in_either_exit_order(self):
        for exit_first in ("cpu", "gpu"):
            with self.subTest(exit_first=exit_first):
                baseline = self.count()
                services = {mode: self.start(mode=mode) for mode in ("gpu", "cpu")}
                self.assertEqual({record["mode"] for record in self.records()}, {"gpu", "cpu"})
                for mode, owned in services.items():
                    reused = self.start(mode=mode)
                    self.assertIsNone(reused.process)
                    self.assertEqual(reused.endpoint, owned.endpoint)
                self.assertEqual(self.count(), baseline + 2)
                stopped = services[exit_first]
                stop_owned(stopped.process)
                remove_owned(self.registry, stopped.record)
                remaining_mode = "gpu" if exit_first == "cpu" else "cpu"
                remaining = services[remaining_mode]
                self.assertEqual(resolve_endpoint(self.config, self.registry, preferred=None), remaining.endpoint)
                reused = self.start(mode=remaining_mode)
                self.assertIsNone(reused.process)
                self.assertEqual(reused.endpoint, remaining.endpoint)
                self.assertEqual(self.count(), baseline + 2)
                stop_owned(remaining.process)
                remove_owned(self.registry, remaining.record)
                self.assertFalse(self.registry.exists())

    def test_stale_cpu_record_does_not_hide_live_gpu(self):
        gpu, cpu = self.start(mode="gpu"), self.start(mode="cpu")
        stop_owned(cpu.process)  # Simulate a launcher exiting without registry cleanup.
        self.assertEqual(resolve_endpoint(self.config, self.registry, preferred=None), gpu.endpoint)
        reused = self.start(mode="gpu")
        self.assertIsNone(reused.process)
        self.assertEqual(reused.endpoint, gpu.endpoint)
        self.assertEqual(self.count(), 2)
        self.assertEqual([record["endpoint"] for record in self.records()], [gpu.endpoint])

    def test_explicit_url_port_contradiction_and_ignored_extras_are_rejected(self):
        fixture = self.fixture(); self.publish(fixture)
        concrete = {**self.config, "endpoint": fixture.endpoint}
        for port in (0, 1 if fixture.server.server_port != 1 else 2):
            with self.assertRaisesRegex(ValueError, "conflicts with the explicit"):
                ensure_service(concrete, self.registry, self.binary, self.environment, port=port)
        with self.assertRaisesRegex(ValueError, "controlled by config"):
            self.start(extra=("-ngl", "0"))
        with self.assertRaisesRegex(RuntimeError, "extra startup options cannot be verified"):
            self.start(extra=("--threads", "4"))
        self.assertEqual(self.count(), 0)

    def test_fifo_registry_rejected_without_waiting_for_a_writer(self):
        self.registry.parent.mkdir()
        os.mkfifo(self.registry, 0o600)
        # A blocking open would hang until a writer connects. Isolate it so
        # the test has a bound without imposing a scheduler-speed threshold.
        result = subprocess.run([sys.executable, "-c",
            "import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); "
            "from discovery import read_registry; read_registry(Path(sys.argv[2]))",
            str(ROOT / "src" / "model"), str(self.registry)],
            capture_output=True, text=True, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("private regular file", result.stderr)
        self.registry.unlink()


if __name__ == "__main__":
    unittest.main(verbosity=2)
